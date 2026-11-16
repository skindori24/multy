#pragma once
// Curve bootstrapping.
//
// Instruments are sorted by pillar date and the curve is solved one node at
// a time: when instrument i is processed, every earlier discount factor is
// already known, so there is a single unknown and a 1-D root search.
//
// With a local interpolation (log-linear) one sweep is exact.  With a
// non-local one (cubic spline, monotone convex) moving a node also changes
// the curve *before* it, so earlier instruments stop repricing; the sweep is
// then repeated until the nodes stop moving (the same iterative scheme
// QuantLib uses).  A simultaneous Newton solve over all nodes is available
// as an alternative ("global bootstrap").
#include <memory>
#include <string>
#include <vector>

#include "mcb/curves/rate_helpers.hpp"
#include "mcb/curves/yield_curve.hpp"

namespace mcb {

struct BootstrapOptions {
  Interpolation interpolation = Interpolation::LogLinear;
  double tolerance = 1e-13;   // largest node move (in ln DF) accepted as converged
  int maxPasses = 200;        // sweeps for non-local interpolation
  bool globalNewton = false;  // solve all nodes simultaneously after the first sweep
};

struct BootstrapReport {
  std::string method;          // "single pass", "iterative", "global Newton"
  int passes = 0;
  long evaluations = 0;        // number of instrument repricings
  bool converged = false;
  double maxAbsError = 0.0;    // max |implied quote - market quote|
  std::vector<double> errors;  // implied - market, in the order the helpers were given
};

/// Bootstraps a curve.
///   helpers          instruments (any order)
///   quotes           market quote of each helper (same order): rates as decimals, futures as prices
///   discountCurve    exogenous discounting curve, or nullptr to self-discount
///   warmStart        previous solution with identical pillars (optional)
///   firstDirtyNode   with a warm start and a local interpolation, nodes before
///                    this index (1-based, sorted by pillar) are reused as-is
std::shared_ptr<YieldCurve> bootstrapCurve(const std::string& name, Date referenceDate,
                                           const std::vector<const RateHelper*>& helpers,
                                           const std::vector<double>& quotes,
                                           const DiscountCurve* discountCurve, const BootstrapOptions& options,
                                           BootstrapReport* report = nullptr, const YieldCurve* warmStart = nullptr,
                                           std::size_t firstDirtyNode = 1);

/// d(implied quote k)/d(ln DF at node i) for the given helpers (rows) and the
/// curve's pillar nodes (columns), by central finite differences.  `curve` is
/// restored before returning.  Row-major, size helpers x curve.size().
std::vector<double> quoteJacobian(YieldCurve& curve, const std::vector<const RateHelper*>& helpers,
                                  const DiscountCurve* forwardingCurve, const DiscountCurve* discountCurve);

}  // namespace mcb
