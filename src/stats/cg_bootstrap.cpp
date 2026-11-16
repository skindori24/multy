#include "mcb/stats/cg_bootstrap.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/stats/term_panel.hpp"

namespace mcb {

int CgBootstrap::ruleOfThumbBlockLength(Eigen::Index T, Eigen::Index N) {
  const double m = std::pow(static_cast<double>(T) * static_cast<double>(N), 0.4);
  return std::max(1, static_cast<int>(std::lround(m)));
}

CgBootstrap::CgBootstrap(const Eigen::MatrixXd& yields, const Eigen::MatrixXd& external, const CgOptions& options)
    : options_(options) {
  const Eigen::Index T = yields.rows(), N = yields.cols();
  MCB_REQUIRE(T >= 12 && N >= 2, "CG bootstrap needs at least 12 dates and 2 maturities");
  MCB_REQUIRE(external.size() == 0 || external.rows() == T, "CG bootstrap: external series must have one row per date");
  observed_.yields = yields;
  observed_.forwards = forwardsFromYields(yields);
  observed_.external = external.size() == 0 ? Eigen::MatrixXd(T, 0) : external;
  dr_ = mcb::differenceReturns(observed_.forwards);

  const Eigen::Index K = observed_.external.cols();
  x_.resize(T, 1 + K);
  x_.col(0) = observed_.forwards.col(N - 1);
  if (K > 0) x_.rightCols(K) = observed_.external;

  if (options_.prewhiten) {
    std::mt19937_64 rng(options_.seed);
    model_ = options_.biasCorrect ? fitVar1BiasCorrected(x_, options_.biasCorrectionReps, rng) : fitVar1(x_);
    shocks_ = model_.residuals;
    shocks_.rowwise() -= shocks_.colwise().mean();
  } else {
    shocks_ = x_.bottomRows(T - 1) - x_.topRows(T - 1);  // level changes
  }

  blockLength_ = options_.blockLength > 0 ? options_.blockLength : ruleOfThumbBlockLength(T, N);
  blockLength_ = std::min<int>(blockLength_, static_cast<int>(T - 1));
}

std::vector<Eigen::Index> CgBootstrap::resampleRows(Eigen::Index count, std::mt19937_64& rng) const {
  // Moving-block bootstrap: blocks of M consecutive rows, start points uniform.
  const Eigen::Index rows = T() - 1, M = blockLength_;
  std::uniform_int_distribution<Eigen::Index> start(0, rows - M);
  std::vector<Eigen::Index> idx;
  idx.reserve(static_cast<std::size_t>(count));
  while (static_cast<Eigen::Index>(idx.size()) < count) {
    const Eigen::Index s = start(rng);
    for (Eigen::Index j = 0; j < M && static_cast<Eigen::Index>(idx.size()) < count; ++j) idx.push_back(s + j);
  }
  return idx;
}

CgSample CgBootstrap::draw(std::mt19937_64& rng) const {
  const Eigen::Index T = this->T(), N = this->N(), K = this->K();
  const std::vector<Eigen::Index> rows = resampleRows(T - 1, rng);

  Eigen::MatrixXd dr(T - 1, N - 1), shock(T - 1, 1 + K);
  for (Eigen::Index t = 0; t < T - 1; ++t) {
    dr.row(t) = dr_.row(rows[static_cast<std::size_t>(t)]);
    shock.row(t) = shocks_.row(rows[static_cast<std::size_t>(t)]);
  }

  // Path of the persistent block (longest forward + external series).
  Eigen::MatrixXd x(T, 1 + K);
  x.row(0) = x_.row(0);
  if (options_.prewhiten) {
    shock.rowwise() -= shock.colwise().mean();  // re-centre the resampled innovations
    for (Eigen::Index t = 0; t < T - 1; ++t)
      x.row(t + 1) = (model_.mu + model_.Phi * x.row(t).transpose() + shock.row(t).transpose()).transpose();
  } else {
    // Without pre-whitening the levels themselves are resampled (rows of z_t).
    for (Eigen::Index t = 0; t < T - 1; ++t) x.row(t + 1) = x_.row(rows[static_cast<std::size_t>(t)] + 1);
  }

  CgSample s;
  s.forwards = rebuildForwards(observed_.forwards.row(0).transpose(), x.col(0), dr);
  s.yields = yieldsFromForwards(s.forwards);
  s.external = x.rightCols(K);
  return s;
}

CgSample CgBootstrap::simulate(const Eigen::VectorXd& currentForwards, const Eigen::VectorXd& currentExternal,
                               int horizon, std::mt19937_64& rng) const {
  const Eigen::Index N = this->N(), K = this->K();
  MCB_REQUIRE(currentForwards.size() == N, "simulate: forward curve must have " << N << " maturities");
  MCB_REQUIRE(currentExternal.size() == K, "simulate: expected " << K << " external values");
  MCB_REQUIRE(horizon >= 1, "simulate: horizon must be positive");
  const std::vector<Eigen::Index> rows = resampleRows(horizon, rng);

  Eigen::MatrixXd dr(horizon, N - 1);
  Eigen::MatrixXd x(horizon + 1, 1 + K);
  x(0, 0) = currentForwards[N - 1];
  if (K > 0) x.row(0).tail(K) = currentExternal.transpose();
  for (int t = 0; t < horizon; ++t) {
    const Eigen::Index r = rows[static_cast<std::size_t>(t)];
    dr.row(t) = dr_.row(r);
    if (options_.prewhiten)
      x.row(t + 1) = (model_.mu + model_.Phi * x.row(t).transpose() + shocks_.row(r).transpose()).transpose();
    else
      x.row(t + 1) = x.row(t) + shocks_.row(r);  // random walk with resampled changes
  }
  CgSample s;
  s.forwards = rebuildForwards(currentForwards, x.col(0), dr);
  s.yields = yieldsFromForwards(s.forwards);
  s.external = x.rightCols(K);
  return s;
}

double normalPValue(double t) { return std::erfc(std::fabs(t) / std::sqrt(2.0)); }

BootstrapRegression bootstrapRegression(const CgBootstrap& cg, const RegressionBuilder& build,
                                        const Eigen::VectorXd& nullValues, int replications,
                                        CovarianceType covariance, int nwLags, std::uint64_t seed) {
  BootstrapRegression out;
  RegressionData data;
  MCB_REQUIRE(build(cg.observed(), data), "bootstrapRegression: cannot build the regression on the observed sample");
  out.sample = ols(data.X, data.y, true, covariance, nwLags);
  const Eigen::Index k = out.sample.beta.size();
  MCB_REQUIRE(nullValues.size() == k, "bootstrapRegression: need " << k << " null values (intercept first)");
  out.tStats = (out.sample.beta - nullValues).cwiseQuotient(out.sample.stdErrors);
  out.pAsymptotic.resize(k);
  for (Eigen::Index i = 0; i < k; ++i) out.pAsymptotic[i] = normalPValue(out.tStats[i]);

  std::mt19937_64 rng(seed);
  out.betaDraws.resize(replications, k);
  Eigen::VectorXd exceed = Eigen::VectorXd::Zero(k);
  int used = 0;
  for (int b = 0; b < replications; ++b) {
    const CgSample s = cg.draw(rng);
    RegressionData d;
    if (!build(s, d)) continue;
    OlsResult r;
    try {
      r = ols(d.X, d.y, true, covariance, nwLags);
    } catch (const Error&) {
      continue;  // a degenerate draw (collinear regressors)
    }
    out.betaDraws.row(used) = r.beta.transpose();
    // In the bootstrap world the "true" coefficient is the sample estimate.
    const Eigen::VectorXd tStar = (r.beta - out.sample.beta).cwiseQuotient(r.stdErrors);
    for (Eigen::Index i = 0; i < k; ++i)
      if (std::fabs(tStar[i]) > std::fabs(out.tStats[i])) exceed[i] += 1.0;
    ++used;
  }
  MCB_REQUIRE(used > 0, "bootstrapRegression: every bootstrap draw failed");
  out.betaDraws.conservativeResize(used, k);
  out.replications = used;
  out.pBootstrap = exceed / static_cast<double>(used);
  return out;
}

}  // namespace mcb
