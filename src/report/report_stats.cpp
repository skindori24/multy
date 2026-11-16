// Report section: the statistical engine (Crump-Gospodinov bootstrap) on the
// Fed's GSW yield history: expectations-hypothesis tests, scenario forecasts
// from today's curve and predictive regressions with valid p-values.
#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/data/gsw.hpp"
#include "mcb/data/usd_market.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/stats/cg_bootstrap.hpp"
#include "mcb/stats/eh_tests.hpp"
#include "mcb/stats/forecast.hpp"
#include "sections.hpp"

namespace mcb::report {
namespace {

const int kMaturities = 40;      // quarterly maturities up to 10 years, as in the paper
const double kPeriod = 0.25;     // years per period

double annualPercent(double perPeriod) { return perPeriod / kPeriod * 100.0; }

std::string tenorLabel(int quarters) {
  if (quarters % 4 == 0) return std::to_string(quarters / 4) + "Y";
  return std::to_string(quarters * 3) + "M";
}

bool isCompleteQuarter(Date d) {
  const auto c = d.ymd();
  return c.month % 3 == 0 && c.day >= 23;
}

Json distributionJson(const Distribution& d, double scale, int decimals) {
  return Json::object()
      .set("mean", num(d.mean * scale, decimals))
      .set("q05", num(d.q05 * scale, decimals))
      .set("q16", num(d.q16 * scale, decimals))
      .set("q50", num(d.q50 * scale, decimals))
      .set("q84", num(d.q84 * scale, decimals))
      .set("q95", num(d.q95 * scale, decimals));
}

// Share of variance explained by the first three principal components.
Json pcaShares(const Eigen::MatrixXd& X) {
  const PcaResult p = pca(X, true);
  std::vector<double> shares;
  for (int i = 0; i < 3 && i < p.explainedShare.size(); ++i) shares.push_back(roundTo(p.explainedShare[i] * 100.0, 2));
  return Json(shares);
}

}  // namespace

Json statsSection(Context& ctx) {
  try {
    ctx.info("Statistics: CG bootstrap on the Fed GSW yield history");
    // ---- data -----------------------------------------------------------------
    const std::string live = joinPath(ctx.opt.dataDir, "fed/feds200628.csv");
    const std::string frozen = joinPath(ctx.opt.referenceDir, "gsw_monthend.csv");
    std::vector<SvenssonParams> daily;
    bool usingLive = false;
    if (fileExists(live)) {
      try {
        daily = parseGswParameters(readTextFile(live));
        usingLive = true;
      } catch (const std::exception& e) {
        ctx.info(std::string("  live GSW file unusable (") + e.what() + "), using the reference extract");
      }
    }
    if (daily.empty()) daily = parseGswParameters(readTextFile(frozen));
    const SvenssonParams latest = daily.back();
    ctx.source("Fed GSW nominal yield curve (Gurkaynak-Sack-Wright)", latest.date.iso(), usingLive ? "ok" : "fallback",
               usingLive ? std::to_string(daily.size()) + " daily curves since 1961"
                         : "live download missing: month-end extract shipped with the repository",
               "https://www.federalreserve.gov/data/nominal-yield-curve.htm");

    auto obs = endOfPeriodObservations(daily, PanelFrequency::Quarterly, Date(1972, 1, 1), latest.date);
    if (!obs.empty() && !isCompleteQuarter(obs.back().date)) obs.pop_back();  // drop the unfinished quarter
    MCB_REQUIRE(obs.size() >= 60, "GSW history too short");
    const YieldPanel panel = buildYieldPanel(obs, PanelFrequency::Quarterly, kMaturities);
    const CgBootstrap cg(panel.yields);
    const Eigen::Index T = panel.T();

    Json stats = Json::object().set("available", true);
    Json sample = Json::object()
                      .set("from", panel.dates.front().iso())
                      .set("to", panel.dates.back().iso())
                      .set("observations", static_cast<double>(T))
                      .set("maturities", kMaturities)
                      .set("frequency", "quarterly")
                      .set("blockLength", cg.blockLength())
                      .set("arCoefficient", num(cg.persistentModel().Phi(0, 0), 4))
                      .set("biasCorrected", cg.persistentModel().biasCorrected)
                      .set("replications", ctx.opt.bootstrapReplications);
    // persistence of the objects involved: the motivation for the method
    double drAc = 0.0;
    for (Eigen::Index j = 0; j < cg.differenceReturns().cols(); ++j)
      drAc = std::max(drAc, std::fabs(autocorrelation(cg.differenceReturns().col(j))));
    sample.set("autocorrYield5y", num(autocorrelation(panel.yields.col(19)), 3))
        .set("autocorrLongForward", num(autocorrelation(cg.observed().forwards.col(kMaturities - 1)), 3))
        .set("maxAutocorrDifferenceReturn", num(drAc, 3))
        .set("pcaYields", pcaShares(panel.yields))
        .set("pcaDifferenceReturns", pcaShares(cg.differenceReturns()));
    stats.set("sample", sample);

    // ---- one bootstrap history next to the real one (Figure 2 of the paper) ---
    {
      std::mt19937_64 rng(2);
      const CgSample s = cg.draw(rng);
      std::vector<std::string> dates;
      std::vector<double> real2, real10, boot2, boot10;
      for (Eigen::Index t = 0; t < T; ++t) {
        dates.push_back(panel.dates[static_cast<std::size_t>(t)].iso());
        real2.push_back(roundTo(annualPercent(panel.yields(t, 7)), 3));
        real10.push_back(roundTo(annualPercent(panel.yields(t, 39)), 3));
        boot2.push_back(roundTo(annualPercent(s.yields(t, 7)), 3));
        boot10.push_back(roundTo(annualPercent(s.yields(t, 39)), 3));
      }
      stats.set("exampleHistory", Json::object()
                                      .set("dates", dates)
                                      .set("real2y", real2)
                                      .set("real10y", real10)
                                      .set("boot2y", boot2)
                                      .set("boot10y", boot10));
      // identities hold in the resampled history
      const double identityError = (yieldsFromForwards(s.forwards) - s.yields).cwiseAbs().maxCoeff();
      ctx.check("Statistics", "Bootstrapped curves satisfy the yield/forward identities", identityError, 1e-12, "",
                "yields are rebuilt from resampled forwards, so the identities hold by construction");
    }

    // ---- expectations-hypothesis tests (section 4.2) -------------------------
    ctx.info("  expectations-hypothesis tests");
    const std::vector<EhTestSpec> specs = {{EhTestKind::CS2, 4}, {EhTestKind::CS2, 20}, {EhTestKind::CS1, 0},
                                           {EhTestKind::FB, 0},  {EhTestKind::CG, 4},   {EhTestKind::CG, 20}};
    const auto eh = runEhTests(cg, specs, ctx.opt.bootstrapReplications, 884);
    Json ehJson = Json::array();
    for (const auto& test : eh) {
      std::vector<double> years, slope, q05, q16, q50, q84, q95, pOne, pZero;
      for (const auto& m : test.byMaturity) {
        years.push_back(m.maturity * kPeriod);
        slope.push_back(roundTo(m.slope, 4));
        q05.push_back(roundTo(m.q05, 4));
        q16.push_back(roundTo(m.q16, 4));
        q50.push_back(roundTo(m.q50, 4));
        q84.push_back(roundTo(m.q84, 4));
        q95.push_back(roundTo(m.q95, 4));
        pOne.push_back(roundTo(m.pSlopeIsOne, 4));
        pZero.push_back(roundTo(m.pSlopeIsZero, 4));
      }
      ehJson.push(Json::object()
                      .set("test", test.spec.label())
                      .set("years", years)
                      .set("slope", slope)
                      .set("q05", q05)
                      .set("q16", q16)
                      .set("q50", q50)
                      .set("q84", q84)
                      .set("q95", q95)
                      .set("pSlopeIsOne", pOne)
                      .set("pSlopeIsZero", pZero));
    }
    stats.set("ehTests", ehJson);
    {
      // The paper's qualitative findings, re-checked on the updated sample.
      auto lastOf = [&](std::size_t i) { return eh[i].byMaturity.back(); };
      const double cs2 = lastOf(0).slope, cg4 = lastOf(4).slope;
      const double widthCg = lastOf(4).q95 - lastOf(4).q05, widthCs2 = lastOf(0).q95 - lastOf(0).q05;
      ctx.check("Statistics", "Paper replication: Campbell-Shiller (CS2, m=4) 10Y slope is negative", cs2, 0.0, "",
                "the classic rejection of the expectations hypothesis");
      ctx.check("Statistics", "Paper replication: CG test is more precisely estimated than CS2", widthCg / widthCs2, 0.5,
                "ratio", "width of the bootstrap 5-95% band, CG (m=4) relative to CS2 (m=4), 10Y");
      ctx.note("Statistics", "CG test (m=4) 10Y slope: 1 under the expectations hypothesis", cg4, "",
               "bootstrap p-value of slope = 1: " + std::to_string(lastOf(4).pSlopeIsOne).substr(0, 5));
    }

    // ---- today's curve ----------------------------------------------------------
    Eigen::VectorXd current(kMaturities);
    std::string currentSource;
    Date currentDate;
    if (ctx.treasury) {
      const YieldCurve& c = ctx.treasury->curve("USD-TSY");
      for (int n = 1; n <= kMaturities; ++n) current[n - 1] = c.zeroRateT(n * kPeriod) * kPeriod;
      currentSource = "Treasury curve bootstrapped from par yields";
      currentDate = ctx.treasury->asOf;
    } else {
      for (int n = 1; n <= kMaturities; ++n) current[n - 1] = svenssonZeroYield(latest, n * kPeriod) * kPeriod;
      currentSource = "latest Fed GSW curve";
      currentDate = latest.date;
    }
    stats.set("current", Json::object().set("date", currentDate.iso()).set("source", currentSource));

    // Sanity check suggested in the brief: our bootstrapped Treasury zero
    // curve against the Fed's smooth fit for the same day.
    for (const auto& row : ctx.treasuryHistory) {
      if (row.date != latest.date) continue;
      try {
        QuoteMap q;
        const CurveEngine engine = makeTreasuryEngine(row, Interpolation::MonotoneConvex, q);
        const auto set = engine.build(q);
        double worst = 0.0;
        for (double years : {2.0, 3.0, 5.0, 7.0, 10.0})
          worst = std::max(worst, std::fabs(set->curve("USD-TSY").zeroRateT(years) - svenssonZeroYield(latest, years)) * 1e4);
        ctx.note("USD Treasury", "Bootstrapped zero rates vs the Fed's GSW fit, same day (2Y-10Y)", worst, "bp",
                 "as of " + latest.date.iso() +
                     ": a few bp are expected, GSW is a smooth fit to off-the-run bonds, not an exact bootstrap");
      } catch (const std::exception&) {
      }
    }

    // ---- scenario forecast ---------------------------------------------------------
    ctx.info("  scenario forecast from today's curve");
    ForecastOptions fo;
    fo.horizon = 8;
    fo.paths = ctx.opt.forecastPaths;
    fo.maturities = {1, 4, 8, 20, 40};
    const ScenarioForecast fans = simulateScenarios(cg, current, Eigen::VectorXd(), fo);
    Json fc = Json::object().set("horizonQuarters", fo.horizon).set("paths", fo.paths);
    std::vector<std::string> stepDates;
    for (int s = 0; s <= fo.horizon; ++s) stepDates.push_back(currentDate.addMonths(3 * s).iso());
    fc.set("dates", stepDates);
    Json fanJson = Json::array();
    for (const auto& fan : fans.fans) {
      std::vector<double> fwd, mean, q05, q16, q50, q84, q95;
      for (std::size_t s = 0; s < fan.steps.size(); ++s) {
        fwd.push_back(std::isfinite(fan.forwardImplied[s]) ? roundTo(annualPercent(fan.forwardImplied[s]), 3) : std::nan(""));
        mean.push_back(roundTo(annualPercent(fan.steps[s].mean), 3));
        q05.push_back(roundTo(annualPercent(fan.steps[s].q05), 3));
        q16.push_back(roundTo(annualPercent(fan.steps[s].q16), 3));
        q50.push_back(roundTo(annualPercent(fan.steps[s].q50), 3));
        q84.push_back(roundTo(annualPercent(fan.steps[s].q84), 3));
        q95.push_back(roundTo(annualPercent(fan.steps[s].q95), 3));
      }
      fanJson.push(Json::object()
                       .set("tenor", tenorLabel(fan.maturity))
                       .set("current", num(annualPercent(fan.current), 3))
                       .set("forwardImplied", fwd)
                       .set("mean", mean)
                       .set("q05", q05)
                       .set("q16", q16)
                       .set("q50", q50)
                       .set("q84", q84)
                       .set("q95", q95));
    }
    fc.set("fans", fanJson);
    std::vector<double> expShort, fwdShort;
    for (int s = 0; s <= fo.horizon; ++s) {
      expShort.push_back(roundTo(annualPercent(fans.expectedShortRate[static_cast<std::size_t>(s)]), 3));
      fwdShort.push_back(roundTo(annualPercent(fans.forwardShortRate[static_cast<std::size_t>(s)]), 3));
    }
    fc.set("shortRate", Json::object().set("expected", expShort).set("forward", fwdShort));
    stats.set("forecast", fc);

    // ---- one-year holding-period returns and the trade signals -----------------------
    ctx.info("  holding-period returns and predictive regressions");
    const int H = 4;
    ForecastOptions ro;
    ro.horizon = H;
    ro.paths = ctx.opt.forecastPaths;
    ro.maturities = {8, 12, 20, 28, 40};
    ro.seed = 4242;
    const ScenarioForecast ret = simulateScenarios(cg, current, Eigen::VectorXd(), ro);
    const int reps = std::min(ctx.opt.bootstrapReplications, 1000);
    Json returns = Json::array();
    for (const auto& r : ret.returns) {
      const PredictiveForecast pf = predictiveForecast(cg, r.maturity, H, current, Eigen::VectorXd(), {}, reps, 17);
      const double prob = r.probPositive, fitted = pf.fitted * 100.0;
      const std::string signal = (prob >= 0.60 && fitted > 0.0) ? "BUY" : ((prob <= 0.40 && fitted < 0.0) ? "SELL" : "HOLD");
      const std::string tenor = tenorLabel(r.maturity);
      Json row = Json::object()
                     .set("tenor", tenor)
                     .set("yield", num(annualPercent(current[r.maturity - 1]), 3))
                     .set("carryRollDown", num(r.carryRollDown * 100.0, 3))
                     .set("breakEvenBp", num(r.breakEvenYieldRise / kPeriod * 1e4, 1))
                     .set("simulated", distributionJson(r.excessReturn, 100.0, 3))
                     .set("probPositive", num(prob, 3))
                     .set("regressionExpected", num(fitted, 3))
                     .set("regressionLo68", num(pf.fittedLo68 * 100.0, 3))
                     .set("regressionHi68", num(pf.fittedHi68 * 100.0, 3))
                     .set("regressionLo90", num(pf.fittedLo90 * 100.0, 3))
                     .set("regressionHi90", num(pf.fittedHi90 * 100.0, 3))
                     .set("historicalMean", num(pf.unconditionalMean * 100.0, 3))
                     .set("slopeBeta", num(pf.beta[2], 3))
                     .set("slopePBootstrap", num(pf.pBootstrap[2], 3))
                     .set("slopePAsymptotic", num(pf.pAsymptotic[2], 4))
                     .set("r2", num(pf.r2, 3))
                     .set("signal", signal);
      returns.push(row);

      char why[320];
      std::snprintf(why, sizeof why,
                    "P(excess return > 0) = %.0f%%; simulated mean %+.2f%%, regression forecast %+.2f%% (68%% band %+.2f%% .. %+.2f%%); "
                    "carry and roll-down %+.2f%%",
                    prob * 100.0, r.excessReturn.mean * 100.0, fitted, pf.fittedLo68 * 100.0, pf.fittedHi68 * 100.0,
                    r.carryRollDown * 100.0);
      ctx.signals.push(Json::object()
                           .set("market", "US Treasuries")
                           .set("asset", tenor + " Treasury vs 1Y bill")
                           .set("signal", signal)
                           .set("horizon", "12 months")
                           .set("edge", num(r.excessReturn.mean * 100.0, 2))
                           .set("edgeUnit", "% expected excess return")
                           .set("confidence", num(prob, 2))
                           .set("confidenceLabel", "P(return > 0)")
                           .set("why", std::string(why)));
    }
    stats.set("returns", Json::object()
                             .set("holdingQuarters", H)
                             .set("rule", "BUY when the simulated probability of a positive 12-month excess return is at "
                                          "least 60% and the regression forecast is positive; SELL when it is at most 40% "
                                          "and the regression forecast is negative; otherwise HOLD")
                             .set("rows", returns));
    return stats;
  } catch (const std::exception& e) {
    ctx.info(std::string("  statistics section unavailable: ") + e.what());
    return unavailable(e.what());
  }
}

}  // namespace mcb::report
