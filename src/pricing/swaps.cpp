#include "mcb/pricing/swaps.hpp"

#include <cmath>

#include "mcb/core/error.hpp"

namespace mcb {

SwapValue priceOis(const OisSwap& swap, const DiscountCurve& fwd, const DiscountCurve& disc) {
  MCB_REQUIRE(!swap.periods.empty(), "OIS has no periods");
  const Date ref = disc.referenceDate();
  SwapValue v;
  for (const auto& p : swap.periods) {
    if (p.pay <= ref) continue;  // already paid
    const double d = disc.discount(p.pay);
    v.annuity += p.tau * d;
    // Compounded overnight growth over the period. For a period that started
    // before today, the realised part is supplied in accruedCompounding and
    // only the remaining part is projected (DF at or before today is 1).
    double growth;
    if (p.start < ref) {
      growth = swap.accruedCompounding / fwd.discount(p.end);
    } else {
      growth = std::exp(fwd.logDiscountT(fwd.time(p.start)) - fwd.logDiscountT(fwd.time(p.end)));
    }
    v.floatLegPv += (growth - 1.0 + swap.floatSpread * p.tau) * d;
  }
  v.annuity *= swap.notional;
  v.floatLegPv *= swap.notional;
  v.fixedLegPv = swap.fixedRate * v.annuity;
  v.parRate = v.annuity != 0.0 ? v.floatLegPv / v.annuity : 0.0;
  v.npv = (swap.payFixed ? 1.0 : -1.0) * (v.floatLegPv - v.fixedLegPv);
  return v;
}

SwapValue priceSwap(const VanillaSwap& swap, const DiscountCurve& fwd, const DiscountCurve& disc) {
  MCB_REQUIRE(!swap.fixedLeg.empty() && !swap.floatLeg.empty(), "swap is missing a leg");
  const Date ref = disc.referenceDate();
  SwapValue v;
  for (const auto& c : swap.fixedLeg) {
    if (c.pay <= ref) continue;
    v.annuity += c.tau * disc.discount(c.pay);
  }
  for (const auto& c : swap.floatLeg) {
    if (c.pay <= ref) continue;
    MCB_REQUIRE(c.valueDate >= ref, "swap has a floating coupon that fixed before the valuation date; "
                                    "past fixings are not supported");
    const double forward =
        (std::exp(fwd.logDiscountT(fwd.time(c.valueDate)) - fwd.logDiscountT(fwd.time(c.indexEnd))) - 1.0) / c.tauIndex;
    v.floatLegPv += c.tauAccrual * (forward + swap.floatSpread) * disc.discount(c.pay);
  }
  v.annuity *= swap.notional;
  v.floatLegPv *= swap.notional;
  v.fixedLegPv = swap.fixedRate * v.annuity;
  v.parRate = v.annuity != 0.0 ? v.floatLegPv / v.annuity : 0.0;
  v.npv = (swap.payFixed ? 1.0 : -1.0) * (v.floatLegPv - v.fixedLegPv);
  return v;
}

double priceFra(const Fra& fra, const DiscountCurve& fwd, const DiscountCurve& disc, double* forwardRate) {
  const double forward =
      (std::exp(fwd.logDiscountT(fwd.time(fra.valueDate)) - fwd.logDiscountT(fwd.time(fra.indexEnd))) - 1.0) / fra.tau;
  if (forwardRate) *forwardRate = forward;
  // Market FRAs settle at the start of the period, discounted at the fixing itself.
  const double payoffAtStart = fra.notional * fra.tau * (forward - fra.strike) / (1.0 + forward * fra.tau);
  return (fra.payFixed ? 1.0 : -1.0) * payoffAtStart * disc.discount(fra.valueDate);
}

}  // namespace mcb
