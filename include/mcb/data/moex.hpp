#pragma once
// Moscow Exchange (MOEX) data from the public ISS API:
//
//   G-curve   the exchange's official zero-coupon yield curve for Russian
//             government bonds (OFZ), published as Nelson-Siegel parameters
//             plus nine Gaussian "humps".
//   OFZ       live prices of the government bonds themselves.  Each fixed
//             coupon bond is priced off the G-curve; the gap between market
//             and model is the bond's rich/cheap signal.
//   Futures   RUONIA-index and key-rate futures, which reveal the path of
//             overnight rates the market is pricing.
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/curves/yield_curve.hpp"
#include "mcb/io/json.hpp"
#include "mcb/pricing/bonds.hpp"

namespace mcb {

// ----------------------------------------------------------------- G-curve ---
struct GCurveParams {
  Date date;
  std::string time;
  double b1 = 0, b2 = 0, b3 = 0, t1 = 1;  // Nelson-Siegel part (basis points)
  double g[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};  // Gaussian hump weights (basis points)
};

/// Continuously compounded zero rate G(t) in basis points:
///   G(t) = b1 + (b2 + b3) (tau/t)(1 - e^{-t/tau}) - b3 e^{-t/tau} + sum_i g_i exp(-(t - a_i)^2 / b_i^2)
/// with fixed knots a_1 = 0, a_2 = 0.6, a_{i+1} = a_i + a_2 k^{i-1}, b_1 = a_2, b_{i+1} = b_i k, k = 1.6.
double gCurveRateBp(const GCurveParams& p, double years);
/// The published "KBD" value: annually compounded zero yield exp(G/10000) - 1 (decimal).
double gCurveYield(const GCurveParams& p, double years);

class GCurve final : public DiscountCurve {
 public:
  /// referenceDate is the date from which time is measured (bond settlement date).
  GCurve(const GCurveParams& params, Date referenceDate) : p_(params), ref_(referenceDate) {}
  Date referenceDate() const override { return ref_; }
  double logDiscountT(double t) const override { return t <= 0.0 ? 0.0 : -gCurveRateBp(p_, t) / 1e4 * t; }
  const GCurveParams& params() const { return p_; }

 private:
  GCurveParams p_;
  Date ref_;
};

struct MoexZcyc {
  GCurveParams params;
  std::vector<std::pair<double, double>> publishedYields;  // (years, yield in percent) as published by MOEX
};
MoexZcyc parseMoexZcyc(const std::string& jsonText);

// --------------------------------------------------------------------- OFZ ---
struct OfzQuote {
  std::string secid, name;
  Date maturity, nextCoupon, settle;
  double couponPercent = 0;   // annual coupon rate, percent
  double couponValue = 0;     // coupon amount in currency per bond
  double faceValue = 1000;
  int couponPeriodDays = 182;
  double accrued = 0;         // accrued interest in currency per bond, as published
  double cleanPrice = 0;      // percent of face
  // MOEX's own analytics for the same price (NaN when not published)
  double moexYield = 0, moexDurationDays = 0, moexZSpreadBp = 0, moexGSpreadBp = 0;
};

/// Fixed-coupon rouble OFZ with a market price, from the TQOB board snapshot.
std::vector<OfzQuote> parseMoexOfz(const std::string& jsonText);
/// Cash-flow schedule of an OFZ: coupons every couponPeriodDays up to maturity.
/// Returns false for bonds whose schedule is irregular.
bool makeOfzBond(const OfzQuote& q, Bond& out);

/// Dirty price (per 100 face) of a bond on the G-curve with a spread added to
/// the annually compounded zero yields: sum CF / (1 + Y(t) + spread)^t.  This
/// is the convention behind the Z-spread MOEX publishes.
double ofzPriceWithSpread(const Bond& bond, const GCurveParams& curve, Date settle, double spread);

struct OfzAnalysis {
  OfzQuote quote;
  double dirtyPrice = 0;        // per 100 face
  double yield = 0;             // effective annual yield (decimal), ours
  double durationYears = 0;     // Macaulay
  double modelCleanPrice = 0;   // G-curve price minus accrued
  double priceGap = 0;          // market - model, per 100 face (negative = cheap)
  double gSpreadBp = 0;         // yield minus G-curve yield at the bond's duration
  double zSpreadBp = 0;         // constant spread over the G-curve that reprices the bond (annual compounding)
  double fittedSpreadBp = 0;    // spread explained by coupon and duration (cross-sectional fit)
  double residualBp = 0;        // zSpread - fitted: > 0 cheap, < 0 rich
  double priceEdge = 0;         // the residual in price terms (points per 100 face): residual x duration
  std::string signal;           // BUY / SELL / HOLD
};

struct OfzReport {
  std::vector<OfzAnalysis> bonds;
  Date settle;                           // settlement date used for all bonds
  double spreadSlopePerCouponPoint = 0;  // bp of spread per 1 point of coupon
  double spreadSlopePerYear = 0;         // bp of spread per year of duration
  double residualStdevBp = 0;
  double thresholdBp = 0;                // |residual| above which a signal is raised ...
  double minPriceEdge = 0.25;            // ... provided it is worth at least this many price points
  // agreement of our analytics with the exchange's
  double maxYieldDiffBp = 0, maxGSpreadDiffBp = 0, maxZSpreadDiffBp = 0, maxAccruedDiff = 0;
  int compared = 0;
};

/// Prices every bond off the G-curve and ranks them rich/cheap.
///
/// The settlement date matters for short bonds and depends on the trading
/// session the prices come from (weekend sessions settle differently).  When
/// the exchange publishes its own yields, the settlement date (the published
/// one or up to three days earlier) that best reproduces them is used.
OfzReport analyzeOfz(const std::vector<OfzQuote>& quotes, const GCurveParams& curve);

// ----------------------------------------------------------------- futures ---
struct MoexFuture {
  std::string secid, name, asset;
  Date lastTradeDate;
  double settlePrice = 0;
  double openInterest = 0;
};
/// RUONIA-index and key-rate futures from the FORTS securities snapshot.
std::vector<MoexFuture> parseMoexRateFutures(const std::string& jsonText);

struct ImpliedRate {
  std::string label;
  Date start, end;
  double rate = 0;  // decimal
};
/// Compounded RUONIA implied between consecutive index-futures expiries:
/// (F2/F1 - 1) * 365/days.
std::vector<ImpliedRate> ruoniaImpliedForwards(const std::vector<MoexFuture>& futures);

}  // namespace mcb
