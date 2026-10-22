# Design notes

These are the decisions I would walk through in an interview. The measurements cited here are the ones in `bench/results.json` from the Release run described in the README.

## Page size

Pages are 4 KiB. That matches a common filesystem block and a common SSD page, so a buffer-pool write is one device block and a torn write shows up as a checksum failure instead of a half-updated row. The 32-byte header holds the page id, type, slot count, free-space bounds, a next-page pointer, an LSN, and a CRC32 taken with the checksum field zeroed. I rejected a larger page because the write-ahead log stores full images: an 8 KiB or 16 KiB page would have made the already-heavy load path worse without helping the 400-row customer table.

## Buffer pool

The pool pins a frame with a move-only guard. Eviction only considers pin count zero, which is what the pin test locks down. Both clock and LRU are implemented. Clock is the default. Under one mutex, clock is a sweep of reference bits and does not splice a linked list on every hit. LRU is still there so a test can name the victim: with a full pool the least recently used frame is the one that goes. Dirty eviction calls the log flusher with the page LSN before `pwrite`. That callback must not re-enter the pool, because the pool mutex is not recursive. `Discard` drops dirty frames without writing them and throws if anything is still pinned. That is the crash switch the recovery tests use.

## Heap files

Records sit in a slotted page. The slot directory grows down from the end of the page. A dead slot has length zero, and a live record never starts at offset zero because the header occupies the first 32 bytes, so a RID stays valid when a neighbor is deleted and the page is compacted. The heap is a chain of pages linked by the header's next pointer. Insert appends to the last page and allocates a new one when the record does not fit. I did not build a free-space map. For the row sizes in the benchmark a missing map only means the occasional new page, and the catalog already remembers the last page.

## Why the row id is part of the key

The B+ tree stores unique binary keys. User values are not unique, and a separator that appears on both sides of a split is a classic way to break the search invariant. I append a 6-byte big-endian RID to every index key. The separator copied up on a leaf split is then a key that exists in exactly one leaf, the right sibling. Deletes do not merge underfull leaves. Keys do not move, so an old separator still sends a search to a leaf that may legally hold it. The cost is wasted space after a heavy delete. Merging would need a second pass over parent separators and is the first thing I would add if the workload were update-heavy rather than the insert-then-query shape of the benchmark.

Encoding is order-preserving and prefix-free so `memcmp` is the comparator. Integers flip the sign bit and go out big-endian. Doubles use the IEEE total order, and NaN is rejected. Strings escape `0x00` and terminate with `0x00 0x00`, so a shorter string is not a byte prefix of a longer one. Null is a single `0x00` tag and sorts first.

## Indexes lose on wide ranges

A secondary index is unclustered. Each match is a random heap fetch. The planner charges 0.15 per fetched row and 1.0 per sequential heap page, against 0.5 per index page. On the 400-row customer table a primary-key equality is selective enough that the plan is an `IndexScan` on `customer_pk`. On 5,000 line items, `l_orderkey >= 0` matches the whole table. The plan is a `SeqScan`, and a cold pool recorded 46 misses for that scan. I would not claim the constants are calibrated to a disk; they are ordered so that a point lookup beats a scan and a range with selectivity around 0.3 does not. The executor still applies the predicate after the index, and it compares a bound with prefix equality rather than raw `memcmp`, because a full key is longer than the encoded prefix and would otherwise look strictly greater.

## Join order

Single-table access paths are costed first. Joins of up to eight tables are enumerated with dynamic programming over subsets. A disconnected intermediate subset is skipped so the search does not invent a cartesian product in the middle of a connected query. The final subset may be a cartesian product, and that becomes a nested loop. An equijoin is a hash join. The build side is the smaller input. In the benchmark, `customer` (400 rows) is the build and `orders` (2,000 rows) is the probe, which is what `EXPLAIN` prints as `HashJoin build=customer`. Every conjunct is bound again on the join output, including after the build side is swapped, so a residual predicate sees the column order the operator actually produces. I did not emit index nested-loop joins. The hash join was the right shape for the nation aggregation, and a nested-loop index join would mostly restate the point-lookup case.

Sort and aggregation are in memory. That is honest for the 5,000-row line item table and is the limit I would state out loud. `GROUP BY` is column-only. Aggregates have to be the top of the select item. `AVG` is always a double, `SUM` of integers stays an integer, and an aggregate over an empty input with no grouping column returns one row (`COUNT` is 0, `SUM` and `AVG` are null).

## Logging

The log is a separate file. Each record carries a checksum, and the LSN is the file offset. A torn tail fails the checksum or the size check and is truncated. Page updates log a full before-image and after-image. Redo copies the after-image when the on-disk page LSN is lower, and a checksum failure is treated as LSN 0 so a torn heap page is repaired. I measured the cost of that simplicity: loading 400 customers, 2,000 orders, and 5,000 line items took 754.465 ms in Keel and 8.51577 ms in SQLite. Physiological logging, a few bytes of changed fields plus the page id, is what I would switch to if load time were the requirement. Full images made redo a memcpy and made the torn-page test obvious.

Undo walks the loser newest-first and writes a compensation record whose image is the before-image, sealed with the compensation LSN. The log is flushed before those pages are installed, so an eviction in the middle of abort still has the compensation record durable. Only one transaction is active, so applying before-images in reverse LSN order is the right undo. A checkpoint is sharp: the pool is flushed, then a checkpoint record is appended and its LSN is stored in the log header. I do not take a fuzzy checkpoint. There is no concurrent writer to protect, and a sharp checkpoint keeps analysis to "the active transaction, if any, plus the records after this LSN."

Steal is allowed. A dirty page can leave the pool before commit, which is what the stolen-page test forces with `FlushDirty` and then `Abandon`. No-steal would have avoided undo, but the bulk load would have pinned every dirty page until commit and the 256-page pool would not have been a cache anymore. No-force is the other half: commit fsyncs the log and does not fsync the data file. The redo test commits, discards the pool, and still sees the rows.

The catalog blob is rewritten only at commit. Abort throws away the in-memory catalog and reloads the last committed one, while undo puts the page images back. Row counts and index roots therefore cannot commit without the rows they describe.

## Concurrency

SQL execution takes one recursive mutex for the whole explicit transaction, and for a single statement in auto-commit. The schedule is serial, which is serializable, and it let me test recovery and the planner without deadlock flakes. A lock manager is the natural next step. I would start with table-level locks and wait-die, using the transaction id as the priority, rather than row locks. Row locks need a deadlock detector or a timeout, and they interact with the buffer-pool latch. I would not pretend the current mutex is two-phase locking.

## What I would say if asked about the gap to SQLite

The query times on a warm cache are in the same order of magnitude for the hash join (0.96894 ms versus 0.786011 ms) and several times slower for a scan (1.11126 ms versus 0.253215 ms) and for 200 primary-key lookups (16.5534 ms versus 0.60799 ms). The load gap is the full-page log, not the B+ tree. Recovery of 500 committed rows lost 0 commits across 5 trials, with a mean reopen of 68.0475 ms. That is the property I optimized for.
