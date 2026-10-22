# minisql

A relational database engine written from scratch in C++20, with a B+tree storage engine, a buffer pool, a SQL planner, and write-ahead-log crash recovery. It also runs in the browser via WebAssembly.

## Features

- Disk pages, clock buffer pool, B+tree primary and secondary indexes
- SQL: CREATE TABLE/INDEX, INSERT, UPDATE, DELETE, SELECT with WHERE, JOIN, GROUP BY, aggregates, ORDER BY, LIMIT
- Transactions (BEGIN, COMMIT, ROLLBACK) and EXPLAIN / EXPLAIN ANALYZE
- Write-ahead log with crash recovery
- Browser SQL playground in `web/`

## Results

Release build vs SQLite 3.45.1, median of 5 runs (400 customers, 2,000 orders, 5,000 line items):

| Workload | minisql | SQLite |
| --- | ---: | ---: |
| Bulk load | 754.5 ms | 8.5 ms |
| COUNT/SUM over lineitem | 1.11 ms | 0.25 ms |
| 200 primary-key lookups | 16.6 ms | 0.61 ms |
| Secondary index lookup | 0.076 ms | 0.005 ms |
| Hash join + GROUP BY | 0.97 ms | 0.79 ms |

Crash recovery: 0 lost commits in 5 of 5 trials. Loading is slower because the log stores full page images; see docs/WALKTHROUGH.md.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/keel_cli /tmp/play.db
```

Benchmark (needs libsqlite3-dev): build `keel_bench` in Release and run it.

Playground:

```sh
cd web && npm ci && npm run dev
```

## Layout

```
include/keel, src   storage, indexes, log, parser, planner, executor
tests               GoogleTest and SQL logic scripts
bench               SQLite comparison
web                 SQL playground
docs/WALKTHROUGH.md design notes
```
