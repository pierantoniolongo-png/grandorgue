// Organ Definition File (GrandOrgue .organ) reader.
// License GPL-2.0 or later.
#pragma once

#include <string>
#include <unordered_map>

namespace organ {

/** Formats "<prefix>%03d", the numbering scheme used in every ODF key */
std::string num3(const std::string &prefix, int n);

/** Converts Latin-1 text to UTF-8 when it is not already valid UTF-8 */
std::string toUtf8(const std::string &text);

/**
 * INI-style organ definition. Section and key names are case-insensitive;
 * values are trimmed.
 */
class OdfFile {
public:
  void parse(const std::string &text);

  bool hasSection(const std::string &section) const;
  bool has(const std::string &section, const std::string &key) const;
  std::string get(
    const std::string &section,
    const std::string &key,
    const std::string &def = "") const;
  long getInt(const std::string &section, const std::string &key, long def)
    const;
  double getFloat(
    const std::string &section, const std::string &key, double def) const;
  bool getBool(const std::string &section, const std::string &key, bool def)
    const;

private:
  using Section = std::unordered_map<std::string, std::string>;
  std::unordered_map<std::string, Section> m_sections;

  const std::string *find(const std::string &section, const std::string &key)
    const;
};

} // namespace organ
