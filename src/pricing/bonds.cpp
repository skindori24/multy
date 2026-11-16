#include "mcb/pricing/bonds.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/core/schedule.hpp"
#include "mcb/math/solver.hpp"

namespace mcb {
namespace {

struct TimedFlow {
  double time;    // in the units required by the yield convention
  double amount;
};

bool isStreet(YieldConvention c) { return c != YieldConvention::AnnualAct365; }

// ACT/ACT (ICMA) time from d1 to d2 in units of coupon periods, measured
// against the coupon's own accrual period; days beyond its end are measured
// against the following notional period.
double periodsBetween(const BondCoupon& c, Date d1, Date d2, int months) {
  const double len = static_cast<double>(c.end - c.start);
  if (d2 <= c.end) return (d2 - d1) / len;
  return (c.end - d1) / len + static_cast<double>(d2 - c.end) / static_cast<double>(c.end.addMonths(months) - c.end);
}

// Cash flows after `settle` with their compounding times.
//  Street:       time in coupon periods = w + k, where w is the fraction of the
//                current coupon period remaining at settlement.
//  StreetCashflowDates: time accumulated flow by flow between actual payment
//                dates (the stepwise scheme QuantLib uses), so a payment
//                rolled past a weekend is discounted for the extra days.
//  AnnualAct365: time in years = calendar days / 365.
std::vector<TimedFlow> timedFlows(const Bond& bond, Date settle, YieldConvention convention) {
  MCB_REQUIRE(bond.maturity > settle, "bond " << bond.id << " has matured before settlement " << settle.iso());
  std::vector<TimedFlow> flows;
  const int months = 12 / bond.frequency;
  int k = 0;
  double w = -1.0, cumulative = 0.0;
  Date last = settle;
  for (const auto& c : bond.coupons) {
    if (c.end <= settle) continue;  // already paid (ex-coupon effects are ignored)
    double t;
    if (convention == YieldConvention::Street) {
      if (w < 0.0) {
        // Fraction of the current coupon period still to run. (If the bond has
        // not started accruing yet, measure against a nominal period length.)
        const double periodDays = c.start <= settle ? static_cast<double>(c.end - c.start) : 365.0 / bond.frequency;
        w = static_cast<double>(c.end - settle) / periodDays;
      }
      t = w + k;
    } else if (convention == YieldConvention::StreetCashflowDates) {
      cumulative += last != c.start ? periodsBetween(c, c.start, c.pay, months) - periodsBetween(c, c.start, last, months)
                                    : periodsBetween(c, last, c.pay, months);
      last = c.pay;
      t = cumulative;
    } else {
      t = (c.end - settle) / 365.0;
    }
    flows.push_back({t, c.amount});
    ++k;
  }
  const double tRedemption = isStreet(convention)
                                 ? (flows.empty() ? (bond.maturity - settle) / (365.0 / bond.frequency) : flows.back().time)
                                 : (bond.maturity - settle) / 365.0;
  flows.push_back({tRedemption, bond.redemption});
  return flows;
}

double discountFactor(double yield, double t, const Bond& bond, YieldConvention convention) {
  if (isStreet(convention)) return std::pow(1.0 + yield / bond.frequency, -t);
  return std::pow(1.0 + yield, -t);
}

}  // namespace

Bond makeFixedRateBond(const std::string& id, Date maturity, double couponRate, int frequency, Date settle,
                       const Calendar& paymentCalendar) {
  MCB_REQUIRE(frequency >= 1 && 12 % frequency == 0, "bond " << id << ": unsupported coupon frequency");
  MCB_REQUIRE(maturity > settle, "bond " << id << " matures on or before settlement");
  Bond b;
  b.id = id;
  b.maturity = maturity;
  b.couponRate = couponRate;
  b.frequency = frequency;
  b.redemptionPay = paymentCalendar.adjust(maturity, BusinessDayConvention::Following);
  // Walk back from maturity until the coupon period containing settlement is covered.
  const int months = 12 / frequency;
  const bool eom = maturity.isEndOfMonth();
  std::vector<Date> dates{maturity};
  for (int k = 1;; ++k) {
    Date d = maturity.addMonths(-k * months);
    if (eom) d = d.endOfMonth();  // month-end maturities pay on month ends
    dates.push_back(d);
    if (d <= settle) break;
  }
  std::reverse(dates.begin(), dates.end());
  for (std::size_t i = 0; i + 1 < dates.size(); ++i) {
    BondCoupon c;
    c.start = dates[i];
    c.end = dates[i + 1];
    c.pay = paymentCalendar.adjust(c.end, BusinessDayConvention::Following);
    c.amount = 100.0 * couponRate / frequency;
    b.coupons.push_back(c);
  }
  return b;
}

Bond makeScheduledBond(const std::string& id, Date previousCouponDate, const std::vector<Date>& couponDates,
                       const std::vector<double>& couponAmounts, double couponRate, int frequency) {
  MCB_REQUIRE(!couponDates.empty() && couponDates.size() == couponAmounts.size(),
              "bond " << id << ": coupon dates and amounts do not match");
  Bond b;
  b.id = id;
  b.couponRate = couponRate;
  b.frequency = frequency;
  b.maturity = couponDates.back();
  b.redemptionPay = b.maturity;
  Date prev = previousCouponDate;
  for (std::size_t i = 0; i < couponDates.size(); ++i) {
    MCB_REQUIRE(couponDates[i] > prev, "bond " << id << ": coupon dates must increase");
    b.coupons.push_back({prev, couponDates[i], couponDates[i], couponAmounts[i]});
    prev = couponDates[i];
  }
  return b;
}

double accruedInterest(const Bond& bond, Date settle) {
  for (const auto& c : bond.coupons)
    if (c.start <= settle && settle < c.end)
      return c.amount * static_cast<double>(settle - c.start) / static_cast<double>(c.end - c.start);
  return 0.0;
}

double dirtyPriceFromCurve(const Bond& bond, const DiscountCurve& curve, Date settle, double zSpread) {
  MCB_REQUIRE(bond.maturity > settle, "bond " << bond.id << " has matured before settlement " << settle.iso());
  const double logDfSettle = curve.logDiscountT(curve.time(settle));
  auto df = [&](Date d) {
    return std::exp(curve.logDiscountT(curve.time(d)) - logDfSettle - zSpread * (d - settle) / 365.0);
  };
  double pv = 0.0;
  for (const auto& c : bond.coupons)
    if (c.end > settle) pv += c.amount * df(c.pay);
  return pv + bond.redemption * df(bond.redemptionPay);
}

double dirtyPriceFromYield(const Bond& bond, Date settle, double yield, YieldConvention convention) {
  double pv = 0.0;
  for (const auto& f : timedFlows(bond, settle, convention)) pv += f.amount * discountFactor(yield, f.time, bond, convention);
  return pv;
}

double yieldFromDirtyPrice(const Bond& bond, Date settle, double dirtyPrice, YieldConvention convention) {
  MCB_REQUIRE(dirtyPrice > 0.0, "bond " << bond.id << ": price must be positive");
  const auto flows = timedFlows(bond, settle, convention);
  auto f = [&](double y) {
    double pv = 0.0;
    for (const auto& fl : flows) pv += fl.amount * discountFactor(y, fl.time, bond, convention);
    return pv - dirtyPrice;
  };
  const RootResult r = findRoot(f, bond.couponRate > 0 ? bond.couponRate : 0.03, 1e-3, -0.9, 10.0, 1e-14);
  MCB_REQUIRE(r.converged, "bond " << bond.id << ": yield solver did not converge");
  return r.x;
}

BondRisk bondRisk(const Bond& bond, Date settle, double yield, YieldConvention convention) {
  const auto flows = timedFlows(bond, settle, convention);
  const double f = isStreet(convention) ? bond.frequency : 1.0;  // compounding periods per year
  const double base = 1.0 + yield / f;
  double p = 0.0, sumT = 0.0, sumTT = 0.0;
  for (const auto& fl : flows) {
    const double pv = fl.amount * std::pow(base, -fl.time);
    p += pv;
    sumT += fl.time * pv;                      // time in compounding periods
    sumTT += fl.time * (fl.time + 1.0) * pv;
  }
  BondRisk r;
  r.macaulayDuration = sumT / p / f;                 // years
  r.modifiedDuration = r.macaulayDuration / base;    // -dP/dy / P
  r.convexity = sumTT / p / (f * f * base * base);   // d2P/dy2 / P
  r.dv01 = r.modifiedDuration * p * 1e-4;
  return r;
}

double zSpread(const Bond& bond, const DiscountCurve& curve, Date settle, double dirtyPrice) {
  auto f = [&](double s) { return dirtyPriceFromCurve(bond, curve, settle, s) - dirtyPrice; };
  const RootResult r = findRoot(f, 0.0, 1e-3, -0.5, 5.0, 1e-14);
  MCB_REQUIRE(r.converged, "bond " << bond.id << ": z-spread solver did not converge");
  return r.x;
}

double assetSwapSpread(const Bond& bond, const DiscountCurve& swapCurve, Date settle, double dirtyPrice) {
  const double pvOnCurve = dirtyPriceFromCurve(bond, swapCurve, settle);
  // Annuity of the floating leg: annual ACT/360 periods stepping back from maturity.
  const double dfSettle = swapCurve.discount(settle);
  double annuity = 0.0;
  Date end = bond.maturity;
  for (int k = 1; end > settle; ++k) {
    Date start = bond.maturity.addYears(-k);
    if (start < settle) start = settle;
    annuity += yearFraction(DayCount::Act360, start, end) * swapCurve.discount(end) / dfSettle;
    end = start;
  }
  return (pvOnCurve - dirtyPrice) / (100.0 * annuity);
}

}  // namespace mcb
