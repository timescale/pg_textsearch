EXTENSION = pg_textsearch
EXTVERSION = $(shell awk -F"'" '/default_version/ {print $$2}' pg_textsearch.control)
DATA = sql/pg_textsearch--1.2.0-dev.sql \
       sql/pg_textsearch--0.0.1--0.0.2.sql \
       sql/pg_textsearch--0.0.2--0.0.3.sql \
       sql/pg_textsearch--0.0.3--0.0.4.sql \
       sql/pg_textsearch--0.0.4--0.0.5.sql \
       sql/pg_textsearch--0.0.5--0.1.0.sql \
       sql/pg_textsearch--0.1.0--0.2.0.sql \
       sql/pg_textsearch--0.2.0--0.3.0.sql \
       sql/pg_textsearch--0.3.0--0.4.0.sql \
       sql/pg_textsearch--0.4.0--0.4.1.sql \
       sql/pg_textsearch--0.4.1--0.4.2.sql \
       sql/pg_textsearch--0.4.2--0.5.0.sql \
       sql/pg_textsearch--0.5.0--0.5.1.sql \
       sql/pg_textsearch--0.5.1--0.6.0.sql \
       sql/pg_textsearch--0.6.0--0.6.1.sql \
       sql/pg_textsearch--0.6.1--1.0.0.sql \
       sql/pg_textsearch--1.0.0--1.1.0.sql \
       sql/pg_textsearch--1.1.0--1.2.0-dev.sql

# Source files organized by directory
OBJS = \
	src/mod.o \
	src/access/handler.o \
	src/access/build.o \
	src/access/build_context.o \
	src/access/build_parallel.o \
	src/access/scan.o \
	src/access/vacuum.o \
	src/memtable/arena.o \
	src/memtable/expull.o \
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
	src/segment/alive_bitset.o \
	src/segment/compression.o \
	src/segment/fieldnorm.o \
	src/scoring/bmw.o \
	src/scoring/bm25.o \
	src/types/array.o \
	src/types/vector.o \
	src/types/query.o \
	src/index/state.o \
	src/index/registry.o \
	src/index/metapage.o \
	src/index/limit.o \
	src/index/memory.o \
	src/index/resolve.o \
	src/index/source.o \
	src/planner/hooks.o \
	src/planner/cost.o \
	src/debug/dump.o

# Shared library target
MODULE_big = pg_textsearch

# Include directories, debug flags, and warning flags for unused code
PG_CPPFLAGS = -I$(srcdir)/src -g -O2 -Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter -Wunused-but-set-variable -DPG_TEXTSEARCH_VERSION=\"$(EXTVERSION)\"

# Suppress GCC-only false positives (clang silently ignores unknown -Wno-*
# flags thanks to -Wno-unknown-warning-option):
#  -Wclobbered: PG_TRY uses setjmp/longjmp; GCC warns about locals across it
#  -Wpacked-not-aligned: intentionally packed on-disk structs
PG_CPPFLAGS += -Wno-unknown-warning-option -Wno-clobbered -Wno-packed-not-aligned

# Uncomment the following line to enable debug index dumps
# PG_CPPFLAGS += -DDEBUG_DUMP_INDEX

# Test configuration
REGRESS = abort aerodocs basic binary_io bmw bulk_load catalog_stats compression concurrent_build coverage deletion vacuum vacuum_bitmap vacuum_extended vacuum_rebuild dropped empty explicit_index expression_index force_merge implicit index inheritance limits lock manyterms max_shared_memory memory merge mixed parallel_build parallel_bmw partitioned partitioned_many partial_index pgstats queries quoted_identifiers rescan schema scoring1 scoring2 scoring3 scoring4 scoring5 scoring6 security segment segment_integrity strings temp_table text_array text_config unsupported updates vector vector_v1_rejected unlogged_index wand
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
	@echo "shared_preload_libraries = '$(MODULE_big)'" >> tmp_check_shared/data/postgresql.conf
	@pg_ctl start -D tmp_check_shared/data -l tmp_check_shared/data/logfile -w
	@createdb -p 55433 contrib_regression
	@$(pg_regress_installcheck) --use-existing --port=55433 --inputdir=test --outputdir=test $(REGRESS) --dbname=contrib_regression
	@pg_ctl stop -D tmp_check_shared/data -l tmp_check_shared/data/logfile
	@rm -rf tmp_check_shared

# Clean test directories
clean: clean-test-dirs

clean-test-dirs:
	@rm -rf tmp_check_shared coverage-html coverage.info
	@find . -name "*.gcda" -delete 2>/dev/null || true
	@find . -name "*.gcno" -delete 2>/dev/null || true

# Shell script test targets (assume extension is already installed)
test-concurrency:
	@echo "Running concurrency tests..."
	@cd test/scripts && ./concurrency.sh

test-recovery:
	@echo "Running crash recovery tests..."
	@cd test/scripts && ./recovery.sh
	@cd test/scripts && ./docid_chain_recovery.sh
	@cd test/scripts && ./shutdown_spill.sh

test-segment:
	@echo "Running multi-backend segment tests..."
	@cd test/scripts && ./segment.sh

test-stress:
	@echo "Running stress tests..."
	@cd test/scripts && ./stress.sh

test-cic:
	@echo "Running CREATE INDEX CONCURRENTLY tests..."
	@cd test/scripts && ./cic.sh

# Replication tests (not in test-shell: each spawns two Postgres instances)
test-replication:
	@echo "Running physical replication tests..."
	@cd test/scripts && ./replication.sh

test-logical-replication:
	@echo "Running logical replication tests..."
	@cd test/scripts && ./logical_replication.sh

test-replication-extended:
	@echo "Running extended physical replication tests..."
	@scripts="\
	    replication.sh \
	    replication_issue_342.sh \
	    replication_parallel_build.sh \
	    replication_correctness.sh \
	    replication_concurrency.sh \
	    replication_failover.sh \
	    replication_compat.sh \
	    replication_cascading.sh \
	    replication_pitr.sh"; \
	failed=""; \
	for s in $$scripts; do \
	    echo ""; \
	    echo "==> $$s"; \
	    if ! (cd test/scripts && "./$$s"); then \
	        failed="$$failed $$s"; \
	    fi; \
	done; \
	if [ -n "$$failed" ]; then \
	    echo ""; \
	    echo "Failed scripts:$$failed"; \
	    exit 1; \
	fi

test-memory:
	@echo "Running memory accounting tests..."
	@cd test/scripts && ./memory_accounting.sh

test-multi-index:
	@echo "Running multi-index / multi-user / multi-schema tests..."
	@cd test/scripts && ./multi_index.sh

test-shell: test-concurrency test-recovery test-segment test-cic test-memory test-multi-index
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

# Code coverage targets (Linux only - requires lcov: apt install lcov)
# Note: macOS is not supported due to gcov runtime crashes with Postgres extensions
COVERAGE_DIR = coverage-html
COVERAGE_INFO = coverage.info

coverage-clean:
	@echo "Cleaning coverage data..."
	@rm -rf $(COVERAGE_DIR) $(COVERAGE_INFO)
	@find . -name "*.gcda" -delete
	@find . -name "*.gcno" -delete

coverage-build: coverage-clean
	@echo "Building with coverage instrumentation..."
	@if ! command -v lcov >/dev/null 2>&1; then \
		echo "lcov not found - install with: brew install lcov (macOS) or apt install lcov (Linux)"; \
		exit 1; \
	fi
	$(MAKE) clean
ifeq ($(shell uname),Darwin)
	$(MAKE) PG_CFLAGS="-fprofile-arcs -ftest-coverage -O0 -g"
else
	$(MAKE) PG_CFLAGS="--coverage -O0 -g" SHLIB_LINK="--coverage"
endif
	$(MAKE) install

coverage: coverage-build
	@echo "Running tests and collecting coverage..."
	@lcov --zerocounters --directory .
	-$(MAKE) test
	@echo "Capturing coverage data..."
	@lcov --capture \
		--directory . \
		--output-file $(COVERAGE_INFO) \
		--base-directory $(shell pwd) \
		--no-external \
		--rc branch_coverage=1 \
		--ignore-errors mismatch 2>/dev/null || \
		lcov --capture \
			--directory . \
			--output-file $(COVERAGE_INFO) \
			--base-directory $(shell pwd) \
			--no-external
	@lcov --remove $(COVERAGE_INFO) '*/test/*' \
		--output-file $(COVERAGE_INFO) \
		--rc branch_coverage=1 \
		--ignore-errors unused 2>/dev/null || \
		lcov --remove $(COVERAGE_INFO) '*/test/*' \
			--output-file $(COVERAGE_INFO)
	@echo "Generating HTML report..."
	@genhtml $(COVERAGE_INFO) \
		--output-directory $(COVERAGE_DIR) \
		--title "pg_textsearch Coverage" \
		--legend \
		--show-details \
		--rc branch_coverage=1
	@echo ""
	@echo "Coverage report generated in $(COVERAGE_DIR)/index.html"
	@lcov --summary $(COVERAGE_INFO) --rc branch_coverage=1

coverage-report:
	@if [ ! -f $(COVERAGE_INFO) ]; then \
		echo "No coverage data found. Run 'make coverage' first."; \
		exit 1; \
	fi
	@lcov --summary $(COVERAGE_INFO) --rc branch_coverage=1

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
	@echo "  make test-cic         - Run CREATE INDEX CONCURRENTLY tests"
	@echo "  make expected     - Generate expected output files from test results"
	@echo ""
	@echo "Code formatting targets:"
	@echo "  make format       - Auto-format C code with clang-format"
	@echo "  make format-check - Check C code formatting (alias: lint-format)"
	@echo "  make format-diff  - Show formatting differences"
	@echo "  make format-single FILE=path/to/file.c - Format specific file"
	@echo ""
	@echo "Code coverage targets (Linux only, requires lcov):"
	@echo "  make coverage       - Build with coverage, run tests, generate HTML report"
	@echo "  make coverage-build - Build with coverage instrumentation only"
	@echo "  make coverage-clean - Remove coverage data and reports"
	@echo "  make coverage-report - Show coverage summary (after running coverage)"
	@echo "  Note: macOS not supported; coverage runs in GitHub Actions CI"
	@echo ""
	@echo "Configuration:"
	@echo "  PG_CONFIG - Path to pg_config (default: pg_config)"
	@echo ""
	@echo "Examples:"
	@echo "  make && make install"
	@echo "  make test-all"
	@echo "  make format"

.PHONY: test clean-test-dirs installcheck test-concurrency test-recovery test-segment test-stress test-cic test-replication test-replication-extended test-logical-replication test-memory test-multi-index test-shell test-all expected lint-format format format-check format-diff format-single coverage coverage-build coverage-clean coverage-report help
