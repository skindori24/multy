#include "mcb/data/eur_reference.hpp"

#include <map>

#include "mcb/core/error.hpp"
#include "mcb/io/csv.hpp"

namespace mcb {

ReferenceMarket parseReferenceMarket(const std::string& csvText) {
  const CsvTable t = parseCsv(csvText);
  const int cCurve = t.requireCol("curve"), cKind = t.requireCol("kind"), cId = t.requireCol("id");
  const int cA = t.requireCol("a"), cB = t.requireCol("b"), cQuote = t.requireCol("quote"), cLink = t.requireCol("link");
  auto cell = [](const std::vector<std::string>& row, int c) {
    return static_cast<std::size_t>(c) < row.size() ? row[static_cast<std::size_t>(c)] : std::string();
  };
  ReferenceMarket m;
  for (const auto& row : t.rows) {
    const std::string curve = cell(row, cCurve);
    if (curve.empty() || curve[0] == '#') continue;  // comment line
    if (curve == "META") {
      if (cell(row, cKind) == "ASOF") m.asOf = Date::parse(cell(row, cA));
      continue;
    }
    MarketInstrument ins;
    ins.curve = curve;
    ins.kind = cell(row, cKind);
    ins.id = cell(row, cId);
    ins.a = cell(row, cA);
    ins.b = cell(row, cB);
    ins.link = cell(row, cLink);
    ins.quote = parseDouble(cell(row, cQuote), "quote of " + ins.id);
    MCB_REQUIRE(!ins.id.empty(), "reference market: instrument without id on curve " << curve);
    m.instruments.push_back(std::move(ins));
  }
  MCB_REQUIRE(!m.asOf.isNull(), "reference market: missing 'META,ASOF' row");
  MCB_REQUIRE(!m.instruments.empty(), "reference market: no instruments");
  return m;
}

ReferenceMarket loadReferenceMarket(const std::string& path) { return parseReferenceMarket(readTextFile(path)); }

CurveEngine makeEurEngine(const ReferenceMarket& market, Interpolation interpolation, bool parCoupons,
                          QuoteMap& quotes, EurSetup* setupOut) {
  const Date asOf = market.asOf;
  const Calendar target(Calendar::Kind::Target);
  EurSetup setup;
  setup.ois = OisConventions::eonia();
  setup.swap6m = SwapConventions::euribor(Period{6, TimeUnit::Months});
  setup.swap3m = SwapConventions::euribor(Period{3, TimeUnit::Months});
  setup.swap6m.parCoupons = setup.swap3m.parCoupons = parCoupons;
  if (setupOut) *setupOut = setup;

  std::map<std::string, CurveDefinition> defs;
  for (const char* name : {"EUR-OIS", "EUR-6M", "EUR-3M"}) {
    CurveDefinition d;
    d.name = name;
    d.interpolation = interpolation;
    d.discountCurve = d.name == "EUR-OIS" ? "" : "EUR-OIS";
    defs[name] = d;
  }

  quotes.clear();
  for (const auto& ins : market.instruments) {
    const auto it = defs.find(ins.curve);
    MCB_REQUIRE(it != defs.end(), "reference market: unknown curve '" << ins.curve << "'");
    const SwapConventions& swapConv = ins.curve == "EUR-3M" ? setup.swap3m : setup.swap6m;
    std::shared_ptr<RateHelper> h;
    quotes[ins.id] = ins.kind == "FUTURE" ? ins.quote : ins.quote / 100.0;  // percent -> decimal
    if (ins.kind == "DEPOSIT") {
      const Period tenor = Period::parse(ins.a);
      const int fixingDays = static_cast<int>(parseDouble(ins.b, "fixing days of " + ins.id));
      const bool isOvernight = tenor.unit == TimeUnit::Days;
      h = depositHelper(asOf, tenor, fixingDays, target,
                        isOvernight ? BusinessDayConvention::Following : BusinessDayConvention::ModifiedFollowing,
                        !isOvernight, DayCount::Act360);
    } else if (ins.kind == "OIS") {
      h = oisHelper(asOf, Period::parse(ins.a), setup.ois);
    } else if (ins.kind == "OIS_DATED") {
      h = datedOisHelper(Date::parse(ins.a), Date::parse(ins.b), setup.ois);
    } else if (ins.kind == "FRA") {
      h = fraHelper(asOf, static_cast<int>(parseDouble(ins.a, "FRA start of " + ins.id)), swapConv.index);
    } else if (ins.kind == "FUTURE") {
      const double convexity = ins.b.empty() ? 0.0 : parseDouble(ins.b, "convexity of " + ins.id) / 100.0;
      h = futuresHelper(Date::parse(ins.a), swapConv.index, convexity);
    } else if (ins.kind == "SWAP" || ins.kind == "SWAP_BASIS") {
      h = swapHelper(asOf, Period::parse(ins.a), swapConv);
    } else {
      MCB_FAIL("reference market: unknown instrument kind '" << ins.kind << "'");
    }
    if (ins.kind == "SWAP_BASIS") {
      MCB_REQUIRE(!ins.link.empty(), "basis swap " << ins.id << " needs the id of its base swap in 'link'");
      h->quoteTerms = {{ins.link, 1.0}, {ins.id, -1.0}};  // swap rate = base swap rate - basis
    } else {
      h->quoteTerms = {{ins.id, 1.0}};
    }
    h->label = ins.id;
    it->second.helpers.push_back(std::move(h));
  }
  std::vector<CurveDefinition> ordered;
  for (const char* name : {"EUR-OIS", "EUR-6M", "EUR-3M"})
    if (!defs[name].helpers.empty()) ordered.push_back(std::move(defs[name]));
  return CurveEngine(asOf, std::move(ordered));
}

}  // namespace mcb
