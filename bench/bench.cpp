#include "keel/database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kCustomers = 400;
constexpr int kOrders = 2000;
constexpr int kLookupRuns = 200;
constexpr int kRepeats = 5;

struct Scale {
  int customers{0};
  int orders{0};
  int lineitems{0};
};

struct QueryTime {
  std::string name;
  std::string sql;
  double keel_ms{0};
  double sqlite_ms{0};
  int rows{0};
  std::string plan;
  uint64_t keel_misses{0};
};

double Median(std::vector<double> samples) {
  if (samples.empty()) {
    throw std::runtime_error("no samples");
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char c : text) {
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
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string JoinRows(const std::vector<keel::Tuple>& rows) {
  std::ostringstream out;
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i != 0) {
        out << '|';
      }
      out << keel::FormatValue(row[i]);
    }
    out << '\n';
  }
  return out.str();
}

void RequireOk(const std::vector<keel::QueryResult>& results, const std::string& what) {
  if (results.empty()) {
    throw std::runtime_error(what + " returned no result");
  }
  for (const auto& result : results) {
    if (!result.ok) {
      throw std::runtime_error(what + ": " + result.error);
    }
  }
}

std::string KeelText(keel::Database& db, const std::string& sql) {
  auto results = db.Execute(sql);
  RequireOk(results, sql);
  return JoinRows(results.back().rows);
}

class Sqlite {
 public:
  explicit Sqlite(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
      throw std::runtime_error("sqlite open failed");
    }
    Exec("PRAGMA journal_mode=WAL;");
    Exec("PRAGMA synchronous=FULL;");
  }

  ~Sqlite() {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
  }

  Sqlite(const Sqlite&) = delete;
  Sqlite& operator=(const Sqlite&) = delete;

  void Exec(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error != nullptr ? error : "sqlite exec failed";
      sqlite3_free(error);
      throw std::runtime_error(message + " in [" + sql + "]");
    }
  }

  std::string Query(const std::string& sql) {
    char* error = nullptr;
    std::string out;
    auto callback = [](void* ctx, int cols, char** values, char**) -> int {
      auto* buffer = static_cast<std::string*>(ctx);
      for (int i = 0; i < cols; ++i) {
        if (i != 0) {
          buffer->push_back('|');
        }
        buffer->append(values[i] != nullptr ? values[i] : "NULL");
      }
      buffer->push_back('\n');
      return 0;
    };
    if (sqlite3_exec(db_, sql.c_str(), callback, &out, &error) != SQLITE_OK) {
      std::string message = error != nullptr ? error : "sqlite query failed";
      sqlite3_free(error);
      throw std::runtime_error(message);
    }
    return out;
  }

  void Load(const Scale& scale) {
    Exec("BEGIN;");
    Exec("CREATE TABLE customer (c_custkey INT PRIMARY KEY, c_name TEXT, c_nation INT);");
    Exec("CREATE TABLE orders (o_orderkey INT PRIMARY KEY, o_custkey INT, o_total INT);");
    Exec("CREATE INDEX orders_cust ON orders (o_custkey);");
    Exec("CREATE TABLE lineitem (l_orderkey INT, l_linenumber INT, l_quantity INT, l_price INT);");
    Exec("CREATE INDEX lineitem_order ON lineitem (l_orderkey);");
    sqlite3_stmt* customer = nullptr;
    sqlite3_stmt* orders = nullptr;
    sqlite3_stmt* line = nullptr;
    Prepare(customer, "INSERT INTO customer VALUES (?, ?, ?);");
    Prepare(orders, "INSERT INTO orders VALUES (?, ?, ?);");
    Prepare(line, "INSERT INTO lineitem VALUES (?, ?, ?, ?);");
    for (int i = 1; i <= scale.customers; ++i) {
      sqlite3_bind_int(customer, 1, i);
      const std::string name = "c" + std::to_string(i);
      sqlite3_bind_text(customer, 2, name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(customer, 3, i % 25);
      StepReset(customer);
    }
    for (int i = 1; i <= scale.orders; ++i) {
      const int cust = 1 + (i * 17 % scale.customers);
      sqlite3_bind_int(orders, 1, i);
      sqlite3_bind_int(orders, 2, cust);
      sqlite3_bind_int(orders, 3, 100 + (i % 500));
      StepReset(orders);
      const int lines = 1 + (i % 4);
      for (int line_no = 1; line_no <= lines; ++line_no) {
        const int qty = 1 + ((i + line_no) % 10);
        sqlite3_bind_int(line, 1, i);
        sqlite3_bind_int(line, 2, line_no);
        sqlite3_bind_int(line, 3, qty);
        sqlite3_bind_int(line, 4, qty * (10 + (i % 20)));
        StepReset(line);
      }
    }
    sqlite3_finalize(customer);
    sqlite3_finalize(orders);
    sqlite3_finalize(line);
    Exec("COMMIT;");
  }

 private:
  void Prepare(sqlite3_stmt*& stmt, const char* sql) {
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(db_));
    }
  }

  void StepReset(sqlite3_stmt* stmt) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      throw std::runtime_error(std::string("step failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3* db_{nullptr};
};

Scale MakeScale() {
  Scale scale;
  scale.customers = kCustomers;
  scale.orders = kOrders;
  for (int i = 1; i <= scale.orders; ++i) {
    scale.lineitems += 1 + (i % 4);
  }
  return scale;
}

std::string CustomerInsert(const Scale& scale) {
  std::ostringstream sql;
  sql << "INSERT INTO customer VALUES ";
  for (int i = 1; i <= scale.customers; ++i) {
    if (i != 1) {
      sql << ',';
    }
    sql << '(' << i << ", 'c" << i << "', " << (i % 25) << ')';
  }
  return sql.str();
}

std::string OrdersInsert(const Scale& scale) {
  std::ostringstream sql;
  sql << "INSERT INTO orders VALUES ";
  for (int i = 1; i <= scale.orders; ++i) {
    if (i != 1) {
      sql << ',';
    }
    const int cust = 1 + (i * 17 % scale.customers);
    sql << '(' << i << ", " << cust << ", " << (100 + (i % 500)) << ')';
  }
  return sql.str();
}

std::string LineInsert(const Scale& scale) {
  std::ostringstream sql;
  sql << "INSERT INTO lineitem VALUES ";
  bool first = true;
  for (int i = 1; i <= scale.orders; ++i) {
    const int lines = 1 + (i % 4);
    for (int line_no = 1; line_no <= lines; ++line_no) {
      if (!first) {
        sql << ',';
      }
      first = false;
      const int qty = 1 + ((i + line_no) % 10);
      sql << '(' << i << ", " << line_no << ", " << qty << ", " << (qty * (10 + (i % 20))) << ')';
    }
  }
  return sql.str();
}

void LoadKeel(keel::Database& db, const Scale& scale) {
  std::string sql = "BEGIN; CREATE TABLE customer (c_custkey INT PRIMARY KEY, c_name VARCHAR(32), c_nation INT);"
                    "CREATE TABLE orders (o_orderkey INT PRIMARY KEY, o_custkey INT, o_total INT);"
                    "CREATE INDEX orders_cust ON orders (o_custkey);"
                    "CREATE TABLE lineitem (l_orderkey INT, l_linenumber INT, l_quantity INT, l_price INT);"
                    "CREATE INDEX lineitem_order ON lineitem (l_orderkey);";
  sql += CustomerInsert(scale);
  sql += ';';
  sql += OrdersInsert(scale);
  sql += ';';
  sql += LineInsert(scale);
  sql += "; COMMIT;";
  RequireOk(db.Execute(sql), "keel load");
}

double TimeKeelQuery(keel::Database& db, const std::string& sql) {
  RequireOk(db.Execute(sql), sql);
  std::vector<double> samples;
  for (int i = 0; i < kRepeats; ++i) {
    const auto start = std::chrono::steady_clock::now();
    RequireOk(db.Execute(sql), sql);
    samples.push_back(MsSince(start));
  }
  return Median(std::move(samples));
}

double TimeSqliteQuery(Sqlite& db, const std::string& sql) {
  db.Query(sql);
  std::vector<double> samples;
  for (int i = 0; i < kRepeats; ++i) {
    const auto start = std::chrono::steady_clock::now();
    db.Query(sql);
    samples.push_back(MsSince(start));
  }
  return Median(std::move(samples));
}

void CheckSame(const std::string& name, const std::string& keel, const std::string& sqlite) {
  if (keel != sqlite) {
    std::cerr << name << " results differ\n-- keel --\n" << keel << "-- sqlite --\n" << sqlite;
    throw std::runtime_error(name + " mismatch");
  }
}

int CountRows(const std::string& text) {
  return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

}  // namespace

int main() {
  try {
    const Scale scale = MakeScale();
    auto dir = std::filesystem::temp_directory_path() / "keel_bench";
    std::filesystem::create_directories(dir);
    const auto keel_path = (dir / "keel.db").string();
    const auto sqlite_path = (dir / "sqlite.db").string();
    std::filesystem::remove(keel_path);
    std::filesystem::remove(keel_path + ".wal");
    std::filesystem::remove(sqlite_path);
    std::filesystem::remove(sqlite_path + "-wal");
    std::filesystem::remove(sqlite_path + "-shm");

    keel::Options options;
    options.sync = true;
    options.buffer_pages = 256;
    options.policy = keel::EvictPolicy::Clock;

    double keel_load_ms = 0;
    {
      keel::Database db(keel_path, options);
      const auto start = std::chrono::steady_clock::now();
      LoadKeel(db, scale);
      keel_load_ms = MsSince(start);
    }
    double sqlite_load_ms = 0;
    {
      Sqlite db(sqlite_path);
      const auto start = std::chrono::steady_clock::now();
      db.Load(scale);
      sqlite_load_ms = MsSince(start);
    }

    keel::Database keel(keel_path, options);
    Sqlite sqlite(sqlite_path);

    const std::vector<std::pair<std::string, std::string>> queries = {
        {"scan_agg", "SELECT COUNT(*), SUM(l_quantity), SUM(l_price) FROM lineitem;"},
        {"pk_lookup_batch", ""},
        {"secondary_lookup", "SELECT o_orderkey, o_total FROM orders WHERE o_custkey = 18 ORDER BY o_orderkey;"},
        {"hash_join_group",
         "SELECT c_nation, COUNT(*), SUM(o_total) FROM customer JOIN orders ON customer.c_custkey = "
         "orders.o_custkey GROUP BY c_nation ORDER BY c_nation;"},
    };

    std::vector<QueryTime> times;
    for (const auto& [name, sql] : queries) {
      QueryTime row;
      row.name = name;
      row.sql = sql;
      if (name == "pk_lookup_batch") {
        for (int i = 0; i < kLookupRuns; ++i) {
          const int key = 1 + (i * 3 % scale.customers);
          const std::string one = "SELECT c_name, c_nation FROM customer WHERE c_custkey = " + std::to_string(key) + ";";
          const std::string k = KeelText(keel, one);
          const std::string s = sqlite.Query(one);
          CheckSame(one, k, s);
          if (CountRows(k) != 1) {
            throw std::runtime_error("primary key lookup returned " + std::to_string(CountRows(k)) + " rows");
          }
        }
        auto time_keel = [&] {
          std::vector<double> samples;
          for (int rep = 0; rep < kRepeats; ++rep) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kLookupRuns; ++i) {
              const int key = 1 + (i * 3 % scale.customers);
              RequireOk(keel.Execute("SELECT c_name, c_nation FROM customer WHERE c_custkey = " + std::to_string(key) +
                                     ";"),
                        "pk");
            }
            samples.push_back(MsSince(start));
          }
          return Median(std::move(samples));
        };
        auto time_sqlite = [&] {
          std::vector<double> samples;
          for (int rep = 0; rep < kRepeats; ++rep) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kLookupRuns; ++i) {
              const int key = 1 + (i * 3 % scale.customers);
              sqlite.Query("SELECT c_name, c_nation FROM customer WHERE c_custkey = " + std::to_string(key) + ";");
            }
            samples.push_back(MsSince(start));
          }
          return Median(std::move(samples));
        };
        row.keel_ms = time_keel();
        row.sqlite_ms = time_sqlite();
        row.rows = kLookupRuns;
        row.sql = "SELECT c_name, c_nation FROM customer WHERE c_custkey = ?;  -- " + std::to_string(kLookupRuns) +
                  " keys";
        auto explained = keel.Execute("EXPLAIN SELECT c_name, c_nation FROM customer WHERE c_custkey = 1;");
        RequireOk(explained, "explain pk");
        row.plan = explained[0].plan;
        keel.ResetBufferStats();
        RequireOk(keel.Execute("SELECT c_name, c_nation FROM customer WHERE c_custkey = 1;"), "pk stats");
        row.keel_misses = keel.buffer_stats().misses;
      } else {
        const std::string keel_text = KeelText(keel, sql);
        const std::string sqlite_text = sqlite.Query(sql);
        CheckSame(name, keel_text, sqlite_text);
        row.rows = CountRows(keel_text);
        row.keel_ms = TimeKeelQuery(keel, sql);
        row.sqlite_ms = TimeSqliteQuery(sqlite, sql);
        auto explained = keel.Execute(std::string("EXPLAIN ") + sql);
        RequireOk(explained, "explain");
        row.plan = explained[0].plan;
        keel.ResetBufferStats();
        RequireOk(keel.Execute(sql), name);
        row.keel_misses = keel.buffer_stats().misses;
      }
      times.push_back(std::move(row));
      std::cout << times.back().name << " keel_ms=" << times.back().keel_ms
                << " sqlite_ms=" << times.back().sqlite_ms << " rows=" << times.back().rows << '\n';
    }

    keel::Database wide(keel_path, options);
    wide.ResetBufferStats();
    RequireOk(wide.Execute("SELECT COUNT(*) FROM lineitem WHERE l_orderkey >= 0;"), "wide");
    const uint64_t wide_misses = wide.buffer_stats().misses;
    auto wide_plan = wide.Execute("EXPLAIN SELECT COUNT(*) FROM lineitem WHERE l_orderkey >= 0;");
    RequireOk(wide_plan, "explain wide");

    int lost = 0;
    double reopen_total = 0;
    constexpr int kTrials = 5;
    constexpr int kRecoverRows = 500;
    for (int trial = 0; trial < kTrials; ++trial) {
      const auto path = (dir / ("recover_" + std::to_string(trial) + ".db")).string();
      std::filesystem::remove(path);
      std::filesystem::remove(path + ".wal");
      {
        keel::Database db(path, options);
        std::ostringstream sql;
        sql << "BEGIN; CREATE TABLE t (id INT PRIMARY KEY, v INT); INSERT INTO t VALUES ";
        for (int i = 1; i <= kRecoverRows; ++i) {
          if (i != 1) {
            sql << ',';
          }
          sql << '(' << i << ", " << (i * 3) << ')';
        }
        sql << "; COMMIT;";
        RequireOk(db.Execute(sql.str()), "recovery load");
        db.Abandon();
      }
      const auto start = std::chrono::steady_clock::now();
      keel::Database db(path, options);
      auto got = db.Execute("SELECT COUNT(*), SUM(v) FROM t;");
      const double reopen_ms = MsSince(start);
      reopen_total += reopen_ms;
      RequireOk(got, "recovery read");
      const std::string text = JoinRows(got[0].rows);
      const long long expect_sum = static_cast<long long>(kRecoverRows) * (kRecoverRows + 1) / 2 * 3;
      const std::string expect = std::to_string(kRecoverRows) + "|" + std::to_string(expect_sum) + "\n";
      if (text != expect) {
        ++lost;
        std::cerr << "recovery trial " << trial << " got [" << text << "] want [" << expect << "]\n";
      }
    }

    const auto out_path = std::filesystem::path("/workspace/bench/results.json");
    std::ofstream out(out_path);
    out << "{\n";
    out << "  \"build\": \"Release g++ -O3, sanitizers off\",\n";
    out << "  \"keel\": {\"sync\": true, \"buffer_pages\": 256, \"policy\": \"clock\"},\n";
    out << "  \"sqlite\": {\"journal_mode\": \"WAL\", \"synchronous\": \"FULL\", \"version\": \""
        << JsonEscape(sqlite3_libversion()) << "\"},\n";
    out << "  \"scale\": {\"customers\": " << scale.customers << ", \"orders\": " << scale.orders
        << ", \"lineitems\": " << scale.lineitems << "},\n";
    out << "  \"timing\": \"median of " << kRepeats
        << " runs after one warmup; buffer pool is cold per process and the OS cache is warm\",\n";
    out << "  \"load_ms\": {\"keel\": " << keel_load_ms << ", \"sqlite\": " << sqlite_load_ms << "},\n";
    out << "  \"wide_range_lineitem_misses\": " << wide_misses << ",\n";
    out << "  \"wide_range_plan\": \"" << JsonEscape(wide_plan[0].plan) << "\",\n";
    out << "  \"queries\": [\n";
    for (std::size_t i = 0; i < times.size(); ++i) {
      const auto& row = times[i];
      out << "    {\"name\": \"" << row.name << "\", \"rows\": " << row.rows << ", \"keel_ms\": " << row.keel_ms
          << ", \"sqlite_ms\": " << row.sqlite_ms << ", \"keel_buffer_misses\": " << row.keel_misses
          << ", \"sql\": \"" << JsonEscape(row.sql) << "\", \"plan\": \"" << JsonEscape(row.plan) << "\"}";
      out << (i + 1 == times.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"recovery\": {\"rows\": " << kRecoverRows << ", \"trials\": " << kTrials << ", \"lost_commits\": " << lost
        << ", \"reopen_ms_total\": " << reopen_total << ", \"reopen_ms_mean\": " << (reopen_total / kTrials) << "}\n";
    out << "}\n";
    out.close();

    std::cout << "load keel_ms=" << keel_load_ms << " sqlite_ms=" << sqlite_load_ms << '\n';
    std::cout << "wide_range_misses=" << wide_misses << '\n';
    std::cout << "recovery trials=" << kTrials << " lost_commits=" << lost
              << " reopen_ms_mean=" << (reopen_total / kTrials) << '\n';
    std::cout << "wrote " << out_path << '\n';
    return lost == 0 ? 0 : 1;
  } catch (const std::exception& err) {
    std::cerr << "bench failed: " << err.what() << '\n';
    return 1;
  }
}
