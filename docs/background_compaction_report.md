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

**Scenario 1 — the average spill round — favours inline, and that is
a real cost, not a measurement artifact.** Inline p50 stayed close to
~0.020-0.024s across 10 to 500 rounds and two `segments_per_level`
settings; background p50 stayed close to ~0.077-0.084s. **The two
nested subtransactions plus SPI plus `df.start()`'s own writes to
`df.instances`/`df.nodes` cost a fairly constant ~0.06s per spill**,
and that cost is paid on *every* spill that requests compaction,
whether or not a merge follows.

But scenario 1 is the wrong question. Most spill rounds merge
nothing, so averaging over them measures the enqueue overhead and
almost nothing else. The cost this design exists to remove is paid
by the one unlucky transaction that crosses the threshold: under
`inline` it waits for the whole merge, under `background` it waits
only for the enqueue. Scenario 2 isolates that transaction — 8
segments of 25,000 substantial documents each (200,000 rows), then
one 20-row INSERT that triggers the cascade:

```
$ BENCH_ROUNDS="10" benchmarks/durable_compaction_latency.sh
[...]
Scenario 2: the single transaction that triggers a real merge
(8 segments x 25000 rows)
mode         commit (s)
----         ----------
inline       0.982
background   0.084
```

**That is the crossover: ~12x on the triggering transaction**, and it
grows with the size of the merge. Measured by hand on the development
cluster at `segments_per_level = 2` (a deeper cascade over the same
200,000-row corpus) the gap was larger still — **2.794s inline versus
0.097s background, ~29x** — and both modes converged on the identical
final layout, `{1,0,0,1,0,0,0,0}`, confirming the background path is
not skipping work but deferring it.

So the honest summary is a trade, not a win everywhere:

| | inline | background |
|---|---|---|
| typical spill (no merge) | ~0.020s | ~0.079s |
| spill that triggers a merge | 0.982s / 2.794s | 0.084s / 0.097s |

Background mode adds a fixed ~0.06s to every compaction request and
removes seconds from the tail. It is a latency-*variance* trade:
worth it when merges are expensive relative to the enqueue, which on
this hardware means roughly a corpus of tens of thousands of
non-trivial documents and up. At toy scale — the scale most of the
regression tests run at — inline is simply cheaper, and the numbers
above should not be read as claiming otherwise.

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

## Task 9 findings (verified 2026-08-26, `run_demo.sh`)

**Inline-mode `statement_timeout` does not undo a completed merge.**
The Task 9 spec assumed that cancelling the triggering transaction
mid-cascade (via `statement_timeout`, relying on
`CHECK_FOR_INTERRUPTS()` inside the merge loop) would leave the
segment layout unchanged, matching ordinary transactional rollback.
Measured behavior on this machine is different: with
`pg_textsearch.compaction_mode = 'inline'`, an 8-segment, 200,000-row
corpus, and `segments_per_level = 2`, cancelling the triggering
statement at timeouts ranging from 50ms up to just under the full
cascade duration *all* produced the exact same result — the
statement ran to the full measured cascade time (matching the
uncancelled calibration run almost exactly) before the cancellation
was observed, and the final segment layout was the fully-converged
one, identical to a successful run. Only the triggering row's
visibility was rolled back (`SELECT ... WHERE body = '...'` returned
`f`); the L0 segment count had already dropped to its final value.

This is consistent with, not a bug in, the documented architecture:
segment merges are physical page mutations WAL-logged directly via
`GenericXLog` (see `CLAUDE.md`'s "Physical replication" note — there
is no custom rmgr and no undo log for these pages), exactly like a
B-tree page split surviving a `ROLLBACK`. `ROLLBACK` undoes heap
tuple visibility, not already-flushed index structure. Practically,
this means inline mode's `statement_timeout` "protection" is
illusory for compaction: the client pays the *full* merge cost and
still receives an error, which is a strictly worse outcome than
either succeeding or failing fast. `demo/background_compaction/run_demo.sh`'s
Act 1 was rewritten to assert the measured behavior (L0 drops despite
the cancellation) rather than the originally assumed one (level
counts unchanged); see the comment above that assertion in the demo
script for detail.

**Root cause: the merge's `CHECK_FOR_INTERRUPTS()` calls cannot
fire.** The behavior above initially looked like a timing accident —
cancellation arriving just as the cascade finished — but it
reproduces at every timeout from 50ms upward, which no timing
explanation covers. The reason is structural:
`src/segment/merge.c` calls `CHECK_FOR_INTERRUPTS()` every 1000
terms (`merge.c:1315`, `:1684`) *while the backend holds the
per-index `LW_EXCLUSIVE`*, and PostgreSQL's `LWLockAcquire()` calls
`HOLD_INTERRUPTS()` for exactly the reason its comment gives:

> ... by the LWLock. This ensures that interrupts will not interfere
> with manipulations of data structures in shared memory.
> — `src/backend/storage/lmgr/lwlock.c:1216-1219`

`CHECK_FOR_INTERRUPTS()` is a no-op while `InterruptHoldoffCount >
0`, and the count only returns to zero when `LWLockRelease()` runs
`RESUME_INTERRUPTS()`. So every interrupt check inside the merge
loop is dead code for as long as the lock is held, and the cancel
is necessarily deferred until the merge has finished and dropped
the lock. The corpus term count is not the limiting factor here —
the demo's dictionary is roughly 64 x 400 = 25,600 distinct terms,
so the checks are reached thousands of times; they simply cannot
act.

Three consequences worth carrying into the next work stream:

1. **`statement_timeout` gives no protection against a long inline
   merge.** A client cannot bound its exposure to compaction; it
   pays the full merge cost and then receives an error. This is the
   strongest practical argument for background mode, stronger than
   the latency numbers above.
2. **The same holdoff bounds Act 3's writer stall.** The 4.5s
   worst-case concurrent-writer latency is not merely a lock wait
   that could be interrupted — a writer blocked behind a merge
   batch cannot be cancelled either.
3. **The interrupt checks in `merge.c` are misleading as written.**
   They suggest long merges are cancellable when they are not.
   Either they should be removed with a comment explaining why
   cancellation is impossible under the lock, or — better, and
   aligned with "Future work: non-blocking merges" — the merge
   should build its output segment *without* holding the lock, at
   which point the checks become both effective and necessary.

**Ranking-invariance must compare document identity, not raw score,
against a table receiving concurrent writes.** The first version of
Act 3's ranking-invariance check captured a `(ctid, score)` baseline
*before* a concurrent writer loop began inserting further rows into
the same table (deliberately — the writer needs to hit the same
index under compaction to measure real `LW_EXCLUSIVE` stall). Every
one of 17/17 later samples showed a numeric score drift after
rounding to 4 decimals — not a ranking bug, but the ordinary and
correct effect of `total_docs`/average document length shifting by a
small amount as new rows commit. Because every demo document is
constructed to be exactly 30 words, BM25's length-normalization term
is identical across all of them, so this drift rescales every
document's score by the same factor and does not reorder them. The
fix was to compare `ctid` *order* only (dropping the score column
from the comparison), which is the correct invariant to assert given
a corpus that is expected to keep growing during the check.

**Measured max concurrent-writer latency (honest, not tuned away):**
with a background cascade running against an 8-segment/200,000-row
`act3_idx`, a concurrent writer hammering the *same* index (0.1s
poll interval, no backoff) saw a worst-case single-`INSERT` latency
of **2.802s** — one merge batch's full `LW_EXCLUSIVE` hold, exactly
the honest limitation `docs/background_compaction.md` already
documents. An independent rerun on the same machine measured
**4.471s** over 81 concurrent inserts, so treat this as "seconds,
varying with the batch that happens to be merging" rather than a
stable figure. Per the root-cause finding above, a writer stalled
this way cannot be cancelled either: it is waiting on an LWLock
whose holder has interrupts held off. All 70 concurrent writes
eventually committed with zero
failures once the batch released the lock; background compaction
does not lose writes, but it does not make merges lock-free either.
This is printed unconditionally in the demo's closing summary, and
is not hidden or averaged away.
