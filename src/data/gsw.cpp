#include "mcb/data/gsw.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "mcb/core/error.hpp"
#include "mcb/io/csv.hpp"

namespace mcb {
namespace {

// (1 - e^{-x}) / x, stable near zero.
double slopeLoading(double x) { return std::fabs(x) < 1e-8 ? 1.0 - 0.5 * x : (1.0 - std::exp(-x)) / x; }

}  // namespace

double svenssonZeroYield(const SvenssonParams& p, double m) {
  if (m <= 0.0) return (p.beta0 + p.beta1) / 100.0;  // limit as m -> 0
  const double x1 = m / p.tau1;
  const double l1 = slopeLoading(x1);
  double y = p.beta0 + p.beta1 * l1 + p.beta2 * (l1 - std::exp(-x1));
  if (p.beta3 != 0.0 && p.tau2 > 0.0) {  // before 1980 the Fed uses the Nelson-Siegel special case
    const double x2 = m / p.tau2;
    y += p.beta3 * (slopeLoading(x2) - std::exp(-x2));
  }
  return y / 100.0;
}

double svenssonForward(const SvenssonParams& p, double m) {
  if (m < 0.0) m = 0.0;
  const double x1 = m / p.tau1;
  double f = p.beta0 + p.beta1 * std::exp(-x1) + p.beta2 * x1 * std::exp(-x1);
  if (p.beta3 != 0.0 && p.tau2 > 0.0) {
    const double x2 = m / p.tau2;
    f += p.beta3 * x2 * std::exp(-x2);
  }
  return f / 100.0;
}

std::vector<SvenssonParams> parseGswParameters(const std::string& csvText) {
  // The Fed file starts with a free-text preamble; the table begins at the
  // line whose first cell is "Date" (the compact extract uses "date").
  std::size_t start = 0;
  for (;;) {
    const std::size_t eol = csvText.find('\n', start);
    const std::string line = csvText.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
    if (line.rfind("Date,", 0) == 0 || line.rfind("date,", 0) == 0) break;
    MCB_REQUIRE(eol != std::string::npos, "GSW file: header row starting with 'Date' not found");
    start = eol + 1;
  }
  CsvTable t = parseCsv(std::string_view(csvText).substr(start));
  for (auto& h : t.header)
    for (auto& ch : h) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const auto cDate = static_cast<std::size_t>(t.requireCol("DATE"));
  const std::size_t cols[6] = {
      static_cast<std::size_t>(t.requireCol("BETA0")), static_cast<std::size_t>(t.requireCol("BETA1")),
      static_cast<std::size_t>(t.requireCol("BETA2")), static_cast<std::size_t>(t.requireCol("BETA3")),
      static_cast<std::size_t>(t.requireCol("TAU1")),  static_cast<std::size_t>(t.requireCol("TAU2"))};
  std::vector<SvenssonParams> out;
  out.reserve(t.rows.size());
  for (const auto& r : t.rows) {
    SvenssonParams p;
    if (r.size() <= cDate || !Date::tryParse(r[cDate], p.date)) continue;
    double v[6];
    bool ok = true;
    for (int i = 0; i < 6 && ok; ++i) ok = cols[i] < r.size() && tryParseDouble(r[cols[i]], v[i]);
    if (!ok || !(v[4] > 0.0)) continue;  // holidays and missing fits
    p.beta0 = v[0], p.beta1 = v[1], p.beta2 = v[2], p.beta3 = v[3], p.tau1 = v[4], p.tau2 = v[5];
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.date < b.date; });
  MCB_REQUIRE(!out.empty(), "GSW file: no usable rows");
  return out;
}

std::string writeSvenssonCsv(const std::vector<SvenssonParams>& rows) {
  std::string out = "date,beta0,beta1,beta2,beta3,tau1,tau2\n";
  char buf[256];
  for (const auto& p : rows) {
    std::snprintf(buf, sizeof buf, "%s,%.15g,%.15g,%.15g,%.15g,%.15g,%.15g\n", p.date.iso().c_str(), p.beta0, p.beta1,
                  p.beta2, p.beta3, p.tau1, p.tau2);
    out += buf;
  }
  return out;
}

std::vector<SvenssonParams> endOfPeriodObservations(const std::vector<SvenssonParams>& daily, PanelFrequency freq,
                                                    Date from, Date to) {
  auto key = [freq](Date d) {
    const auto c = d.ymd();
    return freq == PanelFrequency::Monthly ? c.year * 12 + (c.month - 1) : c.year * 4 + (c.month - 1) / 3;
  };
  std::vector<SvenssonParams> out;
  for (const auto& p : daily) {
    if (p.date < from || p.date > to) continue;
    if (!out.empty() && key(out.back().date) == key(p.date))
      out.back() = p;  // later observation in the same period replaces the earlier one
    else
      out.push_back(p);
  }
  return out;
}

YieldPanel buildYieldPanel(const std::vector<SvenssonParams>& obs, PanelFrequency freq, int maxMaturity) {
  MCB_REQUIRE(!obs.empty() && maxMaturity >= 2, "buildYieldPanel: empty input");
  YieldPanel panel;
  panel.periodYears = freq == PanelFrequency::Monthly ? 1.0 / 12.0 : 0.25;
  panel.yields.resize(static_cast<Eigen::Index>(obs.size()), maxMaturity);
  for (std::size_t t = 0; t < obs.size(); ++t) {
    panel.dates.push_back(obs[t].date);
    for (int n = 1; n <= maxMaturity; ++n)
      panel.yields(static_cast<Eigen::Index>(t), n - 1) =
          svenssonZeroYield(obs[t], n * panel.periodYears) * panel.periodYears;  // per-period log yield
  }
  return panel;
}

}  // namespace mcb
