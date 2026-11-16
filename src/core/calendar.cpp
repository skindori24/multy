#include "mcb/core/calendar.hpp"

#include "mcb/core/error.hpp"

namespace mcb {
namespace {

using W = Weekday;

struct DayInfo {
  int y, m, d, doy;
  W w;
};

DayInfo info(Date date) {
  const auto c = date.ymd();
  return {c.year, c.month, c.day, date.dayOfYear(), date.weekday()};
}

// ----- TARGET (euro area) ----------------------------------------------------
bool targetBusinessDay(const DayInfo& x) {
  const int em = easterMondayDayOfYear(x.y);
  if (x.w >= W::Sat) return false;
  if (x.d == 1 && x.m == 1) return false;                              // New Year's Day
  if (x.doy == em - 3 && x.y >= 2000) return false;                    // Good Friday
  if (x.doy == em && x.y >= 2000) return false;                        // Easter Monday
  if (x.d == 1 && x.m == 5 && x.y >= 2000) return false;               // Labour Day
  if (x.d == 25 && x.m == 12) return false;                            // Christmas
  if (x.d == 26 && x.m == 12 && x.y >= 2000) return false;             // Day of Goodwill
  if (x.d == 31 && x.m == 12 && (x.y == 1998 || x.y == 1999 || x.y == 2001)) return false;
  return true;
}

// ----- United States ---------------------------------------------------------
bool isWashingtonBirthday(const DayInfo& x) {
  if (x.y >= 1971) return x.d >= 15 && x.d <= 21 && x.w == W::Mon && x.m == 2;  // third Monday
  return (x.d == 22 || (x.d == 23 && x.w == W::Mon) || (x.d == 21 && x.w == W::Fri)) && x.m == 2;
}
bool isMemorialDay(const DayInfo& x) {
  if (x.y >= 1971) return x.d >= 25 && x.w == W::Mon && x.m == 5;  // last Monday in May
  return (x.d == 30 || (x.d == 31 && x.w == W::Mon) || (x.d == 29 && x.w == W::Fri)) && x.m == 5;
}
bool isLaborDay(const DayInfo& x) { return x.d <= 7 && x.w == W::Mon && x.m == 9; }
bool isColumbusDay(const DayInfo& x) {
  return x.d >= 8 && x.d <= 14 && x.w == W::Mon && x.m == 10 && x.y >= 1971;
}
bool isVeteransDay(const DayInfo& x, bool moveSaturdayToFriday) {
  if (x.y <= 1970 || x.y >= 1978) {
    return (x.d == 11 || (x.d == 12 && x.w == W::Mon) ||
            (moveSaturdayToFriday && x.d == 10 && x.w == W::Fri)) &&
           x.m == 11;
  }
  return x.d >= 22 && x.d <= 28 && x.w == W::Mon && x.m == 10;  // 1971-1977: fourth Monday in October
}
bool isMartinLutherKing(const DayInfo& x) {
  return x.d >= 15 && x.d <= 21 && x.w == W::Mon && x.m == 1 && x.y >= 1983;
}
bool isJuneteenth(const DayInfo& x) {
  return (x.d == 19 || (x.d == 20 && x.w == W::Mon) || (x.d == 18 && x.w == W::Fri)) && x.m == 6 &&
         x.y >= 2022;
}
bool isIndependenceDay(const DayInfo& x) {
  return (x.d == 4 || (x.d == 5 && x.w == W::Mon) || (x.d == 3 && x.w == W::Fri)) && x.m == 7;
}
bool isThanksgiving(const DayInfo& x) { return x.d >= 22 && x.d <= 28 && x.w == W::Thu && x.m == 11; }
bool isChristmasObserved(const DayInfo& x) {
  return (x.d == 25 || (x.d == 26 && x.w == W::Mon) || (x.d == 24 && x.w == W::Fri)) && x.m == 12;
}

bool usSettlementBusinessDay(const DayInfo& x) {
  if (x.w >= W::Sat) return false;
  if ((x.d == 1 || (x.d == 2 && x.w == W::Mon)) && x.m == 1) return false;  // New Year's Day
  if (x.d == 31 && x.w == W::Fri && x.m == 12) return false;                // ... observed on Friday
  if (isMartinLutherKing(x) || isWashingtonBirthday(x) || isMemorialDay(x) || isJuneteenth(x) ||
      isIndependenceDay(x) || isLaborDay(x) || isColumbusDay(x) || isVeteransDay(x, true) ||
      isThanksgiving(x) || isChristmasObserved(x))
    return false;
  return true;
}

bool usGovernmentBondBusinessDay(const DayInfo& x) {
  const int em = easterMondayDayOfYear(x.y);
  if (x.w >= W::Sat) return false;
  if ((x.d == 1 || (x.d == 2 && x.w == W::Mon)) && x.m == 1) return false;  // New Year's Day
  if (isMartinLutherKing(x) || isWashingtonBirthday(x)) return false;
  // Good Friday. Since 1996 the bond market stays open (early close) when
  // Good Friday coincides with the employment report on the first Friday of
  // the month.
  if (x.doy == em - 3 && (x.y < 1996 || x.d > 7)) return false;
  if (isMemorialDay(x) || isJuneteenth(x) || isIndependenceDay(x) || isLaborDay(x) || isColumbusDay(x) ||
      isVeteransDay(x, false) || isThanksgiving(x) || isChristmasObserved(x))
    return false;
  // Special closings.
  if (x.y == 2018 && x.m == 12 && x.d == 5) return false;   // President G.H.W. Bush's funeral
  if (x.y == 2012 && x.m == 10 && x.d == 30) return false;  // Hurricane Sandy
  if (x.y == 2004 && x.m == 6 && x.d == 11) return false;   // President Reagan's funeral
  return true;
}

bool usSofrBusinessDay(const DayInfo& x) {
  // SOFR has never been published on a Good Friday, including the years in
  // which the bond market was open for the employment report.
  if (x.doy == easterMondayDayOfYear(x.y) - 3) return false;
  return usGovernmentBondBusinessDay(x);
}

}  // namespace

int easterMondayDayOfYear(int year) {
  // Anonymous Gregorian algorithm (Meeus/Jones/Butcher) for Easter Sunday.
  const int a = year % 19, b = year / 100, c = year % 100;
  const int d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
  const int h = (19 * a + b - d - g + 15) % 30;
  const int i = c / 4, k = c % 4;
  const int l = (32 + 2 * e + 2 * i - h - k) % 7;
  const int m = (a + 11 * h + 22 * l) / 451;
  const int month = (h + l - 7 * m + 114) / 31;
  const int day = (h + l - 7 * m + 114) % 31 + 1;
  return Date(year, month, day).dayOfYear() + 1;
}

Date immDate(int year, int month) {
  const Date first(year, month, 1);
  const int offset = (static_cast<int>(Weekday::Wed) - static_cast<int>(first.weekday()) + 7) % 7;
  return first.addDays(offset + 14);
}

Date nextImmDate(Date d) {
  for (int y = d.year();; ++y) {
    for (int m = 3; m <= 12; m += 3) {
      const Date cand = immDate(y, m);
      if (cand > d) return cand;
    }
  }
}

std::string Calendar::name() const {
  switch (kind_) {
    case Kind::Null: return "Null";
    case Kind::WeekendsOnly: return "WeekendsOnly";
    case Kind::Target: return "TARGET";
    case Kind::UsSettlement: return "US settlement";
    case Kind::UsGovernmentBond: return "US government bond market";
    case Kind::UsSofr: return "US SOFR fixing";
  }
  return "?";
}

bool Calendar::isBusinessDay(Date date) const {
  if (extraHolidays_ && extraHolidays_->count(date)) return false;
  switch (kind_) {
    case Kind::Null: return true;
    case Kind::WeekendsOnly: return !date.isWeekend();
    case Kind::Target: return targetBusinessDay(info(date));
    case Kind::UsSettlement: return usSettlementBusinessDay(info(date));
    case Kind::UsGovernmentBond: return usGovernmentBondBusinessDay(info(date));
    case Kind::UsSofr: return usSofrBusinessDay(info(date));
  }
  return true;
}

Date Calendar::adjust(Date d, BusinessDayConvention c) const {
  MCB_REQUIRE(!d.isNull(), "cannot adjust a null date");
  using C = BusinessDayConvention;
  if (c == C::Unadjusted) return d;
  Date d1 = d;
  if (c == C::Following || c == C::ModifiedFollowing) {
    while (!isBusinessDay(d1)) d1 = d1.addDays(1);
    if (c == C::ModifiedFollowing && d1.ymd().month != d.ymd().month) return adjust(d, C::Preceding);
  } else {
    while (!isBusinessDay(d1)) d1 = d1.addDays(-1);
    if (c == C::ModifiedPreceding && d1.ymd().month != d.ymd().month) return adjust(d, C::Following);
  }
  return d1;
}

Date Calendar::advance(Date d, int n, TimeUnit unit, BusinessDayConvention c, bool endOfMonth) const {
  MCB_REQUIRE(!d.isNull(), "cannot advance a null date");
  if (n == 0) return adjust(d, c);
  if (unit == TimeUnit::Days) {
    Date d1 = d;
    const int step = n > 0 ? 1 : -1;
    for (int left = n > 0 ? n : -n; left > 0; --left) {
      d1 = d1.addDays(step);
      while (!isBusinessDay(d1)) d1 = d1.addDays(step);
    }
    return d1;
  }
  if (unit == TimeUnit::Weeks) return adjust(d.addDays(7 * n), c);
  const Date d1 = unit == TimeUnit::Months ? d.addMonths(n) : d.addYears(n);
  if (endOfMonth) {
    if (c == BusinessDayConvention::Unadjusted) {
      if (d.isEndOfMonth()) return d1.endOfMonth();          // last calendar day -> last calendar day
    } else if (isEndOfMonth(d)) {
      return this->endOfMonth(d1);                           // last business day -> last business day
    }
  }
  return adjust(d1, c);
}

bool Calendar::isEndOfMonth(Date d) const {
  return d.ymd().month != adjust(d.addDays(1), BusinessDayConvention::Following).ymd().month;
}

Date Calendar::endOfMonth(Date d) const { return adjust(d.endOfMonth(), BusinessDayConvention::Preceding); }

int Calendar::businessDaysBetween(Date from, Date to) const {
  if (from > to) return -businessDaysBetween(to, from);
  int n = 0;
  for (Date d = from.addDays(1); d <= to; d = d.addDays(1))
    if (isBusinessDay(d)) ++n;
  return n;
}

void Calendar::addHoliday(Date d) {
  if (!extraHolidays_) extraHolidays_ = std::make_shared<std::set<Date>>();
  extraHolidays_->insert(d);
}

}  // namespace mcb
