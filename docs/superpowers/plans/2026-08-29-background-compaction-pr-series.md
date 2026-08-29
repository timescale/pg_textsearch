# Background Compaction PR Series Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract PR #471 into five independently reviewable pg_textsearch
pull requests, plus one narrowly scoped pg_durable prerequisite, without
replaying the POC's superseded commit history.

**Architecture:** Treat POC commit
`c1bedf232f89ec3009292003b00cf00caa8fed16` as a read-only implementation
reference. Rebuild each behavior test-first on clean, stacked branches:
storage correctness, native per-index APIs, scheduler-neutral dispatch,
pg_durable submission, then the recurring recovery backstop.

**Tech Stack:** PostgreSQL 17/18 extension C, PGXS, SQL/PLpgSQL regression
tests, Bash integration tests, Rust/pgrx and Duroxide in pg_durable, GitHub
pull requests.

## Global Constraints

- Keep PR #471 and `task2-bm25-compact` intact as the POC/umbrella branch.
- Start PR 1 from current `origin/main`; stack each later branch on its
  immediate predecessor until that predecessor merges.
- Use POC commit `c1bedf232f89ec3009292003b00cf00caa8fed16` as the behavioral
  source of truth, but do not cherry-pick the POC commits wholesale.
- Preserve `pg_textsearch.compaction_mode = 'inline'` as the default.
- Physical index state is authoritative; queued or durable work is only
  best-effort acceleration.
- `bm25_compact_step(regclass)` performs at most one merge batch.
- L7 remains noncompactable and capacity failures fail closed.
- Ordinary PRE_COMMIT callback errors warn without aborting the user
  transaction; query cancellation and shutdown errors propagate.
- Do not create `textsearch_compactor` from extension installation SQL.
- Do not merge pg_durable integration until a release contains the
  seven-argument `df.start(..., max_attempts, max_backoff, on_failure)` API
  from microsoft/pg_durable#354 and live tests pass against that release.
- Keep `df.loop(body, condition)` as the two-argument graph API. Failure
  resilience belongs to `df.start`; do not change the loop signature.
- Released pg_durable v0.2.6 and current `main` lack the required capability.
  The audited #354 head provides it, but is not a releasable dependency.
- Do not include the POC demo or benchmark in the five-PR merge series.
- Use PostgreSQL 17.10 locally:

```bash
export PATH=/home/azureuser/.pgrx/17.10/pgrx-install/bin:$PATH
export PG_CONFIG=/home/azureuser/.pgrx/17.10/pgrx-install/bin/pg_config
```

- Before every pg_textsearch PR is requested for review, run `make`,
  `make installcheck`, inspect an empty `test/regression.diffs`, and run
  `make format-check`.

---

## File and Responsibility Map

| Responsibility | Files |
|---|---|
| Merge selection, capacity, force-merge planning | `src/segment/merge.c`, `src/segment/merge.h` |
| Spill lifecycle and force-merge entry point | `src/access/build.c` |
| Per-index SQL-callable compaction functions | `src/access/compaction.c` |
| Transaction-local request state | `src/index/compaction_request.c`, `src/index/compaction_request.h` |
| GUCs and transaction callbacks | `src/mod.c` |
| Spill-trigger checks and dropped-index cleanup | `src/index/state.c`, `src/memtable/log.c` |
| Extension SQL surface | `sql/pg_textsearch--1.5.0-dev.sql`, `sql/pg_textsearch--1.4.0--1.5.0-dev.sql` |
| Native regressions | `test/sql/compaction.sql`, `test/sql/compaction_request.sql`, matching `test/expected/*.out` |
| Existing force-merge regressions | `test/sql/force_merge.sql`, `test/sql/vacuum_bitmap.sql`, matching expected files |
| pg_durable role and wrapper | `scripts/durable_compaction/01_setup_role.sql`, `scripts/durable_compaction/02_wrapper.sql` |
| Recovery backstop | `scripts/durable_compaction/03_backstop.sql` |
| Live integration tests | `test/scripts/durable_compaction.sh` |
| Canonical design and operator guide | `docs/background_compaction.md`, `scripts/durable_compaction/README.md` |
| Distribution | `Makefile`, `scripts/package-deb.sh`, `.github/workflows/release.yml`, `.github/workflows/package-release.yml` |

Do not split the existing large C files merely for this extraction. The new
`compaction.c` and `compaction_request.c` files already provide the needed
responsibility boundaries.

### Task 1: Preserve PR #471 as the umbrella

**Files:**
- Read: `docs/superpowers/specs/2026-08-29-background-compaction-pr-series-design.md`
- Modify remotely: PR #471 description

**Interfaces:**
- Consumes: POC branch `task2-bm25-compact` at `c1bedf23`
- Produces: an explicitly non-mergeable umbrella that links the extracted
  work without changing the POC source tree

- [ ] **Step 1: Capture the current PR body**

Run:

```bash
gh pr view 471 --repo timescale/pg_textsearch \
  --json body,headRefOid,url > /tmp/pg_textsearch-pr471-before.json
jq -r '.headRefOid' /tmp/pg_textsearch-pr471-before.json
```

Expected: `c1bedf232f89ec3009292003b00cf00caa8fed16`.

- [ ] **Step 2: Write the corrected umbrella body**

Create `/tmp/pg_textsearch-pr471-body.md` with this content:

```markdown
## Status

This branch is a preserved proof of concept and umbrella, not a merge
candidate. It demonstrates background BM25 compaction through pg_durable and
records the design, measurements, and hardening work from the experiment.

The mergeable implementation is being rebuilt as five focused changes:

1. compaction engine correctness;
2. native per-index compaction APIs;
3. scheduler-neutral transaction dispatch;
4. the pg_durable per-index adapter; and
5. the recurring recovery backstop and operator packaging.

The extraction contract is
[`docs/superpowers/specs/2026-08-29-background-compaction-pr-series-design.md`](docs/superpowers/specs/2026-08-29-background-compaction-pr-series-design.md).
Links to the focused PRs will be added as they open.

## POC behavior

Spills in background mode record transaction-local requests. At PRE_COMMIT,
the configured callback submits pg_durable work with
`transaction_mode => 'new'`. The durable stepped cascade performs one merge
batch per transaction.

Default behavior is unchanged: `pg_textsearch.compaction_mode` defaults to
`inline`.

## Documentation

- Architecture and behavior: `docs/background_compaction.md`
- Operator setup: `scripts/durable_compaction/README.md`

## Important limitations

- Merge batches still hold the per-index exclusive lock.
- Physical index state, not task state, is the source of truth.
- The production backstop depends on a released pg_durable containing #354's
  seven-argument `df.start`, configured with `max_attempts => 1` and
  `on_failure => 'continue'`, so a failed activity remains observable while
  the next scheduled `df.loop(body, condition)` iteration can run.
- Cluster role and local authentication provisioning remain external to
  `CREATE EXTENSION`.
```

- [ ] **Step 3: Update and verify the PR body**

Run:

```bash
gh pr edit 471 --repo timescale/pg_textsearch \
  --body-file /tmp/pg_textsearch-pr471-body.md
gh pr view 471 --repo timescale/pg_textsearch --json body \
  --jq '.body' | grep -F "transaction_mode => 'new'"
! gh pr view 471 --repo timescale/pg_textsearch --json body \
  --jq '.body' | grep -F 'background_compaction_report.md'
```

Expected: the first grep prints the corrected mode and the negated grep exits
successfully.

### Task 2: Extract PR 1 — compaction engine correctness

**Files:**
- Create: `test/scripts/force_merge_invariant_source.sh`
- Modify: `Makefile`
- Modify: `src/access/build.c`
- Modify: `src/segment/merge.c`
- Modify: `src/segment/merge.h`
- Modify: `test/sql/force_merge.sql`
- Modify: `test/expected/force_merge.out`
- Modify: `test/sql/merge.sql`
- Modify: `test/expected/merge.out`
- Modify: `test/sql/vacuum_bitmap.sql`
- Modify: `test/expected/vacuum_bitmap.out`
- Modify: `test/scripts/vacuum_concurrent_merge.sh`
- Test: existing `test/scripts/segment.sh`
- Test: existing `test/scripts/recovery.sh`

**Interfaces:**
- Consumes: existing `tp_merge_level_segments()`, `tp_do_spill()`,
  `bm25_force_merge(text)`, metapage `level_counts[]`, and
  `tp_debug_segment_count_limit`
- Produces:

```c
extern bool tp_compaction_needed(Relation index);
extern void tp_maybe_compact_level(Relation index, uint32 level);
extern bool tp_compact_step(Relation index);
extern bool
tp_force_merge_preflight(Relation index, bool spill_creates_segment);
extern void tp_force_merge_all(Relation index);
```

- [ ] **Step 1: Create the clean PR 1 worktree**

Run:

```bash
git -C /home/azureuser/pg_textsearch_3 fetch origin
git -C /home/azureuser/pg_textsearch_3 worktree add \
  /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-1 \
  -b background-compaction-1-engine origin/main
```

Expected: a clean worktree on `background-compaction-1-engine`.

- [ ] **Step 2: Add failing capacity and force-merge regressions**

Use the POC tests as fixtures, but keep PR 1 independent of the APIs from
PR 2:

```bash
git show c1bedf23:test/sql/force_merge.sql
git show c1bedf23:test/sql/vacuum_bitmap.sql
git show c1bedf23:test/scripts/force_merge_invariant_source.sh
```

Add the 128-L0-plus-memtable, nested-full-level, L7-only, mixed-L7, and
all-dead cases to the existing force-merge/vacuum tests. Where the POC uses
`bm25_level_counts()`, assert layout with the existing summary surface:

```sql
SELECT bm25_summarize_index('force_l7_single_idx')
       ~ 'L7 Segment 1:' AS one_l7_segment;

SELECT bm25_summarize_index('force_l7_mixed_idx')
       ~ 'L0 Segment 1:'
       AND bm25_summarize_index('force_l7_mixed_idx')
           ~ 'L7 Segment 1:' AS mixed_layout_ready;
```

For the full-intermediate-level case, construct L1 and L2 with
`segments_per_level = 2`, lower
`pg_textsearch.debug_segment_count_limit` only after the layout exists, then
trigger another inline L0 merge. Assert that the blocking higher level drains
before L0 promotes.

- [ ] **Step 3: Run the regressions to prove the baseline fails**

Run:

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-1
make install
make installcheck
```

Expected: the new force-merge capacity cases fail because the baseline
force-merge traversal can leave multiple segments or mutate before detecting
an impossible L7 layout.

- [ ] **Step 4: Extract the spill lifecycle split**

From POC `src/access/build.c`, transplant the `TpPreparedSpill` and
`TpSpillPostAction` flow so normal spills use:

```c
return tp_do_spill_internal(
        index_state, index_rel, out_segment_root, TP_SPILL_POST_NORMAL);
```

and force merge uses `TP_SPILL_POST_NONE`. Preserve this ordering:

```text
prepare spill
preflight force-merge count transitions
finish spill without normal post-spill policy
execute force merge
check final segment count
```

Do not copy the background-mode switch from the POC in this PR. Normal
post-spill handling remains inline-only:

```c
if (post_action == TP_SPILL_POST_NORMAL)
    tp_maybe_compact_level(index_rel, 0);
```

- [ ] **Step 5: Extract capacity-aware merge planning**

From POC `src/segment/merge.c`, transplant:

```c
static uint32
tp_compaction_candidate(
        const uint16 level_counts[TP_MAX_LEVELS], uint32 first_level);

typedef enum TpForceMergeAction
{
    TP_FORCE_MERGE_COMPLETE,
    TP_FORCE_MERGE_LEVEL,
    TP_FORCE_MERGE_IMPOSSIBLE
} TpForceMergeAction;
```

Also transplant the shared force-merge count transition planner,
`tp_force_merge_preflight()`, and the production `ERRCODE_INTERNAL_ERROR`
postcondition checks. Keep `tp_compact_step()` internal for PR 2, but compile
and test its one-batch behavior through the same candidate selector.

- [ ] **Step 6: Run focused storage validation**

Run:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make test-force-merge-invariant
make test-segment
make test-recovery
make test-concurrency
make format-check
git diff --check
```

Expected: every command exits 0 and `test/regression.diffs` is empty.

- [ ] **Step 7: Commit and open PR 1**

Run:

```bash
git add Makefile src/access/build.c src/segment/merge.c src/segment/merge.h \
  test/sql/force_merge.sql test/expected/force_merge.out \
  test/sql/merge.sql test/expected/merge.out \
  test/sql/vacuum_bitmap.sql test/expected/vacuum_bitmap.out \
  test/scripts/force_merge_invariant_source.sh \
  test/scripts/vacuum_concurrent_merge.sh
git commit -m "fix: enforce compaction capacity invariants"
git push -u origin background-compaction-1-engine
gh pr create --repo timescale/pg_textsearch --base main \
  --head background-compaction-1-engine \
  --title "Enforce compaction capacity invariants" \
  --body "Extracted from #471. This PR contains scheduler-independent merge, spill, and force-merge correctness only."
```

Expected: one focused PR with no new background GUC, callback, pg_durable
script, or public per-index compaction API.

### Task 3: Extract PR 2 — native per-index compaction APIs

**Files:**
- Create: `src/access/compaction.c`
- Create: `test/sql/compaction.sql`
- Create: `test/expected/compaction.out`
- Modify: `Makefile`
- Modify: `sql/pg_textsearch--1.5.0-dev.sql`
- Modify: `sql/pg_textsearch--1.4.0--1.5.0-dev.sql`
- Modify: `docs/background_compaction.md`

**Interfaces:**
- Consumes: PR 1's `tp_compaction_needed()`, `tp_maybe_compact_level()`, and
  `tp_compact_step()`
- Produces:

```sql
bm25_level_counts(regclass) RETURNS int[]
bm25_compact(regclass) RETURNS void
bm25_compact_step(regclass) RETURNS boolean
bm25_needs_compaction(regclass) RETURNS boolean
```

- [ ] **Step 1: Create the stacked PR 2 worktree**

Run:

```bash
git -C /home/azureuser/pg_textsearch_3 worktree add \
  /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-2 \
  -b background-compaction-2-api background-compaction-1-engine
```

- [ ] **Step 2: Add failing API and ACL regressions**

Create `test/sql/compaction.sql` from the POC's per-index sections only:

```sql
SELECT array_length(
           bm25_level_counts('compaction_test_idx'::regclass), 1) = 8
       AS has_all_levels;

SELECT bm25_compact_step('compaction_step_idx'::regclass)
       AS one_batch_ran;
SELECT bm25_needs_compaction('compaction_step_idx'::regclass)
       AS more_work_remains;
SELECT bm25_compact('compaction_full_idx'::regclass);
```

Include wrong-object, non-owner, read-only/recovery, local temporary-index,
one-batch, convergence, and terminal-L7 cases. Do not copy
`bm25_indexes_needing_compaction()` or `bm25_compact_pending()` tests.

- [ ] **Step 3: Run the regression to prove the API is absent**

Run:

```bash
make installcheck
```

Expected: `compaction` fails with undefined-function errors.

- [ ] **Step 4: Add the C entry points**

Extract only these functions and their shared open/ownership helper from POC
`src/access/compaction.c`:

```c
PG_FUNCTION_INFO_V1(tp_level_counts);
PG_FUNCTION_INFO_V1(tp_compact_index);
PG_FUNCTION_INFO_V1(tp_compact_index_step);
```

Do not extract `tp_open_current_bm25_index()`,
`tp_compact_index_step_if_current()`, or
`tp_needs_compaction_if_current()` yet; those physical-identity helpers belong
with the pg_durable adapter in PR 4.

- [ ] **Step 5: Add fresh and upgrade SQL**

Add identical definitions to both SQL scripts:

```sql
CREATE FUNCTION @extschema@.bm25_level_counts(idx regclass)
RETURNS int[]
AS 'MODULE_PATHNAME', 'tp_level_counts'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25_compact(idx regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_compact_index'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION @extschema@.bm25_compact_step(idx regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_compact_index_step'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION @extschema@.bm25_needs_compaction(idx regclass)
RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM pg_catalog.unnest(@extschema@.bm25_level_counts(idx))
             WITH ORDINALITY AS t(cnt, lvl)
        WHERE t.lvl <= 7
          AND t.cnt >= pg_catalog.current_setting(
                  'pg_textsearch.segments_per_level')::int
    );
$$;
```

- [ ] **Step 6: Document and validate PR 2**

Document per-index ownership, locking, one-batch semantics, and L7 behavior in
`docs/background_compaction.md`. Then run:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make format-check
git diff --check
```

Expected: all commands exit 0.

- [ ] **Step 7: Commit and open PR 2**

Run:

```bash
git add Makefile src/access/compaction.c \
  sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql \
  test/sql/compaction.sql test/expected/compaction.out \
  docs/background_compaction.md
git commit -m "feat: add per-index compaction controls"
git push -u origin background-compaction-2-api
gh pr create --repo timescale/pg_textsearch \
  --base background-compaction-1-engine \
  --head background-compaction-2-api \
  --title "Add per-index compaction controls" \
  --body "Extracted from #471 and stacked on the compaction-engine correctness PR. This adds scheduler-independent inspection and manual compaction APIs."
```

### Task 4: Extract PR 3 — scheduler-neutral transaction dispatch

**Files:**
- Create: `src/index/compaction_request.c`
- Create: `src/index/compaction_request.h`
- Create: `test/sql/compaction_request.sql`
- Create: `test/expected/compaction_request.out`
- Modify: `Makefile`
- Modify: `src/access/build.c`
- Modify: `src/index/state.c`
- Modify: `src/memtable/log.c`
- Modify: `src/mod.c`
- Modify: `docs/background_compaction.md`

**Interfaces:**
- Consumes: PR 2's `bm25_needs_compaction()` behavior and PR 1's internal
  `tp_compaction_needed()`
- Produces:

```c
typedef enum TpCompactionMode
{
    TP_COMPACTION_INLINE = 0,
    TP_COMPACTION_BACKGROUND,
    TP_COMPACTION_OFF
} TpCompactionMode;

extern bool tp_check_compaction_request_function(
        char **newval, void **extra, GucSource source);
extern void tp_compaction_request(Oid indexoid);
extern void tp_compaction_drop_request(Oid indexoid);
extern void tp_compaction_flush_requests(void);
extern void tp_compaction_reset_requests(void);
```

The callback contract is one function taking `regclass` and returning any
type; local effects are rolled back after invocation.

- [ ] **Step 1: Create the stacked PR 3 worktree**

Run:

```bash
git -C /home/azureuser/pg_textsearch_3 worktree add \
  /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-3 \
  -b background-compaction-3-dispatch background-compaction-2-api
```

- [ ] **Step 2: Add failing mode and transaction-lifecycle tests**

Copy the final POC `compaction_request.sql`, retaining tests for:

```text
invalid and quoted-empty callback names
background request deduplication
commit versus abort
savepoint rollback
dropped indexes
ordinary callback failure
callback disappearance after lookup
query cancellation
temporary indexes falling back to inline
PREPARE cleanup and same-backend reuse
```

Use a local test callback such as:

```sql
CREATE SEQUENCE request_calls;

CREATE FUNCTION pg_temp.record_compaction_request(idx regclass)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_catalog.nextval('request_calls');
END
$$;
```

Use the sequence because callback-local table writes are deliberately rolled
back by the dispatch shield.

- [ ] **Step 3: Run the regression to prove the GUCs are absent**

Run:

```bash
make installcheck
```

Expected: `compaction_request` fails because
`pg_textsearch.compaction_mode` and the callback implementation do not exist.

- [ ] **Step 4: Add request state and GUCs**

Extract the final POC request module and register exactly:

```c
DefineCustomEnumVariable(
        "pg_textsearch.compaction_mode",
        "Controls spill-time segment compaction",
        "inline compacts during the spilling transaction; background "
        "records a request for a later worker; off disables automatic "
        "compaction.",
        &tp_compaction_mode,
        TP_COMPACTION_INLINE,
        compaction_mode_options,
        PGC_SUSET,
        0,
        NULL,
        NULL,
        NULL);
```

and the syntax-checked
`pg_textsearch.compaction_request_function` string GUC. The check hook accepts
empty, one identifier, or a schema-qualified identifier and rejects empty
quoted components.

- [ ] **Step 5: Wire spill and transaction lifecycle**

In `src/access/build.c`, use:

```c
switch (tp_compaction_mode)
{
case TP_COMPACTION_INLINE:
    tp_maybe_compact_level(index_rel, 0);
    break;
case TP_COMPACTION_BACKGROUND:
    if (RelationUsesLocalBuffers(index_rel))
        tp_maybe_compact_level(index_rel, 0);
    else if (tp_compaction_needed(index_rel))
        tp_compaction_request(RelationGetRelid(index_rel));
    break;
case TP_COMPACTION_OFF:
    break;
}
```

Wire PRE_COMMIT and PRE_PREPARE to `tp_compaction_flush_requests()`. Wire
COMMIT, ABORT, PREPARE, and parallel transaction exits to
`tp_compaction_reset_requests()` after releasing index-local state.

- [ ] **Step 6: Preserve the protected callback error policy**

Extract the final two-level internal-subtransaction shield from
`tp_lookup_request_function()` and `tp_run_request()`. Keep this exact
classification:

```c
if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
    edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
    edata->sqlerrcode == ERRCODE_CRASH_SHUTDOWN)
    ReThrowError(edata);
```

All other callback errors emit a warning and allow the triggering transaction
to commit.

- [ ] **Step 7: Validate and open PR 3**

Run:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make format-check
git diff --check
git add Makefile src/access/build.c src/index/state.c src/memtable/log.c \
  src/mod.c src/index/compaction_request.c \
  src/index/compaction_request.h test/sql/compaction_request.sql \
  test/expected/compaction_request.out docs/background_compaction.md
git commit -m "feat: dispatch compaction requests at commit"
git push -u origin background-compaction-3-dispatch
gh pr create --repo timescale/pg_textsearch \
  --base background-compaction-2-api \
  --head background-compaction-3-dispatch \
  --title "Dispatch compaction requests at commit" \
  --body "Extracted from #471. This adds a scheduler-neutral callback seam; it contains no pg_durable dependency or operator role."
```

Expected: one PR whose tests use only local PostgreSQL callbacks.

### Task 5: Land and release the pg_durable node-failure policy

**Upstream dependency:** microsoft/pg_durable#354

**Files at the audited #354 head:**
- Modify: `src/dsl.rs`
- Modify: `src/lib.rs`
- Modify: `src/orchestrations/execute_function_graph.rs`
- Modify: `src/types.rs`
- Create: `sql/pg_durable--0.2.6--0.2.7.sql`
- Create: `tests/e2e/sql/68_failure_policy.sql`
- Create: `tests/e2e/sql/69_instance_activity.sql`
- Modify: `docs/api-reference.md`
- Create: `docs/spec-failure-policy.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: the existing two-argument
  `df.loop(body text, condition text DEFAULT NULL)`, activity-node errors,
  loop `continue_as_new`, and node-status monitoring
- Produces:

```sql
df.start(
    fut text,
    label text DEFAULT NULL,
    database text DEFAULT NULL,
    transaction_mode text DEFAULT 'caller',
    max_attempts integer DEFAULT 1,
    max_backoff interval DEFAULT '16 seconds',
    on_failure text DEFAULT 'fail'
) RETURNS text
```

`df.loop` remains two-argument. `max_attempts` includes the first attempt;
`on_failure` accepts only `fail` and `continue`.

- [ ] **Step 1: Preserve the audited dependency boundary**

Released pg_durable v0.2.6 and current `main` do not contain this capability.
The audited microsoft/pg_durable#354 head does. Do not change `df.loop`, do
not open a duplicate upstream PR, and do not merge pg_textsearch PR 4 or PR 5
from a pg_durable feature branch.

Record the #354 head reviewed by the integration work:

```bash
gh pr view 354 --repo microsoft/pg_durable \
  --json headRefOid,state,isDraft,url
```

Expected before the dependency is released: the PR head is auditable, but
released v0.2.6 and current `main` still expose only the old `df.start`.

- [ ] **Step 2: Audit the public SQL and compatibility contract**

Verify #354 keeps:

```sql
df.loop(body text, condition text DEFAULT NULL)
```

and replaces the four-argument `df.start` schema declaration with the
seven-argument signature above. The trailing defaults preserve the legacy
behavior: one attempt, then fail the instance. The old wrapper symbols remain
available for a new shared library running against an un-upgraded schema, and
the upgrade installs exactly one unambiguous `df.start` overload.

Reject `max_attempts < 1`, non-positive `max_backoff`, and any `on_failure`
other than `continue` or `fail`.

- [ ] **Step 3: Audit retry, continuation, and observability semantics**

Eligible `df.sql`, `df.http`, and `df.http_multipart` activity failures retry
with durable exponential delays starting at one second and capped by
`max_backoff`. Once attempts are exhausted:

```text
on_failure = fail      -> fail the durable instance
on_failure = continue  -> abandon the rest of the current enclosing loop
                          iteration and start the next iteration
```

With no enclosing loop, `continue` has nowhere to unwind and the instance
fails. Graph/configuration failures still fail immediately. The failed
activity node and its error remain observable even though the enclosing
instance stays running and a later loop iteration succeeds. Do not replace
that failed node with a success-shaped result or treat instance status alone
as a health signal.

- [ ] **Step 4: Require upstream unit, E2E, and upgrade coverage**

The #354 gate must cover:

```text
transient failure succeeds within max_attempts
exhausted retries continue to a later loop iteration
failed activity remains observable under a running instance
continue without an enclosing loop still fails
fail and all compatibility defaults remain fail-stop
argument validation
old-schema/new-library and in-flight-instance compatibility
```

Run from the pg_durable #354 worktree:

```bash
cargo fmt -p pg_durable -- --check
cargo build --features pg17
cargo clippy --features pg17
./scripts/test-unit.sh
./scripts/test-e2e-local.sh
./scripts/test-upgrade.sh
```

Expected: every command exits 0, including
`tests/e2e/sql/68_failure_policy.sql` and
`tests/e2e/sql/69_instance_activity.sql`.

- [ ] **Step 5: Gate pg_textsearch on a release and live probes**

After #354 merges, wait for a pg_durable release that contains the exact
seven-argument signature. Set the minimum version in pg_textsearch only after
the release is available. The operator preflight must verify both:

```sql
to_regprocedure(
    'df.start(text,text,text,text,integer,interval,text)') IS NOT NULL
to_regprocedure('df.loop(text,text)') IS NOT NULL
```

Run the pg_textsearch adapter and recurrence tests against the installed
release, not #354's branch. The dependency gate is complete only when a
failed activity is observable under a still-running loop and a later
iteration succeeds.

### Task 6: Extract PR 4 — pg_durable per-index adapter

**Files:**
- Create: `scripts/durable_compaction/01_setup_role.sql`
- Create: `scripts/durable_compaction/02_wrapper.sql`
- Create: `scripts/durable_compaction/README.md`
- Create: `test/scripts/durable_compaction.sh`
- Create: `test/scripts/fixtures/pg_config`
- Modify: `src/access/compaction.c`
- Modify: extension SQL scripts
- Modify: `Makefile`
- Modify: `docs/background_compaction.md`

**Interfaces:**
- Consumes: PR 3 callback contract and released pg_durable
  `df.start(text, text, text, text, integer, interval, text)` plus unchanged
  `df.loop(text, text)`
- Produces:

```sql
bm25_compact_step_if_current(oid, oid, oid, oid) RETURNS boolean
bm25_needs_compaction_if_current(oid, oid, oid, oid) RETURNS boolean
public.bm25_request_compaction(regclass) RETURNS text
```

- [ ] **Step 1: Create the PR 4 branch after the upstream release is known**

Run:

```bash
git -C /home/azureuser/pg_textsearch_3 worktree add \
  /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-4 \
  -b background-compaction-4-pg-durable \
  background-compaction-3-dispatch
```

Set the minimum pg_durable version in all three operator preflights to the
first released version containing #354. Verify
`df.start(text,text,text,text,integer,interval,text)` and
`df.loop(text,text)` exist before any operator-side mutation. Do not accept
v0.2.6, current `main`, or a build identified only by a private catalog
assumption.

- [ ] **Step 2: Add failing physical-target and live-adapter tests**

Extract tests 1-14 from the final POC durable script except all backstop
registration scenarios. The retained scenarios cover:

```text
role and wrapper preflight before side effects
exact role reuse and hostile-role rejection
exact SECURITY DEFINER wrapper identity
PUBLIC and grant-option rejection
committed SELECT 1 canary
transaction_mode => 'new'
max_attempts => 5
max_backoff => '16 seconds'
on_failure => 'continue'
transient activity retry and exhausted-attempt continuation
PREPARE dispatch
temporary-index fallback
hostile search_path
real stepped compaction
permissions, partitions, and stale targets
```

Run `make test-durable`; expect failure because the scripts and physical
target helpers do not exist.

- [ ] **Step 3: Add private physical-identity helpers**

Extract `tp_open_current_bm25_index()`,
`tp_compact_index_step_if_current()`, and
`tp_needs_compaction_if_current()` from final POC `src/access/compaction.c`.
Add both SQL definitions and immediately normalize their ACLs:

```sql
REVOKE ALL ON FUNCTION
    @extschema@.bm25_compact_step_if_current(oid, oid, oid, oid),
    @extschema@.bm25_needs_compaction_if_current(oid, oid, oid, oid)
FROM PUBLIC;
```

Retain the named-default-privilege cleanup block from the final POC in both
fresh and upgrade SQL.

- [ ] **Step 4: Extract role setup and wrapper**

Copy the final forms of `01_setup_role.sql` and `02_wrapper.sql` from
`c1bedf23`, not their introducing commits. Preserve their security checks and
adapt the submission call to the released #354 API:

```text
cluster-wide pg_shdepend ownership checks
LOGIN, privilege, credential, setting, and connection-limit checks
owner membership with INHERIT TRUE and SET FALSE
wrapper body hash and security-property authentication
fixed search_path and pg_catalog qualification
non-owner grant-option rejection
transaction_mode => 'new'
max_attempts => 5
max_backoff => '16 seconds'
on_failure => 'continue'
committed execution canary
```

The wrapper must submit:

```sql
df.start(
    df.loop(body, cond),
    label => 'bm25-compact-' || idx_oid,
    transaction_mode => 'new',
    max_attempts => 5,
    max_backoff => '16 seconds',
    on_failure => 'continue')
```

This allows a transient activity to retry in place. If all five attempts are
spent, the failed activity and error remain observable, the rest of that loop
iteration is abandoned, and the next stepped-compaction iteration can
re-evaluate physical index state.

Remove all backstop registration from this PR.

- [ ] **Step 5: Validate and open PR 4**

Run:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make test-durable
make format-check
git diff --check
```

Then commit and open the stacked PR:

```bash
git add Makefile src/access/compaction.c \
  sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql \
  scripts/durable_compaction/01_setup_role.sql \
  scripts/durable_compaction/02_wrapper.sql \
  scripts/durable_compaction/README.md \
  test/scripts/durable_compaction.sh test/scripts/fixtures/pg_config \
  docs/background_compaction.md
git commit -m "feat: add pg_durable compaction adapter"
git push -u origin background-compaction-4-pg-durable
gh pr create --repo timescale/pg_textsearch \
  --base background-compaction-3-dispatch \
  --head background-compaction-4-pg-durable \
  --title "Add pg_durable compaction adapter" \
  --body "Extracted from #471. Requires the released pg_durable seven-argument df.start failure policy from microsoft/pg_durable#354. This PR adds per-index submission only; the recurring backstop is separate."
```

### Task 7: Extract PR 5 — recovery backstop and operations

**Files:**
- Create: `scripts/durable_compaction/03_backstop.sql`
- Modify: `scripts/durable_compaction/README.md`
- Modify: `test/scripts/durable_compaction.sh`
- Modify: `src/access/compaction.c`
- Modify: extension SQL scripts
- Modify: `Makefile`
- Modify: `scripts/package-deb.sh`
- Modify: `.github/workflows/release.yml`
- Modify: `.github/workflows/package-release.yml`
- Modify: `docs/background_compaction.md`

**Interfaces:**
- Consumes: PR 4 role/wrapper and pg_durable
  `df.start(text, text, text, text, integer, interval, text)` plus unchanged
  `df.loop(body, condition)`
- Produces:

```sql
bm25_indexes_needing_compaction() RETURNS SETOF regclass
bm25_compact_pending() RETURNS integer
```

and one canonical database-wide recurring schedule.

- [ ] **Step 1: Create the stacked PR 5 worktree**

Run:

```bash
git -C /home/azureuser/pg_textsearch_3 worktree add \
  /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5 \
  -b background-compaction-5-backstop \
  background-compaction-4-pg-durable
```

- [ ] **Step 2: Add failing sweep and recurrence tests**

Add native SQL tests asserting:

```sql
SELECT bm25_compact_pending() = 1 AS pending_compacted_one;
SELECT NOT bm25_needs_compaction(
    'compaction_pending_idx'::regclass) AS pending_cleared;
```

Include two eligible indexes where one becomes inaccessible; assert a warning
for that index and successful compaction of the other.

Move the POC's ownership-drift/repair-sweep scenario into this PR alongside
the backstop tests.

Add a disposable durable loop whose first body invocation fails and whose
later invocation succeeds under:

```sql
df.start(
    df.loop(body, condition => NULL),
    label => 'bm25-compaction-backstop-policy-probe',
    max_attempts => 1,
    on_failure => 'continue')
```

Assert that there is exactly one attempt in the failed tick, the failed
activity node and error remain observable, the instance remains live, and a
later iteration succeeds.

- [ ] **Step 3: Add cross-index sweep APIs**

Extract only `bm25_indexes_needing_compaction()` and
`bm25_compact_pending()` from the final POC SQL. Preserve these filters:

```sql
am.amname = 'bm25'
AND c.relkind = 'i'
AND c.relpersistence <> 't'
AND i.indisvalid
AND i.indisready
```

Keep per-index exception isolation in `bm25_compact_pending()`, and retain the
caller-visibility/ownership rules from the final POC.

- [ ] **Step 4: Extract and adapt the canonical backstop**

Copy the final `03_backstop.sql` preflight, canary, advisory lock, canonical
graph discovery, duplicate rejection, registration, and post-commit
assertion. Preserve its dynamically schema-qualified `body` variable and
keep the two-argument loop while setting the released public policy on
`df.start`:

```sql
df.start(
    df.loop(
        df.wait_for_schedule(:'backstop_cron')
        OPERATOR(pg_catalog.~>) body,
        condition => NULL),
    label => 'bm25-compaction-backstop-v2',
    max_attempts => 1,
    on_failure => 'continue')
```

`max_attempts => 1` deliberately disables retry within a tick; the backstop
records that failed activity and advances to the next scheduled tick.

#354 carries the failure policy in orchestration input/history, not in the
LOOP node JSON or `df.instances`, so canonical discovery cannot authenticate
the policy by inspecting the graph. Use the new fixed
`bm25-compaction-backstop-v2` label as the policy-aware canonical identity.
Reuse only a live v2 instance with the exact expected graph, database, and
submitter. If any live same-database, same-submitter legacy
`bm25-compaction-backstop` instance exists, fail with its instance ID and
instruct the operator to cancel it before registering v2; do not silently
create overlapping schedules. Add legacy migration, duplicate-v2, and
idempotent v2 reuse cases to `test-durable`.

Do not add keyed schedule lifecycle or per-index metapage schedule IDs.

- [ ] **Step 5: Add packaging and operator documentation**

Install exactly:

```text
scripts/durable_compaction/01_setup_role.sql
scripts/durable_compaction/02_wrapper.sql
scripts/durable_compaction/03_backstop.sql
scripts/durable_compaction/README.md
docs/background_compaction.md
```

Keep the final POC's source, binary, and Debian package path assertions.
Document monitoring of failed nodes under a still-running recurring instance
with `df.instance_activity()` and `df.instance_nodes()`.

- [ ] **Step 6: Validate and open PR 5**

Run:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make test-durable
make format-check
git diff --check
```

Run the existing package-build jobs locally where their tooling is available,
or require both package workflows before merge. Then:

```bash
git add Makefile src/access/compaction.c \
  sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql \
  scripts/durable_compaction/03_backstop.sql \
  scripts/durable_compaction/README.md \
  test/scripts/durable_compaction.sh scripts/package-deb.sh \
  .github/workflows/release.yml \
  .github/workflows/package-release.yml \
  docs/background_compaction.md
git commit -m "feat: add durable compaction backstop"
git push -u origin background-compaction-5-backstop
gh pr create --repo timescale/pg_textsearch \
  --base background-compaction-4-pg-durable \
  --head background-compaction-5-backstop \
  --title "Add durable compaction recovery backstop" \
  --body "Extracted from #471. Adds cross-index debt discovery, failure-isolated sweep, failure-resilient recurrence, singleton registration, monitoring, and package assets."
```

### Task 8: Rebase the stack and retire the POC

**Files:**
- Modify remotely: bases and descriptions of the five extracted PRs
- Modify remotely: PR #471 description and state

**Interfaces:**
- Consumes: merged PRs 1-5 and released pg_durable dependency
- Produces: a linear history on `main` and a closed reference POC

- [ ] **Step 1: After each predecessor merges, rebase the next branch**

For PR 2 after PR 1 merges, and analogously for each later branch:

```bash
git fetch origin
git rebase origin/main
git push --force-with-lease
gh pr edit --repo timescale/pg_textsearch --base main
```

Run that PR's complete acceptance gate again after the rebase.

- [ ] **Step 2: Link the resulting PRs from the umbrella**

Fetch URLs by exact branch:

```bash
for branch in \
  background-compaction-1-engine \
  background-compaction-2-api \
  background-compaction-3-dispatch \
  background-compaction-4-pg-durable \
  background-compaction-5-backstop
do
  gh pr list --repo timescale/pg_textsearch \
    --head "$branch" --state all --json number,title,url \
    --jq '.[] | "- [#\(.number) \(.title)](\(.url))"'
done
```

Add the emitted list and the merged pg_durable PR link to PR #471.

- [ ] **Step 3: Verify final behavior on `main`**

Run from a fresh worktree at `origin/main`:

```bash
make
make install
make installcheck
test ! -s test/regression.diffs
make test-all
make test-durable
make format-check
git diff --check
```

Expected: all commands exit 0, including a failed recurring tick followed by
a successful later tick.

- [ ] **Step 4: Close the umbrella**

Run:

```bash
gh pr close 471 --repo timescale/pg_textsearch \
  --comment "Superseded by the linked focused PR series. The POC branch is preserved as the implementation and discussion reference."
```

Expected: PR #471 is closed without merging and continues to link all
replacement work.
