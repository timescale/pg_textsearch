# Background Compaction Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the transaction, compaction, privilege, benchmark, and
packaging defects found in PR #471 while preserving its background-compaction
POC scope.

**Architecture:** On-disk level counts are the durable record of compaction
need. Immediate pg_durable requests are best-effort accelerators, started in an
independent transaction after a spill reaches threshold; the periodic backstop
repairs anything the hot path misses. Core C helpers decide whether work is
needed and compact all eligible levels, while setup SQL enforces a narrow,
explicit privilege boundary.

**Tech Stack:** PostgreSQL 17/18 extension C APIs, PGXS regression tests,
PL/pgSQL, Bash, pg_durable 0.2.6, GitHub Actions, Debian packaging.

## Global Constraints

- Physical `GenericXLog` spill mutations are not logically undone on rollback.
- `transaction_mode => 'new'` is used only for the best-effort accelerator.
- Explicit rollback before PRE_COMMIT relies on level counts and the backstop.
- Temporary indexes compact inline because workers cannot open another
  backend's temporary relations.
- Review comments and code comments stay concise.
- Do not change the per-batch `LW_EXCLUSIVE` locking model in this PR.
- Support PostgreSQL 17 and 18.

---

### Task 1: Repair Compaction Level Selection and Read-Only Enforcement

**Files:**
- Modify: `test/sql/compaction.sql`
- Modify: `test/expected/compaction.out`
- Modify: `src/segment/merge.c`
- Modify: `src/access/compaction.c`

**Interfaces:**
- Consumes: `tp_compact_step(Relation index) -> bool`.
- Produces: `tp_maybe_compact_level(Relation index, uint32 level)` scans past
  under-threshold lower levels; `bm25_compact*` reject permanent-index writes
  in read-only transactions.

- [ ] **Step 1: Add failing higher-level and read-only regression cases**

After staging four L0 segments for `compaction_step_b_idx`, add:

```sql
SELECT bm25_compact_step('compaction_step_b_idx'::regclass)
       AS full_stage_step1;
SELECT bm25_compact_step('compaction_step_b_idx'::regclass)
       AS full_stage_step2;
SELECT (bm25_level_counts('compaction_step_b_idx'::regclass))[1] < 2
       AND
       (bm25_level_counts('compaction_step_b_idx'::regclass))[2] >= 2
       AS full_stage_higher_only;
SELECT bm25_compact('compaction_step_b_idx'::regclass);
SELECT NOT bm25_needs_compaction('compaction_step_b_idx'::regclass)
       AS full_compact_clears_higher_level;

BEGIN READ ONLY;
SELECT bm25_compact('compaction_step_a_idx'::regclass);
ROLLBACK;
BEGIN READ ONLY;
SELECT bm25_compact_step('compaction_step_a_idx'::regclass);
ROLLBACK;
```

Update `test/expected/compaction.out` with the intended booleans and
`cannot execute ... in a read-only transaction` errors.

- [ ] **Step 2: Run the targeted regression test and verify RED**

Run:

```bash
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test compaction
```

Expected: `full_compact_clears_higher_level` is false and the read-only calls
do not emit the expected errors.

- [ ] **Step 3: Make whole-cascade traversal visit every eligible level**

Change `tp_maybe_compact_level()` so an under-threshold current level skips its
merge loop but still recurses:

```c
if (level_count >= (uint16)tp_segments_per_level)
{
    while (level_count >= (uint16)tp_segments_per_level)
    {
        if (tp_merge_level_segments(
                    index, level, (uint32)tp_segments_per_level) ==
            InvalidBlockNumber)
            break;

        metabuf = ReadBuffer(index, 0);
        LockBuffer(metabuf, BUFFER_LOCK_SHARE);
        metapage = BufferGetPage(metabuf);
        metap = (TpIndexMetaPage)PageGetContents(metapage);
        level_count = metap->level_counts[level];
        UnlockReleaseBuffer(metabuf);
    }
}

tp_maybe_compact_level(index, level + 1);
```

- [ ] **Step 4: Enforce read-only mode for permanent indexes**

Immediately after `tp_open_bm25_index()` in both mutators:

```c
if (!RelationUsesLocalBuffers(index_rel))
    PreventCommandIfReadOnly("bm25 index compaction");
```

The relation must be closed if the code structure changes before this check;
use the existing error cleanup pattern rather than adding a broad catch.

- [ ] **Step 5: Run targeted tests and formatting**

Run:

```bash
make format-single FILE=src/segment/merge.c
make format-single FILE=src/access/compaction.c
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test compaction
make format-check
```

Expected: `compaction` passes and formatting is clean.

- [ ] **Step 6: Commit**

```bash
git add src/segment/merge.c src/access/compaction.c \
  test/sql/compaction.sql test/expected/compaction.out
git commit -m "fix: compact stranded levels safely"
```

---

### Task 2: Gate Requests and Keep Temporary Indexes Local

**Files:**
- Modify: `src/segment/merge.h`
- Modify: `src/segment/merge.c`
- Modify: `src/access/build.c`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`

**Interfaces:**
- Produces: `bool tp_compaction_needed(Relation index)`.
- Consumes: existing per-index `LW_EXCLUSIVE` lock held by `tp_do_spill()`.

- [ ] **Step 1: Add failing request-threshold regression coverage**

Add a dedicated index and verify the first spill creates no request while the
second reaches threshold:

```sql
SET pg_textsearch.segments_per_level = 2;
TRUNCATE compaction_log;

CREATE TABLE compaction_threshold_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_threshold_docs_idx ON compaction_threshold_docs
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_threshold_docs (body)
SELECT 'threshold first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_threshold_docs_idx') IS NOT NULL;
SELECT count(*) AS below_threshold_requests FROM compaction_log;

INSERT INTO compaction_threshold_docs (body)
SELECT 'threshold second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_threshold_docs_idx') IS NOT NULL;
SELECT count(*) AS threshold_requests FROM compaction_log
WHERE idx = 'compaction_threshold_docs_idx'::regclass;
```

Expected output: `below_threshold_requests = 0`,
`threshold_requests = 1`.

- [ ] **Step 2: Add failing temporary-index regression coverage**

```sql
TRUNCATE compaction_log;
CREATE TEMP TABLE compaction_temp_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_temp_docs_idx ON compaction_temp_docs
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_temp_docs (body)
SELECT 'temp first ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_temp_docs_idx') IS NOT NULL;
INSERT INTO compaction_temp_docs (body)
SELECT 'temp second ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_temp_docs_idx') IS NOT NULL;

SELECT count(*) AS temp_background_requests FROM compaction_log;
SELECT NOT bm25_needs_compaction('compaction_temp_docs_idx'::regclass)
       AS temp_compacted_inline;
```

Expected: zero requests and `temp_compacted_inline = true`.

- [ ] **Step 3: Run `compaction_request` and verify RED**

Run the single regression test. Expected: the old implementation reports one
request below threshold and dispatches requests for the temporary index.

- [ ] **Step 4: Add the shared threshold predicate**

Declare in `src/segment/merge.h`:

```c
extern bool tp_compaction_needed(Relation index);
```

Implement in `src/segment/merge.c` by reading metapage level counts under a
shared buffer lock and scanning levels `0..TP_MAX_LEVELS - 2`:

```c
bool
tp_compaction_needed(Relation index)
{
    TpIndexMetaPage metap;
    Buffer metabuf;
    Page metapage;
    bool needed = false;

    metabuf = ReadBuffer(index, 0);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    metapage = BufferGetPage(metabuf);
    metap = (TpIndexMetaPage)PageGetContents(metapage);

    for (uint32 level = 0; level < TP_MAX_LEVELS - 1; level++)
    {
        if (metap->level_counts[level] >=
            (uint16)tp_segments_per_level)
        {
            needed = true;
            break;
        }
    }

    UnlockReleaseBuffer(metabuf);
    return needed;
}
```

- [ ] **Step 5: Gate background dispatch and compact temp indexes inline**

Replace the background branch in `tp_do_spill()`:

```c
case TP_COMPACTION_BACKGROUND:
    if (RelationUsesLocalBuffers(index_rel))
        tp_maybe_compact_level(index_rel, 0);
    else if (tp_compaction_needed(index_rel))
        tp_compaction_request(RelationGetRelid(index_rel));
    break;
```

- [ ] **Step 6: Run targeted tests and formatting**

Run `compaction_request`, `compaction`, `make format-check`.
Expected: both regression tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/segment/merge.c src/segment/merge.h src/access/build.c \
  test/sql/compaction_request.sql test/expected/compaction_request.out
git commit -m "fix: request only necessary background compaction"
```

---

### Task 3: Align Dispatcher Errors and Prepared Transactions

**Files:**
- Modify: `src/index/compaction_request.c`
- Modify: `src/mod.c`
- Modify: `test/sql/compaction_request.sql`
- Modify: `test/expected/compaction_request.out`

**Interfaces:**
- Consumes: `tp_compaction_flush_requests()`.
- Produces: interrupt errors escape; PRE_PREPARE flushes pending requests.

- [ ] **Step 1: Add a failing cancellation regression**

Create a slow request function and a dedicated threshold-one index:

```sql
CREATE FUNCTION slow_compaction(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_catalog.pg_sleep(1);
END;
$$;

CREATE TABLE compaction_cancel_docs (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_cancel_docs_idx ON compaction_cancel_docs
    USING bm25(body) WITH (text_config = 'english');

SET pg_textsearch.segments_per_level = 1;
SET pg_textsearch.compaction_request_function = 'slow_compaction';
BEGIN;
INSERT INTO compaction_cancel_docs (body) VALUES ('cancel request');
SELECT bm25_spill_index('compaction_cancel_docs_idx') IS NOT NULL;
SET LOCAL statement_timeout = '100ms';
COMMIT;
SELECT count(*) AS canceled_rows FROM compaction_cancel_docs;
```

Expected: COMMIT errors with query cancellation and `canceled_rows = 0`.

- [ ] **Step 2: Run `compaction_request` and verify RED**

Expected: the old dispatcher emits a warning and commits the row.

- [ ] **Step 3: Rethrow cancellation and shutdown errors**

After rolling back the inner subtransaction, test:

```c
if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
    edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
    edata->sqlerrcode == ERRCODE_CRASH_SHUTDOWN)
{
    CurrentResourceOwner = oldowner;
    ReThrowError(edata);
}
```

Do not broaden this to ordinary pg_durable failures; those remain warnings.

- [ ] **Step 4: Flush requests before PREPARE**

Change `tp_xact_callback()`:

```c
case XACT_EVENT_PRE_PREPARE:
    tp_bulk_load_spill_check();
    tp_compaction_flush_requests();
    break;

case XACT_EVENT_PREPARE:
    tp_compaction_reset_requests();
    break;
```

Keep parallel workers from dispatching.

- [ ] **Step 5: Run targeted tests, build, and formatting**

Run:

```bash
make
make format-check
$(pg_config --pgxs | xargs dirname)/../../src/test/regress/pg_regress \
  --inputdir=test --outputdir=test compaction_request
```

Expected: build and regression pass.

- [ ] **Step 6: Commit**

```bash
git add src/index/compaction_request.c src/mod.c \
  test/sql/compaction_request.sql test/expected/compaction_request.out
git commit -m "fix: preserve compaction request interrupt semantics"
```

---

### Task 4: Harden Role and Wrapper Setup

**Files:**
- Modify: `scripts/durable_compaction/01_setup_role.sql`
- Modify: `scripts/durable_compaction/02_wrapper.sql`
- Modify: `scripts/durable_compaction/README.md`
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Produces: explicit non-superuser `index_owner`; compactor membership with
  `SET FALSE`; wrapper authorizes the login caller's `INSERT` privilege.
- Produces: pg_durable starts with `transaction_mode => 'new'`.

- [ ] **Step 1: Add failing role-safety shell assertions**

Before normal setup, assert these invocations fail:

```bash
if sql_super -f "${GLUE_DIR}/01_setup_role.sql" >/dev/null 2>&1; then
    error "setup accepted a missing index_owner"
fi
if sql_super -f "${GLUE_DIR}/01_setup_role.sql" \
    -v index_owner=postgres >/dev/null 2>&1; then
    error "setup accepted a superuser index_owner"
fi
```

After normal setup:

```bash
if sql_as textsearch_compactor -c "SET ROLE app_owner;" \
    >/dev/null 2>&1; then
    error "compactor can SET ROLE app_owner"
fi
```

- [ ] **Step 2: Add failing wrapper authorization and ACL-reset tests**

Create a second owner/index, grant that owner to the compactor with
`SET FALSE`, then assert `app_writer` cannot call
`bm25_request_compaction()` for the second owner's index. Grant a temporary
`old_writer`, rerun `02_wrapper.sql` with `writer_role=app_writer`, and assert
`old_writer` no longer has EXECUTE.

- [ ] **Step 3: Run `make test-durable` and verify RED**

Expected: missing/superuser owner setup succeeds incorrectly, `SET ROLE`
succeeds, cross-index submission succeeds, and the old named grant survives.

- [ ] **Step 4: Require and validate the owner**

Use psql conditionals to reject an omitted variable:

```sql
\if :{?index_owner}
\else
\echo 'index_owner is required'
\quit 1
\endif
```

Query `pg_roles` into psql booleans, fail when the role is missing or
`rolsuper`, then grant:

```sql
GRANT :"index_owner" TO textsearch_compactor
    WITH INHERIT TRUE, SET FALSE;
```

After creating/updating the compactor, raise an exception if any row in
`pg_auth_members` gives it membership in a superuser role.

- [ ] **Step 5: Grant the exact extension capabilities**

Resolve `pg_extension.extnamespace` into `ext_schema`, then grant:

```sql
GRANT USAGE ON SCHEMA :"ext_schema" TO textsearch_compactor;
GRANT EXECUTE ON FUNCTION :"ext_schema".bm25_compact(regclass),
    :"ext_schema".bm25_compact_step(regclass),
    :"ext_schema".bm25_level_counts(regclass),
    :"ext_schema".bm25_needs_compaction(regclass),
    :"ext_schema".bm25_indexes_needing_compaction(),
    :"ext_schema".bm25_compact_pending()
TO textsearch_compactor;
```

- [ ] **Step 6: Recreate and authorize the wrapper**

Before creation:

```sql
DROP FUNCTION IF EXISTS public.bm25_request_compaction(regclass);
```

Inside the body, join `pg_index` and require:

```sql
IF NOT EXISTS (
    SELECT 1
    FROM pg_catalog.pg_index i
    WHERE i.indexrelid = idx
      AND pg_catalog.has_table_privilege(
              session_user, i.indrelid, 'INSERT'))
THEN
    RAISE EXCEPTION 'permission denied to request compaction for %', idx
        USING ERRCODE = 'insufficient_privilege';
END IF;
```

Change the durable start to:

```sql
transaction_mode => 'new'
```

Update comments to describe level counts/backstop as the guarantee and the
independent request as an accelerator.

- [ ] **Step 7: Replace production `trust` guidance**

Keep `trust` only in the ephemeral test/demo setup. Document peer/ident or
another passwordless authenticated mapping for production.

- [ ] **Step 8: Run durable integration and syntax checks**

Run:

```bash
bash -n test/scripts/durable_compaction.sh
make test-durable
```

Expected: all role and wrapper assertions pass.

- [ ] **Step 9: Commit**

```bash
git add scripts/durable_compaction/01_setup_role.sql \
  scripts/durable_compaction/02_wrapper.sql \
  scripts/durable_compaction/README.md \
  test/scripts/durable_compaction.sh
git commit -m "fix: harden durable compaction privileges"
```

---

### Task 5: Verify Physical Request and Permission Behavior End to End

**Files:**
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Consumes: independent pg_durable request starts from Task 4.
- Verifies: rollback, PREPARE, temp-index, and real low-privilege compaction.

- [ ] **Step 1: Enable prepared transactions in the test cluster**

Add to the generated `postgresql.conf`:

```bash
echo "max_prepared_transactions = 10" >> "$PGDATA/postgresql.conf"
```

- [ ] **Step 2: Add a rollback/backstop scenario**

Create an over-threshold physical layout inside a transaction that explicitly
rolls back. Assert no hot-path instance was submitted, level counts still show
work, run `bm25_compact_pending()`, and assert
`bm25_needs_compaction()` becomes false.

- [ ] **Step 3: Add an independent-start rollback scenario**

Call `public.bm25_request_compaction()` directly inside a transaction and roll
back. Capture the returned ID and assert the instance still exists and reaches
`completed`, proving `transaction_mode => 'new'`.

- [ ] **Step 4: Add a prepared-transaction scenario**

Spill over threshold, execute:

```sql
PREPARE TRANSACTION 'bm25_compaction_prepare';
COMMIT PREPARED 'bm25_compaction_prepare';
```

Assert a durable instance exists and reaches `completed`.

- [ ] **Step 5: Add a temporary-index scenario**

In one persistent psql session, create a temporary BM25 index, make two spills,
and assert it no longer needs compaction. In a second query, assert no
`df.instances` row was created for its label.

- [ ] **Step 6: Make the permission test perform real compaction**

Run two measured auto-spill batches as `app_writer`, wait for the generated
instance, and require:

```bash
st=$(wait_for_instance "$id" 60)
assert_eq "permission-path instance completes" "completed" "$st"
assert_eq "permission-path compaction clears L0" "f" \
    "$(sql_super -c \
       "SELECT bm25_needs_compaction('t_perm_idx'::regclass);")"
```

- [ ] **Step 7: Run the durable suite and verify GREEN**

Run `make test-durable`.
Expected: every new assertion and the existing suite pass.

- [ ] **Step 8: Commit**

```bash
git add test/scripts/durable_compaction.sh
git commit -m "test: cover durable compaction failure boundaries"
```

---

### Task 6: Make Benchmark Isolation Deterministic

**Files:**
- Modify: `benchmarks/durable_compaction_latency.sh`

**Interfaces:**
- Produces: `drain_background_work(index_name)` that returns only when every
  matching task is terminal and the index needs no compaction.

- [ ] **Step 1: Add a shell-testable drain helper**

Extract label polling into:

```bash
drain_background_work() {
    local idx="$1" label pending waited=0
    label=$(label_for "$idx")

    while [ "$waited" -lt 300 ]; do
        pending=$(sql_super -c "
            SELECT count(*)
            FROM df.instances
            WHERE label = '${label}'
              AND status NOT IN ('completed', 'failed');")
        [ "$pending" = "0" ] && break
        sleep 1
        waited=$((waited + 1))
    done

    [ "$pending" = "0" ] ||
        error "background compaction did not drain for ${idx}"

    local needs
    needs=$(sql_super -c \
        "SELECT bm25_needs_compaction('${idx}'::regclass);")
    [ "$needs" = "f" ] ||
        error "background compaction left work for ${idx}"
}
```

- [ ] **Step 2: Replace newest-instance polling**

Call `drain_background_work "$idx"` after each background benchmark case.

- [ ] **Step 3: Validate syntax and a short benchmark**

Run:

```bash
bash -n benchmarks/durable_compaction_latency.sh
BENCH_ROUNDS="2" BENCH_TRIGGER_DOCS=1000 \
  benchmarks/durable_compaction_latency.sh
```

Expected: the benchmark completes and no case starts with outstanding work.

- [ ] **Step 4: Commit**

```bash
git add benchmarks/durable_compaction_latency.sh
git commit -m "bench: drain all background compaction work"
```

---

### Task 7: Ship Operator Glue in Every Artifact

**Files:**
- Modify: `.github/workflows/package-release.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `scripts/package-deb.sh`

**Interfaces:**
- Produces: operator SQL under
  `${sharedir}/extension/pg_textsearch/durable_compaction` in Debian packages;
  equivalent directories in binary/source archives.

- [ ] **Step 1: Add artifact-content assertions before copy changes**

In package jobs, add checks that fail unless archives/packages contain:

```text
scripts/durable_compaction/01_setup_role.sql
scripts/durable_compaction/02_wrapper.sql
scripts/durable_compaction/03_backstop.sql
docs/background_compaction.md
```

For Debian packages, use `dpkg-deb -c`; for tarballs, use `tar -tf`.

- [ ] **Step 2: Run workflow/script checks and verify RED**

Run:

```bash
bash -n scripts/package-deb.sh
git grep -n "durable_compaction/01_setup_role.sql" \
  .github/workflows/package-release.yml .github/workflows/release.yml
```

Expected: no artifact copy rule currently satisfies the asserted paths.

- [ ] **Step 3: Include source and binary archive content**

Extend source copying to include `scripts`, `docs`, and `demo`. In the
versioned binary archive create `durable_compaction/` and copy the three SQL
files plus `README.md` and `docs/background_compaction.md`.

- [ ] **Step 4: Include Debian package content**

Create:

```bash
COMPACTIONDIR="${SHAREDIR}/extension/pg_textsearch/durable_compaction"
DOCDIR="/usr/share/doc/${PACKAGE_NAME}"
mkdir -p "${BUILDDIR}${COMPACTIONDIR}" "${BUILDDIR}${DOCDIR}"
cp "${BASEDIR}"/scripts/durable_compaction/*.sql \
   "${BUILDDIR}${COMPACTIONDIR}/"
cp "${BASEDIR}/scripts/durable_compaction/README.md" \
   "${BASEDIR}/docs/background_compaction.md" \
   "${BUILDDIR}${DOCDIR}/"
```

- [ ] **Step 5: Run package checks**

Build one Debian package with the existing packaging command, then run:

```bash
dpkg-deb -c dist/*.deb |
  grep -F '/extension/pg_textsearch/durable_compaction/01_setup_role.sql'
dpkg-deb -c dist/*.deb |
  grep -F '/extension/pg_textsearch/durable_compaction/02_wrapper.sql'
dpkg-deb -c dist/*.deb |
  grep -F '/extension/pg_textsearch/durable_compaction/03_backstop.sql'
dpkg-deb -c dist/*.deb |
  grep -F '/usr/share/doc/pg-textsearch-postgresql-'
```

Create a source archive with the workflow's copy/tar commands and run:

```bash
tar -tf pg_textsearch-*.tar.gz |
  grep -F '/scripts/durable_compaction/01_setup_role.sql'
tar -tf pg_textsearch-*.tar.gz |
  grep -F '/docs/background_compaction.md'
```

Expected: every `grep` exits zero.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/package-release.yml .github/workflows/release.yml \
  scripts/package-deb.sh
git commit -m "build: package durable compaction operator files"
```

---

### Task 8: Reconcile Documentation With Physical Semantics

**Files:**
- Modify: `docs/background_compaction.md`
- Modify: `docs/background_compaction_report.md`
- Modify: `scripts/durable_compaction/README.md`
- Modify: `demo/background_compaction/README.md`
- Modify: `demo/background_compaction/run_demo.sh`

**Interfaces:**
- Documents: independent task start, explicit-rollback recovery, threshold
  gating, temp-index inline behavior, loopback limits, role setup, and
  packaged file locations.

- [ ] **Step 1: Replace atomicity claims**

State:

```text
Physical spills survive SQL rollback. A request started at PRE_COMMIT or
PRE_PREPARE uses transaction_mode='new' and survives later caller failure.
Explicit rollback before those callbacks submits no request; durable level
counts let the hourly backstop find and repair the remaining work.
```

- [ ] **Step 2: Document operational behavior**

Document:

- no task is submitted below threshold;
- temporary indexes compact inline;
- `pg_durable.max_new_transaction_starts` and
  `pg_durable.new_transaction_start_timeout` bound independent starts;
- explicit non-superuser `index_owner` is mandatory;
- production uses authenticated peer/ident rather than `trust`;
- packaged setup-file locations.

- [ ] **Step 3: Update demo assertions**

Make demo output distinguish immediate accelerator tasks from backstop repair
after explicit rollback. Remove statements claiming task/heap rollback
atomicity.

- [ ] **Step 4: Check documentation consistency**

Run:

```bash
git grep -n "transaction_mode => 'caller'\\|commit/rollback atomicity\\|queues exactly one" \
  docs scripts/durable_compaction demo/background_compaction
bash -n demo/background_compaction/run_demo.sh
```

Expected: no obsolete atomicity claims; demo syntax passes.

- [ ] **Step 5: Commit**

```bash
git add docs/background_compaction.md docs/background_compaction_report.md \
  scripts/durable_compaction/README.md \
  demo/background_compaction/README.md \
  demo/background_compaction/run_demo.sh
git commit -m "docs: align compaction requests with physical spills"
```

---

### Task 9: Full Verification and PR Update

**Files:**
- Verify all modified files.
- Inspect: `test/regression.diffs`

**Interfaces:**
- Produces: a green, pushed PR #471 with all remediation commits.

- [ ] **Step 1: Build and format**

```bash
make
make format-check
```

Expected: both exit zero.

- [ ] **Step 2: Run SQL regression**

```bash
make installcheck
test ! -s test/regression.diffs
```

Expected: all regression tests pass and `test/regression.diffs` is empty or
absent.

- [ ] **Step 3: Run durable integration**

```bash
make test-durable
```

Expected: every assertion passes.

- [ ] **Step 4: Run shell syntax and targeted packaging checks**

```bash
bash -n test/scripts/durable_compaction.sh \
  benchmarks/durable_compaction_latency.sh \
  demo/background_compaction/run_demo.sh \
  scripts/package-deb.sh
```

Expected: zero syntax errors and artifact-content checks pass.

- [ ] **Step 5: Inspect the final diff**

```bash
git diff --check origin/main...HEAD
git status --short
```

Expected: no whitespace errors and no uncommitted files.

- [ ] **Step 6: Push**

```bash
git push origin task2-bm25-compact
```

Expected: PR #471 updates to the verified head.
