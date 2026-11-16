#pragma once
// First-order (vector) autoregression with Kilian's (1998) bootstrap bias
// correction.
//
// The CG bootstrap resamples objects that are nearly serially uncorrelated.
// The one exception is the longest forward rate, which is persistent; it is
// "pre-whitened" with an AR(1) (a VAR(1) when external predictors are
// resampled with it) and the model residuals are resampled instead.  OLS
// estimates of persistent autoregressions are biased towards zero in small
// samples, hence the bias correction.
#include <Eigen/Dense>

#include <random>

namespace mcb {

struct Var1 {
  Eigen::VectorXd mu;         // intercept
  Eigen::MatrixXd Phi;        // y_t = mu + Phi y_{t-1} + e_t
  Eigen::MatrixXd residuals;  // (T-1) x k, row t-1 holds e_t
  double maxEigenvalue = 0.0; // largest modulus of the eigenvalues of Phi
  bool biasCorrected = false;
};

/// OLS estimate. Y is T x k with one observation per row.
Var1 fitVar1(const Eigen::MatrixXd& Y);

/// Kilian (1998): estimate the small-sample bias of Phi by simulating from
/// the fitted model, subtract it (shrinking the adjustment if it would make
/// the system non-stationary), and reset the intercept so that the model mean
/// equals the sample mean.  No correction is applied if the OLS estimate is
/// already non-stationary.
Var1 fitVar1BiasCorrected(const Eigen::MatrixXd& Y, int bootstrapReplications, std::mt19937_64& rng);

/// Largest eigenvalue modulus of a square matrix.
double spectralRadius(const Eigen::MatrixXd& A);

}  // namespace mcb
