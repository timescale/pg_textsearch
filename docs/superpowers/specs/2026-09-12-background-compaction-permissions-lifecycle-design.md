# Background Compaction Permission and Lifecycle Design

## Goal

Managed background compaction must start only when pg_durable can execute as
the index owner, and it must remain usable across ordinary PostgreSQL index
lifecycle operations.

## Admission

Before creating or adopting a workflow, pg_textsearch validates the index
owner:

- the role exists and has `LOGIN`;
- the role has `CONNECT` on the current database;
- a superuser owner is allowed by
  `pg_durable.enable_superuser_instances`;
- the role has the required pg_durable schema, function, table, and column
  privileges; and
- the role has `USAGE` on the schema containing pg_textsearch's private
  physical-target helpers.

Admission failures are deterministic PostgreSQL errors. CREATE INDEX and ALTER
INDEX failures leave no index shell or reloption change, workflow, extension
dependency, or private-helper grant.

pg_textsearch continues to grant private-helper `EXECUTE` only to admitted
index owners. The helpers independently require index ownership and validate
the captured database, relation, tablespace, relfilenumber, owner, and
background mode.

## Execution Identity

Workflow validation, creation, lookup, and signaling run as the index owner,
not as the DDL or write-transaction actor. pg_durable SQL activities therefore
connect and execute with `current_user` equal to the captured index owner.

A user who is permitted by PostgreSQL to administer the owner's index may
enable background mode. A separate writer may trigger a spill signal without
acquiring the owner's pg_durable authority.

## Lifecycle Reconciliation

The current physical generation remains the source of truth. Old workflows
become stale through the existing physical-identity guard and exit without
touching a replacement relation.

Utility handling reconciles all affected physical background indexes:

- CREATE INDEX on a partitioned table activates each physical child index;
- ALTER OWNER validates and activates workflows for the new physical owners;
- REINDEX validates affected owners before any concurrent irreversible phase
  and activates workflows for the rebuilt physical generations afterward.

The command's native scope is preserved for REINDEX INDEX, TABLE, SCHEMA,
SYSTEM, and DATABASE. Non-bm25 and non-background indexes are ignored.

As a defensive fallback, a spill from a valid background index may adopt the
current generation when no matching workflow history exists. Fallback adoption
performs the complete admission check, dependency pin, and helper grant before
creating or signaling a workflow.

## Tests

The pg_durable integration suite will cover:

- missing database `CONNECT`;
- missing pg_textsearch-schema `USAGE`;
- isolated missing pg_durable privileges;
- atomic ALTER-to-background failures for every admission class;
- successful superuser ownership when the pg_durable policy is enabled;
- a member or administrator enabling an index owned by another role;
- a non-owner writer triggering spill signaling;
- worker `current_user` matching the index owner;
- cross-owner private-helper isolation after both owners receive `EXECUTE`;
- partitioned index creation and physical-child workflows;
- ownership transfer to eligible and ineligible owners;
- REINDEX INDEX and REINDEX TABLE generation replacement; and
- spill-time fallback adoption for an otherwise unreconciled generation.

Assertions cover reloptions, workflow identity and count, helper grants,
extension dependencies, stale-workflow behavior, physical compaction, and
transactional rollback.

