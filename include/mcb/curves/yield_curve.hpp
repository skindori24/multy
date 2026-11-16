#pragma once
// Discount curves.
//
// Everything in fixed income is priced from discount factors: DF(T) is
// today's value of one unit of currency paid at T.  Zero rates and forward
// rates are just different ways of quoting the same function:
//
//   zero rate      z(T) = -ln DF(T) / T
//   forward rate   F(T1,T2) = (DF(T1)/DF(T2) - 1) / tau(T1,T2)
//
// DiscountCurve is the abstract interface used by all pricers; YieldCurve is
// the pillar-based implementation produced by the bootstrapper.  Parametric
// curves (Svensson, the MOEX G-curve) implement the same interface.
#include <cstddef>
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/core/daycount.hpp"
#include "mcb/math/interpolation.hpp"

namespace mcb {

class DiscountCurve {
 public:
  virtual ~DiscountCurve() = default;

  virtual Date referenceDate() const = 0;
  /// ln DF at time t, measured in ACT/365F years from the reference date.
  virtual double logDiscountT(double t) const = 0;
  /// Instantaneous forward rate f(t) = -d ln DF / dt.
  virtual double instantaneousForwardT(double t) const;

  double time(Date d) const { return (d - referenceDate()) / 365.0; }
  double discountT(double t) const;
  double discount(Date d) const { return discountT(time(d)); }
  /// Continuously compounded zero rate.
  double zeroRateT(double t) const;
  double zeroRate(Date d) const { return zeroRateT(time(d)); }
  /// Simply compounded forward rate for the period [start, end].
  double forwardRate(Date start, Date end, DayCount dc) const;
};

class YieldCurve final : public DiscountCurve {
 public:
  /// Builds a curve through (pillarDates[i], discountFactors[i]); the node at
  /// the reference date (DF = 1) is added automatically.
  YieldCurve(std::string name, Date referenceDate, const std::vector<Date>& pillarDates,
             const std::vector<double>& discountFactors, Interpolation interpolation);

  const std::string& name() const { return name_; }
  Date referenceDate() const override { return ref_; }
  double logDiscountT(double t) const override { return interp_.value(t); }
  double instantaneousForwardT(double t) const override { return -interp_.derivative(t); }

  Interpolation interpolation() const { return interp_.kind(); }
  /// Number of pillars (not counting the reference date).
  std::size_t size() const { return dates_.size(); }
  const std::vector<Date>& pillarDates() const { return dates_; }
  std::vector<double> pillarDiscounts() const;
  Date maxDate() const { return dates_.back(); }

  /// Node arrays including the reference node at index 0 (t = 0, ln DF = 0).
  const std::vector<double>& nodeTimes() const { return t_; }
  const std::vector<double>& nodeLogDiscounts() const { return y_; }

  // --- mutation, for the bootstrapper and the risk engine -------------------
  /// Sets ln DF at node i (1-based among pillars: node 0 is the reference
  /// date). Call refit() afterwards.
  void setNodeLogDiscount(std::size_t node, double value);
  /// Refits the interpolation through the first `activeNodes` nodes (all by default).
  void refit(std::size_t activeNodes = static_cast<std::size_t>(-1));

 private:
  std::string name_;
  Date ref_;
  std::vector<Date> dates_;
  std::vector<double> t_, y_;
  CurveInterpolator interp_;
};

/// A curve shifted by a constant continuously-compounded spread:
/// DF'(t) = DF(t) * exp(-spread * t).  Used for Z-spreads and scenario bumps.
class SpreadedCurve final : public DiscountCurve {
 public:
  SpreadedCurve(const DiscountCurve& base, double spread) : base_(base), spread_(spread) {}
  Date referenceDate() const override { return base_.referenceDate(); }
  double logDiscountT(double t) const override { return base_.logDiscountT(t) - spread_ * t; }
  double instantaneousForwardT(double t) const override { return base_.instantaneousForwardT(t) + spread_; }

 private:
  const DiscountCurve& base_;
  double spread_;
};

}  // namespace mcb
