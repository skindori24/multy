#include "mcb/stats/eh_tests.hpp"

#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

std::string EhTestSpec::label() const {
  switch (kind) {
    case EhTestKind::CG: return "CG (m=" + std::to_string(m) + ")";
    case EhTestKind::FB: return "FB";
    case EhTestKind::CS1: return "CS1";
    case EhTestKind::CS2: return "CS2 (m=" + std::to_string(m) + ")";
  }
  return "?";
}

int ehOverlap(const EhTestSpec& spec, int n) {
  switch (spec.kind) {
    case EhTestKind::CG:
    case EhTestKind::CS2: return spec.m;
    case EhTestKind::FB:
    case EhTestKind::CS1: return n - 1;
  }
  return 0;
}

bool ehRegression(const EhTestSpec& spec, int n, const Eigen::MatrixXd& Y, const Eigen::MatrixXd& F,
                  Eigen::VectorXd& x, Eigen::VectorXd& y) {
  const Eigen::Index T = Y.rows(), N = Y.cols();
  if (n < 2 || n > N) return false;
  auto col = [](const Eigen::MatrixXd& M, int maturity) { return M.col(maturity - 1); };
  switch (spec.kind) {
    case EhTestKind::CG: {
      const int m = spec.m;
      if (m < 1 || n - m < 1 || T - m < 20) return false;
      const Eigen::Index L = T - m;
      y = col(F, n - m).tail(L) - col(Y, 1).head(L);
      x = col(F, n).head(L) - col(Y, 1).head(L);
      return true;
    }
    case EhTestKind::FB: {
      const int m = n - 1;
      if (T - m < 20) return false;
      const Eigen::Index L = T - m;
      y = col(Y, 1).tail(L) - col(Y, 1).head(L);
      x = col(F, n).head(L) - col(Y, 1).head(L);
      return true;
    }
    case EhTestKind::CS1: {
      const Eigen::Index L = T - (n - 1);
      if (L < 20) return false;
      // average short rate over the next n periods, minus today's
      Eigen::VectorXd avg = Eigen::VectorXd::Zero(L);
      for (int s = 0; s < n; ++s) avg += col(Y, 1).segment(s, L);
      y = avg / static_cast<double>(n) - col(Y, 1).head(L);
      x = col(Y, n).head(L) - col(Y, 1).head(L);
      return true;
    }
    case EhTestKind::CS2: {
      const int m = spec.m;
      if (m < 1 || n - m < 1 || T - m < 20) return false;
      const Eigen::Index L = T - m;
      y = col(Y, n - m).tail(L) - col(Y, n).head(L);
      x = (static_cast<double>(m) / static_cast<double>(n - m)) * (col(Y, n).head(L) - col(Y, m).head(L));
      return true;
    }
  }
  return false;
}

void slopeWithNeweyWest(const Eigen::VectorXd& x, const Eigen::VectorXd& y, int lags, double& slope, double& se) {
  const Eigen::Index n = x.size();
  const double mx = x.mean(), my = y.mean();
  double sxx = 0.0, sxy = 0.0;
  for (Eigen::Index t = 0; t < n; ++t) {
    sxx += (x[t] - mx) * (x[t] - mx);
    sxy += (x[t] - mx) * (y[t] - my);
  }
  slope = sxx > 0.0 ? sxy / sxx : 0.0;
  // Scores of the slope after partialling out the intercept: u_t = (x_t - mean) * residual_t.
  std::vector<double> u(static_cast<std::size_t>(n));
  for (Eigen::Index t = 0; t < n; ++t)
    u[static_cast<std::size_t>(t)] = (x[t] - mx) * ((y[t] - my) - slope * (x[t] - mx));
  double lrv = 0.0;
  for (double v : u) lrv += v * v;
  const int L = std::min<int>(lags, static_cast<int>(n) - 1);
  for (int j = 1; j <= L; ++j) {
    double g = 0.0;
    for (std::size_t t = static_cast<std::size_t>(j); t < u.size(); ++t) g += u[t] * u[t - static_cast<std::size_t>(j)];
    lrv += 2.0 * (1.0 - static_cast<double>(j) / (L + 1.0)) * g;
  }
  se = sxx > 0.0 ? std::sqrt(std::max(lrv, 0.0)) / sxx : 0.0;
}

std::vector<EhTestResult> runEhTests(const CgBootstrap& cg, const std::vector<EhTestSpec>& specs, int replications,
                                     std::uint64_t seed, int maturityStep) {
  MCB_REQUIRE(replications >= 10, "runEhTests: need at least 10 replications");
  MCB_REQUIRE(maturityStep >= 1, "runEhTests: maturityStep must be positive");
  const int N = static_cast<int>(cg.N());

  // Observed estimates.
  struct Cell {
    int spec, n, lags;
    double slope, se;
    std::vector<double> draws;
    int exceedOne = 0, exceedZero = 0, used = 0;
  };
  std::vector<Cell> cells;
  Eigen::VectorXd x, y;
  for (std::size_t s = 0; s < specs.size(); ++s)
    for (int n = 2; n <= N; n += maturityStep) {
      if (!ehRegression(specs[s], n, cg.observed().yields, cg.observed().forwards, x, y)) continue;
      Cell c;
      c.spec = static_cast<int>(s), c.n = n, c.lags = ehOverlap(specs[s], n);
      slopeWithNeweyWest(x, y, c.lags, c.slope, c.se);
      c.draws.reserve(static_cast<std::size_t>(replications));
      cells.push_back(std::move(c));
    }

  // Bootstrap: every test and maturity is evaluated on the same resampled
  // curve history, which is what makes the tests comparable.
  std::mt19937_64 rng(seed);
  for (int b = 0; b < replications; ++b) {
    const CgSample sample = cg.draw(rng);
    for (auto& c : cells) {
      if (!ehRegression(specs[static_cast<std::size_t>(c.spec)], c.n, sample.yields, sample.forwards, x, y)) continue;
      double slope, se;
      slopeWithNeweyWest(x, y, c.lags, slope, se);
      c.draws.push_back(slope);
      if (se > 0.0 && c.se > 0.0) {
        const double tStar = std::fabs((slope - c.slope) / se);  // bootstrap truth = sample estimate
        if (tStar > std::fabs((c.slope - 1.0) / c.se)) ++c.exceedOne;
        if (tStar > std::fabs(c.slope / c.se)) ++c.exceedZero;
        ++c.used;
      }
    }
  }

  std::vector<EhTestResult> out(specs.size());
  for (std::size_t s = 0; s < specs.size(); ++s) out[s].spec = specs[s];
  for (const auto& c : cells) {
    EhMaturityResult r;
    r.maturity = c.n;
    ehRegression(specs[static_cast<std::size_t>(c.spec)], c.n, cg.observed().yields, cg.observed().forwards, x, y);
    r.observations = static_cast<int>(x.size());
    r.slope = c.slope;
    r.stdError = c.se;
    if (!c.draws.empty()) {
      r.bootMean = mean(c.draws);
      r.q05 = quantile(c.draws, 0.05);
      r.q16 = quantile(c.draws, 0.16);
      r.q50 = quantile(c.draws, 0.50);
      r.q84 = quantile(c.draws, 0.84);
      r.q95 = quantile(c.draws, 0.95);
    }
    if (c.used > 0) {
      r.pSlopeIsOne = static_cast<double>(c.exceedOne) / c.used;
      r.pSlopeIsZero = static_cast<double>(c.exceedZero) / c.used;
    }
    out[static_cast<std::size_t>(c.spec)].byMaturity.push_back(r);
  }
  return out;
}

}  // namespace mcb
