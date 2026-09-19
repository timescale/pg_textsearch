# PR #472 Final Fix Report

## Status

Complete. The final-review truncation finding is confirmed and fixed on
`nonblocking-compaction`.

The implementation commit is:

- `7f2a92b8668edfeabbe0a0b099c5bba9e0f3fb95`
  (`Make force-merge truncation reclaim-safe`)

No changes were pushed.

## Root-cause evidence

Before this fix, `tp_truncate_dead_pages()` in `src/access/build.c`:

1. captured the current published segment graph;
2. walked the current live memtable chain;
3. included the attached deferred-free tombstone chain;
4. called `RelationTruncate(index, max_used)` for every block above the
   resulting high-water mark.

That proves only that a page is not reachable from the current graph. It does
not prove that the page completed reclaim.

Two protected page classes were omitted:

- DEAD memtable pages unlinked by spill but still protected by `dead_fxid`;
- unreachable structural/source pages left by a crash or error after graph
  publication and before deferred-tombstone attachment.

The reader and WAL traces confirm why those omissions are unsafe:

- `tp_segment_graph_snapshot_create()` releases the metapage and tail buffer
  locks after copying roots and a bounded memtable endpoint, then consumers
  read the copied generation.
- Spill deliberately leaves the old chain readable and stamps it
  `DEAD + dead_fxid`; `tp_reclaim_dead_memtable_pages()` waits for
  `GetOldestNonRemovableTransactionId`, emits
  `XLOG_BTREE_REUSE_PAGE`, writes `TP_FREE_PAGE_MAGIC`, and only then returns
  the page to the FSM.
- Parallel legacy VACUUM can publish a replacement graph before building and
  attaching source tombstones. A crash in that interval intentionally leaks
  unreachable source pages until `REINDEX`.
- `RelationTruncate()` has no equivalent standby conflict record. Truncating a
  DEAD or orphan page bypassed the reclaim protocol entirely.

The existing `src/index/freepage.c` contract supplies the correct proof:
`TP_FREE_PAGE_MAGIC` means the page was detached from every owner and completed
the appropriate reclaim path. Allocators already rely on exactly that stamp.

## RED regression

The regression was added to the existing `segment_reclaim` SQL test. It:

1. creates and drains a lower-block free-page pool;
2. leaves a one-page live memtable at the physical EOF;
3. invokes force merge, whose spill and compaction allocations reuse the lower
   free pages;
4. checks that the newly retired EOF memtable page, still carrying
   `DEAD + dead_fxid`, remains in the relation.

Old-code command:

```bash
/home/azureuser/.copilot/session-state/8bf506c4-245d-4e44-88e9-46c74737c1eb/files/pg18-benchmark/lib/pgxs/src/test/regress/pg_regress \
  --use-existing \
  --host=/tmp/pgts472-red-regress-sock \
  --port=55479 \
  --dbname=contrib_regression \
  --inputdir=test \
  --outputdir=test \
  segment_reclaim
```

Old-code result:

```text
not ok 1     - segment_reclaim
1 of 1 tests failed

force_merge_preserved_unreclaimed_suffix
f
(1 row)
```

The concrete old layout was:

```text
after_merge_blocks=315
chain=315
before_drain_blocks=316
after_drain_pending=0
after_drain_chain=315
after_drain_blocks=316
after_force_blocks=314
dead=
```

Force merge removed the unreclaimed DEAD block 315 and truncated one block
below it.

## Hypothesis

The root cause is that truncation used graph absence as a reclaim predicate.
Replacing that predicate with the existing recyclable-page stamp should retain
every DEAD, live, unknown, or orphan page while still allowing already
reclaimed EOF pages to be physically removed.

## Exact fix and invariants

`tp_truncate_dead_pages()` now:

1. reads the relation size while the caller holds the per-index
   `LW_EXCLUSIVE` lock;
2. scans backward from EOF;
3. checks each page with `tp_page_is_recyclable()`;
4. stops at the first page without `TP_FREE_PAGE_MAGIC`;
5. calls `RelationTruncate()` only for the contiguous stamped suffix.

The old segment-graph, live-memtable, and tombstone high-water walk was
removed, along with the now-unused `tp_tombstone_max_used_block()` helper.

Enforced invariants:

- Absence from the published graph is never reclaim proof.
- A DEAD memtable page is retained until horizon-safe reclaim emits conflict
  WAL and free-stamps it.
- Valid structural pages, unknown pages, and unreachable orphans stop
  truncation.
- Every truncated page was already proven recyclable.
- The exclusive per-index lock prevents a stamped EOF page from being claimed
  between inspection and truncation.
- Normal compaction still never truncates.
- A paired regression proves both sides: an unreclaimed suffix survives, and
  the same suffix is truncated after VACUUM completes reclaim.

The published-graph source guard now rejects a return to graph-derived
truncation and requires the recyclable EOF scan.

## Files

- `src/access/build.c`
  - replaces graph high-water truncation with recyclable EOF scanning.
- `src/access/am.h`
  - updates the truncation contract.
- `src/segment/tombstone.c`
  - removes the obsolete maximum-referenced-block walker and updates drain
    comments.
- `src/segment/tombstone.h`
  - removes the obsolete helper declaration.
- `test/sql/segment_reclaim.sql`
  - adds the protected-DEAD and reclaimed-free suffix behaviors.
- `test/expected/segment_reclaim.out`
  - records the new regression results.
- `test/scripts/published_graph_source.sh`
  - guards the free-stamp-only truncation implementation.
- `ARCHITECTURE.md`
  - documents that truncation is not an orphan-reclaim mechanism.
- `docs/nonblocking_compaction_design.md`
  - makes the EOF free-stamp invariant authoritative.

## GREEN validation

### Targeted SQL and source guards

```bash
pg_regress --use-existing --host=/tmp/pgts472-targeted-regress-sock \
  --port=55479 --dbname=contrib_regression \
  --inputdir=test --outputdir=test \
  force_merge segment_reclaim
```

Result:

```text
ok 1 - force_merge
ok 2 - segment_reclaim
All 2 tests passed.
```

```bash
./test/scripts/published_graph_source.sh
./test/scripts/compaction_ownercheck_source.sh
```

Result:

```text
Published graph source guards passed
Compaction ownership and lock ordering passed
```

### Targeted concurrency and recovery

All commands used the PostgreSQL 18.6 binaries from
`pg18-benchmark/bin`.

```bash
cd /tmp/pgts472-shell/test/scripts
./nonblocking_compaction.sh
```

Result: all deterministic nonblocking compaction cases passed.

```bash
cd /tmp/pgts472-shell/test/scripts
./vacuum_concurrent_merge.sh
```

Result: VACUUM/force-merge coordination and concurrent stress passed.

```bash
cd /tmp/pgts472-shell/test/scripts
./compaction_recovery.sh
```

Result: pre-publication, detached-WAL, post-publication, and standby write
guards passed.

### Clean build, format, and SQL regressions

```bash
make PG_CONFIG=.../pg18-benchmark/bin/pg_config clean
make PG_CONFIG=.../pg18-benchmark/bin/pg_config -j2
make PG_CONFIG=.../pg18-benchmark/bin/pg_config install
make PG_CONFIG=.../pg18-benchmark/bin/pg_config format-check
```

Result:

```text
clean_build=PASS
install=PASS
Code formatting check passed
regress_count=79
```

```bash
PGHOST=/tmp/pgts472-final-all-sock PGPORT=55479 \
PGDATABASE=contrib_regression \
make PG_CONFIG=.../pg18-benchmark/bin/pg_config test
```

Result:

```text
All 79 tests passed.
test/regression.diffs absent
```

The first full-regression attempt used `autovacuum=off`, which invalidated
catalog-stat expectations and changed several planner choices. A fresh cluster
with repository-default settings passed all 79 tests.

### Complete fatal shell suite

```bash
cd /tmp/pgts472-shell
PATH=.../pg18-benchmark/bin:$PATH \
make PG_CONFIG=.../pg18-benchmark/bin/pg_config test-shell
```

Result:

```text
All shell-based tests completed
```

This includes RLS locking, nonblocking compaction, lock fairness, parallel
VACUUM, concurrency, Boolean and partial-index concurrent merge, duplicate
read, VACUUM/merge, crash recovery, shutdown spill, compaction recovery,
segment, CIC, multi-index, and multi-backend REINDEX coverage.

### Standalone standby reclaim

```bash
cd /tmp/pgts472-shell/test/scripts
PATH=.../pg18-benchmark/bin:$PATH ./standby_reclaim.sh
```

Result:

```text
All standby reclaim overlap checks passed
```

The suite covered old-graph cursors, feedback-delayed reclaim, disconnected
reader conflict WAL, atomic segment and memtable snapshots, spill horizon
ordering, reused DEAD pages, and promotion.

### PostgreSQL 17 compatibility

```bash
make PG_CONFIG=/home/azureuser/pg17-rel/bin/pg_config clean
make PG_CONFIG=/home/azureuser/pg17-rel/bin/pg_config -j2
```

Result:

```text
PostgreSQL 17.9
postgresql_17_compile=PASS
```

A subsequent clean PostgreSQL 18 rebuild was required before the final SQL
run; reusing PG17 object files directly produced the expected server/library
version mismatch.

### Final hygiene gate

```bash
make PG_CONFIG=.../pg18-benchmark/bin/pg_config format-check
git diff --check
test ! -f test/regression.diffs
```

Result:

```text
Code formatting check passed
git diff check passed
test/regression.diffs absent
```

The final fresh verification gate also reran all 79 SQL regressions:

```text
# All 79 tests passed.
verification_gate=PASS (79/79 SQL, format, diff, no regression.diffs)
```

## Self-review

Reviewed the complete `932c33769debbdb9b2b7a790324bb25e575bec3e` through
`7f2a92b8668edfeabbe0a0b099c5bba9e0f3fb95` change range and the report
addition.

Checks performed:

- confirmed truncation no longer consults graph reachability;
- confirmed every inspected buffer is unlocked before truncation;
- confirmed the metapage can never be truncated;
- confirmed the caller still holds maintenance plus per-index exclusive
  locking around the truncation phase;
- confirmed tombstone drain still free-stamps under the same exclusive lock;
- confirmed the test fails on the original code for the intended reason;
- confirmed the paired test catches disabling truncation entirely;
- confirmed no stale `tp_tombstone_max_used_block()` references remain;
- confirmed docs, source guards, and comments express the same invariant;
- confirmed no unrelated source behavior was changed.

No additional defect was found in the final change range.

## Concerns

1. The safer algorithm intentionally retains unstamped parallel-build margin
   and crash/error orphans. Those pages are not proven recyclable and remain
   until `REINDEX`; this is the required safety tradeoff.
2. An extra, non-mandated run of
   `replication_memtable_dead_reclaim.sh` exposed a pre-existing stale
   expectation: it expects primary reclaim to leave DEAD page contents on the
   standby, but `tp_record_free_index_page()` is WAL-logged and the standby
   correctly observes the free stamp. The required `standby_reclaim.sh` suite
   passed. The stale script was left unchanged to avoid broadening this
   truncation-only fix wave.
