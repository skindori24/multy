// Phase 5: pricing of swaps, FRAs and bonds, and interest-rate risk.
// Prices are checked against QuantLib on the EUR reference curves; the two
// independent risk methods are checked against each other and against
// QuantLib bump-and-rebuild numbers.
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <map>

#include "mcb/data/eur_reference.hpp"
#include "mcb/pricing/bonds.hpp"
#include "mcb/pricing/risk.hpp"
#include "mcb/pricing/swaps.hpp"
#include "test_util.hpp"

using namespace mcb;

namespace {

struct Eur {
  std::shared_ptr<CurveEngine> engine;
  std::shared_ptr<const CurveSet> curves;
  EurSetup setup;
};

const Eur& eur(bool parCoupons = true, Interpolation interp = Interpolation::LogLinear) {
  static std::map<std::pair<bool, int>, Eur> cache;
  const auto key = std::make_pair(parCoupons, static_cast<int>(interp));
  auto it = cache.find(key);
  if (it == cache.end()) {
    const auto market = loadReferenceMarket(mcbtest::sourcePath("data/reference/eur_20121211.csv"));
    Eur e;
    QuoteMap quotes;
    e.engine = std::make_shared<CurveEngine>(makeEurEngine(market, interp, parCoupons, quotes, &e.setup));
    e.curves = e.engine->build(quotes);
    it = cache.emplace(key, std::move(e)).first;
  }
  return it->second;
}

// 10-year payer swap versus Euribor 6M at 2% on a notional of 1,000,000.
CurvePricer tenYearPayer(const Eur& e) {
  const VanillaSwap swap = makeVanillaSwap(Date(2012, 12, 11), Period{10, TimeUnit::Years}, e.setup.swap6m, 0.02, true, 1e6);
  return [swap](const CurveSet& cs) { return priceSwap(swap, cs.curve("EUR-6M"), cs.curve("EUR-OIS")).npv; };
}

}  // namespace

TEST(SwapPricingVsQuantLib, VanillaSwapsOnBothProjectionCurves) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_swaps.csv"));
  ASSERT_EQ(t.rows.size(), 20u);
  for (std::size_t i = 0; i < t.rows.size(); ++i) {
    const bool par = t.at(i, "variant") == "loglinear|par";
    const Eur& e = eur(par);
    const std::string index = t.at(i, "index");
    const SwapConventions& conv = index == "EUR-6M" ? e.setup.swap6m : e.setup.swap3m;
    VanillaSwap swap = makeVanillaSwap(Date::parse(t.at(i, "start")), Date::parse(t.at(i, "end")), conv,
                                       parseDouble(t.at(i, "fixed_rate")), t.at(i, "payer") == "1", 1e6);
    swap.floatSpread = parseDouble(t.at(i, "spread"));
    const SwapValue v = priceSwap(swap, e.curves->curve(index), e.curves->curve("EUR-OIS"));
    const std::string ctx = t.at(i, "variant") + " case " + t.at(i, "case");
    // notional 1e6: 1e-5 is a relative error of 1e-11
    EXPECT_NEAR(v.npv, parseDouble(t.at(i, "npv")), 1e-5) << ctx;
    const double sign = swap.payFixed ? 1.0 : -1.0;
    EXPECT_NEAR(-sign * v.fixedLegPv, parseDouble(t.at(i, "fixed_leg_npv")), 1e-5) << ctx;
    EXPECT_NEAR(sign * v.floatLegPv, parseDouble(t.at(i, "float_leg_npv")), 1e-5) << ctx;
    // fair rate = fixed rate that zeroes the NPV, floating spread included
    EXPECT_NEAR(v.parRate, parseDouble(t.at(i, "fair_rate")), 1e-11) << ctx;
  }
}

TEST(SwapPricingVsQuantLib, OvernightIndexedSwaps) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_ois_swaps.csv"));
  ASSERT_EQ(t.rows.size(), 6u);
  const Eur& e = eur();
  const auto& ois = e.curves->curve("EUR-OIS");
  for (std::size_t i = 0; i < t.rows.size(); ++i) {
    const Date start(2012, 12, 13);
    const OisSwap swap = makeOisSwap(start, start.add(Period::parse(t.at(i, "tenor"))), false, e.setup.ois,
                                     parseDouble(t.at(i, "fixed_rate")), t.at(i, "payer") == "1", 1e6);
    const SwapValue v = priceOis(swap, ois, ois);
    EXPECT_NEAR(v.npv, parseDouble(t.at(i, "npv")), 1e-5) << t.at(i, "tenor");
    // (QuantLib derives the fair rate from NPV/BPS, which costs it a few digits on short swaps)
    EXPECT_NEAR(v.parRate, parseDouble(t.at(i, "fair_rate")), 1e-10) << t.at(i, "tenor");
  }
}

TEST(SwapPricing, ParSwapsHaveZeroValueAndSignsAreConsistent) {
  const Eur& e = eur();
  const auto& ois = e.curves->curve("EUR-OIS");
  const auto& p6 = e.curves->curve("EUR-6M");
  const Date asOf(2012, 12, 11);
  // The 10Y swap quoted at 1.584% is worth nothing at that rate ...
  VanillaSwap par = makeVanillaSwap(asOf, Period{10, TimeUnit::Years}, e.setup.swap6m, 0.01584, true, 1e6);
  const SwapValue v = priceSwap(par, p6, ois);
  EXPECT_NEAR(v.npv, 0.0, 1e-6);
  EXPECT_NEAR(v.parRate, 0.01584, 1e-13);
  // ... paying a higher fixed rate loses money, and the receiver has the opposite position.
  VanillaSwap payer = par, receiver = par;
  payer.fixedRate = receiver.fixedRate = 0.02;
  receiver.payFixed = false;
  EXPECT_LT(priceSwap(payer, p6, ois).npv, 0.0);
  EXPECT_NEAR(priceSwap(payer, p6, ois).npv, -priceSwap(receiver, p6, ois).npv, 1e-9);
  EXPECT_NEAR(priceSwap(payer, p6, ois).npv, -(0.02 - 0.01584) * v.annuity, 1e-6);
  // The same cash flows discounted on the projection curve (pre-2008 practice) are worth something
  // else: this difference is exactly what the multicurve framework is about.
  VanillaSwap deep = par;
  deep.fixedRate = 0.04;
  EXPECT_GT(std::fabs(priceSwap(deep, p6, p6).npv - priceSwap(deep, p6, ois).npv), 1000.0);
}

TEST(SwapPricing, SeasonedOisUsesRealisedCompounding) {
  // An OIS whose single period started 100 days ago: half realised, half projected.
  const YieldCurve flat("flat", Date(2026, 11, 13), {Date(2030, 1, 1)},
                        {std::exp(-0.04 * (Date(2030, 1, 1) - Date(2026, 11, 13)) / 365.0)}, Interpolation::LogLinear);
  OisSwap swap;
  swap.notional = 1e6;
  swap.fixedRate = 0.03;
  swap.periods = {{Date(2026, 8, 5), Date(2027, 8, 5), Date(2027, 8, 9), 365.0 / 360.0}};
  swap.accruedCompounding = 1.011;  // overnight growth already realised
  const SwapValue v = priceOis(swap, flat, flat);
  const double growth = 1.011 / flat.discount(Date(2027, 8, 5));
  EXPECT_NEAR(v.floatLegPv, 1e6 * (growth - 1.0) * flat.discount(Date(2027, 8, 9)), 1e-8);
  // Cash flows paid on or before the valuation date are dropped.
  swap.periods.insert(swap.periods.begin(), {Date(2025, 8, 5), Date(2026, 8, 5), Date(2026, 8, 7), 1.0139});
  EXPECT_NEAR(priceOis(swap, flat, flat).floatLegPv, v.floatLegPv, 1e-12);
}

TEST(FraPricing, MatchesTheTextbookFormula) {
  const Eur& e = eur();
  const auto& ois = e.curves->curve("EUR-OIS");
  const auto& p6 = e.curves->curve("EUR-6M");
  const Date asOf(2012, 12, 11);
  double forward = 0.0;
  // 6x12 FRA: the curve was built from this quote (0.248%), so it has zero value at that strike.
  const Fra atMarket = makeFra(asOf, 6, e.setup.swap6m.index, 0.00248, true, 1e6);
  EXPECT_NEAR(priceFra(atMarket, p6, ois, &forward), 0.0, 1e-7);
  EXPECT_NEAR(forward, 0.00248, 1e-13);
  EXPECT_EQ(atMarket.valueDate, Date(2013, 6, 13));
  EXPECT_EQ(atMarket.indexEnd, Date(2013, 12, 13));
  const Fra cheap = makeFra(asOf, 6, e.setup.swap6m.index, 0.0010, true, 1e6);
  const double expected = 1e6 * cheap.tau * (forward - 0.0010) / (1.0 + forward * cheap.tau) * ois.discount(cheap.valueDate);
  EXPECT_NEAR(priceFra(cheap, p6, ois), expected, 1e-9);
  EXPECT_GT(expected, 0.0);
}

TEST(BondPricingVsQuantLib, AccruedYieldDurationConvexityAndZSpread) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/bonds.csv"));
  ASSERT_EQ(t.rows.size(), 6u);
  const auto& curve = eur().curves->curve("EUR-OIS");
  const Calendar target(Calendar::Kind::Target);
  for (std::size_t i = 0; i < t.rows.size(); ++i) {
    const Date settle = Date::parse(t.at(i, "settle"));
    const Bond bond = makeFixedRateBond("B" + t.at(i, "case"), Date::parse(t.at(i, "maturity")),
                                        parseDouble(t.at(i, "coupon")), 2, settle, target);
    const double clean = parseDouble(t.at(i, "clean_price"));
    const double accrued = accruedInterest(bond, settle);
    const std::string ctx = "bond " + t.at(i, "maturity");
    EXPECT_NEAR(accrued, parseDouble(t.at(i, "accrued")), 1e-12) << ctx;
    // QuantLib discounts from the actual payment dates
    const double y = yieldFromDirtyPrice(bond, settle, clean + accrued, YieldConvention::StreetCashflowDates);
    EXPECT_NEAR(y, parseDouble(t.at(i, "yield")), 1e-11) << ctx;
    const BondRisk r = bondRisk(bond, settle, y, YieldConvention::StreetCashflowDates);
    // the market convention (scheduled dates) differs only by the few days of payment delay
    EXPECT_NEAR(yieldFromDirtyPrice(bond, settle, clean + accrued, YieldConvention::Street), y, 2e-5) << ctx;
    EXPECT_NEAR(r.modifiedDuration, parseDouble(t.at(i, "modified_duration")), 1e-8) << ctx;
    EXPECT_NEAR(r.macaulayDuration, parseDouble(t.at(i, "macaulay_duration")), 1e-8) << ctx;
    EXPECT_NEAR(r.convexity, parseDouble(t.at(i, "convexity")), 1e-6) << ctx;
    EXPECT_NEAR(dirtyPriceFromCurve(bond, curve, settle) - accrued, parseDouble(t.at(i, "clean_from_curve")), 1e-9) << ctx;
    EXPECT_NEAR(zSpread(bond, curve, settle, clean + accrued), parseDouble(t.at(i, "z_spread")), 1e-10) << ctx;
  }
}

TEST(BondPricing, RoundTripsAndSpreadIdentities) {
  const auto& curve = eur().curves->curve("EUR-OIS");
  const Calendar target(Calendar::Kind::Target);
  const Date settle(2012, 12, 12);
  const Bond bond = makeFixedRateBond("X", Date(2022, 8, 15), 0.03, 2, settle, target);
  // price -> yield -> price
  for (double y : {-0.002, 0.0, 0.01, 0.05, 0.15}) {
    const double p = dirtyPriceFromYield(bond, settle, y, YieldConvention::Street);
    EXPECT_NEAR(yieldFromDirtyPrice(bond, settle, p, YieldConvention::Street), y, 1e-11);
    const double pa = dirtyPriceFromYield(bond, settle, y, YieldConvention::AnnualAct365);
    EXPECT_NEAR(yieldFromDirtyPrice(bond, settle, pa, YieldConvention::AnnualAct365), y, 1e-11);
  }
  // A bond priced exactly on the curve has zero Z-spread and zero asset-swap spread.
  const double fair = dirtyPriceFromCurve(bond, curve, settle);
  EXPECT_NEAR(zSpread(bond, curve, settle, fair), 0.0, 1e-12);
  EXPECT_NEAR(assetSwapSpread(bond, curve, settle, fair), 0.0, 1e-14);
  // A cheaper bond has positive spreads, and the Z-spread reprices it.
  const double z = zSpread(bond, curve, settle, fair - 2.0);
  EXPECT_GT(z, 0.0);
  EXPECT_NEAR(dirtyPriceFromCurve(bond, curve, settle, z), fair - 2.0, 1e-10);
  EXPECT_GT(assetSwapSpread(bond, curve, settle, fair - 2.0), 0.0);
  // DV01 is the price change for a 1bp fall in yield (to first order).
  const double y0 = yieldFromDirtyPrice(bond, settle, fair, YieldConvention::Street);
  const BondRisk r = bondRisk(bond, settle, y0, YieldConvention::Street);
  const double central = 0.5 * (dirtyPriceFromYield(bond, settle, y0 - 1e-4, YieldConvention::Street) -
                                dirtyPriceFromYield(bond, settle, y0 + 1e-4, YieldConvention::Street));
  EXPECT_NEAR(r.dv01, central, 1e-6 * r.dv01);
  // ... and convexity explains the asymmetry between a fall and a rise in yield.
  const double up = dirtyPriceFromYield(bond, settle, y0 - 1e-4, YieldConvention::Street) - fair;
  EXPECT_NEAR(up, r.dv01 + 0.5 * r.convexity * fair * 1e-8, 1e-7);  // third-order terms remain
  EXPECT_GT(r.convexity, 0.0);
  EXPECT_GT(r.macaulayDuration, r.modifiedDuration);
  // Par bond: coupon equal to yield prices at 100 on a coupon date.
  const Bond parBond = makeFixedRateBond("P", Date(2022, 12, 12), 0.04, 2, settle, target);
  EXPECT_NEAR(dirtyPriceFromYield(parBond, settle, 0.04, YieldConvention::Street), 100.0, 1e-10);
  EXPECT_NEAR(accruedInterest(parBond, settle), 0.0, 1e-14);
  EXPECT_THROW(makeFixedRateBond("old", Date(2010, 1, 1), 0.04, 2, settle, target), Error);
}

TEST(BondPricing, ScheduledBondWithExplicitCouponDates) {
  // OFZ-style bond: 182-day coupon periods, coupons given in currency per 100 face.
  const Date prev(2026, 9, 23), settle(2026, 11, 17);
  std::vector<Date> dates;
  std::vector<double> amounts;
  for (int k = 1; k <= 6; ++k) {
    dates.push_back(prev.addDays(182 * k));
    amounts.push_back(4.238);  // 8.5% * 182/365
  }
  const Bond bond = makeScheduledBond("OFZ", prev, dates, amounts, 0.085, 2);
  EXPECT_EQ(bond.maturity, prev.addDays(182 * 6));
  EXPECT_NEAR(accruedInterest(bond, settle), 4.238 * 55.0 / 182.0, 1e-12);
  const double dirty = dirtyPriceFromYield(bond, settle, 0.15, YieldConvention::AnnualAct365);
  double expected = 0.0;
  for (int k = 0; k < 6; ++k) expected += 4.238 / std::pow(1.15, (dates[static_cast<std::size_t>(k)] - settle) / 365.0);
  expected += 100.0 / std::pow(1.15, (dates.back() - settle) / 365.0);
  EXPECT_NEAR(dirty, expected, 1e-10);
  EXPECT_NEAR(yieldFromDirtyPrice(bond, settle, dirty, YieldConvention::AnnualAct365), 0.15, 1e-11);
  EXPECT_THROW(makeScheduledBond("bad", prev, dates, {1.0}, 0.085, 2), Error);
}

TEST(Risk, BumpAndRebuildAgreesWithJacobian) {
  for (auto interp : {Interpolation::LogLinear, Interpolation::LogCubic}) {
    const Eur& e = eur(true, interp);
    const auto pricer = tenYearPayer(e);
    const RiskReport bump = bumpAndRebuildRisk(*e.engine, *e.curves, pricer);
    const RiskReport jac = jacobianRisk(*e.engine, *e.curves, pricer);
    ASSERT_EQ(bump.buckets.size(), jac.buckets.size());
    ASSERT_EQ(bump.buckets.size(), 90u);  // one bucket per market quote
    double scale = 0.0;
    for (const auto& b : bump.buckets) scale = std::max(scale, std::fabs(b.delta));
    for (std::size_t i = 0; i < bump.buckets.size(); ++i) {
      EXPECT_EQ(bump.buckets[i].id, jac.buckets[i].id);
      // bump-and-rebuild is a finite 1bp move, the Jacobian is the derivative: they differ by convexity only
      EXPECT_NEAR(bump.buckets[i].delta, jac.buckets[i].delta, 2e-3 * scale)
          << interpolationName(interp) << " " << bump.buckets[i].id;
    }
    std::cout << "  [" << interpolationName(interp) << "] parallel DV01 " << bump.parallel << ", sum of buckets (bump) "
              << bump.bucketSum << ", sum of buckets (Jacobian) " << jac.bucketSum << "\n";
    // Acceptance criterion of the brief: bucket deltas add up to the parallel DV01.
    EXPECT_NEAR(bump.bucketSum, bump.parallel, 2e-3 * std::fabs(bump.parallel));
    EXPECT_NEAR(jac.bucketSum, bump.parallel, 2e-3 * std::fabs(bump.parallel));
  }
}

TEST(Risk, SmallBumpsConvergeToTheJacobian) {
  const Eur& e = eur();
  const auto pricer = tenYearPayer(e);
  const RiskReport tiny = bumpAndRebuildRisk(*e.engine, *e.curves, pricer, 1e-7);  // scaled back to 1bp
  const RiskReport jac = jacobianRisk(*e.engine, *e.curves, pricer);
  for (std::size_t i = 0; i < tiny.buckets.size(); ++i)
    EXPECT_NEAR(tiny.buckets[i].delta, jac.buckets[i].delta, 2e-4) << tiny.buckets[i].id;  // on deltas of up to ~950
}

TEST(RiskVsQuantLib, BumpAndRebuildDeltasMatch) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/eur_deltas.csv"));
  const Eur& e = eur();
  const RiskReport bump = bumpAndRebuildRisk(*e.engine, *e.curves, tenYearPayer(e));
  std::map<std::string, double> ours;
  for (const auto& b : bump.buckets) ours[b.id] = b.delta;
  ASSERT_GE(t.rows.size(), 6u);
  for (std::size_t i = 0; i < t.rows.size(); ++i)
    EXPECT_NEAR(ours.at(t.at(i, "quote_id")), parseDouble(t.at(i, "delta_npv_1bp")), 1e-6) << t.at(i, "quote_id");
}

TEST(Risk, DeltasHaveTheExpectedStructure) {
  const Eur& e = eur();
  const RiskReport r = bumpAndRebuildRisk(*e.engine, *e.curves, tenYearPayer(e));
  std::map<std::string, double> d;
  for (const auto& b : r.buckets) d[b.id] = b.delta;
  // A payer swap gains when its own par rate rises: about annuity x 1bp x notional.
  EXPECT_GT(d["EUR6M_SWAP_10Y"], 900.0);
  EXPECT_LT(d["EUR6M_SWAP_10Y"], 1000.0);
  // With a local interpolation, other 6M pillars do not matter ...
  EXPECT_NEAR(d["EUR6M_SWAP_9Y"], 0.0, 1e-6);
  EXPECT_NEAR(d["EUR6M_SWAP_12Y"], 0.0, 1e-6);
  // ... the 3M curve is irrelevant for a 6M swap ...
  EXPECT_NEAR(d["EUR3M_DEPO"], 0.0, 1e-9);
  EXPECT_NEAR(d["EUR3S6S_10Y"], 0.0, 1e-9);
  // ... and discounting risk is small but not zero (the swap is off-market).
  EXPECT_GT(std::fabs(d["EONIA_10Y"]), 1.0);
  EXPECT_LT(std::fabs(d["EONIA_10Y"]), 20.0);
}

TEST(Risk, BasisQuotesMoveTheThreeMonthCurve) {
  // A 3M swap depends on the 6M swap quote (+) and on the 3s6s basis quote (-).
  const Eur& e = eur();
  const VanillaSwap swap = makeVanillaSwap(Date(2012, 12, 11), Period{10, TimeUnit::Years}, e.setup.swap3m, 0.015, true, 1e6);
  const CurvePricer pricer = [swap](const CurveSet& cs) { return priceSwap(swap, cs.curve("EUR-3M"), cs.curve("EUR-OIS")).npv; };
  const RiskReport bump = bumpAndRebuildRisk(*e.engine, *e.curves, pricer);
  const RiskReport jac = jacobianRisk(*e.engine, *e.curves, pricer);
  std::map<std::string, double> d, dj;
  for (const auto& b : bump.buckets) d[b.id] = b.delta;
  for (const auto& b : jac.buckets) dj[b.id] = b.delta;
  EXPECT_GT(d["EUR6M_SWAP_10Y"], 900.0);
  EXPECT_NEAR(d["EUR3S6S_10Y"], -d["EUR6M_SWAP_10Y"], 0.5);
  EXPECT_NEAR(dj["EUR3S6S_10Y"], -dj["EUR6M_SWAP_10Y"], 1e-6);
  EXPECT_NEAR(dj["EUR6M_SWAP_10Y"], d["EUR6M_SWAP_10Y"], 0.5);
}
