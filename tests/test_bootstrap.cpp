// Single-curve bootstrapping (Phase 3): repricing, interpolation choices,
// solver variants and error handling.
#include <gtest/gtest.h>

#include <cmath>

#include "mcb/curves/bootstrap.hpp"
#include "mcb/core/error.hpp"
#include "mcb/curves/conventions.hpp"

using namespace mcb;

namespace {

const Date kAsOf(2026, 11, 13);

struct Market {
  std::vector<std::shared_ptr<RateHelper>> owned;
  std::vector<const RateHelper*> helpers;
  std::vector<double> quotes;
  void add(std::shared_ptr<RateHelper> h, double q) {
    helpers.push_back(h.get());
    owned.push_back(std::move(h));
    quotes.push_back(q);
  }
};

// A SOFR-style OIS market with a humped term structure.
Market sofrMarket() {
  Market m;
  const auto conv = OisConventions::sofr();
  const std::vector<std::pair<const char*, double>> quotes = {
      {"1W", 0.0388}, {"1M", 0.0396}, {"3M", 0.0416}, {"6M", 0.0436}, {"9M", 0.0452}, {"1Y", 0.0463},
      {"18M", 0.0469}, {"2Y", 0.0473}, {"3Y", 0.0473}, {"4Y", 0.0471}, {"5Y", 0.0471}, {"7Y", 0.0472},
      {"10Y", 0.0478}, {"12Y", 0.0483}, {"15Y", 0.0490}, {"20Y", 0.0495}, {"25Y", 0.0492}, {"30Y", 0.0487}};
  for (const auto& [tenor, q] : quotes) m.add(oisHelper(kAsOf, Period::parse(tenor), conv), q);
  return m;
}

BootstrapOptions opt(Interpolation k, bool newton = false) {
  BootstrapOptions o;
  o.interpolation = k;
  o.globalNewton = newton;
  return o;
}

}  // namespace

TEST(Bootstrap, RepricesEveryInstrumentForEveryInterpolation) {
  const Market m = sofrMarket();
  for (auto kind : {Interpolation::LogLinear, Interpolation::LogCubic, Interpolation::MonotoneLogCubic,
                    Interpolation::LinearZero, Interpolation::MonotoneConvex}) {
    BootstrapReport rep;
    const auto curve = bootstrapCurve("SOFR", kAsOf, m.helpers, m.quotes, nullptr, opt(kind), &rep);
    EXPECT_TRUE(rep.converged) << interpolationName(kind);
    EXPECT_LT(rep.maxAbsError, 1e-12) << interpolationName(kind) << " via " << rep.method << " in " << rep.passes;
    ASSERT_EQ(rep.errors.size(), m.helpers.size());
    for (std::size_t k = 0; k < m.helpers.size(); ++k)
      EXPECT_NEAR(m.helpers[k]->impliedQuote(*curve, *curve), m.quotes[k], 1e-12);
    // Discount factors decrease and stay in (0, 1].
    double prev = 1.0;
    for (double df : curve->pillarDiscounts()) {
      EXPECT_LT(df, prev);
      EXPECT_GT(df, 0.0);
      prev = df;
    }
  }
}

TEST(Bootstrap, LocalInterpolationNeedsOnePassAndNonLocalIterates) {
  const Market m = sofrMarket();
  BootstrapReport local, cubic;
  bootstrapCurve("SOFR", kAsOf, m.helpers, m.quotes, nullptr, opt(Interpolation::LogLinear), &local);
  bootstrapCurve("SOFR", kAsOf, m.helpers, m.quotes, nullptr, opt(Interpolation::LogCubic), &cubic);
  EXPECT_EQ(local.passes, 1);
  EXPECT_EQ(local.method, "single pass");
  EXPECT_GT(cubic.passes, 2);
  EXPECT_EQ(cubic.method, "iterative");
}

TEST(Bootstrap, FlatMarketGivesFlatCurve) {
  // Quotes generated from a flat 4% continuously compounded curve must give
  // that curve back, whatever the interpolation.
  const YieldCurve flat("flat", kAsOf, {Date(2090, 1, 1)}, {std::exp(-0.04 * (Date(2090, 1, 1) - kAsOf) / 365.0)},
                        Interpolation::LogLinear);
  Market m;
  const auto conv = OisConventions::sofr();
  for (const char* tenor : {"1M", "6M", "1Y", "2Y", "5Y", "10Y", "30Y"}) {
    auto h = oisHelper(kAsOf, Period::parse(tenor), conv);
    const double q = h->impliedQuote(flat, flat);
    m.add(std::move(h), q);
  }
  for (auto kind : {Interpolation::LogLinear, Interpolation::LogCubic, Interpolation::LinearZero,
                    Interpolation::MonotoneConvex}) {
    const auto curve = bootstrapCurve("c", kAsOf, m.helpers, m.quotes, nullptr, opt(kind));
    for (double t : {0.05, 0.4, 1.3, 4.0, 8.5, 17.0, 29.5}) {
      EXPECT_NEAR(curve->zeroRateT(t), 0.04, 1e-10) << interpolationName(kind) << " t=" << t;
      EXPECT_NEAR(curve->instantaneousForwardT(t), 0.04, 1e-8) << interpolationName(kind) << " t=" << t;
    }
  }
}

TEST(Bootstrap, GlobalNewtonAgreesWithIterativeSweeps) {
  const Market m = sofrMarket();
  for (auto kind : {Interpolation::LogCubic, Interpolation::MonotoneLogCubic}) {
    BootstrapReport a, b;
    const auto iterative = bootstrapCurve("c", kAsOf, m.helpers, m.quotes, nullptr, opt(kind, false), &a);
    const auto newton = bootstrapCurve("c", kAsOf, m.helpers, m.quotes, nullptr, opt(kind, true), &b);
    EXPECT_EQ(b.method, "global Newton");
    EXPECT_TRUE(b.converged);
    EXPECT_LT(b.maxAbsError, 1e-12);
    const auto d1 = iterative->pillarDiscounts(), d2 = newton->pillarDiscounts();
    for (std::size_t i = 0; i < d1.size(); ++i) EXPECT_NEAR(d1[i], d2[i], 1e-11) << interpolationName(kind) << " " << i;
  }
}

TEST(Bootstrap, WarmStartGivesTheSameCurve) {
  Market m = sofrMarket();
  for (auto kind : {Interpolation::LogLinear, Interpolation::LogCubic}) {
    const auto base = bootstrapCurve("c", kAsOf, m.helpers, m.quotes, nullptr, opt(kind));
    std::vector<double> bumped = m.quotes;
    bumped[12] += 0.0001;  // 10Y quote up 1bp
    BootstrapReport cold, warm;
    const auto fresh = bootstrapCurve("c", kAsOf, m.helpers, bumped, nullptr, opt(kind), &cold);
    const auto reused = bootstrapCurve("c", kAsOf, m.helpers, bumped, nullptr, opt(kind), &warm, base.get(), 13);
    const auto d1 = fresh->pillarDiscounts(), d2 = reused->pillarDiscounts();
    for (std::size_t i = 0; i < d1.size(); ++i) EXPECT_NEAR(d1[i], d2[i], 2e-12) << interpolationName(kind) << " " << i;
    EXPECT_LT(warm.evaluations, cold.evaluations) << interpolationName(kind);
    EXPECT_LT(warm.maxAbsError, 1e-12);
  }
}

TEST(Bootstrap, ShortRateInstrumentsDepositsFuturesAndFras) {
  const IborIndex e3 = IborIndex::euribor(Period{3, TimeUnit::Months});
  const Date asOf(2012, 12, 11);
  Market m;
  m.add(depositHelper(asOf, Period{3, TimeUnit::Months}, 2, e3.calendar, e3.convention, true, e3.dayCount), 0.00179);
  m.add(futuresHelper(Date(2012, 12, 19), e3, 0.0), 99.8225);
  m.add(futuresHelper(Date(2013, 3, 20), e3, 0.00002), 99.8726);  // with a 0.2bp convexity adjustment
  m.add(fraHelper(asOf, 9, e3), 0.0016);
  const auto curve = bootstrapCurve("3M", asOf, m.helpers, m.quotes, nullptr, opt(Interpolation::LogLinear));
  // deposit: the forward over its period is the quoted rate
  EXPECT_NEAR(curve->forwardRate(Date(2012, 12, 13), Date(2013, 3, 13), DayCount::Act360), 0.00179, 1e-14);
  // futures: forward = 100 - price - convexity adjustment
  EXPECT_NEAR(curve->forwardRate(Date(2012, 12, 19), Date(2013, 3, 19), DayCount::Act360), 0.001775, 1e-13);
  EXPECT_NEAR(curve->forwardRate(Date(2013, 3, 20), Date(2013, 6, 20), DayCount::Act360), 0.001274 - 0.00002, 1e-13);
  // 9x12 FRA
  EXPECT_NEAR(curve->forwardRate(Date(2013, 9, 13), Date(2013, 12, 13), DayCount::Act360), 0.0016, 1e-13);
}

TEST(Bootstrap, TreasuryStyleParCurveFromBillsAndParBonds) {
  const Date settle(2026, 11, 16);
  Market m;
  m.add(billHelper(settle, Period::parse("1M")), 0.0410);
  m.add(billHelper(settle, Period::parse("3M")), 0.0432);
  m.add(billHelper(settle, Period::parse("6M")), 0.0442);
  for (const auto& [tenor, y] : std::vector<std::pair<const char*, double>>{
           {"1Y", 0.0465}, {"2Y", 0.0488}, {"3Y", 0.0492}, {"5Y", 0.0498}, {"7Y", 0.0505}, {"10Y", 0.0515},
           {"20Y", 0.0552}, {"30Y", 0.0547}})
    m.add(parBondHelper(settle, Period::parse(tenor), 2), y);
  for (auto kind : {Interpolation::LogLinear, Interpolation::MonotoneConvex, Interpolation::LogCubic}) {
    BootstrapReport rep;
    const auto curve = bootstrapCurve("UST", settle, m.helpers, m.quotes, nullptr, opt(kind), &rep);
    EXPECT_LT(rep.maxAbsError, 1e-12) << interpolationName(kind) << " " << rep.method;
    // A 10-year 5.15% semi-annual bond prices at par on this curve.
    double pv = 0.0;
    for (int k = 1; k <= 20; ++k) pv += 0.5 * 5.15 * curve->discount(settle.addMonths(6 * k));
    pv += 100.0 * curve->discount(settle.addYears(10));
    EXPECT_NEAR(pv, 100.0, 1e-9) << interpolationName(kind);
    // Zero rates of an upward-sloping par curve lie above the par yields at the long end.
    EXPECT_GT(curve->zeroRateT(20.0), 0.0545);
  }
}

TEST(Bootstrap, RejectsInconsistentInputs) {
  Market m = sofrMarket();
  const auto conv = OisConventions::sofr();
  EXPECT_THROW(bootstrapCurve("c", kAsOf, {}, {}, nullptr, {}), Error);
  // quote count mismatch
  std::vector<double> fewer(m.quotes.begin(), m.quotes.end() - 1);
  EXPECT_THROW(bootstrapCurve("c", kAsOf, m.helpers, fewer, nullptr, {}), Error);
  // two instruments with the same pillar date
  Market dup = sofrMarket();
  dup.add(oisHelper(kAsOf, Period::parse("5Y"), conv), 0.0470);
  EXPECT_THROW(bootstrapCurve("c", kAsOf, dup.helpers, dup.quotes, nullptr, {}), Error);
  // an instrument that has already matured
  EXPECT_THROW(bootstrapCurve("c", Date(2060, 1, 1), m.helpers, m.quotes, nullptr, {}), Error);
  // a missing (NaN) quote
  std::vector<double> nanQuotes = m.quotes;
  nanQuotes[3] = std::nan("");
  EXPECT_THROW(bootstrapCurve("c", kAsOf, m.helpers, nanQuotes, nullptr, {}), Error);
  // a quote that no curve can reprice: -200% would need a negative discount factor
  std::vector<double> crazy = m.quotes;
  crazy[5] = -2.0;
  EXPECT_THROW(bootstrapCurve("c", kAsOf, m.helpers, crazy, nullptr, {}), Error);
}

TEST(Bootstrap, JacobianIsLowerTriangularForLocalInterpolation) {
  const Market m = sofrMarket();
  auto curve = bootstrapCurve("c", kAsOf, m.helpers, m.quotes, nullptr, opt(Interpolation::LogLinear));
  const std::size_t n = m.helpers.size();
  const auto jac = quoteJacobian(*curve, m.helpers, nullptr, nullptr);
  ASSERT_EQ(jac.size(), n * n);
  for (std::size_t k = 0; k < n; ++k) {
    EXPECT_NE(jac[k * n + k], 0.0);  // each instrument is sensitive to its own node
    for (std::size_t i = k + 1; i < n; ++i) EXPECT_EQ(jac[k * n + i], 0.0) << k << "," << i;  // not to later ones
  }
  // quoteJacobian leaves the curve untouched
  for (std::size_t k = 0; k < n; ++k) EXPECT_NEAR(m.helpers[k]->impliedQuote(*curve, *curve), m.quotes[k], 1e-12);
}

TEST(YieldCurve, BasicQueriesAndValidation) {
  const YieldCurve c("c", kAsOf, {Date(2027, 11, 13), Date(2028, 11, 13)}, {0.96, 0.92}, Interpolation::LogLinear);
  // The example from the project notes: DF 0.96 and 0.92 give yields 4.08% / 4.17% and a forward of 4.26%.
  EXPECT_NEAR(c.zeroRate(Date(2027, 11, 13)), -std::log(0.96), 1e-12);
  EXPECT_NEAR(c.zeroRateT(c.time(Date(2028, 11, 13))) * c.time(Date(2028, 11, 13)), -std::log(0.92), 1e-12);
  EXPECT_NEAR(std::log(c.discount(Date(2027, 11, 13)) / c.discount(Date(2028, 11, 13))), std::log(0.96 / 0.92), 1e-12);
  EXPECT_NEAR(c.discount(kAsOf), 1.0, 1e-15);
  EXPECT_EQ(c.size(), 2u);
  EXPECT_EQ(c.maxDate(), Date(2028, 11, 13));
  const SpreadedCurve shifted(c, 0.01);
  EXPECT_NEAR(shifted.zeroRateT(1.5), c.zeroRateT(1.5) + 0.01, 1e-12);
  EXPECT_THROW(YieldCurve("bad", kAsOf, {Date(2028, 1, 1), Date(2027, 1, 1)}, {0.9, 0.95}, Interpolation::LogLinear), Error);
  EXPECT_THROW(YieldCurve("bad", kAsOf, {Date(2027, 1, 1)}, {-0.5}, Interpolation::LogLinear), Error);
  EXPECT_THROW(YieldCurve("bad", kAsOf, {kAsOf}, {1.0}, Interpolation::LogLinear), Error);
}
