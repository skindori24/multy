#pragma once
// Shared helpers for the unit tests.
#include <string>

#include "mcb/core/calendar.hpp"
#include "mcb/core/error.hpp"
#include "mcb/core/schedule.hpp"
#include "mcb/io/csv.hpp"

namespace mcbtest {

inline std::string dataPath(const std::string& relative) { return std::string(MCB_TEST_DATA_DIR) + "/" + relative; }
inline std::string sourcePath(const std::string& relative) { return std::string(MCB_SOURCE_DIR) + "/" + relative; }

inline mcb::Calendar calendarByName(const std::string& n) {
  using K = mcb::Calendar::Kind;
  if (n == "TARGET") return mcb::Calendar(K::Target);
  if (n == "US_SETTLEMENT") return mcb::Calendar(K::UsSettlement);
  if (n == "US_GOVBOND") return mcb::Calendar(K::UsGovernmentBond);
  if (n == "US_SOFR") return mcb::Calendar(K::UsSofr);
  if (n == "NULL") return mcb::Calendar(K::Null);
  MCB_FAIL("unknown calendar " << n);
}

inline mcb::BusinessDayConvention conventionByName(const std::string& n) {
  using C = mcb::BusinessDayConvention;
  if (n == "F") return C::Following;
  if (n == "MF") return C::ModifiedFollowing;
  if (n == "P") return C::Preceding;
  if (n == "MP") return C::ModifiedPreceding;
  if (n == "U") return C::Unadjusted;
  MCB_FAIL("unknown convention " << n);
}

}  // namespace mcbtest
