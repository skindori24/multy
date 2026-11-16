#include "mcb/stats/forecast.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/stats/term_panel.hpp"

namespace mcb {

Distribution summarize(const std::vector<double>& v) {
  Distribution d;
  d.mean = mean(v);
  d.stdev = stdev(v);
  d.q05 = quantile(v, 0.05);
  d.q16 = quantile(v, 0.16);
  d.q50 = quantile(v, 0.50);
  d.q84 = quantile(v, 0.84);
  d.q95 = quantile(v, 0.95);
  return d;
}

double carryAndRollDown(const Eigen::VectorXd& y, int n, int h) {
  MCB_REQUIRE(n > h && n <= y.size() && h >= 1, "carryAndRollDown: need horizon < maturity <= N");
  // rx = p(n-h) - p(n) + p(h) with p(m) = -m y(m), on an unchanged curve
  return -static_cast<double>(n - h) * y[n - h - 1] + static_cast<double>(n) * y[n - 1] - static_cast<double>(h) * y[h - 1];
}

ScenarioForecast simulateScenarios(const CgBootstrap& cg, const Eigen::VectorXd& currentYields,
                                   const Eigen::VectorXd& currentExternal, const ForecastOptions& opt) {
  const int N = static_cast<int>(cg.N()), H = opt.horizon;
  MCB_REQUIRE(currentYields.size() == N, "simulateScenarios: current curve must have " << N << " maturities");
  MCB_REQUIRE(H >= 1 && H < N, "simulateScenarios: horizon must be between 1 and N-1");
  MCB_REQUIRE(opt.paths >= 100, "simulateScenarios: need at least 100 paths");
  std::vector<int> mats = opt.maturities;
  if (mats.empty()) mats = {N};
  for (int n : mats) MCB_REQUIRE(n >= 1 && n <= N, "simulateScenarios: maturity " << n << " out of range");

  // Today's forward curve: f(n) = n y(n) - (n-1) y(n-1)
  Eigen::MatrixXd y0(1, N);
  y0.row(0) = currentYields.transpose();
  const Eigen::VectorXd f0 = forwardsFromYields(y0).row(0).transpose();

  // storage[k][step-1][path]
  std::vector<std::vector<std::vector<double>>> yieldDraws(mats.size(),
                                                           std::vector<std::vector<double>>(static_cast<std::size_t>(H)));
  std::vector<std::vector<double>> rxDraws(mats.size());
  std::vector<double> shortSum(static_cast<std::size_t>(H) + 1, 0.0);

  std::mt19937_64 rng(opt.seed);
  for (int p = 0; p < opt.paths; ++p) {
    const CgSample path = cg.simulate(f0, currentExternal, H, rng);
    for (int s = 0; s <= H; ++s) shortSum[static_cast<std::size_t>(s)] += path.yields(s, 0);
    for (std::size_t k = 0; k < mats.size(); ++k) {
      const int n = mats[k];
      for (int s = 1; s <= H; ++s) yieldDraws[k][static_cast<std::size_t>(s - 1)].push_back(path.yields(s, n - 1));
      if (n > H) {
        const double rx = -static_cast<double>(n - H) * path.yields(H, n - H - 1) +
                          static_cast<double>(n) * currentYields[n - 1] - static_cast<double>(H) * currentYields[H - 1];
        rxDraws[k].push_back(rx);
      }
    }
  }

  ScenarioForecast out;
  out.horizon = H;
  out.paths = opt.paths;
  for (int s = 0; s <= H; ++s) {
    out.expectedShortRate.push_back(shortSum[static_cast<std::size_t>(s)] / opt.paths);
    out.forwardShortRate.push_back(f0[s]);  // forward for the period starting s periods ahead
  }
  for (std::size_t k = 0; k < mats.size(); ++k) {
    const int n = mats[k];
    YieldFan fan;
    fan.maturity = n;
    fan.current = currentYields[n - 1];
    for (int s = 1; s <= H; ++s) {
      // n-period yield s periods ahead implied by today's forwards: the average of
      // f(s+1..s+n), available while s + n stays within the curve.
      fan.forwardImplied.push_back(s + n <= N ? f0.segment(s, n).mean() : std::nan(""));
      fan.steps.push_back(summarize(yieldDraws[k][static_cast<std::size_t>(s - 1)]));
    }
    out.fans.push_back(std::move(fan));
    if (n > H) {
      ReturnForecast r;
      r.maturity = n;
      r.horizon = H;
      r.carryRollDown = carryAndRollDown(currentYields, n, H);
      r.excessReturn = summarize(rxDraws[k]);
      int positive = 0;
      for (double v : rxDraws[k]) positive += v > 0.0;
      r.probPositive = static_cast<double>(positive) / static_cast<double>(rxDraws[k].size());
      r.breakEvenYieldRise = r.carryRollDown / static_cast<double>(n - H);
      out.returns.push_back(r);
    }
  }
  return out;
}

Eigen::Vector3d curveFactors(const Eigen::VectorXd& y) {
  const Eigen::Index N = y.size();
  MCB_REQUIRE(N >= 3, "curveFactors: need at least three maturities");
  const Eigen::Index mid = (N + 1) / 2;  // e.g. the 5-year point of a 10-year curve
  // Per-period yields are comparable across maturities, so plain differences are meaningful.
  return Eigen::Vector3d(y[N - 1], y[N - 1] - y[0], 2.0 * y[mid - 1] - y[0] - y[N - 1]);
}

Eigen::MatrixXd curveFactorMatrix(const Eigen::MatrixXd& Y) {
  Eigen::MatrixXd F(Y.rows(), 3);
  for (Eigen::Index t = 0; t < Y.rows(); ++t) F.row(t) = curveFactors(Y.row(t).transpose()).transpose();
  return F;
}

PredictiveForecast predictiveForecast(const CgBootstrap& cg, int maturity, int horizon,
                                      const Eigen::VectorXd& currentYields, const Eigen::VectorXd& currentExternal,
                                      const std::vector<std::string>& externalNames, int replications,
                                      std::uint64_t seed) {
  const int N = static_cast<int>(cg.N());
  const Eigen::Index K = cg.K();
  MCB_REQUIRE(horizon >= 1 && horizon < N, "predictiveForecast: bad horizon");
  MCB_REQUIRE(maturity == 0 || (maturity > horizon && maturity <= N), "predictiveForecast: bad maturity");
  MCB_REQUIRE(currentYields.size() == N && currentExternal.size() == K, "predictiveForecast: bad current state");
  MCB_REQUIRE(static_cast<Eigen::Index>(externalNames.size()) == K, "predictiveForecast: one name per external series");

  const RegressionBuilder build = [=](const CgSample& s, RegressionData& d) {
    const Eigen::Index T = s.yields.rows(), L = T - horizon;
    if (L < 20) return false;
    const Eigen::MatrixXd rx = excessReturns(s.yields, horizon);
    if (maturity > 0) {
      d.y = rx.col(maturity - 1);
    } else {
      d.y = rx.rightCols(N - horizon).rowwise().mean();  // average across maturities above the horizon
    }
    d.X.resize(L, 3 + K);
    d.X.leftCols(3) = curveFactorMatrix(s.yields).topRows(L);
    if (K > 0) d.X.rightCols(K) = s.external.topRows(L);
    return true;
  };

  Eigen::VectorXd nulls = Eigen::VectorXd::Zero(4 + K);
  const BootstrapRegression br =
      bootstrapRegression(cg, build, nulls, replications, CovarianceType::NeweyWest, horizon, seed);

  PredictiveForecast out;
  out.maturity = maturity;
  out.horizon = horizon;
  out.regressors = {"intercept", "level", "slope", "curvature"};
  for (const auto& name : externalNames) out.regressors.push_back(name);
  out.r2 = br.sample.r2;
  out.observations = br.sample.n;
  for (Eigen::Index i = 0; i < br.sample.beta.size(); ++i) {
    out.beta.push_back(br.sample.beta[i]);
    out.tStat.push_back(br.tStats[i]);
    out.pBootstrap.push_back(br.pBootstrap[i]);
    out.pAsymptotic.push_back(br.pAsymptotic[i]);
  }

  // Today's regressor vector (1, level, slope, curvature, external...).
  Eigen::VectorXd x0(4 + K);
  x0[0] = 1.0;
  x0.segment(1, 3) = curveFactors(currentYields);
  if (K > 0) x0.tail(K) = currentExternal;
  out.fitted = x0.dot(br.sample.beta);

  // Sampling uncertainty of the fitted value: spread of x0'beta* around x0'beta-hat
  // across bootstrap histories (basic bootstrap interval).
  std::vector<double> deviations;
  deviations.reserve(static_cast<std::size_t>(br.replications));
  for (Eigen::Index b = 0; b < br.betaDraws.rows(); ++b)
    deviations.push_back(x0.dot(br.betaDraws.row(b).transpose()) - out.fitted);
  out.fittedStdErr = stdev(deviations);
  out.fittedLo68 = out.fitted - quantile(deviations, 0.84);
  out.fittedHi68 = out.fitted - quantile(deviations, 0.16);
  out.fittedLo90 = out.fitted - quantile(deviations, 0.95);
  out.fittedHi90 = out.fitted - quantile(deviations, 0.05);

  RegressionData d;
  build(cg.observed(), d);
  out.unconditionalMean = d.y.mean();
  return out;
}

}  // namespace mcb
