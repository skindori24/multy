// Statistical building blocks: OLS with robust errors, VAR(1) with bias
// correction, yield-curve identities and the GSW (Svensson) data.
#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "mcb/core/error.hpp"
#include "mcb/data/gsw.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/stats/eh_tests.hpp"
#include "mcb/stats/linear_model.hpp"
#include "mcb/stats/term_panel.hpp"
#include "mcb/stats/var_model.hpp"
#include "test_util.hpp"

using namespace mcb;

TEST(Ols, RecoversExactLinearRelation) {
  Eigen::MatrixXd X(6, 2);
  X << 1, 2, 2, 1, 3, 5, 4, 3, 5, 8, 6, 4;
  Eigen::VectorXd y(6);
  for (int i = 0; i < 6; ++i) y[i] = 1.5 + 2.0 * X(i, 0) - 0.5 * X(i, 1);
  const OlsResult r = ols(X, y);
  EXPECT_NEAR(r.beta[0], 1.5, 1e-10);
  EXPECT_NEAR(r.beta[1], 2.0, 1e-10);
  EXPECT_NEAR(r.beta[2], -0.5, 1e-10);
  EXPECT_NEAR(r.r2, 1.0, 1e-12);
  EXPECT_EQ(r.n, 6);
  EXPECT_EQ(r.k, 3);
}

TEST(Ols, TextbookStandardErrors) {
  // y = (1, 3, 2, 5, 4) on x = (1..5): slope 0.8, intercept 0.6, residuals (-0.4, 0.8, -1, 1.2, -0.6),
  // SSR = 3.6, s^2 = 3.6/3 = 1.2, se(slope) = sqrt(1.2/Sxx) with Sxx = 10
  Eigen::MatrixXd X(5, 1);
  X << 1, 2, 3, 4, 5;
  Eigen::VectorXd y(5);
  y << 1, 3, 2, 5, 4;
  const OlsResult r = ols(X, y);
  EXPECT_NEAR(r.beta[1], 0.8, 1e-12);
  EXPECT_NEAR(r.beta[0], 0.6, 1e-12);
  EXPECT_NEAR(r.stdErrors[1], std::sqrt(1.2 / 10.0), 1e-12);
  EXPECT_NEAR(r.r2, 0.64, 1e-12);
  EXPECT_NEAR(r.tStats[1], 0.8 / std::sqrt(0.12), 1e-10);
  // White (HC0): sum (x - xbar)^2 e^2 / Sxx^2 with residuals (-0.4, 0.8, -1, 1.2, -0.6)
  const OlsResult w = ols(X, y, true, CovarianceType::White);
  const double meat = 4 * 0.16 + 1 * 0.64 + 0 + 1 * 1.44 + 4 * 0.36;
  EXPECT_NEAR(w.stdErrors[1], std::sqrt(meat) / 10.0, 1e-12);
  // Newey-West with zero lags is White.
  EXPECT_NEAR(ols(X, y, true, CovarianceType::NeweyWest, 0).stdErrors[1], w.stdErrors[1], 1e-14);
  double a, b;
  simpleRegression(X.col(0), y, a, b);
  EXPECT_NEAR(a, 0.6, 1e-12);
  EXPECT_NEAR(b, 0.8, 1e-12);
}

TEST(Ols, NeweyWestWidensErrorsUnderAutocorrelation) {
  // Persistent regressor and persistent errors: the classical formula is too optimistic.
  std::mt19937_64 rng(7);
  std::normal_distribution<double> z;
  const int n = 400;
  Eigen::MatrixXd X(n, 1);
  Eigen::VectorXd y(n);
  double x = 0, e = 0;
  for (int t = 0; t < n; ++t) {
    x = 0.9 * x + z(rng);
    e = 0.9 * e + z(rng);
    X(t, 0) = x;
    y[t] = 1.0 + 0.5 * x + e;
  }
  const double plain = ols(X, y).stdErrors[1];
  const double nw = ols(X, y, true, CovarianceType::NeweyWest, 12).stdErrors[1];
  EXPECT_GT(nw, 1.5 * plain);
  // The one-regressor fast path used in the bootstrap equals the general estimator.
  double slope, se;
  slopeWithNeweyWest(X.col(0), y, 12, slope, se);
  const OlsResult full = ols(X, y, true, CovarianceType::NeweyWest, 12);
  EXPECT_NEAR(slope, full.beta[1], 1e-12);
  EXPECT_NEAR(se, full.stdErrors[1], 1e-12);
}

TEST(Ols, RejectsBadInput) {
  Eigen::MatrixXd X(4, 2);
  X << 1, 2, 2, 4, 3, 6, 4, 8;  // second column = 2 x first
  Eigen::VectorXd y(4);
  y << 1, 2, 3, 4;
  EXPECT_THROW(ols(X, y), Error);                       // collinear
  EXPECT_THROW(ols(Eigen::MatrixXd(3, 1), y), Error);   // size mismatch
  EXPECT_THROW(ols(Eigen::MatrixXd::Random(3, 3), Eigen::VectorXd::Random(3)), Error);  // too few observations
}

TEST(Stats, QuantilesAndMoments) {
  const std::vector<double> v = {5, 1, 4, 2, 3};
  EXPECT_DOUBLE_EQ(quantile(v, 0.5), 3.0);
  EXPECT_DOUBLE_EQ(quantile(v, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(quantile(v, 1.0), 5.0);
  EXPECT_DOUBLE_EQ(quantile(v, 0.25), 2.0);
  EXPECT_DOUBLE_EQ(quantile(v, 0.1), 1.4);
  EXPECT_DOUBLE_EQ(mean(v), 3.0);
  EXPECT_NEAR(stdev(v), std::sqrt(2.5), 1e-14);
  EXPECT_THROW(quantile({}, 0.5), Error);
  Eigen::VectorXd alt(6);
  alt << 1, -1, 1, -1, 1, -1;
  EXPECT_LT(autocorrelation(alt), -0.8);
}

TEST(Var1, RecoversParametersFromALongSample) {
  std::mt19937_64 rng(11);
  std::normal_distribution<double> z;
  Eigen::Matrix2d Phi;
  Phi << 0.8, 0.1, -0.05, 0.6;
  const Eigen::Vector2d mu(0.2, -0.1);
  const int T = 20000;
  Eigen::MatrixXd Y(T, 2);
  Y.row(0).setZero();
  for (int t = 1; t < T; ++t) {
    const Eigen::Vector2d e(0.1 * z(rng), 0.2 * z(rng));
    Y.row(t) = (mu + Phi * Y.row(t - 1).transpose() + e).transpose();
  }
  const Var1 m = fitVar1(Y);
  EXPECT_LT((m.Phi - Phi).cwiseAbs().maxCoeff(), 0.02);
  EXPECT_LT((m.mu - mu).cwiseAbs().maxCoeff(), 0.02);
  EXPECT_EQ(m.residuals.rows(), T - 1);
  EXPECT_NEAR(m.residuals.col(0).mean(), 0.0, 1e-10);  // OLS residuals have zero mean
  EXPECT_NEAR(m.maxEigenvalue, spectralRadius(m.Phi), 1e-14);
  Eigen::Matrix2d rot;
  rot << 0, -0.5, 0.5, 0;  // eigenvalues +-0.5i
  EXPECT_NEAR(spectralRadius(rot), 0.5, 1e-12);
}

TEST(Var1, KilianCorrectionRemovesTheDownwardBias) {
  // AR(1) with phi = 0.95 and only 80 observations: OLS is biased towards zero.
  std::mt19937_64 rng(3);
  std::normal_distribution<double> z;
  double sumOls = 0, sumCorrected = 0;
  const int trials = 60;
  for (int k = 0; k < trials; ++k) {
    Eigen::MatrixXd Y(80, 1);
    Y(0, 0) = 0;
    for (int t = 1; t < 80; ++t) Y(t, 0) = 0.95 * Y(t - 1, 0) + z(rng);
    sumOls += fitVar1(Y).Phi(0, 0);
    const Var1 c = fitVar1BiasCorrected(Y, 300, rng);
    sumCorrected += c.Phi(0, 0);
    EXPECT_LT(c.maxEigenvalue, 1.0);  // never pushed into non-stationarity
  }
  const double ols = sumOls / trials, corrected = sumCorrected / trials;
  EXPECT_LT(ols, 0.93);                                           // visibly biased
  EXPECT_LT(std::fabs(corrected - 0.95), std::fabs(ols - 0.95));  // the correction helps
  EXPECT_GT(corrected, ols);
}

TEST(Var1, LeavesExplosiveEstimatesAlone) {
  Eigen::MatrixXd Y(40, 1);
  for (int t = 0; t < 40; ++t) Y(t, 0) = std::pow(1.05, t);
  std::mt19937_64 rng(1);
  const Var1 m = fitVar1BiasCorrected(Y, 100, rng);
  EXPECT_FALSE(m.biasCorrected);
  EXPECT_GT(m.maxEigenvalue, 1.0);
}

TEST(TermIdentities, ExampleFromTheProjectNotes) {
  // One-year prices 0.96 and 0.92: yields 4.08% and 4.17%, forward 4.26%.
  Eigen::MatrixXd y(1, 2);
  y << -std::log(0.96), -std::log(0.92) / 2.0;
  const Eigen::MatrixXd f = forwardsFromYields(y);
  EXPECT_NEAR(y(0, 0), 0.0408, 5e-5);
  EXPECT_NEAR(y(0, 1), 0.0417, 5e-5);
  EXPECT_NEAR(f(0, 1), std::log(0.96 / 0.92), 1e-14);
  EXPECT_NEAR(f(0, 1), 0.0426, 5e-5);
  EXPECT_NEAR(0.5 * (f(0, 0) + f(0, 1)), y(0, 1), 1e-15);  // the yield is the average of the forwards
}

TEST(TermIdentities, HoldExactlyOnRandomCurves) {
  std::mt19937_64 rng(5);
  std::normal_distribution<double> z;
  const int T = 30, N = 12;
  Eigen::MatrixXd y(T, N);
  for (int t = 0; t < T; ++t)
    for (int n = 0; n < N; ++n) y(t, n) = 0.01 + 0.002 * std::log(1.0 + n) + 0.001 * z(rng);
  const Eigen::MatrixXd f = forwardsFromYields(y);
  EXPECT_LT((yieldsFromForwards(f) - y).cwiseAbs().maxCoeff(), 1e-15);  // equation (2)

  // equation (6): the one-period excess return is the sum of difference returns
  const Eigen::MatrixXd dr = differenceReturns(f);
  const Eigen::MatrixXd rx = excessReturns(y, 1);
  ASSERT_EQ(dr.rows(), T - 1);
  ASSERT_EQ(dr.cols(), N - 1);
  for (int t = 0; t < T - 1; ++t)
    for (int n = 2; n <= N; ++n) EXPECT_NEAR(rx(t, n - 1), dr.row(t).head(n - 1).sum(), 1e-14);

  // definition: rx(n,h) = p(n-h)_{t+h} - p(n)_t + p(h)_t with p(m) = -m y(m)
  const Eigen::MatrixXd rx4 = excessReturns(y, 4);
  EXPECT_NEAR(rx4(3, 9), -6.0 * y(7, 5) + 10.0 * y(3, 9) - 4.0 * y(3, 3), 1e-15);
  EXPECT_EQ(rx4(3, 3), 0.0);  // undefined for n <= h

  // equation (9): first curve + longest forward + difference returns rebuild everything
  const Eigen::MatrixXd rebuilt = rebuildForwards(f.row(0).transpose(), f.col(N - 1), dr);
  EXPECT_LT((rebuilt - f).cwiseAbs().maxCoeff(), 1e-14);
  EXPECT_THROW(rebuildForwards(f.row(0).transpose(), f.col(N - 1), dr.topRows(3)), Error);
}

TEST(Gsw, SvenssonFormulaReproducesPublishedYields) {
  // Rows of the Fed's feds200628.csv (parameters and the yields it reports, in percent).
  struct Row {
    SvenssonParams p;
    double y1, y5, y10, f5, f10;
  };
  const std::vector<Row> rows = {
      {{Date(2024, 3, 28), 2.30617185264272, 3.21520211877622, 0.000354513650233499, 6.49640689656467,
        1.79391794793064, 17.0585023070271},
       4.9541, 4.1738, 4.1817, 3.9247, 4.4374},
      {{Date(1985, 6, 28), 25.0, -18.2765030140726, -629.208388570078, 609.305188154374, 7.15118510964767,
        6.92707942063751},
       7.8124, 9.8435, 10.3429, 10.9617, 10.8099},
      // before 1980 the Fed fits the Nelson-Siegel special case (beta3 = 0, tau2 = -999.99)
      {{Date(1975, 12, 31), 7.93154068197816, -2.77666814956071, -0.684352016113738, 0.0, 0.821602305394144, -999.99},
       6.1325, 7.3657, 7.6472, 7.9157, 7.9315},
  };
  for (const auto& r : rows) {
    EXPECT_NEAR(svenssonZeroYield(r.p, 1.0) * 100, r.y1, 6e-5) << r.p.date.iso();
    EXPECT_NEAR(svenssonZeroYield(r.p, 5.0) * 100, r.y5, 6e-5) << r.p.date.iso();
    EXPECT_NEAR(svenssonZeroYield(r.p, 10.0) * 100, r.y10, 6e-5) << r.p.date.iso();
    EXPECT_NEAR(svenssonForward(r.p, 5.0) * 100, r.f5, 6e-5) << r.p.date.iso();
    EXPECT_NEAR(svenssonForward(r.p, 10.0) * 100, r.f10, 6e-5) << r.p.date.iso();
    // the curve object is consistent with the formula
    const SvenssonCurve c(r.p);
    EXPECT_NEAR(c.zeroRateT(5.0), svenssonZeroYield(r.p, 5.0), 1e-15);
    EXPECT_NEAR(-std::log(c.discountT(10.0)) / 10.0 * 100, r.y10, 6e-5);
    const double h = 1e-5;  // forward = -d ln DF / dt
    EXPECT_NEAR((c.logDiscountT(5.0 - h) - c.logDiscountT(5.0 + h)) / (2 * h), svenssonForward(r.p, 5.0), 1e-9);
  }
}

TEST(Gsw, ParsesTheFedFileLayout) {
  const std::string text =
      "Note: This is not an official Federal Reserve statistical release.\n\n"
      "Series,Compounding Convention,Mnemonic(s)\nZero-coupon yield,Continuously Compounded,SVENYXX\n\n"
      "Date,BETA0,BETA1,BETA2,BETA3,SVENY01,TAU1,TAU2\n"
      "2024-03-27,2.3,3.2,0.0004,6.5,4.96,1.79,17.06\n"
      "2024-03-28,2.30617185264272,3.21520211877622,0.000354513650233499,6.49640689656467,4.9541,1.79391794793064,17.0585023070271\n"
      "2024-03-29,NA,NA,NA,NA,NA,NA,NA\n"
      "2024-04-01,2.4,3.1,0.0003,6.4,4.97,1.80,17.0\n";
  const auto rows = parseGswParameters(text);
  ASSERT_EQ(rows.size(), 3u);  // the holiday row is skipped
  EXPECT_EQ(rows[1].date, Date(2024, 3, 28));
  EXPECT_NEAR(rows[1].tau2, 17.0585023070271, 1e-12);
  // quarter ends: the last observation of each quarter
  const auto q = endOfPeriodObservations(rows, PanelFrequency::Quarterly, Date(2024, 1, 1), Date(2024, 12, 31));
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0].date, Date(2024, 3, 28));
  EXPECT_EQ(q[1].date, Date(2024, 4, 1));
  // round trip through the compact format
  const auto back = parseGswParameters(writeSvenssonCsv(rows));
  ASSERT_EQ(back.size(), 3u);
  EXPECT_NEAR(back[1].beta3, rows[1].beta3, 1e-12);
  EXPECT_THROW(parseGswParameters("no header here\n1,2,3\n"), Error);
}

TEST(Gsw, ReferenceExtractBuildsThePapersSample) {
  const auto monthly = parseGswParameters(readTextFile(mcbtest::sourcePath("data/reference/gsw_monthend.csv")));
  ASSERT_GT(monthly.size(), 600u);
  const auto obs = endOfPeriodObservations(monthly, PanelFrequency::Quarterly, Date(1972, 1, 1), Date(2024, 3, 31));
  const YieldPanel panel = buildYieldPanel(obs, PanelFrequency::Quarterly, 40);
  EXPECT_EQ(panel.T(), 209);  // 1972Q1 .. 2024Q1
  EXPECT_EQ(panel.N(), 40);
  EXPECT_EQ(panel.dates.front(), Date(1972, 3, 30));
  EXPECT_EQ(panel.dates.back(), Date(2024, 3, 28));
  EXPECT_NEAR(panel.annualPercent(208, 40), 4.1817, 1e-4);  // 10-year yield on 2024-03-28
  EXPECT_NEAR(panel.annualPercent(208, 4), 4.9541, 1e-4);   // 1-year
  EXPECT_NEAR(panel.yields(208, 39), 0.041817 * 0.25, 1e-6);  // stored per quarter
  // Figure 1 of the paper: three principal components "explain" nearly all yield variation.
  const PcaResult pc = pca(panel.yields, true);
  EXPECT_GT(pc.explainedShare[0], 0.97);
  EXPECT_GT(pc.explainedShare.head(3).sum(), 0.999);
  const YieldPanel sub = panel.slice(100, 110);
  EXPECT_EQ(sub.T(), 10);
  EXPECT_EQ(sub.dates.front(), panel.dates[100]);
}
