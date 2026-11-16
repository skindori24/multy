#include "mcb/io/json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "mcb/core/error.hpp"

namespace mcb {
namespace {

const Json& nullJson() {
  static const Json j;
  return j;
}

class Parser {
 public:
  explicit Parser(std::string_view t) : t_(t) {}

  Json parseDocument() {
    skipWs();
    Json v = parseValue(0);
    skipWs();
    MCB_REQUIRE(pos_ == t_.size(), "JSON: unexpected trailing characters at offset " << pos_);
    return v;
  }

 private:
  std::string_view t_;
  std::size_t pos_ = 0;

  [[noreturn]] void fail(const char* what) const { MCB_FAIL("JSON: " << what << " at offset " << pos_); }
  void skipWs() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\n' || t_[pos_] == '\r' || t_[pos_] == '\t')) ++pos_;
  }
  bool consume(std::string_view word) {
    if (t_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  Json parseValue(int depth) {
    if (depth > 200) fail("nesting too deep");
    if (pos_ >= t_.size()) fail("unexpected end of input");
    const char c = t_[pos_];
    if (c == '{') return parseObject(depth);
    if (c == '[') return parseArray(depth);
    if (c == '"') return Json(parseString());
    if (consume("true")) return Json(true);
    if (consume("false")) return Json(false);
    if (consume("null")) return Json();
    return parseNumber();
  }

  Json parseObject(int depth) {
    ++pos_;  // {
    Json::Object obj;
    skipWs();
    if (pos_ < t_.size() && t_[pos_] == '}') {
      ++pos_;
      return Json(std::move(obj));
    }
    for (;;) {
      skipWs();
      if (pos_ >= t_.size() || t_[pos_] != '"') fail("expected object key");
      std::string key = parseString();
      skipWs();
      if (pos_ >= t_.size() || t_[pos_] != ':') fail("expected ':'");
      ++pos_;
      skipWs();
      obj.emplace_back(std::move(key), parseValue(depth + 1));
      skipWs();
      if (pos_ < t_.size() && t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < t_.size() && t_[pos_] == '}') {
        ++pos_;
        break;
      }
      fail("expected ',' or '}'");
    }
    return Json(std::move(obj));
  }

  Json parseArray(int depth) {
    ++pos_;  // [
    Json::Array arr;
    skipWs();
    if (pos_ < t_.size() && t_[pos_] == ']') {
      ++pos_;
      return Json(std::move(arr));
    }
    for (;;) {
      skipWs();
      arr.push_back(parseValue(depth + 1));
      skipWs();
      if (pos_ < t_.size() && t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < t_.size() && t_[pos_] == ']') {
        ++pos_;
        break;
      }
      fail("expected ',' or ']'");
    }
    return Json(std::move(arr));
  }

  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  unsigned parseHex4() {
    if (pos_ + 4 > t_.size()) fail("truncated \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else fail("bad \\u escape");
    }
    return v;
  }

  std::string parseString() {
    ++pos_;  // opening quote
    std::string out;
    for (;;) {
      if (pos_ >= t_.size()) fail("unterminated string");
      const char c = t_[pos_++];
      if (c == '"') break;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= t_.size()) fail("unterminated escape");
      const char e = t_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = parseHex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && t_.substr(pos_, 2) == "\\u") {  // surrogate pair
            pos_ += 2;
            const unsigned lo = parseHex4();
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: fail("unknown escape");
      }
    }
    return out;
  }

  Json parseNumber() {
    const std::size_t start = pos_;
    if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
    while (pos_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[pos_])) || t_[pos_] == '.' ||
                                t_[pos_] == 'e' || t_[pos_] == 'E' || t_[pos_] == '-' || t_[pos_] == '+'))
      ++pos_;
    if (pos_ == start) fail("unexpected character");
    const std::string s(t_.substr(start, pos_ - start));
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') fail("malformed number");
    return Json(v);
  }
};

void dumpString(const std::string& s, std::string& out) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void dumpNumber(double v, std::string& out) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    out += buf;
    return;
  }
  char buf[40];
  const auto res = std::to_chars(buf, buf + sizeof buf, v);  // shortest round-trip form
  out.append(buf, res.ptr);
}

void dumpValue(const Json& j, int indent, int level, std::string& out) {
  const auto newline = [&](int lvl) {
    if (indent >= 0) {
      out += '\n';
      out.append(static_cast<std::size_t>(indent * lvl), ' ');
    }
  };
  switch (j.type()) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.boolean() ? "true" : "false"; break;
    case Json::Type::Number: dumpNumber(j.number(), out); break;
    case Json::Type::String: dumpString(j.str(), out); break;
    case Json::Type::Array: {
      const auto& a = j.items();
      if (a.empty()) {
        out += "[]";
        break;
      }
      // Arrays of scalars stay on one line even when pretty-printing.
      bool scalars = true;
      for (const auto& v : a)
        if (v.isArray() || v.isObject()) scalars = false;
      out += '[';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (i) out += ',';
        if (!scalars) newline(level + 1);
        dumpValue(a[i], indent, level + 1, out);
      }
      if (!scalars) newline(level);
      out += ']';
      break;
    }
    case Json::Type::Object: {
      const auto& o = j.members();
      if (o.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (std::size_t i = 0; i < o.size(); ++i) {
        if (i) out += ',';
        newline(level + 1);
        dumpString(o[i].first, out);
        out += indent >= 0 ? ": " : ":";
        dumpValue(o[i].second, indent, level + 1, out);
      }
      newline(level);
      out += '}';
      break;
    }
  }
}

}  // namespace

Json::Json(const std::vector<double>& v) : type_(Type::Array) {
  arr_.reserve(v.size());
  for (double x : v) arr_.push_back(std::isfinite(x) ? Json(x) : Json());
}

Json::Json(const std::vector<std::string>& v) : type_(Type::Array) {
  arr_.reserve(v.size());
  for (const auto& s : v) arr_.emplace_back(s);
}

Json Json::parse(std::string_view text) { return Parser(text).parseDocument(); }

std::string Json::dump(int indent) const {
  std::string out;
  dumpValue(*this, indent, 0, out);
  return out;
}

double Json::number() const {
  MCB_REQUIRE(type_ == Type::Number, "JSON value is not a number");
  return num_;
}
double Json::numberOr(double fallback) const { return type_ == Type::Number ? num_ : fallback; }
bool Json::boolean() const {
  MCB_REQUIRE(type_ == Type::Bool, "JSON value is not a boolean");
  return bool_;
}
const std::string& Json::str() const {
  MCB_REQUIRE(type_ == Type::String, "JSON value is not a string");
  return str_;
}
std::string Json::strOr(std::string_view fallback) const {
  return type_ == Type::String ? str_ : std::string(fallback);
}
const Json::Array& Json::items() const {
  MCB_REQUIRE(type_ == Type::Array, "JSON value is not an array");
  return arr_;
}
const Json::Object& Json::members() const {
  MCB_REQUIRE(type_ == Type::Object, "JSON value is not an object");
  return obj_;
}
std::size_t Json::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}

const Json& Json::operator[](std::string_view key) const {
  if (type_ == Type::Object)
    for (const auto& kv : obj_)
      if (kv.first == key) return kv.second;
  return nullJson();
}

const Json& Json::operator[](std::size_t index) const {
  MCB_REQUIRE(type_ == Type::Array && index < arr_.size(), "JSON array index out of range");
  return arr_[index];
}

bool Json::contains(std::string_view key) const {
  if (type_ != Type::Object) return false;
  for (const auto& kv : obj_)
    if (kv.first == key) return true;
  return false;
}

Json& Json::set(std::string key, Json value) {
  if (type_ == Type::Null) type_ = Type::Object;
  MCB_REQUIRE(type_ == Type::Object, "JSON set() on a non-object");
  for (auto& kv : obj_)
    if (kv.first == key) {
      kv.second = std::move(value);
      return *this;
    }
  obj_.emplace_back(std::move(key), std::move(value));
  return *this;
}

Json& Json::push(Json value) {
  if (type_ == Type::Null) type_ = Type::Array;
  MCB_REQUIRE(type_ == Type::Array, "JSON push() on a non-array");
  arr_.push_back(std::move(value));
  return *this;
}

double roundTo(double v, int decimals) {
  if (!std::isfinite(v)) return v;
  const double scale = std::pow(10.0, decimals);
  return std::round(v * scale) / scale;
}

}  // namespace mcb
