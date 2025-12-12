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
