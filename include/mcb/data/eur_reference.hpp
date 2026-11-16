#pragma once
// The frozen EUR reference market (11 Dec 2012) and the three-curve EUR
// multicurve set-up built from it:
//
//   EUR-OIS   EONIA overnight curve   deposits + OIS            self-discounted
//   EUR-6M    Euribor 6M projection   deposit + FRAs + swaps    discounted on EUR-OIS
//   EUR-3M    Euribor 3M projection   deposit + futures + swaps discounted on EUR-OIS
//                                     (swap rates = 6M swap rate - 3s6s basis)
//
// The file format is generic (see data/reference/eur_20121211.csv), so other
// snapshots with the same instrument kinds can be loaded the same way.
#include <string>
#include <vector>

#include "mcb/core/date.hpp"
#include "mcb/curves/conventions.hpp"
#include "mcb/curves/curve_engine.hpp"

namespace mcb {

struct MarketInstrument {
  std::string curve, kind, id, a, b, link;
  double quote = 0.0;  // as written in the file: percent for rates, price for futures
};

struct ReferenceMarket {
  Date asOf;
  std::vector<MarketInstrument> instruments;
};

ReferenceMarket loadReferenceMarket(const std::string& path);
ReferenceMarket parseReferenceMarket(const std::string& csvText);

struct EurSetup {
  SwapConventions swap6m, swap3m;
  OisConventions ois;
};

/// Builds the EUR curve engine and the quote map (decimal rates / prices).
/// parCoupons selects how floating coupons project the index (see SwapConventions).
CurveEngine makeEurEngine(const ReferenceMarket& market, Interpolation interpolation, bool parCoupons,
                          QuoteMap& quotes, EurSetup* setup = nullptr);

}  // namespace mcb
