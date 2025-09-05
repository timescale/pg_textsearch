EXTENSION = tapir
DATA = sql/tapir--0.0.sql

# Source files
OBJS = \
	src/mod.o \
	src/memtable.o \
	src/posting.o \
	src/index.o \
	src/vector.o \
	src/stringtable.o

# Shared library target
MODULE_big = tapir

# Include directories and debug flags
PG_CPPFLAGS = -I$(srcdir)/src -g -O0

# Test configuration
REGRESS = basic index vector queries aerodocs mixed strings memory_limits limits
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Custom local test target with dedicated PostgreSQL instance
test-local: install
	@echo "Setting up temporary PostgreSQL instance for local testing..."
	@rm -rf tmp_check_shared
	@mkdir -p tmp_check_shared
	@initdb -D tmp_check_shared/data --auth-local=trust --auth-host=trust
	@echo "port = 55433" >> tmp_check_shared/data/postgresql.conf
	@echo "shared_preload_libraries = 'tapir'" >> tmp_check_shared/data/postgresql.conf
	@echo "log_statement = 'all'" >> tmp_check_shared/data/postgresql.conf
	@echo "shared_buffers = 256MB" >> tmp_check_shared/data/postgresql.conf
	@echo "max_connections = 20" >> tmp_check_shared/data/postgresql.conf
	@pg_ctl start -D tmp_check_shared/data -l tmp_check_shared/data/logfile -w
	@createdb -p 55433 contrib_regression
	@$(pg_regress_installcheck) --use-existing --port=55433 --inputdir=test --outputdir=test $(REGRESS) --dbname=contrib_regression
	@pg_ctl stop -D tmp_check_shared/data -l tmp_check_shared/data/logfile
	@rm -rf tmp_check_shared

# Clean test directories
clean: clean-test-dirs

clean-test-dirs:
	@rm -rf tmp_check_shared

# Shell script test targets
test-concurrency: install
	@echo "Running concurrency tests..."
	@cd test/scripts && ./concurrency.sh

test-recovery: install
	@echo "Running crash recovery tests..."
	@cd test/scripts && ./recovery.sh

test-memory-limits: install
	@echo "Running memory limits stress tests..."
	@cd test/scripts && ./memory_limits.sh

test-shell: test-concurrency test-recovery test-memory-limits
	@echo "All shell-based tests completed"

test-all: test test-shell
	@echo "All tests (SQL regression + shell scripts) completed successfully"

# Override installcheck to use our custom test setup
installcheck: test

# Linting and code quality targets
lint: lint-format lint-whitespace lint-spell

lint-format:
	@echo "Checking C code formatting with clang-format..."
	@if command -v clang-format-14 >/dev/null 2>&1; then \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format-14 --dry-run --Werror; \
	elif command -v clang-format >/dev/null 2>&1; then \
		echo "Warning: Using clang-format instead of clang-format-14"; \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror; \
	else \
		echo "clang-format not found - install with: brew install clang-format (macOS) or apt install clang-format-14 (Linux)"; \
		exit 1; \
	fi
	@echo "Code formatting check passed"

lint-whitespace:
	@echo "Checking for trailing whitespace..."
	@if find src/ -name "*.c" -o -name "*.h" | xargs grep -l "[ \t]$$" 2>/dev/null; then \
		echo "Error: Trailing whitespace found in source files"; \
		exit 1; \
	fi
	@echo "No trailing whitespace found"

lint-spell:
	@echo "Running spell check..."
	@if command -v codespell >/dev/null 2>&1; then \
		codespell --skip="*.out,*.diff" --ignore-words-list="nd,te" src/ README.md CLAUDE.md || true; \
	else \
		echo "codespell not found - install with: pip install codespell"; \
		echo "Skipping spell check"; \
	fi

format:
	@echo "Formatting C code with clang-format..."
	@if command -v clang-format-14 >/dev/null 2>&1; then \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format-14 -i; \
	elif command -v clang-format >/dev/null 2>&1; then \
		echo "Warning: Using clang-format instead of clang-format-14"; \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format -i; \
	else \
		echo "clang-format not found - install with: brew install clang-format (macOS) or apt install clang-format-14 (Linux)"; \
		exit 1; \
	fi
	@echo "Code formatting completed"

format-check: lint-format

.PHONY: test clean-test-dirs installcheck test-concurrency test-recovery test-shell test-all lint lint-format lint-whitespace lint-spell format format-check