#pragma once

#include "keel/catalog.hpp"
#include "keel/planner.hpp"
#include "keel/store.hpp"

#include <memory>

namespace keel {

class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Open() = 0;
  virtual bool Next(Tuple& row) = 0;
  virtual void Close() = 0;
};

std::unique_ptr<Executor> MakeExecutor(Store& store, const Catalog& catalog, const Plan& plan);

}  // namespace keel
