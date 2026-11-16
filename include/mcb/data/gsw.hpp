#pragma once
// The Federal Reserve's Gurkaynak-Sack-Wright (GSW) nominal Treasury yield
// curve: a Svensson curve fitted every day since 1961 to off-the-run
// Treasury coupon securities.  This is the data set used in the paper, and
// the history the statistical bootstrap resamples.
//
// The Fed publishes the six Svensson parameters per day, from which the
// continuously compounded zero yield at any maturity m (years) is
//
//   y(m) = b0 + b1 (1-e^{-m/t1})/(m/t1) + b2 [(1-e^{-m/t1})/(m/t1) - e^{-m/t1}]
//             + b3 [(1-e^{-m/t2})/(m/t2) - e^{-m/t2}]            (in percent)
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/curves/yield_curve.hpp"
#include "mcb/stats/term_panel.hpp"

namespace mcb {

struct SvenssonParams {
  Date date;
  double beta0 = 0, beta1 = 0, beta2 = 0, beta3 = 0, tau1 = 1, tau2 = 1;
};

/// Continuously compounded zero yield (decimal) at a maturity in years.
double svenssonZeroYield(const SvenssonParams& p, double maturityYears);
/// Instantaneous forward rate (decimal).
double svenssonForward(const SvenssonParams& p, double maturityYears);

/// Parses the Fed's feds200628.csv (or the compact extract written by
/// writeSvenssonCsv); rows without parameters are skipped. Sorted by date.
std::vector<SvenssonParams> parseGswParameters(const std::string& csvText);
/// Compact CSV: date,beta0,beta1,beta2,beta3,tau1,tau2.
std::string writeSvenssonCsv(const std::vector<SvenssonParams>& rows);

enum class PanelFrequency { Monthly, Quarterly };

/// Last observation of every month (or quarter) between `from` and `to`.
/// When includePartialLast is false, a final period that has not ended by the
/// last available observation's month/quarter end is dropped.
std::vector<SvenssonParams> endOfPeriodObservations(const std::vector<SvenssonParams>& daily, PanelFrequency freq,
                                                    Date from, Date to);

/// Zero yields for maturities 1..N periods at each observation.
YieldPanel buildYieldPanel(const std::vector<SvenssonParams>& observations, PanelFrequency freq, int maxMaturity);

/// A Svensson curve as a DiscountCurve (used to price off the Fed's fitted curve).
class SvenssonCurve final : public DiscountCurve {
 public:
  explicit SvenssonCurve(const SvenssonParams& p) : p_(p) {}
  Date referenceDate() const override { return p_.date; }
  double logDiscountT(double t) const override { return t <= 0.0 ? 0.0 : -svenssonZeroYield(p_, t) * t; }
  double instantaneousForwardT(double t) const override { return svenssonForward(p_, t); }

 private:
  SvenssonParams p_;
};

}  // namespace mcb
