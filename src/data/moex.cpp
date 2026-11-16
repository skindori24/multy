#include "mcb/data/moex.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>

#include "mcb/core/error.hpp"
#include "mcb/math/solver.hpp"
#include "mcb/stats/linear_model.hpp"

namespace mcb {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// One block of an ISS response: {"columns": [...], "data": [[...], ...]}.
struct IssBlock {
  std::map<std::string, std::size_t> col;
  const Json::Array* rows = nullptr;

  IssBlock(const Json& doc, const char* name) {
    const Json& block = doc[name];
    MCB_REQUIRE(block.isObject() && block["columns"].isArray() && block["data"].isArray(),
                "MOEX ISS: block '" << name << "' is missing");
    const auto& cols = block["columns"].items();
    for (std::size_t i = 0; i < cols.size(); ++i) col[cols[i].strOr("")] = i;
    rows = &block["data"].items();
  }
  bool has(const std::string& c) const { return col.count(c) > 0; }
  const Json& cell(const Json& row, const std::string& c) const {
    static const Json null;
    const auto it = col.find(c);
    if (it == col.end() || !row.isArray() || it->second >= row.size()) return null;
    return row[it->second];
  }
  double num(const Json& row, const std::string& c) const { return cell(row, c).numberOr(kNaN); }
  std::string str(const Json& row, const std::string& c) const { return cell(row, c).strOr(""); }
};

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

// ----------------------------------------------------------------- G-curve ---

double gCurveRateBp(const GCurveParams& p, double t) {
  if (t < 1e-6) t = 1e-6;
  const double x = t / p.t1, e = std::exp(-x);
  double g = p.b1 + (p.b2 + p.b3) * (1.0 - e) / x - p.b3 * e;
  // Gaussian humps on a fixed geometric grid of knots.
  const double k = 1.6;
  double a = 0.0, b = 0.6, step = 0.6;
  for (int i = 0; i < 9; ++i) {
    g += p.g[i] * std::exp(-(t - a) * (t - a) / (b * b));
    a = i == 0 ? 0.6 : a + step * k;  // a_2 = 0.6, then a_{i+1} = a_i + 0.6 k^{i-1}
    if (i > 0) step *= k;
    b *= k;
  }
  return g;
}

double gCurveYield(const GCurveParams& p, double years) { return std::exp(gCurveRateBp(p, years) / 1e4) - 1.0; }

MoexZcyc parseMoexZcyc(const std::string& jsonText) {
  const Json doc = Json::parse(jsonText);
  MoexZcyc out;
  const IssBlock params(doc, "params");
  MCB_REQUIRE(!params.rows->empty(), "MOEX G-curve: no parameters published");
  const Json& row = params.rows->back();
  out.params.date = Date::parse(params.str(row, "tradedate"));
  out.params.time = params.str(row, "tradetime");
  out.params.b1 = params.num(row, "B1");
  out.params.b2 = params.num(row, "B2");
  out.params.b3 = params.num(row, "B3");
  out.params.t1 = params.num(row, "T1");
  for (int i = 0; i < 9; ++i) out.params.g[i] = params.num(row, "G" + std::to_string(i + 1));
  MCB_REQUIRE(std::isfinite(out.params.b1) && out.params.t1 > 0.0, "MOEX G-curve: malformed parameters");
  if (doc.contains("yearyields")) {
    const IssBlock yy(doc, "yearyields");
    for (const auto& r : *yy.rows) {
      const double period = yy.num(r, "period"), value = yy.num(r, "value");
      if (std::isfinite(period) && std::isfinite(value)) out.publishedYields.emplace_back(period, value);
    }
  }
  return out;
}

// --------------------------------------------------------------------- OFZ ---

std::vector<OfzQuote> parseMoexOfz(const std::string& jsonText) {
  const Json doc = Json::parse(jsonText);
  const IssBlock sec(doc, "securities"), md(doc, "marketdata");
  std::map<std::string, const Json*> mdBy, yBy;
  for (const auto& r : *md.rows) mdBy[md.str(r, "SECID")] = &r;
  std::unique_ptr<IssBlock> yields;
  if (doc.contains("marketdata_yields")) {
    yields = std::make_unique<IssBlock>(doc, "marketdata_yields");
    for (const auto& r : *yields->rows) yBy[yields->str(r, "SECID")] = &r;
  }

  std::vector<OfzQuote> out;
  for (const auto& r : *sec.rows) {
    // Plain fixed-coupon rouble bonds only: no floaters, no inflation linkers, no amortisers.
    // (COUPON_DETAILS "Фиксированный с известными купонами", FACEVALUE_TYPE "Фиксированный".)
    if (sec.str(r, "FACEUNIT") != "SUR") continue;
    const std::string details = sec.str(r, "COUPON_DETAILS"), faceType = sec.str(r, "FACEVALUE_TYPE");
    if (sec.has("COUPON_DETAILS") && !contains(details, "\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD1\x8B\xD0\xBC\xD0\xB8")) continue;  // "известными"
    if (sec.has("FACEVALUE_TYPE") && contains(faceType, "\xD0\x98\xD0\xBD\xD0\xB4\xD0\xB5\xD0\xBA\xD1\x81")) continue;                                 // "Индекс..."
    OfzQuote q;
    q.secid = sec.str(r, "SECID");
    q.name = sec.str(r, "SECNAME");
    if (q.name.empty()) q.name = sec.str(r, "SHORTNAME");
    q.couponPercent = sec.num(r, "COUPONPERCENT");
    q.couponValue = sec.num(r, "COUPONVALUE");
    q.faceValue = sec.num(r, "FACEVALUE");
    q.couponPeriodDays = static_cast<int>(sec.cell(r, "COUPONPERIOD").numberOr(0));
    q.accrued = sec.num(r, "ACCRUEDINT");
    if (!Date::tryParse(sec.str(r, "MATDATE"), q.maturity) || !Date::tryParse(sec.str(r, "NEXTCOUPON"), q.nextCoupon) ||
        !Date::tryParse(sec.str(r, "SETTLEDATE"), q.settle))
      continue;
    if (!(q.couponPercent > 0.0) || !(q.couponValue > 0.0) || !(q.faceValue > 0.0) || q.couponPeriodDays <= 0) continue;

    // Price: the one MOEX used for its own yield calculation if available,
    // otherwise the last trade, then the day's volume-weighted average.
    q.moexYield = q.moexDurationDays = q.moexZSpreadBp = q.moexGSpreadBp = kNaN;
    double price = kNaN;
    if (const auto it = yBy.find(q.secid); it != yBy.end()) {
      price = yields->num(*it->second, "PRICE");
      q.moexYield = yields->num(*it->second, "EFFECTIVEYIELD");
      q.moexDurationDays = yields->num(*it->second, "DURATION");
      q.moexZSpreadBp = yields->num(*it->second, "ZSPREADBP");
      q.moexGSpreadBp = yields->num(*it->second, "GSPREADBP");
    }
    if (const auto it = mdBy.find(q.secid); it != mdBy.end()) {
      if (!(price > 0.0)) price = md.num(*it->second, "LAST");
      if (!(price > 0.0)) price = md.num(*it->second, "WAPRICE");
      if (!(price > 0.0)) price = md.num(*it->second, "MARKETPRICE");
    }
    if (!(price > 0.0)) continue;  // no market price today
    q.cleanPrice = price;
    out.push_back(std::move(q));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.maturity < b.maturity; });
  return out;
}

bool makeOfzBond(const OfzQuote& q, Bond& out) {
  if (q.maturity <= q.settle || q.nextCoupon <= q.settle) return false;
  std::vector<Date> dates;
  for (Date d = q.nextCoupon; d <= q.maturity; d = d.addDays(q.couponPeriodDays)) dates.push_back(d);
  if (dates.empty() || dates.back() != q.maturity) return false;  // irregular schedule
  const double perHundred = 100.0 / q.faceValue;
  const std::vector<double> amounts(dates.size(), q.couponValue * perHundred);
  out = makeScheduledBond(q.secid, q.nextCoupon.addDays(-q.couponPeriodDays), dates, amounts, q.couponPercent / 100.0,
                          static_cast<int>(std::lround(365.0 / q.couponPeriodDays)));
  out.name = q.name;
  return true;
}

double ofzPriceWithSpread(const Bond& bond, const GCurveParams& curve, Date settle, double spread) {
  auto df = [&](Date d) {
    const double t = (d - settle) / 365.0;
    return std::pow(1.0 + gCurveYield(curve, t) + spread, -t);
  };
  double pv = 0.0;
  for (const auto& c : bond.coupons)
    if (c.end > settle) pv += c.amount * df(c.pay);
  return pv + bond.redemption * df(bond.redemptionPay);
}

namespace {

// Largest gap (bp) between our yields and the exchange's when every bond
// settles `shift` days before its published settlement date.
double yieldMismatchBp(const std::vector<OfzQuote>& quotes, int shift, int& compared) {
  double worst = 0.0;
  compared = 0;
  for (OfzQuote q : quotes) {
    if (!std::isfinite(q.moexYield) || !(q.moexYield > 0.0)) continue;
    q.settle = q.settle.addDays(-shift);
    Bond bond;
    if (!makeOfzBond(q, bond)) continue;
    try {
      const double y = yieldFromDirtyPrice(bond, q.settle, q.cleanPrice + accruedInterest(bond, q.settle),
                                           YieldConvention::AnnualAct365);
      worst = std::max(worst, std::fabs(y * 100.0 - q.moexYield) * 100.0);
      ++compared;
    } catch (const Error&) {
    }
  }
  return worst;
}

}  // namespace

OfzReport analyzeOfz(const std::vector<OfzQuote>& input, const GCurveParams& curve) {
  OfzReport rep;
  // Pick the settlement date consistent with the exchange's own yields.
  int bestShift = 0, compared = 0;
  double bestGap = yieldMismatchBp(input, 0, compared);
  if (compared >= 5) {
    for (int shift = 1; shift <= 3; ++shift) {
      int n = 0;
      const double gap = yieldMismatchBp(input, shift, n);
      if (n == compared && gap < bestGap) bestGap = gap, bestShift = shift;
    }
  }
  std::vector<OfzQuote> quotes = input;
  for (auto& q : quotes) q.settle = q.settle.addDays(-bestShift);
  if (!quotes.empty()) rep.settle = quotes.front().settle;

  for (const auto& q : quotes) {
    Bond bond;
    if (!makeOfzBond(q, bond)) continue;
    OfzAnalysis a;
    a.quote = q;
    const double accrued = accruedInterest(bond, q.settle);
    a.dirtyPrice = q.cleanPrice + accrued;
    a.yield = yieldFromDirtyPrice(bond, q.settle, a.dirtyPrice, YieldConvention::AnnualAct365);
    a.durationYears = bondRisk(bond, q.settle, a.yield, YieldConvention::AnnualAct365).macaulayDuration;
    const GCurve g(curve, q.settle);
    a.modelCleanPrice = dirtyPriceFromCurve(bond, g, q.settle) - accrued;
    a.priceGap = q.cleanPrice - a.modelCleanPrice;
    a.gSpreadBp = (a.yield - gCurveYield(curve, a.durationYears)) * 1e4;
    const RootResult z = findRoot(
        [&](double s) { return ofzPriceWithSpread(bond, curve, q.settle, s) - a.dirtyPrice; }, 0.0, 1e-3, -0.5, 2.0,
        1e-13);
    a.zSpreadBp = z.converged ? z.x * 1e4 : kNaN;
    if (!std::isfinite(a.zSpreadBp)) continue;

    if (bestShift == 0)  // the published accrued interest refers to the published settlement date
      rep.maxAccruedDiff = std::max(rep.maxAccruedDiff, std::fabs(accrued * q.faceValue / 100.0 - q.accrued));
    if (std::isfinite(q.moexYield) && q.moexYield > 0.0) {
      rep.maxYieldDiffBp = std::max(rep.maxYieldDiffBp, std::fabs(a.yield * 100.0 - q.moexYield) * 100.0);
      ++rep.compared;
    }
    if (std::isfinite(q.moexGSpreadBp)) rep.maxGSpreadDiffBp = std::max(rep.maxGSpreadDiffBp, std::fabs(a.gSpreadBp - q.moexGSpreadBp));
    if (std::isfinite(q.moexZSpreadBp)) rep.maxZSpreadDiffBp = std::max(rep.maxZSpreadDiffBp, std::fabs(a.zSpreadBp - q.moexZSpreadBp));
    rep.bonds.push_back(std::move(a));
  }

  // Spreads to the curve are not pure mispricing: low-coupon bonds trade at a
  // discount and are taxed differently, and liquidity varies with maturity.
  // Regress the spread on coupon and duration and call a bond rich or cheap
  // only relative to bonds like it.
  const auto n = static_cast<Eigen::Index>(rep.bonds.size());
  if (n >= 6) {
    Eigen::MatrixXd X(n, 2);
    Eigen::VectorXd y(n);
    for (Eigen::Index i = 0; i < n; ++i) {
      const auto& b = rep.bonds[static_cast<std::size_t>(i)];
      X(i, 0) = b.quote.couponPercent;
      X(i, 1) = b.durationYears;
      y[i] = b.zSpreadBp;
    }
    try {
      const OlsResult fit = ols(X, y);
      rep.spreadSlopePerCouponPoint = fit.beta[1];
      rep.spreadSlopePerYear = fit.beta[2];
      rep.residualStdevBp = std::sqrt(fit.residuals.squaredNorm() / static_cast<double>(n - 3));
      for (Eigen::Index i = 0; i < n; ++i) {
        auto& b = rep.bonds[static_cast<std::size_t>(i)];
        b.residualBp = fit.residuals[i];
        b.fittedSpreadBp = b.zSpreadBp - b.residualBp;
      }
    } catch (const Error&) {
      for (auto& b : rep.bonds) b.residualBp = b.zSpreadBp;  // degenerate cross-section: fall back to the raw spread
    }
  } else {
    for (auto& b : rep.bonds) b.residualBp = b.zSpreadBp;
  }
  // A signal needs a residual that is large both in absolute terms and relative to the cross-section.
  // It must also be worth something in price terms: 50 bp on a bond maturing in
  // two months is a few cents, less than the bid-offer spread.
  rep.thresholdBp = std::max(10.0, 1.25 * rep.residualStdevBp);
  for (auto& b : rep.bonds) {
    b.priceEdge = b.residualBp / 100.0 * b.durationYears / (1.0 + b.yield);  // bp x modified duration
    const bool material = std::fabs(b.priceEdge) >= rep.minPriceEdge;
    b.signal = (material && b.residualBp > rep.thresholdBp) ? "BUY"
               : ((material && b.residualBp < -rep.thresholdBp) ? "SELL" : "HOLD");
  }
  return rep;
}

// ----------------------------------------------------------------- futures ---

std::vector<MoexFuture> parseMoexRateFutures(const std::string& jsonText) {
  const Json doc = Json::parse(jsonText);
  const IssBlock sec(doc, "securities");
  std::vector<MoexFuture> out;
  for (const auto& r : *sec.rows) {
    MoexFuture f;
    f.asset = sec.str(r, "ASSETCODE");
    if (f.asset != "RUONIA" && f.asset != "KEYRATE") continue;
    f.secid = sec.str(r, "SECID");
    f.name = sec.str(r, "SHORTNAME");
    f.settlePrice = sec.num(r, "PREVSETTLEPRICE");
    if (!(f.settlePrice > 0.0)) f.settlePrice = sec.num(r, "LASTSETTLEPRICE");
    f.openInterest = sec.cell(r, "PREVOPENPOSITION").numberOr(0.0);
    if (!Date::tryParse(sec.str(r, "LASTTRADEDATE"), f.lastTradeDate) || !(f.settlePrice > 0.0)) continue;
    out.push_back(std::move(f));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.asset != b.asset ? a.asset < b.asset : a.lastTradeDate < b.lastTradeDate;
  });
  return out;
}

std::vector<ImpliedRate> ruoniaImpliedForwards(const std::vector<MoexFuture>& futures) {
  std::vector<const MoexFuture*> idx;
  for (const auto& f : futures)
    if (f.asset == "RUONIA") idx.push_back(&f);
  std::sort(idx.begin(), idx.end(), [](auto a, auto b) { return a->lastTradeDate < b->lastTradeDate; });
  std::vector<ImpliedRate> out;
  for (std::size_t i = 0; i + 1 < idx.size(); ++i) {
    const int days = idx[i + 1]->lastTradeDate - idx[i]->lastTradeDate;
    if (days <= 0) continue;
    ImpliedRate r;
    r.label = idx[i]->name + " -> " + idx[i + 1]->name;
    r.start = idx[i]->lastTradeDate;
    r.end = idx[i + 1]->lastTradeDate;
    // The contracts settle on the compounded RUONIA index, so the ratio of
    // two prices is the compounded growth of overnight money between expiries.
    r.rate = (idx[i + 1]->settlePrice / idx[i]->settlePrice - 1.0) * 365.0 / days;
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace mcb
