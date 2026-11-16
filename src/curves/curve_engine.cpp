#include "mcb/curves/curve_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "mcb/core/error.hpp"

namespace mcb {

const YieldCurve& CurveSet::curve(const std::string& name) const {
  const auto it = curves.find(name);
  MCB_REQUIRE(it != curves.end(), "curve '" << name << "' is not in the curve set");
  return *it->second;
}

const BootstrapReport& CurveSet::report(const std::string& name) const {
  const auto it = reports.find(name);
  MCB_REQUIRE(it != reports.end(), "no bootstrap report for curve '" << name << "'");
  return it->second;
}

double CurveSet::maxRepricingError() const {
  double worst = 0.0;
  for (const auto& kv : reports) worst = std::max(worst, kv.second.maxAbsError);
  return worst;
}

CurveEngine::CurveEngine(Date asOf, std::vector<CurveDefinition> definitions, BootstrapOptions options)
    : asOf_(asOf), defs_(std::move(definitions)), options_(options) {
  MCB_REQUIRE(!defs_.empty(), "curve engine needs at least one curve definition");
  std::set<std::string> seen;
  for (auto& def : defs_) {
    MCB_REQUIRE(!def.name.empty() && !seen.count(def.name), "duplicate or empty curve name '" << def.name << "'");
    MCB_REQUIRE(def.discountCurve.empty() || seen.count(def.discountCurve),
                "curve '" << def.name << "' is discounted on '" << def.discountCurve
                          << "', which must be defined before it");
    MCB_REQUIRE(!def.helpers.empty(), "curve '" << def.name << "' has no instruments");
    // Keep helpers in pillar order so node indices are stable.
    std::stable_sort(def.helpers.begin(), def.helpers.end(),
                     [](const auto& a, const auto& b) { return a->pillarDate() < b->pillarDate(); });
    for (const auto& h : def.helpers) {
      MCB_REQUIRE(!h->quoteTerms.empty(), "instrument '" << h->label << "' has no market quote id");
      for (const auto& term : h->quoteTerms) {
        if (!priceQuote_.count(term.id)) {
          quoteIds_.push_back(term.id);
          priceQuote_[term.id] = std::strcmp(h->kind(), "future") == 0;
          quoteInfo_[term.id] = {def.name, h->label};
        }
      }
    }
    seen.insert(def.name);
  }
}

bool CurveEngine::isPriceQuote(const std::string& id) const {
  const auto it = priceQuote_.find(id);
  MCB_REQUIRE(it != priceQuote_.end(), "unknown quote id '" << id << "'");
  return it->second;
}

std::string CurveEngine::quoteCurve(const std::string& id) const {
  const auto it = quoteInfo_.find(id);
  return it == quoteInfo_.end() ? std::string() : it->second.first;
}

std::string CurveEngine::quoteLabel(const std::string& id) const {
  const auto it = quoteInfo_.find(id);
  return it == quoteInfo_.end() ? std::string() : it->second.second;
}

double CurveEngine::helperQuote(const RateHelper& helper, const QuoteMap& quotes) {
  double q = 0.0;
  for (const auto& term : helper.quoteTerms) {
    const auto it = quotes.find(term.id);
    MCB_REQUIRE(it != quotes.end(), "missing market quote '" << term.id << "' for instrument '" << helper.label << "'");
    q += term.weight * it->second;
  }
  return q;
}

std::shared_ptr<const YieldCurve> CurveEngine::buildCurve(std::size_t index, const QuoteMap& quotes,
                                                          const CurveSet& partial, const YieldCurve* warmStart,
                                                          std::size_t firstDirtyNode, BootstrapReport& report) const {
  const CurveDefinition& def = defs_[index];
  std::vector<const RateHelper*> helpers;
  std::vector<double> q;
  helpers.reserve(def.helpers.size());
  q.reserve(def.helpers.size());
  for (const auto& h : def.helpers) {
    helpers.push_back(h.get());
    q.push_back(helperQuote(*h, quotes));
  }
  const DiscountCurve* discount = def.discountCurve.empty() ? nullptr : &partial.curve(def.discountCurve);
  BootstrapOptions opt = options_;
  opt.interpolation = def.interpolation;
  return bootstrapCurve(def.name, asOf_, helpers, q, discount, opt, &report, warmStart, firstDirtyNode);
}

std::shared_ptr<const CurveSet> CurveEngine::build(const QuoteMap& quotes) const {
  auto set = std::make_shared<CurveSet>();
  set->asOf = asOf_;
  set->quotes = quotes;
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    BootstrapReport report;
    set->curves[defs_[i].name] = buildCurve(i, quotes, *set, nullptr, 1, report);
    set->reports[defs_[i].name] = std::move(report);
    set->names.push_back(defs_[i].name);
  }
  return set;
}

std::shared_ptr<const CurveSet> CurveEngine::update(const CurveSet& previous, const QuoteMap& changedQuotes) const {
  MCB_REQUIRE(previous.asOf == asOf_, "cannot update a curve set built for another date");
  auto set = std::make_shared<CurveSet>();
  set->asOf = asOf_;
  set->quotes = previous.quotes;
  for (const auto& kv : changedQuotes) {
    MCB_REQUIRE(priceQuote_.count(kv.first), "unknown quote id '" << kv.first << "'");
    set->quotes[kv.first] = kv.second;
  }
  std::set<std::string> rebuilt;
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    const CurveDefinition& def = defs_[i];
    // First instrument (in pillar order) touched by a changed quote.
    std::size_t firstDirty = 0;
    for (std::size_t k = 0; k < def.helpers.size() && firstDirty == 0; ++k)
      for (const auto& term : def.helpers[k]->quoteTerms)
        if (changedQuotes.count(term.id)) firstDirty = k + 1;
    const bool discountChanged = !def.discountCurve.empty() && rebuilt.count(def.discountCurve);
    if (firstDirty == 0 && !discountChanged) {
      // Untouched: share the previous curve object.
      set->curves[def.name] = previous.curves.at(def.name);
      set->reports[def.name] = previous.reports.at(def.name);
    } else {
      BootstrapReport report;
      const YieldCurve* warm = previous.curves.at(def.name).get();
      set->curves[def.name] = buildCurve(i, set->quotes, *set, warm, discountChanged ? 1 : firstDirty, report);
      set->reports[def.name] = std::move(report);
      rebuilt.insert(def.name);
    }
    set->names.push_back(def.name);
  }
  return set;
}

}  // namespace mcb
