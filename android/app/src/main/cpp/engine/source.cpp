// Access to organ files: a plain directory or a GrandOrgue .orgue package.
// License GPL-2.0 or later.
#include "source.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace organ {

static std::string lower(const std::string &s) {
  std::string r(s);

  for (char &c : r)
    c = (char)std::tolower((unsigned char)c);
  return r;
}

static bool ends_with_ci(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size()
    && lower(s.substr(s.size() - suffix.size())) == suffix;
}

std::string normalizePath(const std::string &path) {
  std::string r(path);

  std::replace(r.begin(), r.end(), '\\', '/');
  while (r.rfind("./", 0) == 0)
    r = r.substr(2);
  while (!r.empty() && r[0] == '/')
    r = r.substr(1);
  return r;
}

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
  return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

std::unique_ptr<FileSource> FileSource::open(
  const std::string &path, std::string &err) {
  std::unique_ptr<FileSource> result;
  struct stat st;

  if (stat(path.c_str(), &st) != 0)
    err = "File not found: " + path;
  else if (S_ISDIR(st.st_mode))
    result = std::make_unique<DirSource>(path);
  else {
    auto pZip = std::make_unique<ZipSource>();

    if (pZip->openFile(path, err))
      result = std::move(pZip);
  }
  return result;
}

/*
 * DirSource
 */

// Resolves each path component case-insensitively (sample sets made on
// Windows often differ in case from the ODF)
static bool resolve_ci(const std::string &root, const std::string &rel, std::string &out) {
  std::string current = root;
  size_t pos = 0;
  bool isOk = true;

  while (isOk && pos <= rel.size()) {
    size_t slash = rel.find('/', pos);

    if (slash == std::string::npos)
      slash = rel.size();

    std::string comp = rel.substr(pos, slash - pos);

    pos = slash + 1;
    if (!comp.empty()) {
      std::string candidate = current + "/" + comp;
      struct stat st;

      if (stat(candidate.c_str(), &st) == 0)
        current = candidate;
      else {
        DIR *pDir = opendir(current.c_str());
        bool isFound = false;

        if (pDir) {
          struct dirent *pEnt;
          std::string want = lower(comp);

          while (!isFound && (pEnt = readdir(pDir)) != nullptr)
            if (lower(pEnt->d_name) == want) {
              current = current + "/" + pEnt->d_name;
              isFound = true;
            }
          closedir(pDir);
        }
        isOk = isFound;
      }
    }
  }
  if (isOk)
    out = current;
  return isOk;
}

bool DirSource::read(const std::string &path, std::vector<uint8_t> &out) {
  std::string full;
  bool isOk = resolve_ci(m_root, normalizePath(path), full);

  if (isOk) {
    int fd = ::open(full.c_str(), O_RDONLY);

    isOk = fd >= 0;
    if (isOk) {
      struct stat st;

      fstat(fd, &st);
      out.resize((size_t)st.st_size);

      size_t done = 0;

      while (isOk && done < out.size()) {
        ssize_t n = ::read(fd, out.data() + done, out.size() - done);

        isOk = n > 0;
        if (isOk)
          done += (size_t)n;
      }
      ::close(fd);
    }
  }
  return isOk;
}

std::vector<std::string> DirSource::listOrgans() {
  std::vector<std::string> result;
  DIR *pDir = opendir(m_root.c_str());

  if (pDir) {
    struct dirent *pEnt;

    while ((pEnt = readdir(pDir)) != nullptr)
      if (ends_with_ci(pEnt->d_name, ".organ"))
        result.push_back(pEnt->d_name);
    closedir(pDir);
  }
  std::sort(result.begin(), result.end());
  return result;
}

/*
 * ZipSource
 */

ZipSource::~ZipSource() {
  if (m_fd >= 0)
    ::close(m_fd);
}

bool ZipSource::readAt(uint64_t offset, void *pBuf, size_t len) {
  size_t done = 0;
  bool isOk = true;

  while (isOk && done < len) {
    ssize_t n = pread(m_fd, (uint8_t *)pBuf + done, len - done, (off_t)(offset + done));

    isOk = n > 0;
    if (isOk)
      done += (size_t)n;
  }
  return isOk;
}

bool ZipSource::openFile(const std::string &path, std::string &err) {
  bool isOk = true;

  m_fd = ::open(path.c_str(), O_RDONLY);
  if (m_fd < 0) {
    err = "Cannot open " + path;
    isOk = false;
  }
  if (isOk) {
    struct stat st;

    fstat(m_fd, &st);
    m_fileSize = (uint64_t)st.st_size;
  }

  // find the end of central directory record in the last 64 KiB
  uint64_t cdOffset = 0;
  uint64_t cdSize = 0;
  uint64_t nEntries = 0;

  if (isOk) {
    size_t tail = (size_t)std::min<uint64_t>(m_fileSize, 65557);
    std::vector<uint8_t> buf(tail);
    long eocd = -1;

    isOk = readAt(m_fileSize - tail, buf.data(), tail);
    for (long i = (long)tail - 22; isOk && eocd < 0 && i >= 0; i--)
      if (le32(&buf[(size_t)i]) == 0x06054b50)
        eocd = i;
    if (eocd < 0) {
      err = "Not a valid organ package (no zip directory)";
      isOk = false;
    } else {
      const uint8_t *p = &buf[(size_t)eocd];

      nEntries = le16(p + 10);
      cdSize = le32(p + 12);
      cdOffset = le32(p + 16);

      // zip64 end of central directory locator just before the EOCD
      if (eocd >= 20 && le32(&buf[(size_t)eocd - 20]) == 0x07064b50) {
        uint64_t z64Offset = le64(&buf[(size_t)eocd - 20 + 8]);
        uint8_t z64[56];

        if (readAt(z64Offset, z64, sizeof(z64)) && le32(z64) == 0x06064b50) {
          nEntries = le64(z64 + 32);
          cdSize = le64(z64 + 40);
          cdOffset = le64(z64 + 48);
        }
      }
    }
  }

  // read the central directory
  if (isOk) {
    std::vector<uint8_t> cd((size_t)cdSize);
    size_t pos = 0;

    isOk = readAt(cdOffset, cd.data(), cd.size());
    for (uint64_t i = 0; isOk && i < nEntries; i++) {
      isOk = pos + 46 <= cd.size() && le32(&cd[pos]) == 0x02014b50;
      if (isOk) {
        const uint8_t *p = &cd[pos];
        Entry e;
        uint16_t nameLen = le16(p + 28);
        uint16_t extraLen = le16(p + 30);
        uint16_t commentLen = le16(p + 32);

        e.method = le16(p + 10);
        e.compSize = le32(p + 20);
        e.size = le32(p + 24);
        e.localOffset = le32(p + 42);
        e.name = std::string((const char *)p + 46, nameLen);

        // zip64 extended information
        size_t x = pos + 46 + nameLen;
        size_t xEnd = x + extraLen;

        while (x + 4 <= xEnd && xEnd <= cd.size()) {
          uint16_t id = le16(&cd[x]);
          uint16_t len = le16(&cd[x + 2]);
          size_t f = x + 4;

          if (id == 0x0001) {
            if (e.size == 0xFFFFFFFF && f + 8 <= x + 4 + len) {
              e.size = le64(&cd[f]);
              f += 8;
            }
            if (e.compSize == 0xFFFFFFFF && f + 8 <= x + 4 + len) {
              e.compSize = le64(&cd[f]);
              f += 8;
            }
            if (e.localOffset == 0xFFFFFFFF && f + 8 <= x + 4 + len)
              e.localOffset = le64(&cd[f]);
          }
          x += 4 + len;
        }
        pos += 46 + nameLen + extraLen + commentLen;
        if (!e.name.empty() && e.name.back() != '/') {
          m_index[lower(normalizePath(e.name))] = m_entries.size();
          m_entries.push_back(e);
        }
      } else
        err = "Damaged organ package directory";
    }
  }
  return isOk;
}

bool ZipSource::read(const std::string &path, std::vector<uint8_t> &out) {
  auto it = m_index.find(lower(normalizePath(path)));
  bool isOk = it != m_index.end();

  if (isOk) {
    const Entry &e = m_entries[it->second];
    uint8_t lh[30];

    isOk = readAt(e.localOffset, lh, sizeof(lh)) && le32(lh) == 0x04034b50;
    if (isOk) {
      uint64_t dataOffset = e.localOffset + 30 + le16(lh + 26) + le16(lh + 28);

      if (e.method == 0) {
        out.resize((size_t)e.size);
        isOk = readAt(dataOffset, out.data(), out.size());
      } else if (e.method == 8) {
        std::vector<uint8_t> comp((size_t)e.compSize);

        isOk = readAt(dataOffset, comp.data(), comp.size());
        if (isOk) {
          z_stream zs;

          std::memset(&zs, 0, sizeof(zs));
          out.resize((size_t)e.size);
          isOk = inflateInit2(&zs, -MAX_WBITS) == Z_OK;
          if (isOk) {
            zs.next_in = comp.data();
            zs.avail_in = (uInt)comp.size();
            zs.next_out = out.data();
            zs.avail_out = (uInt)out.size();
            isOk = inflate(&zs, Z_FINISH) == Z_STREAM_END;
            inflateEnd(&zs);
          }
        }
      } else
        isOk = false;
    }
  }
  return isOk;
}

std::vector<std::string> ZipSource::listOrgans() {
  std::vector<std::string> result;

  for (const Entry &e : m_entries)
    if (ends_with_ci(e.name, ".organ"))
      result.push_back(normalizePath(e.name));
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace organ
