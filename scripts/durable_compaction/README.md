# Per-index background compaction with pg_durable

These operator-managed scripts adapt pg_textsearch's per-index compaction
callback to [pg_durable](https://github.com/microsoft/pg_durable). The
extension does not create cluster roles, change authentication, or depend on
pg_durable at build or install time.

> **Merge blocker:** do not merge this adapter until
> [microsoft/pg_durable#354](https://github.com/microsoft/pg_durable/pull/354)
> lands in a release. That change currently targets pg_durable 0.2.7.

The required pg_durable build provides:

```sql
df.loop(body text, condition text)
df.start(
    ...,
    transaction_mode => 'new',
    max_attempts => 5,
    max_backoff => '16 seconds',
    on_failure => 'continue')
```

Released pg_durable v0.2.6 and current upstream `main` do not contain the
failure policy. Both scripts check the exact seven-argument,
extension-owned `df.start` catalog signature before any managed side effect.
Once #354 is released, replace the localized capability-only message with the
released minimum version.

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
    df.loop(body, condition),
    label => 'bm25-compact-' || index_oid,
    transaction_mode => 'new',
    max_attempts => 5,
    max_backoff => '16 seconds',
    on_failure => 'continue')
```

`transaction_mode => 'new'` persists the request in an independent
transaction, which is required because pg_textsearch invokes callbacks from a
late transaction callback and discards their local transactional effects.
The #354 instance policy retries a failed node up to five times with capped
exponential backoff. Once those attempts are exhausted,
`on_failure => 'continue'` abandons that iteration and starts the next one.
Every iteration revalidates physical identity and compaction debt.

This slice provides per-index submission only. It does not install a
recurring schedule or fleet-wide sweep.

The recurring registration planned for the next PR must use the same #354
`df.start(..., on_failure => 'continue')` policy. It must not add policy
arguments to `df.loop`.

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
