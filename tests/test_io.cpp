// CSV and JSON readers/writers.
#include <gtest/gtest.h>

#include <cmath>

#include "mcb/core/error.hpp"
#include "mcb/io/csv.hpp"
#include "mcb/io/json.hpp"

using namespace mcb;

TEST(Csv, HandlesQuotesBomAndCrLf) {
  const std::string text = "\xEF\xBB\xBF" "Date,\"1 Mo\",\"Name, with comma\"\r\n"
                           "11/13/2026,4.10,\"say \"\"hi\"\"\"\r\n"
                           "\r\n"
                           "11/12/2026, 4.12 ,\n";
  const auto t = parseCsv(text);
  ASSERT_EQ(t.header.size(), 3u);
  EXPECT_EQ(t.header[0], "Date");
  EXPECT_EQ(t.header[1], "1 Mo");
  EXPECT_EQ(t.header[2], "Name, with comma");
  ASSERT_EQ(t.rows.size(), 2u);  // blank line skipped
  EXPECT_EQ(t.rows[0][2], "say \"hi\"");
  EXPECT_EQ(t.at(1, "1 Mo"), "4.12");  // unquoted fields are trimmed
  EXPECT_EQ(t.at(1, "Name, with comma"), "");
  EXPECT_EQ(t.col("missing"), -1);
  EXPECT_THROW(t.requireCol("missing"), Error);
}

TEST(Csv, OtherDelimitersAndNoHeader) {
  const auto t = parseCsv("a;b;c\n1;2;3\n", ';', false);
  EXPECT_TRUE(t.header.empty());
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(t.rows[1][2], "3");
  EXPECT_EQ(split("a|b||c", '|').size(), 4u);
  EXPECT_EQ(trim("  x y \t"), "x y");
}

TEST(Csv, NumberParsing) {
  double v = 0;
  EXPECT_TRUE(tryParseDouble(" 4.25 ", v));
  EXPECT_DOUBLE_EQ(v, 4.25);
  EXPECT_TRUE(tryParseDouble("-1.5e-3", v));
  EXPECT_DOUBLE_EQ(v, -1.5e-3);
  EXPECT_FALSE(tryParseDouble("NA", v));
  EXPECT_FALSE(tryParseDouble("", v));
  EXPECT_FALSE(tryParseDouble("12abc", v));
  EXPECT_THROW(parseDouble("x", "test"), Error);
}

TEST(Json, ParsesNestedDocuments) {
  const auto j = Json::parse(R"({"a": [1, 2.5, -3e2, null, true], "s": "x\"y\néО", "o": {"k": false}})");
  ASSERT_TRUE(j.isObject());
  EXPECT_EQ(j["a"].size(), 5u);
  EXPECT_DOUBLE_EQ(j["a"][1].number(), 2.5);
  EXPECT_DOUBLE_EQ(j["a"][2].number(), -300.0);
  EXPECT_TRUE(j["a"][3].isNull());
  EXPECT_TRUE(j["a"][4].boolean());
  EXPECT_EQ(j["s"].str(), "x\"y\n\xC3\xA9\xD0\x9E");  // UTF-8 for e-acute and Cyrillic O
  EXPECT_FALSE(j["o"]["k"].boolean());
  EXPECT_TRUE(j["missing"].isNull());
  EXPECT_TRUE(j["missing"]["deeper"].isNull());
  EXPECT_DOUBLE_EQ(j["missing"].numberOr(7.0), 7.0);
  EXPECT_TRUE(j.contains("o"));
}

TEST(Json, RejectsMalformedInput) {
  EXPECT_THROW(Json::parse("{\"a\": }"), Error);
  EXPECT_THROW(Json::parse("[1, 2"), Error);
  EXPECT_THROW(Json::parse("{\"a\": 1} extra"), Error);
  EXPECT_THROW(Json::parse("\"unterminated"), Error);
  EXPECT_THROW(Json::parse(""), Error);
}

TEST(Json, RoundTripsThroughDump) {
  Json j = Json::object();
  j.set("name", "curve").set("values", std::vector<double>{1.0, 0.5, 1e-9, std::nan("")}).set("n", 3);
  Json arr = Json::array();
  arr.push(Json::object().set("x", 1.25)).push("text with \"quotes\"");
  j.set("items", arr);
  for (int indent : {-1, 2}) {
    const auto back = Json::parse(j.dump(indent));
    EXPECT_EQ(back["name"].str(), "curve");
    EXPECT_DOUBLE_EQ(back["values"][2].number(), 1e-9);
    EXPECT_TRUE(back["values"][3].isNull());  // NaN is written as null
    EXPECT_DOUBLE_EQ(back["n"].number(), 3.0);
    EXPECT_DOUBLE_EQ(back["items"][0]["x"].number(), 1.25);
    EXPECT_EQ(back["items"][1].str(), "text with \"quotes\"");
  }
  EXPECT_EQ(Json(0.1).dump(), "0.1");  // shortest round-trip formatting
  EXPECT_EQ(Json(42).dump(), "42");
  EXPECT_DOUBLE_EQ(roundTo(1.23456789, 4), 1.2346);
}
