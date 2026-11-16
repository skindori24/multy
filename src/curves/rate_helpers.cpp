#include "mcb/curves/rate_helpers.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

// ---------------------------------------------------------- deposit / FRA ---
ForwardRateHelper::ForwardRateHelper(const char* kind, Date start, Date end, DayCount dc)
    : kind_(kind), start_(start), end_(end), tau_(yearFraction(dc, start, end)) {
  MCB_REQUIRE(end > start, kind << ": end date must be after start date");
}

double ForwardRateHelper::impliedQuote(const DiscountCurve& fwd, const DiscountCurve&) const {
  return (std::exp(fwd.logDiscountT(fwd.time(start_)) - fwd.logDiscountT(fwd.time(end_))) - 1.0) / tau_;
}

// ---------------------------------------------------------------- futures ---
FuturesHelper::FuturesHelper(Date start, Date end, DayCount dc, double convexityAdjustment)
    : start_(start), end_(end), tau_(yearFraction(dc, start, end)), convexity_(convexityAdjustment) {
  MCB_REQUIRE(end > start, "future: end date must be after start date");
}

double FuturesHelper::impliedQuote(const DiscountCurve& fwd, const DiscountCurve&) const {
  const double forward = (std::exp(fwd.logDiscountT(fwd.time(start_)) - fwd.logDiscountT(fwd.time(end_))) - 1.0) / tau_;
  return 100.0 * (1.0 - (forward + convexity_));
}

// -------------------------------------------------------------------- OIS ---
OisHelper::OisHelper(std::vector<Period> periods) : periods_(std::move(periods)) {
  MCB_REQUIRE(!periods_.empty(), "OIS helper needs at least one period");
  pillar_ = periods_.front().end;
  for (const auto& p : periods_) pillar_ = std::max({pillar_, p.end, p.pay});
}

double OisHelper::impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const {
  double floating = 0.0, annuity = 0.0;
  for (const auto& p : periods_) {
    const double d = disc.discount(p.pay);
    floating += (std::exp(fwd.logDiscountT(fwd.time(p.start)) - fwd.logDiscountT(fwd.time(p.end))) - 1.0) * d;
    annuity += p.tau * d;
  }
  return floating / annuity;
}

// ------------------------------------------------------------------- swap ---
SwapHelper::SwapHelper(std::vector<FixedCoupon> fixed, std::vector<FloatCoupon> floating, Date start, Date maturity)
    : fixed_(std::move(fixed)), float_(std::move(floating)), start_(start) {
  MCB_REQUIRE(!fixed_.empty() && !float_.empty(), "swap helper needs both legs");
  pillar_ = std::max(maturity, float_.back().indexEnd);
}

double SwapHelper::impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const {
  double annuity = 0.0, floating = 0.0;
  for (const auto& c : fixed_) annuity += c.tau * disc.discount(c.pay);
  for (const auto& c : float_) {
    const double forward =
        (std::exp(fwd.logDiscountT(fwd.time(c.valueDate)) - fwd.logDiscountT(fwd.time(c.indexEnd))) - 1.0) / c.tauIndex;
    floating += c.tauAccrual * forward * disc.discount(c.pay);
  }
  return floating / annuity;
}

// --------------------------------------------------------------- par bond ---
ParBondHelper::ParBondHelper(Date settle, std::vector<Coupon> coupons) : settle_(settle), coupons_(std::move(coupons)) {
  MCB_REQUIRE(!coupons_.empty(), "par bond helper needs at least one coupon");
  MCB_REQUIRE(coupons_.front().pay > settle_, "par bond: first coupon must be after settlement");
}

double ParBondHelper::impliedQuote(const DiscountCurve& fwd, const DiscountCurve&) const {
  double annuity = 0.0;
  for (const auto& c : coupons_) annuity += c.tau * fwd.discount(c.pay);
  return (fwd.discount(settle_) - fwd.discount(coupons_.back().pay)) / annuity;
}

}  // namespace mcb
