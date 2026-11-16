#include "mcb/curves/yield_curve.hpp"

#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

double DiscountCurve::discountT(double t) const { return std::exp(logDiscountT(t)); }

double DiscountCurve::zeroRateT(double t) const {
  // At t = 0 the zero rate is the limit, i.e. the instantaneous forward.
  if (std::fabs(t) < 1e-10) return instantaneousForwardT(0.0);
  return -logDiscountT(t) / t;
}

double DiscountCurve::forwardRate(Date start, Date end, DayCount dc) const {
  MCB_REQUIRE(end > start, "forward rate needs end > start");
  const double tau = yearFraction(dc, start, end);
  return (std::exp(logDiscountT(time(start)) - logDiscountT(time(end))) - 1.0) / tau;
}

double DiscountCurve::instantaneousForwardT(double t) const {
  const double h = 1e-5;
  const double lo = t > h ? t - h : 0.0;
  return -(logDiscountT(t + h) - logDiscountT(lo)) / (t + h - lo);
}

YieldCurve::YieldCurve(std::string name, Date referenceDate, const std::vector<Date>& pillarDates,
                       const std::vector<double>& discountFactors, Interpolation interpolation)
    : name_(std::move(name)), ref_(referenceDate), dates_(pillarDates), interp_(interpolation) {
  MCB_REQUIRE(!dates_.empty(), "curve '" << name_ << "' needs at least one pillar");
  MCB_REQUIRE(dates_.size() == discountFactors.size(), "curve '" << name_ << "': pillar/discount size mismatch");
  t_.reserve(dates_.size() + 1);
  y_.reserve(dates_.size() + 1);
  t_.push_back(0.0);
  y_.push_back(0.0);
  Date prev = ref_;
  for (std::size_t i = 0; i < dates_.size(); ++i) {
    MCB_REQUIRE(dates_[i] > prev, "curve '" << name_ << "': pillar " << dates_[i].iso()
                                            << " is not after the previous node " << prev.iso());
    MCB_REQUIRE(discountFactors[i] > 0.0, "curve '" << name_ << "': non-positive discount factor");
    t_.push_back((dates_[i] - ref_) / 365.0);
    y_.push_back(std::log(discountFactors[i]));
    prev = dates_[i];
  }
  interp_.fit(t_, y_);
}

std::vector<double> YieldCurve::pillarDiscounts() const {
  std::vector<double> out;
  out.reserve(dates_.size());
  for (std::size_t i = 1; i < y_.size(); ++i) out.push_back(std::exp(y_[i]));
  return out;
}

void YieldCurve::setNodeLogDiscount(std::size_t node, double value) {
  MCB_REQUIRE(node >= 1 && node < y_.size(), "curve node index out of range");
  y_[node] = value;
}

void YieldCurve::refit(std::size_t activeNodes) {
  const std::size_t n = activeNodes > t_.size() ? t_.size() : activeNodes;
  interp_.fit(t_.data(), y_.data(), n);
}

}  // namespace mcb
