# Final Static-Review Blockers Report

## Status and commits

- Base: `07166697900433b3e9b70099044706330ba10fe3`
- Initial implementation:
  - `c94c2fd3` — common standby generation and unlocked VACUUM reclaim
  - `e1042428` — initial verification report
- Independent-review fixes:
  - `149de820` — writer-compatible snapshots, memtable reuse conflict WAL,
    promotion pinning, deterministic tests, and corrected contracts
- All confirmed blockers are fixed and verified on PostgreSQL 18.6.

## RED evidence

Tests were added before each production fix.

### Initial blockers

- Standby ranked scoring replayed a spill after root snapshot unlock and
  returned `1000/1500` rows.
- VACUUM reclaim held `tapir_index_lock`; an exclusive spill and the later
  writer-preference-gated reader remained blocked during the deterministic
  reclaim pause.

### Independent-review blockers

1. **Metapage/tail ABBA**
   - The source guard rejected the old metapage -> tail nesting.
   - With only the one-shot append pause hook added, the writer held old-tail
     EXCLUSIVE before requesting metapage EXCLUSIVE. The overlapping ranked
     snapshot held metapage SHARE and waited for tail SHARE. The reader failed
     its ten-second completion assertion.
2. **Missing memtable reuse conflict**
   - The source guard found no shared conflict-WAL helper and no per-page
     `dead_fxid` conflict before free stamping.
   - With feedback disabled and replication disconnected, a bounded old-chain
     reader survived spill, horizon advance, VACUUM reclaim, and reclaimed
     page reuse; it was not canceled by conflict WAL.
3. **Promotion race**
   - After the conflict fix, ranked scoring captured a recovery snapshot,
     replayed spill, then was promoted during the post-snapshot pause.
     The late `RecoveryInProgress()` check selected the primary path and
     returned only `1000/1500` rows.

## Final ownership, locking, and WAL contracts

### Common read snapshot

The snapshot first reads and releases a candidate memtable head/tail. For a
nonempty chain it locks candidate tail SHARE, then metapage SHARE, validates
that head/tail are still current, and retries on mismatch. With tail then
metapage held it captures the tail free offset and every segment root.
Metapage is never held while acquiring tail, matching new-tail append's
tail -> new page -> metapage order and removing the ABBA cycle.

`tp_memtable_chain_snapshot_capture_locked()` consumes the already-locked tail
without acquiring another buffer lock. Empty head/tail is validated without a
tail lock. Ranked recovery mode is pinned before snapshot acquisition and used
through source selection. Standalone decides its branch before snapshot
creation; Boolean consumes the same bounded endpoint. Primary cache behavior
and standalone source-before-root admission remain unchanged.

### Reclaimed-page standby conflicts

`index/freepage.c` owns the shared `tp_log_page_reuse_conflict()` helper, which
emits stock `XLOG_BTREE_REUSE_PAGE` conflict-only WAL. Tombstone drain uses it
for displaced segment batches. DEAD-memtable reclaim captures each page's
`dead_fxid`, releases the page SHARE lock, emits conflict WAL, then writes the
recyclable free-page stamp and enters the page in the FSM. WAL ordering
therefore cancels disconnected/no-feedback standby snapshots before later WAL
can overwrite a retired chain page.

### VACUUM reclaim

`tp_vacuumcleanup()` still retains maintenance through the O(index-pages)
DEAD-memtable scan and holds no per-index LWLock there. Maintenance excludes
force-merge truncation. Concurrent spill may make the reachable set
conservative; live/new pages are not DEAD, page locks serialize inspection,
and `dead_fxid` plus conflict WAL cover connected and disconnected readers.

## GREEN evidence

PostgreSQL 18 environment:

```text
PG_CONFIG=/home/azureuser/.copilot/session-state/8bf506c4-245d-4e44-88e9-46c74737c1eb/files/pg18-benchmark/bin/pg_config
```

- Format, build, install, shell syntax, diff checks, and all source guards:
  passed.
- New append/snapshot lock-order case: writer and exact ranked reader
  completed without deadlock.
- `standby_reclaim.sh`: passed all cases.
  - WAL contained 23 per-page memtable reuse conflict records.
  - The disconnected old-chain reader was canceled before 23 reclaimed pages
    were reused.
  - Promotion race returned IDs 1..1500 exactly once.
  - Existing ranked, standalone, segment-reclaim, and recovery-conflict cases
    remained green.
- `vacuum_concurrent_merge.sh`: three consecutive final full-scale runs
  passed. Each included the lock-order case, paused reclaim proof, maintenance
  serialization, and full stress phase. The former 120-second
  BufferContent-wait timeout did not reproduce in any of the three runs.
- Targeted ranked/standalone/Boolean/VACUUM SQL suite: 22/22 passed.
- `nonblocking_compaction.sh`: all deterministic cases passed.
- `compaction_recovery.sh`: all crash/recovery cases passed.
- Final `make test-local`: 79/79 passed; `test/regression.diffs` absent.

## Files

- Snapshot/append/promotion:
  `src/segment/graph_snapshot.c`, `src/memtable/chain_walker.{c,h}`,
  `src/memtable/log.{c,h}`, `src/scoring/bm25.c`, `src/mod.c`.
- Reuse conflict:
  `src/index/freepage.{c,h}`, `src/access/vacuum.c`,
  `src/segment/tombstone.c`.
- Tests/guards:
  `test/scripts/vacuum_concurrent_merge.sh`,
  `test/scripts/standby_reclaim.sh`,
  `test/scripts/graph_snapshot_source.sh`,
  `test/scripts/reclaim_conflict_source.sh`,
  `test/scripts/compaction_ownercheck_source.sh`, `Makefile`.
- Contracts:
  `ARCHITECTURE.md`, `docs/nonblocking_compaction_design.md`, `CLAUDE.md`.

## Simplification and concerns

- One common snapshot owns segment roots and the bounded memtable endpoint.
- One locked-tail capture helper performs endpoint validation without lock
  reacquisition.
- Segment-root enumeration remains in one helper; retry/lock-order logic stays
  isolated in snapshot creation.
- One shared conflict-WAL helper serves segment tombstones and memtable pages.
- No unresolved correctness concerns. The conflict test intentionally pauses
  the reader so replay ordering is observable; cancellation occurs before the
  bounded walker can consume reused pages.
