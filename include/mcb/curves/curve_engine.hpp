#pragma once
// The real-time layer: market snapshot -> curve builder -> immutable curves.
//
//   QuoteMap (id -> value)  --build-->  CurveSet (shared, immutable)  -->  pricers
//
// A CurveEngine owns the *definition* of a set of curves (which instruments,
// which interpolation, which curve discounts which).  Definitions never
// change after construction, so one engine can serve any number of market
// snapshots, from any number of threads.  Each build returns a new CurveSet;
// pricers keep a shared_ptr to the set they started with and are never
// disturbed by later ticks.
//
// When a single quote ticks, update() re-solves only what that quote can
// affect: the curve that uses it (from its pillar onwards when the
// interpolation is local) and the curves discounted on it.  Everything else
// is shared with the previous CurveSet.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mcb/curves/bootstrap.hpp"
#include "mcb/curves/rate_helpers.hpp"
#include "mcb/curves/yield_curve.hpp"

namespace mcb {

/// Market quotes by id. Rates are decimals (0.0125 = 1.25%), futures are prices.
using QuoteMap = std::map<std::string, double>;

struct CurveDefinition {
  std::string name;
  std::string discountCurve;  // name of the exogenous discount curve; empty = self-discounting
  Interpolation interpolation = Interpolation::LogLinear;
  std::vector<std::shared_ptr<const RateHelper>> helpers;
};

/// An immutable set of bootstrapped curves for one market snapshot.
struct CurveSet {
  Date asOf;
  QuoteMap quotes;
  std::vector<std::string> names;  // in build (dependency) order
  std::map<std::string, std::shared_ptr<const YieldCurve>> curves;
  std::map<std::string, BootstrapReport> reports;

  const YieldCurve& curve(const std::string& name) const;
  bool has(const std::string& name) const { return curves.count(name) > 0; }
  const BootstrapReport& report(const std::string& name) const;
  double maxRepricingError() const;
};

class CurveEngine {
 public:
  /// Definitions must be listed so that a discount curve precedes its users.
  CurveEngine(Date asOf, std::vector<CurveDefinition> definitions, BootstrapOptions options = {});

  Date asOf() const { return asOf_; }
  const std::vector<CurveDefinition>& definitions() const { return defs_; }
  const BootstrapOptions& options() const { return options_; }

  /// Builds every curve from scratch.
  std::shared_ptr<const CurveSet> build(const QuoteMap& quotes) const;
  /// Applies changed quotes to a previous build, re-solving only what they affect.
  std::shared_ptr<const CurveSet> update(const CurveSet& previous, const QuoteMap& changedQuotes) const;

  /// All market quote ids, ordered by curve then pillar.
  const std::vector<std::string>& quoteIds() const { return quoteIds_; }
  /// True for quotes that are futures prices rather than rates.
  bool isPriceQuote(const std::string& id) const;
  /// Curve and instrument label that first use a quote id (for reports).
  std::string quoteCurve(const std::string& id) const;
  std::string quoteLabel(const std::string& id) const;

  /// The helper's quote in its natural units, assembled from market quotes.
  static double helperQuote(const RateHelper& helper, const QuoteMap& quotes);

 private:
  std::shared_ptr<const YieldCurve> buildCurve(std::size_t index, const QuoteMap& quotes, const CurveSet& partial,
                                               const YieldCurve* warmStart, std::size_t firstDirtyNode,
                                               BootstrapReport& report) const;

  Date asOf_;
  std::vector<CurveDefinition> defs_;
  BootstrapOptions options_;
  std::vector<std::string> quoteIds_;
  std::map<std::string, bool> priceQuote_;
  std::map<std::string, std::pair<std::string, std::string>> quoteInfo_;  // id -> (curve, label)
};

}  // namespace mcb
