#pragma once
// Regression-based tests of the expectations hypothesis (section 4.2 of the
// paper), with bootstrap sampling distributions.
//
// The expectations hypothesis (EH) says forward rates are unbiased forecasts
// of future short rates (up to a constant).  If it held, the slope in each
// regression below would be 1.  The four classic formulations are all linear
// combinations of one primitive equation, yet behave very differently:
//
//   CG   f(n-m)_{t+m} - y(1)_t          on  f(n)_t - y(1)_t          (the paper's unifying test)
//   FB   y(1)_{t+n-1} - y(1)_t          on  f(n)_t - y(1)_t          (Fama-Bliss; CG with m = n-1)
//   CS1  (1/n) sum_s y(1)_{t+s} - y(1)_t on y(n)_t - y(1)_t           (Campbell-Shiller, long-rate spread)
//   CS2  y(n-m)_{t+m} - y(n)_t          on  m/(n-m) (y(n)_t - y(m)_t) (Campbell-Shiller, yield change)
//
// "Do forward rates predict future rates?" is exactly the question whether
// the CG/FB slope is 1 (yes, one-for-one), 0 (not at all) or in between.
#include <cstdint>
#include <string>
#include <vector>

#include "mcb/stats/cg_bootstrap.hpp"

namespace mcb {

enum class EhTestKind { CG, FB, CS1, CS2 };

struct EhTestSpec {
  EhTestKind kind = EhTestKind::CG;
  int m = 0;  // forecast horizon in periods (CG and CS2 only)
  std::string label() const;
};

/// Builds the regression for maturity n. Returns false when the test is not
/// defined for that maturity (or the sample is too short).
bool ehRegression(const EhTestSpec& spec, int n, const Eigen::MatrixXd& yields, const Eigen::MatrixXd& forwards,
                  Eigen::VectorXd& x, Eigen::VectorXd& y);
/// Newey-West lag appropriate for the overlap in the regression.
int ehOverlap(const EhTestSpec& spec, int n);

/// Slope, intercept and Newey-West standard error of the slope for a
/// one-regressor regression (fast path used inside the bootstrap loop).
void slopeWithNeweyWest(const Eigen::VectorXd& x, const Eigen::VectorXd& y, int lags, double& slope, double& se);

struct EhMaturityResult {
  int maturity = 0;
  int observations = 0;
  double slope = 0.0;       // OLS estimate on the observed data
  double stdError = 0.0;    // Newey-West
  // bootstrap sampling distribution of the OLS slope
  double bootMean = 0.0, q05 = 0.0, q16 = 0.0, q50 = 0.0, q84 = 0.0, q95 = 0.0;
  double pSlopeIsOne = 1.0;   // bootstrap p-value of H0: slope = 1 (EH holds)
  double pSlopeIsZero = 1.0;  // bootstrap p-value of H0: slope = 0 (no predictive content)
};

struct EhTestResult {
  EhTestSpec spec;
  std::vector<EhMaturityResult> byMaturity;
};

/// Runs each test for every maturity (in steps of maturityStep) with
/// `replications` bootstrap histories.
std::vector<EhTestResult> runEhTests(const CgBootstrap& cg, const std::vector<EhTestSpec>& specs, int replications,
                                     std::uint64_t seed, int maturityStep = 1);

}  // namespace mcb
