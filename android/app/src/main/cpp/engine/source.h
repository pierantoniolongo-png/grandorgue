// Access to organ files: a plain directory or a GrandOrgue .orgue package.
// License GPL-2.0 or later.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace organ {

/** Normalizes an ODF path: backslashes to slashes, no leading "./" */
std::string normalizePath(const std::string &path);

class FileSource {
public:
  virtual ~FileSource() = default;

  /** Reads a file given relative to the source root */
  virtual bool read(const std::string &path, std::vector<uint8_t> &out) = 0;

  /** Lists the organ definition files (*.organ) of the source */
  virtual std::vector<std::string> listOrgans() = 0;

  /** Opens a directory or a package depending on the path */
  static std::unique_ptr<FileSource> open(
    const std::string &path, std::string &err);
};

class DirSource : public FileSource {
public:
  explicit DirSource(const std::string &root) : m_root(root) {}

  bool read(const std::string &path, std::vector<uint8_t> &out) override;
  std::vector<std::string> listOrgans() override;

private:
  std::string m_root;
};

/** A zip archive (stored or deflated entries, zip64 supported) */
class ZipSource : public FileSource {
public:
  ~ZipSource() override;

  bool openFile(const std::string &path, std::string &err);
  bool read(const std::string &path, std::vector<uint8_t> &out) override;
  std::vector<std::string> listOrgans() override;

private:
  struct Entry {
    std::string name;
    uint64_t localOffset;
    uint64_t compSize;
    uint64_t size;
    uint16_t method;
  };

  int m_fd = -1;
  uint64_t m_fileSize = 0;
  std::vector<Entry> m_entries;
  // lowercase normalized name -> index in m_entries
  std::unordered_map<std::string, size_t> m_index;

  bool readAt(uint64_t offset, void *pBuf, size_t len);
};

} // namespace organ
