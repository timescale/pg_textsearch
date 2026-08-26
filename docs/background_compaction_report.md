# Background compaction via pg_durable — POC report-back

This answers §7 of the original spec (`~/compaction-poc-prompt.md`)
and the extended report-back list in Task 8 of
`docs/superpowers/plans/2026-08-26-background-compaction-pg-durable.md`.
Read alongside [`docs/background_compaction.md`](background_compaction.md)
(architecture and GUCs) and
[`scripts/durable_compaction/README.md`](../scripts/durable_compaction/README.md)
(operator setup).

## Where the spill hook ended up, and why SPI at `tp_do_spill()` was
## not an option

`tp_do_spill()` holds the per-index `LWLock` in `LW_EXCLUSIVE` mode
for its entire body. SPI opens a subtransaction, runs the planner
and executor, and can itself acquire buffer locks and heavyweight
locks on catalogs — doing that while already holding an `LWLock` is
a lock-ordering hazard PostgreSQL's own conventions rule out (LWLocks
are meant to be held only across short, non-reentrant critical
sections, never around arbitrary SQL execution). So the request is
recorded as data, not executed, at the spill site:
`tp_compaction_request(Oid indexoid)` in
`src/index/compaction_request.c` only dedupes and appends an OID to
a `TopMemoryContext`-resident `List` — no SPI, no catalog access, no
`palloc` outside `TopMemoryContext`, no `ereport` above `DEBUG`.

The request is actually dispatched at `XACT_EVENT_PRE_COMMIT`
(`tp_compaction_flush_requests()`, called from `tp_xact_callback()`
in `src/mod.c`, after `tp_bulk_load_spill_check()`). By that point
the per-index lock from the spill is long released, and the
transaction is still `TRANS_INPROGRESS`, so a SQL call made here
commits atomically with everything the writer already did —
including the spill.

**`BeginInternalSubTransaction()` at `TBLOCK_END` did *not* behave
cleanly with a single subtransaction** — this was the single biggest
surprise of the whole POC. `BeginInternalSubTransaction()` explicitly
permits starting a subtransaction while the parent's `blockState` is
`TBLOCK_END` (the state `CommitTransaction()` is in while running the
`PRE_COMMIT` callbacks). But the assertion at the tail of
`RollbackAndReleaseCurrentSubTransaction()` (`xact.c:4806` in both
PostgreSQL 17.10 and 18.4) does not list `TBLOCK_END` among the
states it accepts for the parent:

```c
Assert(s->blockState == TBLOCK_SUBINPROGRESS ||
       s->blockState == TBLOCK_INPROGRESS ||
       s->blockState == TBLOCK_IMPLICIT_INPROGRESS ||
       s->blockState == TBLOCK_PARALLEL_INPROGRESS ||
       s->blockState == TBLOCK_STARTED);
```

A single subtransaction that rolls back from `PRE_COMMIT` therefore
crashes an assert-enabled build. This is **reproducible on stock
PostgreSQL with no extension loaded at all**, via a `DEFERRABLE
INITIALLY DEFERRED` constraint trigger whose PL/pgSQL body catches an
error in an `EXCEPTION` block — so it is an upstream inconsistency
between `BeginInternalSubTransaction()`'s stated contract and
`RollbackAndReleaseCurrentSubTransaction()`'s assertion, not a misuse
on pg_textsearch's part. **Worth reporting upstream.**

The workaround, now in `tp_run_request()`
(`src/index/compaction_request.c`), is two *nested* subtransactions:
an inner one that does the actual SPI call and is rolled back on
failure, wrapped in an outer one that does no work of its own. The
inner rollback pops to a parent in `TBLOCK_SUBINPROGRESS`, which the
assertion does accept; the outer subtransaction is then closed with
`ReleaseCurrentSubTransaction()`, whose success path only requires
`TRANS_INPROGRESS`, which still holds at `PRE_COMMIT`. Verified by
hand: a single subtransaction crashes an assert-enabled build here;
the nested pair does not. **This is load-bearing — there is a
comment in `tp_run_request()` warning against "simplifying" it away,
and this report repeats that warning.**

A second, smaller issue: `PreCommit_Portals()` runs *before*
`CallXactCallbacks(XACT_EVENT_PRE_COMMIT)`, so by the time the hook
runs there is no active snapshot left, and SPI fails with `cannot
execute SQL without an outer snapshot or portal`. The fix is a
`PushActiveSnapshot(GetTransactionSnapshot())` before the SPI call
(popped again, or unwound automatically by
`AtSubAbort_Snapshot()` on the error path).

## Overlapping compaction tasks under sustained write load

**Observed, and expected by design (spec §2.3) — no locking was
added.** Spills are serialized by the per-index lock, so exactly one
request is enqueued per spill that needs compaction, but nothing
prevents a *second* spill on the same index from firing a second
request while the first task's `df.loop` is still stepping through
levels. `bm25_compact_step()` re-reads `level_counts` under its own
lock acquisition on every call, so a redundant step is a correct,
cheap no-op rather than a correctness problem — the tasks do not
corrupt anything, they just do some duplicate work. Documented in
`scripts/durable_compaction/README.md` under "Known limitations"
("Overlapping tasks are possible") and in
`docs/background_compaction.md`.

## Which `df` shape drove the cascade, and whether splitting it
## reduced blocking

**`df.loop` with a result-dependent condition** — not the bounded
`df.seq` chain. `bm25_request_compaction()`
(`scripts/durable_compaction/02_wrapper.sql`) submits
`df.start(df.loop(body, condition), ...)` where `body` calls
`bm25_compact_step(idx)` (merges the lowest over-threshold level,
returns whether more work remains) and `condition` calls
`bm25_needs_compaction(idx)` (re-evaluates the same threshold test
independently). Termination is guaranteed because both read the same
levels against the same threshold.

**Splitting the cascade did measurably reduce how long a concurrent
writer blocked.** Every `df.loop` node execution — body and condition
alike — runs on its own libpq connection in its own transaction, so
the per-index `LW_EXCLUSIVE` lock is dropped between levels instead
of held for the whole cascade. `test/scripts/durable_compaction.sh`
(`test_cascade`) proves this directly: with a table set up to need a
multi-level cascade, a concurrent `app_writer` transaction committed
in 0.078s while `df.instances.status` for the cascade instance was
still `running` — i.e., the writer was not blocked for the cascade's
full duration. The same run recorded 4 distinct `execution_id`
generations in the server log for that one instance (confirming the
cascade really did span multiple transactions, since `df.nodes`
itself reuses node rows across `continue_as_new` generations and
cannot be used to count iterations).

## Measured write-latency delta

Measured with
[`benchmarks/durable_compaction_latency.sh`](../benchmarks/durable_compaction_latency.sh),
which times individual "20-row INSERT + `bm25_spill_index()`"
transactions and reports p50/p99 per mode. Two runs, both on this
development VM:

```
$ benchmarks/durable_compaction_latency.sh
[...]
pg_textsearch.segments_per_level = 4
Round counts to measure: 10 50 200

rounds   mode       p50 (s)      p99 (s)
------   ----       -------      -------
10       inline     0.020819     0.026888
10       background 0.076781     0.095585
50       inline     0.020428     0.031954
50       background 0.082624     0.163292
200      inline     0.020363     0.031222
200      background 0.083082     0.100869
```

```
$ BENCH_ROUNDS="500" BENCH_SEGMENTS_PER_LEVEL=2 \
      benchmarks/durable_compaction_latency.sh
[...]
pg_textsearch.segments_per_level = 2
Round counts to measure: 500

rounds   mode       p50 (s)      p99 (s)
------   ----       -------      -------
500      inline     0.023933     0.044854
500      background 0.084154     0.106680
```

**Be honest about what this shows: background was slower at every
scale tested, not faster.** Inline p50 stayed close to ~0.020-0.024s
across 10 to 500 rounds and two `segments_per_level` settings;
background p50 stayed close to ~0.077-0.084s. Even at 500 rounds
with `segments_per_level = 2` (forcing more frequent, deeper merges),
inline's p99 (0.045s) was still well under background's p50
(0.084s) — **no crossover scale was found in this testing.**

This matches, and sharpens, the Task 7 end-to-end test's own
observation of `inline=0.141s background=0.159s` over 10 spill
rounds *inside one transaction* (a coarser measurement that amortizes
the per-round overhead across the whole batch). Measuring individual
rounds isolates the effect: **the two nested subtransactions plus SPI
plus `df.start()`'s own writes to `df.instances`/`df.nodes` cost a
fairly constant ~0.06s per spill**, and at the corpus sizes reachable
in this testing (documents of a handful of words, up to 500 rounds ×
20 rows), the actual merge work `bm25_compact_step()` performs never
got large enough to exceed that fixed enqueue cost. The upside this
POC exists to capture — moving merge CPU/IO off the writer — only
pays for itself once merges are expensive enough to dwarf the
enqueue overhead; this testing did not reach that scale, and no
number here should be read as showing that it does. Do not
extrapolate these figures to a production-sized corpus without
rerunning at that scale.

## pg_durable limitations hit

- **No retry mechanism.** 0.2.6 has no `max_attempts`, backoff, or
  `on_failure` — a node that raises kills its instance outright. This
  is why the two-layer failure story (next-spill retry plus the
  hourly backstop) exists at all rather than being an optional
  extra; without it, a single transient compaction failure would
  leave an index degraded indefinitely.
- **`connect_as_user` sends no password (and no host).** The worker
  connects as `df.instances.submitted_by` using only username,
  database, and port, so it always goes over the unix socket — where
  `peer` authentication fails for any role that isn't the OS user.
  This forced a dedicated, role-scoped `trust` (or password-free
  ident) `pg_hba.conf` entry for `textsearch_compactor`; see
  `01_setup_role.sql`.
- **Async bgworker init race after `CREATE EXTENSION`.** The
  background worker takes a few seconds to connect and write its
  epoch sentinel after `CREATE EXTENSION pg_durable` (and after every
  restart). `df.start()` calls made too soon fail with "background
  worker not yet initialized"; in the release build this aborts
  cleanly, but every setup/test script here still has to poll and
  retry rather than assume readiness. `03_backstop.sql` and
  `wait_for_durable_worker()` in both the test and the benchmark do
  this.
- **`df.nodes.error` is always empty in 0.2.6.** A failed node has
  `status = 'failed'`, an empty `error` column, and
  `status_details` holding only `{"execution_id": ...}`. The real
  error text is only in the PostgreSQL server log (`duroxide::
  orchestration: Function failed with error: ...`), so diagnosing a
  failed compaction requires log access — `logging_collector` must
  be on. Verified directly: a `SELECT 1/0` node reaches
  `status = 'failed'` with `error = ''`.
- **`LOOP_MIN_ITER_DURATION = 1s`** rate-limits every `df.loop`
  iteration, so an n-step cascade takes at least n seconds of
  orchestration time. This is invisible to writers (it happens
  entirely in the background) and is not merge cost — it should not
  be read as evidence the merges themselves are slow.
- **`df.nodes` cannot be used to count cascade iterations.**
  pg_durable reuses node rows across `continue_as_new` generations,
  so a `df.loop` shows one row per node type regardless of how many
  iterations actually ran. Counting distinct `execution_id` values in
  the server log is the only way found to confirm how many
  generations a cascade actually took.

## Was the two-layer failure story sufficient in testing?

Yes, both layers were exercised and both worked as designed in
`test/scripts/durable_compaction.sh`:

- **Next-spill retry**: revoking `textsearch_compactor`'s membership
  in the index owner's role made `bm25_compact_step()`'s ownership
  check fail, so the compaction instance reached `failed` and level
  counts stayed untouched. Restoring the membership and firing
  another spill produced a fresh instance that reached `completed`,
  and level counts dropped on that retry.
- **Hourly backstop**: with `compaction_mode = 'off'` (spill hook
  disabled entirely) and a table already over threshold,
  `03_backstop.sql`'s sweep — running on a fast test cadence via
  `-v cron='* * * * *'` — compacted the index with no writer
  involvement at all, purely from `bm25_compact_pending()`.

Neither layer needed anything beyond what the spec called for.

## The GUC-scope trap (verified 2026-08-26)

`pg_textsearch.segments_per_level` (and any other GUC
`bm25_compact_step()` / `bm25_needs_compaction()` reads) is evaluated
in **two different sessions**: the writer's, deciding whether to
enqueue a request at `PRE_COMMIT`, and the compactor's own worker
session, deciding — via the `df.loop` condition — whether to
actually merge anything. The worker connects fresh as
`textsearch_compactor` and inherits nothing from the writer's
session. Setting the threshold only in the writer's session produces
an instance that reaches `completed` having merged nothing, while
level counts never move — a silent no-op that looks exactly like
success unless you specifically check that the level counts changed.
Both `scripts/durable_compaction/README.md` and
`docs/background_compaction.md` document this prominently, and
`test/scripts/durable_compaction.sh` asserts on level counts (not
just instance status) specifically to guard against it.

## `memtable_pages_threshold = 1` re-triggers auto-spill per tuple

Confirmed: with `pg_textsearch.memtable_pages_threshold = 1`, a
single 400-row `INSERT` produced roughly 400 individual L0 segments
instead of one, because auto-spill re-fires as soon as the memtable
chain exceeds one page, which a handful of rows can already do. At
`segments_per_level = 2` this is a near-runaway cascade — every
couple of spills immediately requires a merge. This looks like an
existing edge case in the auto-spill/threshold interaction rather
than something introduced by this POC (`tp_bulk_load_spill_check()`
and the page-threshold check are unchanged by this work), but it is
worth flagging: a threshold of `1` is realistically unusable in
practice and should probably be special-cased or documented as
such. Reported here rather than silently worked around.

## Further discrepancies between the plan/spec and reality

- The spec's §5 verification list and the plan's Task 8 report-back
  both assume background compaction will show a **clear** latency
  win once you look at the right scale. That was not observed: see
  "Measured write-latency delta" above. The direction (background
  slower) matches the plan's own caveat about the Task 7 test result,
  but the magnitude (roughly 3-4x at every scale tried, not "roughly
  even" as the smaller Task 7 test suggested) is worse than the plan
  implies, and no scale tested inverted it. This should feed back
  into how the POC's headline claim is framed: it demonstrates
  *mechanism* (the write path no longer pays merge cost, verified via
  the cascade test's concurrent-commit-during-merge behavior)
  more convincingly than it demonstrates *net latency improvement*
  at the corpus sizes reachable here.
- Everything else in the plan and spec — the `transaction_mode =>
  'caller'` atomicity guarantee, the `SECURITY DEFINER` permission
  model, the non-goals in spec §6, the `df.loop` termination
  argument — held up exactly as designed; no other discrepancies were
  found.
