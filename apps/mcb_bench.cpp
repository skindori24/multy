// mcb_bench: latency of the real-time layer on the EUR multicurve set-up
// (3 curves, 90 instruments).
//
//   ./build/mcb_bench [data/reference/eur_20121211.csv] [iterations]
#include <cstdio>
#include <iostream>
#include <string>

#include "mcb/report/report.hpp"

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : "data/reference/eur_20121211.csv";
  const int iterations = argc > 2 ? std::stoi(argv[2]) : 1000;
  try {
    const mcb::Json rows = mcb::runLatencyBenchmark(file, iterations);
    std::printf("%-58s %12s %12s\n", "operation", "median (us)", "p95 (us)");
    for (const auto& r : rows.items())
      std::printf("%-58s %12.1f %12.1f\n", r["operation"].str().c_str(), r["medianMicros"].number(),
                  r["p95Micros"].number());
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "mcb_bench: " << e.what() << "\n";
    return 1;
  }
}
