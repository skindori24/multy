#pragma once
// Market conventions: how a quote such as "EUR 10Y swap vs Euribor 6M" turns
// into dated cash flows.
//
// Getting these details right (spot lag, calendars, roll conventions, day
// counts, payment lags) is most of the work in curve building; the rules
// here reproduce QuantLib's helpers so the engine can be validated against it.
#include <memory>
#include <string>

#include "mcb/core/calendar.hpp"
#include "mcb/core/daycount.hpp"
#include "mcb/core/schedule.hpp"
#include "mcb/curves/rate_helpers.hpp"
#include "mcb/pricing/swaps.hpp"

namespace mcb {

/// A term-rate index such as Euribor 6M.
struct IborIndex {
  std::string name;
  Period tenor{6, TimeUnit::Months};
  int fixingDays = 2;
  Calendar calendar;
  BusinessDayConvention convention = BusinessDayConvention::ModifiedFollowing;
  bool endOfMonth = true;
  DayCount dayCount = DayCount::Act360;

  Date valueDate(Date fixing) const { return calendar.advance(fixing, fixingDays, TimeUnit::Days); }
  Date fixingDate(Date value) const { return calendar.advance(value, -fixingDays, TimeUnit::Days); }
  Date maturityDate(Date value) const { return calendar.advance(value, tenor, convention, endOfMonth); }

  /// Euribor: T+2, TARGET, modified following, end-of-month, ACT/360.
  static IborIndex euribor(Period tenor);
};

/// Spot (settlement) date: `days` business days after the as-of date.
Date spotDate(Date asOf, const Calendar& calendar, int days);

struct OisConventions {
  int settlementDays = 2;
  Calendar calendar;                                   // accrual and fixing calendar
  DayCount dayCount = DayCount::Act360;
  Period paymentTenor{1, TimeUnit::Years};             // both legs pay annually
  int paymentLag = 0;                                  // business days after the accrual end
  BusinessDayConvention paymentConvention = BusinessDayConvention::Following;
  /// Roll month-end starts to month-end maturities (QuantLib's default for OIS).
  bool endOfMonthRule = true;

  static OisConventions eonia();  // EUR overnight (EONIA/ESTR): TARGET, no payment lag
  /// USD SOFR: US government securities business days, 2-day payment lag and no
  /// end-of-month roll (checked against the schedules published by Eris).
  static OisConventions sofr();
};

struct SwapConventions {
  int settlementDays = 2;
  Calendar calendar;
  Period fixedTenor{1, TimeUnit::Years};
  BusinessDayConvention fixedConvention = BusinessDayConvention::Unadjusted;
  DayCount fixedDayCount = DayCount::Thirty360;
  IborIndex index;
  /// true  = "par coupon": the index period is taken to end on the accrual end date
  /// false = "indexed coupon": the index period is the index's own value-to-maturity period
  bool parCoupons = true;

  /// EUR swap: annual 30/360 fixed versus Euribor of the given tenor.
  static SwapConventions euribor(Period indexTenor);
};

// ---- instrument builders ----------------------------------------------------
/// Spot-starting OIS of the given tenor (e.g. 1W, 18M, 10Y).
OisSwap makeOisSwap(Date asOf, Period tenor, const OisConventions& c, double fixedRate = 0.0, bool payFixed = true,
                    double notional = 1.0);
/// OIS between two given accrual dates.
OisSwap makeOisSwap(Date start, Date end, bool endOfMonth, const OisConventions& c, double fixedRate = 0.0,
                    bool payFixed = true, double notional = 1.0);
/// Spot-starting fixed-vs-Ibor swap.
VanillaSwap makeVanillaSwap(Date asOf, Period tenor, const SwapConventions& c, double fixedRate = 0.0,
                            bool payFixed = true, double notional = 1.0);
VanillaSwap makeVanillaSwap(Date start, Date end, const SwapConventions& c, double fixedRate = 0.0,
                            bool payFixed = true, double notional = 1.0);
/// m x (m + index tenor) FRA, e.g. monthsToStart = 3 on Euribor 6M is the 3x9.
Fra makeFra(Date asOf, int monthsToStart, const IborIndex& index, double strike = 0.0, bool payFixed = true,
            double notional = 1.0);

// ---- rate-helper builders (for bootstrapping) -------------------------------
std::shared_ptr<RateHelper> depositHelper(Date asOf, Period tenor, int fixingDays, const Calendar& calendar,
                                          BusinessDayConvention convention, bool endOfMonth, DayCount dc);
std::shared_ptr<RateHelper> fraHelper(Date asOf, int monthsToStart, const IborIndex& index);
std::shared_ptr<RateHelper> futuresHelper(Date immStart, const IborIndex& index, double convexityAdjustment = 0.0);
std::shared_ptr<RateHelper> oisHelper(Date asOf, Period tenor, const OisConventions& c);
std::shared_ptr<RateHelper> datedOisHelper(Date start, Date end, const OisConventions& c);
std::shared_ptr<RateHelper> swapHelper(Date asOf, Period tenor, const SwapConventions& c);
/// Par bond settling on `settle`, paying `frequency` coupons per year until settle + tenor.
std::shared_ptr<RateHelper> parBondHelper(Date settle, Period tenor, int frequency);
/// Zero-coupon bill quoted as a simple ACT/365 yield from settle to settle + tenor.
std::shared_ptr<RateHelper> billHelper(Date settle, Period tenor);

OisHelper::Period toHelperPeriod(const OisPeriod& p);

}  // namespace mcb
