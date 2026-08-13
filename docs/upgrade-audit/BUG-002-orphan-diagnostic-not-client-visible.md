# BUG-002: The upgrade data-loss diagnostic is not client-visible and is unreliable

- **Severity:** Medium (observability; it is the *only* signal for the
  high-severity BUG-001, and it fails to reach operators)
- **Component:** `src/index/metapage.c` `tp_metapage_upgrade_to_current()`
- **Related:** [BUG-001](BUG-001-unspilled-memtable-data-loss.md)

## Summary

The sole diagnostic emitted when the current binary orphans a pre-1.3
docid chain (the event that causes BUG-001's data loss) is a single
`LOG`-level message. It has three independent problems that together
make it useless as an operator warning:

1. **Not client-visible.** It is `ereport(LOG, …)`, which goes only to
   the server log, never to the connected client. Operators running
   `ALTER EXTENSION … UPDATE` and a few queries see nothing.

2. **False negatives (fires only on the first *write*).** The message
   is emitted from `tp_metapage_upgrade_to_current()`, which runs on
   the first metapage **mutation**. A **read-only** workload after
   upgrade never triggers it, so an index can silently serve
   incomplete results indefinitely with no log line at all.

3. **False positives (fires when no data was lost).** The message
   fires whenever `_unused_docid_page` is non-Invalid, regardless of
   whether those documents were actually unspilled. On releases
   0.5.1 / 0.6.0 / 0.6.1 / 1.0.0 the pointer stays set even after a
   normal spill, so upgrading a fully-spilled `two_seg` / `multi_seg`
   index logs the scary "orphaning a non-empty docid chain" line
   **even though recall is 100%**. (1.2.0 clears the pointer on spill,
   so its false-positive rate is lower — but it still loses truly
   unspilled data, per BUG-001.)

## Evidence

From `test/scripts/upgrade_matrix.sh` (see the matrix in
[README](README.md)):

| old ver | shape              | recall  | forensic LOG | interpretation |
|---------|--------------------|:-------:|:------------:|----------------|
| 0.5.1   | two_seg            | 50/50   | **1**        | false positive (no loss, but LOG fires) |
| 0.5.1   | multi_seg          | 50/50   | **1**        | false positive |
| 1.2.0   | two_seg / multi_seg| 50/50   | 0            | correctly silent |
| 1.2.0   | memtable_unspilled | **25/50** | 1          | true positive — but only visible in server log, and only because the writeprobe INSERT fired it |

A purely read-only post-upgrade session on the 1.2.0
`memtable_unspilled` index would show `forensic=0` **and** `25/50`
recall — total silence over real data loss.

## Recommended fix

Covered by BUG-001 fix option (1): move the detection to the **read**
path in `tp_get_metapage()` (where the v6 page + valid
`_unused_docid_page` are still observable), and emit a
**client-visible** `WARNING` (or `ERROR` directing `REINDEX`).
Because the false-positive cases (spilled-but-pointer-set) cannot be
distinguished from real loss purely from the metapage bytes, the
message should be advisory ("index may be missing recently-inserted
documents from a pre-1.3 upgrade; run `REINDEX` to be certain") rather
than asserting definite loss — but it must be visible to the client,
not buried at `LOG` level and gated on a write.
