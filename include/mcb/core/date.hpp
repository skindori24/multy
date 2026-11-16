#pragma once
// Calendar dates and tenors.
//
// A Date is stored as a single integer (days since 1970-01-01), which makes
// differences and comparisons trivial and keeps the type as cheap as an int.
// Year/month/day are recovered with Howard Hinnant's civil-calendar
// algorithms (proleptic Gregorian calendar).
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcb {

enum class Weekday { Mon = 0, Tue, Wed, Thu, Fri, Sat, Sun };
enum class TimeUnit { Days, Weeks, Months, Years };

/// A tenor such as 3M or 10Y.
struct Period {
  int length = 0;
  TimeUnit unit = TimeUnit::Days;

  /// Parses "2D", "1W", "18M", "10Y" (case-insensitive). Throws on bad input.
  static Period parse(std::string_view text);
  std::string str() const;
  /// Rough length in years (for sorting and display only).
  double approxYears() const;

  friend bool operator==(const Period&, const Period&) = default;
};
inline Period operator*(int n, Period p) { return {n * p.length, p.unit}; }
inline Period operator-(Period p) { return {-p.length, p.unit}; }

struct YearMonthDay {
  int year, month, day;
};

class Date {
 public:
  /// A default-constructed date is "null" (isNull() == true).
  constexpr Date() = default;
  Date(int year, int month, int day);

  static Date fromSerial(std::int32_t serial);
  /// Accepts YYYY-MM-DD, YYYYMMDD, MM/DD/YYYY (US) and DD.MM.YYYY. Throws on bad input.
  static Date parse(std::string_view text);
  static bool tryParse(std::string_view text, Date& out);

  bool isNull() const { return serial_ == kNull; }
  std::int32_t serial() const { return serial_; }

  YearMonthDay ymd() const;
  int year() const { return ymd().year; }
  int month() const { return ymd().month; }
  int day() const { return ymd().day; }
  Weekday weekday() const;
  bool isWeekend() const { return weekday() >= Weekday::Sat; }
  int dayOfYear() const;  // 1 = January 1st

  Date addDays(int n) const { return fromSerial(serial_ + n); }
  /// Month arithmetic keeps the day of month, clipped to the month's length
  /// (Jan 31 + 1M = Feb 28/29).
  Date addMonths(int n) const;
  Date addYears(int n) const { return addMonths(12 * n); }
  Date add(Period p) const;
  Date endOfMonth() const;
  bool isEndOfMonth() const;

  std::string iso() const;  // YYYY-MM-DD

  static bool isLeap(int year);
  static int daysInMonth(int year, int month);

  auto operator<=>(const Date&) const = default;

 private:
  static constexpr std::int32_t kNull = INT32_MIN;
  std::int32_t serial_ = kNull;
};

/// Number of calendar days from b to a.
inline int operator-(Date a, Date b) { return a.serial() - b.serial(); }
inline Date operator+(Date a, int days) { return a.addDays(days); }

}  // namespace mcb
