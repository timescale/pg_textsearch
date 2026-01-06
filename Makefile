EXTENSION = pg_textsearch
DATA = sql/pg_textsearch--0.3.0-dev.sql \
       sql/pg_textsearch--0.0.1--0.0.2.sql \
       sql/pg_textsearch--0.0.2--0.0.3.sql \
       sql/pg_textsearch--0.0.3--0.0.4.sql \
       sql/pg_textsearch--0.0.4--0.0.5.sql \
       sql/pg_textsearch--0.0.5--0.1.0.sql \
       sql/pg_textsearch--0.1.0--0.2.0.sql \
       sql/pg_textsearch--0.2.0--0.3.0-dev.sql

# Source files organized by directory
OBJS = \
	src/mod.o \
	src/source.o \
	src/am/handler.o \
	src/am/build.o \
	src/am/scan.o \
	src/am/vacuum.o \
	src/memtable/memtable.o \
	src/memtable/posting.o \
	src/memtable/stringtable.o \
	src/memtable/scan.o \
	src/memtable/source.o \
	src/segment/segment.o \
	src/segment/dictionary.o \
	src/segment/scan.o \
	src/segment/merge.o \
	src/segment/docmap.o \
	src/segment/source.o \
	src/types/vector.o \
	src/types/query.o \
	src/types/score.o \
	src/state/state.o \
	src/state/registry.o \
	src/state/metapage.o \
	src/state/limit.o \
	src/planner/hooks.o \
	src/planner/cost.o \
	src/debug/dump.o

# Shared library target
MODULE_big = pg_textsearch

# Include directories, debug flags, and warning flags for unused code
PG_CPPFLAGS = -I$(srcdir)/src -g -O2 -Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter -Wunused-but-set-variable

# Uncomment the following line to enable debug index dumps
# PG_CPPFLAGS += -DDEBUG_DUMP_INDEX

# Test configuration
REGRESS = aerodocs basic deletion vacuum dropped empty implicit index inheritance limits lock manyterms memory merge mixed partitioned queries schema scoring1 scoring2 scoring3 scoring4 scoring5 scoring6 segment strings unsupported updates vector unlogged_index
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# SQL regression tests
test:
	@echo "Running SQL regression tests..."
	@$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)

# Custom local test target with dedicated PostgreSQL instance
test-local: install
	@echo "Setting up temporary PostgreSQL instance for local testing..."
	@rm -rf tmp_check_shared
	@mkdir -p tmp_check_shared
	@initdb -D tmp_check_shared/data --auth-local=trust --auth-host=trust
	@echo "port = 55433" >> tmp_check_shared/data/postgresql.conf
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

# Shell script test targets (assume extension is already installed)
test-concurrency:
	@echo "Running concurrency tests..."
	@cd test/scripts && ./concurrency.sh

test-recovery:
	@echo "Running crash recovery tests..."
	@cd test/scripts && ./recovery.sh

test-segment:
	@echo "Running multi-backend segment tests..."
	@cd test/scripts && ./segment.sh

test-stress:
	@echo "Running stress tests..."
	@cd test/scripts && ./stress.sh

test-shell: test-concurrency test-recovery test-segment
	@echo "All shell-based tests completed"

test-all: test test-shell
	@echo "All tests (SQL regression + shell scripts) completed successfully"

# Override installcheck to run all tests (SQL regression + shell scripts)
installcheck:
	@$(MAKE) test
	@$(MAKE) test-shell

# Generate expected output files from current test results
expected:
	@echo "Generating expected output files from current results..."
	@for test in $(REGRESS); do \
		if [ -f test/results/$$test.out ]; then \
			cp test/results/$$test.out test/expected/$$test.out; \
			echo "  Updated test/expected/$$test.out"; \
		else \
			echo "  Warning: No results file for $$test"; \
		fi; \
	done
	@echo "Expected files updated. Review changes before committing."

# Code formatting targets
lint-format:
	@echo "Checking C code formatting with clang-format..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror --style=file; \
	else \
		echo "clang-format not found - install with: brew install clang-format (macOS) or apt install clang-format (Linux)"; \
		exit 1; \
	fi
	@echo "Code formatting check passed"

format:
	@echo "Formatting C code with clang-format..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find src/ -name "*.c" -o -name "*.h" | xargs clang-format -i --style=file; \
	else \
		echo "clang-format not found - install with: brew install clang-format (macOS) or apt install clang-format (Linux)"; \
		exit 1; \
	fi
	@echo "Code formatting completed"

format-diff:
	@echo "Showing formatting differences..."
	@if command -v clang-format >/dev/null 2>&1; then \
		for file in `find src/ -name "*.c" -o -name "*.h"`; do \
			echo "=== $$file ==="; \
			clang-format --style=file "$$file" | diff -u "$$file" - || true; \
		done; \
	else \
		echo "clang-format not found"; \
		exit 1; \
	fi

format-single:
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make format-single FILE=path/to/file.c"; \
		exit 1; \
	fi
	@echo "Formatting $(FILE)..."
	@clang-format -i --style=file $(FILE)
	@echo "$(FILE) formatted"

format-check: lint-format


# Help target
.PHONY: help
help:
	@echo "pg_textsearch Makefile"
	@echo ""
	@echo "Build targets:"
	@echo "  make              - Build the extension"
	@echo "  make install      - Build and install the extension"
	@echo "  make clean        - Clean build artifacts and test directories"
	@echo ""
	@echo "Testing targets:"
	@echo "  make test         - Run SQL regression tests only"
	@echo "  make installcheck - Run all tests (SQL + shell scripts)"
	@echo "  make test-local   - Run tests with dedicated PostgreSQL instance"
	@echo "  make test-all     - Run all tests (SQL regression + shell scripts)"
	@echo "  make test-shell   - Run shell-based tests (all shell scripts)"
	@echo "  make test-concurrency - Run concurrency tests"
	@echo "  make test-recovery    - Run crash recovery tests"
	@echo "  make test-segment     - Run multi-backend segment tests"
	@echo "  make test-stress      - Run long-running stress tests"
	@echo "  make expected     - Generate expected output files from test results"
	@echo ""
	@echo "Code formatting targets:"
	@echo "  make format       - Auto-format C code with clang-format"
	@echo "  make format-check - Check C code formatting (alias: lint-format)"
	@echo "  make format-diff  - Show formatting differences"
	@echo "  make format-single FILE=path/to/file.c - Format specific file"
	@echo ""
	@echo "Configuration:"
	@echo "  PG_CONFIG - Path to pg_config (default: pg_config)"
	@echo ""
	@echo "Examples:"
	@echo "  make && make install"
	@echo "  make test-all"
	@echo "  make format"

.PHONY: test clean-test-dirs installcheck test-concurrency test-recovery test-segment test-stress test-shell test-all expected lint-format format format-check format-diff format-single help
