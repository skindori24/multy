// mcb_report: runs every engine on the downloaded market data and writes the
// results for the dashboard.
//
//   ./build/mcb_report --data data/raw --out site/data
//
// produces site/data/results.json (machine-readable) and site/data/results.js
// (the same document as a script, so site/index.html also works when opened
// straight from disk).
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>

#include "mcb/core/error.hpp"
#include "mcb/data/gsw.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/report/report.hpp"

namespace {

void usage() {
  std::cout << "Usage: mcb_report [options]\n"
               "  --data DIR         downloaded market data          (default data/raw)\n"
               "  --reference DIR    reference data in the repo      (default data/reference)\n"
               "  --quantlib DIR     QuantLib reference values       (default tests/data/ql)\n"
               "  --portfolio FILE   instruments to price            (default data/portfolio/usd_portfolio.csv)\n"
               "  --out DIR          output directory                (default site/data)\n"
               "  --replications N   bootstrap replications          (default 2000)\n"
               "  --paths N          simulated forecast paths        (default 10000)\n"
               "  --strict           exit with an error if a validation check fails\n"
               "  --write-gsw-extract FILE   refresh the month-end GSW extract from --data and exit\n";
}

std::string utcNow() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  mcb::ReportOptions opt;
  std::string outDir = "site/data", gswExtract;
  bool strict = false;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto value = [&]() -> std::string {
        MCB_REQUIRE(i + 1 < argc, "option " << a << " needs a value");
        return argv[++i];
      };
      if (a == "--help" || a == "-h") {
        usage();
        return 0;
      } else if (a == "--data") opt.dataDir = value();
      else if (a == "--reference") opt.referenceDir = value();
      else if (a == "--quantlib") opt.quantlibDir = value();
      else if (a == "--portfolio") opt.portfolioFile = value();
      else if (a == "--out") outDir = value();
      else if (a == "--replications") opt.bootstrapReplications = std::stoi(value());
      else if (a == "--paths") opt.forecastPaths = std::stoi(value());
      else if (a == "--strict") strict = true;
      else if (a == "--write-gsw-extract") gswExtract = value();
      else MCB_FAIL("unknown option '" << a << "' (try --help)");
    }

    if (!gswExtract.empty()) {
      // Month-end Svensson parameters since 1971: a small file that lets the
      // tests and the report run without the 16 MB download.
      const auto daily = mcb::parseGswParameters(mcb::readTextFile(opt.dataDir + "/fed/feds200628.csv"));
      const auto monthly = mcb::endOfPeriodObservations(daily, mcb::PanelFrequency::Monthly, mcb::Date(1971, 8, 1),
                                                        daily.back().date);
      mcb::writeTextFile(gswExtract, mcb::writeSvenssonCsv(monthly));
      std::cout << "Wrote " << monthly.size() << " month-end curves to " << gswExtract << "\n";
      return 0;
    }

    opt.generatedAt = utcNow();
    const auto t0 = std::chrono::steady_clock::now();
    const mcb::Json doc = mcb::buildReport(opt, std::cout);

    std::filesystem::create_directories(outDir);
    const std::string json = doc.dump(1);
    mcb::writeTextFile(outDir + "/results.json", json);
    mcb::writeTextFile(outDir + "/results.js", "window.MCB_RESULTS = " + doc.dump() + ";\n");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Wrote %s/results.json (%.0f kB) in %.1f s\n", outDir.c_str(), json.size() / 1024.0, seconds);

    if (strict && doc["meta"]["checksFailed"].numberOr(0.0) > 0.0) {
      std::cerr << "Validation checks failed (--strict)\n";
      return 2;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "mcb_report: " << e.what() << "\n";
    return 1;
  }
}
