#include "keel/types.hpp"

#include "keel/buf.hpp"
#include "keel/error.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace keel {
namespace {

void AppendBE(std::string& out, uint64_t u) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((u >> shift) & 0xFF));
  }
}

}  // namespace

const char* TypeName(Type type) {
  switch (type) {
    case Type::Integer:
      return "INT";
    case Type::Double:
      return "DOUBLE";
    case Type::Boolean:
      return "BOOL";
    case Type::Varchar:
      return "VARCHAR";
  }
  return "UNKNOWN";
}

Value Value::Null(Type type) {
  Value v;
  v.type = type;
  v.is_null = true;
  return v;
}

Value Value::Int(int64_t v) {
  Value out;
  out.type = Type::Integer;
  out.is_null = false;
  out.i = v;
  return out;
}

Value Value::Double(double v) {
  if (!std::isfinite(v)) {
    throw Error("only finite doubles are supported");
  }
  Value out;
  out.type = Type::Double;
  out.is_null = false;
  out.d = v;
  return out;
}

Value Value::Bool(bool v) {
  Value out;
  out.type = Type::Boolean;
  out.is_null = false;
  out.b = v;
  return out;
}

Value Value::Varchar(std::string v) {
  Value out;
  out.type = Type::Varchar;
  out.is_null = false;
  out.s = std::move(v);
  return out;
}

bool Value::operator==(const Value& other) const {
  if (is_null || other.is_null) {
    return is_null && other.is_null && type == other.type;
  }
  if (type != other.type) {
    return false;
  }
  switch (type) {
    case Type::Integer:
      return i == other.i;
    case Type::Double:
      return d == other.d;
    case Type::Boolean:
      return b == other.b;
    case Type::Varchar:
      return s == other.s;
  }
  return false;
}

std::string FormatValue(const Value& value) {
  if (value.is_null) {
    return "NULL";
  }
  switch (value.type) {
    case Type::Integer:
      return std::to_string(value.i);
    case Type::Double: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.10g", value.d);
      return buf;
    }
    case Type::Boolean:
      return value.b ? "true" : "false";
    case Type::Varchar:
      return value.s;
  }
  return "?";
}

int CompareBytes(std::string_view a, std::string_view b) {
  const std::size_t n = std::min(a.size(), b.size());
  if (n > 0) {
    const int c = std::memcmp(a.data(), b.data(), n);
    if (c < 0) {
      return -1;
    }
    if (c > 0) {
      return 1;
    }
  }
  if (a.size() == b.size()) {
    return 0;
  }
  return a.size() < b.size() ? -1 : 1;
}

void AppendEncodedValue(std::string& out, const Value& value) {
  if (value.is_null) {
    out.push_back('\0');
    return;
  }
  out.push_back('\1');
  switch (value.type) {
    case Type::Integer: {
      const uint64_t u = static_cast<uint64_t>(value.i) ^ (1ULL << 63);
      AppendBE(out, u);
      break;
    }
    case Type::Double: {
      uint64_t u = 0;
      std::memcpy(&u, &value.d, sizeof(u));
      if ((u & (1ULL << 63)) != 0) {
        u = ~u;
      } else {
        u ^= (1ULL << 63);
      }
      AppendBE(out, u);
      break;
    }
    case Type::Boolean:
      out.push_back(value.b ? '\2' : '\1');
      break;
    case Type::Varchar:
      for (unsigned char c : value.s) {
        if (c == 0) {
          out.push_back('\0');
          out.push_back(static_cast<char>(0xFF));
        } else {
          out.push_back(static_cast<char>(c));
        }
      }
      out.push_back('\0');
      out.push_back('\0');
      break;
  }
}

std::string EncodeValues(const std::vector<Value>& values) {
  std::string out;
  out.reserve(values.size() * 16);
  for (const auto& value : values) {
    AppendEncodedValue(out, value);
  }
  return out;
}

std::string WithRid(std::string user_key, RID rid) {
  user_key.push_back(static_cast<char>((rid.page >> 24) & 0xFF));
  user_key.push_back(static_cast<char>((rid.page >> 16) & 0xFF));
  user_key.push_back(static_cast<char>((rid.page >> 8) & 0xFF));
  user_key.push_back(static_cast<char>(rid.page & 0xFF));
  user_key.push_back(static_cast<char>((rid.slot >> 8) & 0xFF));
  user_key.push_back(static_cast<char>(rid.slot & 0xFF));
  return user_key;
}

std::string_view UserKeyOf(std::string_view full_key) {
  if (full_key.size() < kRidBytes) {
    throw Error("index key is shorter than a RID");
  }
  return full_key.substr(0, full_key.size() - kRidBytes);
}

std::vector<uint8_t> SerializeTuple(const Schema& schema, const Tuple& tuple) {
  if (tuple.size() != schema.size()) {
    throw Error("tuple arity does not match the table schema");
  }
  const std::size_t bitmap = (schema.size() + 7) / 8;
  std::string body(bitmap, '\0');
  for (std::size_t i = 0; i < schema.size(); ++i) {
    const Value coerced = Coerce(tuple[i], schema[i]);
    if (coerced.is_null) {
      if (schema[i].not_null || schema[i].primary_key) {
        throw Error("column " + schema[i].name + " cannot be null");
      }
      body[i / 8] = static_cast<char>(static_cast<unsigned char>(body[i / 8]) | (1u << (i % 8)));
      continue;
    }
    switch (schema[i].type) {
      case Type::Integer: {
        char b[8];
        WriteU64(b, static_cast<uint64_t>(coerced.i));
        body.append(b, 8);
        break;
      }
      case Type::Double: {
        char b[8];
        std::memcpy(b, &coerced.d, 8);
        body.append(b, 8);
        break;
      }
      case Type::Boolean:
        body.push_back(coerced.b ? '\1' : '\0');
        break;
      case Type::Varchar: {
        if (schema[i].varchar_len != 0 && coerced.s.size() > schema[i].varchar_len) {
          throw Error("value too long for " + schema[i].name);
        }
        char b[2];
        WriteU16(b, static_cast<uint16_t>(coerced.s.size()));
        body.append(b, 2);
        body.append(coerced.s);
        break;
      }
    }
  }
  return std::vector<uint8_t>(body.begin(), body.end());
}

Tuple DeserializeTuple(const Schema& schema, const uint8_t* data, std::size_t len) {
  Reader in(reinterpret_cast<const char*>(data), len);
  const std::size_t bitmap = (schema.size() + 7) / 8;
  const std::string bits = in.bytes(bitmap);
  Tuple tuple;
  tuple.reserve(schema.size());
  for (std::size_t i = 0; i < schema.size(); ++i) {
    const bool is_null = (static_cast<unsigned char>(bits[i / 8]) & (1u << (i % 8))) != 0;
    if (is_null) {
      tuple.push_back(Value::Null(schema[i].type));
      continue;
    }
    switch (schema[i].type) {
      case Type::Integer:
        tuple.push_back(Value::Int(static_cast<int64_t>(in.u64())));
        break;
      case Type::Double: {
        const std::string raw = in.bytes(8);
        double d = 0;
        std::memcpy(&d, raw.data(), 8);
        tuple.push_back(Value::Double(d));
        break;
      }
      case Type::Boolean:
        tuple.push_back(Value::Bool(in.u8() != 0));
        break;
      case Type::Varchar: {
        const uint16_t n = in.u16();
        tuple.push_back(Value::Varchar(in.bytes(n)));
        break;
      }
    }
  }
  if (!in.empty()) {
    throw Error("tuple has trailing bytes");
  }
  return tuple;
}

Value Coerce(const Value& value, const Column& column) {
  if (value.is_null) {
    return Value::Null(column.type);
  }
  if (value.type == column.type) {
    if (column.type == Type::Varchar && column.varchar_len != 0 && value.s.size() > column.varchar_len) {
      throw Error("value too long for " + column.name);
    }
    return value;
  }
  if (value.type == Type::Integer && column.type == Type::Double) {
    return Value::Double(static_cast<double>(value.i));
  }
  if (value.type == Type::Double && column.type == Type::Integer) {
    if (value.d < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        value.d > static_cast<double>(std::numeric_limits<int64_t>::max())) {
      throw Error("double does not fit in INT");
    }
    const auto as_int = static_cast<int64_t>(value.d);
    if (static_cast<double>(as_int) != value.d) {
      throw Error("cannot coerce a fractional double to INT");
    }
    return Value::Int(as_int);
  }
  throw Error(std::string("cannot coerce ") + TypeName(value.type) + " to " + TypeName(column.type));
}

std::size_t EstimateRowBytes(const Schema& schema) {
  std::size_t n = (schema.size() + 7) / 8;
  for (const auto& column : schema) {
    switch (column.type) {
      case Type::Integer:
      case Type::Double:
        n += 8;
        break;
      case Type::Boolean:
        n += 1;
        break;
      case Type::Varchar:
        n += 18;
        break;
    }
  }
  return n + 4;
}

}  // namespace keel
