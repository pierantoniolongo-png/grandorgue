// Organ Definition File (GrandOrgue .organ) reader.
// License GPL-2.0 or later.
#include "odf.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace organ {

static std::string lower(const std::string &s) {
  std::string r(s);

  for (char &c : r)
    c = (char)std::tolower((unsigned char)c);
  return r;
}

static std::string trim(const std::string &s) {
  size_t b = 0;
  size_t e = s.size();

  while (b < e && std::isspace((unsigned char)s[b]))
    b++;
  while (e > b && std::isspace((unsigned char)s[e - 1]))
    e--;
  return s.substr(b, e - b);
}

std::string num3(const std::string &prefix, int n) {
  char buf[16];

  std::snprintf(buf, sizeof(buf), "%03d", n);
  return prefix + buf;
}

static bool is_valid_utf8(const std::string &s) {
  size_t i = 0;
  bool isValid = true;

  while (isValid && i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    // number of continuation bytes; 4 = invalid lead byte
    size_t nCont = c < 0x80 ? 0
      : (c >> 5) == 0x6     ? 1
      : (c >> 4) == 0xE     ? 2
      : (c >> 3) == 0x1E    ? 3
                            : 4;

    isValid = nCont < 4 && i + nCont < s.size();
    for (size_t k = 1; isValid && k <= nCont; k++)
      isValid = ((unsigned char)s[i + k] >> 6) == 0x2;
    i += nCont + 1;
  }
  return isValid;
}

std::string toUtf8(const std::string &text) {
  std::string out;

  if (is_valid_utf8(text))
    out = text;
  else {
    for (unsigned char c : text) {
      if (c < 0x80)
        out.push_back((char)c);
      else {
        out.push_back((char)(0xC0 | (c >> 6)));
        out.push_back((char)(0x80 | (c & 0x3F)));
      }
    }
  }
  return out;
}

void OdfFile::parse(const std::string &text) {
  Section *pCurrent = nullptr;
  size_t pos = 0;

  // skip an UTF-8 byte order mark
  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF
      && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
    pos = 3;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);

    if (eol == std::string::npos)
      eol = text.size();

    std::string line = trim(text.substr(pos, eol - pos));

    pos = eol + 1;
    if (!line.empty() && line[0] != ';') {
      if (line[0] == '[') {
        size_t close = line.find(']');
        std::string name
          = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));

        pCurrent = &m_sections[name];
      } else if (pCurrent) {
        size_t eq = line.find('=');

        if (eq != std::string::npos) {
          std::string key = lower(trim(line.substr(0, eq)));

          // the first occurrence wins, as in GrandOrgue
          if (pCurrent->find(key) == pCurrent->end())
            (*pCurrent)[key] = trim(line.substr(eq + 1));
        }
      }
    }
  }
}

const std::string *OdfFile::find(
  const std::string &section, const std::string &key) const {
  const std::string *pValue = nullptr;
  auto sit = m_sections.find(lower(section));

  if (sit != m_sections.end()) {
    auto kit = sit->second.find(lower(key));

    if (kit != sit->second.end())
      pValue = &kit->second;
  }
  return pValue;
}

bool OdfFile::hasSection(const std::string &section) const {
  return m_sections.find(lower(section)) != m_sections.end();
}

bool OdfFile::has(const std::string &section, const std::string &key) const {
  return find(section, key) != nullptr;
}

std::string OdfFile::get(
  const std::string &section,
  const std::string &key,
  const std::string &def) const {
  const std::string *pValue = find(section, key);

  return pValue ? *pValue : def;
}

long OdfFile::getInt(
  const std::string &section, const std::string &key, long def) const {
  const std::string *pValue = find(section, key);
  long result = def;

  if (pValue && !pValue->empty()) {
    char *pEnd = nullptr;
    long v = std::strtol(pValue->c_str(), &pEnd, 10);

    if (pEnd != pValue->c_str())
      result = v;
  }
  return result;
}

double OdfFile::getFloat(
  const std::string &section, const std::string &key, double def) const {
  const std::string *pValue = find(section, key);
  double result = def;

  if (pValue && !pValue->empty()) {
    std::string v(*pValue);

    for (char &c : v)
      if (c == ',')
        c = '.';

    char *pEnd = nullptr;
    double d = std::strtod(v.c_str(), &pEnd);

    if (pEnd != v.c_str())
      result = d;
  }
  return result;
}

bool OdfFile::getBool(
  const std::string &section, const std::string &key, bool def) const {
  const std::string *pValue = find(section, key);
  bool result = def;

  if (pValue && !pValue->empty()) {
    char c = (char)std::toupper((unsigned char)(*pValue)[0]);

    result = c == 'Y' || c == 'T' || c == '1';
  }
  return result;
}

} // namespace organ
