// Phase 4 acceptance test: the three-curve EUR multicurve bootstrap (EONIA
// discounting, Euribor 6M and 3M projection) must match QuantLib on the same
// inputs.  Reference values come from scripts/quantlib_reference.py.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

#include "mcb/data/eur_reference.hpp"
#include "test_util.hpp"

using namespace mcb;

namespace {

struct Variant {
  std::string name;
  Interpolation interpolation;
  bool parCoupons;
  double discountTolerance;
};

const std::vector<Variant>& variants() {
  static const std::vector<Variant> v = {
      {"loglinear|par", Interpolation::LogLinear, true, 1e-12},
      {"loglinear|indexed", Interpolation::LogLinear, false, 1e-12},
      {"logcubic|par", Interpolation::LogCubic, true, 2e-10},
      {"monotonelogcubic|par", Interpolation::MonotoneLogCubic, true, 2e-10},
  };
  return v;
}

std::shared_ptr<const CurveSet> buildEur(const Variant& v, CurveEngine** engineOut = nullptr) {
  static std::map<std::string, std::pair<std::shared_ptr<CurveEngine>, std::shared_ptr<const CurveSet>>> cache;
  auto it = cache.find(v.name);
  if (it == cache.end()) {
    const auto market = loadReferenceMarket(mcbtest::sourcePath("data/reference/eur_20121211.csv"));
    QuoteMap quotes;
    auto engine = std::make_shared<CurveEngine>(makeEurEngine(market, v.interpolation, v.parCoupons, quotes));
    auto set = engine->build(quotes);
    it = cache.emplace(v.name, std::make_pair(engine, set)).first;
  }
  if (engineOut) *engineOut = it->second.first.get();
  return it->second.second;
}

}  // namespace

TEST(EurReferenceMarket, LoadsAllInstruments) {
  const auto market = loadReferenceMarket(mcbtest::sourcePath("data/reference/eur_20121211.csv"));
  EXPECT_EQ(market.asOf, Date(2012, 12, 11));
  std::map<std::string, int> count;
  for (const auto& i : market.instruments) ++count[i.curve];
  EXPECT_EQ(count["EUR-OIS"], 30);
  EXPECT_EQ(count["EUR-6M"], 36);
  EXPECT_EQ(count["EUR-3M"], 24);
  EXPECT_THROW(parseReferenceMarket("curve,kind,id,a,b,quote,link\nEUR-OIS,OIS,X,1Y,,1.0,\n"), Error);  // no as-of date
}

TEST(EurMulticurve, EveryInstrumentRepricesExactly) {
  for (const auto& v : variants()) {
    const auto set = buildEur(v);
    for (const auto& name : set->names) {
      const auto& rep = set->report(name);
      EXPECT_TRUE(rep.converged) << v.name << " " << name;
      // rates are decimals, so 1e-12 is a billionth of a basis point (futures prices: 1e-10)
      EXPECT_LT(rep.maxAbsError, 1e-10) << v.name << " " << name << " (" << rep.method << ", " << rep.passes
                                        << " passes)";
    }
  }
}

TEST(EurMulticurveVsQuantLib, PillarDatesMatch) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_pillars.csv"));
  for (const auto& v : variants()) {
    const auto set = buildEur(v);
    std::map<std::string, std::vector<Date>> expected;
    for (std::size_t i = 0; i < t.rows.size(); ++i)
      if (t.at(i, "variant") == v.name) expected[t.at(i, "curve")].push_back(Date::parse(t.at(i, "pillar")));
    ASSERT_EQ(expected.size(), 3u) << v.name;
    for (const auto& [name, dates] : expected) EXPECT_EQ(set->curve(name).pillarDates(), dates) << v.name << " " << name;
  }
}

TEST(EurMulticurveVsQuantLib, DiscountFactorsMatch) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_curves.csv"));
  ASSERT_GT(t.rows.size(), 1000u);
  for (const auto& v : variants()) {
    const auto set = buildEur(v);
    std::map<std::string, double> worst;
    int n = 0;
    for (std::size_t i = 0; i < t.rows.size(); ++i) {
      if (t.at(i, "variant") != v.name) continue;
      const auto& curve = set->curve(t.at(i, "curve"));
      const double diff = std::fabs(curve.discount(Date::parse(t.at(i, "date"))) - parseDouble(t.at(i, "discount")));
      worst[t.at(i, "curve")] = std::max(worst[t.at(i, "curve")], diff);
      ++n;
    }
    ASSERT_GT(n, 300) << v.name;
    for (const auto& [name, w] : worst) {
      std::cout << "  [" << v.name << "] " << name << ": max |DF - QuantLib| = " << w << "\n";
      EXPECT_LT(w, v.discountTolerance) << v.name << " " << name;
    }
  }
}

TEST(EurMulticurveVsQuantLib, ForwardEuriborFixingsMatch) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_forwards.csv"));
  for (const auto& v : variants()) {
    const auto set = buildEur(v);
    double worst = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < t.rows.size(); ++i) {
      if (t.at(i, "variant") != v.name) continue;
      const bool sixMonth = t.at(i, "index") == "EUR-6M";
      const IborIndex index = IborIndex::euribor(Period{sixMonth ? 6 : 3, TimeUnit::Months});
      const Date value = index.valueDate(Date::parse(t.at(i, "fixing_date")));
      const double forward = set->curve(t.at(i, "index")).forwardRate(value, index.maturityDate(value), index.dayCount);
      worst = std::max(worst, std::fabs(forward - parseDouble(t.at(i, "forward"))));
      ++n;
    }
    ASSERT_GT(n, 100) << v.name;
    std::cout << "  [" << v.name << "] max |forward - QuantLib| = " << worst << "\n";
    EXPECT_LT(worst, 100 * v.discountTolerance) << v.name;  // forwards are differences of DFs divided by ~0.25-0.5
  }
}

TEST(EurMulticurve, ProjectionCurvesSitAboveTheDiscountCurve) {
  // The whole point of the multicurve framework: after 2008 a 6M Euribor
  // loan carries more credit/liquidity risk than rolling overnight, so
  // Euribor forwards exceed OIS forwards, and 6M exceeds 3M.
  const auto set = buildEur(variants()[0]);
  const Calendar target(Calendar::Kind::Target);
  for (int years : {1, 2, 5, 10, 20}) {
    const Date start = target.adjust(Date(2012 + years, 12, 13));
    const Date end6 = target.advance(start, Period{6, TimeUnit::Months}, BusinessDayConvention::ModifiedFollowing);
    const double ois = set->curve("EUR-OIS").forwardRate(start, end6, DayCount::Act360);
    const double e3 = set->curve("EUR-3M").forwardRate(start, end6, DayCount::Act360);
    const double e6 = set->curve("EUR-6M").forwardRate(start, end6, DayCount::Act360);
    EXPECT_GT(e3, ois) << years << "Y";
    EXPECT_GT(e6, e3) << years << "Y";
  }
}
