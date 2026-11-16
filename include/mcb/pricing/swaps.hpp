#pragma once
// Linear interest-rate derivatives: overnight-indexed swaps, fixed-vs-Ibor
// swaps and FRAs, priced off a forwarding curve and a discounting curve.
//
// Sign convention: NPV is from the point of view of the holder described by
// `payFixed` (true = pays the fixed rate and receives floating).
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/curves/yield_curve.hpp"

namespace mcb {

struct OisPeriod {
  Date start, end, pay;
  double tau = 0.0;  // accrual year fraction (same for both legs)
};

/// Fixed rate versus compounded overnight rate over the same periods.
struct OisSwap {
  double notional = 1.0;
  double fixedRate = 0.0;
  bool payFixed = true;
  double floatSpread = 0.0;
  std::vector<OisPeriod> periods;
  /// For a seasoned swap: overnight compounding already realised in the
  /// current floating period up to the valuation date (1 = none).
  double accruedCompounding = 1.0;
};

struct FixedCoupon {
  Date start, end, pay;
  double tau = 0.0;
};

struct IborCoupon {
  Date fixing, valueDate, indexEnd;  // index observed on `fixing` for [valueDate, indexEnd]
  Date start, end, pay;              // accrual period and payment date
  double tauIndex = 0.0, tauAccrual = 0.0;
};

/// Fixed rate versus a term index such as Euribor 6M.
struct VanillaSwap {
  double notional = 1.0;
  double fixedRate = 0.0;
  bool payFixed = true;
  double floatSpread = 0.0;
  std::vector<FixedCoupon> fixedLeg;
  std::vector<IborCoupon> floatLeg;
};

/// Forward rate agreement settled at the start of the period (FRA discounting).
struct Fra {
  double notional = 1.0;
  double strike = 0.0;
  bool payFixed = true;  // true = long the FRA (benefits when rates rise)
  Date valueDate, indexEnd;
  double tau = 0.0;
};

struct SwapValue {
  double npv = 0.0;
  double fixedLegPv = 0.0;  // PV of the fixed leg (positive number)
  double floatLegPv = 0.0;  // PV of the floating leg (positive number)
  double annuity = 0.0;     // PV of 1 unit of fixed rate: notional * sum tau_i D(T_i)
  double parRate = 0.0;     // fixed rate that would make npv = 0
};

/// Cash flows paid on or before the curve reference date are ignored.
SwapValue priceOis(const OisSwap& swap, const DiscountCurve& forwarding, const DiscountCurve& discounting);
SwapValue priceSwap(const VanillaSwap& swap, const DiscountCurve& forwarding, const DiscountCurve& discounting);
/// FRA value today and the forward rate it references.
double priceFra(const Fra& fra, const DiscountCurve& forwarding, const DiscountCurve& discounting,
                double* forwardRate = nullptr);

}  // namespace mcb
