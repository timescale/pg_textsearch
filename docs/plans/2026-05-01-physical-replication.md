# Physical Replication Implementation Plan (revised 2026-05-01, v3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Lifecycle:** This plan is committed as part of PR 1 to anchor the design in repo history while the work is in progress. PR 3's final task **deletes this file**. Do not leave it in the tree after #345 is closed.

**Tracks:** [#345](https://github.com/timescale/pg_textsearch/issues/345) — long-lived standby backend staleness.

## Design pivot summary

Earlier draft logged `INSERT_TID(oid, ctid)` per insert, queued the TID into a bounded shared-memory pending list on the standby, and re-tokenized from the heap on the standby's read path. That works but pays the tokenization CPU cost twice — once on the primary's `aminsert`, once on the standby's `tp_beginscan`. For the AlloyDB-shape workload that motivated #345 (writes routed to the primary, reads to the standby), CPU on the standby is exactly what we're trying to save.

This revision instead emits **`INSERT_TERMS(oid, ctid, terms)`** — the full tokenized output. The standby's redo applies the terms directly to the memtable; no re-tokenization, no pending list, no drain on read path, no stale-flag fallback.

The pivot also enables a major simplification: **the docid-page chain becomes vestigial and is removed.** Today's docid pages exist solely for crash recovery (`tp_rebuild_posting_lists_from_docids` rebuilds the memtable from them on cold start). Once WAL replay populates the memtable directly, the docid chain has no remaining consumer.

To make per-insert WAL volume tolerable at scale, we tighten the `bm25vector` wire format up front. The `int32 frequency / int32 lexeme_len + MAXALIGN padding` per entry costs ~16B for a 6-char lexeme today; v2 brings it to ~9B. That gain applies everywhere `bm25vector` is materialized — not just in WAL — and pg_textsearch is at v1.2.0-dev, so the type-format break is at its lowest cost point.

---

## PR roadmap

The work is split into three sequential PRs. Each is independently reviewable, each ships green, each has a clear scope.

| | Title | Closes | LOC est. | Why this slice |
|---|---|---|---|---|
| **PR 1** | Compact `bm25vector` v2 format | — (prep) | ~400 | Type-format change with v1 read-compat. No replication semantics. Lands the smaller-footprint vector format that PR 2's WAL records will use, so we don't churn the WAL format twice. |
| **PR 2** | WAL-replicate memtable mutations | #345 | ~700 | The actual fix. Custom rmgr with `INSERT_TERMS` + `SPILL`. Docid pages still being written on the primary alongside (redundant but safe); PR 3 removes them. |
| **PR 3** | Remove docid-page machinery | — (cleanup) | -800 net | Cleanup. Drop `tp_add_docid_to_pages`, `tp_clear_docid_pages`, `tp_rebuild_posting_lists_from_docids`. Bump metapage version. In-place upgrade for v1 metapages. Removes ~2× redundant WAL volume on insert path that PR 2 introduces. |

**Why three PRs and not one:**
- PR 1 is a prerequisite optimization that touches type-system code; mixing it with replication semantics would muddy review.
- PR 2 is the load-bearing semantic change. It needs to land cleanly with green CI before any cleanup happens; if there's a problem with the rmgr design, we want to find it before deleting the docid pages that are the safety net.
- PR 3 is a code-reduction cleanup that's safe to delay arbitrarily. If PR 2 reveals issues, PR 3 stays as a follow-up; PR 2 is correct on its own.

---

# PR 1: Compact `bm25vector` v2 format

**Goal:** reduce the `bm25vector` per-entry size from ~16B to ~9B for typical lexemes by tightening the entry header (`uint16 freq + uint8 lex_len` instead of `int32 + int32 + MAXALIGN padding`). Maintain backward compatibility for v1 binary input — readers accept both, writers always emit v2.

**Files:**
- Modify: `src/types/vector.h`
- Modify: `src/types/vector.c`
- Modify: `src/access/scan.c`, `src/access/build.c`, `src/memtable/scan.c` — internal entry walkers
- Add: `test/sql/vector_format_compat.sql` + expected output

**Closes nothing externally.** This is preparatory work for PR 2.

## Task 1.1: Define v2 layout

- [ ] **Step 1: Add version field + new entry struct in `src/types/vector.h`**

```c
#define TPVECTOR_VERSION_V2 2

typedef struct TpVector {
    int32 vl_len_;             /* varlena header (must be first) */
    uint8 version;             /* 2 for v2; v1 has no such field */
    uint8 reserved[3];         /* zero; future flags / format bits */
    int32 index_name_len;
    int32 entry_count;
    char  data[FLEXIBLE_ARRAY_MEMBER];
} TpVector;

typedef struct TpVectorEntry {
    uint16 frequency;
    uint8  lexeme_len;
    char   lexeme[FLEXIBLE_ARRAY_MEMBER];
} TpVectorEntry;

#define TPVECTOR_ENTRY_SIZE(lex_len) (3 + (lex_len))
#define TPVECTOR_NEXT_ENTRY(e) \
    ((TpVectorEntry *)((char *)(e) + TPVECTOR_ENTRY_SIZE((e)->lexeme_len)))
```

Adjust `TPVECTOR_INDEX_NAME_PTR` and `TPVECTOR_ENTRIES_PTR` for the new offsets.

- [ ] **Step 2: Keep v1 struct definitions visible for reader fallback**

```c
/* Legacy v1 layout — accepted by readers, never written. */
typedef struct TpVectorEntry_v1 {
    int32 frequency;
    int32 lexeme_len;
    char  lexeme[FLEXIBLE_ARRAY_MEMBER];
} TpVectorEntry_v1;
```

## Task 1.2: Read v1 + v2; write v2

- [ ] **Step 1: Detect format in `tpvector_recv`**

The first 4 bytes after the varlena header are `{version, reserved[3]}` in v2 (so `0x02, 0x00, 0x00, 0x00`) and `int32 index_name_len` in v1. v1's `index_name_len` would have to be exactly `0x02` (and zero upper bytes) to alias v2's marker, which is unrealistic for valid index names. Sniff the first byte after the varlena header; dispatch to `tpvector_parse_v1` or `tpvector_parse_v2`.

If we want a paranoid disambiguator, use a magic-number version marker like `{0xBM, 0x25, 0x00, 0x02}` ("BM25 v2") instead of plain `0x02`. Implementer's call; the simple version is fine for an extension that never wrote anything resembling an `index_name_len = 2`.

- [ ] **Step 2: Always emit v2 in `tpvector_send`, `tpvector_out`, `create_tpvector_from_strings`**

Text output (`tpvector_out`) is unchanged — the canonical text representation is `index:{lex:freq,...}` regardless of version. Only the binary serialization changes.

## Task 1.3: Update internal entry walkers

`get_tpvector_first_entry` and `get_tpvector_next_entry` are the two entry-walking primitives used by `src/access/scan.c:296`, `src/access/build.c:1428`, `src/access/build.c:1472`, `src/memtable/scan.c:33`.

- [ ] **Step 1: Thread `TpVector *` through `get_tpvector_next_entry`**

Currently the API is `get_tpvector_next_entry(TpVectorEntry *current)`. Change to `get_tpvector_next_entry(TpVector *vec, TpVectorEntry *current)` so the walker can dispatch on `vec->version`. Update all four call sites.

```c
TpVectorEntry *
get_tpvector_next_entry(TpVector *vec, TpVectorEntry *current)
{
    if (vec->version == TPVECTOR_VERSION_V2)
        return TPVECTOR_NEXT_ENTRY(current);
    /* v1 path */
    TpVectorEntry_v1 *cur_v1 = (TpVectorEntry_v1 *)current;
    return (TpVectorEntry *)((char *)cur_v1 + MAXALIGN(
            sizeof(TpVectorEntry_v1) + cur_v1->lexeme_len));
}
```

- [ ] **Step 2: Internal callers extract `frequency` / `lexeme_len` via type-aware accessors**

Where call sites today read `entry->frequency` directly, introduce `tpvector_entry_freq(vec, entry)` and `tpvector_entry_lex_len(vec, entry)` that dispatch on version.

Alternative: have `get_tpvector_first_entry` always return a uniform "decoded" view (a small struct with `uint32 freq, uint32 lex_len, const char *lex`). Slightly more allocation churn but simpler call sites. Implementer's choice based on hot-path profiling — start with the macro approach.

## Task 1.4: Tests + commit

- [ ] **Step 1: Add `test/sql/vector_format_compat.sql`**

```sql
-- Round-trip: insert text, build index, query (the path that
-- materializes a bm25vector internally). All values written are
-- v2 by construction.
CREATE TABLE compat_test (id serial, content text);
INSERT INTO compat_test (content)
    SELECT 'document ' || g || ' alpha bravo'
    FROM generate_series(1,100) g;
CREATE INDEX compat_idx ON compat_test USING bm25(content)
    WITH (text_config='english');

SELECT count(*) FROM (
    SELECT id FROM compat_test
    ORDER BY content <@> to_bm25query('alpha', 'compat_idx')
    LIMIT 100
) t;
-- Expected: 100
```

If the project has any committed v1-format binary fixtures (it likely doesn't — bm25vector is transient), add a test that loads one and asserts equality with its v2 equivalent. Skip if there are no such fixtures.

- [ ] **Step 2: Build, regression**

```bash
make clean && make && make install
~/pgz/target/release/pgz restart pg17
make installcheck
```

Expected: all tests pass. No expected-output regeneration (text format unchanged).

- [ ] **Step 3: Commit**

```bash
git add src/types/vector.{c,h} src/access/scan.c src/access/build.c \
        src/memtable/scan.c test/sql/vector_format_compat.sql \
        test/expected/vector_format_compat.out
git commit -m "feat: compact bm25vector v2 wire format"
```

- [ ] **Step 4: Open PR**

Title: `feat: compact bm25vector v2 wire format`. Body: explain the per-entry savings, the v1-read / v2-write compat strategy, that this is preparatory work for #345, and that the included `docs/plans/2026-05-01-physical-replication.md` design doc will be removed by PR 3 once the series lands.

---

# PR 2: WAL-replicate memtable mutations (closes #345)

**Goal:** Fix #345 by adding a custom WAL resource manager that emits `INSERT_TERMS` per insert and `SPILL` per memtable spill, and applies them directly to the in-shared-memory memtable on redo. Standby backends — including long-lived ones — see the memtable populated by the startup process during streaming replay. No re-tokenization on the standby, no pending list, no drain on read path.

**Files:**
- Create: `src/replication/replication.h`
- Create: `src/replication/rmgr.c`
- Create: `src/replication/apply.c`, `apply.h` — redo helpers that mutate shared memtable state.
- Modify: `src/access/build.c` — `tp_insert` emits `INSERT_TERMS`; `tp_do_spill` emits `SPILL`.
- Modify: `src/mod.c` — register rmgr.
- Modify: `Makefile`.

**Note: docid pages still get written on insert in this PR.** `tp_add_docid_to_pages` stays in place; it's redundant with the new `INSERT_TERMS` records but harmless. PR 3 removes it. The transient WAL bloat is acceptable for a single PR cycle.

## Task 2.1: Baseline existing failing tests

- [ ] **Step 1: Run the suite, record per-script outcomes**

```bash
make test-replication-extended 2>&1 | tee /tmp/repl_baseline_pre_345.log
```

Expected (post-#343, pre-#345 fix): 18 PASS, 11 FAIL, 1 SKIP across 30 tests in 9 scripts.

- [ ] **Step 2: Inventory the FAILs**

Failures map to:
- `replication.sh` test 5
- `replication_correctness.sh` A2-A5, E1-E5
- `replication_concurrency.sh` B2
- `replication_cascading.sh` C2
- `replication_compat.sh` F2
- `replication_pitr.sh` PITR-1 (out of scope — corpus-stats bug, separate plan)

After PR 2 lands, the first 10 should flip to PASS. PITR-1 will still FAIL.

## Task 2.2: Custom rmgr scaffolding

- [ ] **Step 1: Create `src/replication/replication.h`**

```c
#ifndef PG_TEXTSEARCH_REPLICATION_H
#define PG_TEXTSEARCH_REPLICATION_H

#include "postgres.h"
#include "access/xlogreader.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

#include "../types/vector.h"

#define TP_RMGR_ID 145
#define TP_RMGR_NAME "pg_textsearch"

#define XLOG_TP_INSERT_TERMS 0x10
#define XLOG_TP_SPILL        0x20

typedef struct xl_tp_insert_terms {
    Oid             index_oid;
    ItemPointerData ctid;
    uint32          vector_size;  /* bytes of v2 bm25vector following */
} xl_tp_insert_terms;

typedef struct xl_tp_spill {
    Oid index_oid;
} xl_tp_spill;

extern void tp_register_rmgr(void);
extern void tp_rmgr_redo(XLogReaderState *record);
extern void tp_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *tp_rmgr_identify(uint8 info);

extern XLogRecPtr tp_xlog_insert_terms(
        Oid index_oid, ItemPointer ctid, const TpVector *vec);
extern XLogRecPtr tp_xlog_spill(Oid index_oid);

#endif
```

- [ ] **Step 2: Create `src/replication/rmgr.c` with stub callbacks**

Standard rmgr boilerplate: `RegisterCustomRmgrId`, `tp_rmgr_redo` switch on info type, `tp_rmgr_desc` for `pg_waldump`, `tp_rmgr_identify`. Stub the actual record handling — Tasks 2.3 and 2.4 fill them in.

- [ ] **Step 3: Wire registration in `_PG_init`**

```c
/* Near the end of _PG_init, after GUCs */
tp_register_rmgr();
```

Add `#include "replication/replication.h"` at top of `src/mod.c`.

- [ ] **Step 4: Add to Makefile, build, smoke test**

```
    src/replication/rmgr.o \
    src/replication/apply.o \
```

```bash
make clean && make && make install
~/pgz/target/release/pgz restart pg17
psql -p 5417 -d postgres -c "DROP EXTENSION IF EXISTS pg_textsearch; CREATE EXTENSION pg_textsearch;"
make installcheck
```

Expected: clean, no records emitted yet (no behavior change).

- [ ] **Step 5: Commit (intermediate)**

```bash
git add src/replication/{replication.h,rmgr.c} src/mod.c Makefile
git commit -m "feat: register custom WAL rmgr for pg_textsearch (no records yet)"
```

## Task 2.3: `INSERT_TERMS` emission and redo

- [ ] **Step 1: `tp_xlog_insert_terms` emits the record**

```c
XLogRecPtr
tp_xlog_insert_terms(Oid index_oid, ItemPointer ctid, const TpVector *vec)
{
    xl_tp_insert_terms hdr;
    Size               vec_size = VARSIZE(vec);

    hdr.index_oid   = index_oid;
    hdr.ctid        = *ctid;
    hdr.vector_size = (uint32)vec_size;

    XLogBeginInsert();
    XLogRegisterData((char *)&hdr, sizeof(hdr));
    XLogRegisterData((char *)vec, vec_size);
    return XLogInsert(TP_RMGR_ID, XLOG_TP_INSERT_TERMS);
}
```

The vector is v2 by construction (PR 1). No additional framing.

- [ ] **Step 2: Redo applies directly to the memtable**

In `src/replication/apply.c`, define `tp_redo_apply_insert(Oid, ItemPointer, TpVector *)`:

1. Look up the per-index shared registry entry (`tp_registry_lookup(oid)`).
2. If the entry doesn't exist yet: lazily create it from inside the startup process. The startup process can do DSA allocation; the catalog isn't accessed (the index_oid is enough — we don't need the index relation's metapage for this, just the per-index DSA area).
3. Acquire the memtable's write lock.
4. For each `(lexeme, freq)` entry in the vector, update the memtable's posting list and document-length tracker.
5. Release lock.

The redo helper reuses the same primitives that `tp_insert_core` uses on the primary — extract them into a shared apply path so both code paths call into the same memtable-mutation function.

In `tp_rmgr_redo`:

```c
case XLOG_TP_INSERT_TERMS:
{
    char               *raw = XLogRecGetData(record);
    xl_tp_insert_terms *hdr = (xl_tp_insert_terms *)raw;
    TpVector           *vec = (TpVector *)(raw + sizeof(*hdr));
    tp_redo_apply_insert(hdr->index_oid, &hdr->ctid, vec);
    break;
}
```

- [ ] **Step 3: Convert `tp_insert` to emit `INSERT_TERMS`**

In `src/access/build.c:tp_insert`, after tokenizing into `TpVector`, add the WAL emission inside a critical section. **Keep `tp_add_docid_to_pages` for now** — PR 3 removes it.

```c
START_CRIT_SECTION();
tp_add_docid_to_pages(index, ht_ctid);   /* legacy; removed in PR 3 */
tp_xlog_insert_terms(RelationGetRelid(index), ht_ctid, tpvec);
tp_memtable_apply_insert(...);           /* same primitive redo uses */
END_CRIT_SECTION();
```

- [ ] **Step 4: Build, smoke test**

```bash
make clean && make && make install
~/pgz/target/release/pgz restart pg17
make installcheck
```

Expected: all primary regression tests pass. New WAL records are emitted but the existing docid-page WAL is still emitted alongside (extra WAL volume; cleaned up in PR 3).

- [ ] **Step 5: Verify records appear in WAL**

```bash
psql -p 5417 -d postgres -c "
    DROP TABLE IF EXISTS wal_test;
    CREATE TABLE wal_test (id serial, content text);
    CREATE INDEX wal_test_idx ON wal_test USING bm25(content)
        WITH (text_config='english');
    INSERT INTO wal_test (content) VALUES ('hello world replication');
"
pg_waldump -p ~/pg17/data -r pg_textsearch -n 5
```

Expected: `INSERT_TERMS index_oid <oid> tid (...) vec_size <N>` plus the existing GenericXLog records for the docid page.

- [ ] **Step 6: Commit (intermediate)**

```bash
git add src/replication/{rmgr.c,apply.c,apply.h} src/access/build.c
git commit -m "feat: emit INSERT_TERMS WAL records and apply directly in redo"
```

## Task 2.4: `SPILL` emission and redo

- [ ] **Step 1: `tp_xlog_spill` emits the record**

```c
XLogRecPtr
tp_xlog_spill(Oid index_oid)
{
    xl_tp_spill hdr = {.index_oid = index_oid};
    XLogBeginInsert();
    XLogRegisterData((char *)&hdr, sizeof(hdr));
    return XLogInsert(TP_RMGR_ID, XLOG_TP_SPILL);
}
```

- [ ] **Step 2: Redo clears the memtable**

`tp_redo_clear_memtable(Oid)` resets the per-index memtable's hash tables to `DSHASH_HANDLE_INVALID`, mirroring `tp_clear_memtable` but operating on shared state alone (no `TpLocalIndexState`).

- [ ] **Step 3: Emit SPILL after `tp_do_spill`**

In `src/access/build.c:tp_do_spill` (line 129), after all segment-write WAL has been issued and the metapage is updated, add:

```c
tp_xlog_spill(RelationGetRelid(index_rel));
```

Ordering matters: SPILL must come AFTER the GenericXLog records that PR #343 added for segment data. Standby redo then sees: segment populated → metapage points at it → memtable cleared. Post-replay state matches the primary.

- [ ] **Step 4: Build, full replication suite, baseline diff**

```bash
make clean && make && make install
~/pgz/target/release/pgz restart pg17
make installcheck
make test-replication-extended 2>&1 | tee /tmp/repl_baseline_post.log

# Confirm the right tests flipped
diff <(grep -E "PASSED|FAIL" /tmp/repl_baseline_pre_345.log | sort) \
     <(grep -E "PASSED|FAIL" /tmp/repl_baseline_post.log | sort)
```

Expected: 10 of the 11 #345-class FAILs are now PASS. PITR-1 unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/replication/{rmgr.c,apply.c} src/access/build.c
git commit -m "feat: emit SPILL WAL record and clear memtable in redo"
```

## Task 2.5: Documentation, PR

- [ ] **Step 1: Update `CLAUDE.md`**

Add under "Important Notes for Development":

```markdown
- **Physical replication**: Memtable mutations are WAL-logged via a
  custom rmgr (ID 145, records `INSERT_TERMS` and `SPILL`). On a
  standby — and during primary crash recovery — redo applies the
  records directly to the in-shared-memory memtable. Closes #345.
```

Note in the `bm25vector` data-types section that the binary representation is now v2 (already done in PR 1, but re-mention since this PR is the user-visible "fix").

- [ ] **Step 2: README updates if applicable**

```bash
grep -n -i "replication" README.md
```

If a Limitations section exists, note that physical replication is now supported.

- [ ] **Step 3: Commit docs, open PR**

```bash
git add CLAUDE.md README.md
git commit -m "docs: physical replication via custom rmgr (#345)"
```

PR title: `fix: WAL-replicate memtable mutations (closes #345)`. Body: link #345, explain the rmgr architecture (`INSERT_TERMS` carries v2 bm25vector, `SPILL` clears memtable), attach baseline-vs-post test diff, explicitly call out that PITR-1 remains failing (separate issue) and that PR 3 will remove the now-redundant docid-page writes.

---

# PR 3: Remove docid-page machinery

**Goal:** Drop `tp_add_docid_to_pages`, `tp_clear_docid_pages`, `tp_rebuild_posting_lists_from_docids`, and the `first_docid_page` field on the metapage. Reduces per-insert WAL volume by removing the redundant `GenericXLog` docid-page records.

**Why this isn't just "WAL is now durable enough":** Postgres only retains WAL since the last checkpoint, so `INSERT_TERMS` records age out at every checkpoint. Today that's fine because docid pages are the durable on-disk record of what's in the memtable; on cold start, `tp_rebuild_posting_lists_from_docids` walks them and rebuilds. Once the docid pages are gone, *something else* has to make the memtable's pre-checkpoint contents recoverable. The cleanest answer is **spill-at-checkpoint**: ensure the memtable is empty at every checkpoint boundary, so WAL replay since the last checkpoint is sufficient by construction. PR 3 is therefore not just "delete the docid path" — it is "delete the docid path *and* introduce the durability mechanism that replaces it."

**No longer load-bearing for the docid-rebuild race.** Earlier drafts of this plan listed the rebuild-vs-redo race on a hot standby as a side benefit of removing the rebuild. PR 2 closes that race in place: `tp_create_shared_index_state` now takes a `start_locked` flag that acquires the per-index LWLock EXCLUSIVE before the registry entry becomes visible, and the cold-start rebuild path holds the lock across the docid walk. Standby redo blocks on the lock for the duration of the walk and then proceeds against the populated memtable, so PR 3 inherits a clean baseline.

**Files:**
- Modify: `src/index/metapage.h` — drop `first_docid_page`; bump metapage version.
- Modify: `src/index/metapage.c` — gut docid-page allocation and management.
- Modify: `src/access/build.c` — drop `tp_add_docid_to_pages` calls; drop `tp_clear_docid_pages` from `tp_do_spill`.
- Modify: `src/access/vacuum.c` — drop `tp_clear_docid_pages` call.
- Modify: `src/index/registry.c` — drop `tp_clear_docid_pages` call.
- Modify: `src/index/state.c` — `tp_get_local_index_state` no longer calls `tp_rebuild_posting_lists_from_docids` for v2 metapage indexes; for v1 (legacy), in-place upgrade.

**Closes nothing externally.** Pure cleanup + WAL volume reduction.

## Task 3.1: Implement spill-at-checkpoint (or equivalent) durability

This task is a **design-then-implement** step and must land before the docid-page deletions in 3.4 so we never have a window where the memtable's pre-checkpoint contents are unrecoverable.

- [ ] **Step 1: Pick a mechanism**

There is no extension hook for "before checkpoint" in Postgres 17 / 18, so we can't directly piggyback on the checkpointer. Three viable shapes:

  1. **Background worker, timer + WAL-volume heuristic.** A dedicated bgworker started from `_PG_init` wakes on a timer (e.g. once per `checkpoint_timeout / 4`) and on `LATCH` after a configurable number of WAL bytes since the last spill, and forces a spill on every index whose memtable is non-empty. Approximates spill-at-checkpoint without coordinating with the checkpointer; the trade-off is an extra spill / segment per cycle even on idle indexes.
  2. **Per-backend pre-commit auto-spill, sized by retained-WAL distance.** On `XactCallback` PRE_COMMIT, if `pg_current_wal_lsn() - shared->last_spill_lsn` exceeds a threshold derived from `max_wal_size`, the committing backend spills before returning. No bgworker; cost is paid by the writer that crossed the threshold. Trade-off: large transactions can be hit with a big spill at commit time.
  3. **Loosen the "memtable must survive checkpoint" requirement.** If the memtable can be lost on cold start as long as the segments + WAL-since-spill are intact, then no new mechanism is needed — first-access just builds an empty memtable and starts replaying WAL. This requires that segments+SPILL are emitted often enough that a memtable rebuild from scratch is bounded in time. In practice that's also a heuristic-driven spill, just one that runs whenever the memtable is "big enough" rather than "checkpoint is near."

Recommend (3) as the default and (1) as a fallback if (3) leaves recovery-time unbounded under realistic workloads. Either way, the spill itself is already a single function call (`tp_do_spill`) — the new code is the trigger logic, not the spill.

- [ ] **Step 2: Wire the chosen trigger**

Whichever mechanism is chosen, the trigger calls `tp_do_spill` (which in PR 2 emits the SPILL WAL record and clears the memtable). No new WAL record types are needed.

- [ ] **Step 3: Crash-recovery test**

Add a test that:
1. Inserts N docs (enough that a spill would normally occur).
2. Issues `CHECKPOINT`.
3. Inserts M more docs without spilling.
4. `kill -9` the postmaster.
5. Restarts.
6. Verifies all N+M docs are queryable.

Without this task, that test would fail under PR 3 because the first N docs would be lost (gone from WAL after the checkpoint, no docid pages to rebuild from).

## Task 3.2: Bump metapage version, gate docid-page reads

- [ ] **Step 1: Define metapage v2**

In `src/index/metapage.h`:

```c
#define TP_METAPAGE_VERSION_V1 1
#define TP_METAPAGE_VERSION_V2 2  /* no first_docid_page */
```

The metapage already has a version field (verify by reading current `TpIndexMetaPage`). Bump the constant when writing a fresh metapage; keep v1 reads working.

For storage compactness, the `first_docid_page` field can stay in the struct but is unused on v2 metapages (always `InvalidBlockNumber`). Or it can be removed and the field repurposed. Implementer's call based on `pg_upgrade` semantics — leaving the field in place is friendlier to anyone with a v1 metapage on disk.

## Task 3.3: In-place v1 → v2 metapage upgrade

- [ ] **Step 1: Upgrade on first open of a legacy index**

In `tp_get_local_index_state`, on the path that builds the local state for a previously-untouched index:

```c
if (metap->version == TP_METAPAGE_VERSION_V1) {
    /*
     * Legacy index from before PR 3. Read the docid chain into
     * the memtable one last time, clear it, mark the metapage
     * as v2.
     */
    tp_rebuild_posting_lists_from_docids(index_rel, ls, metap);
    tp_clear_docid_pages(index_rel);
    tp_metapage_set_version(index_rel, TP_METAPAGE_VERSION_V2);
}
```

After this point, the index is v2 and never touches the docid-page code path again. The upgrade is a one-time cost on first open, bounded by the size of the existing docid chain (at most one memtable's worth of TIDs).

- [ ] **Step 2: Add a test exercising the upgrade**

In `test/scripts/upgrade_*.sh` (or a new one), build a v1 index using a pre-PR-3 binary, upgrade the extension, open the index, query it, verify results match. The project already runs upgrade tests against historical versions in CI (`test-upgrades` workflow).

## Task 3.4: Remove `tp_add_docid_to_pages` from insert path

- [ ] **Step 1: `tp_insert` no longer calls `tp_add_docid_to_pages`**

The two call sites in `src/access/build.c` (lines 1578 and 1593 as of PR 2) get removed. The insert path becomes:

```c
START_CRIT_SECTION();
tp_xlog_insert_terms(RelationGetRelid(index), ht_ctid, tpvec);
tp_memtable_apply_insert(...);
END_CRIT_SECTION();
```

- [ ] **Step 2: `tp_do_spill` no longer calls `tp_clear_docid_pages`**

`src/access/build.c:138`, `src/access/build.c:496`, `src/access/vacuum.c:204`, `src/index/registry.c:831` — drop the calls.

- [ ] **Step 3: Build, full regression + replication + recovery**

```bash
make clean && make && make install
~/pgz/target/release/pgz restart pg17
make installcheck
make test-replication-extended
bash test/scripts/recovery.sh
```

All should pass. The 10 #345-class tests remain passing (memtable still populated by WAL replay). Recovery still works (no docid pages to read on cold start; WAL replay is the sole mechanism).

- [ ] **Step 4: Commit (intermediate)**

```bash
git commit -m "refactor: drop docid-page writes from insert and spill paths"
```

## Task 3.5: Remove now-dead code

- [ ] **Step 1: Delete the dead functions**

Once the upgrade path has consumed `tp_rebuild_posting_lists_from_docids` and `tp_clear_docid_pages` for v1 indexes, those functions can be inlined into the upgrade path or deleted entirely. Same for `tp_add_docid_to_pages` and the docid-page allocation machinery in `src/index/metapage.c`.

- [ ] **Step 2: Remove `first_docid_page` from `TpIndexMetaPage`**

Or, more conservatively, keep the field but mark it deprecated and never write to it. A future major version can drop it. Either is fine.

- [ ] **Step 3: Update WAL volume note in CLAUDE.md**

Mention that PR 3 reduces per-insert WAL volume by removing the redundant docid-page records.

- [ ] **Step 4: Delete this plan file**

```bash
git rm docs/plans/2026-05-01-physical-replication.md
```

The plan was committed as part of PR 1 as a design anchor while the work was in progress. Now that PR 3 closes out #345, the plan has served its purpose; remove it to avoid stale design docs accumulating in the tree.

- [ ] **Step 5: Final test sweep**

```bash
make installcheck
make test-replication-extended
bash test/scripts/recovery.sh
make test-upgrades   # v1-metapage upgrade path
```

- [ ] **Step 6: Commit, open PR**

```bash
git commit -m "refactor: remove docid-page machinery; metapage v2"
```

PR title: `refactor: remove docid-page machinery; metapage v2`. Body: explain that the docid pages were a crash-recovery mechanism made redundant by PR 2's WAL replay; describe the in-place v1→v2 upgrade; attach before/after WAL bytes-per-insert numbers from a quick benchmark.

---

# Self-Review Notes

**Spec coverage:**
- ✅ bm25vector v2 compaction (PR 1).
- ✅ Custom rmgr with `INSERT_TERMS` and `SPILL` records (PR 2).
- ✅ Direct memtable mutation in redo — no pending list, no drain on read path, no stale flag (PR 2).
- ✅ Docid-page chain removed; primary crash recovery uses spill-at-checkpoint + WAL replay (PR 3).
- ✅ Backward compat for legacy v1 metapage indexes via in-place upgrade on first open (PR 3).
- ✅ Pre-checkpoint memtable durability without docid pages — covered by Task 3.1 (PR 3).
- ✅ Rebuild-vs-redo race on a hot standby — closed in PR 2 via the `start_locked` flag on `tp_create_shared_index_state`. PR 3 inherits a clean baseline.
- ✅ Baseline-then-verify test methodology (PR 2 Task 2.1, 2.4).
- ✅ Documentation in each PR.

**What this plan deliberately doesn't do:**
- **Compress `INSERT_TERMS` payloads.** Deferred. PR 1's v2 vector format already cuts per-record size by ~2×; LZ4 on top is a future optimization once we have wire-format numbers from a real workload. The `xl_tp_insert_terms` struct has no `flags` field today — a future compression bit would require a record-format bump (or a new info code).
- **Fix the PITR corpus-stats bug.** Out of scope. `replication_pitr.sh` PITR-1 remains failing after PR 2 / PR 3. Separate plan needed.
- **Address the `bm25vector.index_name` field.** Kept as-is. Removing it would simplify the type but break the `<@>` operator semantics that depend on the vector knowing its index. Worth revisiting separately.

**Open implementation questions:**
1. **Spill-at-checkpoint mechanism (PR 3 Task 3.1).** No public extension hook exists for "before checkpoint" in PG17/18, so the trigger has to be approximated. Three candidate shapes are sketched in Task 3.1; recommend trying the loosest one first ("memtable can be empty on cold start; first-access just starts replaying WAL since the most recent SPILL"), and only adding a bgworker-driven forced spill if recovery time becomes unbounded. This question must be settled before deleting the docid-page rebuild.
2. **PR 1 → PR 2 → PR 3 sequencing.** The PRs must land in order. PR 2 depends on PR 1's v2 vector (the WAL records carry it). PR 3 depends on PR 2 (removes WAL paths whose redundant copies PR 2 introduced). If PR 2 reveals issues during review, PR 3 stays as a follow-up; PR 2 is correct on its own (just emits redundant docid-page records).
3. **WAL volume tracking.** Each PR should report bytes-per-insert from a controlled benchmark. PR 1 should be neutral (type format change, no WAL records). PR 2 should show ~2× WAL volume vs main (docid pages + INSERT_TERMS). PR 3 should bring it back down to ~1× of main, possibly slightly below. If PR 2's 2× is unacceptable for review, accelerate PR 3 or land them as a single combined PR — but try the staged path first.
4. **Coexistence with PR #343 WAL changes.** PR #343 added `log_newpage_buffer` for page-index pages and `GenericXLog FULL_IMAGE` passes for segment data pages. Those records remain — they cover on-disk segment state on the standby, which is orthogonal to the memtable state this plan covers. After PR 2 lands, run `pg_waldump -r pg_textsearch | sort -u` and confirm that all four record types coexist without duplication or conflict.
