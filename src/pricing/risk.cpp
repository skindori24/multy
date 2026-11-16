#include "mcb/pricing/risk.hpp"

#include <Eigen/Dense>

#include <map>

#include "mcb/core/error.hpp"

namespace mcb {
namespace {

// A +1bp move in rate terms: +bump for a rate quote, -100*bump for a futures price.
double quoteShift(const CurveEngine& engine, const std::string& id, double bump) {
  return engine.isPriceQuote(id) ? -100.0 * bump : bump;
}

}  // namespace

RiskReport bumpAndRebuildRisk(const CurveEngine& engine, const CurveSet& base, const CurvePricer& pricer, double bump) {
  RiskReport r;
  r.method = "bump-and-rebuild";
  r.baseValue = pricer(base);
  QuoteMap all;
  for (const auto& id : engine.quoteIds()) {
    const double shifted = base.quotes.at(id) + quoteShift(engine, id, bump);
    const auto bumped = engine.update(base, QuoteMap{{id, shifted}});
    const double delta = (pricer(*bumped) - r.baseValue) * (1e-4 / bump);
    r.buckets.push_back({id, engine.quoteCurve(id), delta});
    r.bucketSum += delta;
    all[id] = shifted;
  }
  r.parallel = (pricer(*engine.update(base, all)) - r.baseValue) * (1e-4 / bump);
  return r;
}

RiskReport jacobianRisk(const CurveEngine& engine, const CurveSet& base, const CurvePricer& pricer, double bump) {
  RiskReport r;
  r.method = "Jacobian";
  r.baseValue = pricer(base);
  const auto& defs = engine.definitions();

  // Mutable copies of the curves, and the layout of the stacked node vector.
  CurveSet work = base;
  std::map<std::string, std::shared_ptr<YieldCurve>> mut;
  std::map<std::string, std::size_t> offset;
  std::size_t total = 0;
  for (const auto& def : defs) {
    mut[def.name] = std::make_shared<YieldCurve>(base.curve(def.name));
    work.curves[def.name] = mut[def.name];
    offset[def.name] = total;
    total += def.helpers.size();
  }
  const auto N = static_cast<Eigen::Index>(total);

  // g = dPV/dy for every node of every curve (central differences).
  Eigen::VectorXd g(N);
  const double h = 1e-6;
  for (const auto& def : defs) {
    YieldCurve& c = *mut[def.name];
    for (std::size_t i = 1; i <= c.size(); ++i) {
      const double y = c.nodeLogDiscounts()[i];
      c.setNodeLogDiscount(i, y + h);
      c.refit();
      const double up = pricer(work);
      c.setNodeLogDiscount(i, y - h);
      c.refit();
      const double down = pricer(work);
      c.setNodeLogDiscount(i, y);
      c.refit();
      g[static_cast<Eigen::Index>(offset[def.name] + i - 1)] = (up - down) / (2.0 * h);
    }
  }

  // J = d(implied quote)/dy: block lower-triangular (own curve + its discount curve).
  Eigen::MatrixXd J = Eigen::MatrixXd::Zero(N, N);
  for (const auto& def : defs) {
    std::vector<const RateHelper*> helpers;
    for (const auto& hp : def.helpers) helpers.push_back(hp.get());
    const std::size_t rows = offset[def.name], m = helpers.size();
    YieldCurve& own = *mut[def.name];
    const DiscountCurve* disc = def.discountCurve.empty() ? nullptr : mut[def.discountCurve].get();
    const auto ownBlock = quoteJacobian(own, helpers, nullptr, disc);
    for (std::size_t k = 0; k < m; ++k)
      for (std::size_t i = 0; i < m; ++i)
        J(static_cast<Eigen::Index>(rows + k), static_cast<Eigen::Index>(rows + i)) = ownBlock[k * m + i];
    if (disc) {
      YieldCurve& d = *mut[def.discountCurve];
      const std::size_t cols = offset[def.discountCurve], nd = d.size();
      const auto cross = quoteJacobian(d, helpers, &own, &d);
      for (std::size_t k = 0; k < m; ++k)
        for (std::size_t i = 0; i < nd; ++i)
          J(static_cast<Eigen::Index>(rows + k), static_cast<Eigen::Index>(cols + i)) = cross[k * nd + i];
    }
  }

  // dPV/d(helper quote) = lambda, with J^T lambda = g.
  const Eigen::VectorXd lambda = J.transpose().partialPivLu().solve(g);

  // Map helper quotes back to market quotes (a market quote can feed several helpers).
  std::map<std::string, double> byQuote;
  for (const auto& def : defs)
    for (std::size_t k = 0; k < def.helpers.size(); ++k)
      for (const auto& term : def.helpers[k]->quoteTerms)
        byQuote[term.id] += term.weight * lambda[static_cast<Eigen::Index>(offset[def.name] + k)];
  for (const auto& id : engine.quoteIds()) {
    const double delta = byQuote[id] * quoteShift(engine, id, bump) * (1e-4 / bump);
    r.buckets.push_back({id, engine.quoteCurve(id), delta});
    r.bucketSum += delta;
  }
  r.parallel = r.bucketSum;  // first-order: the parallel move is the sum of the buckets
  return r;
}

}  // namespace mcb
