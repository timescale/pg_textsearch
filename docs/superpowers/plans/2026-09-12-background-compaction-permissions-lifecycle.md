# Background Compaction Permission and Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make managed background compaction admit only executable owner
identities and automatically reconcile workflows across ownership changes,
REINDEX, and partitioned index creation.

**Architecture:** Extend the existing owner admission boundary with database
and extension-schema ACL checks. Refactor utility-hook index discovery so each
relevant DDL command produces a deduplicated list of affected physical bm25
indexes, then activate their current generations after PostgreSQL completes
the command. Keep the physical-target guard authoritative and allow spill
signaling to adopt a valid generation missed by utility reconciliation.

**Tech Stack:** PostgreSQL 17/18 extension C APIs, ProcessUtility hooks,
pg_durable 0.2.8, Bash integration tests, pg_regress.

## Global Constraints

- pg_durable 0.2.8 remains the minimum supported version.
- Workflow SQL executes as the physical index owner.
- Old physical generations must return false without touching replacements.
- Concurrent DDL must preflight every deterministic admission requirement
  before PostgreSQL crosses an irreversible transaction boundary.
- Failed transactional DDL must leave no reloption change, workflow,
  dependency, or private-helper grant.
- Use PostgreSQL catalog APIs and ACL checks rather than SQL string probes.
- Keep PostgreSQL 17 and 18 compatibility.

---

### Task 1: Complete Owner Admission

**Files:**
- Modify: `src/index/compaction_job.c:54-741`
- Modify: `test/scripts/durable_compaction.sh:84-155`

**Interfaces:**
- Consumes: `TpCompactionJobObjects`, `tp_compaction_job_preflight()`, and
  `tp_require_owner_durable_privileges()`.
- Produces: complete owner admission covering database `CONNECT` and
  pg_textsearch-schema `USAGE`.

- [ ] **Step 1: Add failing integration cases**

Add roles `durable_no_connect` and `durable_no_textsearch_schema` in
`setup_cluster()`. Extend `test_cic_owner_privilege_preflight()` with:

```sql
REVOKE CONNECT ON DATABASE durable_compaction_test FROM PUBLIC;
GRANT CONNECT ON DATABASE durable_compaction_test
TO postgres, durable_owner, durable_usage_only, durable_read_only,
   durable_bypass, durable_actor, durable_writer;
```

Create a table owned by `durable_no_connect`, invoke concurrent CREATE as a
member role which retains `CONNECT`, and assert:

```sql
SELECT to_regclass('no_connect_docs_idx') IS NULL;
SELECT count(*) FROM df.instances
WHERE label LIKE 'pg_textsearch:bg:v1:%';
```

For schema coverage, revoke PUBLIC usage on the extension schema, grant it to
all test roles except `durable_no_textsearch_schema`, attempt concurrent
CREATE through an eligible member, and assert the stable owner-privilege error,
no index shell, no workflow, no dependency, and no helper grant.

- [ ] **Step 2: Run the integration test and confirm both cases fail**

Run:

```bash
PATH=/home/azureuser/pg17/bin:$PATH \
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_PACKAGE_DIR="$PG_DURABLE_PACKAGE_DIR" \
make test-durable
```

Expected: the new CREATE commands succeed or reach pg_durable instead of
failing during deterministic preflight.

- [ ] **Step 3: Store the pg_textsearch namespace in discovery results**

Extend `TpCompactionJobObjects`:

```c
Oid   textsearch_namespace_oid;
char *textsearch_schema;
```

Populate both fields in `tp_discover_job_objects()` from
`get_extension_schema(textsearch_oid)` and use the stored schema when resolving
the two private helpers.

- [ ] **Step 4: Add database and schema ACL admission checks**

Add:

```c
static void
tp_require_owner_database_connect(Oid owner_oid)
{
    if (object_aclcheck(DatabaseRelationId,
                        MyDatabaseId,
                        owner_oid,
                        ACL_CONNECT) != ACLCHECK_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("index owner cannot connect for background compaction"),
                 errdetail("Role \"%s\" lacks CONNECT privilege on database \"%s\".",
                           GetUserNameFromId(owner_oid, false),
                           get_database_name(MyDatabaseId))));
}
```

Call it after `tp_require_owner_login()`. In
`tp_require_owner_durable_privileges()`, require `ACL_USAGE` on
`objects->textsearch_namespace_oid` and identify the actual schema in the
error detail. Do not grant schema usage automatically.

- [ ] **Step 5: Run the integration test**

Run the Task 1 command. Expected: both new admission cases fail before index
creation and every existing durable-compaction scenario passes.

- [ ] **Step 6: Commit**

```bash
git add src/index/compaction_job.c test/scripts/durable_compaction.sh
git commit -m "Validate background compaction owner access"
```

---

### Task 2: Cover ALTER Atomicity and Execution Identity

**Files:**
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Consumes: completed Task 1 admission checks and existing
  `tp_compaction_job_activate()` behavior.
- Produces: permission regression coverage for ALTER, actor/owner separation,
  worker identity, and cross-owner helper isolation.

- [ ] **Step 1: Add an ALTER rejection assertion helper**

Implement a Bash helper which creates a manual bm25 index, attempts:

```sql
ALTER INDEX <index> SET (compaction = 'background');
```

and verifies:

```sql
SELECT reloptions @> ARRAY['compaction=manual']
FROM pg_class WHERE oid = '<index>'::regclass;
```

It must also assert unchanged managed-job count, dependency count, and owner
helper privileges.

- [ ] **Step 2: Add the ALTER failure matrix**

Exercise owners which are:

- `NOLOGIN`;
- superuser while `pg_durable.enable_superuser_instances = off`;
- missing pg_durable table privileges;
- missing database `CONNECT`; and
- missing pg_textsearch-schema `USAGE`.

Run `make test-durable` and confirm at least one new assertion fails if the
ALTER path is not transactional or skips admission.

- [ ] **Step 3: Add actor, writer, and worker identity roles**

Create:

```sql
CREATE ROLE durable_actor LOGIN;
CREATE ROLE durable_writer LOGIN;
GRANT durable_owner TO durable_actor;
```

Have `durable_actor`, without `SET ROLE`, enable an index owned by
`durable_owner`. Grant only table INSERT to `durable_writer` and use it to
trigger a spill.

- [ ] **Step 4: Instrument the worker identity**

Create an audit table owned by `durable_owner`, grant INSERT to
`durable_owner`, and temporarily replace the extension-member helper with a
PL/pgSQL wrapper that records:

```sql
INSERT INTO worker_identity_audit(role_name)
VALUES (current_user);
```

The wrapper must delegate to a temporary C alias bound to
`tp_compact_index_step_if_current`. Assert every recorded role is
`durable_owner`, then restore the original helper definition.

- [ ] **Step 5: Add cross-owner helper isolation**

Activate one index owned by `durable_owner` and another owned by a new
`durable_owner_two`. After both have helper EXECUTE, call each private helper
as owner one with owner two's captured physical identity. Expect `must be
owner` and verify no level counts or workflow state changed.

- [ ] **Step 6: Add superuser-policy success**

Temporarily set:

```sql
ALTER SYSTEM SET pg_durable.enable_superuser_instances = on;
```

Restart, activate a postgres-owned background index, and assert that its
workflow executes successfully as `postgres`. Restore the setting afterward.

- [ ] **Step 7: Run the integration suite and commit**

```bash
PATH=/home/azureuser/pg17/bin:$PATH \
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_PACKAGE_DIR="$PG_DURABLE_PACKAGE_DIR" \
make test-durable
git add test/scripts/durable_compaction.sh
git commit -m "Cover background compaction permissions"
```

Expected: all permission scenarios pass with no leaked cluster or package
state.

---

### Task 3: Reconcile Physical Index Lifecycles

**Files:**
- Modify: `src/mod.c:700-945`
- Modify: `src/index/compaction_job.c:1470-1660`
- Modify: `src/index/compaction_job.h`
- Modify: `test/scripts/durable_compaction.sh`

**Interfaces:**
- Consumes: complete admission from Task 1.
- Produces:
  - `tp_compaction_job_preflight_index(Oid indexoid)`
  - post-utility activation for affected physical indexes
  - spill-time adoption when no workflow family exists.

- [ ] **Step 1: Add failing lifecycle scenarios**

Extend the durable integration suite with:

1. CREATE INDEX on a partitioned table with two partitions; assert two
   physical child workflows and no workflow for the partitioned parent.
2. ALTER TABLE OWNER from `durable_owner` to `durable_owner_two`; assert the
   old workflow becomes stale and a new workflow runs as owner two.
3. Repeat ownership transfer to each ineligible owner class and assert the
   command rolls back.
4. REINDEX INDEX and REINDEX TABLE; capture old relfilenumbers and instance
   IDs, assert new physical identities and replacement workflows.
5. Cancel/remove the current generation's workflow, create compaction debt,
   and verify a later spill adopts the valid generation.

- [ ] **Step 2: Run the integration suite and confirm lifecycle failures**

Run the Task 2 command. Expected: partition children, ownership transfer,
REINDEX, and fallback adoption lack replacement workflows.

- [ ] **Step 3: Factor physical-index collection in the utility hook**

Add helpers in `src/mod.c`:

```c
static List *tp_relation_tree_indexes(Oid relation_oid);
static List *tp_reindex_target_indexes(ReindexStmt *stmt);
static void tp_activate_background_indexes(List *indexoids,
                                           bool refresh_default);
static void tp_preflight_background_indexes(List *indexoids);
```

`tp_relation_tree_indexes()` must use `find_all_inheritors()` and
`RelationGetIndexList()` to include indexes on every partition, deduplicate OIDs,
and retain locks through command completion. Activation must filter for valid,
ready, live physical bm25 indexes in background mode.

- [ ] **Step 4: Reconcile partitioned CREATE INDEX**

Replace the current parent-only before/after lists with relation-tree index
lists. Diff the complete lists after PostgreSQL returns, then activate each new
physical background bm25 index. Keep CIC preflight behavior unchanged for
non-partitioned tables.

- [ ] **Step 5: Reconcile ownership changes**

Recognize `AT_ChangeOwner` for `OBJECT_INDEX` and table-like
`AlterTableStmt` objects. Collect affected physical indexes before execution,
run PostgreSQL's utility implementation, then activate surviving background
indexes with `refresh_default = true`. Activation failure must propagate so
transactional ownership changes roll back.

- [ ] **Step 6: Reconcile REINDEX**

Recognize `ReindexStmt`. Resolve affected physical background bm25 indexes for
INDEX, TABLE, SCHEMA, SYSTEM, and DATABASE scope. Preflight all of them before
calling PostgreSQL, then activate their current physical generations after
PostgreSQL returns. This preflight is mandatory even for non-concurrent
commands so concurrent and ordinary REINDEX share one path.

- [ ] **Step 7: Add spill-time fallback adoption**

Capture the current schedule while preparing a signal:

```c
tp_capture_target(indexoid, true, &target);
```

Keep reconciliation's active- and terminal-family lookup first. If neither
exists, create the current exact workflow using `target.schedule`. Before
signal reconciliation, apply the full owner policy and privilege checks, pin
the pg_durable dependency, and grant private-helper access.

- [ ] **Step 8: Run targeted and full integration tests**

```bash
PATH=/home/azureuser/pg17/bin:$PATH make
PATH=/home/azureuser/pg17/bin:$PATH make format-check
PATH=/home/azureuser/pg17/bin:$PATH \
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_PACKAGE_DIR="$PG_DURABLE_PACKAGE_DIR" \
make test-durable
```

Expected: all commands exit zero and old workflow identities never mutate a
new physical generation.

- [ ] **Step 9: Commit**

```bash
git add src/mod.c src/index/compaction_job.c src/index/compaction_job.h \
        test/scripts/durable_compaction.sh
git commit -m "Reconcile background compaction lifecycles"
```

---

### Task 4: Document and Validate the Complete Change

**Files:**
- Modify: `README.md:423-457`
- Modify: `ARCHITECTURE.md:90-123`

**Interfaces:**
- Consumes: Tasks 1-3 behavior.
- Produces: public prerequisites and lifecycle guarantees matching the
  implementation.

- [ ] **Step 1: Update concise user documentation**

Add database `CONNECT` and extension-schema `USAGE` to the README prerequisite
sentence. State in one sentence that ownership changes, REINDEX, and
partitioned index creation reconcile physical workflows automatically.

- [ ] **Step 2: Update architecture documentation**

Document deterministic owner admission, post-utility generation
reconciliation, stale-workflow retirement, and spill-time fallback adoption.
Remove the claim that only CREATE INDEX and ALTER INDEX reloptions are
reconciled.

- [ ] **Step 3: Run PostgreSQL 17 validation**

```bash
PATH=/home/azureuser/pg17/bin:$PATH make clean
PATH=/home/azureuser/pg17/bin:$PATH make
PATH=/home/azureuser/pg17/bin:$PATH make format-check
PATH=/home/azureuser/pg17/bin:$PATH make installcheck
test ! -s test/regression.diffs
PATH=/home/azureuser/pg17/bin:$PATH \
PG_CONFIG=/home/azureuser/pg17/bin/pg_config \
PG_DURABLE_PACKAGE_DIR="$PG_DURABLE_PACKAGE_DIR" \
make test-durable
```

Expected: build and formatting succeed, all regression tests pass, no
`test/regression.diffs` remains, and the durable integration suite passes.

- [ ] **Step 4: Run PostgreSQL 18 validation**

```bash
PATH=/home/azureuser/pg18/bin:$PATH make clean
PATH=/home/azureuser/pg18/bin:$PATH make
PATH=/home/azureuser/pg18/bin:$PATH make format-check
PATH=/home/azureuser/pg18/bin:$PATH make installcheck
test ! -s test/regression.diffs
```

Expected: build, formatting, and all regression tests pass.

- [ ] **Step 5: Commit documentation**

```bash
git add README.md ARCHITECTURE.md
git commit -m "Document managed compaction reconciliation"
```

- [ ] **Step 6: Review and push**

Run a whole-change review against `origin/main`, address every confirmed
finding, push `background-compaction-5-backstop`, and verify #478 CI and review
threads are clean.

