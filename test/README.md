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
| `make test` | SQL regression suite and source guards |
| `make installcheck` | Standard PGXS regression run and source guards |
| `make test-local` | Install and test in a temporary cluster on port 55433 |
| `make test-shell` | Concurrency, recovery, segment, CIC, multi-index, and reindex |
| `make test-all` | `make test` plus `make test-shell` |
| `make test-concurrency` | Multi-backend concurrency and VACUUM/merge races |
| `make test-recovery` | Crash, shutdown-spill, reclaim, and compaction recovery |
| `make test-segment` | Multi-backend segment tests |
| `make test-stress` | Long-running stress workload |
| `make test-replication` | Basic physical replication |
| `make test-replication-extended` | Extended physical replication and WAL audit |
| `make test-logical-replication` | Logical replication |
| `make test-cic` | `CREATE INDEX CONCURRENTLY` |
| `make test-multi-index` | Multi-index, user, and schema behavior |
| `make test-reindex` | Multi-backend reindex invalidation |
| `make test-chinese` | Optional zhparser regression |

`make test-shell` is the standard shell suite. Replication and stress targets
are separate because they create additional PostgreSQL instances or run for an
extended period.

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
