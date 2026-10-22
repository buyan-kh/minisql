#include "keel/database.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string TempPath(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "keel_tests";
  std::filesystem::create_directories(dir);
  auto path = dir / (std::string(name) + "_" + std::to_string(::getpid()));
  std::filesystem::remove(path);
  std::filesystem::remove(std::string(path) + ".wal");
  return path.string();
}

std::string Trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  std::size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
    ++i;
  }
  return text.substr(i);
}

bool IsDirective(const std::string& line) {
  return line.rfind("statement", 0) == 0 || line.rfind("query", 0) == 0 || line == "halt";
}

std::vector<std::string> LoadLines(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

char TypeLetter(const keel::Value& value) {
  switch (value.type) {
    case keel::Type::Integer:
      return 'I';
    case keel::Type::Double:
      return 'D';
    case keel::Type::Boolean:
      return 'B';
    case keel::Type::Varchar:
      return 'T';
  }
  return '?';
}

std::vector<std::string> SplitCells(const std::string& line) {
  std::vector<std::string> cells;
  std::string current;
  for (char c : line) {
    if (c == '|') {
      cells.push_back(Trim(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  cells.push_back(Trim(current));
  return cells;
}

void RunScript(const std::filesystem::path& path) {
  const auto lines = LoadLines(path);
  const std::string db_path = TempPath(path.stem().string().c_str());
  keel::Options options;
  options.sync = false;
  options.buffer_pages = 32;
  keel::Database db(db_path, options);
  for (std::size_t i = 0; i < lines.size();) {
    const std::string line = Trim(lines[i]);
    if (line.empty() || line[0] == '#') {
      ++i;
      continue;
    }
    if (line == "halt") {
      break;
    }
    const std::size_t directive_line = i + 1;
    if (line.rfind("statement", 0) == 0) {
      const bool expect_ok = line.find("ok") != std::string::npos;
      ++i;
      std::string sql;
      while (i < lines.size() && !Trim(lines[i]).empty() && !IsDirective(Trim(lines[i]))) {
        sql.append(lines[i]);
        sql.push_back('\n');
        ++i;
      }
      const auto results = db.Execute(sql);
      bool ok = !results.empty();
      std::string error;
      for (const auto& result : results) {
        if (!result.ok) {
          ok = false;
          error = result.error;
        }
      }
      if (results.empty()) {
        ok = false;
        error = "no result";
      }
      if (expect_ok && !ok) {
        FAIL() << path.filename().string() << ":" << directive_line << " expected success for [" << sql
               << "] but got: " << error;
      }
      if (!expect_ok && ok) {
        FAIL() << path.filename().string() << ":" << directive_line << " expected an error for [" << sql << "]";
      }
      continue;
    }
    if (line.rfind("query", 0) == 0) {
      const std::string types = Trim(line.substr(5));
      ++i;
      std::string sql;
      while (i < lines.size() && Trim(lines[i]) != "----") {
        sql.append(lines[i]);
        sql.push_back('\n');
        ++i;
      }
      if (i >= lines.size() || Trim(lines[i]) != "----") {
        FAIL() << path.filename().string() << ":" << directive_line << " query is missing ----";
      }
      ++i;
      std::vector<std::string> expected;
      while (i < lines.size() && !Trim(lines[i]).empty() && !IsDirective(Trim(lines[i]))) {
        expected.push_back(Trim(lines[i]));
        ++i;
      }
      const auto results = db.Execute(sql);
      ASSERT_FALSE(results.empty()) << path.filename().string() << ":" << directive_line;
      if (!results.back().ok) {
        FAIL() << path.filename().string() << ":" << directive_line << " query failed: " << results.back().error
               << "\nSQL:\n"
               << sql;
      }
      const auto& rows = results.back().rows;
      ASSERT_EQ(rows.size(), expected.size()) << path.filename().string() << ":" << directive_line << "\nSQL:\n"
                                              << sql;
      for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto cells = SplitCells(expected[r]);
        ASSERT_EQ(cells.size(), rows[r].size()) << "row " << r << " of " << path.filename().string() << ":"
                                                << directive_line;
        ASSERT_EQ(types.size(), rows[r].size()) << "type string width at " << path.filename().string() << ":"
                                                << directive_line;
        for (std::size_t c = 0; c < cells.size(); ++c) {
          EXPECT_EQ(keel::FormatValue(rows[r][c]), cells[c])
              << path.filename().string() << ":" << directive_line << " row " << r << " col " << c;
          if (cells[c] != "NULL") {
            EXPECT_EQ(TypeLetter(rows[r][c]), types[c])
                << path.filename().string() << ":" << directive_line << " type of column " << c;
          }
        }
      }
      continue;
    }
    FAIL() << path.filename().string() << ":" << directive_line << " unknown directive " << line;
  }
}

}  // namespace

TEST(SqlLogic, Scripts) {
  const std::filesystem::path dir{KEEL_SQL_DIR};
  ASSERT_TRUE(std::filesystem::exists(dir)) << dir;
  int files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".test") {
      RunScript(entry.path());
      ++files;
    }
  }
  EXPECT_GT(files, 0);
}

TEST(Planner, ChoosesIndexForPointLookupAndScanForWideRange) {
  keel::Options options;
  options.sync = false;
  options.buffer_pages = 64;
  keel::Database db(TempPath("plan"), options);
  auto created = db.Execute(
      "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16));"
      "CREATE INDEX t_name ON t (name);");
  for (const auto& result : created) {
    ASSERT_TRUE(result.ok) << result.error;
  }
  std::ostringstream sql;
  sql << "INSERT INTO t VALUES ";
  for (int i = 0; i < 150; ++i) {
    if (i != 0) {
      sql << ',';
    }
    sql << '(' << i << ", 'n" << i << "')";
  }
  auto inserted = db.Execute(sql.str());
  ASSERT_TRUE(inserted[0].ok) << inserted[0].error;

  auto point = db.Execute("EXPLAIN SELECT id FROM t WHERE id = 40;");
  ASSERT_TRUE(point[0].ok) << point[0].error;
  EXPECT_NE(point[0].plan.find("IndexScan"), std::string::npos) << point[0].plan;
  EXPECT_EQ(point[0].plan.find("SeqScan"), std::string::npos) << point[0].plan;

  auto wide = db.Execute("EXPLAIN SELECT id FROM t WHERE id >= 0;");
  ASSERT_TRUE(wide[0].ok) << wide[0].error;
  EXPECT_NE(wide[0].plan.find("SeqScan"), std::string::npos) << wide[0].plan;
  EXPECT_EQ(wide[0].plan.find("IndexScan"), std::string::npos) << wide[0].plan;

  auto other = db.Execute("EXPLAIN SELECT id FROM t WHERE name >= 'n';");
  ASSERT_TRUE(other[0].ok) << other[0].error;
  EXPECT_NE(other[0].plan.find("SeqScan"), std::string::npos) << other[0].plan;

  auto got = db.Execute("SELECT id, name FROM t WHERE id = 40;");
  ASSERT_TRUE(got[0].ok) << got[0].error;
  ASSERT_EQ(got[0].rows.size(), 1u);
  EXPECT_EQ(keel::FormatValue(got[0].rows[0][0]), "40");
  EXPECT_EQ(keel::FormatValue(got[0].rows[0][1]), "n40");
}

TEST(Sql, JoinGroupOrderLimit) {
  keel::Options options;
  options.sync = false;
  keel::Database db(TempPath("join"), options);
  const char* setup =
      "CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(16));"
      "CREATE TABLE emp (id INT PRIMARY KEY, dept INT, name VARCHAR(16), pay INT);"
      "INSERT INTO dept VALUES (1, 'eng'), (2, 'sales');"
      "INSERT INTO emp VALUES (10, 1, 'ada', 30), (11, 1, 'bea', 40), (12, 2, 'cy', 20), (13, NULL, 'dee', NULL);";
  for (const auto& result : db.Execute(setup)) {
    ASSERT_TRUE(result.ok) << result.error;
  }
  auto joined = db.Execute(
      "SELECT dept.name, emp.name FROM dept JOIN emp ON dept.id = emp.dept "
      "WHERE emp.pay >= 30 ORDER BY emp.name;");
  ASSERT_TRUE(joined[0].ok) << joined[0].error;
  ASSERT_EQ(joined[0].rows.size(), 2u);
  EXPECT_EQ(keel::FormatValue(joined[0].rows[0][0]), "eng");
  EXPECT_EQ(keel::FormatValue(joined[0].rows[0][1]), "ada");
  EXPECT_EQ(keel::FormatValue(joined[0].rows[1][1]), "bea");

  auto plan = db.Execute(
      "EXPLAIN SELECT dept.name FROM dept JOIN emp ON dept.id = emp.dept;");
  ASSERT_TRUE(plan[0].ok) << plan[0].error;
  EXPECT_NE(plan[0].plan.find("HashJoin"), std::string::npos) << plan[0].plan;
  EXPECT_NE(plan[0].plan.find("build=dept"), std::string::npos) << plan[0].plan;

  auto grouped = db.Execute(
      "SELECT dept, COUNT(*), SUM(pay), AVG(pay), MIN(name), MAX(name) "
      "FROM emp GROUP BY dept ORDER BY dept;");
  ASSERT_TRUE(grouped[0].ok) << grouped[0].error;
  ASSERT_EQ(grouped[0].rows.size(), 3u);
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[0][0]), "NULL");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[0][1]), "1");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[0][2]), "NULL");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][0]), "1");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][1]), "2");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][2]), "70");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][3]), "35");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][4]), "ada");
  EXPECT_EQ(keel::FormatValue(grouped[0].rows[1][5]), "bea");

  auto limited = db.Execute("SELECT id FROM emp ORDER BY id DESC LIMIT 2;");
  ASSERT_TRUE(limited[0].ok) << limited[0].error;
  ASSERT_EQ(limited[0].rows.size(), 2u);
  EXPECT_EQ(keel::FormatValue(limited[0].rows[0][0]), "13");
  EXPECT_EQ(keel::FormatValue(limited[0].rows[1][0]), "12");

  auto cross = db.Execute("SELECT COUNT(*) FROM dept CROSS JOIN dept d2;");
  ASSERT_TRUE(cross[0].ok) << cross[0].error;
  EXPECT_EQ(keel::FormatValue(cross[0].rows[0][0]), "4");
}
