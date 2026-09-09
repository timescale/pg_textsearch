# Contributing to pg_textsearch

pg_textsearch was originally named Tapir (Textual Analysis for Postgres
Information Retrieval), which remains the project mascot and appears in some
source names.

## Development Setup

Install PostgreSQL 17 or 18 and its development headers, then build and install
the extension:

```bash
make
make install
```

Add `pg_textsearch` to `shared_preload_libraries`, restart PostgreSQL, and run
the installed-extension regression suite:

```bash
make installcheck
```

See [test/README.md](test/README.md) for a temporary-cluster target and the
specialized test suites.

## Before Submitting

Run:

```bash
make
make installcheck
make format-check
```

Changes to storage, WAL, the memtable cache, compaction, or deferred reclaim
also require the relevant shell and replication tests documented in
[test/README.md](test/README.md). If output changes intentionally, review and
update the corresponding file under `test/expected/`.

## Code Style

Follow the
[PostgreSQL coding conventions](https://www.postgresql.org/docs/current/source-format.html):

- Wrap lines at 79 characters and indent with tabs.
- Use Allman braces, with opening braces on a new line.
- Put `postgres.h` first in C source files.
- Write project includes as paths relative to `src/`, such as
  `#include "segment/segment.h"`.

Use `make format` to apply formatting and `make format-check` to verify it.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the source layout, storage design,
and invariants that changes must preserve.

## Pull Requests

Open focused pull requests against `main`. Explain the problem and approach,
list the tests run, note user-visible or compatibility effects, and link
related issues. Keep commits clear and scoped to the change.

Performance work should include relevant measurements; see
[benchmarks/README.md](benchmarks/README.md) for the benchmark tooling.

## Reporting Issues

Search existing issues first. Bug reports should include PostgreSQL and
pg_textsearch versions, operating system, reproduction steps, expected and
actual behavior, and relevant logs. Feature requests should describe the
problem, proposed outcome, and alternatives considered.

Use [GitHub Issues](https://github.com/timescale/pg_textsearch/issues) for bugs
and feature requests, and
[GitHub Discussions](https://github.com/timescale/pg_textsearch/discussions)
for general questions.

## License

By contributing, you agree that your contributions are licensed under the
PostgreSQL License.
