// Market-data loaders (Eris, Treasury, NY Fed, MOEX) and the report builder.
// The inputs here are small synthetic files in the publishers' formats; the
// real files are downloaded at run time and are not part of the repository.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "mcb/core/error.hpp"
#include "mcb/data/moex.hpp"
#include "mcb/data/usd_market.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/report/report.hpp"
#include "test_util.hpp"

using namespace mcb;

namespace {

std::string us(Date d) {
  const auto c = d.ymd();
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d/%02d/%04d", c.month, c.day, c.year);
  return buf;
}

// A par-curve file in the Eris layout with made-up rates.
std::string erisParFile(Date asOf) {
  const auto conv = OisConventions::sofr();
  const Date effective = spotDate(asOf, conv.calendar, 2);
  std::ostringstream os;
  os << "Symbol,EvaluationDate,FirstTradeDate,ErisPAIDate,EffectiveDate,CashFlowAlignmentDate,MaturityDate,NPV (A),"
        "FixedNPV,FloatingNPV,Coupon (%),FairCoupon (%),Nominal,Spread (Bps),Index\n";
  const std::vector<std::pair<const char*, double>> quotes = {
      {"1W", 3.88}, {"1M", 3.96}, {"3M", 4.16}, {"6M", 4.36}, {"12M", 4.63}, {"18M", 4.69}, {"2Y", 4.73},
      {"3Y", 4.73}, {"5Y", 4.71},  {"7Y", 4.72}, {"10Y", 4.78}, {"15Y", 4.90}, {"20Y", 4.95}, {"30Y", 4.87}};
  for (const auto& [tenor, rate] : quotes) {
    const Date align = effective.add(Period::parse(tenor));
    const Date end = conv.calendar.adjust(align, BusinessDayConvention::ModifiedFollowing);
    const Date pay = conv.calendar.advance(end, 2, TimeUnit::Days);
    os << "SOFR" << tenor << "," << us(asOf) << "," << us(asOf) << "," << us(asOf) << "," << us(effective) << ","
       << us(align) << "," << us(pay) << ",0,1,-1," << rate << "," << rate << ",100,0,SOFRON Actual/360\n";
  }
  return os.str();
}

const char* kTreasuryCsv =
    "Date,\"1 Mo\",\"1.5 Month\",\"2 Mo\",\"3 Mo\",\"4 Mo\",\"6 Mo\",\"1 Yr\",\"2 Yr\",\"3 Yr\",\"5 Yr\",\"7 Yr\",\"10 Yr\",\"20 Yr\",\"30 Yr\"\n"
    "11/13/2026,4.10,4.18,4.26,4.32,4.34,4.42,4.65,4.88,4.92,4.98,5.05,5.15,5.52,5.47\n"
    "11/12/2026,4.12,4.18,4.26,4.32,4.35,4.42,4.65,4.86,4.90,4.95,5.02,5.10,5.48,5.43\n"
    "11/11/2026,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A\n"
    "11/10/2026,4.05,4.16,4.21,4.30,4.28,4.38,4.56,4.73,4.74,4.81,4.89,4.98,5.39,5.35\n";

// The G-curve parameters published by MOEX for 13 Nov 2026 and its yields.
const char* kZcycJson = R"({
 "params": {"columns": ["tradedate","tradetime","B1","B2","B3","T1","G1","G2","G3","G4","G5","G6","G7","G8","G9"],
            "data": [["2026-11-13","18:54:59",1579.580772,-355.786825,155.676185,2.0,12.606815,-8.545117,-13.221452,-5.479985,10.435647,8.708645,-3.19402,0.0,0.0]]},
 "yearyields": {"columns": ["tradedate","tradetime","period","value"],
            "data": [["2026-11-13","18:54:59",0.25,13.3351],["2026-11-13","18:54:59",1.0,14.0063],
                     ["2026-11-13","18:54:59",5.0,16.2379],["2026-11-13","18:54:59",10.0,16.7429],
                     ["2026-11-13","18:54:59",20.0,16.8524]]}})";

struct TestOfz {
  std::string secid;
  double coupon;      // percent
  Date maturity;
  double spreadBp;    // Z-spread used to generate the price
  std::string details = "\xD0\xA4\xD0\xB8\xD0\xBA\xD1\x81\xD0\xB8\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9 \xD1\x81 "
                        "\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD1\x8B\xD0\xBC\xD0\xB8 \xD0\xBA\xD1\x83\xD0\xBF\xD0\xBE\xD0\xBD\xD0\xB0\xD0\xBC\xD0\xB8";
};

OfzQuote quoteFor(const TestOfz& b, Date settle) {
  OfzQuote q;
  q.secid = b.secid;
  q.couponPercent = b.coupon;
  q.faceValue = 1000;
  q.couponPeriodDays = 182;
  q.couponValue = std::round(1000.0 * b.coupon / 100.0 * 182.0 / 365.0 * 100.0) / 100.0;
  q.maturity = b.maturity;
  q.settle = settle;
  q.nextCoupon = b.maturity;
  while (q.nextCoupon.addDays(-182) > settle) q.nextCoupon = q.nextCoupon.addDays(-182);
  return q;
}

// An ISS "securities + marketdata + marketdata_yields" snapshot priced off the curve with known spreads.
std::string ofzJson(const std::vector<TestOfz>& bonds, const GCurveParams& g, Date settle, Date yieldSettle) {
  std::ostringstream sec, md, my;
  for (std::size_t i = 0; i < bonds.size(); ++i) {
    OfzQuote q = quoteFor(bonds[i], settle);
    Bond bond;
    EXPECT_TRUE(makeOfzBond(q, bond));
    const double dirty = ofzPriceWithSpread(bond, g, settle, bonds[i].spreadBp / 1e4);
    const double accrued = accruedInterest(bond, settle);
    const double clean = dirty - accrued;
    // the exchange's yield, computed for a (possibly different) settlement date
    OfzQuote qy = q;
    qy.settle = yieldSettle;
    Bond by;
    EXPECT_TRUE(makeOfzBond(qy, by));
    const double y = yieldFromDirtyPrice(by, yieldSettle, clean + accruedInterest(by, yieldSettle), YieldConvention::AnnualAct365);
    const char* sep = i ? "," : "";
    char row[600];
    std::snprintf(row, sizeof row,
                  "%s[\"%s\",\"OFZ %s\",\"SUR\",\"%s\",\"x\",%.2f,%.2f,1000,182,%.4f,\"%s\",\"%s\",\"%s\"]", sep,
                  bonds[i].secid.c_str(), bonds[i].secid.c_str(), bonds[i].details.c_str(), bonds[i].coupon, q.couponValue,
                  accrued * 10.0, q.maturity.iso().c_str(), q.nextCoupon.iso().c_str(), settle.iso().c_str());
    sec << row;
    std::snprintf(row, sizeof row, "%s[\"%s\",%.6f,%.6f,%.6f]", sep, bonds[i].secid.c_str(), clean, clean, clean);
    md << row;
    // (the exchange's G-spread is left out: it is not the same quantity as the Z-spread used to build the price)
    std::snprintf(row, sizeof row, "%s[\"%s\",%.6f,%.6f,1000,%.0f,null]", sep, bonds[i].secid.c_str(), clean, y * 100.0,
                  bonds[i].spreadBp);
    my << row;
  }
  return std::string("{\"securities\":{\"columns\":[\"SECID\",\"SECNAME\",\"FACEUNIT\",\"COUPON_DETAILS\",\"FACEVALUE_TYPE\","
                     "\"COUPONPERCENT\",\"COUPONVALUE\",\"FACEVALUE\",\"COUPONPERIOD\",\"ACCRUEDINT\",\"MATDATE\",\"NEXTCOUPON\","
                     "\"SETTLEDATE\"],\"data\":[") +
         sec.str() + "]},\"marketdata\":{\"columns\":[\"SECID\",\"LAST\",\"WAPRICE\",\"MARKETPRICE\"],\"data\":[" + md.str() +
         "]},\"marketdata_yields\":{\"columns\":[\"SECID\",\"PRICE\",\"EFFECTIVEYIELD\",\"DURATION\",\"ZSPREADBP\",\"GSPREADBP\"],"
         "\"data\":[" + my.str() + "]}}";
}

std::vector<TestOfz> sampleBonds() {
  return {{"SU01", 7.0, Date(2028, 3, 15), -5.0},  {"SU02", 8.5, Date(2029, 9, 12), 3.0},
          {"SU03", 6.0, Date(2030, 6, 5), -8.0},   {"SU04", 12.0, Date(2031, 11, 19), 6.0},
          {"SU05", 9.5, Date(2033, 4, 13), 0.0},   {"SU06", 11.0, Date(2034, 8, 2), 2.0},
          {"SU07", 7.5, Date(2036, 2, 20), 90.0},  // clearly cheap
          {"SU08", 13.0, Date(2038, 10, 6), 4.0},  {"SU09", 10.0, Date(2040, 5, 16), -3.0}};
}

}  // namespace

TEST(ErisData, ParsesTheParCurveAndBootstrapsIt) {
  const Date asOf(2026, 11, 13);
  const ErisParCurve par = parseErisParCurve(erisParFile(asOf));
  EXPECT_EQ(par.asOf, asOf);
  ASSERT_EQ(par.quotes.size(), 14u);
  EXPECT_EQ(par.quotes[4].symbol, "SOFR12M");
  EXPECT_EQ(par.quotes[4].tenor, (Period{12, TimeUnit::Months}));
  EXPECT_EQ(par.quotes[0].effective, Date(2026, 11, 17));
  EXPECT_NEAR(par.quotes[10].rate, 0.0478, 1e-15);
  // the 3Y swap has the accrual dates published by the exchange (17 Nov 2029 is a Saturday)
  const OisSwap swap = makeSofrSwap(par.quotes[7]);
  ASSERT_EQ(swap.periods.size(), 3u);
  EXPECT_EQ(swap.periods[2].end, Date(2029, 11, 19));
  EXPECT_EQ(swap.periods[2].pay, Date(2029, 11, 21));
  EXPECT_NEAR(swap.periods[0].tau, 365.0 / 360.0, 1e-15);

  for (auto k : {Interpolation::LogLinear, Interpolation::LogCubic}) {
    QuoteMap quotes;
    const CurveEngine engine = makeSofrEngine(par, k, quotes);
    const auto set = engine.build(quotes);
    EXPECT_LT(set->report("USD-SOFR").maxAbsError, 1e-13);
    EXPECT_EQ(quotes.size(), 14u);
    const YieldCurve& curve = set->curve("USD-SOFR");

    // "Published" discount factors generated from our own curve: the comparison must come out clean.
    std::ostringstream df;
    df << "Date,DiscountFactor,SpotRate (Actual360 Continuous),ForwardRate,ValueDate,MaturityDate\n";
    char buf[64];
    for (Date d = asOf; d <= curve.maxDate(); d = d.addDays(1)) {
      std::snprintf(buf, sizeof buf, "%s,%.17f,,,,\n", us(d).c_str(), curve.discount(d));
      df << buf;
    }
    const auto dfs = parseErisDiscountFactors(df.str());
    const ErisComparison c = compareWithEris(curve, dfs, par);
    EXPECT_GT(c.datesCompared, 10000);
    EXPECT_LT(c.maxAbsDfDiff, 1e-15);
    EXPECT_LT(c.maxZeroRateDiffBp, 1e-9);
    // cubic curve values are reproduced exactly on every date, so repricing is exact too
    EXPECT_LT(c.maxConventionErrorBp, 1e-8);
  }
  EXPECT_THROW(parseErisParCurve("Symbol,EvaluationDate\nSOFR1Y,11/13/2026\n"), Error);
  EXPECT_THROW(parseErisDiscountFactors("Date,DiscountFactor\n"), Error);
}

TEST(TreasuryData, ParsesTheParYieldFileAndBootstrapsIt) {
  const auto rows = parseTreasuryParYields(kTreasuryCsv);
  ASSERT_EQ(rows.size(), 3u);  // the holiday row (all N/A) is dropped
  EXPECT_EQ(rows.front().date, Date(2026, 11, 10));  // sorted oldest first
  EXPECT_EQ(rows.back().date, Date(2026, 11, 13));
  ASSERT_EQ(rows.back().yields.size(), 14u);
  EXPECT_EQ(rows.back().yields[1].first, (Period{6, TimeUnit::Weeks}));  // "1.5 Month" is the 6-week bill
  EXPECT_NEAR(rows.back().yields[11].second, 0.0515, 1e-15);

  for (auto k : {Interpolation::MonotoneConvex, Interpolation::LogLinear}) {
    QuoteMap quotes;
    const CurveEngine engine = makeTreasuryEngine(rows.back(), k, quotes);
    const auto set = engine.build(quotes);
    const YieldCurve& curve = set->curve("USD-TSY");
    EXPECT_LT(set->report("USD-TSY").maxAbsError, 1e-12);
    EXPECT_NEAR(quotes.at("UST_10Y"), 0.0515, 1e-15);
    // reading par yields back off the curve returns the quotes
    for (const char* tenor : {"1Y", "2Y", "5Y", "10Y", "30Y"})
      EXPECT_NEAR(parYield(curve, rows.back().date, Period::parse(tenor)), quotes.at(std::string("UST_") + tenor), 1e-12) << tenor;
    // bills are simple ACT/365 yields
    EXPECT_NEAR(curve.forwardRate(Date(2026, 11, 13), Date(2027, 2, 13), DayCount::Act365Fixed), 0.0432, 1e-13);
  }
}

TEST(TreasuryData, ParsesTheFredFallback) {
  const char* fred = "observation_date,DGS1MO,DGS3MO,DGS6MO,DGS1,DGS2,DGS3,DGS5,DGS7,DGS10,DGS20,DGS30\n"
                     "2026-11-11,.,.,.,.,.,.,.,.,.,.,.\n"
                     "2026-11-12,4.12,4.32,4.42,4.65,4.86,4.90,4.95,5.02,5.10,5.48,5.43\n"
                     "2026-11-13,4.10,4.32,4.42,4.65,4.88,4.92,4.98,5.05,5.15,5.52,5.47\n";
  const auto rows = parseFredParYields(fred);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows.back().date, Date(2026, 11, 13));
  EXPECT_EQ(rows.back().yields.size(), 11u);
  EXPECT_NEAR(rows.back().yields.back().second, 0.0547, 1e-15);
  EXPECT_THROW(parseTreasuryParYields("Date,Foo\n11/13/2026,1\n"), Error);
}

TEST(NyFedData, ParsesTheLatestSofrFixing) {
  const char* json = R"({"refRates":[{"effectiveDate":"2026-11-10","type":"SOFR","percentRate":3.89},
                                      {"effectiveDate":"2026-11-12","type":"SOFR","percentRate":3.90}]})";
  const RateFixing fix = parseNyFedSofr(json);
  EXPECT_EQ(fix.date, Date(2026, 11, 12));
  EXPECT_NEAR(fix.rate, 0.039, 1e-15);
  EXPECT_THROW(parseNyFedSofr(R"({"refRates":[]})"), Error);
  EXPECT_THROW(parseNyFedSofr("not json"), Error);
}

TEST(MoexData, GCurveReproducesPublishedYields) {
  const MoexZcyc z = parseMoexZcyc(kZcycJson);
  EXPECT_EQ(z.params.date, Date(2026, 11, 13));
  EXPECT_EQ(z.params.time, "18:54:59");
  ASSERT_EQ(z.publishedYields.size(), 5u);
  for (const auto& [years, published] : z.publishedYields)
    EXPECT_NEAR(gCurveYield(z.params, years) * 100.0, published, 6e-5) << years;  // four published decimals
  // as a discount curve: DF = exp(-G t), i.e. (1 + KBD)^-t
  const GCurve curve(z.params, Date(2026, 11, 16));
  EXPECT_NEAR(curve.discountT(5.0), std::pow(1.162379, -5.0), 2e-6);
  EXPECT_NEAR(curve.discountT(0.0), 1.0, 1e-15);
  EXPECT_THROW(parseMoexZcyc(R"({"params":{"columns":["tradedate"],"data":[]}})"), Error);
}

TEST(MoexData, OfzAnalysisRecoversSpreadsAndFlagsTheCheapBond) {
  const MoexZcyc z = parseMoexZcyc(kZcycJson);
  const Date settle(2026, 11, 17);
  auto bonds = sampleBonds();
  // a floater and a foreign-currency bond that must be ignored
  TestOfz floater{"SU99", 14.0, Date(2032, 1, 14), 0.0};
  floater.details = "RUONIA";
  bonds.push_back(floater);
  const auto quotes = parseMoexOfz(ofzJson(bonds, z.params, settle, settle));
  ASSERT_EQ(quotes.size(), 9u);  // the floater is filtered out
  EXPECT_EQ(quotes.front().secid, "SU01");  // sorted by maturity
  EXPECT_EQ(quotes.front().settle, settle);

  const OfzReport rep = analyzeOfz(quotes, z.params);
  ASSERT_EQ(rep.bonds.size(), 9u);
  EXPECT_EQ(rep.settle, settle);
  const auto expected = sampleBonds();
  for (std::size_t i = 0; i < rep.bonds.size(); ++i) {
    const auto& b = rep.bonds[i];
    EXPECT_NEAR(b.zSpreadBp, expected[i].spreadBp, 2e-3) << b.quote.secid;  // prices carry six decimals
    // a positive spread means the market price is below the curve price
    if (expected[i].spreadBp > 1.0) {
      EXPECT_LT(b.priceGap, 0.0) << b.quote.secid;
    }
    if (expected[i].spreadBp < -1.0) {
      EXPECT_GT(b.priceGap, 0.0) << b.quote.secid;
    }
    EXPECT_GT(b.durationYears, 1.0);
    EXPECT_NEAR(b.gSpreadBp, (b.yield - gCurveYield(z.params, b.durationYears)) * 1e4, 1e-9);
  }
  // our yields agree with "the exchange's" and the one bond with a 90 bp spread is the only BUY
  EXPECT_LT(rep.maxYieldDiffBp, 1e-3);
  EXPECT_LT(rep.maxAccruedDiff, 1e-3);
  int buys = 0, sells = 0;
  for (const auto& b : rep.bonds) {
    buys += b.signal == "BUY";
    sells += b.signal == "SELL";
    if (b.quote.secid == "SU07") {
      EXPECT_EQ(b.signal, "BUY");
      EXPECT_GT(b.residualBp, rep.thresholdBp);
      EXPECT_GT(b.priceEdge, rep.minPriceEdge);
    }
  }
  EXPECT_EQ(buys, 1);
  EXPECT_EQ(sells, 0);
  EXPECT_GE(rep.thresholdBp, 10.0);
}

TEST(MoexData, SettlementDateIsCalibratedToTheExchangeYields) {
  // Weekend sessions: prices settle a day earlier than the date in the snapshot.
  const MoexZcyc z = parseMoexZcyc(kZcycJson);
  const Date published(2026, 11, 17), actual(2026, 11, 16);
  const auto quotes = parseMoexOfz(ofzJson(sampleBonds(), z.params, published, actual));
  const OfzReport rep = analyzeOfz(quotes, z.params);
  EXPECT_EQ(rep.settle, actual);
  EXPECT_LT(rep.maxYieldDiffBp, 1e-3);
}

TEST(MoexData, IrregularSchedulesAndFuturesImpliedRates) {
  OfzQuote q = quoteFor({"SUXX", 8.0, Date(2030, 5, 15), 0.0}, Date(2026, 11, 17));
  Bond bond;
  EXPECT_TRUE(makeOfzBond(q, bond));
  EXPECT_EQ(bond.maturity, Date(2030, 5, 15));
  EXPECT_EQ(bond.frequency, 2);
  q.nextCoupon = q.nextCoupon.addDays(5);  // coupons no longer land on the maturity date
  EXPECT_FALSE(makeOfzBond(q, bond));

  const char* forts = R"({"securities":{"columns":["SECID","SHORTNAME","ASSETCODE","PREVSETTLEPRICE","LASTTRADEDATE","PREVOPENPOSITION"],
    "data":[["RFH7","RUONIA-3.27","RUONIA",4.7918,"2027-03-01",96],["RFZ6","RUONIA-12.26","RUONIA",4.6308,"2026-12-01",100],
            ["KKZ6","KEYRATE-12.26","KEYRATE",13.94,"2026-12-18",3926],["SiZ6","Si-12.26","Si",81000,"2026-12-17",1]]}})";
  const auto futures = parseMoexRateFutures(forts);
  ASSERT_EQ(futures.size(), 3u);  // the currency future is ignored
  EXPECT_EQ(futures[0].asset, "KEYRATE");
  const auto implied = ruoniaImpliedForwards(futures);
  ASSERT_EQ(implied.size(), 1u);
  EXPECT_EQ(implied[0].start, Date(2026, 12, 1));
  EXPECT_NEAR(implied[0].rate, (4.7918 / 4.6308 - 1.0) * 365.0 / 90.0, 1e-12);  // about 14.1%
}

// Validation checks that failed, as a readable message. Latency checks are
// skipped: they depend on the machine running the tests.
std::string failedChecks(const Json& doc) {
  std::string out;
  for (const auto& row : doc["validation"].items())
    if (!row["ok"].isNull() && !row["ok"].boolean() && row["group"].str() != "Real-time layer")
      out += "\n  FAILED CHECK: " + row["group"].str() + " / " + row["check"].str() + " = " +
             std::to_string(row["value"].numberOr(std::nan(""))) + " " + row["unit"].str();
  return out;
}

TEST(Report, BuildsFromReferenceDataAloneAndFromLiveStyleFiles) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "mcb_report_test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  ReportOptions opt;
  opt.dataDir = dir.string();
  opt.referenceDir = mcbtest::sourcePath("data/reference");
  opt.quantlibDir = mcbtest::dataPath("ql");
  opt.portfolioFile = mcbtest::sourcePath("data/portfolio/usd_portfolio.csv");
  opt.generatedAt = "2026-11-16T00:00:00Z";
  opt.bootstrapReplications = 100;
  opt.forecastPaths = 300;
  opt.benchmarkIterations = 3;

  // 1) no downloads at all: the frozen reference data still produces a report
  std::ostringstream log;
  Json doc = buildReport(opt, log);
  EXPECT_FALSE(doc["usd"]["sofr"]["available"].boolean());
  EXPECT_FALSE(doc["usd"]["treasury"]["available"].boolean());
  EXPECT_FALSE(doc["moex"]["available"].boolean());
  EXPECT_TRUE(doc["eur"]["available"].boolean());
  EXPECT_TRUE(doc["stats"]["available"].boolean());
  EXPECT_EQ(doc["stats"]["ehTests"].size(), 6u);
  EXPECT_EQ(failedChecks(doc), "");
  EXPECT_GT(doc["meta"]["checksPassed"].number(), 5.0);
  EXPECT_EQ(doc["eur"]["instruments"].size(), 90u);
  EXPECT_NEAR(doc["eur"]["riskExample"]["npv"].number(), -39773.52, 0.01);  // QuantLib: -39773.5195

  // 2) with market files in place the live sections appear
  fs::create_directories(dir / "eris");
  fs::create_directories(dir / "treasury");
  fs::create_directories(dir / "moex");
  fs::create_directories(dir / "nyfed");
  writeTextFile((dir / "eris" / "Eris_20261113_EOD_ParCouponCurve_SOFR.csv").string(), erisParFile(Date(2026, 11, 13)));
  writeTextFile((dir / "eris" / "Eris_20261112_EOD_ParCouponCurve_SOFR.csv").string(), erisParFile(Date(2026, 11, 12)));
  writeTextFile((dir / "treasury" / "par_yields_2026.csv").string(), kTreasuryCsv);
  writeTextFile((dir / "nyfed" / "sofr.json").string(), R"({"refRates":[{"effectiveDate":"2026-11-12","percentRate":3.90}]})");
  const MoexZcyc z = parseMoexZcyc(kZcycJson);
  writeTextFile((dir / "moex" / "zcyc.json").string(), kZcycJson);
  writeTextFile((dir / "moex" / "ofz.json").string(), ofzJson(sampleBonds(), z.params, Date(2026, 11, 17), Date(2026, 11, 17)));

  doc = buildReport(opt, log);
  ASSERT_TRUE(doc["usd"]["sofr"]["available"].boolean()) << log.str();
  ASSERT_TRUE(doc["usd"]["treasury"]["available"].boolean()) << log.str();
  ASSERT_TRUE(doc["moex"]["available"].boolean()) << log.str();
  EXPECT_EQ(doc["usd"]["sofr"]["asOf"].str(), "2026-11-13");
  EXPECT_EQ(doc["usd"]["sofr"]["quotes"].size(), 14u);
  EXPECT_EQ(doc["usd"]["treasury"]["quotes"].size(), 14u);
  EXPECT_NEAR(doc["usd"]["sofrFixing"]["rate"].number(), 3.9, 1e-12);
  EXPECT_GE(doc["usd"]["swapSpreads"].size(), 5u);
  EXPECT_EQ(doc["usd"]["portfolio"]["items"].size(), 6u);
  EXPECT_EQ(doc["stats"]["current"]["date"].str(), "2026-11-13");  // forecasts start from the bootstrapped curve
  EXPECT_EQ(doc["stats"]["returns"]["rows"].size(), 5u);
  EXPECT_EQ(doc["moex"]["ofz"]["bonds"].size(), 9u);
  EXPECT_EQ(failedChecks(doc), "");
  // one signal per Treasury tenor plus the cheap OFZ
  int treasury = 0, ofz = 0;
  for (const auto& s : doc["signals"].items()) {
    const std::string sig = s["signal"].str();
    EXPECT_TRUE(sig == "BUY" || sig == "SELL" || sig == "HOLD");
    treasury += s["market"].str() == "US Treasuries";
    ofz += s["market"].str() != "US Treasuries";
  }
  EXPECT_EQ(treasury, 5);
  EXPECT_EQ(ofz, 1);
  // the document survives a round trip through its own serialisation
  const Json back = Json::parse(doc.dump());
  EXPECT_EQ(back["meta"]["generatedAt"].str(), "2026-11-16T00:00:00Z");
  EXPECT_EQ(back["validation"].size(), doc["validation"].size());

  // 3) a corrupt file disables its own section only
  writeTextFile((dir / "moex" / "zcyc.json").string(), "{ this is not json");
  doc = buildReport(opt, log);
  EXPECT_FALSE(doc["moex"]["available"].boolean());
  EXPECT_TRUE(doc["usd"]["sofr"]["available"].boolean());
  fs::remove_all(dir);
}
