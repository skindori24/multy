#pragma once
// Live USD market data.
//
//   USD-SOFR  overnight (SOFR) swap curve, bootstrapped from the end-of-day
//             par swap rates published by Eris Innovations.  The same
//             publisher also releases its own discount factors, which gives
//             an independent curve to compare ours with every day.
//
//   USD-TSY   US Treasury curve, bootstrapped from the Treasury's daily par
//             yield ("constant maturity") curve: bills as simple yields,
//             1Y and longer as semi-annual par bonds.
#include <map>
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/curves/conventions.hpp"
#include "mcb/curves/curve_engine.hpp"

namespace mcb {

// ----------------------------------------------------------------- SOFR -----
struct ErisParQuote {
  std::string symbol;  // e.g. "SOFR10Y"
  Period tenor;
  Date effective;      // accrual start (T+2)
  Date alignment;      // unadjusted maturity
  Date lastPayment;    // final payment date
  double rate = 0.0;   // par rate, decimal
};

struct ErisParCurve {
  Date asOf;
  std::vector<ErisParQuote> quotes;
};

ErisParCurve parseErisParCurve(const std::string& csvText);
/// Eris end-of-day discount factors by date.
std::map<Date, double> parseErisDiscountFactors(const std::string& csvText);

/// Single self-discounted curve "USD-SOFR"; quote ids are the Eris symbols.
CurveEngine makeSofrEngine(const ErisParCurve& par, Interpolation interpolation, QuoteMap& quotes);
/// The swap behind one par quote (useful for pricing and reporting).
OisSwap makeSofrSwap(const ErisParQuote& q, double fixedRate = 0.0, bool payFixed = true, double notional = 1.0);

struct ErisComparison {
  int datesCompared = 0;
  double maxAbsDfDiff = 0.0;        // max |our DF - Eris DF| over all dates up to the last pillar
  Date worstDate;
  double maxZeroRateDiffBp = 0.0;   // the same in zero-rate terms (basis points)
  double maxPillarDfDiff = 0.0;     // restricted to our pillar dates
  /// Largest error (bp) when Eris's own par swaps are repriced from Eris's own
  /// discount factors with our schedules and formulas: a pure test of conventions.
  double maxConventionErrorBp = 0.0;
};
ErisComparison compareWithEris(const YieldCurve& ours, const std::map<Date, double>& erisDfs, const ErisParCurve& par);

// ------------------------------------------------------------- Treasury -----
struct TreasuryParRow {
  Date date;
  std::vector<std::pair<Period, double>> yields;  // tenor -> par yield (decimal), short to long
};

/// Parses the Treasury "daily par yield curve" CSV; rows sorted by date ascending.
std::vector<TreasuryParRow> parseTreasuryParYields(const std::string& csvText);
/// Parses the same data from FRED's multi-series CSV (DGS1MO, DGS3MO, ... DGS30).
std::vector<TreasuryParRow> parseFredParYields(const std::string& csvText);

/// Single curve "USD-TSY"; quote ids are "UST_<tenor>".
CurveEngine makeTreasuryEngine(const TreasuryParRow& row, Interpolation interpolation, QuoteMap& quotes);

/// Par yield of a bond starting at `start` with the given tenor and coupon
/// frequency, read off a curve: c = f (D(start) - D(T)) / sum D(t_i).
double parYield(const DiscountCurve& curve, Date start, Period tenor, int frequency = 2);

// ------------------------------------------------------------ SOFR fixing ---
struct RateFixing {
  Date date;
  double rate = 0.0;  // decimal
};
/// Latest SOFR fixing from the New York Fed JSON feed.
RateFixing parseNyFedSofr(const std::string& jsonText);

}  // namespace mcb
