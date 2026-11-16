#include "mcb/math/interpolation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

std::string interpolationName(Interpolation k) {
  switch (k) {
    case Interpolation::LogLinear: return "log-linear";
    case Interpolation::LogCubic: return "log-cubic";
    case Interpolation::MonotoneLogCubic: return "monotone-log-cubic";
    case Interpolation::LinearZero: return "linear-zero";
    case Interpolation::MonotoneConvex: return "monotone-convex";
  }
  return "?";
}

Interpolation parseInterpolation(std::string_view name) {
  std::string s;
  for (char c : name)
    if (std::isalnum(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "loglinear") return Interpolation::LogLinear;
  if (s == "logcubic") return Interpolation::LogCubic;
  if (s == "monotonelogcubic") return Interpolation::MonotoneLogCubic;
  if (s == "linearzero") return Interpolation::LinearZero;
  if (s == "monotoneconvex") return Interpolation::MonotoneConvex;
  MCB_FAIL("unknown interpolation '" << name << "'");
}

bool isLocal(Interpolation k) { return k == Interpolation::LogLinear || k == Interpolation::LinearZero; }

namespace {

// Natural cubic spline: solves the tridiagonal system for the node slopes m_i
// (zero second derivative at both ends), optionally followed by Hyman's
// filter, which caps slopes so the spline cannot overshoot between nodes.
// Formulation and filter follow QuantLib's CubicInterpolation.
void cubicSlopes(const std::vector<double>& t, const std::vector<double>& y, bool monotone, std::vector<double>& m) {
  const std::size_t n = t.size();
  std::vector<double> dx(n - 1), S(n - 1);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    dx[i] = t[i + 1] - t[i];
    S[i] = (y[i + 1] - y[i]) / dx[i];
  }
  m.assign(n, 0.0);
  if (n == 2) {
    m[0] = m[1] = S[0];
    return;
  }
  // tridiagonal system: lower[i] m[i-1] + diag[i] m[i] + upper[i] m[i+1] = rhs[i]
  std::vector<double> lower(n, 0.0), diag(n, 0.0), upper(n, 0.0), rhs(n, 0.0);
  diag[0] = 2.0, upper[0] = 1.0, rhs[0] = 3.0 * S[0];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    lower[i] = dx[i];
    diag[i] = 2.0 * (dx[i] + dx[i - 1]);
    upper[i] = dx[i - 1];
    rhs[i] = 3.0 * (dx[i] * S[i - 1] + dx[i - 1] * S[i]);
  }
  lower[n - 1] = 1.0, diag[n - 1] = 2.0, rhs[n - 1] = 3.0 * S[n - 2];
  // Thomas algorithm
  for (std::size_t i = 1; i < n; ++i) {
    const double w = lower[i] / diag[i - 1];
    diag[i] -= w * upper[i - 1];
    rhs[i] -= w * rhs[i - 1];
  }
  m[n - 1] = rhs[n - 1] / diag[n - 1];
  for (std::size_t i = n - 1; i-- > 0;) m[i] = (rhs[i] - upper[i] * m[i + 1]) / diag[i];

  if (!monotone) return;
  auto sign = [](double v) { return v < 0.0 ? -1.0 : 1.0; };
  for (std::size_t i = 0; i < n; ++i) {
    double corrected = 0.0;
    if (i == 0) {
      if (m[i] * S[0] > 0.0) corrected = sign(m[i]) * std::min(std::fabs(m[i]), std::fabs(3.0 * S[0]));
    } else if (i == n - 1) {
      if (m[i] * S[n - 2] > 0.0) corrected = sign(m[i]) * std::min(std::fabs(m[i]), std::fabs(3.0 * S[n - 2]));
    } else {
      const double pm = (S[i - 1] * dx[i] + S[i] * dx[i - 1]) / (dx[i - 1] + dx[i]);
      double M = 3.0 * std::min({std::fabs(S[i - 1]), std::fabs(S[i]), std::fabs(pm)});
      if (i > 1 && (S[i - 1] - S[i - 2]) * (S[i] - S[i - 1]) > 0.0) {
        const double pd = (S[i - 1] * (2.0 * dx[i - 1] + dx[i - 2]) - S[i - 2] * dx[i - 1]) / (dx[i - 2] + dx[i - 1]);
        if (pm * pd > 0.0 && pm * (S[i - 1] - S[i - 2]) > 0.0)
          M = std::max(M, 1.5 * std::min(std::fabs(pm), std::fabs(pd)));
      }
      if (i + 2 < n && (S[i] - S[i - 1]) * (S[i + 1] - S[i]) > 0.0) {
        const double pu = (S[i] * (2.0 * dx[i] + dx[i + 1]) - S[i + 1] * dx[i]) / (dx[i] + dx[i + 1]);
        if (pm * pu > 0.0 && -pm * (S[i] - S[i - 1]) > 0.0)
          M = std::max(M, 1.5 * std::min(std::fabs(pm), std::fabs(pu)));
      }
      if (m[i] * pm > 0.0) corrected = sign(m[i]) * std::min(std::fabs(m[i]), M);
    }
    m[i] = corrected;
  }
}

// Hagan-West monotone-convex shape function on a unit interval.
// g0, g1 are the node forwards minus the interval's discrete forward; the
// function returns g(x) (deviation of the instantaneous forward from the
// discrete forward) and its integral G(x) from 0 to x.  By construction
// G(1) = 0, so the average forward over the interval is preserved.
void monotoneConvexShape(double g0, double g1, double x, double& g, double& G) {
  if (g0 == 0.0 && g1 == 0.0) {
    g = G = 0.0;
    return;
  }
  const bool sectorI = (g0 < 0.0 && -0.5 * g0 <= g1 && g1 <= -2.0 * g0) ||
                       (g0 > 0.0 && -0.5 * g0 >= g1 && g1 >= -2.0 * g0);
  if (sectorI) {  // plain quadratic
    g = g0 * (1.0 - 4.0 * x + 3.0 * x * x) + g1 * (-2.0 * x + 3.0 * x * x);
    G = g0 * (x - 2.0 * x * x + x * x * x) + g1 * (-x * x + x * x * x);
    return;
  }
  const bool sectorII = (g0 < 0.0 && g1 > -2.0 * g0) || (g0 > 0.0 && g1 < -2.0 * g0);
  if (sectorII) {  // flat at g0, then a parabola up to g1
    const double eta = (g1 + 2.0 * g0) / (g1 - g0);
    if (x <= eta) {
      g = g0;
      G = g0 * x;
    } else {
      const double u = (x - eta) / (1.0 - eta);
      g = g0 + (g1 - g0) * u * u;
      G = g0 * x + (g1 - g0) * (x - eta) * u * u / 3.0;
    }
    return;
  }
  const bool sectorIII = (g0 > 0.0 && 0.0 > g1 && g1 > -0.5 * g0) || (g0 < 0.0 && 0.0 < g1 && g1 < -0.5 * g0);
  if (sectorIII) {  // parabola from g0 down to g1, then flat at g1
    const double eta = 3.0 * g1 / (g1 - g0);
    if (x < eta) {
      const double u = (eta - x) / eta;
      g = g1 + (g0 - g1) * u * u;
      G = g1 * x + (g0 - g1) * (eta - (eta - x) * u * u) / 3.0;
    } else {
      g = g1;
      G = g1 * x + (g0 - g1) * eta / 3.0;
    }
    return;
  }
  // Sector IV: g0 and g1 have the same sign -> two parabolas meeting at A.
  const double eta = g1 / (g1 + g0);
  const double A = -g0 * g1 / (g0 + g1);
  if (x < eta) {
    const double u = (eta - x) / eta;
    g = A + (g0 - A) * u * u;
    G = A * x + (g0 - A) * (eta - (eta - x) * u * u) / 3.0;
  } else if (eta >= 1.0) {  // degenerate (g0 == 0): only reached at x == 1
    g = g1;
    G = A * x + (g0 - A) * eta / 3.0;
  } else {
    const double u = (x - eta) / (1.0 - eta);
    g = A + (g1 - A) * u * u;
    G = A * x + (g0 - A) * eta / 3.0 + (g1 - A) * (x - eta) * u * u / 3.0;
  }
}

}  // namespace

void CurveInterpolator::fit(const double* t, const double* y, std::size_t n) {
  MCB_REQUIRE(n >= 2, "interpolation needs at least two nodes");
  t_.assign(t, t + n);
  y_.assign(y, y + n);
  for (std::size_t i = 1; i < n; ++i)
    MCB_REQUIRE(t_[i] > t_[i - 1], "interpolation nodes must be strictly increasing in time");

  if (kind_ == Interpolation::LogCubic || kind_ == Interpolation::MonotoneLogCubic) {
    std::vector<double> m;
    cubicSlopes(t_, y_, kind_ == Interpolation::MonotoneLogCubic, m);
    a_.resize(n - 1), b_.resize(n - 1), c_.resize(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
      const double dx = t_[i + 1] - t_[i], S = (y_[i + 1] - y_[i]) / dx;
      a_[i] = m[i];
      b_[i] = (3.0 * S - m[i + 1] - 2.0 * m[i]) / dx;
      c_[i] = (m[i + 1] + m[i] - 2.0 * S) / (dx * dx);
    }
  } else if (kind_ == Interpolation::MonotoneConvex) {
    // discrete forwards per interval (index i = interval ending at node i)
    fd_.assign(n, 0.0);
    for (std::size_t i = 1; i < n; ++i) fd_[i] = -(y_[i] - y_[i - 1]) / (t_[i] - t_[i - 1]);
    // node forwards: interval-length-weighted average of the neighbouring discrete forwards
    f_.assign(n, 0.0);
    if (n == 2) {
      f_[0] = f_[1] = fd_[1];
    } else {
      // Positivity: where the neighbouring discrete forwards are positive, a
      // node forward is collared to [0, 2 min(fd_i, fd_i+1)], which keeps the
      // interpolated forward non-negative on both sides (Hagan-West, sec. 5).
      // Genuinely negative forwards (EUR 2012-2022) are left alone.
      auto collar = [](double v, double fdLeft, double fdRight) {
        if (fdLeft <= 0.0 || fdRight <= 0.0) return v;
        return std::clamp(v, 0.0, 2.0 * std::min(fdLeft, fdRight));
      };
      for (std::size_t i = 1; i + 1 < n; ++i) {
        f_[i] = ((t_[i] - t_[i - 1]) * fd_[i + 1] + (t_[i + 1] - t_[i]) * fd_[i]) / (t_[i + 1] - t_[i - 1]);
        f_[i] = collar(f_[i], fd_[i], fd_[i + 1]);
      }
      f_[0] = collar(fd_[1] - 0.5 * (f_[1] - fd_[1]), fd_[1], fd_[1]);
      f_[n - 1] = collar(fd_[n - 1] - 0.5 * (f_[n - 2] - fd_[n - 1]), fd_[n - 1], fd_[n - 1]);
    }
  }
}

std::size_t CurveInterpolator::segment(double t) const {
  const auto it = std::upper_bound(t_.begin(), t_.end(), t);
  if (it == t_.begin()) return 0;
  const auto i = static_cast<std::size_t>(it - t_.begin()) - 1;
  return std::min(i, t_.size() - 2);
}

double CurveInterpolator::value(double t) const {
  MCB_REQUIRE(t_.size() >= 2, "interpolator has not been fitted");
  const double tn = t_.back();
  if (t > tn) return y_.back() + (t - tn) * derivative(tn);  // flat-forward extrapolation
  const std::size_t i = segment(t);
  const double dx = t - t_[i];
  switch (kind_) {
    case Interpolation::LogLinear:
      return y_[i] + dx * (y_[i + 1] - y_[i]) / (t_[i + 1] - t_[i]);
    case Interpolation::LogCubic:
    case Interpolation::MonotoneLogCubic:
      return y_[i] + dx * (a_[i] + dx * (b_[i] + dx * c_[i]));
    case Interpolation::LinearZero: {
      // zero rate z = -y/t; the first segment is flat at the first pillar's rate
      const double z1 = -y_[i + 1] / t_[i + 1];
      const double z0 = i == 0 ? z1 : -y_[i] / t_[i];
      const double z = z0 + (z1 - z0) * dx / (t_[i + 1] - t_[i]);
      return -z * t;
    }
    case Interpolation::MonotoneConvex: {
      const double h = t_[i + 1] - t_[i], x = dx / h;
      double g, G;
      monotoneConvexShape(f_[i] - fd_[i + 1], f_[i + 1] - fd_[i + 1], x, g, G);
      return y_[i] - (fd_[i + 1] * dx + h * G);
    }
  }
  return 0.0;
}

double CurveInterpolator::derivative(double t) const {
  MCB_REQUIRE(t_.size() >= 2, "interpolator has not been fitted");
  if (t > t_.back()) t = t_.back();
  const std::size_t i = segment(t);
  const double dx = t - t_[i];
  switch (kind_) {
    case Interpolation::LogLinear:
      return (y_[i + 1] - y_[i]) / (t_[i + 1] - t_[i]);
    case Interpolation::LogCubic:
    case Interpolation::MonotoneLogCubic:
      return a_[i] + dx * (2.0 * b_[i] + 3.0 * c_[i] * dx);
    case Interpolation::LinearZero: {
      const double z1 = -y_[i + 1] / t_[i + 1];
      const double z0 = i == 0 ? z1 : -y_[i] / t_[i];
      const double slope = (z1 - z0) / (t_[i + 1] - t_[i]);
      return -(z0 + slope * dx) - t * slope;
    }
    case Interpolation::MonotoneConvex: {
      const double h = t_[i + 1] - t_[i], x = dx / h;
      double g, G;
      monotoneConvexShape(f_[i] - fd_[i + 1], f_[i + 1] - fd_[i + 1], x, g, G);
      return -(fd_[i + 1] + g);
    }
  }
  return 0.0;
}

}  // namespace mcb
