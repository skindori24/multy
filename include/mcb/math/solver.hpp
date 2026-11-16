#pragma once
// One-dimensional root finding.
//
// Bootstrapping a curve is root-finding repeated once per instrument: "which
// discount factor at this maturity makes the instrument reprice to its
// quote?".  The pricing function is smooth and nearly linear in the unknown,
// so a couple of secant steps normally suffice; Brent's method is the
// bracketing fallback that cannot fail once a sign change is found.
#include <algorithm>
#include <cmath>
#include <limits>

#include "mcb/core/error.hpp"

namespace mcb {

struct RootResult {
  double x = 0.0;
  double fx = 0.0;
  int evaluations = 0;
  bool converged = false;
};

/// Brent's method on [a, b]; requires f(a) and f(b) to have opposite signs.
template <class F>
RootResult brent(F&& f, double a, double b, double fa, double fb, double xtol, int maxIter = 200) {
  RootResult res;
  MCB_REQUIRE(fa * fb <= 0.0, "brent: root is not bracketed");
  double c = b, fc = fb, d = b - a, e = d;
  const double eps = std::numeric_limits<double>::epsilon();
  for (int it = 0; it < maxIter; ++it) {
    if ((fb > 0.0 && fc > 0.0) || (fb < 0.0 && fc < 0.0)) {
      c = a;
      fc = fa;
      e = d = b - a;
    }
    if (std::fabs(fc) < std::fabs(fb)) {
      a = b, b = c, c = a;
      fa = fb, fb = fc, fc = fa;
    }
    const double tol1 = 2.0 * eps * std::fabs(b) + 0.5 * xtol;
    const double xm = 0.5 * (c - b);
    if (std::fabs(xm) <= tol1 || fb == 0.0) {
      res.x = b, res.fx = fb, res.converged = true;
      return res;
    }
    if (std::fabs(e) >= tol1 && std::fabs(fa) > std::fabs(fb)) {
      // inverse quadratic interpolation (or secant when only two points)
      const double s = fb / fa;
      double p, q;
      if (a == c) {
        p = 2.0 * xm * s;
        q = 1.0 - s;
      } else {
        const double qq = fa / fc, r = fb / fc;
        p = s * (2.0 * xm * qq * (qq - r) - (b - a) * (r - 1.0));
        q = (qq - 1.0) * (r - 1.0) * (s - 1.0);
      }
      if (p > 0.0) q = -q;
      p = std::fabs(p);
      const double min1 = 3.0 * xm * q - std::fabs(tol1 * q), min2 = std::fabs(e * q);
      if (2.0 * p < std::min(min1, min2)) {
        e = d;
        d = p / q;
      } else {
        d = xm;
        e = d;
      }
    } else {
      d = xm;
      e = d;
    }
    a = b;
    fa = fb;
    b += std::fabs(d) > tol1 ? d : (xm >= 0.0 ? tol1 : -tol1);
    fb = f(b);
    ++res.evaluations;
  }
  res.x = b, res.fx = fb, res.converged = false;
  return res;
}

/// Finds a root of f near `guess`, constrained to [lo, hi].
/// Secant steps from the guess come first (fast when the guess is good, as in
/// a warm-started rebuild); if they do not settle, a bracket is grown around
/// the guess and Brent's method finishes the job.
template <class F>
RootResult findRoot(F&& f, double guess, double step, double lo, double hi, double xtol = 1e-15,
                    double ftol = 0.0) {
  RootResult res;
  guess = std::clamp(guess, lo, hi);
  double x0 = guess, f0 = f(x0);
  res.evaluations = 1;
  if (f0 == 0.0 || std::fabs(f0) <= ftol) {
    res.x = x0, res.fx = f0, res.converged = true;
    return res;
  }
  // --- secant phase ---------------------------------------------------------
  double x1 = std::clamp(x0 + step, lo, hi);
  if (x1 == x0) x1 = std::clamp(x0 - step, lo, hi);
  double f1 = f(x1);
  ++res.evaluations;
  double bl = 0, bh = 0, fbl = 0, fbh = 0;  // best bracket seen so far
  bool bracketed = false;
  auto noteBracket = [&](double xa, double fa_, double xb, double fb_) {
    if (fa_ * fb_ <= 0.0 && (!bracketed || std::fabs(xb - xa) < std::fabs(bh - bl))) {
      bl = xa, fbl = fa_, bh = xb, fbh = fb_;
      bracketed = true;
    }
  };
  noteBracket(x0, f0, x1, f1);
  for (int it = 0; it < 12; ++it) {
    if (f1 == 0.0 || std::fabs(f1) <= ftol) {
      res.x = x1, res.fx = f1, res.converged = true;
      return res;
    }
    if (f1 == f0) break;
    double x2 = x1 - f1 * (x1 - x0) / (f1 - f0);
    if (!(x2 > lo && x2 < hi) || !std::isfinite(x2)) break;
    const double f2 = f(x2);
    ++res.evaluations;
    noteBracket(x1, f1, x2, f2);
    const bool small = std::fabs(x2 - x1) <= xtol * std::max(1.0, std::fabs(x2));
    x0 = x1, f0 = f1, x1 = x2, f1 = f2;
    if (small) {
      res.x = x1, res.fx = f1, res.converged = true;
      return res;
    }
  }
  // --- bracketing phase -----------------------------------------------------
  if (!bracketed) {
    double a = guess, b = guess, fa = f(guess), fb = fa, w = std::fabs(step) > 0 ? std::fabs(step) : 1e-4;
    ++res.evaluations;
    for (int it = 0; it < 80 && fa * fb > 0.0; ++it) {
      w *= 1.6;
      if (a > lo) {
        a = std::max(lo, a - w);
        fa = f(a);
        ++res.evaluations;
        if (fa * fb <= 0.0) break;
      }
      if (b < hi) {
        b = std::min(hi, b + w);
        fb = f(b);
        ++res.evaluations;
      }
      if (a <= lo && b >= hi) break;
    }
    if (fa * fb > 0.0) {
      res.x = std::fabs(fa) < std::fabs(fb) ? a : b;
      res.fx = std::min(std::fabs(fa), std::fabs(fb));
      res.converged = false;
      return res;
    }
    bl = a, fbl = fa, bh = b, fbh = fb;
  }
  RootResult br = brent(f, bl, bh, fbl, fbh, xtol);
  br.evaluations += res.evaluations;
  return br;
}

}  // namespace mcb
