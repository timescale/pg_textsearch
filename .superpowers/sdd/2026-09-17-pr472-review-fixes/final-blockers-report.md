# Final Static-Review Blockers Report

## Status and commits

- Base: `07166697900433b3e9b70099044706330ba10fe3`
- Implementation: `c94c2fd3` (`Complete standby snapshots and VACUUM reclaim locking`)
- Both final blockers are implemented and verified on PostgreSQL 18.6.

## RED evidence

Tests were added before the corresponding fixes.

1. Atomic standby generation:
   - `graph_snapshot_source.sh` failed because the common snapshot did not
     capture a memtable endpoint and recovery scoring had no bounded source.
   - `boolean_lock_source.sh` failed because Boolean execution recaptured the
     endpoint separately.
   - `standby_reclaim.sh` deterministically paused ranked scoring after the
     segment snapshot unlock, replayed spill WAL, and returned only
     `1000/1500` rows. This reproduced omission of the spilled memtable
     generation on `07166697`.
2. VACUUM full-fork reclaim:
   - `vacuum_reclaim_source.sh` failed because cleanup released maintenance
     before reclaim and acquired the per-index shared lock around the scan.
   - With only the reclaim pause hook added, `vacuum_concurrent_merge.sh`
     paused cleanup in the fork scan. The exclusive spill remained on
     `tapir_index_lock`, the later reader remained queued by writer
     preference, and the spill failed the five-second completion assertion.

## Implemented ownership and locking

### Standby read generation

`TpSegmentGraphSnapshot` is now the common read snapshot. While the metapage
buffer remains `BUFFER_LOCK_SHARE`, it copies every segment root and captures
`TpMemtableChainSnapshot` from the copied head/tail, including the tail free
offset. Generic WAL replay therefore cannot publish a spill between those two
components.

Recovery ranked and standalone scoring use
`tp_memtable_chain_source_create_bounded()`. It shares the existing chain
source constructor and ingestion code, consumes only the supplied endpoint,
does not reread the metapage, and does not rely on the extension LWLock.
Primary ranked scoring retains the cache chooser. Primary standalone scoring
retains source-before-root admission and cache semantics. Boolean execution
uses the common snapshot endpoint instead of recapturing it.

### VACUUM reclaim

`tp_vacuumcleanup()` retains the per-index maintenance object lock through
`tp_reclaim_dead_memtable_pages()` and holds no per-index LWLock during that
O(index-pages) work. Maintenance excludes force-merge/compaction truncation.
A racing spill can only make the reachable-chain set conservative; live/new
pages are not DEAD, page buffer locks serialize inspection, and `dead_fxid`
prevents reuse while an old primary or feedback-protected standby snapshot
can reference a retired chain.

## GREEN evidence

PostgreSQL 18 environment:

```text
PG_CONFIG=/home/azureuser/.copilot/session-state/8bf506c4-245d-4e44-88e9-46c74737c1eb/files/pg18-benchmark/bin/pg_config
```

- Format, build, install, and all source guards: passed.
- Targeted ranked/standalone/Boolean/VACUUM SQL suite: 22/22 passed.
- `standby_reclaim.sh`: passed; ranked race returned IDs 1..1500 exactly
  once, standalone race preserved exact score `-1.28048980`, and existing
  reclaim/recovery-conflict cases passed.
- `vacuum_concurrent_merge.sh`: passed at full scale; spill and later ranked
  reader completed while reclaim remained paused, force merge was blocked by
  maintenance, and the stress phase passed.
- `nonblocking_compaction.sh`: all deterministic cases passed.
- `compaction_recovery.sh`: all crash/recovery and standby rejection cases
  passed.
- Final `make test-local`: 79/79 passed; `test/regression.diffs` absent.

## Files

- Snapshot/source/scoring:
  `src/segment/graph_snapshot.{c,h}`,
  `src/memtable/chain_source.{c,h}`, `src/scoring/bm25.c`,
  `src/types/query.c`, `src/access/boolean.c`, `src/mod.c`.
- Reclaim/locking: `src/access/vacuum.c`, `src/access/am.h`, `src/mod.c`.
- Tests/guards: `test/scripts/standby_reclaim.sh`,
  `test/scripts/vacuum_concurrent_merge.sh`,
  `test/scripts/vacuum_reclaim_source.sh`,
  `test/scripts/graph_snapshot_source.sh`,
  `test/scripts/boolean_lock_source.sh`, `Makefile`.
- Contracts: `ARCHITECTURE.md`, `docs/nonblocking_compaction_design.md`,
  `CLAUDE.md`.

## Simplification pass and concerns

- One common snapshot owns roots plus the bounded memtable endpoint.
- One shared chain-source constructor/ingestion path serves ordinary and
  bounded sources.
- Superseded Boolean endpoint capture and standalone duplicate acquisition
  ordering were removed.
- Concern: the first full-scale VACUUM stress run reached its 120-second
  harness timeout while backends were making progress under buffer-content
  contention; no storage error or crash occurred. Investigation runs at
  0.1x, 0.5x, and two subsequent full-scale runs all passed, including the
  final recorded run.
