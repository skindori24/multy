#include "mcb/stats/term_panel.hpp"

#include "mcb/core/error.hpp"

namespace mcb {

YieldPanel YieldPanel::slice(Eigen::Index from, Eigen::Index to) const {
  MCB_REQUIRE(from >= 0 && to <= T() && from < to, "YieldPanel::slice: bad range");
  YieldPanel p;
  p.periodYears = periodYears;
  p.dates.assign(dates.begin() + from, dates.begin() + to);
  p.yields = yields.middleRows(from, to - from);
  return p;
}

Eigen::MatrixXd forwardsFromYields(const Eigen::MatrixXd& y) {
  Eigen::MatrixXd f(y.rows(), y.cols());
  f.col(0) = y.col(0);
  for (Eigen::Index n = 2; n <= y.cols(); ++n)
    f.col(n - 1) = static_cast<double>(n) * y.col(n - 1) - static_cast<double>(n - 1) * y.col(n - 2);
  return f;
}

Eigen::MatrixXd yieldsFromForwards(const Eigen::MatrixXd& f) {
  Eigen::MatrixXd y(f.rows(), f.cols());
  Eigen::VectorXd running = Eigen::VectorXd::Zero(f.rows());
  for (Eigen::Index n = 1; n <= f.cols(); ++n) {
    running += f.col(n - 1);
    y.col(n - 1) = running / static_cast<double>(n);
  }
  return y;
}

Eigen::MatrixXd differenceReturns(const Eigen::MatrixXd& f) {
  const Eigen::Index T = f.rows(), N = f.cols();
  MCB_REQUIRE(T >= 2 && N >= 2, "differenceReturns: need at least two dates and two maturities");
  // dr(n)_{t+1} = f(n)_t - f(n-1)_{t+1}
  return f.block(0, 1, T - 1, N - 1) - f.block(1, 0, T - 1, N - 1);
}

Eigen::MatrixXd excessReturns(const Eigen::MatrixXd& y, int h) {
  const Eigen::Index T = y.rows(), N = y.cols();
  MCB_REQUIRE(h >= 1 && h < T, "excessReturns: bad horizon " << h);
  Eigen::MatrixXd rx = Eigen::MatrixXd::Zero(T - h, N);
  for (Eigen::Index n = h + 1; n <= N; ++n) {
    // p(n-h)_{t+h} - p(n)_t + p(h)_t with p(m) = -m y(m)
    rx.col(n - 1) = -static_cast<double>(n - h) * y.col(n - h - 1).tail(T - h) +
                    static_cast<double>(n) * y.col(n - 1).head(T - h) - static_cast<double>(h) * y.col(h - 1).head(T - h);
  }
  return rx;
}

Eigen::MatrixXd rebuildForwards(const Eigen::VectorXd& firstCurve, const Eigen::VectorXd& longForward,
                                const Eigen::MatrixXd& dr) {
  const Eigen::Index N = firstCurve.size(), T = longForward.size();
  MCB_REQUIRE(dr.rows() == T - 1 && dr.cols() == N - 1, "rebuildForwards: inconsistent dimensions");
  Eigen::MatrixXd f(T, N);
  f.row(0) = firstCurve.transpose();
  for (Eigen::Index t = 0; t + 1 < T; ++t) {
    // maturities 1..N-1 at t+1 come from maturities 2..N at t
    f.block(t + 1, 0, 1, N - 1) = f.block(t, 1, 1, N - 1) - dr.row(t);
    f(t + 1, N - 1) = longForward[t + 1];
  }
  return f;
}

}  // namespace mcb
