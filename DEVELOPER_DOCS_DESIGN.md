# Developer Documentation Cleanup

## Goal

Make the public developer documentation concise, current, and self-contained.
Remove the `docs/` directory and retain only implementation guidance that is
useful to contributors.

## Scope

- Remove the Project History section from `README.md`.
- Rewrite `CONTRIBUTING.md` as the developer entry point.
- Replace `test/README.md` with a short guide based on current Makefile
  targets.
- Delete `docs/background_compaction.md`, `docs/memtable_cache.md`, and
  `docs/memtable_v2.md`.
- Update references that would otherwise point to the deleted files.

`RELEASING.md`, `SECURITY.md`, `scripts/README.md`, and
`benchmarks/README.md` remain outside this pass.

## Contributor Guide

`CONTRIBUTING.md` will retain:

- development setup and core build/test commands;
- PostgreSQL code style and include conventions;
- the source-layer overview;
- pull request and issue-reporting essentials;
- a one-sentence note that pg_textsearch was originally named Tapir.

It will also consolidate these durable implementation invariants:

- The WAL-logged on-disk memtable chain is the source of truth; the
  shared-memory cache is derived and disposable.
- Cache allocation follows the per-index, global-soft, and global-hard budget
  tiers, with the documented lock ordering.
- Each live heap TID occurs in at most one published segment. Segment-local
  numeric `doc_id` values may repeat across segments.
- Compaction is size-bounded and publishes replacements atomically.
  Published physical changes are not undone by transaction rollback.
- Deferred page reclaim must respect the oldest non-removable transaction
  horizon; query-serving standbys require `hot_standby_feedback = on`.

Detailed page layouts, historical migration designs, benchmark snapshots, and
superseded implementation proposals will not be retained.

## Test Guide

`test/README.md` will describe:

- SQL regression, shell, replication, stress, and optional test categories;
- the current Makefile targets for running those categories;
- how to add a SQL regression test and update expected output;
- concise failure-diagnosis steps.

It will not maintain exhaustive test-file inventories, predicted outcomes,
performance estimates, or duplicated CI descriptions.

## Reference Cleanup

References in source comments, SQL comments, expected output, `CLAUDE.md`, and
the README will point to the relevant `CONTRIBUTING.md` section or become
self-contained. No tracked file may retain a `docs/` reference after the
directory is removed.

## Validation

- Search the repository for stale `docs/` links and obsolete Tapir branding.
- Check Markdown links, anchors, and code fences.
- Keep SQL comments and expected output synchronized.
- Run formatting checks, source guards, and the build.
- Review the final diff for accuracy, concision, and duplicated guidance.
