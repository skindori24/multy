// Phase 8: the Crump-Gospodinov bootstrap, the expectations-hypothesis tests
// of the paper (section 4.2 / Figure 4) and the forecast engine.
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <map>

#include "mcb/core/error.hpp"
#include "mcb/data/gsw.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/stats/cg_bootstrap.hpp"
#include "mcb/stats/eh_tests.hpp"
#include "mcb/stats/forecast.hpp"
#include "mcb/stats/term_panel.hpp"
#include "test_util.hpp"

using namespace mcb;

namespace {

// The paper's sample: end-of-quarter GSW yields, quarterly maturities up to 10 years, 1972Q1-2024Q1.
const YieldPanel& paperPanel() {
  static const YieldPanel panel = [] {
    const auto monthly = parseGswParameters(readTextFile(mcbtest::sourcePath("data/reference/gsw_monthend.csv")));
    const auto obs = endOfPeriodObservations(monthly, PanelFrequency::Quarterly, Date(1972, 1, 1), Date(2024, 3, 31));
    return buildYieldPanel(obs, PanelFrequency::Quarterly, 40);
  }();
  return panel;
}

const CgBootstrap& paperBootstrap() {
  static const CgBootstrap cg(paperPanel().yields);
  return cg;
}

}  // namespace

TEST(CgBootstrap, BlockLengthRuleOfThumb) {
  EXPECT_EQ(CgBootstrap::ruleOfThumbBlockLength(209, 40), 37);   // (209 * 40)^(2/5) = 37.06
  EXPECT_EQ(CgBootstrap::ruleOfThumbBlockLength(600, 10), 32);   // the paper's simulations: 6000^0.4 = 32.5
  EXPECT_EQ(paperBootstrap().blockLength(), 37);
  EXPECT_EQ(paperBootstrap().T(), 209);
  EXPECT_EQ(paperBootstrap().N(), 40);
  CgOptions opt;
  opt.blockLength = 5;
  EXPECT_EQ(CgBootstrap(paperPanel().yields, opt).blockLength(), 5);
  EXPECT_THROW(CgBootstrap(paperPanel().yields.topRows(5)), Error);
}

TEST(CgBootstrap, DifferenceReturnsAreTheWeaklyDependentBuildingBlocks) {
  // The motivation of the paper in three numbers: yields and forwards are
  // extremely persistent, difference returns are not.
  const CgBootstrap& cg = paperBootstrap();
  const double yieldAc = autocorrelation(cg.observed().yields.col(19));          // 5-year yield
  const double longFwdAc = autocorrelation(cg.observed().forwards.col(39));      // longest forward
  EXPECT_GT(yieldAc, 0.95);
  EXPECT_GT(longFwdAc, 0.9);
  for (int n : {2, 8, 20, 40}) EXPECT_LT(std::fabs(autocorrelation(cg.differenceReturns().col(n - 2))), 0.3) << n;
  // The pre-whitening autoregression is persistent but stationary after bias correction.
  EXPECT_GT(cg.persistentModel().Phi(0, 0), 0.95);
  EXPECT_LT(cg.persistentModel().maxEigenvalue, 1.0);
  EXPECT_TRUE(cg.persistentModel().biasCorrected);
}

TEST(CgBootstrap, EveryBootstrapHistorySatisfiesTheCurveIdentities) {
  const CgBootstrap& cg = paperBootstrap();
  std::mt19937_64 rng(1);
  for (int b = 0; b < 5; ++b) {
    const CgSample s = cg.draw(rng);
    ASSERT_EQ(s.yields.rows(), 209);
    ASSERT_EQ(s.yields.cols(), 40);
    // conditional on the observed first curve
    EXPECT_LT((s.forwards.row(0) - cg.observed().forwards.row(0)).cwiseAbs().maxCoeff(), 1e-15);
    // yields are averages of forwards, forwards are differences of log prices
    EXPECT_LT((yieldsFromForwards(s.forwards) - s.yields).cwiseAbs().maxCoeff(), 1e-15);
    EXPECT_LT((forwardsFromYields(s.yields) - s.forwards).cwiseAbs().maxCoeff(), 1e-12);
    // excess returns built from bootstrap yields equal sums of bootstrap difference returns
    const Eigen::MatrixXd dr = differenceReturns(s.forwards), rx = excessReturns(s.yields, 1);
    EXPECT_NEAR(rx(100, 39), dr.row(100).sum(), 1e-12);
    // the history is new: it is not a copy of the data
    EXPECT_GT((s.yields - cg.observed().yields).cwiseAbs().maxCoeff(), 1e-4);
  }
}

TEST(CgBootstrap, OneBlockCoveringTheSampleReproducesTheData) {
  // With a single block spanning all T-1 rows, resampling returns the rows in
  // their original order, so the reconstruction must give back the data.
  CgOptions opt;
  opt.blockLength = 208;
  opt.prewhiten = false;
  std::mt19937_64 rng(1);
  CgSample s = CgBootstrap(paperPanel().yields, opt).draw(rng);
  EXPECT_LT((s.yields - paperPanel().yields).cwiseAbs().maxCoeff(), 1e-13);
  // Same with pre-whitening by plain OLS (whose residuals have zero mean).
  opt.prewhiten = true;
  opt.biasCorrect = false;
  s = CgBootstrap(paperPanel().yields, opt).draw(rng);
  EXPECT_LT((s.yields - paperPanel().yields).cwiseAbs().maxCoeff(), 1e-12);
}

TEST(CgBootstrap, PreservesTheDistributionOfReturnsAndThePersistenceOfYields) {
  const CgBootstrap& cg = paperBootstrap();
  std::mt19937_64 rng(42);
  const int B = 200;
  double meanDr = 0, sdDr = 0, acYield = 0, corr = 0;
  auto correlation = [](const Eigen::VectorXd& a, const Eigen::VectorXd& b) {
    const Eigen::ArrayXd x = a.array() - a.mean(), y = b.array() - b.mean();
    return (x * y).sum() / std::sqrt(x.square().sum() * y.square().sum());
  };
  for (int b = 0; b < B; ++b) {
    const CgSample s = cg.draw(rng);
    const Eigen::MatrixXd dr = differenceReturns(s.forwards);
    meanDr += dr.col(18).mean();
    sdDr += std::sqrt((dr.col(18).array() - dr.col(18).mean()).square().mean());
    acYield += autocorrelation(s.yields.col(19));
    corr += correlation(dr.col(10), dr.col(30));
  }
  const Eigen::VectorXd d = cg.differenceReturns().col(18);
  const double sd = std::sqrt((d.array() - d.mean()).square().mean());
  EXPECT_NEAR(meanDr / B, d.mean(), 0.25 * sd);   // average risk premium is retained
  EXPECT_NEAR(sdDr / B, sd, 0.15 * sd);           // and so is volatility
  EXPECT_GT(acYield / B, 0.9);                    // bootstrapped yields are persistent like real ones
  // cross-sectional dependence between maturities survives because whole rows are resampled
  EXPECT_NEAR(corr / B, correlation(cg.differenceReturns().col(10), cg.differenceReturns().col(30)), 0.1);
}

TEST(CgBootstrap, ExternalSeriesAreResampledJointly) {
  const YieldPanel& panel = paperPanel();
  // an external predictor: a noisy copy of the 10-year yield
  Eigen::MatrixXd ext(panel.T(), 1);
  std::mt19937_64 gen(9);
  std::normal_distribution<double> z;
  for (Eigen::Index t = 0; t < panel.T(); ++t) ext(t, 0) = panel.yields(t, 39) * 4 + 0.002 * z(gen);
  const CgBootstrap cg(panel.yields, ext);
  EXPECT_EQ(cg.K(), 1);
  EXPECT_EQ(cg.persistentModel().Phi.rows(), 2);  // VAR(1) on (f(N), external)
  std::mt19937_64 rng(3);
  const CgSample s = cg.draw(rng);
  ASSERT_EQ(s.external.rows(), panel.T());
  ASSERT_EQ(s.external.cols(), 1);
  EXPECT_NEAR(s.external(0, 0), ext(0, 0), 1e-15);
  EXPECT_THROW(CgBootstrap(panel.yields, ext.topRows(10)), Error);
}

TEST(EhTests, RegressionsAreBuiltAsInThePaper) {
  const CgBootstrap& cg = paperBootstrap();
  const Eigen::MatrixXd& Y = cg.observed().yields;
  const Eigen::MatrixXd& F = cg.observed().forwards;
  Eigen::VectorXd x, y;
  // CG: f(n-m)_{t+m} - y(1)_t on f(n)_t - y(1)_t
  ASSERT_TRUE(ehRegression({EhTestKind::CG, 4}, 12, Y, F, x, y));
  EXPECT_EQ(x.size(), 205);
  EXPECT_NEAR(y[10], F(14, 7) - Y(10, 0), 1e-16);
  EXPECT_NEAR(x[10], F(10, 11) - Y(10, 0), 1e-16);
  // FB is CG with m = n - 1
  Eigen::VectorXd x2, y2;
  ASSERT_TRUE(ehRegression({EhTestKind::FB, 0}, 12, Y, F, x, y));
  ASSERT_TRUE(ehRegression({EhTestKind::CG, 11}, 12, Y, F, x2, y2));
  EXPECT_LT((x - x2).cwiseAbs().maxCoeff(), 1e-16);
  EXPECT_LT((y - y2).cwiseAbs().maxCoeff(), 1e-16);
  // CS1: average future short rate on the yield spread
  ASSERT_TRUE(ehRegression({EhTestKind::CS1, 0}, 4, Y, F, x, y));
  EXPECT_NEAR(y[7], (Y(7, 0) + Y(8, 0) + Y(9, 0) + Y(10, 0)) / 4.0 - Y(7, 0), 1e-16);
  EXPECT_NEAR(x[7], Y(7, 3) - Y(7, 0), 1e-16);
  // CS2: change in the long yield on the scaled spread
  ASSERT_TRUE(ehRegression({EhTestKind::CS2, 4}, 20, Y, F, x, y));
  EXPECT_NEAR(y[3], Y(7, 15) - Y(3, 19), 1e-16);
  EXPECT_NEAR(x[3], 4.0 / 16.0 * (Y(3, 19) - Y(3, 3)), 1e-16);
  // undefined cases
  EXPECT_FALSE(ehRegression({EhTestKind::CG, 4}, 4, Y, F, x, y));   // n - m < 1
  EXPECT_FALSE(ehRegression({EhTestKind::CS2, 4}, 41, Y, F, x, y)); // beyond the curve
  EXPECT_EQ(ehOverlap({EhTestKind::CG, 4}, 12), 4);
  EXPECT_EQ(ehOverlap({EhTestKind::FB, 0}, 12), 11);
  EXPECT_EQ(EhTestSpec({EhTestKind::CS2, 20}).label(), "CS2 (m=20)");
}

TEST(EhTests, UnderTheExpectationsHypothesisTheSlopeIsOne) {
  // Build a world in which the EH holds exactly: the short rate is an AR(1)
  // and every forward equals the expected future short rate.
  const int T = 400, N = 12;
  const double phi = 0.9, mu = 0.01;
  std::mt19937_64 rng(17);
  std::normal_distribution<double> z;
  Eigen::MatrixXd f(T, N);
  double r = mu;
  for (int t = 0; t < T; ++t) {
    r = mu + phi * (r - mu) + 0.002 * z(rng);
    for (int n = 1; n <= N; ++n) f(t, n - 1) = mu + std::pow(phi, n - 1) * (r - mu);  // E_t[r_{t+n-1}]
  }
  const Eigen::MatrixXd y = yieldsFromForwards(f);
  Eigen::VectorXd x, dep;
  for (const EhTestSpec spec : {EhTestSpec{EhTestKind::CG, 2}, EhTestSpec{EhTestKind::FB, 0}, EhTestSpec{EhTestKind::CS1, 0}}) {
    ASSERT_TRUE(ehRegression(spec, 6, y, f, x, dep));
    double slope, se;
    slopeWithNeweyWest(x, dep, ehOverlap(spec, 6), slope, se);
    EXPECT_NEAR(slope, 1.0, 4.0 * se + 0.05) << spec.label();
    EXPECT_GT(slope, 0.5) << spec.label();
  }
}

TEST(EhTests, ReplicatesSection42OfThePaper) {
  // Quarterly GSW yields 1972Q1-2024Q1, 1,000 bootstrap draws: the qualitative
  // findings reported around Figure 4 of the paper.
  const std::vector<EhTestSpec> specs = {{EhTestKind::CS2, 4}, {EhTestKind::CS2, 20}, {EhTestKind::CS1, 0},
                                         {EhTestKind::FB, 0},  {EhTestKind::CG, 4},   {EhTestKind::CG, 20}};
  const auto results = runEhTests(paperBootstrap(), specs, 1000, 884);
  ASSERT_EQ(results.size(), 6u);
  std::map<std::string, std::map<int, EhMaturityResult>> r;
  for (const auto& res : results)
    for (const auto& m : res.byMaturity) r[res.spec.label()][m.maturity] = m;
  for (const auto& res : results) {
    const auto& last = res.byMaturity.back();
    std::cout << "  " << res.spec.label() << ": slope at n=" << last.maturity << " is " << last.slope << " (bootstrap 5-95%: "
              << last.q05 << " .. " << last.q95 << ", p[slope=1] = " << last.pSlopeIsOne << ")\n";
  }

  // "For small m ... estimates of beta are negative and large in magnitude" (CS2, m = 4)
  for (const auto& [n, m] : r["CS2 (m=4)"]) EXPECT_LT(m.slope, 0.0) << n;
  EXPECT_LT(r["CS2 (m=4)"][40].slope, -1.5);
  // "... the evidence against the EH for the CS2 test is generally weaker than the point
  //  estimate would suggest as most bootstrap draws lie above it"
  EXPECT_GT(r["CS2 (m=4)"][40].q50, r["CS2 (m=4)"][40].slope);
  // "When m = 20, the point estimate of beta rises but is close to zero for most maturities"
  for (int n : {24, 28, 32, 36, 40}) EXPECT_LT(std::fabs(r["CS2 (m=20)"][n].slope), 0.5) << n;
  // "the CS1 and FB tests ... point estimates are much closer to one and are comfortably positive"
  for (const auto& [n, m] : r["CS1"]) {
    EXPECT_GT(m.slope, 0.3) << n;
    EXPECT_LT(m.slope, 1.0) << n;
  }
  for (const auto& [n, m] : r["FB"]) EXPECT_GT(m.slope, 0.2) << n;
  // "the estimate of beta appears unstable across maturities" (FB) versus
  // "the OLS coefficient is substantially more stable across maturities" (CG)
  auto range = [&](const std::string& test, int from) {
    double lo = 1e9, hi = -1e9;
    for (const auto& [n, m] : r[test])
      if (n >= from) lo = std::min(lo, m.slope), hi = std::max(hi, m.slope);
    return hi - lo;
  };
  EXPECT_GT(range("FB", 8), range("CG (m=4)", 8));
  // CG slopes rise smoothly with maturity
  for (int n = 6; n <= 40; ++n) EXPECT_GE(r["CG (m=4)"][n].slope, r["CG (m=4)"][n - 1].slope - 1e-3) << n;
  // "the sampling uncertainty is lower than in the CS1 and FB tests and significantly smaller than in the CS2 test"
  auto width = [&](const std::string& test, int n) { return r[test][n].q95 - r[test][n].q05; };
  for (int n : {12, 24, 40}) {
    EXPECT_LT(width("CG (m=4)", n), width("FB", n)) << n;
    EXPECT_LT(width("CG (m=4)", n), width("CS1", n)) << n;
    EXPECT_LT(2.0 * width("FB", n), width("CS2 (m=4)", n)) << n;
  }
  // Forwards do carry information about future rates (slope > 0) but less than one-for-one at short horizons.
  EXPECT_LT(r["CG (m=4)"][20].pSlopeIsZero, 0.01);
  EXPECT_LT(r["CG (m=4)"][20].pSlopeIsOne, 0.05);
  EXPECT_GT(r["CG (m=20)"][40].pSlopeIsOne, 0.10);
  EXPECT_EQ(r["CG (m=4)"][20].observations, 205);
}

TEST(Forecast, CarryAndRollDown) {
  Eigen::VectorXd flat = Eigen::VectorXd::Constant(40, 0.01);
  EXPECT_NEAR(carryAndRollDown(flat, 20, 4), 0.0, 1e-15);  // nothing to earn on a flat curve
  Eigen::VectorXd up(40);
  for (int n = 1; n <= 40; ++n) up[n - 1] = 0.008 + 0.0001 * n;  // upward sloping
  const double c = carryAndRollDown(up, 20, 4);
  EXPECT_GT(c, 0.0);
  EXPECT_NEAR(c, -16 * up[15] + 20 * up[19] - 4 * up[3], 1e-15);
  EXPECT_THROW(carryAndRollDown(up, 4, 4), Error);
  const Eigen::Vector3d fac = curveFactors(up);
  EXPECT_NEAR(fac[0], up[39], 1e-15);
  EXPECT_NEAR(fac[1], up[39] - up[0], 1e-15);
  EXPECT_NEAR(fac[2], 2 * up[19] - up[0] - up[39], 1e-15);
}

TEST(Forecast, ScenarioSimulationFromTheLatestCurve) {
  const CgBootstrap& cg = paperBootstrap();
  const Eigen::VectorXd today = paperPanel().yields.row(208).transpose();
  ForecastOptions opt;
  opt.horizon = 8;
  opt.paths = 2000;
  opt.maturities = {4, 8, 20, 40};
  const ScenarioForecast fc = simulateScenarios(cg, today, Eigen::VectorXd(), opt);
  ASSERT_EQ(fc.fans.size(), 4u);
  ASSERT_EQ(fc.expectedShortRate.size(), 9u);
  EXPECT_NEAR(fc.expectedShortRate[0], today[0], 1e-15);  // step 0 is today
  EXPECT_NEAR(fc.forwardShortRate[0], today[0], 1e-15);
  for (const auto& fan : fc.fans) {
    ASSERT_EQ(fan.steps.size(), 8u);
    EXPECT_NEAR(fan.current, today[fan.maturity - 1], 1e-15);
    for (const auto& d : fan.steps) {
      EXPECT_LT(d.q05, d.q16);
      EXPECT_LT(d.q16, d.q50);
      EXPECT_LT(d.q50, d.q84);
      EXPECT_LT(d.q84, d.q95);
    }
    EXPECT_GT(fan.steps[7].stdev, fan.steps[0].stdev);  // uncertainty grows with the horizon
    // the 1-step forward-implied yield is the forward average, e.g. for n = 4: mean f(2..5)
    if (fan.maturity == 4) {
      const Eigen::VectorXd f0 = forwardsFromYields(today.transpose()).row(0).transpose();
      EXPECT_NEAR(fan.forwardImplied[0], f0.segment(1, 4).mean(), 1e-15);
    }
    if (fan.maturity == 40) EXPECT_TRUE(std::isnan(fan.forwardImplied[0]));  // beyond the observed curve
  }
  // returns are reported for maturities longer than the holding period
  ASSERT_EQ(fc.returns.size(), 2u);
  for (const auto& r : fc.returns) {
    EXPECT_EQ(r.horizon, 8);
    EXPECT_GE(r.probPositive, 0.0);
    EXPECT_LE(r.probPositive, 1.0);
    EXPECT_NEAR(r.carryRollDown, carryAndRollDown(today, r.maturity, 8), 1e-15);
    EXPECT_GT(r.excessReturn.stdev, 0.0);
    EXPECT_NEAR(r.breakEvenYieldRise * (r.maturity - 8), r.carryRollDown, 1e-15);
  }
  EXPECT_GT(fc.returns[1].excessReturn.stdev, fc.returns[0].excessReturn.stdev);  // longer bonds are riskier
  // same seed, same answer; different seed, different answer
  const ScenarioForecast again = simulateScenarios(cg, today, Eigen::VectorXd(), opt);
  EXPECT_EQ(again.returns[1].excessReturn.mean, fc.returns[1].excessReturn.mean);
  opt.seed = 99;
  EXPECT_NE(simulateScenarios(cg, today, Eigen::VectorXd(), opt).returns[1].excessReturn.mean, fc.returns[1].excessReturn.mean);
  opt.horizon = 40;
  EXPECT_THROW(simulateScenarios(cg, today, Eigen::VectorXd(), opt), Error);
}

TEST(Forecast, PredictiveRegressionWithBootstrapInference) {
  const CgBootstrap& cg = paperBootstrap();
  const Eigen::VectorXd today = paperPanel().yields.row(208).transpose();
  const PredictiveForecast pf = predictiveForecast(cg, 20, 4, today, Eigen::VectorXd(), {}, 300, 5);
  ASSERT_EQ(pf.regressors.size(), 4u);
  EXPECT_EQ(pf.regressors[2], "slope");
  EXPECT_EQ(pf.observations, 205);
  for (std::size_t i = 0; i < pf.beta.size(); ++i) {
    EXPECT_GE(pf.pBootstrap[i], 0.0);
    EXPECT_LE(pf.pBootstrap[i], 1.0);
  }
  // A steeper curve predicts higher excess returns (Fama-Bliss / Campbell-Shiller) ...
  EXPECT_GT(pf.beta[2], 0.0);
  // ... but the bootstrap is more cautious than the textbook normal approximation.
  EXPECT_GE(pf.pBootstrap[2], pf.pAsymptotic[2]);
  EXPECT_LT(pf.fittedLo90, pf.fittedLo68);
  EXPECT_LT(pf.fittedLo68, pf.fitted);
  EXPECT_LT(pf.fitted, pf.fittedHi68);
  EXPECT_LT(pf.fittedHi68, pf.fittedHi90);
  EXPECT_GT(pf.fittedStdErr, 0.0);
  EXPECT_GT(pf.r2, 0.0);
  // maturity 0 = average across maturities
  const PredictiveForecast avg = predictiveForecast(cg, 0, 4, today, Eigen::VectorXd(), {}, 100, 5);
  EXPECT_EQ(avg.maturity, 0);
  EXPECT_THROW(predictiveForecast(cg, 3, 4, today, Eigen::VectorXd(), {}, 100, 5), Error);
}

TEST(BootstrapRegression, PValuesBehaveSensibly) {
  const CgBootstrap& cg = paperBootstrap();
  // Regress the 5-year yield on the 10-year yield: an enormous t-statistic that stays significant.
  const RegressionBuilder strong = [](const CgSample& s, RegressionData& d) {
    d.X = s.yields.col(39);
    d.y = s.yields.col(19);
    return true;
  };
  const Eigen::VectorXd zero = Eigen::VectorXd::Zero(2);
  const BootstrapRegression a = bootstrapRegression(cg, strong, zero, 200, CovarianceType::NeweyWest, 4, 1);
  EXPECT_EQ(a.replications, 200);
  EXPECT_LT(a.pBootstrap[1], 0.01);
  EXPECT_EQ(a.betaDraws.rows(), 200);
  // Testing the estimate against itself gives t = 0 and p = 1.
  const BootstrapRegression self = bootstrapRegression(cg, strong, a.sample.beta, 100, CovarianceType::NeweyWest, 4, 1);
  EXPECT_NEAR(self.pBootstrap[1], 1.0, 1e-12);
  EXPECT_NEAR(self.pAsymptotic[1], 1.0, 1e-12);
  EXPECT_NEAR(normalPValue(1.959964), 0.05, 1e-6);
  // a builder that never succeeds is an error, as is a wrong number of null values
  const RegressionBuilder never = [](const CgSample&, RegressionData&) { return false; };
  EXPECT_THROW(bootstrapRegression(cg, never, zero, 10, CovarianceType::White, 0, 1), Error);
  EXPECT_THROW(bootstrapRegression(cg, strong, Eigen::VectorXd::Zero(3), 10, CovarianceType::White, 0, 1), Error);
}
