# Upgrade path × on-disk-state × functionality audit

**Date:** 2026-08
**Scope:** Silent data-integrity problems when upgrading a
pre-existing pg_textsearch index to the current (1.4.0-dev, metapage
v8) binary.
**Trigger:** Persistent field reports of "random issues after
upgrades" suspected to stem from on-disk-format incompatibilities.

## TL;DR

A systematic, empirical sweep of the upgrade matrix (every released
on-disk format × several distinct index states × a heap-ground-truth
recall oracle) found **one high-severity data-loss bug** plus a
**related diagnostic-quality bug**:

- **[BUG-001](BUG-001-unspilled-memtable-data-loss.md) — silent loss
  of unspilled memtable data when upgrading from any pre-1.3 (metapage
  v6) release.** The normal, *clean* upgrade path (`pg_ctl stop` →
  swap binary → start) silently drops every document that was still in
  the L0 memtable (not yet spilled to a segment) at shutdown. No
  client-visible error or warning. `REINDEX` fully recovers.

- **[BUG-002](BUG-002-orphan-diagnostic-not-client-visible.md) — the
  only diagnostic for BUG-001 is a `LOG`-level message that never
  reaches clients, fires only on the first *write*, and has both false
  negatives and false positives.** A read-only workload after upgrade
  gets zero signal that documents are missing.

The legacy tier (metapage v5, ≤0.5.0) and the crash-recovery /
GenericXLog replay of the lazy v6→v8 metapage upgrade were both
verified **safe** — no bugs there.

## Why the existing tests missed this

`.github/workflows/upgrade-tests.yml` (before this PR) had two
structural blind spots:

1. **One data shape only.** It always builds the same index —
   `CREATE INDEX` on batch 1 (→ segment 0), then a second batch that
   is *explicitly spilled* to segment 1. It never leaves data in the
   L0 memtable at upgrade time, which is exactly the state that
   triggers BUG-001.

2. **Self-referential oracle.** "Baselines" are captured from the
   *already-upgraded* new binary reading the old segments, then later
   compared against themselves. The assertions only check
   non-empty / negative-score / index-used. As long as *some* rows
   come back, silently-dropped documents are invisible.

## Methodology

For each released on-disk format we ran the **realistic** upgrade flow
(identical to what operators and CI do — **no** extra old-binary
restart):

```
install old release → initdb → build STATE (old binary)
  → clean `pg_ctl stop` → install current binary → start new binary
  → measure recall → REINDEX → measure recall
```

**States** (`test/scripts/upgrade_matrix.sh`):
`single_seg`, `two_seg`, `multi_seg`, and `memtable_unspilled`
(index built, then a second batch inserted and **left in L0**).

**Ground-truth oracle.** A rare sentinel token (`qwxsentinel`) is
planted in a known subset of rows. Recall = how many of the heap's
sentinel rows appear in the BM25 index scan's top-K (K = sentinel
count). Non-matching rows score 0 and sort after negative-scored
matches, so a healthy index yields `recall == truth`. Recall is
measured under the **old** binary (sanity), the **new** binary
(post-upgrade), and after **REINDEX**.

This oracle also avoids the confounds that made earlier ad-hoc probes
unreliable: BMW/IDF saturation (query terms present in nearly all
docs → IDF ≤ 0 → all-pruned), old-version durability quirks (some
releases lose the `CREATE INDEX` segment across their *own* restart),
and BM25 index scans returning extra non-matching rows.

## Result matrix

Realistic clean-upgrade path, `truth = 50` sentinel docs, PG 17.
`post` = recall under the new binary before REINDEX;
`forensic` = count of the "orphaning a non-empty docid chain" LOG line.

| old ver | shape                | pre | post | REINDEX | forensic | verdict |
|---------|----------------------|----:|-----:|--------:|---------:|---------|
| 0.5.1   | single_seg           | 50  | 50   | 50      | 0        | OK |
| 0.5.1   | two_seg              | 50  | 50   | 50      | 1        | OK (false-positive LOG) |
| 0.5.1   | multi_seg            | 50  | 50   | 50      | 1        | OK (false-positive LOG) |
| 0.5.1   | **memtable_unspilled** | 50 | **25** | 50    | 1        | **DATA LOSS** |
| 0.6.1   | memtable_unspilled   | 50  | **25** | 50     | 1        | **DATA LOSS** |
| 1.0.0   | memtable_unspilled   | 50  | **25** | 50     | 1        | **DATA LOSS** |
| 1.2.0   | single/two/multi_seg | 50  | 50   | 50      | 0        | OK |
| 1.2.0   | **memtable_unspilled** | 50 | **25** | 50    | 1        | **DATA LOSS** |
| 1.3.0   | all four             | 50  | 50   | 50      | 0        | OK (native v7 L0) |

(0.5.1/0.6.1/1.0.0 also emit the `forensic` LOG on the *safe*
two_seg/multi_seg shapes — a false positive fixed in 1.2.0; see
BUG-002.)

Additional checks, both **safe**:

- **Legacy (0.5.0, metapage v5) without REINDEX:** the new binary
  raises a clean, client-visible `ERROR` ("incompatible pg_textsearch
  index version … found 5, expected 8") with a `REINDEX` hint, does
  **not** crash, and `REINDEX` restores full recall.
- **Crash after the lazy v6→v8 upgrade:** first write triggers the
  in-place metapage upgrade inside a `GenericXLog` record; a hard
  `SIGKILL` before checkpoint followed by crash recovery leaves recall
  **stable** (25→25, no *additional* loss or corruption); the index
  remains writable/readable and `REINDEX` recovers.

## Reproduction

```bash
# Representative matrix (one release per on-disk format combination):
PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config \
  test/scripts/upgrade_matrix.sh 0.5.0 0.5.1 1.0.0 1.2.0 1.3.0

# Just the data-loss case:
PG_CONFIG=... OLD_VERSIONS="1.2.0" test/scripts/upgrade_matrix.sh
# → ::warning::BUG-001 reproduced upgrading v1.2.0: unspilled
#   memtable data lost (recall 25/50). REINDEX recovers (50).
```

The harness keeps CI green: segment-backed shapes and the REINDEX
safety net are **hard** assertions (they pass on current code); the
unspilled-loss case is surfaced as a non-fatal `::warning::`
annotation. When BUG-001 is fixed, set `STRICT_UNSPILLED=1` to promote
it to a hard assertion.
