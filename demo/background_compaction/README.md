# Background compaction demo

`run_demo.sh` is a self-contained, three-act demonstration of
`pg_durable`-driven background BM25 segment compaction in
`pg_textsearch`. It stands up its own PostgreSQL cluster (own data
directory, own port), builds a real corpus, and asserts real,
measured behavior — it does not print canned or fabricated numbers.

## What it shows

- **Act 1 (inline mode fails under load).** A single-row `INSERT`
  that happens to cross the compaction threshold, under
  `pg_textsearch.compaction_mode = 'inline'`, blows through a
  `statement_timeout` derived from a real calibration run on the
  machine it's running on. The row is rolled back — but, measured
  rather than assumed, the merge is *not*: segment merges are
  physical, `GenericXLog`-logged page mutations with no undo log
  (like a B-tree page split), so by the time the cancellation is
  observed the cascade has already run to completion. The client
  pays the full merge cost and still gets an error.
- **Act 2 (background mode succeeds).** The identical insert,
  against a freshly rebuilt identical corpus, under
  `compaction_mode = 'background'` commits in well under the same
  timeout. The merge still happens — off the triggering transaction,
  driven by `pg_durable` — and the index's level counts are checked
  to have actually changed, not just the instance status.
- **Act 3 (concurrency correctness).** While a background cascade is
  confirmed still `running`, the demo runs, concurrently: a
  ranking-invariance check (top-20 results for a fixed query term
  are compared by document identity across samples taken during and
  after the cascade), a "no torn reads" check (one continuous
  `REPEATABLE READ` transaction samples the same query three times,
  spanning before/during/after the cascade), a transactional
  atomicity check (`ROLLBACK` enqueues nothing, `COMMIT` enqueues
  exactly one durable instance), a concurrent writer loop against the
  *same* index being merged (with its worst observed latency printed
  honestly), and a final row-count sanity check. Every one of these
  is a hard, non-vacuous assertion that exits non-zero on failure.

## How to run it

```bash
export PATH="/home/azureuser/.pgrx/17.10/pgrx-install/bin:$PATH"
demo/background_compaction/run_demo.sh
```

It requires `pg_durable` and `pg_textsearch` to both be built and
discoverable via `pg_config --pkglibdir` / `--sharedir`; the script
preflight-checks for both and fails with a clear message naming the
missing module if either is absent. It creates its own cluster in
`demo/background_compaction/tmp_demo_data` (gitignored, `tmp_*/`) on
port 55449, and tears it down (stop + remove the data directory) on
exit, success or failure. It is **not** part of `make test-all` or
any CI target.

Expected runtime: **about 4.5-5 minutes** on the machine this was
developed on (four full corpus builds of 8 segments x 25,000 rows
each — one for calibration, one per act — dominate the wall time;
the actual compaction/assertion logic in each act is seconds).

## Honest caveat: background mode does not make merges lock-free

Background compaction moves the merge **off the triggering
transaction**. It does **not** make merges lock-free:
`bm25_compact_step()` still takes the per-index `LW_EXCLUSIVE` lock
for the duration of one merge batch, and concurrent readers and
writers serialize behind it — see the comment at
`src/access/build.c:606`, which confirms the scan and append paths
take `LW_SHARED` on that same lock. On the run captured for this
demo, a concurrent writer against the same index saw a worst-case
single-`INSERT` latency of **2.802s** while a merge batch held the
lock — a real, measured stall, printed unconditionally by the
demo's closing summary rather than hidden or averaged away. See
`docs/background_compaction.md`'s "Future work: non-blocking merges"
section for the proposed (out-of-scope-for-this-POC) fix: since
segments are immutable, a merge could build its output out-of-band
and take the exclusive lock only for the metapage pointer swap.
