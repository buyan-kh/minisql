#pragma once

#include "keel/buffer_pool.hpp"
#include "keel/catalog.hpp"
#include "keel/disk_manager.hpp"
#include "keel/store.hpp"
#include "keel/wal.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace keel {

struct Options {
  std::size_t buffer_pages{128};
  EvictPolicy policy{EvictPolicy::Clock};
  bool sync{true};
};

struct QueryResult {
  bool ok{true};
  std::string error;
  std::vector<std::string> columns;
  std::vector<Tuple> rows;
  std::string message;
  uint64_t rows_affected{0};
  std::string plan;
  double elapsed_ms{0};
};

// One open transaction at a time. An explicit BEGIN holds the database latch
// until COMMIT or ROLLBACK, so statements from other threads wait and the
// schedule is serial. Auto-commit statements take the latch for one statement.
class Database {
 public:
  explicit Database(std::string path, Options options = {});
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  std::vector<QueryResult> Execute(const std::string& sql);
  void Checkpoint();
  void FlushDirty();
  // Drop dirty pages and skip shutdown. The next open runs recovery.
  void Abandon();
  BufferStats buffer_stats() const;
  void ResetBufferStats();
  std::string SchemaJson() const;

 private:
  struct Txn {
    TxnId id{0};
    LSN last{0};
    bool open{false};
  };

  void OnUpdate(PageGuard& page, const char* before);
  void Bootstrap();
  void StartTxn();
  void CommitTxn();
  void AbortTxn();
  void PersistCatalog();
  void CheckpointUnlocked();
  QueryResult ExecOne(const struct Statement& stmt);
  void CreateTable(const struct CreateTableData& data);
  void CreateIndex(const struct CreateIndexData& data);
  uint64_t InsertRows(const struct InsertData& data);
  uint64_t UpdateRows(const struct UpdateData& data);
  uint64_t DeleteRows(const struct DeleteData& data);
  QueryResult RunSelect(const struct SelectData& data, bool with_plan, bool execute);
  void InsertIndexEntry(IndexInfo& index, const Schema& schema, const Tuple& tuple, RID rid);
  void RemoveIndexEntry(IndexInfo& index, const Schema& schema, const Tuple& tuple, RID rid);
  bool IndexContains(const IndexInfo& index, const std::string& user, RID skip);
  Schema Aliased(const TableInfo& table) const;

  Options options_;
  DiskManager disk_;
  Wal wal_;
  BufferPool pool_;
  Store store_;
  Catalog catalog_;
  Catalog committed_;
  Txn txn_;
  TxnId next_txn_{1};
  bool catalog_dirty_{false};
  bool explicit_txn_{false};
  bool opened_{false};
  bool abandoned_{false};
  mutable std::recursive_mutex mu_;
  std::unique_lock<std::recursive_mutex> txn_hold_;
};

}  // namespace keel
