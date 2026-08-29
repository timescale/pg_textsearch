# Per-index background compaction with pg_durable

These operator-managed scripts adapt pg_textsearch's per-index compaction
callback to [pg_durable](https://github.com/microsoft/pg_durable). The
extension does not create cluster roles, change authentication, or depend on
pg_durable at build or install time.

This adapter requires a released pg_durable build that provides:

```sql
df.loop(body text, condition text, on_error text)
df.start(..., transaction_mode => 'new')
```

No released pg_durable version contains the three-argument `df.loop` at the
time this draft was created. Both scripts check the exact extension-owned
catalog signature before any managed side effect. Once upstream publishes
the capability, update the localized preflight comments and add the released
minimum version without changing the wrapper body.

## Prerequisites

Preload both libraries, select the application database for pg_durable, and
restart PostgreSQL:

```conf
shared_preload_libraries = 'pg_durable,pg_textsearch'
pg_durable.database = 'application_database'
```

Install both extensions and the BM25 indexes in that database. Choose a
non-superuser role that owns the indexes and one or more non-superuser writer
roles.

pg_durable opens worker connections as the submitted role without supplying
a password. `textsearch_compactor` is intentionally passwordless. Configure
a scoped peer/ident mapping (or an equivalent authenticated passwordless
route) and set `PGHOST` in the PostgreSQL service environment to the socket
directory. Do not use `trust` outside a disposable test cluster.

Revoke `CREATE` on `public` from every untrusted role before running the
scripts. The request wrapper is a `SECURITY DEFINER` function in that schema.

## Install

Run both scripts as a database superuser in `pg_durable.database`:

```sh
psql -d application_database \
  -f scripts/durable_compaction/01_setup_role.sql \
  -v index_owner=app_owner

psql -d application_database \
  -f scripts/durable_compaction/02_wrapper.sql \
  -v writer_role=app_writer
```

Run `01_setup_role.sql` once for every role that owns BM25 indexes. It creates
`textsearch_compactor` only when absent, then grants inherited owner
membership with `SET FALSE`. Existing roles fail closed if they have
credentials, privileged attributes, an unsafe connection limit or role
setting, unexpected membership paths, inbound members, or unexpected owned
objects anywhere in the cluster.

A clean rerun permits only the exact managed
`public.bm25_request_compaction(regclass)` ownership dependency. The
allowlist authenticates its body hash, language, signature, owner,
`SECURITY DEFINER` flag, fixed `search_path`, volatility, parallel safety,
leakproof state, and ACL. `PUBLIC` execution and non-owner grant options are
rejected rather than silently repaired.

`02_wrapper.sql` also fails closed on a hostile or drifted existing wrapper.
Before replacing an absent or exact managed wrapper, it commits a real
`SELECT 1` task as `textsearch_compactor` and waits for completion. This
checks worker authentication and execution, not just catalog shape.
Reinstallation then recreates the wrapper to discard stale ACLs, removes
named default-privilege grants, and grants non-delegable `EXECUTE` only to
the selected writer. Omit `writer_role` to grant no writer.

## Configure

After the callback-dispatch branch is present, configure settings at a scope
visible to both writers and worker sessions:

```conf
pg_textsearch.compaction_mode = 'background'
pg_textsearch.compaction_request_function = \
    'public.bm25_request_compaction'
pg_textsearch.segments_per_level = 8
```

The wrapper accepts only BM25 indexes. It rejects temporary indexes and
requires `session_user` to have `INSERT` on the indexed table or one of its
partition ancestors. Approved writers receive wrapper execution only; they
do not receive `df` schema access or execution on the private physical-target
helpers.

Each request captures the database OID, relation OID, tablespace OID, and
relfilenumber. The durable loop calls only:

```sql
bm25_compact_step_if_current(oid, oid, oid, oid)
bm25_needs_compaction_if_current(oid, oid, oid, oid)
```

A dropped, recreated, reindexed, moved, temporary, or wrong-access-method
target returns `false` without touching replacement storage. A live target
still enforces index ownership.

The wrapper submits:

```sql
df.start(
    df.loop(body, condition, on_error => 'continue'),
    label => 'bm25-compact-' || index_oid,
    transaction_mode => 'new')
```

`transaction_mode => 'new'` persists the request in an independent
transaction, which is required because pg_textsearch invokes callbacks from a
late transaction callback and discards their local transactional effects.
`on_error => 'continue'` lets a concurrent compactor, transient lock error,
or other failed step return to the physical-state condition instead of
stranding the loop immediately. Every iteration revalidates physical
identity and compaction debt.

This slice provides per-index submission only. It does not install a
recurring schedule or fleet-wide sweep.

## Test

The static shape checks require only the source tree:

```sh
make test-durable-static
```

The live harness creates a disposable local cluster and requires compatible
pg_durable and pg_textsearch libraries installed for the selected
`PG_CONFIG`:

```sh
make install
make test-durable
```

The harness pins every PostgreSQL binary to that `PG_CONFIG`, checks role and
wrapper fail-closed behavior, verifies owner-only helper ACLs, and confirms
that a direct request survives caller rollback.
