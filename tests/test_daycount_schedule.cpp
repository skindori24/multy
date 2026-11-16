// Day-count conventions and coupon schedules, against hand-checked values and
// QuantLib fixtures.
#include <gtest/gtest.h>

#include "mcb/core/daycount.hpp"
#include "mcb/core/schedule.hpp"
#include "test_util.hpp"

using namespace mcb;

TEST(DayCount, HandCheckedValues) {
  const Date a(2026, 11, 17), b(2027, 11, 17);
  EXPECT_DOUBLE_EQ(yearFraction(DayCount::Act360, a, b), 365.0 / 360.0);
  EXPECT_DOUBLE_EQ(yearFraction(DayCount::Act365Fixed, a, b), 1.0);
  EXPECT_DOUBLE_EQ(yearFraction(DayCount::Thirty360, a, b), 1.0);
  EXPECT_EQ(dayCount(DayCount::Thirty360, Date(2026, 1, 31), Date(2026, 2, 28)), 28);
  EXPECT_EQ(dayCount(DayCount::Thirty360, Date(2026, 1, 30), Date(2026, 3, 31)), 60);
  EXPECT_EQ(dayCount(DayCount::Thirty360, Date(2026, 1, 15), Date(2026, 3, 31)), 76);   // 31st kept: start < 30
  EXPECT_EQ(dayCount(DayCount::Thirty360E, Date(2026, 1, 15), Date(2026, 3, 31)), 75);  // 31st -> 30th always
  // ACT/ACT ISDA across a leap-year boundary: 2023-11-01 -> 2024-03-01
  EXPECT_NEAR(yearFraction(DayCount::ActActIsda, Date(2023, 11, 1), Date(2024, 3, 1)), 61.0 / 365.0 + 60.0 / 366.0,
              1e-15);
  // ACT/ACT ICMA: half a semi-annual coupon period is a quarter of a year.
  const Date s(2026, 5, 15), e(2026, 11, 15);
  EXPECT_NEAR(yearFraction(DayCount::ActActIcma, s, Date(2026, 8, 15), s, e), 0.5 * 92.0 / 184.0, 1e-15);
  EXPECT_THROW(yearFraction(DayCount::ActActIcma, s, e), Error);
  EXPECT_EQ(parseDayCount("Actual/360"), DayCount::Act360);
  EXPECT_EQ(parseDayCount("30/360"), DayCount::Thirty360);
  EXPECT_THROW(parseDayCount("banana"), Error);
}

TEST(DayCountVsQuantLib, RandomDatePairs) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/daycount.csv"));
  ASSERT_GT(t.rows.size(), 1000u);
  int failures = 0;
  for (std::size_t i = 0; i < t.rows.size() && failures < 10; ++i) {
    const DayCount dc = parseDayCount(t.at(i, "daycount"));
    const Date d1 = Date::parse(t.at(i, "d1")), d2 = Date::parse(t.at(i, "d2"));
    const int days = static_cast<int>(parseDouble(t.at(i, "days")));
    const double yf = parseDouble(t.at(i, "year_fraction"));
    if (dayCount(dc, d1, d2) != days || std::abs(yearFraction(dc, d1, d2) - yf) > 1e-14) {
      ++failures;
      ADD_FAILURE() << t.at(i, "daycount") << " " << d1.iso() << " -> " << d2.iso() << ": mcb " << dayCount(dc, d1, d2)
                    << " / " << yearFraction(dc, d1, d2) << ", QuantLib " << days << " / " << yf;
    }
  }
  EXPECT_EQ(failures, 0);
}

TEST(Schedule, AnnualSofrSwapMatchesPublishedCashFlowDates) {
  // 3Y SOFR swap traded 2026-11-13, effective 2026-11-17: the accrual dates
  // published by the exchange are 11/17/2027, 11/17/2028 and 11/19/2029
  // (17 Nov 2029 is a Saturday).
  ScheduleSpec s;
  s.effective = Date(2026, 11, 17);
  s.termination = Date(2029, 11, 17);
  s.tenor = Period{1, TimeUnit::Years};
  s.calendar = Calendar(Calendar::Kind::UsSofr);
  const auto sch = Schedule::make(s);
  ASSERT_EQ(sch.size(), 4u);
  EXPECT_EQ(sch[1], Date(2027, 11, 17));
  EXPECT_EQ(sch[2], Date(2028, 11, 17));
  EXPECT_EQ(sch[3], Date(2029, 11, 19));
  EXPECT_EQ(sch.periods(), 3u);
}

TEST(Schedule, BackwardGenerationPutsTheStubAtTheFront) {
  // 18-month swap with annual payments: a 6-month first period, then a full year.
  ScheduleSpec s;
  s.effective = Date(2026, 11, 17);
  s.termination = Date(2028, 5, 17);
  s.tenor = Period{1, TimeUnit::Years};
  s.calendar = Calendar(Calendar::Kind::UsSofr);
  const auto back = Schedule::make(s);
  ASSERT_EQ(back.size(), 3u);
  EXPECT_EQ(back[1], Date(2027, 5, 17));
  s.rule = DateGeneration::Forward;
  const auto fwd = Schedule::make(s);
  ASSERT_EQ(fwd.size(), 3u);
  EXPECT_EQ(fwd[1], Date(2027, 11, 17));
  EXPECT_THROW(Schedule::make(ScheduleSpec{Date(2027, 1, 1), Date(2026, 1, 1)}), Error);
}

TEST(ScheduleVsQuantLib, RandomSchedules) {
  const auto t = readCsvFile(mcbtest::dataPath("ql/schedules.csv"));
  ASSERT_GT(t.rows.size(), 300u);
  int failures = 0;
  for (std::size_t i = 0; i < t.rows.size() && failures < 10; ++i) {
    ScheduleSpec s;
    s.calendar = mcbtest::calendarByName(t.at(i, "calendar"));
    s.effective = Date::parse(t.at(i, "effective"));
    s.termination = Date::parse(t.at(i, "termination"));
    s.tenor = Period::parse(t.at(i, "tenor"));
    s.convention = mcbtest::conventionByName(t.at(i, "convention"));
    s.terminationConvention = mcbtest::conventionByName(t.at(i, "term_convention"));
    s.rule = t.at(i, "rule") == "B" ? DateGeneration::Backward : DateGeneration::Forward;
    s.endOfMonth = t.at(i, "eom") == "1";
    std::string got;
    try {
      const auto sch = Schedule::make(s);
      for (std::size_t k = 0; k < sch.size(); ++k) got += (k ? "|" : "") + sch[k].iso();
    } catch (const Error& e) {
      got = std::string("ERROR: ") + e.what();
    }
    if (got != t.at(i, "dates")) {
      ++failures;
      ADD_FAILURE() << "row " << i << " " << t.at(i, "calendar") << " " << s.effective.iso() << " -> "
                    << s.termination.iso() << " " << t.at(i, "tenor") << " " << t.at(i, "convention") << "/"
                    << t.at(i, "term_convention") << " rule=" << t.at(i, "rule") << " eom=" << s.endOfMonth
                    << "\n  mcb:      " << got << "\n  QuantLib: " << t.at(i, "dates");
    }
  }
  EXPECT_EQ(failures, 0);
}
