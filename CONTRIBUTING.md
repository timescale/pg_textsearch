# Contributing to pg_textsearch

Thank you for your interest in contributing to pg_textsearch! This document
provides guidelines for contributing to the project.

## Getting Started

### Development Setup

1. Clone the repository:
   ```sh
   git clone https://github.com/timescale/pg_textsearch
   cd pg_textsearch
   ```

2. Install PostgreSQL 17 or 18 with development headers:
   ```sh
   # Ubuntu/Debian
   sudo apt install postgresql-server-dev-17  # or 18

   # macOS with Homebrew
   brew install postgresql@17  # or @18
   ```

3. Build the extension:
   ```sh
   make
   make install  # may need sudo
   ```

4. Run tests:
   ```sh
   make installcheck
   ```

### Pre-commit Hooks (Recommended)

Install pre-commit hooks to automatically check formatting:

```sh
# macOS
brew install pre-commit && pre-commit install

# Linux/pip
pip install pre-commit && pre-commit install
```

## Making Changes

### Code Style

We follow PostgreSQL coding conventions. Key points:

- **Line limit**: 79 characters
- **Indentation**: Tabs
- **Brace style**: Allman (opening braces on new lines)
- **Naming**: snake_case for functions and variables
- **Comments**: 2 spaces before trailing comments
- **Headers**: Use `#pragma once` instead of include guards
- **Includes**: `postgres.h` must be the first include, followed by standard
  library headers (with `<>`), then project headers (with `""`)

See the [PostgreSQL coding conventions](https://www.postgresql.org/docs/current/source-format.html)
for more details.

Format your code before committing:
```sh
make format        # auto-format all source files
make format-check  # check formatting without changes
```

### Testing Requirements

Before submitting a pull request:

1. **Build**: `make` must succeed without errors
2. **Tests**: `make installcheck` must pass
3. **Concurrency**: `make test-concurrency` should pass
4. **Formatting**: `make format-check` must pass

If you modify error messages, update the corresponding expected output files
in `test/expected/`.

## Benchmarks

Automated benchmarks run weekly and can be triggered on-demand. The benchmark
suite uses public IR datasets to measure indexing and query performance.

### Running Benchmarks On-Demand

Trigger benchmarks manually using the GitHub CLI:

```sh
# Full MS MARCO benchmark (8.8M passages, ~13 minutes)
gh workflow run benchmark.yml -f dataset=msmarco -f msmarco_size=full

# Quick test with smaller subset (1M passages, ~4 minutes)
gh workflow run benchmark.yml -f dataset=msmarco -f msmarco_size=1M

# Run all datasets (MS MARCO + Wikipedia)
gh workflow run benchmark.yml -f dataset=all

# Wikipedia only (configurable size: 10K, 100K, 1M, full)
gh workflow run benchmark.yml -f dataset=wikipedia -f wikipedia_size=100K
```

### Viewing Results

Check benchmark status and results:

```sh
# List recent benchmark runs
gh run list --workflow=benchmark.yml

# View a specific run
gh run view <run-id>

# Download benchmark artifacts (includes JSON metrics)
gh run download <run-id>
```

Each run produces:
- `benchmark_results.txt` - Full output log
- `benchmark_metrics.json` - Structured metrics for comparison
- `benchmark_summary.md` - Formatted summary

### Local Benchmarks

Run benchmarks locally using the benchmark runner:

```sh
cd benchmarks

# Run Cranfield (quick validation, ~1400 docs)
./runner/run_benchmark.sh cranfield --download --load --query

# Run MS MARCO locally (requires ~4GB disk space for full dataset)
./runner/run_benchmark.sh msmarco --download --load --query --report
```

### Benchmark Datasets

| Dataset | Documents | Description |
|---------|-----------|-------------|
| Cranfield | 1,400 | Classic IR test collection (quick validation) |
| MS MARCO | 8.8M | Microsoft passage ranking dataset |
| Wikipedia | Configurable | Wikipedia article extracts |

### Performance Dashboard

Historical benchmark results are tracked and published to GitHub Pages:

**Dashboard URL**: https://timescale.github.io/pg_textsearch/benchmarks/

The dashboard shows:
- **Index Build Time** - Time to build the BM25 index
- **Query Latencies** - Per-query execution times (short, medium, long queries)
- **Average Throughput** - Mean latency across 20 representative queries

### Regression Alerts

Performance is automatically monitored:

- **PRs**: Cranfield benchmarks run on every PR touching `src/` or `benchmarks/`.
  Results are posted as PR comments comparing against the baseline.
- **Weekly**: Full MS MARCO benchmarks run every Sunday, updating the baseline.
- **Releases**: A benchmark gate runs before each release with a stricter 120%
  threshold. Releases are blocked if performance regresses significantly.

Alert thresholds:
- **PRs and weekly**: 150% of baseline (warn but don't fail)
- **Releases**: 120% of baseline (blocks release)

### Commit Guidelines

- Write clear, concise commit messages
- Focus on the "why" rather than the "what"
- Reference related issues when applicable

## Pull Request Process

1. Fork the repository and create a feature branch
2. Make your changes with appropriate tests
3. Ensure all tests pass locally
4. Submit a pull request to the `main` branch

All pull requests are automatically tested against PostgreSQL 17 and 18.

### PR Description

Include:
- A brief summary of changes
- Testing steps or notes
- Any breaking changes or migration notes

## Reporting Issues

### Bug Reports

When reporting bugs, please include:
- PostgreSQL version
- pg_textsearch version
- Operating system
- Steps to reproduce
- Expected vs actual behavior
- Relevant error messages or logs

### Feature Requests

For feature requests, describe:
- The problem you're trying to solve
- Your proposed solution (if any)
- Any alternatives you've considered

## Code of Conduct

Be respectful and constructive in all interactions. We're building something
together.

## Questions?

- Open a [GitHub Discussion](https://github.com/timescale/pg_textsearch/discussions)
  for general questions
- Check existing issues before opening new ones

## License

By contributing to pg_textsearch, you agree that your contributions will be
licensed under the PostgreSQL License.
