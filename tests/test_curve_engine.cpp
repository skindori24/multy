// Phase 6: the real-time layer. Incremental updates must give exactly the
// curves a full rebuild would, while touching as little as possible.
#include <gtest/gtest.h>

#include <cmath>

#include "mcb/data/eur_reference.hpp"
#include "test_util.hpp"

using namespace mcb;

namespace {

struct Fixture {
  std::shared_ptr<CurveEngine> engine;
  QuoteMap quotes;
};

Fixture makeFixture(Interpolation interp) {
  const auto market = loadReferenceMarket(mcbtest::sourcePath("data/reference/eur_20121211.csv"));
  Fixture f;
  f.engine = std::make_shared<CurveEngine>(makeEurEngine(market, interp, true, f.quotes));
  return f;
}

double maxPillarDiff(const YieldCurve& a, const YieldCurve& b) {
  const auto da = a.pillarDiscounts(), db = b.pillarDiscounts();
  EXPECT_EQ(da.size(), db.size());
  double worst = 0.0;
  for (std::size_t i = 0; i < da.size() && i < db.size(); ++i) worst = std::max(worst, std::fabs(da[i] - db[i]));
  return worst;
}

}  // namespace

TEST(CurveEngine, DescribesItsQuotes) {
  const Fixture f = makeFixture(Interpolation::LogLinear);
  EXPECT_EQ(f.engine->quoteIds().size(), 90u);
  EXPECT_EQ(f.quotes.size(), 90u);
  EXPECT_TRUE(f.engine->isPriceQuote("EUR3M_FUT_Z2"));
  EXPECT_FALSE(f.engine->isPriceQuote("EONIA_10Y"));
  EXPECT_EQ(f.engine->quoteCurve("EUR6M_SWAP_10Y"), "EUR-6M");
  EXPECT_EQ(f.engine->quoteCurve("EUR3S6S_10Y"), "EUR-3M");
  EXPECT_THROW(f.engine->isPriceQuote("NOPE"), Error);
  EXPECT_NEAR(f.quotes.at("EONIA_10Y"), 0.0128, 1e-15);       // percent converted to decimal
  EXPECT_NEAR(f.quotes.at("EUR3M_FUT_Z2"), 99.8225, 1e-12);   // futures stay as prices
}

TEST(CurveEngine, UpdateEqualsFullRebuild) {
  for (auto interp : {Interpolation::LogLinear, Interpolation::LogCubic}) {
    const Fixture f = makeFixture(interp);
    const auto base = f.engine->build(f.quotes);
    for (const char* id : {"EONIA_5Y", "EUR6M_SWAP_10Y", "EUR6M_FRA_6x12", "EUR3M_FUT_M3", "EUR3S6S_20Y", "EONIA_ON"}) {
      const double shift = f.engine->isPriceQuote(id) ? -0.01 : 0.0001;
      QuoteMap changed{{id, f.quotes.at(id) + shift}};
      QuoteMap all = f.quotes;
      all[id] = changed[id];
      const auto incremental = f.engine->update(*base, changed);
      const auto full = f.engine->build(all);
      for (const auto& name : full->names) {
        EXPECT_LT(maxPillarDiff(incremental->curve(name), full->curve(name)), interp == Interpolation::LogLinear ? 1e-14 : 5e-12)
            << interpolationName(interp) << " " << id << " " << name;
        EXPECT_LT(incremental->report(name).maxAbsError, 1e-10) << id << " " << name;
      }
      EXPECT_NEAR(incremental->quotes.at(id), changed[id], 0.0);
    }
  }
}

TEST(CurveEngine, UpdateRebuildsOnlyWhatTheQuoteAffects) {
  const Fixture f = makeFixture(Interpolation::LogLinear);
  const auto base = f.engine->build(f.quotes);
  auto shared = [&](const std::shared_ptr<const CurveSet>& s, const char* name) {
    return s->curves.at(name).get() == base->curves.at(name).get();
  };
  // A 3M futures tick: only the 3M curve is rebuilt, the other two are shared.
  auto s = f.engine->update(*base, {{"EUR3M_FUT_M3", 99.87}});
  EXPECT_TRUE(shared(s, "EUR-OIS"));
  EXPECT_TRUE(shared(s, "EUR-6M"));
  EXPECT_FALSE(shared(s, "EUR-3M"));
  // A 6M FRA tick: the 3M curve does not use that quote and is shared too.
  s = f.engine->update(*base, {{"EUR6M_FRA_6x12", 0.0026}});
  EXPECT_TRUE(shared(s, "EUR-OIS"));
  EXPECT_FALSE(shared(s, "EUR-6M"));
  EXPECT_TRUE(shared(s, "EUR-3M"));
  // A 6M swap tick also moves the 3M curve, whose swap quotes are "6M swap minus basis".
  s = f.engine->update(*base, {{"EUR6M_SWAP_10Y", 0.0160}});
  EXPECT_TRUE(shared(s, "EUR-OIS"));
  EXPECT_FALSE(shared(s, "EUR-6M"));
  EXPECT_FALSE(shared(s, "EUR-3M"));
  // An OIS tick changes discounting, so everything is rebuilt.
  s = f.engine->update(*base, {{"EONIA_5Y", 0.0050}});
  EXPECT_FALSE(shared(s, "EUR-OIS"));
  EXPECT_FALSE(shared(s, "EUR-6M"));
  EXPECT_FALSE(shared(s, "EUR-3M"));
  // No change at all: everything is shared.
  s = f.engine->update(*base, {});
  EXPECT_TRUE(shared(s, "EUR-OIS") && shared(s, "EUR-6M") && shared(s, "EUR-3M"));
}

TEST(CurveEngine, IncrementalUpdateDoesLessWork) {
  const Fixture f = makeFixture(Interpolation::LogLinear);
  const auto base = f.engine->build(f.quotes);
  const auto tick = f.engine->update(*base, {{"EUR6M_SWAP_30Y", f.quotes.at("EUR6M_SWAP_30Y") + 0.0001}});
  // Only nodes from the 30Y pillar onwards are re-solved.
  EXPECT_LT(tick->report("EUR-6M").evaluations, base->report("EUR-6M").evaluations / 3);
  // Nodes before the ticked pillar are bit-for-bit identical.
  const auto before = base->curve("EUR-6M").pillarDiscounts(), after = tick->curve("EUR-6M").pillarDiscounts();
  const auto& dates = base->curve("EUR-6M").pillarDates();
  for (std::size_t i = 0; i < dates.size(); ++i) {
    if (dates[i] < Date(2042, 1, 1)) {
      EXPECT_EQ(before[i], after[i]) << dates[i].iso();
    }
  }
  EXPECT_NE(before.back(), after.back());
}

TEST(CurveEngine, PreviousSnapshotsStayValid) {
  // Pricers holding an old CurveSet must not see later ticks.
  const Fixture f = makeFixture(Interpolation::LogLinear);
  const auto base = f.engine->build(f.quotes);
  const double dfBefore = base->curve("EUR-OIS").discount(Date(2022, 12, 13));
  const auto next = f.engine->update(*base, {{"EONIA_10Y", 0.02}});
  EXPECT_EQ(base->curve("EUR-OIS").discount(Date(2022, 12, 13)), dfBefore);
  EXPECT_NE(next->curve("EUR-OIS").discount(Date(2022, 12, 13)), dfBefore);
  EXPECT_NEAR(base->quotes.at("EONIA_10Y"), 0.0128, 1e-15);
  EXPECT_NEAR(next->quotes.at("EONIA_10Y"), 0.02, 1e-15);
}

TEST(CurveEngine, RejectsBadInput) {
  const Fixture f = makeFixture(Interpolation::LogLinear);
  const auto base = f.engine->build(f.quotes);
  EXPECT_THROW(f.engine->update(*base, {{"NOT_A_QUOTE", 1.0}}), Error);
  QuoteMap missing = f.quotes;
  missing.erase("EUR6M_SWAP_10Y");
  EXPECT_THROW(f.engine->build(missing), Error);
  EXPECT_THROW(base->curve("EUR-12M"), Error);
  // a curve discounted on a curve defined after it
  CurveDefinition a, b;
  a.name = "A";
  a.discountCurve = "B";
  a.helpers = f.engine->definitions()[0].helpers;
  b.name = "B";
  b.helpers = f.engine->definitions()[0].helpers;
  EXPECT_THROW(CurveEngine(Date(2012, 12, 11), {a, b}), Error);
  EXPECT_THROW(CurveEngine(Date(2012, 12, 11), {}), Error);
}
