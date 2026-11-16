#include "mcb/data/usd_market.hpp"

#include <algorithm>
#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/io/json.hpp"

namespace mcb {

// ----------------------------------------------------------------- SOFR -----

ErisParCurve parseErisParCurve(const std::string& csvText) {
  const CsvTable t = parseCsv(csvText);
  const int cSym = t.requireCol("Symbol"), cEval = t.requireCol("EvaluationDate"), cEff = t.requireCol("EffectiveDate");
  const int cAlign = t.requireCol("CashFlowAlignmentDate"), cMat = t.requireCol("MaturityDate");
  const int cFair = t.requireCol("FairCoupon (%)");
  ErisParCurve curve;
  for (const auto& row : t.rows) {
    if (row.size() <= static_cast<std::size_t>(cFair)) continue;
    ErisParQuote q;
    q.symbol = row[static_cast<std::size_t>(cSym)];
    MCB_REQUIRE(q.symbol.rfind("SOFR", 0) == 0, "Eris par curve: unexpected symbol '" << q.symbol << "'");
    q.tenor = Period::parse(q.symbol.substr(4));
    q.effective = Date::parse(row[static_cast<std::size_t>(cEff)]);
    q.alignment = Date::parse(row[static_cast<std::size_t>(cAlign)]);
    q.lastPayment = Date::parse(row[static_cast<std::size_t>(cMat)]);
    q.rate = parseDouble(row[static_cast<std::size_t>(cFair)], "Eris fair coupon") / 100.0;
    const Date eval = Date::parse(row[static_cast<std::size_t>(cEval)]);
    MCB_REQUIRE(curve.asOf.isNull() || curve.asOf == eval, "Eris par curve: mixed evaluation dates");
    curve.asOf = eval;
    curve.quotes.push_back(std::move(q));
  }
  MCB_REQUIRE(!curve.quotes.empty(), "Eris par curve: no quotes found");
  return curve;
}

std::map<Date, double> parseErisDiscountFactors(const std::string& csvText) {
  const CsvTable t = parseCsv(csvText);
  const auto cDate = static_cast<std::size_t>(t.requireCol("Date"));
  const auto cDf = static_cast<std::size_t>(t.requireCol("DiscountFactor"));
  std::map<Date, double> out;
  for (const auto& row : t.rows) {
    if (row.size() <= cDf) continue;
    Date d;
    double df = 0.0;
    if (Date::tryParse(row[cDate], d) && tryParseDouble(row[cDf], df) && df > 0.0) out[d] = df;
  }
  MCB_REQUIRE(!out.empty(), "Eris discount factors: no rows found");
  return out;
}

OisSwap makeSofrSwap(const ErisParQuote& q, double fixedRate, bool payFixed, double notional) {
  // Built from the published accrual start and unadjusted end: annual
  // periods rolled backward, ACT/360, paid two business days in arrears.
  return makeOisSwap(q.effective, q.alignment, false, OisConventions::sofr(), fixedRate, payFixed, notional);
}

CurveEngine makeSofrEngine(const ErisParCurve& par, Interpolation interpolation, QuoteMap& quotes) {
  CurveDefinition def;
  def.name = "USD-SOFR";
  def.interpolation = interpolation;
  quotes.clear();
  for (const auto& q : par.quotes) {
    const OisSwap swap = makeSofrSwap(q);
    std::vector<OisHelper::Period> periods;
    for (const auto& p : swap.periods) periods.push_back(toHelperPeriod(p));
    auto h = std::make_shared<OisHelper>(std::move(periods));
    h->label = "SOFR OIS " + q.tenor.str();
    h->quoteTerms = {{q.symbol, 1.0}};
    quotes[q.symbol] = q.rate;
    def.helpers.push_back(std::move(h));
  }
  return CurveEngine(par.asOf, {def});
}

ErisComparison compareWithEris(const YieldCurve& ours, const std::map<Date, double>& erisDfs, const ErisParCurve& par) {
  ErisComparison c;
  const Date ref = ours.referenceDate();
  for (const auto& [date, df] : erisDfs) {
    if (date <= ref || date > ours.maxDate()) continue;
    const double mine = ours.discount(date);
    const double diff = std::fabs(mine - df);
    ++c.datesCompared;
    if (diff > c.maxAbsDfDiff) {
      c.maxAbsDfDiff = diff;
      c.worstDate = date;
    }
    const double t = ours.time(date);
    if (t > 7.0 / 365.0) c.maxZeroRateDiffBp = std::max(c.maxZeroRateDiffBp, std::fabs(std::log(mine / df)) / t * 1e4);
  }
  for (const Date& d : ours.pillarDates()) {
    const auto it = erisDfs.find(d);
    if (it != erisDfs.end()) c.maxPillarDfDiff = std::max(c.maxPillarDfDiff, std::fabs(ours.discount(d) - it->second));
  }
  // Reprice each published par swap from the published discount factors.
  // Every date we need is a calendar day present in the file, so a log-linear
  // curve through all of them returns the published values exactly.
  std::vector<Date> dates;
  std::vector<double> dfs;
  for (const auto& [date, df] : erisDfs)
    if (date > ref) {
      dates.push_back(date);
      dfs.push_back(df);
    }
  if (!dates.empty()) {
    const YieldCurve published("Eris", ref, dates, dfs, Interpolation::LogLinear);
    for (const auto& q : par.quotes) {
      if (q.lastPayment > dates.back()) continue;
      const SwapValue v = priceOis(makeSofrSwap(q), published, published);
      c.maxConventionErrorBp = std::max(c.maxConventionErrorBp, std::fabs(v.parRate - q.rate) * 1e4);
    }
  }
  return c;
}

// ------------------------------------------------------------- Treasury -----
namespace {

// Maps a column header to a tenor; returns false for non-yield columns.
bool treasuryTenor(const std::string& header, Period& out) {
  static const std::vector<std::pair<const char*, const char*>> known = {
      {"1 Mo", "1M"},    {"1.5 Month", "6W"}, {"2 Mo", "2M"},   {"3 Mo", "3M"},   {"4 Mo", "4M"},
      {"6 Mo", "6M"},    {"1 Yr", "1Y"},      {"2 Yr", "2Y"},   {"3 Yr", "3Y"},   {"5 Yr", "5Y"},
      {"7 Yr", "7Y"},    {"10 Yr", "10Y"},    {"20 Yr", "20Y"}, {"30 Yr", "30Y"},
      // FRED series ids
      {"DGS1MO", "1M"},  {"DGS3MO", "3M"},    {"DGS6MO", "6M"}, {"DGS1", "1Y"},   {"DGS2", "2Y"},
      {"DGS3", "3Y"},    {"DGS5", "5Y"},      {"DGS7", "7Y"},   {"DGS10", "10Y"}, {"DGS20", "20Y"},
      {"DGS30", "30Y"}};
  for (const auto& [name, tenor] : known)
    if (header == name) {
      out = Period::parse(tenor);
      return true;
    }
  return false;
}

std::vector<TreasuryParRow> parseParTable(const std::string& csvText, const char* dateColumn) {
  const CsvTable t = parseCsv(csvText);
  const auto cDate = static_cast<std::size_t>(t.requireCol(dateColumn));
  std::vector<std::pair<std::size_t, Period>> cols;
  for (std::size_t i = 0; i < t.header.size(); ++i) {
    Period p;
    if (treasuryTenor(t.header[i], p)) cols.emplace_back(i, p);
  }
  MCB_REQUIRE(!cols.empty(), "par yield table: no maturity columns recognised");
  std::vector<TreasuryParRow> rows;
  for (const auto& r : t.rows) {
    TreasuryParRow row;
    if (r.size() <= cDate || !Date::tryParse(r[cDate], row.date)) continue;
    for (const auto& [i, tenor] : cols) {
      double y = 0.0;
      if (i < r.size() && tryParseDouble(r[i], y)) row.yields.emplace_back(tenor, y / 100.0);
    }
    if (row.yields.size() >= 4) rows.push_back(std::move(row));  // skip holidays / empty rows
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.date < b.date; });
  for (auto& row : rows)
    std::stable_sort(row.yields.begin(), row.yields.end(),
                     [](const auto& a, const auto& b) { return a.first.approxYears() < b.first.approxYears(); });
  return rows;
}

}  // namespace

std::vector<TreasuryParRow> parseTreasuryParYields(const std::string& csvText) { return parseParTable(csvText, "Date"); }
std::vector<TreasuryParRow> parseFredParYields(const std::string& csvText) {
  return parseParTable(csvText, "observation_date");
}

CurveEngine makeTreasuryEngine(const TreasuryParRow& row, Interpolation interpolation, QuoteMap& quotes) {
  CurveDefinition def;
  def.name = "USD-TSY";
  def.interpolation = interpolation;
  quotes.clear();
  for (const auto& [tenor, yield] : row.yields) {
    // Under one year the par curve is quoted as a simple (bond-equivalent)
    // bill yield; from one year on, as the coupon of a semi-annual par bond.
    const bool bill = tenor.approxYears() < 0.99;
    std::shared_ptr<RateHelper> h = bill ? billHelper(row.date, tenor) : parBondHelper(row.date, tenor, 2);
    const std::string id = "UST_" + tenor.str();
    h->label = std::string(bill ? "Bill " : "Par bond ") + tenor.str();
    h->quoteTerms = {{id, 1.0}};
    quotes[id] = yield;
    def.helpers.push_back(std::move(h));
  }
  return CurveEngine(row.date, {def});
}

double parYield(const DiscountCurve& curve, Date start, Period tenor, int frequency) {
  MCB_REQUIRE(frequency >= 1 && 12 % frequency == 0, "parYield: unsupported frequency");
  const Date end = start.add(tenor);
  const int months = 12 / frequency;
  double annuity = 0.0;
  // coupon dates stepped back from maturity
  for (int k = 0;; ++k) {
    const Date d = end.addMonths(-k * months);
    if (d <= start) break;
    annuity += curve.discount(d) / frequency;
  }
  MCB_REQUIRE(annuity > 0.0, "parYield: tenor shorter than one coupon period");
  return (curve.discount(start) - curve.discount(end)) / annuity;
}

// ------------------------------------------------------------ SOFR fixing ---

RateFixing parseNyFedSofr(const std::string& jsonText) {
  const Json j = Json::parse(jsonText);
  const Json& rates = j["refRates"];
  MCB_REQUIRE(rates.isArray() && rates.size() > 0, "NY Fed SOFR feed: no refRates");
  RateFixing best;
  for (const auto& r : rates.items()) {
    Date d;
    if (!Date::tryParse(r["effectiveDate"].strOr(""), d) || !r["percentRate"].isNumber()) continue;
    if (best.date.isNull() || d > best.date) best = {d, r["percentRate"].number() / 100.0};
  }
  MCB_REQUIRE(!best.date.isNull(), "NY Fed SOFR feed: no usable fixing");
  return best;
}

}  // namespace mcb
