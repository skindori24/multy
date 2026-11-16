#include "mcb/core/date.hpp"

#include <cctype>
#include <cstdio>

#include "mcb/core/error.hpp"

namespace mcb {
namespace {

// days since 1970-01-01 for a civil date (H. Hinnant, "chrono-compatible
// low-level date algorithms").
std::int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

YearMonthDay civilFromDays(std::int32_t z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  return {y + (m <= 2), m, d};
}

bool allDigits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

int toInt(std::string_view s) {
  int v = 0;
  for (char c : s) v = v * 10 + (c - '0');
  return v;
}

bool validYmd(int y, int m, int d) {
  return y >= 1600 && y <= 2400 && m >= 1 && m <= 12 && d >= 1 && d <= Date::daysInMonth(y, m);
}

}  // namespace

// ---------------------------------------------------------------- Period ---

Period Period::parse(std::string_view text) {
  // trim
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  MCB_REQUIRE(text.size() >= 2, "cannot parse tenor '" << text << "'");
  const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
  std::string_view num = text.substr(0, text.size() - 1);
  bool negative = false;
  if (!num.empty() && num.front() == '-') {
    negative = true;
    num.remove_prefix(1);
  }
  MCB_REQUIRE(allDigits(num), "cannot parse tenor '" << text << "'");
  Period p;
  p.length = negative ? -toInt(num) : toInt(num);
  switch (u) {
    case 'D': p.unit = TimeUnit::Days; break;
    case 'W': p.unit = TimeUnit::Weeks; break;
    case 'M': p.unit = TimeUnit::Months; break;
    case 'Y': p.unit = TimeUnit::Years; break;
    default: MCB_FAIL("unknown tenor unit in '" << text << "'");
  }
  return p;
}

std::string Period::str() const {
  static const char units[] = {'D', 'W', 'M', 'Y'};
  return std::to_string(length) + units[static_cast<int>(unit)];
}

double Period::approxYears() const {
  switch (unit) {
    case TimeUnit::Days: return length / 365.0;
    case TimeUnit::Weeks: return length * 7.0 / 365.0;
    case TimeUnit::Months: return length / 12.0;
    case TimeUnit::Years: return static_cast<double>(length);
  }
  return 0.0;
}

// ------------------------------------------------------------------ Date ---

Date::Date(int year, int month, int day) {
  MCB_REQUIRE(validYmd(year, month, day), "invalid date " << year << "-" << month << "-" << day);
  serial_ = daysFromCivil(year, month, day);
}

Date Date::fromSerial(std::int32_t serial) {
  Date d;
  d.serial_ = serial;
  return d;
}

bool Date::tryParse(std::string_view s, Date& out) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  int y = 0, m = 0, d = 0;
  if (s.size() == 10 && s[4] == '-' && s[7] == '-') {  // YYYY-MM-DD
    if (!allDigits(s.substr(0, 4)) || !allDigits(s.substr(5, 2)) || !allDigits(s.substr(8, 2))) return false;
    y = toInt(s.substr(0, 4)), m = toInt(s.substr(5, 2)), d = toInt(s.substr(8, 2));
  } else if (s.size() == 8 && allDigits(s)) {  // YYYYMMDD
    y = toInt(s.substr(0, 4)), m = toInt(s.substr(4, 2)), d = toInt(s.substr(6, 2));
  } else if (s.size() == 10 && s[2] == '.' && s[5] == '.') {  // DD.MM.YYYY
    if (!allDigits(s.substr(0, 2)) || !allDigits(s.substr(3, 2)) || !allDigits(s.substr(6, 4))) return false;
    d = toInt(s.substr(0, 2)), m = toInt(s.substr(3, 2)), y = toInt(s.substr(6, 4));
  } else {  // M/D/YYYY with optional leading zeros
    const auto a = s.find('/');
    const auto b = a == std::string_view::npos ? a : s.find('/', a + 1);
    if (a == std::string_view::npos || b == std::string_view::npos) return false;
    const auto ms = s.substr(0, a), ds = s.substr(a + 1, b - a - 1), ys = s.substr(b + 1);
    if (!allDigits(ms) || !allDigits(ds) || !allDigits(ys) || ys.size() != 4 || ms.size() > 2 || ds.size() > 2)
      return false;
    m = toInt(ms), d = toInt(ds), y = toInt(ys);
  }
  if (!validYmd(y, m, d)) return false;
  out = Date(y, m, d);
  return true;
}

Date Date::parse(std::string_view text) {
  Date d;
  MCB_REQUIRE(tryParse(text, d), "cannot parse date '" << text << "'");
  return d;
}

YearMonthDay Date::ymd() const {
  MCB_REQUIRE(!isNull(), "null date");
  return civilFromDays(serial_);
}

Weekday Date::weekday() const {
  // 1970-01-01 was a Thursday.
  const int w = ((serial_ % 7) + 7 + 3) % 7;
  return static_cast<Weekday>(w);
}

int Date::dayOfYear() const {
  const auto c = ymd();
  return serial_ - daysFromCivil(c.year, 1, 1) + 1;
}

bool Date::isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int Date::daysInMonth(int y, int m) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && isLeap(y)) ? 29 : days[m - 1];
}

Date Date::addMonths(int n) const {
  const auto c = ymd();
  int total = c.year * 12 + (c.month - 1) + n;
  int y = total / 12, m = total % 12;
  if (m < 0) {
    m += 12;
    --y;
  }
  ++m;
  const int dim = daysInMonth(y, m);
  return Date(y, m, c.day < dim ? c.day : dim);
}

Date Date::add(Period p) const {
  switch (p.unit) {
    case TimeUnit::Days: return addDays(p.length);
    case TimeUnit::Weeks: return addDays(7 * p.length);
    case TimeUnit::Months: return addMonths(p.length);
    case TimeUnit::Years: return addYears(p.length);
  }
  return *this;
}

Date Date::endOfMonth() const {
  const auto c = ymd();
  return Date(c.year, c.month, daysInMonth(c.year, c.month));
}

bool Date::isEndOfMonth() const {
  const auto c = ymd();
  return c.day == daysInMonth(c.year, c.month);
}

std::string Date::iso() const {
  if (isNull()) return "null";
  const auto c = ymd();
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
  return buf;
}

}  // namespace mcb
