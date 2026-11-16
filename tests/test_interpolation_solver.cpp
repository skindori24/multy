// Root finders and curve interpolators.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "mcb/math/interpolation.hpp"
#include "mcb/math/solver.hpp"

using namespace mcb;

namespace {
const std::vector<Interpolation> kAll = {Interpolation::LogLinear, Interpolation::LogCubic,
                                         Interpolation::MonotoneLogCubic, Interpolation::LinearZero,
                                         Interpolation::MonotoneConvex};

// ln DF nodes of an upward-sloping curve: zero rate rising from 2% to 4.5%.
void sampleNodes(std::vector<double>& t, std::vector<double>& y) {
  t = {0.0, 0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0, 30.0};
  y.clear();
  for (double x : t) y.push_back(-(0.02 + 0.025 * (1.0 - std::exp(-x / 6.0))) * x);
}
}  // namespace

TEST(Solver, BrentFindsRootsOfSmoothFunctions) {
  auto f = [](double x) { return std::cos(x) - x; };
  const auto r = brent(f, 0.0, 1.0, f(0.0), f(1.0), 1e-15);
  EXPECT_TRUE(r.converged);
  EXPECT_NEAR(r.x, 0.7390851332151607, 1e-14);
  auto g = [](double x) { return x * x * x - 2.0 * x - 5.0; };
  EXPECT_NEAR(brent(g, 2.0, 3.0, g(2.0), g(3.0), 1e-15).x, 2.0945514815423265, 1e-13);
  EXPECT_THROW(brent(g, 3.0, 4.0, g(3.0), g(4.0), 1e-15), Error);  // not bracketed
}

TEST(Solver, FindRootWorksFromGoodAndBadGuesses) {
  auto f = [](double x) { return std::exp(x) - 3.0; };
  for (double guess : {1.0, 1.0986, -5.0, 8.0}) {
    const auto r = findRoot(f, guess, 1e-3, -20.0, 20.0, 1e-15);
    EXPECT_TRUE(r.converged) << guess;
    EXPECT_NEAR(r.x, std::log(3.0), 1e-12) << guess;
  }
  // A warm start needs only a handful of evaluations.
  EXPECT_LE(findRoot(f, std::log(3.0) + 1e-9, 1e-6, -20.0, 20.0, 1e-15).evaluations, 6);
  // No root inside the bounds: reported, not thrown.
  auto positive = [](double x) { return x * x + 1.0; };
  EXPECT_FALSE(findRoot(positive, 0.5, 0.1, -3.0, 3.0).converged);
  // Non-differentiable root.
  auto kink = [](double x) { return std::fabs(x - 0.3) < 1e-300 ? 0.0 : (x > 0.3 ? std::sqrt(x - 0.3) : -std::sqrt(0.3 - x)); };
  EXPECT_NEAR(findRoot(kink, 2.0, 0.1, -5.0, 5.0, 1e-14).x, 0.3, 1e-10);
}

TEST(Interpolation, EverySchemePassesThroughItsNodes) {
  std::vector<double> t, y;
  sampleNodes(t, y);
  for (auto kind : kAll) {
    CurveInterpolator interp(kind);
    interp.fit(t, y);
    for (std::size_t i = 0; i < t.size(); ++i) EXPECT_NEAR(interp.value(t[i]), y[i], 1e-14) << interpolationName(kind);
  }
}

TEST(Interpolation, DerivativeIsConsistentWithValue) {
  std::vector<double> t, y;
  sampleNodes(t, y);
  for (auto kind : kAll) {
    CurveInterpolator interp(kind);
    interp.fit(t, y);
    for (double x : {0.1, 0.4, 0.8, 1.7, 4.2, 6.5, 9.9, 15.0, 29.0}) {
      const double h = 1e-6;
      const double numeric = (interp.value(x + h) - interp.value(x - h)) / (2 * h);
      EXPECT_NEAR(interp.derivative(x), numeric, 1e-7) << interpolationName(kind) << " at " << x;
    }
  }
}

TEST(Interpolation, FlatCurveStaysFlatInEveryScheme) {
  const std::vector<double> t = {0.0, 0.5, 1.0, 3.0, 10.0};
  std::vector<double> y;
  for (double x : t) y.push_back(-0.037 * x);
  for (auto kind : kAll) {
    CurveInterpolator interp(kind);
    interp.fit(t, y);
    for (double x : {0.01, 0.3, 0.77, 2.0, 6.0, 9.99, 12.0}) {
      EXPECT_NEAR(interp.value(x), -0.037 * x, 1e-14) << interpolationName(kind);
      EXPECT_NEAR(interp.derivative(x), -0.037, 1e-13) << interpolationName(kind);
    }
  }
}

TEST(Interpolation, LogLinearGivesPiecewiseConstantForwards) {
  std::vector<double> t, y;
  sampleNodes(t, y);
  CurveInterpolator interp(Interpolation::LogLinear);
  interp.fit(t, y);
  EXPECT_DOUBLE_EQ(interp.derivative(2.1), interp.derivative(2.9));
  EXPECT_NE(interp.derivative(2.9), interp.derivative(3.1));  // jump at the 3y pillar
  // Beyond the last node the forward stays flat.
  EXPECT_NEAR(interp.value(35.0), y.back() + 5.0 * interp.derivative(29.0), 1e-13);
}

TEST(Interpolation, NaturalCubicSplineIsSmoothWithZeroEndCurvature) {
  std::vector<double> t, y;
  sampleNodes(t, y);
  CurveInterpolator interp(Interpolation::LogCubic);
  interp.fit(t, y);
  const double h = 1e-4;
  auto second = [&](double x) { return (interp.derivative(x + h) - interp.derivative(x - h)) / (2 * h); };
  // First derivative (the forward curve) is continuous across interior nodes ...
  for (std::size_t i = 1; i + 1 < t.size(); ++i)
    EXPECT_NEAR(interp.derivative(t[i] - 1e-9), interp.derivative(t[i] + 1e-9), 1e-7) << "node " << i;
  // ... and so is the second derivative (C2 spline), which vanishes at both ends.
  for (std::size_t i = 1; i + 1 < t.size(); ++i) EXPECT_NEAR(second(t[i] - 2 * h), second(t[i] + 2 * h), 2e-3);
  EXPECT_NEAR((interp.derivative(h) - interp.derivative(0.0)) / h, 0.0, 1e-5);
  EXPECT_NEAR((interp.derivative(30.0) - interp.derivative(30.0 - h)) / h, 0.0, 1e-5);
}

TEST(Interpolation, HymanFilterRemovesOvershoot) {
  // Data with a plateau: a plain spline overshoots it, the filtered one cannot.
  const std::vector<double> t = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
  const std::vector<double> y = {0.0, -1.0, -1.0, -1.0, -3.0, -3.2};
  CurveInterpolator plain(Interpolation::LogCubic), monotone(Interpolation::MonotoneLogCubic);
  plain.fit(t, y);
  monotone.fit(t, y);
  double plainMax = -10, monoMax = -10;
  for (double x = 1.0; x <= 3.0; x += 0.01) {
    plainMax = std::max(plainMax, plain.value(x));
    monoMax = std::max(monoMax, monotone.value(x));
  }
  EXPECT_GT(plainMax, -1.0 + 1e-3);    // overshoots above the plateau
  EXPECT_LE(monoMax, -1.0 + 1e-12);    // stays on it
  for (double x = 0.0; x < 5.0; x += 0.01) EXPECT_LE(monotone.derivative(x), 1e-12) << x;  // never increasing
}

TEST(Interpolation, MonotoneConvexKeepsForwardsContinuousAndPositive) {
  std::vector<double> t, y;
  sampleNodes(t, y);
  CurveInterpolator interp(Interpolation::MonotoneConvex);
  interp.fit(t, y);
  for (std::size_t i = 1; i + 1 < t.size(); ++i)
    EXPECT_NEAR(interp.derivative(t[i] - 1e-9), interp.derivative(t[i] + 1e-9), 1e-7) << "node " << i;
  for (double x = 0.0; x <= 30.0; x += 0.05) EXPECT_GT(-interp.derivative(x), 0.0) << x;
  // A hump in the discrete forwards must not create negative forwards either.
  const std::vector<double> t2 = {0.0, 1.0, 2.0, 3.0, 4.0};
  const std::vector<double> y2 = {0.0, -0.001, -0.081, -0.082, -0.083};  // forwards 0.1%, 8%, 0.1%, 0.1%
  CurveInterpolator hump(Interpolation::MonotoneConvex);
  hump.fit(t2, y2);
  for (double x = 0.0; x <= 4.0; x += 0.01) EXPECT_GE(-hump.derivative(x), -1e-12) << x;
  for (std::size_t i = 0; i < t2.size(); ++i) EXPECT_NEAR(hump.value(t2[i]), y2[i], 1e-14);
}

TEST(Interpolation, NamesRoundTripAndBadInputIsRejected) {
  for (auto kind : kAll) EXPECT_EQ(parseInterpolation(interpolationName(kind)), kind);
  EXPECT_THROW(parseInterpolation("quintic"), Error);
  EXPECT_TRUE(isLocal(Interpolation::LogLinear));
  EXPECT_FALSE(isLocal(Interpolation::LogCubic));
  CurveInterpolator interp(Interpolation::LogLinear);
  EXPECT_THROW(interp.value(1.0), Error);  // not fitted
  const std::vector<double> bad = {0.0, 1.0, 1.0}, vals = {0.0, -0.1, -0.2};
  EXPECT_THROW(interp.fit(bad, vals), Error);  // times must increase
}
