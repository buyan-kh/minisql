#include "keel/database.hpp"

#include "keel/ast.hpp"
#include "keel/btree.hpp"
#include "keel/error.hpp"
#include "keel/executor.hpp"
#include "keel/expr.hpp"
#include "keel/heap.hpp"
#include "keel/page.hpp"
#include "keel/planner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>

namespace keel {
namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::string UserKey(const IndexInfo& index, const Tuple& tuple, bool& any_null) {
  std::vector<Value> values;
  values.reserve(index.columns.size());
  any_null = false;
  for (uint16_t column : index.columns) {
    const Value& value = tuple[column];
    if (value.is_null) {
      any_null = true;
    }
    values.push_back(value);
  }
  return EncodeValues(values);
}

int ColumnIndex(const Schema& schema, const std::string& name) {
  return ResolveColumn(schema, "", name);
}

}  // namespace

Database::Database(std::string path, Options options)
    : options_(options),
      disk_(std::move(path), options_.sync),
      wal_(disk_.path() + ".wal", options_.sync),
      pool_(disk_, std::max(options_.buffer_pages, std::size_t{8}), options_.policy,
            [this](LSN lsn) { wal_.FlushThrough(lsn); }),
      store_(pool_, [this](PageGuard& page, const char* before) { OnUpdate(page, before); }) {
  if (disk_.page_count() == 0) {
    Bootstrap();
  } else {
    Recover(disk_, wal_);
    PageGuard meta = pool_.Fetch(0);
    if (!MetaMagicOk(meta.data())) {
      throw Error("not a keel database");
    }
    catalog_ = LoadCatalog(store_);
    committed_ = catalog_;
  }
  opened_ = true;
}

Database::~Database() {
  if (!opened_ || abandoned_) {
    return;
  }
  try {
    std::lock_guard lock(mu_);
    if (txn_.open) {
      AbortTxn();
      explicit_txn_ = false;
      if (txn_hold_.owns_lock()) {
        txn_hold_.unlock();
      }
    }
    CheckpointUnlocked();
    LogRecord record;
    record.type = LogType::Shutdown;
    wal_.Append(record);
    wal_.Flush();
  } catch (...) {
  }
}

void Database::OnUpdate(PageGuard& page, const char* before) {
  if (!txn_.open) {
    throw Error("page write outside a transaction");
  }
  const LSN lsn = wal_.next_lsn();
  PageSetLsn(page.data(), lsn);
  SealPage(page.data());
  LogRecord record;
  record.lsn = lsn;
  record.txn = txn_.id;
  record.prev = txn_.last;
  record.type = LogType::PageWrite;
  record.page = page.id();
  record.before.assign(before, before + kPageSize);
  record.after.assign(page.data(), page.data() + kPageSize);
  txn_.last = wal_.Append(record);
  page.MarkDirty();
}

void Database::Bootstrap() {
  StartTxn();
  {
    PageGuard meta = store_.NewPage(PageType::Meta);
    if (meta.id() != 0) {
      throw Error("meta page was not allocated at page 0");
    }
    store_.Update(meta, [](char* raw) {
      std::memcpy(raw + kPageHeaderSize, kMagic, 8);
      WriteU32(raw + kPageHeaderSize + 8, 1);
    });
  }
  {
    PageGuard catalog = store_.NewPage(PageType::Catalog);
    if (catalog.id() != 1) {
      throw Error("catalog page was not allocated at page 1");
    }
  }
  catalog_ = Catalog{};
  catalog_dirty_ = true;
  PersistCatalog();
  CommitTxn();
  CheckpointUnlocked();
}

void Database::StartTxn() {
  if (txn_.open) {
    throw Error("transaction already open");
  }
  txn_.id = next_txn_++;
  txn_.last = 0;
  LogRecord record;
  record.txn = txn_.id;
  record.type = LogType::Begin;
  txn_.last = wal_.Append(record);
  txn_.open = true;
}

void Database::CommitTxn() {
  if (!txn_.open) {
    throw Error("no transaction to commit");
  }
  if (catalog_dirty_) {
    PersistCatalog();
  }
  LogRecord record;
  record.txn = txn_.id;
  record.prev = txn_.last;
  record.type = LogType::Commit;
  txn_.last = wal_.Append(record);
  wal_.Flush();
  txn_.open = false;
  committed_ = catalog_;
  catalog_dirty_ = false;
}

void Database::AbortTxn() {
  if (!txn_.open) {
    return;
  }
  const TxnId id = txn_.id;
  const LSN last = txn_.last;
  txn_.open = false;
  UndoTransaction(wal_, id, last, [this](PageId page, const char* image) {
    PageGuard guard = pool_.Fetch(page);
    std::memcpy(guard.data(), image, kPageSize);
    guard.MarkDirty();
  });
  catalog_ = committed_;
  catalog_dirty_ = false;
}

void Database::PersistCatalog() { SaveCatalog(store_, catalog_); }

void Database::CheckpointUnlocked() {
  if (txn_.open) {
    throw Error("cannot checkpoint inside a transaction");
  }
  pool_.FlushAll();
  LogRecord record;
  record.type = LogType::Checkpoint;
  record.lsn = wal_.next_lsn();
  const LSN lsn = wal_.Append(record);
  wal_.SetCheckpoint(lsn);
  wal_.Flush();
}

void Database::Checkpoint() {
  std::lock_guard lock(mu_);
  CheckpointUnlocked();
}

void Database::FlushDirty() {
  std::lock_guard lock(mu_);
  pool_.FlushAll();
}

void Database::Abandon() {
  std::lock_guard lock(mu_);
  abandoned_ = true;
  opened_ = false;
  explicit_txn_ = false;
  if (txn_hold_.owns_lock()) {
    txn_hold_.unlock();
  }
  pool_.Discard();
}

BufferStats Database::buffer_stats() const { return pool_.Stats(); }

void Database::ResetBufferStats() { pool_.ResetStats(); }

std::string Database::SchemaJson() const {
  std::lock_guard lock(mu_);
  std::ostringstream out;
  out << '[';
  for (std::size_t t = 0; t < catalog_.tables.size(); ++t) {
    const TableInfo& table = catalog_.tables[t];
    if (t > 0) {
      out << ',';
    }
    out << "{\"name\":\"" << JsonEscape(table.name) << "\",\"columns\":[";
    for (std::size_t c = 0; c < table.schema.size(); ++c) {
      const Column& column = table.schema[c];
      if (c > 0) {
        out << ',';
      }
      out << "{\"name\":\"" << JsonEscape(column.name) << "\",\"type\":\"" << TypeName(column.type) << '"';
      if (column.type == Type::Varchar) {
        out << ",\"length\":" << column.varchar_len;
      }
      out << ",\"primary_key\":" << (column.primary_key ? "true" : "false");
      out << ",\"not_null\":" << (column.not_null ? "true" : "false") << '}';
    }
    out << "],\"indexes\":[";
    bool first = true;
    for (const auto& index : catalog_.indexes) {
      if (index.table_id != table.id) {
        continue;
      }
      if (!first) {
        out << ',';
      }
      first = false;
      out << "{\"name\":\"" << JsonEscape(index.name) << "\",\"unique\":" << (index.unique ? "true" : "false")
          << ",\"columns\":[";
      for (std::size_t c = 0; c < index.columns.size(); ++c) {
        if (c > 0) {
          out << ',';
        }
        const auto ordinal = index.columns[c];
        const std::string name = ordinal < table.schema.size() ? table.schema[ordinal].name : "?";
        out << '"' << JsonEscape(name) << '"';
      }
      out << "]}";
    }
    out << "],\"rows\":" << table.rows << '}';
  }
  out << ']';
  return out.str();
}

std::vector<QueryResult> Database::Execute(const std::string& sql) {
  std::unique_lock lock(mu_);
  std::vector<QueryResult> results;
  std::vector<Statement> statements;
  try {
    statements = ParseSql(sql);
  } catch (const Error& err) {
    QueryResult result;
    result.ok = false;
    result.error = err.what();
    results.push_back(result);
    return results;
  }
  for (const auto& statement : statements) {
    const auto started = std::chrono::steady_clock::now();
    try {
      QueryResult result = ExecOne(statement);
      const auto elapsed = std::chrono::steady_clock::now() - started;
      result.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
      results.push_back(std::move(result));
    } catch (const std::exception& err) {
      QueryResult result;
      result.ok = false;
      result.error = err.what();
      const auto elapsed = std::chrono::steady_clock::now() - started;
      result.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
      results.push_back(std::move(result));
      break;
    }
  }
  return results;
}

QueryResult Database::ExecOne(const Statement& stmt) {
  if (stmt.kind == Statement::Kind::Begin) {
    if (txn_.open) {
      throw Error("transaction already open");
    }
    StartTxn();
    explicit_txn_ = true;
    if (!txn_hold_.owns_lock()) {
      txn_hold_ = std::unique_lock<std::recursive_mutex>(mu_);
    }
    QueryResult result;
    result.message = "begin";
    return result;
  }
  if (stmt.kind == Statement::Kind::Commit) {
    if (!txn_.open || !explicit_txn_) {
      throw Error("no transaction to commit");
    }
    CommitTxn();
    explicit_txn_ = false;
    if (txn_hold_.owns_lock()) {
      txn_hold_.unlock();
    }
    QueryResult result;
    result.message = "commit";
    return result;
  }
  if (stmt.kind == Statement::Kind::Rollback) {
    if (!txn_.open || !explicit_txn_) {
      throw Error("no transaction to roll back");
    }
    AbortTxn();
    explicit_txn_ = false;
    if (txn_hold_.owns_lock()) {
      txn_hold_.unlock();
    }
    QueryResult result;
    result.message = "rollback";
    return result;
  }

  const bool auto_commit = !txn_.open;
  if (auto_commit) {
    StartTxn();
  }
  try {
    QueryResult result;
    switch (stmt.kind) {
      case Statement::Kind::CreateTable:
        if (stmt.explain && !stmt.analyze) {
          result.plan = "CreateTable " + stmt.create_table.name;
          result.message = "explain";
          break;
        }
        CreateTable(stmt.create_table);
        result.message = "created table " + stmt.create_table.name;
        break;
      case Statement::Kind::CreateIndex:
        if (stmt.explain && !stmt.analyze) {
          result.plan = "CreateIndex " + stmt.create_index.name;
          result.message = "explain";
          break;
        }
        CreateIndex(stmt.create_index);
        result.message = "created index " + stmt.create_index.name;
        break;
      case Statement::Kind::Insert:
        if (stmt.explain && !stmt.analyze) {
          result.plan = "Insert " + stmt.insert.table;
          result.message = "explain";
          break;
        }
        result.rows_affected = InsertRows(stmt.insert);
        result.message = "inserted " + std::to_string(result.rows_affected);
        break;
      case Statement::Kind::Update:
        if (stmt.explain && !stmt.analyze) {
          result.plan = "Update " + stmt.update.table;
          result.message = "explain";
          break;
        }
        result.rows_affected = UpdateRows(stmt.update);
        result.message = "updated " + std::to_string(result.rows_affected);
        break;
      case Statement::Kind::Delete:
        if (stmt.explain && !stmt.analyze) {
          result.plan = "Delete " + stmt.del.table;
          result.message = "explain";
          break;
        }
        result.rows_affected = DeleteRows(stmt.del);
        result.message = "deleted " + std::to_string(result.rows_affected);
        break;
      case Statement::Kind::Select:
        result = RunSelect(stmt.select, stmt.explain, !stmt.explain || stmt.analyze);
        break;
      case Statement::Kind::Begin:
      case Statement::Kind::Commit:
      case Statement::Kind::Rollback:
        break;
    }
    if (auto_commit) {
      CommitTxn();
    }
    return result;
  } catch (...) {
    if (txn_.open) {
      AbortTxn();
    }
    if (explicit_txn_) {
      explicit_txn_ = false;
      if (txn_hold_.owns_lock()) {
        txn_hold_.unlock();
      }
    }
    throw;
  }
}

Schema Database::Aliased(const TableInfo& table) const {
  Schema schema = table.schema;
  for (auto& column : schema) {
    column.table = table.name;
  }
  return schema;
}

void Database::CreateTable(const CreateTableData& data) {
  if (data.name.empty() || catalog_.FindTable(data.name) != nullptr) {
    throw Error("table " + data.name + " already exists");
  }
  if (data.columns.empty()) {
    throw Error("table needs at least one column");
  }
  TableInfo table;
  table.id = catalog_.next_table_id++;
  table.name = data.name;
  for (const auto& column : data.columns) {
    if (column.name.empty()) {
      throw Error("column name is empty");
    }
    for (const auto& existing : table.schema) {
      if (existing.name == column.name) {
        throw Error("duplicate column " + column.name);
      }
    }
    Column stored;
    stored.name = column.name;
    stored.type = column.type;
    stored.varchar_len = column.varchar_len;
    stored.primary_key = column.primary_key;
    stored.not_null = column.not_null || column.primary_key;
    table.schema.push_back(std::move(stored));
  }
  std::vector<uint16_t> primary;
  for (uint16_t i = 0; i < table.schema.size(); ++i) {
    if (table.schema[i].primary_key) {
      primary.push_back(i);
    }
  }
  catalog_.tables.push_back(std::move(table));
  catalog_dirty_ = true;
  if (!primary.empty()) {
    CreateIndexData index;
    index.name = data.name + "_pk";
    index.table = data.name;
    index.unique = true;
    for (uint16_t column : primary) {
      index.columns.push_back(catalog_.tables.back().schema[column].name);
    }
    CreateIndex(index);
  }
}

void Database::CreateIndex(const CreateIndexData& data) {
  if (catalog_.FindIndex(data.name) != nullptr) {
    throw Error("index " + data.name + " already exists");
  }
  TableInfo* table = catalog_.FindTable(data.table);
  if (table == nullptr) {
    throw Error("no such table " + data.table);
  }
  IndexInfo index;
  index.id = catalog_.next_index_id++;
  index.name = data.name;
  index.table_id = table->id;
  index.unique = data.unique;
  for (const auto& name : data.columns) {
    bool found = false;
    for (uint16_t i = 0; i < table->schema.size(); ++i) {
      if (table->schema[i].name == name) {
        index.columns.push_back(i);
        found = true;
        break;
      }
    }
    if (!found) {
      throw Error("no such column " + name);
    }
  }
  if (index.columns.empty()) {
    throw Error("index needs at least one column");
  }
  index.root = BTree::Create(store_);
  catalog_.indexes.push_back(index);
  IndexInfo& stored = catalog_.indexes.back();
  const Schema schema = table->schema;
  const uint32_t table_id = table->id;
  HeapForEach(store_, table->first, [&](RID rid, std::string_view bytes) {
    const Tuple tuple = DeserializeTuple(schema, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    InsertIndexEntry(stored, schema, tuple, rid);
    return true;
  });
  (void)table_id;
  catalog_dirty_ = true;
}

bool Database::IndexContains(const IndexInfo& index, const std::string& user, RID skip) {
  PageId root = index.root;
  if (root == kInvalidPage) {
    return false;
  }
  BTree tree(store_, root);
  bool found = false;
  tree.Scan(&user, [&](const std::string& key, RID rid) {
    if (UserKeyOf(key) != user) {
      return false;
    }
    if (rid == skip) {
      return true;
    }
    found = true;
    return false;
  });
  return found;
}

void Database::InsertIndexEntry(IndexInfo& index, const Schema&, const Tuple& tuple, RID rid) {
  bool any_null = false;
  const std::string user = UserKey(index, tuple, any_null);
  if (index.unique && !any_null && IndexContains(index, user, RID{kInvalidPage, 0})) {
    throw Error("duplicate key on index " + index.name);
  }
  BTree tree(store_, index.root);
  tree.Insert(WithRid(user, rid), rid);
  catalog_dirty_ = true;
}

void Database::RemoveIndexEntry(IndexInfo& index, const Schema&, const Tuple& tuple, RID rid) {
  bool any_null = false;
  const std::string user = UserKey(index, tuple, any_null);
  BTree tree(store_, index.root);
  if (!tree.Remove(WithRid(user, rid))) {
    throw Error("missing index entry in " + index.name);
  }
  catalog_dirty_ = true;
}

uint64_t Database::InsertRows(const InsertData& data) {
  TableInfo* table = catalog_.FindTable(data.table);
  if (table == nullptr) {
    throw Error("no such table " + data.table);
  }
  if (data.rows.empty()) {
    throw Error("INSERT needs at least one row");
  }
  std::vector<int> targets;
  if (data.columns.empty()) {
    for (int i = 0; i < static_cast<int>(table->schema.size()); ++i) {
      targets.push_back(i);
    }
  } else {
    for (const auto& name : data.columns) {
      targets.push_back(ColumnIndex(table->schema, name));
    }
  }
  uint64_t inserted = 0;
  for (const auto& exprs : data.rows) {
    if (exprs.size() != targets.size()) {
      throw Error("INSERT column count does not match the value list");
    }
    Tuple tuple(table->schema.size());
    std::vector<bool> seen(table->schema.size(), false);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const int column = targets[i];
      if (seen[static_cast<std::size_t>(column)]) {
        throw Error("column " + table->schema[static_cast<std::size_t>(column)].name + " specified twice");
      }
      seen[static_cast<std::size_t>(column)] = true;
      const Value value = EvalScalar(exprs[i], {}, {});
      tuple[static_cast<std::size_t>(column)] = Coerce(value, table->schema[static_cast<std::size_t>(column)]);
    }
    for (std::size_t i = 0; i < table->schema.size(); ++i) {
      if (seen[i]) {
        continue;
      }
      if (table->schema[i].not_null || table->schema[i].primary_key) {
        throw Error("column " + table->schema[i].name + " cannot be null");
      }
      tuple[i] = Value::Null(table->schema[i].type);
    }
    const std::vector<uint8_t> bytes = SerializeTuple(table->schema, tuple);
    for (const auto& index : catalog_.indexes) {
      if (index.table_id != table->id || !index.unique) {
        continue;
      }
      bool any_null = false;
      const std::string user = UserKey(index, tuple, any_null);
      if (!any_null && IndexContains(index, user, RID{kInvalidPage, 0})) {
        throw Error("duplicate key on index " + index.name);
      }
    }
    const RID rid = HeapInsert(store_, table->first, table->last, bytes.data(),
                               static_cast<uint16_t>(bytes.size()));
    for (auto& index : catalog_.indexes) {
      if (index.table_id == table->id) {
        InsertIndexEntry(index, table->schema, tuple, rid);
      }
    }
    ++table->rows;
    ++inserted;
    catalog_dirty_ = true;
  }
  return inserted;
}

uint64_t Database::DeleteRows(const DeleteData& data) {
  TableInfo* table = catalog_.FindTable(data.table);
  if (table == nullptr) {
    throw Error("no such table " + data.table);
  }
  const Schema schema = Aliased(*table);
  std::shared_ptr<Expr> pred;
  if (data.where) {
    pred = std::make_shared<Expr>(*data.where);
  }
  struct Hit {
    RID rid;
    Tuple tuple;
  };
  std::vector<Hit> hits;
  const Schema stored = table->schema;
  HeapForEach(store_, table->first, [&](RID rid, std::string_view bytes) {
    Tuple tuple = DeserializeTuple(stored, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (pred && EvalPred(*pred, tuple, schema) != Truth::True) {
      return true;
    }
    hits.push_back(Hit{rid, std::move(tuple)});
    return true;
  });
  for (const auto& hit : hits) {
    for (auto& index : catalog_.indexes) {
      if (index.table_id == table->id) {
        RemoveIndexEntry(index, stored, hit.tuple, hit.rid);
      }
    }
    if (!HeapDelete(store_, hit.rid)) {
      throw Error("failed to delete a heap row");
    }
    if (table->rows > 0) {
      --table->rows;
    }
    catalog_dirty_ = true;
  }
  return hits.size();
}

uint64_t Database::UpdateRows(const UpdateData& data) {
  TableInfo* table = catalog_.FindTable(data.table);
  if (table == nullptr) {
    throw Error("no such table " + data.table);
  }
  if (data.sets.empty()) {
    throw Error("UPDATE needs at least one assignment");
  }
  const Schema schema = Aliased(*table);
  std::vector<int> targets;
  for (const auto& assign : data.sets) {
    targets.push_back(ColumnIndex(schema, assign.column));
  }
  std::shared_ptr<Expr> pred;
  if (data.where) {
    pred = std::make_shared<Expr>(*data.where);
  }
  struct Hit {
    RID rid;
    Tuple tuple;
  };
  std::vector<Hit> hits;
  const Schema stored = table->schema;
  HeapForEach(store_, table->first, [&](RID rid, std::string_view bytes) {
    Tuple tuple = DeserializeTuple(stored, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (pred && EvalPred(*pred, tuple, schema) != Truth::True) {
      return true;
    }
    hits.push_back(Hit{rid, std::move(tuple)});
    return true;
  });
  for (const auto& hit : hits) {
    Tuple updated = hit.tuple;
    for (std::size_t i = 0; i < data.sets.size(); ++i) {
      const Value value = EvalScalar(data.sets[i].value, hit.tuple, schema);
      const int column = targets[i];
      updated[static_cast<std::size_t>(column)] = Coerce(value, schema[static_cast<std::size_t>(column)]);
    }
    const std::vector<uint8_t> bytes = SerializeTuple(stored, updated);
    for (const auto& index : catalog_.indexes) {
      if (index.table_id != table->id || !index.unique) {
        continue;
      }
      bool any_null = false;
      const std::string user = UserKey(index, updated, any_null);
      if (!any_null && IndexContains(index, user, hit.rid)) {
        throw Error("duplicate key on index " + index.name);
      }
    }
    for (auto& index : catalog_.indexes) {
      if (index.table_id == table->id) {
        RemoveIndexEntry(index, stored, hit.tuple, hit.rid);
      }
    }
    const RID rid = HeapInsert(store_, table->first, table->last, bytes.data(),
                               static_cast<uint16_t>(bytes.size()));
    for (auto& index : catalog_.indexes) {
      if (index.table_id == table->id) {
        InsertIndexEntry(index, stored, updated, rid);
      }
    }
    if (!HeapDelete(store_, hit.rid)) {
      throw Error("failed to replace a heap row");
    }
    catalog_dirty_ = true;
  }
  return hits.size();
}

QueryResult Database::RunSelect(const SelectData& data, bool explain, bool execute) {
  auto plan = PlanQuery(catalog_, data);
  QueryResult result;
  result.columns = plan->names;
  if (explain) {
    result.plan = Explain(*plan);
  }
  if (!execute) {
    result.message = "explain";
    return result;
  }
  auto exec = MakeExecutor(store_, catalog_, *plan);
  exec->Open();
  Tuple row;
  while (exec->Next(row)) {
    result.rows.push_back(std::move(row));
  }
  exec->Close();
  result.rows_affected = result.rows.size();
  result.message = std::to_string(result.rows.size()) + (result.rows.size() == 1 ? " row" : " rows");
  return result;
}

}  // namespace keel
