#pragma once
// Interest-rate risk: how much does a price move when a market quote moves?
//
// Two independent ways of computing the same "bucketed deltas" (the PV change
// for a 1 basis point rise in each individual curve input):
//
//   bump-and-rebuild   bump one quote, re-bootstrap the curves, reprice.
//                      Simple and exact, but needs one rebuild per quote.
//
//   Jacobian           differentiate through the bootstrap analytically:
//                        dPV/dq = dPV/dy * (dq/dy)^-1
//                      where y are the curve nodes and dq/dy is the Jacobian
//                      of the instruments' implied quotes.  One linear solve
//                      gives every bucket at once.
//
// The two must agree, and the buckets must add up to the parallel DV01 (all
// quotes bumped together); both facts are unit-tested.
#include <functional>
#include <string>
#include <vector>

#include "mcb/curves/curve_engine.hpp"

namespace mcb {

/// A pricing function of the curves, e.g. the NPV of a swap or a portfolio.
using CurvePricer = std::function<double(const CurveSet&)>;

struct QuoteDelta {
  std::string id;     // market quote id
  std::string curve;  // curve the quote belongs to
  double delta = 0.0; // PV change for +1bp in this quote (for futures: +1bp in the implied rate)
};

struct RiskReport {
  std::string method;
  std::vector<QuoteDelta> buckets;
  double bucketSum = 0.0;
  double parallel = 0.0;  // PV change when every quote moves +1bp together
  double baseValue = 0.0;
};

RiskReport bumpAndRebuildRisk(const CurveEngine& engine, const CurveSet& base, const CurvePricer& pricer,
                              double bump = 1e-4);
RiskReport jacobianRisk(const CurveEngine& engine, const CurveSet& base, const CurvePricer& pricer,
                        double bump = 1e-4);

}  // namespace mcb
