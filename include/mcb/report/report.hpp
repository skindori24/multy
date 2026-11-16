#pragma once
// Builds the results document shown on the dashboard.
//
// The report is assembled from independent sections (USD, EUR, MOEX,
// statistics).  A section whose input data is missing or broken is reported
// as unavailable with the reason, and the rest of the report is still
// produced, so one failed download never takes the whole dashboard down.
#include <iosfwd>
#include <string>

#include "mcb/io/json.hpp"

namespace mcb {

struct ReportOptions {
  std::string dataDir = "data/raw";              // downloaded market data (see scripts/fetch_data.sh)
  std::string referenceDir = "data/reference";   // frozen reference data shipped with the repository
  std::string quantlibDir = "tests/data/ql";     // QuantLib reference values (optional)
  std::string portfolioFile = "data/portfolio/usd_portfolio.csv";
  std::string generatedAt;                       // ISO timestamp; filled by the caller
  int bootstrapReplications = 2000;              // CG bootstrap draws for the tests
  int forecastPaths = 10000;                     // simulated future curve paths
  int benchmarkIterations = 300;                 // repetitions per latency measurement
};

/// Runs every engine on the available data and returns the results document.
Json buildReport(const ReportOptions& options, std::ostream& log);

/// Latency of curve builds and updates on the EUR reference market, in microseconds.
Json runLatencyBenchmark(const std::string& eurReferenceFile, int iterations);

}  // namespace mcb
