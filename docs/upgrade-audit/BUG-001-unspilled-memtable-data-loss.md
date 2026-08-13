# BUG-001: Silent loss of unspilled memtable data on upgrade from pre-1.3 (metapage v6)

- **Severity:** High (silent, permanent data loss / incomplete query
  results with no error)
- **Component:** `src/index/metapage.c`, `src/constants.h` (v6→v8
  upgrade path)
- **Affected upgrade sources:** every metapage-v6 release —
  `0.5.1, 0.6.0, 0.6.1, 1.0.0, 1.1.0, 1.2.0` — upgrading to any
  metapage-v7+ binary (1.3.0 through current 1.4.0-dev, metapage v8).
- **Not affected:** 1.3.0+ sources (native on-disk L0 memtable, v7).
- **Recovery:** `REINDEX INDEX <name>` fully restores results.

## Summary

When a pre-1.3 (metapage v6) index is upgraded to the current binary,
**every document still resident in the L0 memtable that has not been
spilled to a segment is silently dropped** from the index. Queries
return incomplete results with no error and no client-visible warning.
This happens on the **normal, clean** upgrade path — no crash is
required.

## Root cause

In metapage-v6 releases, the L0 memtable was an in-memory structure.
Durability across restart was provided by a **docid recovery chain**
(on-disk pages anchored by `TpIndexMetaPageData.first_docid_page`,
now renamed `_unused_docid_page`) that recorded the heap ctids of
in-flight documents so they could be **re-tokenized from the heap on
restart**. A clean shutdown did **not** necessarily spill the
memtable; it relied on this chain to replay on next startup.

The v7 redesign (issue #374) replaced the in-memory memtable + docid
chain with an on-disk memtable and **retired the docid chain
entirely**. The current binary has no code that can read or replay it.

On upgrade, `tp_get_metapage()` accepts the v6 page and, on the first
metapage mutation, `tp_metapage_upgrade_to_current()`
(`src/index/metapage.c:245-300`) simply **orphans** the pointer:

```c
BlockNumber orphan_docid_page = metap->_unused_docid_page;
metap->memtable_head_blkno = InvalidBlockNumber;
metap->memtable_tail_blkno = InvalidBlockNumber;
if (BlockNumberIsValid(orphan_docid_page) && !RecoveryInProgress())
    ereport(LOG, (errmsg("... orphaning a non-empty docid chain "
                         "pointer ...")));
metap->_unused_docid_page = InvalidBlockNumber;
```

The documents referenced only by that chain are never re-tokenized, so
they vanish from the index.

## The false assumption

The design explicitly assumes this is safe on a clean upgrade.
`src/constants.h:44-56`:

> "a v1.2.x clean shutdown **always spilled and cleared this
> pointer** … Indexes from a clean v1.2.x shutdown have
> `_unused_docid_page == InvalidBlockNumber` and **upgrade silently
> with no operator intervention**."

**This assumption is false.** Empirically, after a normal clean
`pg_ctl stop -w` (the exact stop used by
`.github/workflows/upgrade-tests.yml:234`), a v0.5.1 / 0.6.1 / 1.0.0 /
1.2.0 index that had post-`CREATE INDEX` inserts still in the memtable
has a **non-Invalid** `_unused_docid_page` — proven by the
`forensic=1` LOG firing on the clean path — and loses exactly those
unspilled documents. A clean shutdown in these releases persists
unspilled memtable data **via the docid chain**, not by spilling.

## Reproduction

```bash
PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config \
  OLD_VERSIONS="1.2.0" test/scripts/upgrade_matrix.sh
```

Observed (also 0.5.1, 0.6.1, 1.0.0 — identical):

```
[1.2.0/memtable_unspilled] truth=50 pre=50 post=25 reindex=50 forensic=1
::warning::BUG-001 reproduced upgrading v1.2.0: unspilled memtable
data lost (recall 25/50). REINDEX recovers (50).
```

- `pre=50` — the old binary finds all 50 sentinel docs.
- `post=25` — after a clean upgrade, the new binary finds only the 25
  that had been spilled into a segment; the 25 that were still in the
  memtable are gone.
- `reindex=50` — `REINDEX` rebuilds from heap and recovers everything.

Minimal manual repro:

```sql
-- under old (e.g. 1.2.0) binary:
CREATE TABLE d(id serial primary key, c text);
INSERT INTO d(c) SELECT 'doc '||g FROM generate_series(1,300) g;
CREATE INDEX i ON d USING bm25(c) WITH (text_config='english');
INSERT INTO d(c) SELECT 'later '||g FROM generate_series(1,300) g; -- stays in L0
-- clean: pg_ctl stop -w ; install current binary ; pg_ctl start
-- the 300 'later' rows are now missing from index scans; REINDEX recovers them.
```

## Impact

- Any operator who inserts into a table after `CREATE INDEX` and
  upgrades (without a manual `bm25_spill_index()` or `REINDEX`) can
  silently lose the most recent documents from search results.
- Read-only workloads after upgrade get **no signal at all** (see
  BUG-002 — the sole diagnostic fires only on the first write).
- Matches the "random issues after upgrades" field reports: the loss
  is proportional to whatever happened to be unspilled at upgrade
  time, so it looks nondeterministic.

## Recommended fixes (in preference order)

1. **Make it loud and recoverable (minimum bar).** When
   `tp_get_metapage()` observes a v6 page with a valid
   `_unused_docid_page`, surface a **client-visible** signal on the
   **read** path (not just first write): either an `ERROR` that
   directs `REINDEX`, or (if erroring on read is too disruptive) a
   `WARNING` on scan start, persisted until a `REINDEX`/spill clears
   it. This converts silent loss into an actionable message and closes
   BUG-002 at the same time.

2. **Auto-recover the data.** During the v6→v8 upgrade, walk the
   orphaned docid chain and re-tokenize the referenced heap ctids into
   the new on-disk memtable (a one-shot replay of the mechanism v6
   used on every restart), so no documents are lost. More work, but
   removes the failure mode entirely.

3. **Force spill/reindex at upgrade time.** Have
   `ALTER EXTENSION … UPDATE` (or first open) refuse to serve a v6
   index with pending docid data until it is rebuilt, rather than
   silently discarding it.

Whichever fix lands, flip `STRICT_UNSPILLED=1` in
`test/scripts/upgrade_matrix.sh` so the harness asserts zero loss.

## Cross-references

- Retirement of the docid chain: issue #374 (on-disk memtable v7).
- v6 read-compat acceptance: issue #383.
- Orphan logic + forensic LOG: `src/index/metapage.c:245-300`.
- False "clean shutdown always spilled" assumption:
  `src/constants.h:44-56`.
