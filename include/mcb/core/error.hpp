#pragma once
// Error handling used across the library: one exception type and a
// precondition macro. Throwing (rather than returning error codes) keeps the
// numerical code readable; callers that process live market data catch
// mcb::Error at the boundary and report the failing source.
#include <sstream>
#include <stdexcept>
#include <string>

namespace mcb {

class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace mcb

#define MCB_REQUIRE(cond, msg)                         \
  do {                                                 \
    if (!(cond)) {                                     \
      std::ostringstream mcb_require_os_;              \
      mcb_require_os_ << msg;                          \
      throw ::mcb::Error(mcb_require_os_.str());       \
    }                                                  \
  } while (false)

#define MCB_FAIL(msg) MCB_REQUIRE(false, msg)
