# BM25 Row-Level Security Restriction Design

## Goal

Document the term-frequency leakage risk when BM25 indexes are used with
PostgreSQL row-level security (RLS), and add an administrator-controlled
setting that can prevent new RLS/BM25 combinations.

## Configuration

Add `pg_textsearch.allow_rls`, a boolean GUC with these properties:

- Default: `on`
- Context: `PGC_SUSET`
- When `on`, existing behavior is unchanged.
- When `off`, creating or rebuilding a BM25 index over an RLS-protected
  relation fails, and enabling RLS on a relation that already has a BM25 index
  fails.

Superusers are subject to the restriction while the GUC is off, but can
explicitly enable it. Existing RLS/BM25 combinations remain usable for scans,
writes, and other maintenance that does not rebuild the index.

## RLS and Inheritance Semantics

A relation is RLS-protected when RLS is enabled on the relation itself or on
an inheritance or partition ancestor. This prevents a physical child index
from bypassing a restriction configured for its parent.

Enabling RLS on a relation is rejected when that relation or any inheritance
or partition descendant has a BM25 index. This covers indexes used while
scanning a protected partitioned or inherited hierarchy.

## Enforcement

Use a shared helper for catalog traversal and BM25 index detection.

The `ProcessUtility` hook performs user-facing DDL checks:

- For `CREATE INDEX ... USING bm25`, reject an RLS-protected target before
  starting the build.
- For `ALTER TABLE ... ENABLE ROW LEVEL SECURITY`, reject the command when
  the target hierarchy contains a BM25 index.

The BM25 `tp_build()` access-method callback repeats the RLS-protection check
as the authoritative defense-in-depth guard. This covers partition child
builds, concurrent builds, `REINDEX`, and build paths that do not originate
from the expected utility statement shape.

### Concurrent DDL Serialization

The catalog checks are quick, but they must be atomic with respect to other
DDL that can create the opposite half of an RLS/BM25 combination. Otherwise,
one transaction can observe no RLS ancestor while another observes no BM25
descendant, and both can commit.

When `pg_textsearch.allow_rls` is off, relevant utility statements acquire one
transaction-scoped exclusive PostgreSQL object lock on the `pg_textsearch`
extension before core PostgreSQL acquires relation locks. This serializes BM25
index creation and rebuilds with RLS enablement and hierarchy attachment
within the database.

Hierarchy traversal uses relation locks only while each catalog object is
being inspected and releases them immediately. It does not retain a lock on
every ancestor or descendant until transaction end. The extension object lock
therefore provides the serialization point without reversing PostgreSQL's
parent-to-child DDL lock order or consuming one shared lock-table entry per
relation in a large hierarchy.

Errors use `ERRCODE_FEATURE_NOT_SUPPORTED`, identify the conflicting
relation, name `pg_textsearch.allow_rls`, and hint that enabling the setting
accepts the documented term-frequency leakage risk.

## Documentation

Keep the README addition terse:

- Add `pg_textsearch.allow_rls` to the Settings table.
- Add a Row-Level Security subsection under Limitations.
- Explain that BM25 corpus statistics include all indexed rows, so a user who
  already knows a term can infer frequency information influenced by rows
  hidden by RLS, while the index does not reveal unknown terms.
- Link to Elastic's analogous limitation:
  <https://www.elastic.co/docs/deploy-manage/security/limitations>.
- State what setting `pg_textsearch.allow_rls = off` prevents.
- Briefly note that the setting does not disable combinations that already
  exist when it is turned off.

## Tests

Regression coverage will verify:

- The GUC defaults to `on`.
- Ordinary users cannot change the `PGC_SUSET` GUC.
- Default-on behavior permits a BM25 index on an RLS table.
- With the GUC off, BM25 `CREATE INDEX` and `REINDEX` fail on an
  RLS-protected relation.
- With the GUC off, enabling RLS fails when the relation hierarchy already
  contains a BM25 index.
- Switching the GUC off does not prevent scans or writes through an existing
  RLS/BM25 combination.
- Partition and inheritance hierarchies are protected in both directions.
- Relevant concurrent DDL waits on the extension object lock before acquiring
  relation locks.
- Successful checks do not retain relation locks across every traversed
  ancestor or descendant.

The tests will use the existing SQL regression framework and assert the
resulting errors in the expected output.
