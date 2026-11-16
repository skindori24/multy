#include "mcb/stats/linear_model.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

OlsResult ols(const Eigen::MatrixXd& Xin, const Eigen::VectorXd& y, bool addIntercept, CovarianceType covariance,
              int nwLags) {
  const Eigen::Index n = y.size();
  MCB_REQUIRE(Xin.rows() == n, "ols: X has " << Xin.rows() << " rows but y has " << n);
  const Eigen::Index p = Xin.cols() + (addIntercept ? 1 : 0);
  MCB_REQUIRE(n > p, "ols: not enough observations (" << n << ") for " << p << " coefficients");

  Eigen::MatrixXd X(n, p);
  if (addIntercept) {
    X.col(0).setOnes();
    X.rightCols(Xin.cols()) = Xin;
  } else {
    X = Xin;
  }

  OlsResult r;
  r.n = static_cast<int>(n);
  r.k = static_cast<int>(p);
  const Eigen::MatrixXd XtX = X.transpose() * X;
  const Eigen::LDLT<Eigen::MatrixXd> ldlt(XtX);
  MCB_REQUIRE(ldlt.info() == Eigen::Success && ldlt.isPositive(), "ols: regressors are collinear");
  r.beta = ldlt.solve(X.transpose() * y);
  r.residuals = y - X * r.beta;
  const Eigen::MatrixXd XtXinv = ldlt.solve(Eigen::MatrixXd::Identity(p, p));

  const double ssr = r.residuals.squaredNorm();
  const double sst = addIntercept ? (y.array() - y.mean()).matrix().squaredNorm() : y.squaredNorm();
  r.r2 = sst > 0.0 ? 1.0 - ssr / sst : 0.0;
  r.adjR2 = 1.0 - (1.0 - r.r2) * static_cast<double>(n - (addIntercept ? 1 : 0)) / static_cast<double>(n - p);

  if (covariance == CovarianceType::Homoskedastic) {
    r.covariance = XtXinv * (ssr / static_cast<double>(n - p));
  } else {
    // "Meat" of the sandwich: long-run variance of the scores x_t * e_t.
    const Eigen::MatrixXd scores = X.array().colwise() * r.residuals.array();  // n x p
    Eigen::MatrixXd B = scores.transpose() * scores;                           // lag 0
    if (covariance == CovarianceType::NeweyWest) {
      const int L = std::min<int>(nwLags, static_cast<int>(n) - 1);
      for (int j = 1; j <= L; ++j) {
        const double w = 1.0 - static_cast<double>(j) / (L + 1.0);  // Bartlett weights keep B positive semi-definite
        const Eigen::MatrixXd G = scores.bottomRows(n - j).transpose() * scores.topRows(n - j);
        B += w * (G + G.transpose());
      }
    }
    r.covariance = XtXinv * B * XtXinv;
  }
  r.stdErrors = r.covariance.diagonal().cwiseMax(0.0).cwiseSqrt();
  r.tStats = r.beta.cwiseQuotient(r.stdErrors);
  return r;
}

void simpleRegression(const Eigen::VectorXd& x, const Eigen::VectorXd& y, double& intercept, double& slope) {
  const double mx = x.mean(), my = y.mean();
  const double sxx = (x.array() - mx).square().sum();
  const double sxy = ((x.array() - mx) * (y.array() - my)).sum();
  slope = sxx > 0.0 ? sxy / sxx : 0.0;
  intercept = my - slope * mx;
}

double quantile(std::vector<double> v, double q) {
  MCB_REQUIRE(!v.empty(), "quantile of an empty sample");
  std::sort(v.begin(), v.end());
  const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(pos));
  const auto hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (pos - static_cast<double>(lo)) * (v[hi] - v[lo]);
}

double mean(const std::vector<double>& v) {
  MCB_REQUIRE(!v.empty(), "mean of an empty sample");
  double s = 0.0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

double stdev(const std::vector<double>& v) {
  if (v.size() < 2) return 0.0;
  const double m = mean(v);
  double s = 0.0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / static_cast<double>(v.size() - 1));
}

double autocorrelation(const Eigen::VectorXd& x, int lag) {
  const Eigen::Index n = x.size();
  MCB_REQUIRE(n > lag + 1, "autocorrelation: series too short");
  const double m = x.mean();
  const double denom = (x.array() - m).square().sum();
  if (denom <= 0.0) return 0.0;
  return ((x.tail(n - lag).array() - m) * (x.head(n - lag).array() - m)).sum() / denom;
}

PcaResult pca(const Eigen::MatrixXd& X, bool useCorrelation) {
  MCB_REQUIRE(X.rows() > 2 && X.cols() >= 1, "pca: not enough data");
  Eigen::MatrixXd Z = X.rowwise() - X.colwise().mean();
  if (useCorrelation) {
    for (Eigen::Index j = 0; j < Z.cols(); ++j) {
      const double s = std::sqrt(Z.col(j).squaredNorm() / static_cast<double>(Z.rows() - 1));
      if (s > 0.0) Z.col(j) /= s;
    }
  }
  const Eigen::MatrixXd C = Z.transpose() * Z / static_cast<double>(Z.rows() - 1);
  const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(C);
  MCB_REQUIRE(es.info() == Eigen::Success, "pca: eigen-decomposition failed");
  const Eigen::Index k = C.rows();
  PcaResult r;
  r.eigenvalues.resize(k);
  r.loadings.resize(k, k);
  for (Eigen::Index j = 0; j < k; ++j) {  // Eigen returns ascending order
    r.eigenvalues[j] = es.eigenvalues()[k - 1 - j];
    r.loadings.col(j) = es.eigenvectors().col(k - 1 - j);
    if (r.loadings.col(j).sum() < 0.0) r.loadings.col(j) *= -1.0;  // fix the sign
  }
  r.explainedShare = r.eigenvalues / r.eigenvalues.sum();
  return r;
}

}  // namespace mcb
