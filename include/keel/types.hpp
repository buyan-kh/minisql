#pragma once

#include "keel/config.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace keel {

enum class Type : uint8_t { Integer = 1, Double = 2, Boolean = 3, Varchar = 4 };

const char* TypeName(Type type);

struct Column {
  std::string name;
  Type type{Type::Integer};
  uint32_t varchar_len{0};
  bool primary_key{false};
  bool not_null{false};
  // Runtime qualifier used by the planner. Not stored in the catalog.
  std::string table;
};

using Schema = std::vector<Column>;

struct Value {
  Type type{Type::Integer};
  bool is_null{true};
  int64_t i{0};
  double d{0};
  bool b{false};
  std::string s;

  static Value Null(Type type);
  static Value Int(int64_t v);
  static Value Double(double v);
  static Value Bool(bool v);
  static Value Varchar(std::string v);

  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }
};

using Tuple = std::vector<Value>;

struct RID {
  PageId page{kInvalidPage};
  uint16_t slot{0};
  bool operator==(const RID& other) const { return page == other.page && slot == other.slot; }
};

std::string FormatValue(const Value& value);
int CompareBytes(std::string_view a, std::string_view b);

// Order-preserving, prefix-free encoding of a value list. Used both for
// index keys and for hash/group keys. Null sorts before every real value.
void AppendEncodedValue(std::string& out, const Value& value);
std::string EncodeValues(const std::vector<Value>& values);
std::string WithRid(std::string user_key, RID rid);
std::string_view UserKeyOf(std::string_view full_key);

std::vector<uint8_t> SerializeTuple(const Schema& schema, const Tuple& tuple);
Tuple DeserializeTuple(const Schema& schema, const uint8_t* data, std::size_t len);

// Cast literals and expression results into a column type.
Value Coerce(const Value& value, const Column& column);

std::size_t EstimateRowBytes(const Schema& schema);

}  // namespace keel
