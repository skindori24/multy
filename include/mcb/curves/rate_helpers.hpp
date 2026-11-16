#pragma once
// Rate helpers: the market instruments a curve is bootstrapped from.
//
// Each helper knows (a) which market quote it represents, (b) how to compute
// the same quote from a pair of curves ("implied quote") and (c) its pillar
// date, the node of the curve it pins down.  The bootstrapper moves that node
// until impliedQuote == market quote.  Helpers are immutable once built, so
// one set of helpers can serve many market snapshots (and threads).
//
// Two curves are passed to impliedQuote:
//   forwarding   the curve under construction (projects the floating index)
//   discounting  the curve used to present-value cash flows
// In a single-curve world they are the same object.  In the post-2008
// multicurve world, collateralised swaps are discounted on the overnight
// (OIS) curve while each Euribor tenor has its own forwarding curve.
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/core/daycount.hpp"
#include "mcb/curves/yield_curve.hpp"

namespace mcb {

/// A helper's quote is a linear combination of market quotes, e.g. a 3M swap
/// rate given as "6M swap rate minus the 3s6s basis".
struct QuoteTerm {
  std::string id;
  double weight = 1.0;
};

class RateHelper {
 public:
  virtual ~RateHelper() = default;

  virtual double impliedQuote(const DiscountCurve& forwarding, const DiscountCurve& discounting) const = 0;
  virtual Date pillarDate() const = 0;
  virtual Date earliestDate() const = 0;
  virtual const char* kind() const = 0;

  std::string label;                  // e.g. "OIS 5Y"
  /// Market quote ids this helper's quote is built from. Units: rates as
  /// decimals (0.0125 = 1.25%), futures as prices (99.82).
  std::vector<QuoteTerm> quoteTerms;
};

/// Deposit or FRA: a single simply-compounded rate over [start, end].
///   implied = (P(start)/P(end) - 1) / tau
/// Also used for Treasury bills quoted as a simple yield.
class ForwardRateHelper final : public RateHelper {
 public:
  ForwardRateHelper(const char* kind, Date start, Date end, DayCount dc);
  double impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const override;
  Date pillarDate() const override { return end_; }
  Date earliestDate() const override { return start_; }
  const char* kind() const override { return kind_; }

 private:
  const char* kind_;
  Date start_, end_;
  double tau_;
};

/// Interest-rate future quoted as 100 - rate, with a convexity adjustment
/// (futures rate = forward rate + adjustment, because daily margining makes
/// a future worth slightly less than the equivalent FRA to the long).
class FuturesHelper final : public RateHelper {
 public:
  FuturesHelper(Date start, Date end, DayCount dc, double convexityAdjustment = 0.0);
  double impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const override;
  Date pillarDate() const override { return end_; }
  Date earliestDate() const override { return start_; }
  const char* kind() const override { return "future"; }
  double convexityAdjustment() const { return convexity_; }

 private:
  Date start_, end_;
  double tau_, convexity_;
};

/// Overnight-indexed swap: fixed rate against the daily-compounded overnight
/// rate.  Compounding telescopes, so each floating coupon is worth
/// P(start)/P(end) - 1 and no daily schedule is needed.
///   implied = sum_i (P(s_i)/P(e_i) - 1) D(pay_i) / sum_i tau_i D(pay_i)
class OisHelper final : public RateHelper {
 public:
  struct Period {
    Date start, end, pay;
    double tau;
  };
  explicit OisHelper(std::vector<Period> periods);
  double impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const override;
  Date pillarDate() const override { return pillar_; }
  Date earliestDate() const override { return periods_.front().start; }
  const char* kind() const override { return "ois"; }
  const std::vector<Period>& periods() const { return periods_; }

 private:
  std::vector<Period> periods_;
  Date pillar_;
};

/// Fixed-for-floating swap against a term index (Euribor 6M, ...).
///   implied = sum_j tau_j F_j D(pay_j) / sum_i tau_i D(pay_i)
/// with F_j = (P(v_j)/P(e_j) - 1)/tauIndex_j projected on the forwarding curve.
class SwapHelper final : public RateHelper {
 public:
  struct FixedCoupon {
    Date pay;
    double tau;
  };
  struct FloatCoupon {
    Date valueDate, indexEnd, pay;  // the index period [valueDate, indexEnd]
    double tauIndex, tauAccrual;
  };
  SwapHelper(std::vector<FixedCoupon> fixed, std::vector<FloatCoupon> floating, Date start, Date maturity);
  double impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const override;
  Date pillarDate() const override { return pillar_; }
  Date earliestDate() const override { return start_; }
  const char* kind() const override { return "swap"; }

 private:
  std::vector<FixedCoupon> fixed_;
  std::vector<FloatCoupon> float_;
  Date start_, pillar_;
};

/// Par bond: a coupon bond priced at 100 on its settlement date, quoted by
/// its par yield (e.g. the US Treasury "constant maturity" par curve).
///   implied = (D(settle) - D(T_n)) / sum_i tau_i D(T_i)
class ParBondHelper final : public RateHelper {
 public:
  struct Coupon {
    Date pay;
    double tau;  // accrual fraction, e.g. 0.5 for a semi-annual coupon
  };
  ParBondHelper(Date settle, std::vector<Coupon> coupons);
  double impliedQuote(const DiscountCurve& fwd, const DiscountCurve& disc) const override;
  Date pillarDate() const override { return coupons_.back().pay; }
  Date earliestDate() const override { return settle_; }
  const char* kind() const override { return "parbond"; }

 private:
  Date settle_;
  std::vector<Coupon> coupons_;
};

}  // namespace mcb
