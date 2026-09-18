# Final Static-Review Blockers Report

## Status and commits

- Final implementation range:
  `95e03ef6cc9659c7ae0e076b564292e66ad2bd36..3e67bda0585799653c98223437670ac5cfc15abc`
- Initial implementation:
  - `c94c2fd3` — common standby generation and unlocked VACUUM reclaim
  - `e1042428` — initial verification report
- Independent-review fixes:
  - `149de820` — writer-compatible snapshots, memtable reuse conflict WAL,
    promotion pinning, deterministic tests, and corrected contracts
- Test-validity follow-up:
  - `0958a343` — condition-controlled tail-extension gate and reader-scoped
    recovery-conflict assertions
- Final review follow-up:
  - `659b4eae` — sample the DEAD-memtable reuse horizon after unpublication
    and add a condition-gated standby regression
- Legacy parallel VACUUM follow-up:
  - `9e2413d7` — design parallel legacy VACUUM publication
  - `6c4011f0` — support legacy segments in parallel VACUUM
  - `4ce2e55e` — check the compaction maintenance object lock
  - `3e67bda0` — update the split-publication source guard

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

### Test-validity follow-up

1. The original ABBA regression used a fixed three-second writer pause and did
   not prove the reader had reached the expected buffer-content wait before
   release.
2. The original memtable reuse assertion accepted a recovery-conflict string
   from the entire standby log, which already contained an earlier segment
   conflict.

The new assertion self-test rejects both stale unrelated conflict output and
output that combines the expected cancellation with an invalid-magic error.

### Final review blocker

1. **Premature DEAD-memtable reuse horizon**
   - Spill sampled `ReadNextFullTransactionId()` before the WAL record that
     removed the old chain from the metapage.
   - A transaction committed in that window, and a standby then captured the
     still-published old chain with `xmin=858` while its pages were stamped
     with `dead_fxid=857`.
   - Reuse conflict WAL at horizon 857 would not cancel that newer reader.

### Legacy parallel VACUUM follow-up

- RED: an affected V4 singleton under `VACUUM (PARALLEL 1)` raised
  `cannot vacuum legacy pg_textsearch segments during a parallel operation`.
- GREEN: the same command launched one worker, retained exactly 15,000 ranked
  rows, replaced the old root, and increased the deferred-free page count.
- The split protocol publishes the replacement graph first, samples
  `ReadNextFullTransactionId()` after unpublication WAL, then attaches the
  displaced pages under the still-held maintenance lock.

### Maintenance-lock assertion follow-up

- The SQL regression now checks the `pg_am` object lock at `objsubid = 3` in
  `ExclusiveLock`, rather than unrelated relation-lock columns and mode.

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

Spill now samples `dead_fxid` after `tp_spill_finalize()` inserts the
unpublication WAL. Every standby snapshot that can still discover the old
chain therefore has xmin at or below the reuse-conflict horizon.

### VACUUM reclaim

`tp_vacuumcleanup()` still retains maintenance through the O(index-pages)
DEAD-memtable scan and holds no per-index LWLock there. Maintenance excludes
force-merge truncation. Concurrent spill may make the reachable set
conservative; live/new pages are not DEAD, page locks serialize inspection,
and `dead_fxid` plus conflict WAL cover connected and disconnected readers.

### Deterministic test controls

The append hook now uses one advisory-lock gate GUC instead of a timed pause.
The test session holds the gate before starting the writer. After the writer's
old-tail EXCLUSIVE marker, the test starts a PID-identified reader and proves:

- the writer is waiting on the advisory gate;
- the reader is active on the exact ranked query and waiting on
  `LWLock/BufferContent`;
- both clients remain alive before release.

Only then does the test explicitly unlock the advisory gate and assert exact
results. The recovery-conflict validator reads only the redirected output of
the memtable reader, requires PostgreSQL's exact cancellation text, and rejects
invalid page/magic/index-corruption messages.

The spill-horizon regression similarly holds an advisory gate immediately
before `tp_spill_finalize()`, replays an intervening transaction, and proves a
standby reader captured the still-published chain before releasing publication.
The old ordering failed with `dead_fxid=857`, `xmin=858`; the fixed ordering
passes with `dead_fxid=858`, `xmin=858`.

## GREEN evidence

PostgreSQL 18 environment:

```text
PG_CONFIG=/home/azureuser/.copilot/session-state/8bf506c4-245d-4e44-88e9-46c74737c1eb/files/pg18-benchmark/bin/pg_config
```

- Format, build, install, shell syntax, diff checks, and all source guards:
  passed.
- Condition-gated append/snapshot lock-order case: the reader was observed on
  `LWLock/BufferContent` while the writer waited on the advisory gate; after
  explicit unlock, writer and exact ranked reader completed without deadlock.
- `standby_reclaim.sh`: passed all cases.
  - WAL contained 23 per-page memtable reuse conflict records.
  - The disconnected old-chain reader was canceled before 23 reclaimed pages
    were reused, with cancellation proven from that reader's output only.
  - Promotion race returned IDs 1..1500 exactly once.
  - Pre-publication old-chain reader xmin was covered by the DEAD horizon.
  - Existing ranked, standalone, segment-reclaim, and recovery-conflict cases
    remained green.
- The final clean rebuild, formatting check, 79/79 SQL regressions, complete
  shell suite, and empty `test/regression.diffs` all passed after the
  spill-horizon fix.
- `vacuum_concurrent_merge.sh`: three consecutive final full-scale runs
  passed after the gate change. Each included the condition-proven lock-order
  case, paused reclaim proof, maintenance serialization, and full stress
  phase. The former 120-second
  BufferContent-wait timeout did not reproduce in any of the three runs.
- Targeted ranked/standalone/Boolean/VACUUM SQL suite: 22/22 passed.
- `nonblocking_compaction.sh`: all deterministic cases passed.
- `compaction_recovery.sh`: all crash/recovery cases passed.
- Final `make test-local`: 79/79 passed; `test/regression.diffs` absent.

## Files

- Snapshot/append/promotion:
  `src/access/build.c`, `src/segment/graph_snapshot.c`,
  `src/memtable/chain_walker.{c,h}`, `src/memtable/log.{c,h}`,
  `src/scoring/bm25.c`, `src/mod.c`.
- Reuse conflict:
  `src/index/freepage.{c,h}`, `src/access/vacuum.c`,
  `src/segment/tombstone.c`.
- Tests/guards:
  `test/scripts/vacuum_concurrent_merge.sh`,
  `test/scripts/standby_reclaim.sh`,
  `test/scripts/standby_conflict_output.sh`,
  `test/scripts/graph_snapshot_source.sh`,
  `test/scripts/reclaim_conflict_source.sh`,
  `test/scripts/review_test_validity_source.sh`,
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
- One advisory gate replaces the superseded timer; no extra debug GUC was
  added.
- No unresolved correctness concerns. The conflict test intentionally pauses
  the reader so replay ordering is observable; its own output proves
  cancellation before the bounded walker can consume reused pages.
