#include "mcb/io/csv.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "mcb/core/error.hpp"

namespace mcb {

int CsvTable::col(std::string_view name) const {
  for (std::size_t i = 0; i < header.size(); ++i)
    if (header[i] == name) return static_cast<int>(i);
  return -1;
}

int CsvTable::requireCol(std::string_view name) const {
  const int i = col(name);
  MCB_REQUIRE(i >= 0, "CSV column '" << name << "' not found");
  return i;
}

const std::string& CsvTable::at(std::size_t row, std::string_view column) const {
  static const std::string empty;
  const auto c = static_cast<std::size_t>(requireCol(column));
  const auto& r = rows.at(row);
  return c < r.size() ? r[c] : empty;
}

CsvTable parseCsv(std::string_view text, char delimiter, bool hasHeader) {
  CsvTable table;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);  // UTF-8 byte-order mark

  std::vector<std::string> record;
  std::string field;
  bool inQuotes = false, fieldWasQuoted = false, recordHasContent = false;

  auto endField = [&] {
    record.push_back(fieldWasQuoted ? field : trim(field));
    field.clear();
    fieldWasQuoted = false;
  };
  auto endRecord = [&] {
    endField();
    if (recordHasContent) {
      if (hasHeader && table.header.empty())
        table.header = std::move(record);
      else
        table.rows.push_back(std::move(record));
    }
    record.clear();
    recordHasContent = false;
  };

  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (inQuotes) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          field += '"';
          ++i;
        } else {
          inQuotes = false;
        }
      } else {
        field += c;
      }
    } else if (c == '"') {
      inQuotes = true;
      fieldWasQuoted = true;
      recordHasContent = true;
    } else if (c == delimiter) {
      endField();
      recordHasContent = true;
    } else if (c == '\n') {
      endRecord();
    } else if (c != '\r') {
      field += c;
      if (!std::isspace(static_cast<unsigned char>(c))) recordHasContent = true;
    }
  }
  if (!field.empty() || !record.empty() || recordHasContent) endRecord();
  return table;
}

std::string readTextFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  MCB_REQUIRE(in.good(), "cannot open file '" << path << "'");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool fileExists(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return in.good();
}

void writeTextFile(const std::string& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary);
  MCB_REQUIRE(out.good(), "cannot write file '" << path << "'");
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  MCB_REQUIRE(out.good(), "write failed for '" << path << "'");
}

CsvTable readCsvFile(const std::string& path, char delimiter, bool hasHeader) {
  return parseCsv(readTextFile(path), delimiter, hasHeader);
}

std::string trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}

std::vector<std::string> split(std::string_view s, char delimiter) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (;;) {
    const auto pos = s.find(delimiter, start);
    if (pos == std::string_view::npos) {
      out.emplace_back(s.substr(start));
      break;
    }
    out.emplace_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

bool tryParseDouble(std::string_view s, double& out) {
  const std::string t = trim(s);
  if (t.empty() || t == "NA" || t == "N/A" || t == "NaN" || t == "nan" || t == "null" || t == "-" || t == ".")
    return false;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

double parseDouble(std::string_view s, std::string_view context) {
  double v = 0.0;
  MCB_REQUIRE(tryParseDouble(s, v), "cannot parse number '" << s << "'" << (context.empty() ? "" : " in ") << context);
  return v;
}

}  // namespace mcb
