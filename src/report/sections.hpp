#pragma once
// Internal: shared state and helpers for the report sections.
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "mcb/curves/curve_engine.hpp"
#include "mcb/data/usd_market.hpp"
#include "mcb/io/json.hpp"
#include "mcb/report/report.hpp"
#include "mcb/stats/term_panel.hpp"

namespace mcb::report {

struct Context {
  ReportOptions opt;
  std::ostream* log = nullptr;
  Json validation = Json::array();  // rows of the validation report
  Json sources = Json::array();     // data freshness table
  Json signals = Json::array();     // unified trade-signal table

  // Results shared between sections.
  std::shared_ptr<const CurveSet> treasury;  // USD-TSY curve set (if built)
  std::shared_ptr<const CurveSet> sofr;      // USD-SOFR curve set (if built)
  std::vector<TreasuryParRow> treasuryHistory;  // daily Treasury par curves (if loaded)

  void info(const std::string& message) const {
    if (log) (*log) << message << "\n";
  }

  /// Adds a row to the validation report. `ok` is value <= limit.
  void check(const std::string& group, const std::string& name, double value, double limit, const std::string& unit,
             const std::string& detail = "") {
    Json row = Json::object();
    row.set("group", group).set("check", name).set("value", std::isfinite(value) ? Json(value) : Json());
    row.set("limit", limit).set("unit", unit).set("ok", std::isfinite(value) && value <= limit).set("detail", detail);
    validation.push(row);
  }
  /// Adds an informational row (no pass/fail).
  void note(const std::string& group, const std::string& name, double value, const std::string& unit,
            const std::string& detail = "") {
    Json row = Json::object();
    row.set("group", group).set("check", name).set("value", std::isfinite(value) ? Json(value) : Json());
    row.set("limit", Json()).set("unit", unit).set("ok", Json()).set("detail", detail);
    validation.push(row);
  }
  void source(const std::string& name, const std::string& asOf, const std::string& status, const std::string& note,
              const std::string& url) {
    Json row = Json::object();
    row.set("name", name).set("asOf", asOf).set("status", status).set("note", note).set("url", url);
    sources.push(row);
  }
};

inline std::string joinPath(const std::string& dir, const std::string& file) {
  if (dir.empty()) return file;
  return dir.back() == '/' ? dir + file : dir + "/" + file;
}

/// Rounded number for the output file (NaN becomes null).
inline Json num(double v, int decimals) { return std::isfinite(v) ? Json(roundTo(v, decimals)) : Json(); }

/// A section that could not be produced.
inline Json unavailable(const std::string& reason) {
  return Json::object().set("available", false).set("reason", reason);
}

/// Zero / forward curve sampled on a time grid for charts.
Json curveGrid(const DiscountCurve& curve, double maxYears);

Json usdSection(Context& ctx);
Json eurSection(Context& ctx);
Json moexSection(Context& ctx);
/// Uses the bootstrapped Treasury curve in ctx (when available) as "today".
Json statsSection(Context& ctx);

}  // namespace mcb::report
