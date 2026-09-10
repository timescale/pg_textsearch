# Managed Background Compaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Historical note:** This plan records implementation against the
> pre-#483 documentation tree. References to `docs/background_compaction.md`
> are historical; current documentation lives in `README.md` and
> `ARCHITECTURE.md`.

**Goal:** Replace scheduler-neutral background callbacks and database-wide sweeps with one automatically managed, owner-scoped pg_durable workflow per physical BM25 index while preserving one transaction and lock-release point per merge pass.

**Architecture:** The spill path continues to record only transaction-local index OIDs while holding the index lock. A new `compaction_job` module owns optional pg_durable discovery, canonical labels, owner-scoped job admission/signaling, the sticky extension dependency, and workflow construction. Existing utility and object-access hooks reconcile job lifecycle after authorized DDL; private stale-safe compaction functions validate physical identity before every durable step.

**Tech Stack:** PostgreSQL 17/18 C extension APIs, PGXS, GenericXLog-backed BM25 storage, SPI, pg_durable v0.2.7 SQL DSL, Bash integration tests, pg_regress, GitHub Actions.

## Global Constraints

- Preserve `inline` as the default.
- Replace unreleased `off` with `manual`; do not retain an alias.
- `background` requires pg_durable v0.2.7 or newer at activation time.
- pg_durable remains an optional runtime dependency; do not add it to `pg_textsearch.control`.
- Default `pg_textsearch.background_compaction_schedule` to `*/5 * * * *`.
- Snapshot the default schedule when background mode is activated; later GUC changes do not reschedule an existing job.
- Reject background mode for temporary indexes; do not compact them inline as a fallback.
- Create one owner-scoped workflow per storage-bearing physical BM25 index; never create one for a partitioned parent.
- Drive the durable cascade from `bm25_compact_step()`'s actual result, never from `bm25_needs_compaction()`.
- Run every merge pass as a separate pg_durable SQL node and PostgreSQL transaction.
- Keep index pages and metapage counts authoritative; labels and workflows are accelerators.
- Add a sticky normal dependency from the `bm25` access-method object to pg_durable after the first successful background activation.
- Preserve ordinary writer commits when runtime signaling fails; rethrow cancellation and shutdown errors.
- Keep install and `1.4.0--1.5.0-dev` upgrade SQL exactly synchronized for new function signatures and ACLs.
- Use American English and wrap C and prose lines at 79 characters.
- Do not add Copilot attribution or commit trailers.

## File Structure

| File | Responsibility |
|---|---|
| `src/index/compaction_request.c` | Transaction-local spill request collection and PRE_COMMIT dispatch only. |
| `src/index/compaction_request.h` | Compaction mode enum and request-dispatch interface. |
| `src/index/compaction_job.c` | Optional pg_durable discovery, labels, graph construction, dependency pinning, owner-scoped admission, lookup, recovery, and signaling. |
| `src/index/compaction_job.h` | Narrow lifecycle and signal interfaces consumed by hooks and request dispatch. |
| `src/access/compaction_api.c` | Public compaction controls plus private physical-target step/current helpers. |
| `src/access/am.h`, `src/access/handler.c` | Parsed `compaction` and `compaction_schedule` reloptions. |
| `src/access/build.c` | Spill/build behavior for inline, background, and manual modes. |
| `src/mod.c` | GUC/reloption registration and post-utility lifecycle orchestration. |
| `sql/pg_textsearch--1.5.0-dev.sql` | Fresh-install public and private SQL function declarations. |
| `sql/pg_textsearch--1.4.0--1.5.0-dev.sql` | Upgrade declarations identical to fresh install. |
| `test/sql/compaction.sql`, `test/expected/compaction.out` | Scheduler-independent mode, API, stale-target, and ACL regression coverage. |
| `test/sql/compaction_request.sql`, `test/expected/compaction_request.out` | Transaction-local request bookkeeping without arbitrary callbacks. |
| `test/scripts/compaction_request_source.sh` | Static guards for no work under the index LWLock and safe PRE_COMMIT behavior. |
| `test/scripts/durable_compaction.sh` | Real pg_durable v0.2.7 lifecycle, ownership, transaction, and recovery tests. |
| `Makefile` | New source object and `test-durable` target. |
| `.github/workflows/ci.yml` | Pinned pg_durable v0.2.7 integration lane. |
| `README.md`, `docs/background_compaction.md` | User-facing mode, dependency, lifecycle, scheduling, and preview documentation. |

---

### Task 1: Establish the three-mode option contract

**Files:**
- Modify: `src/index/compaction_request.h`
- Modify: `src/index/compaction_request.c`
- Modify: `src/access/am.h`
- Modify: `src/access/handler.c`
- Modify: `src/access/build.c`
- Modify: `src/mod.c`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`

**Interfaces:**
- Produces: `TP_COMPACTION_INLINE`, `TP_COMPACTION_BACKGROUND`, and `TP_COMPACTION_MANUAL`.
- Produces: `char *tp_background_compaction_schedule`.
- Produces: `const char *tp_index_compaction_schedule(Relation index_rel)`, returning the explicit reloption or `NULL`.
- Consumes: existing `TpOptions` reloption parsing.

- [ ] **Step 1: Replace callback-focused option tests with failing mode tests**

In `test/sql/compaction_request.sql`, delete the callback GUC, callback
function, sequence-observation, callback error, search-path, and callback
reentry fixtures. Keep the per-index reloption fixture and replace its option
assertions with:

```sql
SHOW pg_textsearch.background_compaction_schedule;

CREATE INDEX relopt_bad_idx ON relopt_docs
    USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');

ALTER INDEX relopt_docs_idx SET (compaction = 'manual');
SELECT reloptions @> ARRAY['compaction=manual']
FROM pg_class WHERE oid = 'relopt_docs_idx'::regclass;

ALTER INDEX relopt_docs_idx
    SET (compaction_schedule = '17 * * * *');
SELECT reloptions @> ARRAY['compaction_schedule=17 * * * *']
FROM pg_class WHERE oid = 'relopt_docs_idx'::regclass;
```

Add a manual-mode spill fixture that creates two L0 segments at
`segments_per_level = 2` and asserts:

```sql
SELECT bm25_needs_compaction('manual_docs_idx'::regclass)
       AS manual_debt_remains;
```

- [ ] **Step 2: Run the focused regression and confirm the new vocabulary fails**

Run:

```bash
make install
make installcheck REGRESS="compaction_request"
```

Expected: failure because `manual`, `compaction_schedule`, and
`pg_textsearch.background_compaction_schedule` do not exist and `off` is
still accepted.

- [ ] **Step 3: Add the enum, GUC, and reloption storage**

Change `src/index/compaction_request.h` to:

```c
typedef enum TpCompactionMode
{
	TP_COMPACTION_INLINE = 0,
	TP_COMPACTION_BACKGROUND,
	TP_COMPACTION_MANUAL
} TpCompactionMode;

extern char *tp_background_compaction_schedule;
extern int tp_index_compaction_mode(Relation index_rel);
extern const char *tp_index_compaction_schedule(Relation index_rel);
```

Extend `TpOptions` in `src/access/am.h`:

```c
int32  compaction_schedule_offset;
int    compaction;
```

Add the matching `RELOPT_TYPE_STRING` parse entry in
`src/access/handler.c`. In `src/mod.c`, replace `off` with `manual`, remove
the callback GUC, register:

```c
DefineCustomStringVariable(
		"pg_textsearch.background_compaction_schedule",
		"Default schedule for managed background compaction.",
		NULL,
		&tp_background_compaction_schedule,
		"*/5 * * * *",
		PGC_SUSET,
		0,
		NULL,
		NULL,
		NULL);
```

Register `compaction_schedule` as a string reloption with
`ShareUpdateExclusiveLock`. Return its varlena-offset value from
`tp_index_compaction_schedule()`.

- [ ] **Step 4: Make build and spill policy honor manual mode**

In `src/access/build.c`:

```c
case TP_COMPACTION_MANUAL:
	break;
```

For serial and parallel CREATE INDEX batch flushes, compact only in inline
mode:

```c
if (tp_index_compaction_mode(index) == TP_COMPACTION_INLINE)
	tp_maybe_compact_level(index_state, index, 0);
```

Leave background spill request recording in place until Task 5 replaces its
dispatcher.

- [ ] **Step 5: Regenerate and inspect expected output**

Run:

```bash
make install
make installcheck REGRESS="compaction_request"
cp test/results/compaction_request.out test/expected/compaction_request.out
git --no-pager diff -- test/expected/compaction_request.out
```

Expected: `SHOW` returns `*/5 * * * *`, `off` is rejected, manual and
schedule reloptions are stored, and manual debt remains.

- [ ] **Step 6: Format, rerun, and commit**

Run:

```bash
make format
make format-check
make installcheck REGRESS="compaction_request"
git add src/index/compaction_request.h src/index/compaction_request.c \
  src/access/am.h src/access/handler.c src/access/build.c src/mod.c \
  test/sql/compaction_request.sql test/expected/compaction_request.out
git commit -m "Define managed compaction modes and schedule"
```

Expected: format and focused regression pass.

### Task 2: Add stale-safe private worker APIs

**Files:**
- Modify: `src/access/compaction_api.c`
- Modify: `sql/pg_textsearch--1.5.0-dev.sql`
- Modify: `sql/pg_textsearch--1.4.0--1.5.0-dev.sql`
- Modify: `test/sql/compaction.sql`
- Modify: `test/expected/compaction.out`

**Interfaces:**
- Produces: `bm25_compact_step_if_current(oid, oid, oid, oid, oid) RETURNS boolean`.
- Produces: `bm25_background_target_is_current(oid, oid, oid, oid, oid) RETURNS boolean`.
- Captured arguments: index OID, database OID, effective tablespace OID, relfilenumber, and owner OID.
- Invariant: both functions return false for a stale/missing target and never redirect work through OID reuse.

- [ ] **Step 1: Add failing physical-identity tests**

In `test/sql/compaction.sql`, capture a real identity:

```sql
SELECT d.oid AS db_oid,
       c.oid AS index_oid,
       coalesce(nullif(c.reltablespace, 0), d.dattablespace) AS spc_oid,
       pg_relation_filenode(c.oid) AS relfilenumber,
       c.relowner AS owner_oid
FROM pg_class c
JOIN pg_database d ON d.datname = current_database()
WHERE c.oid = 'compaction_step_idx'::regclass
\gset target_
```

Assert that the current helper returns true, a wrong database/tablespace/file
identity returns false, and a dropped index returns false. Use a dedicated
background fixture for `bm25_background_target_is_current()` and verify that
changing it to manual returns false.

As a nonowner role, assert both private functions fail with permission denied
before the SQL ACLs are revoked in the next step.

- [ ] **Step 2: Run the focused test and verify undefined-function failures**

Run:

```bash
make install
make installcheck REGRESS="compaction"
```

Expected: failure because the two five-argument functions do not exist.

- [ ] **Step 3: Refactor physical validation into one internal helper**

In `src/access/compaction_api.c`, add:

```c
typedef struct TpCompactionTarget
{
	Oid			  index_oid;
	Oid			  database_oid;
	Oid			  tablespace_oid;
	RelFileNumber relfilenumber;
	Oid			  owner_oid;
} TpCompactionTarget;

static Relation tp_open_current_bm25_target(
		const TpCompactionTarget *target,
		LOCKMODE				   lockmode,
		bool					   need_owner,
		bool					   need_background);
```

The function must:

1. return `NULL` when `database_oid != MyDatabaseId`;
2. precheck ownership before waiting for the relation lock;
3. use `try_relation_open()` so a missing target returns `NULL`;
4. verify BM25 AM, physical index relkind, non-temporary persistence,
   effective tablespace, relfilenumber, and owner under the lock;
5. optionally require `TP_COMPACTION_BACKGROUND`; and
6. recheck ownership after locking.

Implement the SQL entry points with `PG_FUNCTION_INFO_V1`:

```c
Datum tp_compact_index_step_if_current(PG_FUNCTION_ARGS);
Datum tp_background_target_is_current(PG_FUNCTION_ARGS);
```

The step function uses `RowExclusiveLock`, obtains the per-index
`LW_EXCLUSIVE` lock, calls `tp_compact_step()`, and returns its actual result.
The current function uses `AccessShareLock` and performs no writes.

- [ ] **Step 4: Declare private SQL functions in both extension scripts**

Add identical declarations:

```sql
CREATE FUNCTION @extschema@.bm25_compact_step_if_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION @extschema@.bm25_background_target_is_current(
    index_oid oid, database_oid oid, tablespace_oid oid,
    relfilenumber oid, owner_oid oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_background_target_is_current'
LANGUAGE C VOLATILE STRICT;

REVOKE ALL ON FUNCTION
    @extschema@.bm25_compact_step_if_current(oid, oid, oid, oid, oid)
    FROM PUBLIC;
REVOKE ALL ON FUNCTION
    @extschema@.bm25_background_target_is_current(oid, oid, oid, oid, oid)
    FROM PUBLIC;
```

- [ ] **Step 5: Update expected output and run owner/source guards**

Run:

```bash
make format
make install
make installcheck REGRESS="compaction"
cp test/results/compaction.out test/expected/compaction.out
make test-compaction-ownercheck
make format-check
```

Expected: current identity succeeds, every stale identity returns false,
manual mode is not current, and nonowners cannot execute private helpers.

- [ ] **Step 6: Commit the stale-safe worker boundary**

Run:

```bash
git add src/access/compaction_api.c \
  sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql \
  test/sql/compaction.sql test/expected/compaction.out
git commit -m "Add stale-safe background compaction steps"
```

### Task 3: Implement pg_durable admission and the stepped workflow

**Files:**
- Create: `src/index/compaction_job.c`
- Create: `src/index/compaction_job.h`
- Modify: `src/mod.c`
- Modify: `Makefile`
- Modify: `sql/pg_textsearch--1.5.0-dev.sql`
- Modify: `sql/pg_textsearch--1.4.0--1.5.0-dev.sql`
- Modify: `test/sql/compaction.sql`
- Modify: `test/expected/compaction.out`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`
- Create: `test/scripts/durable_compaction.sh`
- Modify: `test/scripts/compaction_request_source.sh`

**Interfaces:**
- Produces: `void tp_compaction_job_activate(Oid indexoid, bool refresh_default)`.
- Produces: `void tp_compaction_job_signal(Oid indexoid)`.
- `activate` is strict and raises ERROR; `signal` raises ordinary errors for its PRE_COMMIT caller to downgrade to WARNING.
- Uses the private five-OID functions from Task 2.

- [ ] **Step 1: Write the first failing pg_durable integration scenarios**

Create `test/scripts/durable_compaction.sh` by retaining the proven disposable
cluster helpers from `task2-bm25-compact:test/scripts/durable_compaction.sh`,
but remove all wrapper-role and callback setup. Configure:

```conf
shared_preload_libraries = 'pg_durable,pg_textsearch'
pg_durable.database = 'durable_compaction_test'
pg_durable.worker_role = 'postgres'
pg_durable.max_user_connections = 4
```

Create pg_durable before pg_textsearch. Add a failing CREATE test that:

1. creates a LOGIN owner and a table owned by that role;
2. creates a background BM25 index as the owner;
3. asserts one pending/running pg_durable instance with a
   `pg_textsearch:bg:v1:` label;
4. waits for the initial cascade to remove build-time debt;
5. verifies a normal dependency from the BM25 access method to pg_durable;
6. repeats CREATE in a transaction that rolls back and verifies that neither
   the job nor dependency survives in a fresh database.

Begin the script with reusable assertions:

```bash
active_jobs_for_index() {
    local oid=$1
    sql_super -c "SELECT count(*)
      FROM df.instances
      WHERE label LIKE 'pg_textsearch:bg:v1:%:${oid}:%'
        AND status IN ('pending', 'running');"
}

wait_for_no_debt() {
    local index_name=$1 timeout=$2
    local waited=0
    while [ "$waited" -lt "$timeout" ]; do
        if [ "$(sql_super -c "SELECT NOT bm25_needs_compaction(
                '${index_name}'::regclass);")" = "t" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "index ${index_name} retained compaction debt"
}
```

In core regression, add failing tests that background CREATE reports the
pg_durable requirement when the extension is absent and rejects a temporary
index before attempting activation.

Remove the database-wide `bm25_compact_pending()` definition from both SQL
scripts and remove its sweep fixtures from `compaction.sql`. Per-index managed
activation supersedes the sweep immediately; do not leave the normal
regression suite dependent on creating background indexes without
pg_durable.

Add source assertions that `compaction_job.c` contains:

```text
df.wait_for_signal
df.wait_for_schedule
bm25_compact_step_if_current
{sys_instance_id}
DEPENDENCY_NORMAL
AccessMethodRelationId
```

- [ ] **Step 2: Add the new module and build object**

Add `src/index/compaction_job.o` to `OBJS` in `Makefile`. Add:

```make
test-durable:
	@echo "Running managed pg_durable compaction tests..."
	@cd test/scripts && ./durable_compaction.sh
```

Add `test-durable` to `.PHONY`, but not to `test-shell`. Define in
`src/index/compaction_job.h`:

```c
#pragma once

#include <postgres.h>

extern void tp_compaction_job_activate(Oid indexoid, bool refresh_default);
extern void tp_compaction_job_signal(Oid indexoid);
```

Keep every pg_durable-specific catalog lookup and SQL string in
`compaction_job.c`; no pg_durable header may be included.

- [ ] **Step 3: Implement extension discovery and version validation**

Resolve `pg_durable` with `get_extension_oid("pg_durable", true)`, lock its
`pg_extension` object with `AccessShareLock`, recheck it, and read
`extversion`. Parse three numeric components and reject versions below 0.2.7.

Resolve `df.start(text,text,text,text)`, `df.signal(text,text,text)`,
`df.wait_for_signal(text,integer)`, and `df.wait_for_schedule(text)` by OID
and verify that each function belongs to the pg_durable extension through
`pg_depend`.
Do not trust `search_path` or a same-named user function.

Use errors with these stable leading messages:

```text
background compaction requires pg_durable 0.2.7 or newer
pg_durable is not initialized for this database
index owner must have LOGIN for background compaction
```

- [ ] **Step 4: Implement the sticky dependency**

Build these addresses:

```c
ObjectAddress bm25_am = {
	.classId = AccessMethodRelationId,
	.objectId = get_index_am_oid("bm25", false),
	.objectSubId = 0
};
ObjectAddress durable_ext = {
	.classId = ExtensionRelationId,
	.objectId = durable_extension_oid,
	.objectSubId = 0
};
```

Check for the exact existing normal dependency, then call:

```c
recordDependencyOn(&bm25_am, &durable_ext, DEPENDENCY_NORMAL);
CommandCounterIncrement();
```

The lock and dependency insert remain in the activation transaction. Never
remove this dependency based on current index count.

- [ ] **Step 5: Implement target capture and canonical labels**

Capture database, relation, effective tablespace, relfilenumber, owner, and
schedule under a relation lock. Build the label with concrete decimal fields and a hex-encoded schedule:

```text
pg_textsearch:bg:v1:16384:24576:1663:32768:16385:2a2f35202a202a202a202a
```

Build a family prefix through the relfilenumber field for generation lookup. Hex
encode the complete schedule rather than hashing it.

Take `pg_advisory_xact_lock(database_oid, index_oid)` before searching or
creating a job. Under the index owner's pg_durable RLS, select nonterminal
instances by exact label ordered by `created_at DESC, id DESC`. Warn if more
than one exists and select the first deterministically.

- [ ] **Step 6: Build the result-driven workflow graph**

Construct fixed SQL for:

```sql
SELECT @extschema@.bm25_compact_step_if_current(
    index_oid, database_oid, tablespace_oid, relfilenumber, owner_oid)
       AS ran
```

Name that result `step`, then branch:

```sql
$step.ran ?> 'SELECT true' !> df.break('false')
```

Wrap it in an inner `df.loop`. The outer workflow:

1. checks `bm25_background_target_is_current(...)` and that
   `{sys_instance_id}` is the newest nonterminal workflow for its physical
   family;
2. runs the inner cascade immediately;
3. races `df.wait_for_signal('compact', NULL)` against
   `df.wait_for_schedule(schedule)`;
4. repeats the target/current-instance check and cascade; and
5. calls `df.break('stale')` when it is no longer current.

Submit with:

```sql
df.start(graph, label, current_database(), 'caller')
```

All identifiers come from extension catalogs and are quoted; all OIDs are
formatted as unsigned integers; schedule and label values are SPI
parameters.

- [ ] **Step 7: Implement activation, recovery, and signaling**

Implement the public module functions around the admission primitives:

```c
void
tp_compaction_job_activate(Oid indexoid, bool refresh_default)
{
	/* Capture and lock the target, validate pg_durable, pin the
	 * dependency, enter owner identity, and reuse or start the job. */
}

void
tp_compaction_job_signal(Oid indexoid)
{
	/* Enter owner identity, recover a previously managed terminal
	 * generation if needed, and signal the exact current instance. */
}
```

When `refresh_default` is true, use an explicit `compaction_schedule` or
capture the current GUC and create/reuse that exact label. When it is false,
recover the complete schedule from the newest terminal managed label for the
same physical family. If no current or historical managed label exists,
raise:

```text
background compaction for index "documents_idx" requires explicit adoption
```

Signal exactly:

```sql
SELECT df.signal(instance_id, 'compact', '{}')
```

After successful explicit activation, emit:

```text
pg_textsearch background compaction is a preview feature
```

The warning detail states that pg_durable v0.2.7 jobs fail permanently on a
node error, receive no autonomous idle recovery, and stop after 100,000 loop
iterations.

- [ ] **Step 8: Implement scoped owner execution**

Wrap pg_durable SPI operations with:

```c
GetUserIdAndSecContext(&save_userid, &save_sec_context);
SetUserIdAndSecContext(owner_oid,
					   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
PG_TRY();
{
	/* fixed SPI operation */
}
PG_FINALLY();
{
	SetUserIdAndSecContext(save_userid, save_sec_context);
}
PG_END_TRY();
```

Activation may use this only after PostgreSQL has authorized index DDL.
Signal lookup may use it only for an OID recorded by the internal spill path.

- [ ] **Step 9: Wire basic CREATE activation**

Extend `tp_process_utility()` so a successful physical BM25 `IndexStmt`
reopens the created index after standard utility processing and calls:

```c
if (tp_index_compaction_mode(index_rel) == TP_COMPACTION_BACKGROUND)
	tp_compaction_job_activate(indexoid, true);
```

Call activation only after the index is valid and ready. Reject local-buffer
temporary indexes with:

```text
background compaction is not supported for temporary indexes
```

Leave partition-leaf enumeration and non-CREATE lifecycle commands to Task 4.

- [ ] **Step 10: Build and run focused behavioral tests**

Run:

```bash
make format
make install
make installcheck REGRESS="compaction compaction_request"
make test-compaction-request-source
make test-durable
make format-check
```

Expected: a normal build remains independent of pg_durable; background CREATE
fails clearly without it; the disposable pg_durable cluster creates one
owner job, drains the initial cascade, and rolls activation/dependency back
with CREATE while emitting the preview warning on successful activation.

- [ ] **Step 11: Commit the adapter core**

Run:

```bash
git add Makefile src/index/compaction_job.c src/index/compaction_job.h \
  src/mod.c sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql test/sql/compaction.sql \
  test/expected/compaction.out test/sql/compaction_request.sql \
  test/expected/compaction_request.out test/scripts/durable_compaction.sh \
  test/scripts/compaction_request_source.sh
git commit -m "Add managed pg_durable compaction jobs"
```

### Task 4: Wire transactional DDL lifecycle

**Files:**
- Modify: `src/mod.c`
- Modify: `src/index/compaction_job.c`
- Modify: `src/index/compaction_job.h`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Consumes: `tp_compaction_job_activate(Oid, bool)` from Task 3.
- Produces: post-utility reconciliation for CREATE INDEX, reloption ALTER, ALTER OWNER, and REINDEX.
- Invariant: activation commits or rolls back with the DDL.

- [ ] **Step 1: Add failing ALTER activation tests**

In core regression, assert a rolled-back ALTER leaves prior reloptions:

```sql
BEGIN;
ALTER INDEX relopt_docs_idx SET (compaction = 'background');
ROLLBACK;
SELECT reloptions @> ARRAY['compaction=manual']
FROM pg_class WHERE oid = 'relopt_docs_idx'::regclass;
```

Expected: the existing manual setting remains.

- [ ] **Step 2: Add failing activation and dependency tests to the shell suite**

Add cases that:

1. create an owner role and a manual BM25 index;
2. begin, ALTER it to background, observe one pending/running instance, then
   roll back and observe no instance or dependency;
3. commit the same ALTER and observe one instance;
4. verify a second identical ALTER reuses that instance;
5. verify `DROP EXTENSION pg_durable` fails; and
6. in a disposable second database, verify
   `DROP EXTENSION pg_durable CASCADE` also removes pg_textsearch.

- [ ] **Step 3: Classify relevant utility commands before delegation**

In `tp_process_utility()`, recognize:

- BM25 `IndexStmt`;
- `AlterTableStmt` with `OBJECT_INDEX` and `AT_SetRelOptions`,
  `AT_ResetRelOptions`, or `AT_ChangeOwner`;
- `AlterOwnerStmt` for `OBJECT_INDEX`; and
- every `ReindexStmt` kind.

Resolve and save affected OIDs before calling the previous hook where the
command may replace storage. Call the previous hook exactly once. After it
returns successfully, enumerate storage-bearing BM25 indexes affected by the
command and reconcile them.

For reloption ALTER, inspect `DefElem` names so only changes involving
`compaction` or `compaction_schedule` refresh the captured default. RENAME,
SET TABLESPACE, and unrelated reloptions must not reschedule a job.

- [ ] **Step 4: Reconcile CREATE and partition leaves**

After CREATE INDEX returns, resolve the created index and walk
`pg_inherits` for a partitioned parent. Call activation only for valid,
ready, live physical BM25 indexes whose mode is background.

For CREATE INDEX CONCURRENTLY, perform this after the final phase has made the
index valid and ready. Reject temporary background indexes before attempting
pg_durable activation.

- [ ] **Step 5: Reconcile ALTER OWNER and REINDEX**

After ALTER OWNER, read the new `relowner` with `SnapshotSelf` and activate as
that owner. The old workflow remains untouched and exits through its target
check.

For REINDEX INDEX/TABLE/SCHEMA/DATABASE/SYSTEM:

1. snapshot the affected BM25 OIDs before standard utility execution;
2. after success, reopen surviving OIDs;
3. capture each new relfilenumber;
4. activate a replacement only when its mode is background.

For REINDEX CONCURRENTLY, enumerate the final valid indexes after PostgreSQL
has swapped identities; never activate a transient replacement index.

- [ ] **Step 6: Run focused core and durable tests**

Run:

```bash
make format
make install
make installcheck REGRESS="compaction_request"
make test-durable
make format-check
```

Expected: no-pg_durable and temporary cases fail clearly; committed activation
creates one job and the sticky dependency; rollback leaves neither.

- [ ] **Step 7: Commit lifecycle wiring**

Run:

```bash
git add src/mod.c src/index/compaction_job.c \
  src/index/compaction_job.h test/sql/compaction_request.sql \
  test/expected/compaction_request.out \
  test/scripts/durable_compaction.sh
git commit -m "Manage background compaction job lifecycle"
```

### Task 5: Replace callback dispatch with managed signaling

**Files:**
- Modify: `src/index/compaction_request.c`
- Modify: `src/index/compaction_request.h`
- Modify: `src/index/compaction_job.c`
- Modify: `src/access/build.c`
- Modify: `src/mod.c`
- Modify: `test/scripts/compaction_request_source.sh`
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Consumes: `tp_compaction_job_signal(Oid)` from Task 3.
- Preserves: `tp_compaction_request(Oid)` performs only deduplicated list append under the LWLock.
- Produces: PRE_COMMIT owner-scoped exact-instance signaling with warning-only ordinary failure.

- [ ] **Step 1: Port transaction cases into failing managed-request tests**

In `durable_compaction.sh`, port the former callback suite's assertions for:

- no dispatch below the threshold;
- one request per index per transaction;
- top-level abort;
- savepoint rollback retaining physical spill debt;
- committed DROP removing the pending target;
- rolled-back DROP preserving it; and
- PREPARE TRANSACTION performing no pg_durable SQL.

Create debt in a writer transaction, commit, and assert the owner's existing
workflow wakes before the five-minute schedule. Keep
`compaction_request.sql` focused on the mode/reloption contract because the
normal pg_regress cluster intentionally has no pg_durable installation.

- [ ] **Step 2: Run tests and verify callback assumptions fail**

Run:

```bash
make installcheck REGRESS="compaction_request"
make test-compaction-request-source
make test-durable
```

Expected: failures refer to removed callback behavior or absent managed
signaling.

- [ ] **Step 3: Simplify request dispatch**

Delete:

- `tp_compaction_request_function`;
- `tp_check_compaction_request_function()`;
- callback name parsing;
- arbitrary callback execution;
- callback reentry handling; and
- callback-specific double-subtransaction rollback.

Keep the TopTransactionContext list and reset callback. Implement:

```c
void
tp_compaction_flush_requests(void)
{
	List *pending = tp_pending_compactions;

	tp_pending_compactions = NIL;
	foreach_oid(indexoid, pending)
		tp_signal_one_request(indexoid);
	list_free(pending);
}
```

`tp_signal_one_request()` calls `tp_compaction_job_signal(indexoid)` in an
internal subtransaction. It rethrows `QUERY_CANCELED`, `ADMIN_SHUTDOWN`, and
`CRASH_SHUTDOWN`; other errors become:

```text
bm25: could not signal background compaction for index "documents_idx": reason
```

Do not dispatch during PRE_PREPARE, parallel workers, recovery, or autovacuum.

- [ ] **Step 4: Remove every hidden inline fallback**

In `tp_do_spill()`:

```c
case TP_COMPACTION_BACKGROUND:
	if (tp_compaction_needed(index_rel))
		tp_compaction_request(RelationGetRelid(index_rel));
	break;
case TP_COMPACTION_MANUAL:
	break;
```

Background is already rejected for temporary indexes at activation. If an
unsupported backend reaches this branch, retain debt and let the schedule
repair it rather than compacting inline.

- [ ] **Step 5: Test immediate cascade and transaction boundaries**

In `durable_compaction.sh`:

1. set `segments_per_level = 2`;
2. create enough segments for a multi-level cascade;
3. commit the triggering writer transaction;
4. assert the job wakes without waiting for the cron schedule;
5. inspect pg_durable node/execution history or server execution IDs to show
   multiple durable step nodes;
6. record `txid_current()` from a test audit wrapper around the fixed private
   step and assert distinct transaction IDs; and
7. run a concurrent writer between step generations to prove the per-index
   lock is released.

The audit wrapper exists only in the disposable test database and delegates
to the private function as the index owner; production graph SQL remains
fixed.

- [ ] **Step 6: Update source guards**

Assert:

- `tp_compaction_request()` contains no SPI or relation open;
- pending OIDs still live in `TopTransactionContext`;
- PRE_PREPARE does not flush;
- `tp_compaction_flush_requests()` calls only
  `tp_compaction_job_signal()`; and
- no `compaction_request_function` symbol remains under `src/`.

- [ ] **Step 7: Run focused tests and commit**

Run:

```bash
make format
make install
make installcheck REGRESS="compaction_request"
make test-compaction-request-source
make test-durable
make format-check
git add src/index/compaction_request.c src/index/compaction_request.h \
  src/index/compaction_job.c src/access/build.c src/mod.c \
  test/scripts/compaction_request_source.sh \
  test/scripts/durable_compaction.sh
git commit -m "Signal managed compaction jobs at commit"
```

### Task 6: Complete lifecycle, security, and failure integration coverage

**Files:**
- Modify: `src/index/compaction_job.c`
- Modify: `src/mod.c`
- Modify: `test/scripts/durable_compaction.sh`
- Modify: `Makefile`

**Interfaces:**
- Hardens: label admission and owner switching from Tasks 3-5.
- Completes the scenarios run by: `make test-durable`.
- Invariant: stale or unauthorized workflows cannot mutate a replacement or another owner's index.

- [ ] **Step 1: Add failing ownership and privilege cases**

Add owner and writer roles. Grant the writer INSERT on the indexed table but
no index ownership and no direct private-helper access. Assert:

- writer inserts wake the owner's workflow;
- `submitted_by` in `df.instances` is the index owner;
- writer cannot execute either private helper;
- writer cannot signal another owner's instance directly through RLS;
- a NOLOGIN owner cannot enable background mode; and
- a superuser owner receives pg_durable's policy error unless
  `pg_durable.enable_superuser_instances` is enabled.

- [ ] **Step 2: Add failing identity and lifecycle cases**

Cover:

- committed and rolled-back DROP;
- drop/recreate with the same name;
- ordinary and concurrent REINDEX;
- OID/relfilenumber mismatch;
- background to manual and background to inline;
- schedule replacement;
- reapplying background after changing the GUC;
- ALTER OWNER;
- partitioned CREATE producing one job per physical leaf; and
- adding a new partition/index after the parent exists; and
- an unlogged background index completing work before and after a clean
  restart, subject to PostgreSQL's normal crash-reset semantics.

For each stale workflow, wait for its next scheduled/signal activity and
assert terminal completion without a mutation to the replacement index.

- [ ] **Step 3: Add failing duplicate and recovery cases**

Run concurrent spill transactions against one index and assert one
nonterminal canonical job. Insert one accidental duplicate with the same
label through pg_durable, trigger reconciliation, assert a warning, and assert
only the deterministic current instance remains active after a wake.

Force a workflow node failure, then:

1. verify an idle index remains failed;
2. perform another spill;
3. verify one replacement workflow appears; and
4. verify the replacement drains the debt.

Delete all history for a separate managed index and verify its next spill
warns that explicit re-adoption is required rather than silently capturing
the current GUC.

Query `pg_depend` and assert the dependency's depender is the `bm25` row in
`pg_am`, not the pg_textsearch row in `pg_extension`. This is the invariant
that prevents PostgreSQL's extension-update prerequisite reconciliation from
removing the conditional dependency.

- [ ] **Step 4: Add the scheduled backstop case**

Create debt while preventing the PRE_COMMIT signal from reaching the
workflow, restore normal operation, and wait for a one-minute test schedule.
Assert the same inner stepped cascade drains the debt without a new write.

- [ ] **Step 5: Harden implementation until every case passes**

Use relation locks around every identity read, transaction advisory locks
around admission, exact extension-owned function OIDs for pg_durable calls,
and `PG_TRY`/`PG_FINALLY` around every user-ID switch.

Do not add a pg_textsearch metadata table, a central compactor role, a
SECURITY DEFINER request function, or database-wide catalog sweep.

- [ ] **Step 6: Run and commit the completed integration**

Run:

```bash
make format
make install
make test-durable
make test-compaction-request-source
make test-compaction-ownercheck
make format-check
git add src/index/compaction_job.c src/mod.c \
  test/scripts/durable_compaction.sh
git commit -m "Cover durable compaction lifecycle and recovery"
```

### Task 7: Add the pinned pg_durable CI lane

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `make test-durable` from Task 6.
- Pins: pg_durable `v0.2.7`, cargo-pgrx `0.16.1`.
- Keeps: normal PostgreSQL 17/18 jobs free of pg_durable.

- [ ] **Step 1: Add the integration job**

Add one Ubuntu PostgreSQL 17 job named `durable-compaction` with paths already
covered by the workflow trigger. Install PostgreSQL server development
packages, Rust, OpenSSL/build prerequisites, and:

```bash
cargo install cargo-pgrx --version 0.16.1 --locked
cargo pgrx init --pg17 /usr/lib/postgresql/17/bin/pg_config
git clone --depth 1 --branch v0.2.7 \
  https://github.com/microsoft/pg_durable.git /tmp/pg_durable
make -C /tmp/pg_durable \
  PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make -C /tmp/pg_durable install \
  PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
```

Build/install pg_textsearch with `-Werror`, then run:

```bash
export PATH="/usr/lib/postgresql/17/bin:$PATH"
make test-durable
```

Set a timeout of 30 minutes and print the preserved disposable-cluster log on
failure.

- [ ] **Step 2: Validate workflow syntax and local YAML diff**

Run:

```bash
git diff --check
sed -n '/durable-compaction:/,/^[^ ]/p' .github/workflows/ci.yml
```

Expected: a single isolated optional-dependency job with the exact v0.2.7 and
0.16.1 pins.

- [ ] **Step 3: Commit the CI lane**

Run:

```bash
git add .github/workflows/ci.yml
git commit -m "Test managed compaction with pg_durable"
```

### Task 8: Finalize SQL surface and documentation

**Files:**
- Modify: `sql/pg_textsearch--1.5.0-dev.sql`
- Modify: `sql/pg_textsearch--1.4.0--1.5.0-dev.sql`
- Modify: `README.md`
- Modify: `docs/background_compaction.md`
- Modify: `test/sql/compaction.sql`
- Modify: `test/expected/compaction.out`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`

**Interfaces:**
- Confirms removal: `bm25_compact_pending()`.
- Removes: `pg_textsearch.compaction_request_function`.
- Documents: managed background and scheduler-neutral manual modes.

- [ ] **Step 1: Add failing absence and upgrade assertions**

Add regression queries:

```sql
SELECT to_regprocedure('bm25_compact_pending()') IS NULL
       AS no_database_wide_sweep;
SELECT current_setting(
           'pg_textsearch.compaction_request_function', true) IS NULL
       AS no_callback_guc;
```

Keep install and upgrade SQL comparison coverage that checks private function
signatures and revoked PUBLIC ACLs.

- [ ] **Step 2: Verify the sweep is absent from both SQL scripts**

Confirm the Task 3 removal remains synchronized:

```bash
diff -u \
  <(sed -n '/CREATE FUNCTION @extschema@.bm25_level_counts/,/COMMENT ON FUNCTION @extschema@.bm25_needs_compaction/p' sql/pg_textsearch--1.5.0-dev.sql) \
  <(sed -n '/CREATE FUNCTION @extschema@.bm25_level_counts/,/COMMENT ON FUNCTION @extschema@.bm25_needs_compaction/p' sql/pg_textsearch--1.4.0--1.5.0-dev.sql)
```

Expected: no difference.

- [ ] **Step 3: Rewrite user documentation**

In `README.md`:

- list inline/background/manual;
- list `compaction_schedule` and the default-schedule GUC;
- remove callback and sweep examples;
- describe pg_durable as optional until first background activation;
- explain the sticky dependency and DROP behavior; and
- retain direct `bm25_compact_step()` examples for manual mode.

Rewrite `docs/background_compaction.md` around:

1. transaction-per-batch semantics;
2. per-index owner-scoped workflow;
3. immediate signal plus scheduled backstop;
4. snapshot schedule behavior;
5. CREATE/ALTER/OWNER/REINDEX/DROP lifecycle;
6. manual pg_cron example:

```sql
SELECT cron.schedule(
    'documents-bm25-compaction',
    '*/5 * * * *',
    $$SELECT public.bm25_compact_step(
          'documents_body_idx'::regclass)$$
);
```

7. pg_durable v0.2.7 preload, LOGIN, superuser, fail-stop, no-idle-recovery,
   and 100,000-iteration limitations; and
8. explicit adoption after upgrade.

- [ ] **Step 4: Run focused regressions and commit**

Run:

```bash
make install
make installcheck REGRESS="compaction compaction_request"
make format-check
git diff --check
git add sql/pg_textsearch--1.5.0-dev.sql \
  sql/pg_textsearch--1.4.0--1.5.0-dev.sql README.md \
  docs/background_compaction.md test/sql/compaction.sql \
  test/expected/compaction.out test/sql/compaction_request.sql \
  test/expected/compaction_request.out
git commit -m "Document managed background compaction"
```

### Task 9: Verify the complete PR rewrite

**Files:**
- Review: all files changed from `origin/main`
- Update if required: `/home/azureuser/.copilot/session-state/aec34288-ddf3-4022-8f54-c14397a087cc/files/pr478-sweep-body.md`

**Interfaces:**
- Produces: a clean, tested branch and current PR description.

- [ ] **Step 1: Build and run targeted tests**

Run:

```bash
make clean
make
make format-check
make install
make installcheck REGRESS="compaction compaction_request"
make test-compaction-ownercheck
make test-compaction-request-source
make test-durable
```

Expected: every command exits zero and `test/regression.diffs` is absent or
empty.

- [ ] **Step 2: Run the repository-required suite**

Run:

```bash
make installcheck
test ! -s test/regression.diffs
make test-all
```

Expected: all 77 SQL regressions and every existing shell suite pass, followed
by the dedicated pg_durable suite.

- [ ] **Step 3: Inspect the final diff for forbidden remnants**

Run:

```bash
rg -n "TP_COMPACTION_OFF|compaction_request_function|bm25_compact_pending" \
  src sql test README.md docs
git --no-pager diff --check origin/main...HEAD
git status --short
```

Expected: `rg` finds only intentional upgrade/removal assertions or no
matches, diff check is clean, and the worktree contains only intended files.

- [ ] **Step 4: Review security and lifecycle invariants**

Read the final diff and confirm:

- no user-controlled SQL reaches the durable graph;
- every owner switch restores identity through `PG_FINALLY`;
- every physical mutation revalidates owner and identity under relation lock;
- dependency pinning is transactional and idempotent;
- no pg_durable call occurs under the per-index LWLock;
- every durable cascade is driven by the step result;
- DROP rollback never cancels a valid job; and
- ordinary signal failure cannot abort a writer commit.

- [ ] **Step 5: Replace the PR description**

Rewrite the session PR body artifact to describe only the final implementation:

```text
## Summary
- manage one owner-scoped pg_durable compaction workflow per background BM25 index
- run each compaction pass in its own durable transaction, with immediate spill signals and a scheduled backstop
- add manual mode for pg_cron and other owner-managed schedulers

## Compatibility
- pg_durable remains optional until background mode is first enabled
- background mode is preview and requires pg_durable 0.2.7 or newer
```

Include the exact tests run and the v0.2.7 limitations. Remove all sweep and
callback language.

- [ ] **Step 6: Commit any final test/document corrections**

If Step 1-5 changed tracked files:

```bash
git add -u
git commit -m "Finalize managed background compaction"
```

If no tracked files changed, do not create an empty commit.
