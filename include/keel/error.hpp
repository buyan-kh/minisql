#pragma once

#include <stdexcept>
#include <string>

namespace keel {

// SQL and storage failures surface as Error. Execute() catches it and
// turns it into a result row so callers never see a raw exception.
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace keel
