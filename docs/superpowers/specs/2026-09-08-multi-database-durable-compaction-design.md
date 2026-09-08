# Multi-Database Durable Compaction Rejection Design

**Date:** 2026-09-08

## Goal

Keep pg_textsearch background compaction limited to the database where
pg_durable is installed, and prove that a BM25 index in another database is
rejected deterministically without leaving partial activation state.

pg_durable supports executing SQL in another target database after a workflow
is submitted in its control database. It does not expose `df.start`,
`df.signal`, or workflow metadata in other databases. Its documented
cross-database caller path requires dblink or postgres_fdw, neither of which
can make pg_textsearch activation atomic with target-database DDL.

## Cluster Topology

The existing `durable_compaction.sh` cluster will contain:

- `durable_compaction_test`, the configured pg_durable control database with
  pg_durable and pg_textsearch installed. Existing scenario A coverage remains
  unchanged.
- `durable_compaction_remote`, a second database with pg_textsearch installed
  and no pg_durable extension objects.

`pg_durable.database` continues to name `durable_compaction_test`.

## Scenario B Test Flow

1. Create `durable_compaction_remote` and install only pg_textsearch.
2. Assert pg_durable is installed in the control database and absent from the
   remote database.
3. Configure the existing login owner in the remote database.
4. Record the control database's managed-workflow count.
5. Attempt `CREATE INDEX CONCURRENTLY` with
   `compaction = 'background'` in the remote database.
6. Assert the command reports that pg_durable is not initialized for the
   current database and identifies the configured-database mismatch.
7. Assert the rejected concurrent build leaves no index relation and creates
   no control-database workflow.
8. Create a valid manual BM25 index in the remote database, then attempt
   `ALTER INDEX ... SET (compaction = 'background')`.
9. Assert the ALTER fails transactionally, the index remains in manual mode,
   and no workflow, helper grant, or dependency is created.
10. Confirm manual compaction remains usable in the remote database.

All workflow-count assertions run in the control database. All index,
reloption, privilege, and dependency assertions run in the remote database.

## Error Contract

The remote-database rejection must be distinguishable from a cluster where
pg_durable is not installed at all:

```text
ERROR:  pg_durable is not initialized for this database
DETAIL: pg_durable.database does not name the current database
```

The check therefore occurs before local pg_durable extension-object discovery.
The existing same-database missing-extension case retains:

```text
ERROR:  background compaction requires pg_durable 0.2.8 or newer
```

Neither error may silently fall back to inline compaction.

## Implementation

### Test helpers

Extend `durable_compaction.sh` with database-parameterized SQL helpers while
preserving the existing `sql_as` and `sql_super` call sites for the control
database.

Add remote helpers for:

- executing SQL as a role in a named database;
- counting the remote pg_textsearch-to-pg_durable dependency;
- checking remote helper grants; and
- reading remote index reloptions and compaction debt.

### Admission ordering

In `tp_discover_job_objects()`, read `pg_durable.database` and compare it with
the current database before resolving the local pg_durable extension. When the
configured database differs, raise the stable initialization error above.

When the names match, continue with existing local extension, version,
preload, object-ownership, privilege, and workflow checks.

This is an error-ordering change only. It does not add cross-database
submission or relax any admission requirement.

### Documentation

State explicitly that pg_textsearch background compaction requires pg_durable
to be installed in the same database as the BM25 index. pg_durable's ability
to target another database does not make its control APIs available there.

Users who need a separate pg_durable control database must use manual
pg_textsearch compaction until a transaction-safe cross-database integration
exists.

## Safety Assertions

The scenario B test must prove:

- no concurrent-index shell survives rejection;
- ALTER rolls back to the prior manual reloption;
- no control-database workflow is created;
- no remote helper EXECUTE grant is created;
- no remote dependency is recorded;
- manual compaction still clears debt; and
- scenario A remains unchanged and green.

## Non-Goals

- dblink or postgres_fdw submission from pg_textsearch.
- A native pg_durable cross-extension bridge.
- Cross-database dependency or cascade behavior.
- Multiple pg_durable installations in one cluster.
- Changes to pg_durable PR #377.

## Validation

Run the focused scenario B case RED before changing admission ordering, then
GREEN afterward. Run the complete live scenario A and B integration on
PostgreSQL 17 and 18. The coverage job must run both scenarios against the
released pg_durable version before #478 is merged.

## Success Criteria

Scenario A continues to pass. Scenario B fails early with the documented
current-database error and leaves the remote database usable in manual mode
with no durable activation residue.
