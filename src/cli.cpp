#include "keel/database.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

void PrintResult(const keel::QueryResult& result) {
  if (!result.ok) {
    std::cerr << "error: " << result.error << '\n';
    return;
  }
  if (!result.plan.empty()) {
    std::cout << result.plan;
    if (result.plan.back() != '\n') {
      std::cout << '\n';
    }
  }
  if (!result.columns.empty()) {
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
      if (i != 0) {
        std::cout << '|';
      }
      std::cout << result.columns[i];
    }
    std::cout << '\n';
    for (const auto& row : result.rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) {
          std::cout << '|';
        }
        std::cout << keel::FormatValue(row[i]);
      }
      std::cout << '\n';
    }
    std::cout << result.message << '\n';
    return;
  }
  if (!result.message.empty()) {
    std::cout << result.message << '\n';
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw keel::Error("cannot read " + path);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: keel <database> [-c sql | -f file]\n";
    return 2;
  }
  try {
    keel::Database db(argv[1]);
    if (argc >= 4 && std::string(argv[2]) == "-c") {
      bool failed = false;
      for (const auto& result : db.Execute(argv[3])) {
        PrintResult(result);
        failed = failed || !result.ok;
      }
      return failed ? 1 : 0;
    }
    if (argc >= 4 && std::string(argv[2]) == "-f") {
      bool failed = false;
      for (const auto& result : db.Execute(ReadFile(argv[3]))) {
        PrintResult(result);
        failed = failed || !result.ok;
      }
      return failed ? 1 : 0;
    }
    if (argc != 2) {
      std::cerr << "usage: keel <database> [-c sql | -f file]\n";
      return 2;
    }
    const bool interactive = ::isatty(STDIN_FILENO) == 1;
    std::string pending;
    std::string line;
    while (true) {
      if (interactive && pending.empty()) {
        std::cout << "keel> " << std::flush;
      } else if (interactive) {
        std::cout << "....> " << std::flush;
      }
      if (!std::getline(std::cin, line)) {
        break;
      }
      pending.append(line);
      pending.push_back('\n');
      if (line.find(';') == std::string::npos) {
        continue;
      }
      bool failed = false;
      for (const auto& result : db.Execute(pending)) {
        PrintResult(result);
        failed = failed || !result.ok;
      }
      pending.clear();
      if (failed && !interactive) {
        return 1;
      }
    }
    return 0;
  } catch (const std::exception& err) {
    std::cerr << "error: " << err.what() << '\n';
    return 1;
  }
}
