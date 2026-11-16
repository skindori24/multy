#pragma once
// A small JSON value type with a parser and a writer.
//
// Used to read exchange APIs that only speak JSON (Moscow Exchange ISS) and
// to write the results file consumed by the dashboard.  Objects keep their
// insertion order so the output is stable and diff-friendly.
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcb {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Json>;
  using Object = std::vector<std::pair<std::string, Json>>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(double v) : type_(Type::Number), num_(v) {}
  Json(int v) : type_(Type::Number), num_(v) {}
  Json(long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
  Json(long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
  Json(std::size_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
  Json(std::string_view s) : type_(Type::String), str_(s) {}
  Json(Array a) : type_(Type::Array), arr_(std::move(a)) {}
  Json(Object o) : type_(Type::Object), obj_(std::move(o)) {}
  /// Array of numbers; non-finite values become null.
  Json(const std::vector<double>& v);
  Json(const std::vector<std::string>& v);

  static Json object() { return Json(Object{}); }
  static Json array() { return Json(Array{}); }

  /// Parses JSON text; throws mcb::Error on malformed input.
  static Json parse(std::string_view text);
  /// Serialises; indent < 0 gives the most compact form.
  std::string dump(int indent = -1) const;

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  double number() const;                 // throws unless Number
  double numberOr(double fallback) const;
  bool boolean() const;
  const std::string& str() const;        // throws unless String
  std::string strOr(std::string_view fallback) const;
  const Array& items() const;            // throws unless Array
  const Object& members() const;         // throws unless Object
  std::size_t size() const;

  /// Object member lookup; returns a shared null value when absent.
  const Json& operator[](std::string_view key) const;
  /// Array element; throws when out of range.
  const Json& operator[](std::size_t index) const;
  bool contains(std::string_view key) const;

  /// Builders. set() replaces an existing key.
  Json& set(std::string key, Json value);
  Json& push(Json value);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  Array arr_;
  Object obj_;
};

/// Rounds to a number of decimal places (keeps result files small and stable).
double roundTo(double v, int decimals);

}  // namespace mcb
