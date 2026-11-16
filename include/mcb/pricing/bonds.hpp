#pragma once
// Fixed-rate bonds: pricing off a curve, yields, risk measures and spreads.
//
// A bond is a list of dated cash flows.  Its "model" price is the sum of the
// flows discounted on a curve; comparing that with the market price tells you
// whether the bond is rich or cheap relative to the curve, most conveniently
// expressed as a Z-spread: the constant spread that must be added to the
// curve's zero rates for the model price to equal the market price.
//
// Prices are per 100 of face value.  "Dirty" price includes accrued interest,
// "clean" price (the quoted one) does not.
#include <string>
#include <vector>

#include "mcb/core/calendar.hpp"
#include "mcb/core/date.hpp"
#include "mcb/curves/yield_curve.hpp"

namespace mcb {

struct BondCoupon {
  Date start, end;      // accrual period
  Date pay;             // payment date (end rolled to a business day)
  double amount = 0.0;  // coupon amount per 100 face
};

struct Bond {
  std::string id;
  std::string name;
  Date maturity;
  double couponRate = 0.0;  // annual rate, decimal
  int frequency = 2;        // coupons per year
  double redemption = 100.0;
  Date redemptionPay;
  /// Coupons whose accrual period ends after the settlement date used to build
  /// the bond (so the first one may have started in the past).
  std::vector<BondCoupon> coupons;
};

enum class YieldConvention {
  /// Compounding at the coupon frequency, time counted in coupon periods from
  /// the *scheduled* coupon dates.  This is the market ("street") convention
  /// for US Treasuries: a coupon due on a Sunday is treated as paid that day.
  Street,
  /// As Street, but each flow is discounted from its actual payment date
  /// (rolled to a business day).  This is what QuantLib computes.
  StreetCashflowDates,
  /// Annual compounding, time = days/365 ("effective yield", used by MOEX for OFZ).
  AnnualAct365
};

/// Regular fixed-rate bond. Coupon dates are generated backward from maturity
/// (unadjusted); payments falling on a holiday are made the next business day.
Bond makeFixedRateBond(const std::string& id, Date maturity, double couponRate, int frequency, Date settle,
                       const Calendar& paymentCalendar);

/// Bond with an explicit schedule: coupon i accrues over
/// (couponDates[i-1], couponDates[i]] with previousCouponDate preceding the first.
Bond makeScheduledBond(const std::string& id, Date previousCouponDate, const std::vector<Date>& couponDates,
                       const std::vector<double>& couponAmounts, double couponRate, int frequency);

/// Interest accrued from the last coupon date to `settle` (proportional to
/// elapsed days in the coupon period: ACT/ACT ICMA).
double accruedInterest(const Bond& bond, Date settle);

/// Dirty price implied by a curve for settlement on `settle`, with an optional
/// continuously compounded Z-spread (decimal) on top of the curve.
double dirtyPriceFromCurve(const Bond& bond, const DiscountCurve& curve, Date settle, double zSpread = 0.0);

double dirtyPriceFromYield(const Bond& bond, Date settle, double yield, YieldConvention convention);
double yieldFromDirtyPrice(const Bond& bond, Date settle, double dirtyPrice, YieldConvention convention);

struct BondRisk {
  double macaulayDuration = 0.0;  // years
  double modifiedDuration = 0.0;  // -dP/dy / P
  double convexity = 0.0;         // d2P/dy2 / P
  double dv01 = 0.0;              // price change (per 100 face) for a 1bp fall in yield
};
BondRisk bondRisk(const Bond& bond, Date settle, double yield, YieldConvention convention);

/// Z-spread (continuously compounded, ACT/365) that reprices the bond on the curve.
double zSpread(const Bond& bond, const DiscountCurve& curve, Date settle, double dirtyPrice);

/// Par asset-swap spread against a swap curve: the spread over the floating
/// rate received by an investor who buys the bond at par and swaps its
/// coupons, with annual ACT/360 floating payments.
double assetSwapSpread(const Bond& bond, const DiscountCurve& swapCurve, Date settle, double dirtyPrice);

}  // namespace mcb
