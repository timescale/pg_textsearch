# Testing

## Quick Start

Choose the target that matches the environment:

```bash
make installcheck    # installed extension, existing PostgreSQL server
make test-local      # install and run against a temporary local cluster
make test-all        # SQL regression plus the standard shell suite
```

The extension must be built for the selected PostgreSQL installation. Shell
targets assume it is already installed.

`make test-local` runs `make install` before `initdb`, so the invoking user
must be non-root and able to write to the selected PostgreSQL installation
prefix.

## Test Targets

The Makefile defines these entry points:

| Target | Coverage |
| --- | --- |
| `make test` | SQL regression suite |
| `make installcheck` | Standard PGXS regression run |
| `make test-local` | Install and test in a temporary cluster on port 55433 |
| `make test-shell` | Concurrency, recovery, segment, CIC, multi-index, and reindex |
| `make test-all` | `make test` plus `make test-shell` |
| `make test-compression` | Standalone scalar/AVX2 decoder and dispatch coverage |
| `make test-concurrency` | Multi-backend concurrency and VACUUM/merge races |
| `make test-memtable-stale-tail` | Corrupt-tail rejection and REINDEX repair |
| `make test-recovery` | Crash, shutdown-spill, reclaim, and compaction recovery |
| `make test-segment` | Multi-backend segments and parallel-build bulk reads |
| `make test-stress` | Long-running stress workload |
| `make test-replication` | Basic physical replication |
| `make test-replication-extended` | Extended physical replication and WAL audit |
| `make test-logical-replication` | Logical replication |
| `make test-cic` | `CREATE INDEX CONCURRENTLY` |
| `make test-multi-index` | Multi-index, user, and schema behavior |
| `make test-reindex` | Rebuild, rollback, and prepared-transaction caches |
| `make test-cross-database-registry` | Database isolation and DROP cleanup |
| `make test-drop-rollback` | Multi-backend DROP rollback and deferred cleanup |
| `make test-injection-sql` | Behavior-specific SQL regressions requiring injection points |
| `make test-injection-shell` | Crash and concurrency injection-point tests |
| `make test-chinese` | Optional zhparser regression |

The compression harness checks all supported widths, counts, and input
alignments through runtime dispatch and the scalar fallback. It also rebuilds
with the CPU probe forced to report no AVX2 support; no backend is required.

`test/scripts/parallel_build_bulkread.sh` starts a private 16MB-buffer cluster
and builds an index with two workers on a heap large enough for a bulk-read
strategy. It checks complete, duplicate-free results and asserts worker
buffer reuse through `pg_stat_io`, catching scans that fill shared buffers
instead of reusing the bulk-read ring.

`make test-shell` is the standard shell suite. Replication and stress targets
are separate because they create additional PostgreSQL instances or run for an
extended period.

## Injection Points

Injection-point tests require PostgreSQL configured with
`--enable-injection-points`. The Makefile detects this through
`enable_injection_points` and, when it is set, adds the injection
regressions to `REGRESS` and installs the `pg_textsearch_test` helper
extension as part of `make install`. On ordinary packaged PostgreSQL
builds, these tests are omitted.

Scripts that mix injection-point cases with ordinary ones — such as
`standby_reclaim.sh`, `compaction_recovery.sh`, and
`vacuum_concurrent_merge.sh` — skip only the cases that need a
deterministic pause, so they stay useful on packaged builds.
`nonblocking_compaction.sh` needs injection points throughout and skips
entirely.

`nonblocking_spill.sh` also races force-merge suffix truncation with spills
in both lock-acquisition orders. It pauses between suffix inspection and
truncation, checks writer-gate exclusion, and verifies segment page references
and exact ranked results before and after crash recovery.

CI builds PostgreSQL 17, 18, and 19 with injection points enabled and caches
the installed prefixes. The sanitizer builds use the same configure option.

`memtable_stale_tail_injection` pauses normal and oversized writers after
reading the tail, advances it from another session, then requires both writers
to retry successfully. `make test-memtable-stale-tail` covers a corrupt tail
and REINDEX repair without requiring injection points.

`tombstone_bounds_injection` corrupts head and next-page links at block zero,
EOF, and past EOF. It checks diagnostic errors and drain recovery while a
pinned horizon keeps the valid chain prefix parked.

## Sanitizers

Pull-request CI builds PostgreSQL 17.2 and 18.1 and pg_textsearch with Clang
AddressSanitizer and UndefinedBehaviorSanitizer. It runs SQL regression;
concurrency, duplicate-read, and VACUUM/merge race tests; crash, deferred
reclaim, and shutdown-spill recovery tests; multi-backend segment tests;
`CREATE INDEX CONCURRENTLY`; multi-index, user, and schema tests; and standby
segment-reclaim coverage.

The nightly stress workflow enables leak detection. There is no local
`make sanitizer` target;
[`.github/workflows/ci.yml`](https://github.com/timescale/pg_textsearch/blob/main/.github/workflows/ci.yml)
is the canonical reproduction recipe. The standalone
[`sanitizer-build-and-test.yml`](https://github.com/timescale/pg_textsearch/blob/main/.github/workflows/sanitizer-build-and-test.yml)
workflow runs on `main`; pull-request sanitizer coverage is consolidated in
`.github/workflows/ci.yml`.

## Adding SQL Tests

1. Add `test/sql/<name>.sql`.
2. Add `<name>` to `REGRESS` in `Makefile`.
3. Run `make installcheck REGRESS=<name>`.
4. Inspect `test/results/<name>.out` and `test/regression.diffs`.
5. Copy the reviewed output to `test/expected/<name>.out`.
6. Rerun `make installcheck REGRESS=<name>`.

Keep fixtures deterministic and include only output that the test intends to
verify.

Use temporary tables for single-session tests that require immediate VACUUM
cleanup or page reuse. Their reclaim horizon is session-local, so unrelated
transactions and standby feedback cannot retain dead tuples or pages. Keep
shared-buffer, WAL, and standby-reclaim coverage in permanent-table tests
such as `segment_reclaim_injection.sql` and `standby_reclaim.sh`.

## Debugging Failures

Inspect `test/regression.diffs` first, then compare the generated file under
`test/results/` with its counterpart under `test/expected/`. Rerun a single SQL
test with:

```bash
make installcheck REGRESS=<name>
```

For `make test-local`, PostgreSQL logs are written to
`tmp_check_shared/data/logfile` while the temporary cluster exists. Shell tests
print their commands and diagnostics directly; rerun the corresponding
individual target to isolate a failure.
