#pragma once
// Business-day calendars and date rolling.
//
// Interest-rate contracts never pay on weekends or holidays, so every date
// produced by "start + 6 months" has to be rolled onto a business day using
// a market convention.  The holiday rules below follow the same definitions
// as QuantLib (and are unit-tested against it day by day).
#include <memory>
#include <set>
#include <string>

#include "mcb/core/date.hpp"

namespace mcb {

enum class BusinessDayConvention {
  Following,          // next business day
  ModifiedFollowing,  // next business day unless it crosses a month end, then previous
  Preceding,          // previous business day
  ModifiedPreceding,  // previous unless it crosses a month start, then next
  Unadjusted          // leave the date alone
};

class Calendar {
 public:
  enum class Kind {
    Null,              // every day is a business day
    WeekendsOnly,      // Saturday/Sunday only
    Target,            // euro-area TARGET2 settlement calendar
    UsSettlement,      // US bank settlement
    UsGovernmentBond,  // SIFMA recommended closings for US Treasuries
    UsSofr             // US Government Securities business days (SOFR fixing days)
  };

  Calendar() = default;
  explicit Calendar(Kind kind) : kind_(kind) {}

  Kind kind() const { return kind_; }
  std::string name() const;

  bool isBusinessDay(Date d) const;
  bool isHoliday(Date d) const { return !isBusinessDay(d); }

  /// Rolls a date onto a business day.
  Date adjust(Date d, BusinessDayConvention c = BusinessDayConvention::Following) const;

  /// Advances by n units. Days are counted in business days; weeks, months
  /// and years use calendar arithmetic and the result is then rolled with c.
  /// With endOfMonth = true, a start date that is the last business day of
  /// its month maps to the last business day of the target month.
  Date advance(Date d, int n, TimeUnit unit,
               BusinessDayConvention c = BusinessDayConvention::Following, bool endOfMonth = false) const;
  Date advance(Date d, Period p, BusinessDayConvention c = BusinessDayConvention::Following,
               bool endOfMonth = false) const {
    return advance(d, p.length, p.unit, c, endOfMonth);
  }

  /// True if d is the last business day of its month (or later).
  bool isEndOfMonth(Date d) const;
  /// Last business day of the month containing d.
  Date endOfMonth(Date d) const;

  /// Business days in (from, to].
  int businessDaysBetween(Date from, Date to) const;

  /// Adds an ad-hoc closing (shared by copies of this calendar made afterwards).
  void addHoliday(Date d);

 private:
  Kind kind_ = Kind::Null;
  std::shared_ptr<std::set<Date>> extraHolidays_;
};

/// Day of the year (1-based) on which Easter Monday falls (Gregorian calendar).
int easterMondayDayOfYear(int year);

/// Third Wednesday of the given month: IMM date used by interest-rate futures.
Date immDate(int year, int month);
/// First IMM date (Mar/Jun/Sep/Dec third Wednesday) strictly after d.
Date nextImmDate(Date d);

}  // namespace mcb
