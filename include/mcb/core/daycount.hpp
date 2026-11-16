#pragma once
// Day-count conventions: how a pair of dates becomes a year fraction.
//
// The same 4% rate pays different cash amounts under ACT/360 and 30/360, so
// using the wrong convention is the classic reason a bootstrapped curve fails
// to reprice its inputs.
#include <string>
#include <string_view>

#include "mcb/core/date.hpp"

namespace mcb {

enum class DayCount {
  Act360,         // actual days / 360        (money markets, SOFR, Euribor)
  Act365Fixed,    // actual days / 365        (curve time axis, GBP, RUB bonds)
  Thirty360,      // 30/360 "bond basis"      (EUR/USD swap fixed legs)
  Thirty360E,     // 30E/360 "eurobond basis"
  ActActIsda,     // actual/actual, split by calendar year
  ActActIcma      // actual/actual per coupon period (government bonds)
};

/// Number of days between two dates under the convention (30/360 variants
/// count 30-day months; the others count actual days).
int dayCount(DayCount dc, Date d1, Date d2);

/// Year fraction between d1 and d2.
/// For ActActIcma the reference period [refStart, refEnd] (the regular coupon
/// period containing the dates) must be supplied.
double yearFraction(DayCount dc, Date d1, Date d2, Date refStart = Date(), Date refEnd = Date());

std::string dayCountName(DayCount dc);
DayCount parseDayCount(std::string_view name);

}  // namespace mcb
