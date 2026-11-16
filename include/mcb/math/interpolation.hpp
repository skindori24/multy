#pragma once
// Interpolation of a discount curve between its pillars.
//
// All schemes interpolate y(t) = ln DF(t) through nodes (t_i, y_i) with
// t_0 = 0, y_0 = 0, and expose y and dy/dt (minus the instantaneous forward
// rate).  The choice matters: every scheme reprices the bootstrap
// instruments exactly, but the forward rates in between differ.
//
//   LogLinear         straight lines in ln DF  -> piecewise-constant forwards
//                     (local, robust; forwards jump at pillars)
//   LogCubic          natural cubic spline in ln DF -> smooth forwards
//                     (non-local: each node influences the whole curve)
//   MonotoneLogCubic  the same spline with Hyman's monotonicity filter
//   LinearZero        straight lines in the zero rate
//   MonotoneConvex    Hagan-West (2006): continuous forwards that preserve the
//                     monotonicity/positivity of the discrete forwards
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcb {

enum class Interpolation { LogLinear, LogCubic, MonotoneLogCubic, LinearZero, MonotoneConvex };

std::string interpolationName(Interpolation k);
Interpolation parseInterpolation(std::string_view name);
/// True when changing a node only affects the curve up to the next node, so a
/// single bootstrap pass is exact.
bool isLocal(Interpolation k);

class CurveInterpolator {
 public:
  explicit CurveInterpolator(Interpolation kind = Interpolation::LogLinear) : kind_(kind) {}

  Interpolation kind() const { return kind_; }

  /// Fits through the first n nodes (n >= 2, t strictly increasing, t[0] == 0).
  void fit(const double* t, const double* y, std::size_t n);
  void fit(const std::vector<double>& t, const std::vector<double>& y) { fit(t.data(), y.data(), t.size()); }

  /// ln DF(t). Beyond the last node the instantaneous forward is held flat.
  double value(double t) const;
  /// d ln DF / dt = -(instantaneous forward rate).
  double derivative(double t) const;

 private:
  std::size_t segment(double t) const;  // index i with t in [t_i, t_{i+1}]

  Interpolation kind_;
  std::vector<double> t_, y_;
  std::vector<double> a_, b_, c_;   // cubic: y = y_i + a dx + b dx^2 + c dx^3
  std::vector<double> fd_, f_;      // monotone convex: discrete and node forwards
};

}  // namespace mcb
