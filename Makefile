EXTENSION = pg_textsearch
DATA = sql/pg_textsearch--0.0.4-dev.sql \
       sql/pg_textsearch--0.0.1--0.0.3.sql \
       sql/pg_textsearch--0.0.2--0.0.3.sql \
       sql/pg_textsearch--0.0.3--0.0.4-dev.sql

# Source files
# Full build - debugging initialization crash
OBJS = \
	src/mod.o \
	src/memtable.o \
	src/memory.o \
	src/metapage.o \
	src/posting.o \
	src/index.o \
	src/vector.o \
	src/query.o \
	src/stringtable.o \
	src/operator.o \
	src/limit.o \
	src/registry.o \
	src/state.o

# Shared library target
MODULE_big = pg_textsearch

# Include directories, debug flags, and warning flags for unused code
PG_CPPFLAGS = -I$(srcdir)/src -g -O0 -Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter -Wunused-but-set-variable

# Uncomment the following line to enable debug index dumps
# PG_CPPFLAGS += -DDEBUG_DUMP_INDEX

# Test configuration
REGRESS = aerodocs basic deletion vacuum dropped empty index inheritance limits manyterms memory mixed queries schema scoring1 scoring2 scoring3 scoring4 scoring5 scoring6 strings vector
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
	@echo "log_statement = 'all'" >> tmp_check_shared/data/postgresql.conf
	@echo "shared_buffers = 256MB" >> tmp_check_shared/data/postgresql.conf
	@echo "max_connections = 20" >> tmp_check_shared/data/postgresql.conf
	@pg_ctl start -D tmp_check_shared/data -l tmp_check_shared/data/logfile -w
	@createdb -p 55433 contrib_regression
	@$(pg_regress_installcheck) --use-existing --port=55433 --inputdir=test --outputdir=test $(REGRESS) --dbname=contrib_regression
	@pg_ctl stop -D tmp_check_shared/data -l tmp_check_shared/data/logfile
	@rm -rf tmp_check_shared

# Clean test directories
clean: clean-test-dirs clean-validation

clean-test-dirs:
	@rm -rf tmp_check_shared

clean-validation:
	@# Clean generated validation SQL files
	@rm -f test/sql/*_validated.sql
	@# Clean expected output files
	@rm -f test/expected/*_validated.out
	@# Clean any Python cache
	@rm -rf test/python/__pycache__
	@rm -f test/python/*.pyc

# Shell script test targets
test-concurrency: install
	@echo "Running concurrency tests..."
	@cd test/scripts && ./concurrency.sh

test-recovery: install
	@echo "Running crash recovery tests..."
	@cd test/scripts && ./recovery.sh

test-shell: test-concurrency test-recovery
	@echo "All shell-based tests completed"

test-all: test test-shell
	@echo "All tests (SQL regression + shell scripts) completed successfully"

# Override installcheck to use our custom test setup
installcheck: test

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

# BM25 Validation targets
PYTHON = python3
VALIDATION_DIR = test/python

# Single validation target that does everything needed
.PHONY: validate
validate: install
	@echo "Running BM25 validation..."
	@# Check Python dependencies
	@if ! $(PYTHON) -c "import psycopg2" 2>/dev/null; then \
		echo "Installing psycopg2-binary..."; \
		$(PYTHON) -m pip install --user --break-system-packages psycopg2-binary || \
			(echo "Failed to install psycopg2. Please run: pip install --user --break-system-packages psycopg2-binary" && exit 1); \
	fi
	@if ! $(PYTHON) -c "import rank_bm25" 2>/dev/null; then \
		echo "Installing rank-bm25..."; \
		$(PYTHON) -m pip install --user --break-system-packages rank-bm25 || \
			(echo "Failed to install rank-bm25. Please run: pip install --user --break-system-packages rank-bm25" && exit 1); \
	fi
	@# Run validation using the dedicated validation makefile
	@$(MAKE) -f Makefile.validation validate-all

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
	@echo "  make test         - Run regression tests (alias for installcheck)"
	@echo "  make installcheck - Run regression tests"
	@echo "  make test-local   - Run tests with dedicated PostgreSQL instance"
	@echo "  make test-all     - Run all tests (SQL regression + shell scripts)"
	@echo "  make test-shell   - Run shell-based tests (concurrency + recovery)"
	@echo "  make test-concurrency - Run concurrency tests"
	@echo "  make test-recovery    - Run crash recovery tests"
	@echo ""
	@echo "Code formatting targets:"
	@echo "  make format       - Auto-format C code with clang-format"
	@echo "  make format-check - Check C code formatting (alias: lint-format)"
	@echo "  make format-diff  - Show formatting differences"
	@echo "  make format-single FILE=path/to/file.c - Format specific file"
	@echo ""
	@echo "BM25 validation target:"
	@echo "  make validate     - Run BM25 scoring validation tests"
	@echo "                      (auto-installs Python dependencies, generates expected outputs)"
	@echo ""
	@echo "Configuration:"
	@echo "  PG_CONFIG - Path to pg_config (default: pg_config)"
	@echo ""
	@echo "Examples:"
	@echo "  make && make install"
	@echo "  make test-all"
	@echo "  make format"

.PHONY: test clean-test-dirs installcheck test-concurrency test-recovery test-shell test-all lint-format format format-check format-diff format-single process-templates validate-templates install-validation-deps test-validation clean-templates
