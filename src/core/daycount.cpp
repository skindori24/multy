#include "mcb/core/daycount.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

int dayCount(DayCount dc, Date d1, Date d2) {
  switch (dc) {
    case DayCount::Thirty360: {
      // ISMA / bond basis: a 31st counts as the 30th; the end date only when
      // the start date is the 30th (or 31st).
      const auto a = d1.ymd(), b = d2.ymd();
      int dd1 = a.day, dd2 = b.day;
      if (dd1 == 31) dd1 = 30;
      if (dd2 == 31 && dd1 == 30) dd2 = 30;
      return 360 * (b.year - a.year) + 30 * (b.month - a.month) + (dd2 - dd1);
    }
    case DayCount::Thirty360E: {
      const auto a = d1.ymd(), b = d2.ymd();
      const int dd1 = std::min(a.day, 30), dd2 = std::min(b.day, 30);
      return 360 * (b.year - a.year) + 30 * (b.month - a.month) + (dd2 - dd1);
    }
    default:
      return d2 - d1;
  }
}

double yearFraction(DayCount dc, Date d1, Date d2, Date refStart, Date refEnd) {
  switch (dc) {
    case DayCount::Act360: return (d2 - d1) / 360.0;
    case DayCount::Act365Fixed: return (d2 - d1) / 365.0;
    case DayCount::Thirty360:
    case DayCount::Thirty360E: return dayCount(dc, d1, d2) / 360.0;
    case DayCount::ActActIsda: {
      if (d1 == d2) return 0.0;
      if (d1 > d2) return -yearFraction(dc, d2, d1);
      const int y1 = d1.year(), y2 = d2.year();
      const double dib1 = Date::isLeap(y1) ? 366.0 : 365.0;
      const double dib2 = Date::isLeap(y2) ? 366.0 : 365.0;
      double sum = y2 - y1 - 1;
      sum += (Date(y1 + 1, 1, 1) - d1) / dib1;
      sum += (d2 - Date(y2, 1, 1)) / dib2;
      return sum;
    }
    case DayCount::ActActIcma: {
      MCB_REQUIRE(!refStart.isNull() && !refEnd.isNull() && refEnd > refStart,
                  "ACT/ACT ICMA needs a reference coupon period");
      // The reference period is one regular coupon period; its length in
      // years is rounded to the nearest whole number of months.
      const int months = std::max(1, static_cast<int>(std::lround(12.0 * (refEnd - refStart) / 365.0)));
      return (static_cast<double>(d2 - d1) / (refEnd - refStart)) * (months / 12.0);
    }
  }
  return 0.0;
}

std::string dayCountName(DayCount dc) {
  switch (dc) {
    case DayCount::Act360: return "ACT/360";
    case DayCount::Act365Fixed: return "ACT/365F";
    case DayCount::Thirty360: return "30/360";
    case DayCount::Thirty360E: return "30E/360";
    case DayCount::ActActIsda: return "ACT/ACT ISDA";
    case DayCount::ActActIcma: return "ACT/ACT ICMA";
  }
  return "?";
}

DayCount parseDayCount(std::string_view name) {
  std::string s;
  for (char c : name)
    if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (s == "ACT/360" || s == "ACTUAL/360" || s == "A360") return DayCount::Act360;
  if (s == "ACT/365F" || s == "ACT/365" || s == "ACTUAL/365" || s == "A365" || s == "ACTUAL/365(FIXED)")
    return DayCount::Act365Fixed;
  if (s == "30/360" || s == "30/360BONDBASIS" || s == "BONDBASIS") return DayCount::Thirty360;
  if (s == "30E/360" || s == "EUROBONDBASIS") return DayCount::Thirty360E;
  if (s == "ACT/ACTISDA" || s == "ACT/ACT" || s == "ACTUAL/ACTUAL(ISDA)") return DayCount::ActActIsda;
  if (s == "ACT/ACTICMA" || s == "ACTUAL/ACTUAL(ISMA)" || s == "ACT/ACTISMA") return DayCount::ActActIcma;
  MCB_FAIL("unknown day count '" << name << "'");
}

}  // namespace mcb
