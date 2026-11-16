// Report section: live USD curves (SOFR swaps and Treasuries), swap spreads
// and the sample portfolio.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <regex>

#include "mcb/core/error.hpp"
#include "mcb/data/usd_market.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/pricing/bonds.hpp"
#include "mcb/pricing/risk.hpp"
#include "mcb/stats/linear_model.hpp"
#include "sections.hpp"

namespace mcb::report {

Json curveGrid(const DiscountCurve& curve, double maxYears) {
  std::vector<double> t, zero, fwd, df;
  auto add = [&](double x) {
    t.push_back(roundTo(x, 4));
    zero.push_back(roundTo(curve.zeroRateT(x) * 100.0, 4));
    fwd.push_back(roundTo(curve.instantaneousForwardT(x) * 100.0, 4));
    df.push_back(roundTo(curve.discountT(x), 6));
  };
  for (double x = 1.0 / 12.0; x <= std::min(2.0, maxYears) + 1e-9; x += 1.0 / 12.0) add(x);
  for (double x = 2.25; x <= std::min(10.0, maxYears) + 1e-9; x += 0.25) add(x);
  for (double x = 10.5; x <= maxYears + 1e-9; x += 0.5) add(x);
  return Json::object().set("years", t).set("zero", zero).set("forward", fwd).set("discount", df);
}

namespace {

namespace fs = std::filesystem;

// All Eris par-curve files in the data directory, by evaluation date.
std::map<Date, ErisParCurve> loadErisHistory(const std::string& dir, Context& ctx) {
  std::map<Date, ErisParCurve> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  const std::regex pattern(R"(Eris_(\d{8})_EOD_ParCouponCurve_SOFR\.csv)");
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    std::smatch m;
    const std::string name = entry.path().filename().string();
    if (!std::regex_match(name, m, pattern)) continue;
    try {
      ErisParCurve c = parseErisParCurve(readTextFile(entry.path().string()));
      out[c.asOf] = std::move(c);
    } catch (const std::exception& e) {
      ctx.info("  skipping " + name + ": " + e.what());
    }
  }
  return out;
}

std::vector<TreasuryParRow> loadTreasuryHistory(const std::string& dir, std::string& sourceName) {
  std::map<Date, TreasuryParRow> byDate;
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("par_yields", 0) != 0) continue;
      try {
        for (auto& row : parseTreasuryParYields(readTextFile(entry.path().string()))) byDate[row.date] = std::move(row);
      } catch (const std::exception&) {
      }
    }
  }
  sourceName = "US Treasury daily par yield curve";
  if (byDate.empty() && fileExists(joinPath(dir, "fred_par_yields.csv"))) {
    for (auto& row : parseFredParYields(readTextFile(joinPath(dir, "fred_par_yields.csv")))) byDate[row.date] = std::move(row);
    sourceName = "FRED (Treasury constant-maturity yields)";
  }
  std::vector<TreasuryParRow> rows;
  for (auto& kv : byDate) rows.push_back(std::move(kv.second));
  return rows;
}

double yieldAt(const TreasuryParRow& row, const char* tenor) {
  const Period p = Period::parse(tenor);
  for (const auto& kv : row.yields)
    if (kv.first == p) return kv.second;
  return std::nan("");
}

double quoteAt(const ErisParCurve& c, const std::string& symbol) {
  for (const auto& q : c.quotes)
    if (q.symbol == symbol) return q.rate;
  return std::nan("");
}

// Builds a curve set, falling back to log-linear if the preferred interpolation fails.
template <class MakeEngine>
std::shared_ptr<const CurveSet> buildWithFallback(const MakeEngine& make, Interpolation preferred, Interpolation& used,
                                                  std::shared_ptr<CurveEngine>& engineOut, Context& ctx) {
  for (Interpolation k : {preferred, Interpolation::LogLinear}) {
    try {
      QuoteMap quotes;
      auto engine = std::make_shared<CurveEngine>(make(k, quotes));
      auto set = engine->build(quotes);
      const auto& rep = set->report(set->names.front());
      if (!rep.converged || rep.maxAbsError > 1e-9) throw Error("bootstrap did not converge");
      used = k;
      engineOut = engine;
      return set;
    } catch (const std::exception& e) {
      ctx.info("  " + interpolationName(k) + " bootstrap failed (" + e.what() + ")");
      if (k == Interpolation::LogLinear) throw;
    }
  }
  return nullptr;
}

Json bootstrapSummary(const CurveSet& set, const std::string& name, Interpolation used) {
  const auto& rep = set.report(name);
  return Json::object()
      .set("interpolation", interpolationName(used))
      .set("method", rep.method)
      .set("passes", rep.passes)
      .set("evaluations", static_cast<double>(rep.evaluations))
      .set("maxRepricingErrorBp", rep.maxAbsError * 1e4);
}

// ---------------------------------------------------------------- portfolio
struct PortfolioRow {
  std::string type, id, side;
  double notional = 0, rate = 0, price = std::nan("");
  std::string tenor, maturity;
};

std::vector<PortfolioRow> loadPortfolio(const std::string& path) {
  std::vector<PortfolioRow> rows;
  if (!fileExists(path)) return rows;
  const CsvTable t = readCsvFile(path);
  for (std::size_t i = 0; i < t.rows.size(); ++i) {
    PortfolioRow r;
    r.type = t.at(i, "type");
    if (r.type.empty() || r.type[0] == '#') continue;
    r.id = t.at(i, "id");
    r.side = t.at(i, "side");
    r.notional = parseDouble(t.at(i, "notional"), "portfolio notional of " + r.id);
    r.tenor = t.at(i, "tenor");
    r.maturity = t.at(i, "maturity");
    r.rate = parseDouble(t.at(i, "rate"), "portfolio rate of " + r.id) / 100.0;
    tryParseDouble(t.at(i, "price"), r.price);
    rows.push_back(std::move(r));
  }
  return rows;
}

Json riskTable(const RiskReport& jac, const RiskReport* bump) {
  Json buckets = Json::array();
  for (std::size_t i = 0; i < jac.buckets.size(); ++i) {
    Json b = Json::object().set("id", jac.buckets[i].id).set("jacobian", num(jac.buckets[i].delta, 2));
    if (bump) b.set("bump", num(bump->buckets[i].delta, 2));
    buckets.push(b);
  }
  Json out = Json::object().set("buckets", buckets).set("sumJacobian", num(jac.bucketSum, 2));
  if (bump) out.set("sumBump", num(bump->bucketSum, 2)).set("parallel", num(bump->parallel, 2));
  return out;
}

}  // namespace

Json usdSection(Context& ctx) {
  Json usd = Json::object();
  const std::string erisDir = joinPath(ctx.opt.dataDir, "eris");
  const std::string tsyDir = joinPath(ctx.opt.dataDir, "treasury");

  // ============================================================== SOFR ======
  std::map<Date, ErisParCurve> erisHistory;
  std::shared_ptr<CurveEngine> sofrEngine;
  const ErisParCurve* erisLatest = nullptr;
  try {
    ctx.info("USD: SOFR curve from Eris par swap rates");
    erisHistory = loadErisHistory(erisDir, ctx);
    MCB_REQUIRE(!erisHistory.empty(), "no Eris par curve files in " << erisDir);
    erisLatest = &erisHistory.rbegin()->second;
    const ErisParCurve& par = *erisLatest;
    Interpolation used = Interpolation::LogCubic;
    ctx.sofr = buildWithFallback(
        [&](Interpolation k, QuoteMap& q) { return makeSofrEngine(par, k, q); }, Interpolation::LogCubic, used,
        sofrEngine, ctx);
    const YieldCurve& curve = ctx.sofr->curve("USD-SOFR");

    Json s = Json::object().set("available", true).set("asOf", par.asOf.iso());
    s.set("bootstrap", bootstrapSummary(*ctx.sofr, "USD-SOFR", used));
    Json quotes = Json::array();
    for (const auto& q : par.quotes) {
      const SwapValue v = priceOis(makeSofrSwap(q, q.rate, true, 1e6), curve, curve);
      quotes.push(Json::object()
                      .set("tenor", q.tenor.str())
                      .set("rate", num(q.rate * 100, 4))
                      .set("maturity", q.alignment.iso())
                      .set("pillar", q.lastPayment.iso())
                      .set("discount", num(curve.discount(q.lastPayment), 6))
                      .set("zero", num(curve.zeroRate(q.lastPayment) * 100, 4))
                      .set("dv01", num(v.annuity * 1e-4, 2))  // USD per 1bp on 1mm notional
                      .set("errorBp", num((v.parRate - q.rate) * 1e4, 9)));
    }
    s.set("quotes", quotes);
    s.set("curve", curveGrid(curve, 30.0));

    // forward-starting swap rates: what the curve implies for future par rates
    Json fwd = Json::array();
    const Date spot = par.quotes.front().effective;
    for (const auto& [startY, lenY] : std::vector<std::pair<int, int>>{{1, 1}, {2, 1}, {3, 2}, {5, 5}, {10, 10}, {1, 10}, {2, 10}, {5, 10}}) {
      const Date start = OisConventions::sofr().calendar.adjust(spot.addYears(startY));
      const OisSwap swap = makeOisSwap(start, start.addYears(lenY), false, OisConventions::sofr());
      fwd.push(Json::object()
                   .set("label", std::to_string(startY) + "Y" + std::to_string(lenY) + "Y")
                   .set("rate", num(priceOis(swap, curve, curve).parRate * 100, 4)));
    }
    s.set("forwardSwaps", fwd);

    // comparison with Eris's own discount factors
    const std::string stamp = std::to_string(par.asOf.year() * 10000 + par.asOf.month() * 100 + par.asOf.day());
    const std::string dfFile = joinPath(erisDir, "Eris_" + stamp + "_EOD_DiscountFactors_SOFR.csv");
    if (fileExists(dfFile)) {
      const auto dfs = parseErisDiscountFactors(readTextFile(dfFile));
      const ErisComparison c = compareWithEris(curve, dfs, par);
      s.set("eris", Json::object()
                        .set("dates", c.datesCompared)
                        .set("maxDfDiff", c.maxAbsDfDiff)
                        .set("worstDate", c.worstDate.isNull() ? Json() : Json(c.worstDate.iso()))
                        .set("maxZeroDiffBp", num(c.maxZeroRateDiffBp, 4))
                        .set("maxPillarDfDiff", c.maxPillarDfDiff)
                        .set("conventionErrorBp", c.maxConventionErrorBp));
      ctx.check("USD SOFR", "Eris swaps repriced from Eris's own discount factors (schedules and formulas)",
                c.maxConventionErrorBp, 1e-4, "bp", "pure test of day counts, calendars, payment lag");
      ctx.check("USD SOFR", "Our zero rates vs Eris's published curve", c.maxZeroRateDiffBp, 0.5, "bp",
                "Eris interpolates through IMM-dated futures; we bootstrap its par rates, so ~0.1 bp is the expected gap");
      ctx.note("USD SOFR", "Largest discount-factor difference vs Eris", c.maxAbsDfDiff, "",
               std::to_string(c.datesCompared) + " daily dates compared");
    }
    ctx.check("USD SOFR", "Par swaps reprice on the bootstrapped curve", ctx.sofr->report("USD-SOFR").maxAbsError * 1e4,
              1e-6, "bp", std::to_string(par.quotes.size()) + " instruments, " + interpolationName(used));
    ctx.source("Eris SOFR par swap curve", par.asOf.iso(), "ok", std::to_string(erisHistory.size()) + " daily files",
               "https://files.erisfutures.com/ftp/");
    usd.set("sofr", s);
  } catch (const std::exception& e) {
    ctx.info(std::string("  SOFR section unavailable: ") + e.what());
    ctx.source("Eris SOFR par swap curve", "", "missing", e.what(), "https://files.erisfutures.com/ftp/");
    usd.set("sofr", unavailable(e.what()));
  }

  // latest SOFR fixing (optional)
  try {
    const std::string f = joinPath(ctx.opt.dataDir, "nyfed/sofr.json");
    if (fileExists(f)) {
      const RateFixing fix = parseNyFedSofr(readTextFile(f));
      usd.set("sofrFixing", Json::object().set("date", fix.date.iso()).set("rate", num(fix.rate * 100, 4)));
      ctx.source("NY Fed SOFR fixing", fix.date.iso(), "ok", "", "https://www.newyorkfed.org/markets/reference-rates/sofr");
    }
  } catch (const std::exception& e) {
    ctx.source("NY Fed SOFR fixing", "", "error", e.what(), "https://www.newyorkfed.org/markets/reference-rates/sofr");
  }

  // ========================================================== Treasury ======
  std::vector<TreasuryParRow> tsyHistory;
  std::shared_ptr<CurveEngine> tsyEngine;
  try {
    ctx.info("USD: Treasury curve from par yields");
    std::string sourceName;
    tsyHistory = loadTreasuryHistory(tsyDir, sourceName);
    MCB_REQUIRE(!tsyHistory.empty(), "no Treasury par yield file in " << tsyDir);
    ctx.treasuryHistory = tsyHistory;
    const TreasuryParRow& row = tsyHistory.back();
    Interpolation used = Interpolation::MonotoneConvex;
    ctx.treasury = buildWithFallback(
        [&](Interpolation k, QuoteMap& q) { return makeTreasuryEngine(row, k, q); }, Interpolation::MonotoneConvex, used,
        tsyEngine, ctx);
    const YieldCurve& curve = ctx.treasury->curve("USD-TSY");

    Json t = Json::object().set("available", true).set("asOf", row.date.iso()).set("source", sourceName);
    t.set("bootstrap", bootstrapSummary(*ctx.treasury, "USD-TSY", used));
    Json quotes = Json::array();
    for (const auto& [tenor, y] : row.yields) {
      const Date maturity = row.date.add(tenor);
      Json q = Json::object()
                   .set("tenor", tenor.str())
                   .set("parYield", num(y * 100, 3))
                   .set("zero", num(curve.zeroRate(maturity) * 100, 4))
                   .set("discount", num(curve.discount(maturity), 6));
      if (tenor.approxYears() >= 0.99) {
        // risk of the par bond: price change per 100 face for 1bp
        const Bond par = makeFixedRateBond("par", maturity, y, 2, row.date, Calendar(Calendar::Kind::Null));
        const BondRisk r = bondRisk(par, row.date, y, YieldConvention::Street);
        q.set("duration", num(r.modifiedDuration, 3)).set("dv01", num(r.dv01, 4));
      }
      quotes.push(q);
    }
    t.set("quotes", quotes);
    t.set("curve", curveGrid(curve, 30.0));

    // history of benchmark yields for the charts (about one year)
    const std::size_t first = tsyHistory.size() > 260 ? tsyHistory.size() - 260 : 0;
    std::vector<std::string> dates;
    std::vector<double> y2, y5, y10, y30;
    for (std::size_t i = first; i < tsyHistory.size(); ++i) {
      dates.push_back(tsyHistory[i].date.iso());
      y2.push_back(roundTo(yieldAt(tsyHistory[i], "2Y") * 100, 3));
      y5.push_back(roundTo(yieldAt(tsyHistory[i], "5Y") * 100, 3));
      y10.push_back(roundTo(yieldAt(tsyHistory[i], "10Y") * 100, 3));
      y30.push_back(roundTo(yieldAt(tsyHistory[i], "30Y") * 100, 3));
    }
    t.set("history", Json::object().set("dates", dates).set("y2", y2).set("y5", y5).set("y10", y10).set("y30", y30));

    ctx.check("USD Treasury", "Bills and par bonds reprice on the bootstrapped curve",
              ctx.treasury->report("USD-TSY").maxAbsError * 1e4, 1e-6, "bp",
              std::to_string(row.yields.size()) + " instruments, " + interpolationName(used));
    ctx.source(sourceName, row.date.iso(), "ok", std::to_string(tsyHistory.size()) + " daily curves",
               "https://home.treasury.gov/resource-center/data-chart-center/interest-rates");
    usd.set("treasury", t);
  } catch (const std::exception& e) {
    ctx.info(std::string("  Treasury section unavailable: ") + e.what());
    ctx.source("US Treasury daily par yield curve", "", "missing", e.what(),
               "https://home.treasury.gov/resource-center/data-chart-center/interest-rates");
    usd.set("treasury", unavailable(e.what()));
  }

  // ====================================================== swap spreads ======
  if (erisLatest && !tsyHistory.empty()) {
    try {
      std::map<Date, const TreasuryParRow*> tsyBy;
      for (const auto& r : tsyHistory) tsyBy[r.date] = &r;
      Json rows = Json::array();
      for (const char* tenor : {"2Y", "3Y", "5Y", "7Y", "10Y", "20Y", "30Y"}) {
        std::vector<double> series;
        std::vector<std::string> dates;
        for (const auto& [date, par] : erisHistory) {
          const auto it = tsyBy.find(date);
          if (it == tsyBy.end()) continue;
          const double spread = (quoteAt(par, std::string("SOFR") + tenor) - yieldAt(*it->second, tenor)) * 1e4;
          if (std::isfinite(spread)) {
            series.push_back(spread);
            dates.push_back(date.iso());
          }
        }
        if (series.size() > 250) {  // at most about one year of trading days
          series.erase(series.begin(), series.end() - 250);
          dates.erase(dates.begin(), dates.end() - 250);
        }
        if (series.size() < 2) continue;
        const double last = series.back(), m = mean(series), sd = stdev(series);
        Json row = Json::object()
                       .set("tenor", tenor)
                       .set("asOf", dates.back())
                       .set("spreadBp", num(last, 1))
                       .set("meanBp", num(m, 1))
                       .set("stdevBp", num(sd, 1))
                       .set("zScore", num(sd > 0 ? (last - m) / sd : 0.0, 2))
                       .set("observations", series.size());
        if (std::string(tenor) == "10Y") {
          std::vector<double> rounded;
          for (double v : series) rounded.push_back(roundTo(v, 1));
          usd.set("swapSpreadHistory", Json::object().set("tenor", tenor).set("dates", dates).set("spreadBp", rounded));
        }
        rows.push(row);
      }
      usd.set("swapSpreads", rows);
    } catch (const std::exception& e) {
      usd.set("swapSpreads", Json::array());
      ctx.info(std::string("  swap spreads unavailable: ") + e.what());
    }
  }

  // ========================================================= portfolio ======
  try {
    const auto rows = loadPortfolio(ctx.opt.portfolioFile);
    if (!rows.empty() && (ctx.sofr || ctx.treasury)) {
      ctx.info("USD: pricing the sample portfolio");
      Json items = Json::array();
      std::vector<OisSwap> swaps;
      std::vector<std::pair<Bond, double>> bonds;  // bond, signed face amount
      Date bondSettle;
      for (const auto& r : rows) {
        Json item = Json::object().set("type", r.type).set("id", r.id).set("side", r.side).set("notional", r.notional);
        if (r.type == "OIS" && ctx.sofr) {
          const YieldCurve& c = ctx.sofr->curve("USD-SOFR");
          const OisSwap swap = makeOisSwap(ctx.sofr->asOf, Period::parse(r.tenor), OisConventions::sofr(), r.rate,
                                           r.side == "PAY", r.notional);
          const SwapValue v = priceOis(swap, c, c);
          swaps.push_back(swap);
          item.set("description", r.tenor + " SOFR swap, " + (r.side == "PAY" ? "pay" : "receive") + " fixed")
              .set("rate", num(r.rate * 100, 4))
              .set("fairRate", num(v.parRate * 100, 4))
              .set("value", num(v.npv, 0))
              .set("dv01", num((swap.payFixed ? 1.0 : -1.0) * v.annuity * 1e-4, 0));
          items.push(item);
        } else if (r.type == "UST" && ctx.treasury) {
          const YieldCurve& c = ctx.treasury->curve("USD-TSY");
          const Calendar gov(Calendar::Kind::UsGovernmentBond);
          const Date settle = gov.advance(ctx.treasury->asOf, 1, TimeUnit::Days);
          bondSettle = settle;
          const Bond bond = makeFixedRateBond(r.id, Date::parse(r.maturity), r.rate, 2, settle, gov);
          const double accrued = accruedInterest(bond, settle);
          const double modelDirty = dirtyPriceFromCurve(bond, c, settle);
          const double sign = r.side == "SHORT" ? -1.0 : 1.0;
          const bool hasPrice = std::isfinite(r.price) && r.price > 0.0;
          const double dirty = hasPrice ? r.price + accrued : modelDirty;
          const double y = yieldFromDirtyPrice(bond, settle, dirty, YieldConvention::Street);
          const BondRisk risk = bondRisk(bond, settle, y, YieldConvention::Street);
          bonds.emplace_back(bond, sign * r.notional);
          item.set("description", "Treasury " + std::to_string(r.rate * 100).substr(0, 5) + "% due " + r.maturity)
              .set("rate", num(r.rate * 100, 3))
              .set("modelPrice", num(modelDirty - accrued, 3))
              .set("accrued", num(accrued, 4))
              .set("yield", num(y * 100, 4))
              .set("duration", num(risk.modifiedDuration, 3))
              .set("value", num(sign * r.notional * dirty / 100.0, 0))
              .set("dv01", num(sign * r.notional / 100.0 * risk.dv01, 0));
          if (hasPrice) {
            item.set("marketPrice", num(r.price, 3))
                .set("richCheap", num(r.price - (modelDirty - accrued), 3))  // > 0: rich to the curve
                .set("zSpreadTsyBp", num(zSpread(bond, c, settle, dirty) * 1e4, 2));
            if (ctx.sofr && settle >= ctx.sofr->asOf) {
              const YieldCurve& s = ctx.sofr->curve("USD-SOFR");
              item.set("zSpreadSofrBp", num(zSpread(bond, s, settle, dirty) * 1e4, 2))
                  .set("aswSpreadBp", num(assetSwapSpread(bond, s, settle, dirty) * 1e4, 2));
            }
          }
          items.push(item);
        }
      }
      Json pf = Json::object().set("items", items).set("file", ctx.opt.portfolioFile);

      // bucketed risk, both ways, for the swaps (SOFR curve) and the bonds (Treasury curve)
      if (!swaps.empty() && sofrEngine) {
        const CurvePricer pricer = [swaps](const CurveSet& cs) {
          double v = 0.0;
          for (const auto& s : swaps) v += priceOis(s, cs.curve("USD-SOFR"), cs.curve("USD-SOFR")).npv;
          return v;
        };
        const RiskReport jac = jacobianRisk(*sofrEngine, *ctx.sofr, pricer);
        const RiskReport bump = bumpAndRebuildRisk(*sofrEngine, *ctx.sofr, pricer);
        pf.set("swapRisk", riskTable(jac, &bump));
        ctx.check("Risk", "USD swaps: bucket deltas add up to the parallel DV01",
                  std::fabs(bump.bucketSum - bump.parallel) / std::max(1.0, std::fabs(bump.parallel)) * 100.0, 1.0, "%",
                  "bump-and-rebuild, +1bp per SOFR tenor vs all tenors together");
        double worst = 0.0, scale = 1.0;
        for (std::size_t i = 0; i < jac.buckets.size(); ++i) {
          worst = std::max(worst, std::fabs(jac.buckets[i].delta - bump.buckets[i].delta));
          scale = std::max(scale, std::fabs(bump.buckets[i].delta));
        }
        ctx.check("Risk", "USD swaps: Jacobian deltas agree with bump-and-rebuild", worst / scale * 100.0, 1.0, "%",
                  "largest bucket difference relative to the largest bucket");
      }
      if (!bonds.empty() && tsyEngine) {
        const Date settle = bondSettle;
        const CurvePricer pricer = [bonds, settle](const CurveSet& cs) {
          double v = 0.0;
          for (const auto& [b, face] : bonds) v += face / 100.0 * dirtyPriceFromCurve(b, cs.curve("USD-TSY"), settle);
          return v;
        };
        const RiskReport jac = jacobianRisk(*tsyEngine, *ctx.treasury, pricer);
        const RiskReport bump = bumpAndRebuildRisk(*tsyEngine, *ctx.treasury, pricer);
        pf.set("bondRisk", riskTable(jac, &bump));
      }
      usd.set("portfolio", pf);
    }
  } catch (const std::exception& e) {
    ctx.info(std::string("  portfolio unavailable: ") + e.what());
    usd.set("portfolio", unavailable(e.what()));
  }
  return usd;
}

}  // namespace mcb::report
