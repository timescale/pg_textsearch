# Background compaction via pg_durable

Status: proof of concept.

## Why

Compaction runs **synchronously inside the writing transaction** by
default. `tp_do_spill()` spills the memtable to an L0 segment, then
`tp_maybe_compact_level(index_rel, 0)` merges any level that is over
`pg_textsearch.segments_per_level`, recursing into higher levels —
all while holding the per-index `LW_EXCLUSIVE` lock, and all charged
to whichever writer's transaction happened to trigger the spill.

This POC moves that merge work off the write path and into a
[pg_durable](https://github.com/timescale/pg_durable) background
task, answering one narrow question: does pg_textsearch's storage
model fit pg_durable's execution model at all? It does, with one
real limitation documented below.

pg_textsearch itself gains no dependency on pg_durable — none of its
C code mentions `df.*`. The bridge is a single GUC that names a SQL
function to call; the pg_durable-specific glue lives entirely in
[`scripts/durable_compaction/`](../scripts/durable_compaction/) as
scripts, not compiled code.

## Architecture

```
INSERT ... -> tp_auto_spill_if_needed()
                 |
                 v
            tp_do_spill()                [holds LW_EXCLUSIVE]
                 |
      mode=inline| mode=background   mode=off
                 |        |             |
   tp_maybe_compact_level |          (nothing)
                          v
              tp_compaction_request(relid)
                 append OID to a backend-local List in
                 TopMemoryContext (no SPI, no catalog access,
                 no locks -- the per-index LWLock is held here)
                          |
                 ... rest of transaction ...
                          |
                          v
            XACT_EVENT_PRE_COMMIT  (src/mod.c tp_xact_callback)
                          |
                 tp_compaction_flush_requests()
                   for each requested relid:
                     two nested BeginInternalSubTransaction()
                       PushActiveSnapshot + SPI:
                         SELECT <request_fn>(<oid>::oid::regclass)
                     Release / Rollback -> WARNING, xact still commits
                          |
                          v
     bm25_request_compaction(regclass)   [SECURITY DEFINER, owned by
                                          textsearch_compactor]
                          |
                 df.start(df.loop(body, cond), label,
                          transaction_mode => 'caller')
                          |
                    (row in df.instances, same xact as the spill)
                          |
                     COMMIT  <-- the writer's transaction ends here
                          |
                          v
     pg_durable bgworker connects as textsearch_compactor,
     on its own libpq connection, per node execution
                          |
        each df.loop iteration is a SEPARATE transaction on a
        separate backend, so the per-index LW_EXCLUSIVE lock is
        released between merge batches instead of held for the
        whole cascade
                          |
                 SELECT bm25_compact_step(<oid>::oid::regclass)
                          |
                    tp_compact_step()  -> one merge batch,
                                          one level at a time
```

Two layers of failure coverage, because pg_durable v0.2.6 has **no
retry**:

1. **Next-spill retry.** Level counts stay over threshold, so the
   next spill on that index enqueues another request. Free for any
   index still being written to.
2. **Hourly cron backstop.** A single long-lived
   `df.loop(df.wait_for_schedule('0 * * * *') ~>
   'SELECT bm25_compact_pending()')` instance, started once, sweeps
   every BM25 index the caller has rights on and compacts any that
   are over threshold. Covers an index that stops receiving writes
   right after a failed compaction — the case next-spill retry
   cannot reach, and exactly when queries are slowest relative to
   the outstanding work.

The backstop is the guarantee; the spill-time request is an
accelerator on top of it.

### The decoupling seam

`pg_textsearch.compaction_request_function` (string, `PGC_SUSET`,
default `''`) names a one-argument `regclass` SQL function called at
`XACT_EVENT_PRE_COMMIT`, once per index that requested compaction
during the transaction. pg_textsearch does not care what that
function does; the pg_durable wrapper
(`scripts/durable_compaction/02_wrapper.sql`) is one implementation
of it, supplied as an operator script rather than built in. This
also lets the SQL regression suite exercise the whole hook mechanism
— including commit/rollback atomicity — against a trivial logging
function, with no pg_durable installed, so CI stays green without
the dependency.

## GUCs

| GUC | Description | Default |
|-----|-------------|---------|
| `pg_textsearch.compaction_mode` | `inline` merges during the spilling transaction (unchanged default behavior); `background` records a request, dispatched at pre-commit, instead; `off` disables automatic compaction entirely (pair with the cron backstop). | `inline` |
| `pg_textsearch.compaction_request_function` | Schema-qualified name of a one-argument `regclass` function called at pre-commit for each index that requested compaction. Unset or unresolvable: a `WARNING` is logged and the writer's transaction still commits. | `''` |
| `pg_textsearch.segments_per_level` | Also read by `bm25_needs_compaction()` — see the GUC-scope trap below. | `8` |

Both new GUCs are `PGC_SUSET`: a plain writer role cannot `SET`
them, so misconfiguring `compaction_request_function` requires
superuser access, not an ordinary application bug.

## Setup checklist

Full detail, rationale, and copy-pasteable commands are in
[`scripts/durable_compaction/README.md`](../scripts/durable_compaction/README.md).
In order:

1. **Preload both extensions** —
   `shared_preload_libraries = 'pg_durable,pg_textsearch'` — and
   restart the server.
2. **Put pg_textsearch's BM25 indexes in the database named by
   `pg_durable.database`.** `df.start()` must run inside the
   writer's own transaction for the atomicity guarantee to hold, so
   the `df` schema and the indexes have to share a database.
   `df.start`'s `database =>` parameter selects where the *nodes*
   run, not where the instance row is written, so it does not help
   here.
3. **Create the `textsearch_compactor` role**
   (`01_setup_role.sql`) — `LOGIN`, `NOSUPERUSER`, `INHERIT`, and a
   member of the index owner's role (needed because
   `bm25_compact_step()` and friends gate on `object_ownercheck()`,
   which resolves through inherited membership).
4. **Add a passwordless `pg_hba.conf` entry for that role.** The
   worker opens a fresh libpq connection as `submitted_by` with no
   password and no host, so it hits the unix socket — where `peer`
   fails for a role that is not the OS user.
5. **Wait for the pg_durable background worker to initialize.** It
   connects and starts the duroxide runtime asynchronously after
   `CREATE EXTENSION pg_durable` and after every restart; `df.start()`
   fails with "background worker not yet initialized" until it does.
6. **Install the SECURITY DEFINER wrapper** (`02_wrapper.sql`), owned
   by `textsearch_compactor`, and grant `EXECUTE` on it to writer
   roles — and nothing else pg_durable-related to them.
7. **Start the cron backstop once**, running *as*
   `textsearch_compactor` (`03_backstop.sql`), so
   `df.instances.submitted_by` matches what the SECURITY DEFINER
   path produces.
8. **Set `compaction_mode = background`** and
   `compaction_request_function =
   'public.bm25_request_compaction'`, then
   `SELECT pg_reload_conf();`.
9. **Set `segments_per_level` where the *worker* session sees it** —
   see immediately below. This step is easy to skip and produces a
   silent no-op, not an error.

### The GUC-scope trap

`segments_per_level` decides two different things in two different
sessions:

| Decision | Evaluated in |
|----------|--------------|
| enqueue a request at `XACT_EVENT_PRE_COMMIT` | the **writer's** session |
| `bm25_needs_compaction()`, the `df.loop` condition | the **compactor's worker** session |

The worker connects fresh as `textsearch_compactor` and inherits
nothing from the writer's session. Lowering the threshold only for
the writer produces an instance that reaches `completed` having
merged nothing, while level counts never move — success with no
effect:

```sql
-- writer session: SET pg_textsearch.segments_per_level = 2
-- worker session never saw it, still uses the default 8
SELECT bm25_level_counts('my_idx');    -- {4,0,0,0,0,0,0,0}
-- ... df.loop instance completes ...
SELECT bm25_level_counts('my_idx');    -- {4,0,0,0,0,0,0,0}  (unchanged)
```

Set it somewhere both sessions inherit — `postgresql.conf`,
`ALTER SYSTEM`, `ALTER DATABASE ... SET` — or explicitly on the
compactor role with
`ALTER ROLE textsearch_compactor SET pg_textsearch.segments_per_level
= <n>`. The same applies to any other GUC that `bm25_compact_step()`
or `bm25_needs_compaction()` reads.

## Two-layer failure story

pg_durable 0.2.6 has no retry, backoff, or `on_failure`: a node that
raises an error kills its instance outright. Coverage is therefore
deliberately two layers, not one:

- A **failed** compaction leaves level counts unchanged, so the very
  next spill against that index enqueues a fresh request. This is
  free and requires no operator action for an actively-written
  index.
- The **hourly backstop** (`bm25_compact_pending()`, driven by
  `df.wait_for_schedule`) catches everything else: an index that
  stops being written to right after a failure, or one that was
  never wired to the spill hook at all (`compaction_mode = 'off'`).
  It iterates every index `bm25_indexes_needing_compaction()`
  returns, wraps each in its own `EXCEPTION WHEN OTHERS`, and reports
  the count actually compacted — one broken index cannot take the
  whole sweep down.

Both were exercised end-to-end in
[`test/scripts/durable_compaction.sh`](../test/scripts/durable_compaction.sh):
revoking the compactor's membership in the index owner's role forces
a compaction failure (instance `failed`), and restoring it lets the
next spill self-heal; and disabling the spill hook entirely
(`compaction_mode = 'off'`) still lets the backstop compact a
degraded index with no writer involvement at all.

## The honest limitation

Background compaction moves the merge CPU and I/O off the
triggering transaction, but `bm25_compact` still holds the
per-index `LW_EXCLUSIVE` lock for the duration of the merge.
Concurrent writers therefore still block; only the spilling
transaction is freed. Removing that serialization is out of
scope for this POC.

The stepped cascade (`bm25_compact_step()` driven by `df.loop`)
narrows the exclusive window from "the whole multi-level cascade" to
"one merge batch at one level" — each `df.loop` iteration runs on
its own backend in its own transaction, so the lock is dropped and a
writer can commit between levels. But within a single batch, the
lock is still held for as long as that merge takes, exactly as it
would be for `bm25_compact()` called inline.

## When background mode is worth enabling

Background mode is a latency-*variance* trade, not a free win. It
adds a roughly constant **~0.06s** to every spill that requests
compaction — two nested subtransactions, SPI, and `df.start()`'s own
writes to `df.instances`/`df.nodes` — and removes the merge from the
tail. Measured with
[`benchmarks/durable_compaction_latency.sh`](../benchmarks/durable_compaction_latency.sh):

| | inline | background |
|---|---|---|
| typical spill, no merge triggered | ~0.020s | ~0.079s |
| spill that triggers a merge (200k-row corpus) | 0.982s | 0.084s |

At `segments_per_level = 2` over the same corpus, forcing a deeper
cascade, the triggering transaction measured **2.794s inline versus
0.097s background** — and both modes converged on the identical final
layout, so the background path defers the work rather than skipping
it.

The implication for operators: enable background mode when merges are
expensive relative to the enqueue, which on commodity hardware means
roughly tens of thousands of non-trivial documents and up. Below that,
inline is simply cheaper, and turning background mode on will make
every write marginally slower for no benefit.

## Future work: non-blocking merges

Segments are immutable, and
the metapage reaches them only through `level_heads[]`. A merge
can therefore build its output segment entirely out-of-band while
readers and writers continue against the existing segments, and
take an exclusive lock only for the metapage pointer swap that
publishes the result and unlinks the sources. Displaced source
pages already have a standby-safe reclaim path — the
`pending_free_head` tombstone chain from #380 — which is exactly
the mechanism needed to keep in-flight readers valid after the
swap. This would shrink the exclusive window from "duration of
the merge" to "duration of one buffer update", unblocking both
readers and writers.

The stepped cascade introduced by `bm25_compact_step()` is the
first move in that direction: it already reduces the exclusive
window from "the whole cascade" to "one merge batch". Making a
single batch non-blocking is the remaining step.

## See also

- [`scripts/durable_compaction/README.md`](../scripts/durable_compaction/README.md)
  — setup commands, operating queries, and pg_durable-specific
  known limitations.
- [`test/scripts/durable_compaction.sh`](../test/scripts/durable_compaction.sh)
  — the end-to-end verification test (`make test-durable`).
- [`benchmarks/durable_compaction_latency.sh`](../benchmarks/durable_compaction_latency.sh)
  — p50/p99 insert latency, `inline` vs `background`.
- [`docs/background_compaction_report.md`](background_compaction_report.md)
  — the POC report-back: findings, measured numbers, and pg_durable
  limitations discovered along the way.
