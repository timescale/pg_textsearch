# Multi-Database Compaction Rejection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add live multi-database coverage proving that background compaction
is rejected safely when pg_durable is installed in a different database, while
manual compaction remains usable.

**Architecture:** Keep pg_durable and all managed workflows in the configured
control database. Add a second database containing only pg_textsearch, require
CREATE and ALTER background admission to fail before leaving activation
residue, and reorder pg_textsearch's checks so the error identifies the
configured-database mismatch before local extension discovery.

**Tech Stack:** PostgreSQL 17/18, C, PGXS, SPI, pg_durable 0.2.8, Bash,
pg_regress.

## Global Constraints

- pg_durable remains installed only in `durable_compaction_test`.
- `durable_compaction_remote` contains pg_textsearch but not pg_durable.
- Do not add dblink, postgres_fdw, libpq, or a cross-database submission path.
- Do not modify microsoft/pg_durable#377.
- Scenario A must retain its existing behavior and coverage.
- Scenario B must raise
  `pg_durable is not initialized for this database` with detail
  `pg_durable.database does not name the current database`.
- The existing same-database missing-extension case must retain
  `background compaction requires pg_durable 0.2.8 or newer`.
- Rejection must not leave an index shell, workflow, helper grant, dependency,
  or changed reloption.
- Manual compaction must remain usable in the remote database.
- Use American English and wrap C and prose lines at 79 characters.
- Do not add Copilot attribution or commit trailers.

---

## File Structure

- `test/scripts/durable_compaction.sh`: Create the second database, exercise
  CREATE and ALTER rejection, inspect both databases, and prove manual
  fallback.
- `src/index/compaction_job.c`: Report a configured-database mismatch before
  looking for local pg_durable catalog objects.
- `README.md`: State the same-database installation requirement in the public
  setup documentation.
- `docs/background_compaction.md`: Explain why pg_durable target routing does
  not provide cross-database pg_textsearch admission.

---

### Task 1: Add the multi-database rejection contract

**Files:**
- Modify: `test/scripts/durable_compaction.sh:13-75`
- Modify: `test/scripts/durable_compaction.sh:400-550`
- Modify: `test/scripts/durable_compaction.sh:590-625`
- Modify: `test/scripts/durable_compaction.sh:947-1035`
- Modify: `src/index/compaction_job.c:393-448`

**Interfaces:**
- Consumes: `pg_durable.database`, existing `tp_durable_required()`, existing
  `tp_durable_not_initialized(const char *detail)`, and the
  `make test-durable` package interface.
- Produces: `sql_in_db_as(database, role, ...)`, `sql_remote_as(role, ...)`,
  `sql_remote_super(...)`, and `test_remote_database_rejection()`.

- [ ] **Step 1: Parameterize database connections in the test harness**

Add an immutable control database name and the remote database name near the
existing constants:

```bash
CONTROL_DB=durable_compaction_test
REMOTE_DB=durable_compaction_remote
TEST_DB="${CONTROL_DB}"
ROLLBACK_DB=durable_compaction_rollback
```

Replace the current `sql_as()` implementation with:

```bash
sql_in_db_as() {
    local database=$1 role=$2
    shift 2
    "${PGBINDIR}/psql" -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -U "${role}" -d "${database}" -qAt -v ON_ERROR_STOP=1 "$@"
}

sql_as() {
    local role=$1
    shift
    sql_in_db_as "${TEST_DB}" "${role}" "$@"
}

sql_super() {
    sql_as postgres "$@"
}

sql_remote_as() {
    local role=$1
    shift
    sql_in_db_as "${REMOTE_DB}" "${role}" "$@"
}

sql_remote_super() {
    sql_remote_as postgres "$@"
}
```

Keep every existing `sql_as` and `sql_super` call unchanged.

- [ ] **Step 2: Add remote-state inspection helpers**

Add these helpers after `dependency_count()`:

```bash
remote_dependency_count() {
    sql_remote_super -c "SELECT count(*)
      FROM pg_catalog.pg_depend AS dep
      JOIN pg_catalog.pg_am AS am
        ON dep.classid = 'pg_catalog.pg_am'::regclass
       AND dep.objid = am.oid
      JOIN pg_catalog.pg_extension AS ext
        ON dep.refclassid = 'pg_catalog.pg_extension'::regclass
       AND dep.refobjid = ext.oid
      WHERE am.amname = 'bm25'
        AND ext.extname = 'pg_durable'
        AND dep.deptype = 'n';"
}

remote_helper_grant() {
    sql_remote_super -c "SELECT
        pg_catalog.has_function_privilege(
            'durable_owner',
            'bm25_compact_step_if_current(oid,oid,oid,oid,oid)',
            'EXECUTE')
        OR pg_catalog.has_function_privilege(
            'durable_owner',
            'bm25_background_target_is_current(oid,oid,oid,oid,oid)',
            'EXECUTE');"
}
```

These queries run in the remote database and must return `0` and `f`
respectively before and after rejected activation.

- [ ] **Step 3: Write the failing scenario B test**

Add `test_remote_database_rejection()` before
`test_rollback_in_fresh_database()`:

```bash
test_remote_database_rejection() {
    local alter_error create_error jobs_before

    sql_super -c "CREATE DATABASE ${REMOTE_DB};"
    sql_remote_super -c "CREATE EXTENSION pg_textsearch;
        GRANT CREATE ON SCHEMA public TO durable_owner;
        ALTER ROLE durable_owner IN DATABASE ${REMOTE_DB}
          SET maintenance_work_mem = '1MB';"

    assert_eq "pg_durable exists in the control database" "t" \
        "$(sql_super -c "SELECT EXISTS (
            SELECT 1 FROM pg_catalog.pg_extension
            WHERE extname = 'pg_durable');")"
    assert_eq "pg_durable is absent from the remote database" "f" \
        "$(sql_remote_super -c "SELECT EXISTS (
            SELECT 1 FROM pg_catalog.pg_extension
            WHERE extname = 'pg_durable');")"

    jobs_before="$(managed_job_count)"
    sql_remote_super -c "CREATE TABLE remote_cic_docs (body text);
        ALTER TABLE remote_cic_docs OWNER TO durable_owner;"
    if create_error="$(sql_remote_as durable_owner -c "
        CREATE INDEX CONCURRENTLY remote_cic_docs_idx
          ON remote_cic_docs USING bm25(body)
          WITH (text_config = 'english',
                compaction = 'background');" 2>&1)"; then
        error "remote background CIC unexpectedly succeeded"
    fi
    if ! grep -Fq \
        "pg_durable is not initialized for this database" \
        <<<"${create_error}" ||
       ! grep -Fq \
        "pg_durable.database does not name the current database" \
        <<<"${create_error}"; then
        error "remote CIC did not report the configured-database mismatch: \
${create_error}"
    fi
    assert_eq "remote CIC leaves no relation" "t" \
        "$(sql_remote_super -c "SELECT
            pg_catalog.to_regclass('remote_cic_docs_idx') IS NULL;")"
    assert_eq "remote CIC creates no control workflow" "${jobs_before}" \
        "$(managed_job_count)"

    sql_remote_super -c "
        CREATE TABLE remote_manual_docs (id integer, body text);
        ALTER TABLE remote_manual_docs OWNER TO durable_owner;"
    sql_remote_as durable_owner <<'SQL' >/dev/null
CREATE INDEX remote_manual_docs_idx ON remote_manual_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
DO $body$
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO remote_manual_docs
        SELECT n * 100 + i, format('remote round %s document %s', n, i)
        FROM generate_series(1, 20) AS i;
        PERFORM bm25_spill_index('remote_manual_docs_idx');
    END LOOP;
END
$body$;
SQL
    assert_eq "remote manual index has compaction debt" "t" \
        "$(sql_remote_as durable_owner -c "SELECT
            bm25_needs_compaction('remote_manual_docs_idx'::regclass);")"

    if alter_error="$(sql_remote_as durable_owner -c "
        ALTER INDEX remote_manual_docs_idx
          SET (compaction = 'background');" 2>&1)"; then
        error "remote background ALTER unexpectedly succeeded"
    fi
    if ! grep -Fq \
        "pg_durable is not initialized for this database" \
        <<<"${alter_error}" ||
       ! grep -Fq \
        "pg_durable.database does not name the current database" \
        <<<"${alter_error}"; then
        error "remote ALTER did not report the configured-database mismatch: \
${alter_error}"
    fi
    assert_eq "remote ALTER preserves manual mode" "t" \
        "$(sql_remote_super -c "SELECT reloptions @>
            ARRAY['compaction=manual']
            FROM pg_catalog.pg_class
            WHERE oid = 'remote_manual_docs_idx'::regclass;")"
    assert_eq "remote ALTER creates no control workflow" "${jobs_before}" \
        "$(managed_job_count)"
    assert_eq "remote ALTER creates no helper grant" "f" \
        "$(remote_helper_grant)"
    assert_eq "remote ALTER creates no dependency" "0" \
        "$(remote_dependency_count)"

    sql_remote_as durable_owner -c \
        "SELECT bm25_compact('remote_manual_docs_idx'::regclass);" \
        >/dev/null
    assert_eq "remote manual compaction clears debt" "t" \
        "$(sql_remote_as durable_owner -c "SELECT NOT
            bm25_needs_compaction('remote_manual_docs_idx'::regclass);")"

    sql_super -c "DROP DATABASE ${REMOTE_DB};"
}
```

Invoke it after `test_bypassrls_owner_isolation` and before
`test_sticky_dependency`:

```bash
test_bypassrls_owner_isolation
test_remote_database_rejection
test_sticky_dependency
```

- [ ] **Step 4: Run the new test and verify RED**

Build a PostgreSQL 17 pg_durable package:

```bash
make -C /home/azureuser/pg_durable-worktrees/loop-continue-on-failure \
  package \
  PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
  PGRX_PACKAGE_DIR=/tmp/pg_durable-multidb-pg17
```

Build and install pg_textsearch, then run the integration:

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5
make clean PG_CONFIG=/home/azureuser/pg17/bin/pg_config
make PG_CONFIG=/home/azureuser/pg17/bin/pg_config
make install PG_CONFIG=/home/azureuser/pg17/bin/pg_config
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_VERSION=0.2.8 \
PG_DURABLE_PACKAGE_DIR=/tmp/pg_durable-multidb-pg17 \
make test-durable
```

Expected: FAIL in `test_remote_database_rejection` because the current code
reports `background compaction requires pg_durable 0.2.8 or newer` before it
examines the configured database.

- [ ] **Step 5: Report the configured-database mismatch first**

In `tp_discover_job_objects()`, add a cached preload flag:

```c
	bool		durable_preloaded;
```

Immediately after `memset(objects, 0, sizeof(*objects));`, add:

```c
	durable_preloaded = tp_library_is_preloaded("pg_durable");
	if (durable_preloaded)
	{
		configured_database =
				GetConfigOption("pg_durable.database", true, false);
		database_name = get_database_name(MyDatabaseId);
		if (configured_database != NULL && database_name != NULL &&
			strcmp(configured_database, database_name) != 0)
			tp_durable_not_initialized(
					"pg_durable.database does not name the current database");
	}
```

Keep extension discovery and version validation after this block. Replace the
later repeated preload lookup and GUC assignments with:

```c
	if (!durable_preloaded)
		tp_durable_not_initialized(
				"pg_durable is not present in shared_preload_libraries");

	if (configured_database == NULL || database_name == NULL ||
		strcmp(configured_database, database_name) != 0)
		tp_durable_not_initialized(
				"pg_durable.database does not name the current database");
```

This preserves the existing same-database missing-extension error while making
the known remote-database configuration fail before local catalog discovery.

- [ ] **Step 6: Run the integration and verify GREEN**

Run:

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5
make format-single FILE=src/index/compaction_job.c
make clean PG_CONFIG=/home/azureuser/pg17/bin/pg_config
make PG_CONFIG=/home/azureuser/pg17/bin/pg_config
make install PG_CONFIG=/home/azureuser/pg17/bin/pg_config
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_VERSION=0.2.8 \
PG_DURABLE_PACKAGE_DIR=/tmp/pg_durable-multidb-pg17 \
make test-durable
```

Expected: PASS, including:

```text
[durable] PASS: pg_durable is absent from the remote database
[durable] PASS: remote CIC leaves no relation
[durable] PASS: remote ALTER preserves manual mode
[durable] PASS: remote ALTER creates no control workflow
[durable] PASS: remote manual compaction clears debt
```

- [ ] **Step 7: Commit the implementation and test**

```bash
git add src/index/compaction_job.c test/scripts/durable_compaction.sh
git diff --cached --check
git commit -m "Test multi-database compaction rejection"
```

---

### Task 2: Document the same-database requirement

**Files:**
- Modify: `README.md:618-645`
- Modify: `docs/background_compaction.md:174-190`

**Interfaces:**
- Consumes: scenario B's stable rejection behavior from Task 1.
- Produces: an explicit public contract that background mode requires
  pg_durable in the BM25 index database.

- [ ] **Step 1: Update the README**

After the paragraph describing pg_durable setup, add:

```markdown
pg_durable must be installed in the same database as the BM25 index.
pg_durable can execute a workflow's SQL in another target database, but its
submission and control APIs exist only in the configured database.
pg_textsearch does not use dblink or postgres_fdw to bridge that boundary.
Use `manual` compaction for indexes in other databases.
```

- [ ] **Step 2: Update the detailed background-compaction documentation**

After the same-database setup requirements, add:

```markdown
The pg_durable extension and pg_textsearch index must be in the same database.
pg_durable's `database` argument routes SQL activities after submission; it
does not expose `df.start`, `df.signal`, or workflow metadata in another
database. pg_textsearch therefore rejects background mode when
`pg_durable.database` names a different database. Use `manual` mode there.
```

- [ ] **Step 3: Check documentation consistency**

Run:

```bash
rg -n "same database|current database|pg_durable.database|manual" \
  README.md docs/background_compaction.md
git diff --check
```

Expected: both documents state the same-database requirement, explain the
target-routing distinction, and recommend manual mode.

- [ ] **Step 4: Commit the documentation**

```bash
git add README.md docs/background_compaction.md
git diff --cached --check
git commit -m "Document same-database durable requirement"
```

---

### Task 3: Verify both PostgreSQL versions

**Files:**
- Verify: `src/index/compaction_job.c`
- Verify: `test/scripts/durable_compaction.sh`
- Verify: `README.md`
- Verify: `docs/background_compaction.md`

**Interfaces:**
- Consumes: the implementation and documentation from Tasks 1 and 2.
- Produces: final evidence that scenario A and scenario B behave identically
  on PostgreSQL 17 and 18.

- [ ] **Step 1: Run source and formatting gates**

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5
make format-check
git diff --check
./test/scripts/compaction_request_source.sh
```

Expected: all commands exit zero.

- [ ] **Step 2: Run the complete PostgreSQL 17 regression suite**

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5
PATH=/home/azureuser/pg17/bin:$PATH \
PGHOST=/tmp \
make test-local PG_CONFIG=/home/azureuser/pg17/bin/pg_config
```

Expected: all 77 SQL regression tests pass.

- [ ] **Step 3: Build the PostgreSQL 18 pg_durable package**

```bash
make -C /home/azureuser/pg_durable-worktrees/loop-continue-on-failure \
  package \
  PG_CONFIG=/home/azureuser/pg18/bin/pg_config \
  PGRX_PACKAGE_DIR=/tmp/pg_durable-multidb-pg18
```

Expected: pg_durable 0.2.8 is packaged under
`/tmp/pg_durable-multidb-pg18`.

- [ ] **Step 4: Run scenario A and B on PostgreSQL 18**

```bash
cd /home/azureuser/.copilot/worktrees/pg_textsearch-bg-compaction-5
make clean PG_CONFIG=/home/azureuser/pg18/bin/pg_config
make PG_CONFIG=/home/azureuser/pg18/bin/pg_config
make install PG_CONFIG=/home/azureuser/pg18/bin/pg_config
PATH=/home/azureuser/pg18/bin:$PATH \
PG_CONFIG=/home/azureuser/pg18/bin/pg_config \
PG_DURABLE_VERSION=0.2.8 \
PG_DURABLE_PACKAGE_DIR=/tmp/pg_durable-multidb-pg18 \
make test-durable
```

Expected: the complete integration passes, including same-database workflow
execution and remote-database rejection.

- [ ] **Step 5: Review the final branch**

```bash
git status --short
git log --oneline --decorate -5
git diff origin/background-compaction-5-backstop...HEAD --check
git diff origin/background-compaction-5-backstop...HEAD --stat
```

Expected: the branch contains only the design, plan, implementation, tests,
and documentation for deterministic multi-database rejection.

- [ ] **Step 6: Request code review**

Dispatch a read-only reviewer over:

```bash
git diff origin/background-compaction-5-backstop...HEAD
```

Require the reviewer to check error ordering, CREATE INDEX CONCURRENTLY
cleanup, ALTER rollback, cross-database query isolation, and whether scenario A
remains unchanged.

- [ ] **Step 7: Clean generated artifacts**

```bash
rm -rf /tmp/pg_durable-multidb-pg17
rm -rf /tmp/pg_durable-multidb-pg18
make clean PG_CONFIG=/home/azureuser/pg18/bin/pg_config
git status --short
```

Expected: only intentional tracked changes remain.
