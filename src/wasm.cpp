#include "keel/database.hpp"

#include <emscripten/bind.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <utility>

namespace {

const char* kPath = "/keel.db";

keel::Options BrowserOptions() {
  keel::Options options;
  options.buffer_pages = 64;
  options.policy = keel::EvictPolicy::Clock;
  options.sync = false;
  return options;
}

std::string Escape(std::string_view text) {
  std::string out;
  for (unsigned char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          continue;
        }
        out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

const char* TypeLetter(keel::Type type) {
  switch (type) {
    case keel::Type::Integer:
      return "I";
    case keel::Type::Double:
      return "D";
    case keel::Type::Boolean:
      return "B";
    case keel::Type::Varchar:
      return "T";
  }
  return "?";
}

std::string ResultsJson(const std::vector<keel::QueryResult>& results) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (i != 0) {
      out << ',';
    }
    out << "{\"ok\":" << (result.ok ? "true" : "false");
    out << ",\"error\":\"" << Escape(result.error) << '"';
    out << ",\"message\":\"" << Escape(result.message) << '"';
    out << ",\"plan\":\"" << Escape(result.plan) << '"';
    out << ",\"elapsed_ms\":" << result.elapsed_ms;
    out << ",\"rows_affected\":" << result.rows_affected;
    out << ",\"columns\":[";
    for (std::size_t c = 0; c < result.columns.size(); ++c) {
      if (c != 0) {
        out << ',';
      }
      out << '"' << Escape(result.columns[c]) << '"';
    }
    out << "],\"types\":[";
    if (!result.rows.empty()) {
      for (std::size_t c = 0; c < result.rows[0].size(); ++c) {
        if (c != 0) {
          out << ',';
        }
        out << '"' << TypeLetter(result.rows[0][c].type) << '"';
      }
    }
    out << "],\"rows\":[";
    for (std::size_t r = 0; r < result.rows.size(); ++r) {
      if (r != 0) {
        out << ',';
      }
      out << '[';
      for (std::size_t c = 0; c < result.rows[r].size(); ++c) {
        if (c != 0) {
          out << ',';
        }
        out << '"' << Escape(keel::FormatValue(result.rows[r][c])) << '"';
      }
      out << ']';
    }
    out << "]}";
  }
  out << ']';
  return out.str();
}

class Session {
 public:
  Session() : db_(std::make_unique<keel::Database>(kPath, BrowserOptions())) {}

  std::string exec(const std::string& sql) {
    try {
      return ResultsJson(db_->Execute(sql));
    } catch (const std::exception& err) {
      return std::string("[{\"ok\":false,\"error\":\"") + Escape(err.what()) +
             "\",\"columns\":[],\"types\":[],\"rows\":[],\"message\":\"\",\"plan\":\"\",\"elapsed_ms\":0,"
             "\"rows_affected\":0}]";
    }
  }

  std::string schema() const {
    try {
      return db_->SchemaJson();
    } catch (const std::exception& err) {
      return std::string("{\"error\":\"") + Escape(err.what()) + "\"}";
    }
  }

  void reset() {
    db_.reset();
    std::filesystem::remove(kPath);
    std::filesystem::remove(std::string(kPath) + ".wal");
    db_ = std::make_unique<keel::Database>(kPath, BrowserOptions());
  }

 private:
  std::unique_ptr<keel::Database> db_;
};

}  // namespace

EMSCRIPTEN_BINDINGS(keel) {
  emscripten::class_<Session>("Database")
      .constructor<>()
      .function("exec", &Session::exec)
      .function("schema", &Session::schema)
      .function("reset", &Session::reset);
}
