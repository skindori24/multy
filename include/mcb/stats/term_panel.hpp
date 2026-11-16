#pragma once
// A history of zero-coupon yield curves and the identities that link yields,
// forwards and returns (Crump & Gospodinov, "Deconstructing the Yield Curve",
// section 2.1).
//
// Time is measured in periods (months or quarters) and maturities in whole
// numbers of periods n = 1..N.  All rates are per-period log rates, so that
// with log prices p(n) = -n y(n) the identities are exact and unit-free:
//
//   forward            f(n)_t      = p(n-1)_t - p(n)_t = n y(n)_t - (n-1) y(n-1)_t
//   yield              y(n)_t      = (1/n) sum_{i<=n} f(i)_t
//   holding return     r(n)_{t+1}  = p(n-1)_{t+1} - p(n)_t
//   excess return      rx(n)_{t+1} = r(n)_{t+1} - y(1)_t
//   difference return  dr(n)_{t+1} = r(n)_{t+1} - r(n-1)_{t+1} = f(n)_t - f(n-1)_{t+1}
//   so that            rx(n)_{t+1} = sum_{i=2..n} dr(i)_{t+1}
//
// A difference return compares two forward rates for the *same* future
// period observed one period apart: it is the market's revision of that rate,
// i.e. mostly news.  That is why it has little serial correlation and no
// mechanical overlap across maturities, and why the bootstrap resamples it.
#include <Eigen/Dense>

#include <vector>

#include "mcb/core/date.hpp"

namespace mcb {

struct YieldPanel {
  std::vector<Date> dates;   // observation dates, one per row
  double periodYears = 0.25; // length of one period in years
  /// T x N per-period log yields: column n-1 is the n-period zero yield
  /// (annualised continuously compounded yield times periodYears).
  Eigen::MatrixXd yields;

  Eigen::Index T() const { return yields.rows(); }
  Eigen::Index N() const { return yields.cols(); }
  /// Annualised yield in percent, for display.
  double annualPercent(Eigen::Index t, int maturity) const { return yields(t, maturity - 1) / periodYears * 100.0; }
  /// Rows [from, to).
  YieldPanel slice(Eigen::Index from, Eigen::Index to) const;
};

/// f(n) = n y(n) - (n-1) y(n-1), same shape as the input.
Eigen::MatrixXd forwardsFromYields(const Eigen::MatrixXd& yields);
/// y(n) = average of the first n forwards.
Eigen::MatrixXd yieldsFromForwards(const Eigen::MatrixXd& forwards);
/// (T-1) x (N-1): row t, column n-2 holds dr(n)_{t+1} = f(n)_t - f(n-1)_{t+1}, n = 2..N.
Eigen::MatrixXd differenceReturns(const Eigen::MatrixXd& forwards);
/// (T-h) x N: row t, column n-1 holds the h-period excess return
/// rx(n,h)_{t+h} = p(n-h)_{t+h} - p(n)_t + p(h)_t.  Columns with n <= h are zero.
Eigen::MatrixXd excessReturns(const Eigen::MatrixXd& yields, int horizon);

/// Rebuilds the whole forward history from the three primitive objects:
/// the first forward curve, the path of the longest forward f(N), and the
/// difference returns (equations (11)-(13) of the paper):
///   f(n-1)_{t+1} = f(n)_t - dr(n)_{t+1}.
Eigen::MatrixXd rebuildForwards(const Eigen::VectorXd& firstCurve, const Eigen::VectorXd& longForward,
                                const Eigen::MatrixXd& differenceReturns);

}  // namespace mcb
