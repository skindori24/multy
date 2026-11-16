#pragma once
// Minimal CSV reader (RFC 4180 quoting) plus a few text helpers.
// Market-data files come from several publishers with slightly different
// dialects (quoted headers, CRLF line ends, a UTF-8 byte-order mark), all of
// which are handled here.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcb {

struct CsvTable {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> rows;

  /// Column index by (exact) header name, or -1.
  int col(std::string_view name) const;
  /// Column index by header name; throws if missing.
  int requireCol(std::string_view name) const;
  /// Cell by row index and column name ("" if the row is short).
  const std::string& at(std::size_t row, std::string_view column) const;
};

/// Parses CSV text. When hasHeader is false, the header is left empty.
CsvTable parseCsv(std::string_view text, char delimiter = ',', bool hasHeader = true);
CsvTable readCsvFile(const std::string& path, char delimiter = ',', bool hasHeader = true);

std::string readTextFile(const std::string& path);
bool fileExists(const std::string& path);
void writeTextFile(const std::string& path, std::string_view content);

std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char delimiter);
/// Parses a floating-point number; returns false for empty/"NA"/malformed text.
bool tryParseDouble(std::string_view s, double& out);
/// Parses a number, throwing mcb::Error with the given context on failure.
double parseDouble(std::string_view s, std::string_view context = "");

}  // namespace mcb
