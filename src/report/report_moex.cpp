// Report section: Moscow Exchange. The official G-curve, every fixed-coupon
// OFZ priced against it (rich/cheap), and the rate path implied by futures.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "mcb/core/error.hpp"
#include "mcb/data/moex.hpp"
#include "mcb/io/csv.hpp"
#include "sections.hpp"

namespace mcb::report {

Json moexSection(Context& ctx) {
  const char* url = "https://iss.moex.com/iss/reference/";
  try {
    ctx.info("MOEX: G-curve, OFZ bonds and rate futures");
    const std::string dir = joinPath(ctx.opt.dataDir, "moex");
    MCB_REQUIRE(fileExists(joinPath(dir, "zcyc.json")), "no MOEX G-curve file in " << dir);
    const MoexZcyc zcyc = parseMoexZcyc(readTextFile(joinPath(dir, "zcyc.json")));
    const GCurveParams& g = zcyc.params;

    Json moex = Json::object().set("available", true).set("asOf", g.date.iso()).set("curveTime", g.time);

    // ---- the curve ------------------------------------------------------------
    std::vector<double> years, yields;
    for (double t = 0.25; t <= 20.0 + 1e-9; t += 0.25) {
      years.push_back(t);
      yields.push_back(roundTo(gCurveYield(g, t) * 100.0, 4));
    }
    Json published = Json::array();
    double worst = 0.0;
    for (const auto& [t, v] : zcyc.publishedYields) {
      const double ours = gCurveYield(g, t) * 100.0;
      worst = std::max(worst, std::fabs(ours - v) * 100.0);
      published.push(Json::object().set("years", t).set("published", num(v, 4)).set("ours", num(ours, 4)));
    }
    moex.set("curve", Json::object().set("years", years).set("yield", yields)).set("published", published);
    if (!zcyc.publishedYields.empty())
      ctx.check("MOEX", "G-curve formula reproduces the yields published by the exchange", worst, 0.02, "bp",
                std::to_string(zcyc.publishedYields.size()) + " tenors; the exchange publishes four decimals");
    ctx.source("MOEX G-curve (OFZ zero-coupon yield curve)", g.date.iso() + " " + g.time, "ok", "", url);

    // ---- bonds ------------------------------------------------------------------
    if (fileExists(joinPath(dir, "ofz.json"))) {
      try {
        const auto quotes = parseMoexOfz(readTextFile(joinPath(dir, "ofz.json")));
        const OfzReport rep = analyzeOfz(quotes, g);
        MCB_REQUIRE(!rep.bonds.empty(), "no priced fixed-coupon OFZ in the snapshot");
        Json bonds = Json::array();
        for (const auto& b : rep.bonds) {
          bonds.push(Json::object()
                         .set("secid", b.quote.secid)
                         .set("name", b.quote.name)
                         .set("maturity", b.quote.maturity.iso())
                         .set("coupon", num(b.quote.couponPercent, 2))
                         .set("price", num(b.quote.cleanPrice, 3))
                         .set("modelPrice", num(b.modelCleanPrice, 3))
                         .set("priceGap", num(b.priceGap, 3))
                         .set("yield", num(b.yield * 100.0, 3))
                         .set("curveYield", num(gCurveYield(g, b.durationYears) * 100.0, 3))
                         .set("duration", num(b.durationYears, 2))
                         .set("gSpreadBp", num(b.gSpreadBp, 1))
                         .set("zSpreadBp", num(b.zSpreadBp, 1))
                         .set("fittedSpreadBp", num(b.fittedSpreadBp, 1))
                         .set("residualBp", num(b.residualBp, 1))
                         .set("priceEdge", num(b.priceEdge, 3))
                         .set("moexZSpreadBp", num(b.quote.moexZSpreadBp, 0))
                         .set("moexGSpreadBp", num(b.quote.moexGSpreadBp, 0))
                         .set("signal", b.signal));
          if (b.signal != "HOLD") {
            char why[320];
            std::snprintf(why, sizeof why,
                          "Z-spread %+.0f bp vs %+.0f bp for bonds with similar coupon and duration: %.0f bp %s "
                          "(threshold %.0f bp), worth about %.2f price points; market price %.2f, G-curve price %.2f",
                          b.zSpreadBp, b.fittedSpreadBp, std::fabs(b.residualBp), b.residualBp > 0 ? "cheap" : "rich",
                          rep.thresholdBp, std::fabs(b.priceEdge), b.quote.cleanPrice, b.modelCleanPrice);
            ctx.signals.push(Json::object()
                                 .set("market", "Russian government bonds (OFZ)")
                                 .set("asset", b.quote.name.empty() ? b.quote.secid : b.quote.name)
                                 .set("signal", b.signal)
                                 .set("horizon", "relative value")
                                 .set("edge", num(b.residualBp, 0))
                                 .set("edgeUnit", "bp cheap (+) / rich (-)")
                                 .set("confidence", num(std::fabs(b.residualBp) / std::max(1.0, rep.residualStdevBp), 2))
                                 .set("confidenceLabel", "residual / cross-section st.dev.")
                                 .set("why", std::string(why)));
          }
        }
        moex.set("ofz", Json::object()
                            .set("settle", rep.settle.iso())
                            .set("bonds", bonds)
                            .set("spreadPerCouponPoint", num(rep.spreadSlopePerCouponPoint, 2))
                            .set("spreadPerDurationYear", num(rep.spreadSlopePerYear, 2))
                            .set("residualStdevBp", num(rep.residualStdevBp, 1))
                            .set("thresholdBp", num(rep.thresholdBp, 1))
                            .set("minPriceEdge", num(rep.minPriceEdge, 2))
                            .set("rule", "Each bond's Z-spread to the G-curve is compared with the spread of bonds "
                                         "with a similar coupon and duration (cross-sectional regression). BUY when it "
                                         "is cheaper than that by more than the threshold (1.25 standard deviations, at "
                                         "least 10 bp) and the gap is worth at least 0.25 price points; SELL when "
                                         "richer by the same margins."));
        if (rep.compared > 0) {
          ctx.check("MOEX", "OFZ yields: ours vs the exchange's", rep.maxYieldDiffBp, 0.5, "bp",
                    std::to_string(rep.compared) + " bonds, effective annual yield from the same price");
          ctx.check("MOEX", "OFZ G-spreads: ours vs the exchange's", rep.maxGSpreadDiffBp, 1.5, "bp",
                    "the exchange publishes whole basis points");
          ctx.check("MOEX", "OFZ Z-spreads: ours vs the exchange's", rep.maxZSpreadDiffBp, 1.5, "bp",
                    "the exchange publishes whole basis points");
        }
        ctx.source("MOEX OFZ prices (board TQOB)", rep.settle.iso() + " (settlement)", "ok",
                   std::to_string(rep.bonds.size()) + " fixed-coupon bonds", url);
      } catch (const std::exception& e) {
        moex.set("ofz", unavailable(e.what()));
        ctx.source("MOEX OFZ prices (board TQOB)", "", "error", e.what(), url);
      }
    }

    // ---- futures ------------------------------------------------------------------
    if (fileExists(joinPath(dir, "forts.json"))) {
      try {
        const auto futures = parseMoexRateFutures(readTextFile(joinPath(dir, "forts.json")));
        Json list = Json::array();
        for (const auto& f : futures)
          list.push(Json::object()
                        .set("secid", f.secid)
                        .set("name", f.name)
                        .set("asset", f.asset)
                        .set("expiry", f.lastTradeDate.iso())
                        .set("settle", num(f.settlePrice, 4))
                        .set("openInterest", f.openInterest));
        Json implied = Json::array();
        for (const auto& r : ruoniaImpliedForwards(futures))
          implied.push(Json::object()
                           .set("label", r.label)
                           .set("start", r.start.iso())
                           .set("end", r.end.iso())
                           .set("rate", num(r.rate * 100.0, 3)));
        moex.set("futures", Json::object().set("contracts", list).set("ruoniaImplied", implied));
        ctx.source("MOEX rate futures (RUONIA index, key rate)", g.date.iso(), "ok",
                   std::to_string(futures.size()) + " contracts", url);
      } catch (const std::exception& e) {
        moex.set("futures", unavailable(e.what()));
      }
    }
    return moex;
  } catch (const std::exception& e) {
    ctx.info(std::string("  MOEX section unavailable: ") + e.what());
    ctx.source("MOEX (Moscow Exchange ISS)", "", "missing", e.what(), url);
    return unavailable(e.what());
  }
}

}  // namespace mcb::report
