#pragma once
// Ordinary least squares with robust standard errors, plus small helpers.
//
// Bond-return regressions have autocorrelated errors: a 12-month return
// sampled monthly shares 11 months with its neighbour.  Newey-West (HAC)
// standard errors correct for that asymptotically, although (as the paper
// stresses) not well enough in small samples, which is what the bootstrap
// in cg_bootstrap.hpp is for.
#include <Eigen/Dense>

#include <vector>

namespace mcb {

enum class CovarianceType {
  Homoskedastic,  // classical s^2 (X'X)^-1
  White,          // heteroskedasticity-robust (Eicker-White, HC0)
  NeweyWest       // heteroskedasticity- and autocorrelation-robust, Bartlett kernel
};

struct OlsResult {
  Eigen::VectorXd beta;       // coefficients (intercept first when requested)
  Eigen::VectorXd stdErrors;
  Eigen::VectorXd tStats;     // beta / stdErrors
  Eigen::MatrixXd covariance; // estimated covariance of beta
  Eigen::VectorXd residuals;
  double r2 = 0.0;
  double adjR2 = 0.0;
  int n = 0;                  // observations
  int k = 0;                  // regressors including the intercept
};

/// Regresses y on X (n x p). When addIntercept is true a constant is
/// prepended, so beta[0] is the intercept and beta[j+1] belongs to column j.
/// nwLags is the Newey-West truncation lag (ignored otherwise).
OlsResult ols(const Eigen::MatrixXd& X, const Eigen::VectorXd& y, bool addIntercept = true,
              CovarianceType covariance = CovarianceType::Homoskedastic, int nwLags = 0);

/// Slope and intercept of a one-regressor regression (no standard errors):
/// the inner loop of the bootstrap.
void simpleRegression(const Eigen::VectorXd& x, const Eigen::VectorXd& y, double& intercept, double& slope);

/// Empirical quantile (linear interpolation between order statistics). q in [0,1].
double quantile(std::vector<double> values, double q);
double mean(const std::vector<double>& values);
double stdev(const std::vector<double>& values);
/// First-order autocorrelation.
double autocorrelation(const Eigen::VectorXd& x, int lag = 1);

struct PcaResult {
  Eigen::VectorXd eigenvalues;       // descending
  Eigen::MatrixXd loadings;          // columns = eigenvectors
  Eigen::VectorXd explainedShare;    // eigenvalue / sum
};
/// Principal components of the columns of X (correlation or covariance matrix).
PcaResult pca(const Eigen::MatrixXd& X, bool useCorrelation);

}  // namespace mcb
