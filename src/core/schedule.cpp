#include "mcb/core/schedule.hpp"

#include "mcb/core/error.hpp"

namespace mcb {

Schedule Schedule::make(const ScheduleSpec& s) {
  MCB_REQUIRE(!s.effective.isNull() && !s.termination.isNull(), "schedule needs effective and termination dates");
  MCB_REQUIRE(s.effective < s.termination,
              "schedule effective date " << s.effective.iso() << " must precede termination " << s.termination.iso());
  using C = BusinessDayConvention;
  const Calendar& cal = s.calendar;
  const Calendar nullCalendar(Calendar::Kind::Null);
  DateGeneration rule = s.tenor.length == 0 ? DateGeneration::Zero : s.rule;
  // The end-of-month rule only makes sense for month/year tenors.
  const bool eom = s.endOfMonth && (s.tenor.unit == TimeUnit::Months || s.tenor.unit == TimeUnit::Years);

  std::vector<Date> dates;
  Date seed;
  int periods = 1;

  switch (rule) {
    case DateGeneration::Zero:
      dates = {s.effective, s.termination};
      seed = s.effective;
      break;

    case DateGeneration::Backward: {
      dates.push_back(s.termination);
      seed = s.termination;
      for (;;) {
        // Always step from the seed with a growing multiple of the tenor;
        // stepping date-to-date would drift after a month-end clip.
        const Date temp = nullCalendar.advance(seed, -periods * s.tenor, s.convention, eom);
        if (temp < s.effective) break;
        // skip dates that would collapse onto their neighbour after rolling
        if (cal.adjust(dates.front(), s.convention) != cal.adjust(temp, s.convention))
          dates.insert(dates.begin(), temp);
        ++periods;
      }
      if (cal.adjust(dates.front(), s.convention) != cal.adjust(s.effective, s.convention))
        dates.insert(dates.begin(), s.effective);
      break;
    }

    case DateGeneration::Forward: {
      dates.push_back(s.effective);
      seed = s.effective;
      for (;;) {
        const Date temp = nullCalendar.advance(seed, periods * s.tenor, s.convention, eom);
        if (temp > s.termination) break;
        if (cal.adjust(dates.back(), s.convention) != cal.adjust(temp, s.convention)) dates.push_back(temp);
        ++periods;
      }
      if (cal.adjust(dates.back(), s.terminationConvention) != cal.adjust(s.termination, s.terminationConvention))
        dates.push_back(s.termination);
      break;
    }
  }

  // Roll the generated dates. The two end dates follow their own conventions;
  // interior dates snap to the month end first when the end-of-month rule is
  // active (i.e. the anchor date is the last business day of its month).
  const std::size_t n = dates.size();
  dates.front() = cal.adjust(dates.front(), s.convention);
  if (s.terminationConvention != C::Unadjusted) dates.back() = cal.adjust(dates.back(), s.terminationConvention);
  const bool snapToMonthEnd = eom && cal.isEndOfMonth(seed);
  for (std::size_t i = 1; i + 1 < n; ++i)
    dates[i] = cal.adjust(snapToMonthEnd ? dates[i].endOfMonth() : dates[i], s.convention);

  // Rolling can make the last (or first) two dates coincide; drop the extra one.
  if (dates.size() >= 2 && dates[dates.size() - 2] >= dates.back()) {
    dates[dates.size() - 2] = dates.back();
    dates.pop_back();
  }
  if (dates.size() >= 2 && dates[1] <= dates.front()) {
    dates[1] = dates.front();
    dates.erase(dates.begin());
  }
  MCB_REQUIRE(dates.size() >= 2, "schedule collapsed to a single date");
  return Schedule(std::move(dates));
}

}  // namespace mcb
