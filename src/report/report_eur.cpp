// Report section: the EUR multicurve reference set-up (OIS discounting,
// Euribor 6M and 3M projection curves), its validation against QuantLib,
// a worked risk example and the latency benchmark.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "mcb/core/error.hpp"
#include "mcb/data/eur_reference.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/pricing/risk.hpp"
#include "mcb/pricing/swaps.hpp"
#include "mcb/stats/linear_model.hpp"
#include "sections.hpp"

namespace mcb::report {
namespace {

struct EurBuild {
  std::shared_ptr<CurveEngine> engine;
  std::shared_ptr<const CurveSet> curves;
  QuoteMap quotes;
  EurSetup setup;
};

EurBuild buildEur(const ReferenceMarket& market, Interpolation k) {
  EurBuild b;
  b.engine = std::make_shared<CurveEngine>(makeEurEngine(market, k, true, b.quotes, &b.setup));
  b.curves = b.engine->build(b.quotes);
  return b;
}

// Root-mean-square second difference of the 6M forward curve on a monthly
// grid (in bp): a simple measure of how jagged the forwards are.
double forwardRoughnessBp(const YieldCurve& curve, const IborIndex& index, Date asOf, int years) {
  std::vector<double> f;
  for (int m = 1; m <= 12 * years; ++m) {
    const Date start = index.calendar.adjust(asOf.addMonths(m));
    f.push_back(curve.forwardRate(start, index.maturityDate(start), index.dayCount) * 1e4);
  }
  double sum = 0.0;
  for (std::size_t i = 1; i + 1 < f.size(); ++i) {
    const double d2 = f[i + 1] - 2.0 * f[i] + f[i - 1];
    sum += d2 * d2;
  }
  return std::sqrt(sum / static_cast<double>(f.size() - 2));
}

double microsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

template <class F>
Json timeIt(const std::string& label, int iterations, F&& f) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    f(i);
    samples.push_back(microsSince(t0));
  }
  return Json::object()
      .set("operation", label)
      .set("medianMicros", num(quantile(samples, 0.5), 1))
      .set("p95Micros", num(quantile(samples, 0.95), 1))
      .set("iterations", iterations);
}

}  // namespace
}  // namespace mcb::report

namespace mcb {

Json runLatencyBenchmark(const std::string& eurReferenceFile, int iterations) {
  using namespace mcb::report;
  const ReferenceMarket market = loadReferenceMarket(eurReferenceFile);
  Json rows = Json::array();
  for (Interpolation k : {Interpolation::LogLinear, Interpolation::LogCubic}) {
    const EurBuild b = buildEur(market, k);
    const std::string tag = " (" + interpolationName(k) + ")";
    double sink = 0.0;
    rows.push(timeIt("Full build: 3 curves, 90 instruments" + tag, iterations, [&](int) {
      sink += b.engine->build(b.quotes)->curve("EUR-3M").discountT(10.0);
    }));
    // A tick alternates between two values so every update really changes the quote.
    auto tick = [&](const char* id, double bump) {
      return [&, id, bump](int i) {
        const QuoteMap changed{{id, b.quotes.at(id) + ((i % 2) ? bump : -bump)}};
        sink += b.engine->update(*b.curves, changed)->curve("EUR-3M").discountT(10.0);
      };
    };
    rows.push(timeIt("Tick: one Euribor 3M future" + tag, iterations, tick("EUR3M_FUT_M3", 0.005)));
    rows.push(timeIt("Tick: 30Y Euribor 6M swap" + tag, iterations, tick("EUR6M_SWAP_30Y", 0.00005)));
    rows.push(timeIt("Tick: 5Y OIS (all curves depend on it)" + tag, iterations, tick("EONIA_5Y", 0.00005)));
    if (k == Interpolation::LogLinear) {
      const VanillaSwap swap = makeVanillaSwap(market.asOf, Period{10, TimeUnit::Years}, b.setup.swap6m, 0.02, true, 1e6);
      const auto& p6 = b.curves->curve("EUR-6M");
      const auto& ois = b.curves->curve("EUR-OIS");
      rows.push(timeIt("Price a 10Y swap off built curves", iterations * 10, [&](int) { sink += priceSwap(swap, p6, ois).npv; }));
    }
    (void)sink;
  }
  return rows;
}

}  // namespace mcb

namespace mcb::report {

Json eurSection(Context& ctx) {
  try {
    ctx.info("EUR: multicurve reference market (11 Dec 2012)");
    const std::string file = joinPath(ctx.opt.referenceDir, "eur_20121211.csv");
    const ReferenceMarket market = loadReferenceMarket(file);
    const EurBuild ll = buildEur(market, Interpolation::LogLinear);
    const EurBuild lc = buildEur(market, Interpolation::LogCubic);
    const EurBuild mc = buildEur(market, Interpolation::MonotoneConvex);

    Json eur = Json::object().set("available", true).set("asOf", market.asOf.iso());
    const Date asOf = market.asOf;

    // ---- curves for the charts (log-cubic: smooth forwards) -----------------
    Json curves = Json::object();
    for (const auto& name : lc.curves->names) curves.set(name, curveGrid(lc.curves->curve(name), 30.0));
    eur.set("curves", curves);

    // six-month forward rates projected on each curve, and the basis to OIS
    const IborIndex e6 = lc.setup.swap6m.index, e3 = lc.setup.swap3m.index;
    std::vector<double> years, fOis, f6, f3;
    for (int m = 0; m <= 12 * 28; m += 3) {
      const Date start = e6.calendar.adjust(asOf.addMonths(m).addDays(2));
      const Date end6 = e6.maturityDate(start), end3 = e3.maturityDate(start);
      years.push_back(roundTo(m / 12.0, 3));
      fOis.push_back(roundTo(lc.curves->curve("EUR-OIS").forwardRate(start, end6, DayCount::Act360) * 100, 4));
      f6.push_back(roundTo(lc.curves->curve("EUR-6M").forwardRate(start, end6, DayCount::Act360) * 100, 4));
      f3.push_back(roundTo(lc.curves->curve("EUR-3M").forwardRate(start, end3, DayCount::Act360) * 100, 4));
    }
    eur.set("forwards", Json::object().set("years", years).set("ois6m", fOis).set("euribor6m", f6).set("euribor3m", f3));

    // ---- instruments and repricing errors -----------------------------------
    Json instruments = Json::array();
    double worstError = 0.0;
    for (const EurBuild* b : {&ll, &lc, &mc})
      for (const auto& name : b->curves->names) worstError = std::max(worstError, b->curves->report(name).maxAbsError);
    for (const auto& def : lc.engine->definitions()) {
      const auto& curve = lc.curves->curve(def.name);
      const auto& rep = lc.curves->report(def.name);
      for (std::size_t i = 0; i < def.helpers.size(); ++i) {
        const auto& h = *def.helpers[i];
        const bool isPrice = std::string(h.kind()) == "future";
        const double q = CurveEngine::helperQuote(h, lc.quotes);
        instruments.push(Json::object()
                             .set("curve", def.name)
                             .set("id", h.label)
                             .set("kind", h.kind())
                             .set("quote", num(isPrice ? q : q * 100.0, 4))
                             .set("pillar", h.pillarDate().iso())
                             .set("discount", num(curve.discount(h.pillarDate()), 6))
                             .set("error", rep.errors[i]));
      }
    }
    eur.set("instruments", instruments);
    Json builds = Json::array();
    for (const EurBuild* b : {&ll, &lc, &mc})
      for (const auto& name : b->curves->names) {
        const auto& rep = b->curves->report(name);
        builds.push(Json::object()
                        .set("curve", name)
                        .set("interpolation", interpolationName(b->curves->curve(name).interpolation()))
                        .set("method", rep.method)
                        .set("passes", rep.passes)
                        .set("maxError", rep.maxAbsError));
      }
    eur.set("builds", builds);
    ctx.check("EUR multicurve", "All 90 instruments reprice (3 curves x 3 interpolations)", worstError, 1e-10,
              "quote units", "rates as decimals: 1e-10 is a millionth of a basis point");

    // ---- comparison with QuantLib -------------------------------------------
    const std::string qlCurves = joinPath(ctx.opt.quantlibDir, "eur_curves.csv");
    if (fileExists(qlCurves)) {
      const CsvTable t = readCsvFile(qlCurves);
      Json rows = Json::array();
      double worstLl = 0.0, worstLc = 0.0;
      for (const auto& [variant, build] : std::vector<std::pair<std::string, const EurBuild*>>{
               {"loglinear|par", &ll}, {"logcubic|par", &lc}}) {
        std::map<std::string, std::pair<double, int>> acc;
        for (std::size_t i = 0; i < t.rows.size(); ++i) {
          if (t.at(i, "variant") != variant) continue;
          const double diff = std::fabs(build->curves->curve(t.at(i, "curve")).discount(Date::parse(t.at(i, "date"))) -
                                        parseDouble(t.at(i, "discount")));
          auto& a = acc[t.at(i, "curve")];
          a.first = std::max(a.first, diff);
          ++a.second;
        }
        for (const auto& [curve, a] : acc) {
          rows.push(Json::object()
                        .set("interpolation", variant.substr(0, variant.find('|')))
                        .set("curve", curve)
                        .set("dates", a.second)
                        .set("maxDfDiff", a.first));
          (build == &ll ? worstLl : worstLc) = std::max(build == &ll ? worstLl : worstLc, a.first);
        }
      }
      eur.set("quantlib", rows);
      ctx.check("EUR multicurve", "Discount factors vs QuantLib, log-linear (OIS, 6M, 3M curves)", worstLl, 1e-11, "",
                "same quotes, same conventions; reference generated by scripts/quantlib_reference.py");
      ctx.check("EUR multicurve", "Discount factors vs QuantLib, log-cubic (OIS, 6M, 3M curves)", worstLc, 1e-9, "",
                "non-local interpolation: both libraries iterate to convergence");
    }

    // ---- forward smoothness ---------------------------------------------------
    Json smooth = Json::array();
    for (const EurBuild* b : {&ll, &lc, &mc}) {
      const auto& c6 = b->curves->curve("EUR-6M");
      smooth.push(Json::object()
                      .set("interpolation", interpolationName(c6.interpolation()))
                      .set("roughnessBp", num(forwardRoughnessBp(c6, e6, asOf, 20), 3)));
    }
    eur.set("forwardSmoothness", smooth);
    ctx.note("EUR multicurve", "Forward roughness, log-linear vs log-cubic",
             forwardRoughnessBp(ll.curves->curve("EUR-6M"), e6, asOf, 20) /
                 std::max(1e-12, forwardRoughnessBp(lc.curves->curve("EUR-6M"), e6, asOf, 20)),
             "x", "RMS second difference of monthly 6M forwards: the spline is this many times smoother");

    // ---- a worked pricing and risk example ------------------------------------
    {
      const VanillaSwap swap = makeVanillaSwap(asOf, Period{10, TimeUnit::Years}, ll.setup.swap6m, 0.02, true, 1e6);
      const CurvePricer pricer = [swap](const CurveSet& cs) {
        return priceSwap(swap, cs.curve("EUR-6M"), cs.curve("EUR-OIS")).npv;
      };
      const SwapValue v = priceSwap(swap, ll.curves->curve("EUR-6M"), ll.curves->curve("EUR-OIS"));
      const SwapValue single = priceSwap(swap, ll.curves->curve("EUR-6M"), ll.curves->curve("EUR-6M"));
      const RiskReport bump = bumpAndRebuildRisk(*ll.engine, *ll.curves, pricer);
      const RiskReport jac = jacobianRisk(*ll.engine, *ll.curves, pricer);
      Json buckets = Json::array();
      double worst = 0.0, scale = 1.0;
      for (std::size_t i = 0; i < bump.buckets.size(); ++i) {
        worst = std::max(worst, std::fabs(bump.buckets[i].delta - jac.buckets[i].delta));
        scale = std::max(scale, std::fabs(bump.buckets[i].delta));
        if (std::fabs(bump.buckets[i].delta) < 0.005 && std::fabs(jac.buckets[i].delta) < 0.005) continue;
        buckets.push(Json::object()
                         .set("id", bump.buckets[i].id)
                         .set("curve", bump.buckets[i].curve)
                         .set("bump", num(bump.buckets[i].delta, 2))
                         .set("jacobian", num(jac.buckets[i].delta, 2)));
      }
      eur.set("riskExample", Json::object()
                                 .set("description", "Pay 2.00% fixed vs Euribor 6M for 10 years on EUR 1,000,000")
                                 .set("npv", num(v.npv, 2))
                                 .set("parRate", num(v.parRate * 100, 4))
                                 .set("parRateSingleCurve", num(single.parRate * 100, 4))
                                 .set("buckets", buckets)
                                 .set("sumBump", num(bump.bucketSum, 2))
                                 .set("sumJacobian", num(jac.bucketSum, 2))
                                 .set("parallel", num(bump.parallel, 2)));
      ctx.check("Risk", "EUR swap: bucket deltas add up to the parallel DV01",
                std::fabs(bump.bucketSum - bump.parallel) / std::fabs(bump.parallel) * 100.0, 1.0, "%",
                "90 quotes bumped one at a time vs all together");
      ctx.check("Risk", "EUR swap: Jacobian deltas agree with bump-and-rebuild", worst / scale * 100.0, 1.0, "%",
                "largest bucket difference relative to the largest bucket");
    }

    // ---- latency -----------------------------------------------------------------
    const Json latency = runLatencyBenchmark(file, ctx.opt.benchmarkIterations);
    eur.set("latency", latency);
    for (const auto& row : latency.items()) {
      const std::string op = row["operation"].str();
      if (op.rfind("Full build", 0) == 0 && op.find("log-linear") != std::string::npos)
        ctx.check("Real-time layer", "Full rebuild of the EUR multicurve (median)", row["medianMicros"].number(), 10000.0,
                  "microseconds", "target: under 10 ms on a shared CI runner");
      if (op.rfind("Tick: one Euribor 3M future", 0) == 0 && op.find("log-linear") != std::string::npos)
        ctx.check("Real-time layer", "Incremental update after one quote tick (median)", row["medianMicros"].number(),
                  3000.0, "microseconds", "target: under 3 ms; only the affected curve is re-solved, from the ticked pillar on");
    }
    ctx.source("EUR reference market (Ametrano-Bianchetti / QuantLib cookbook)", market.asOf.iso(), "frozen",
               "validation data set, not live", "https://www.quantlib.org");
    return eur;
  } catch (const std::exception& e) {
    ctx.info(std::string("  EUR section unavailable: ") + e.what());
    return unavailable(e.what());
  }
}

}  // namespace mcb::report
