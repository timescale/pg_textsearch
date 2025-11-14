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
TEMPLATE_DIR = test/sql
SQL_DIR = test/sql

# Find all template files
TEMPLATES = $(wildcard $(TEMPLATE_DIR)/*.sql.template)
# Generate corresponding SQL file names
GENERATED_SQL = $(TEMPLATES:.sql.template=.sql)

# Rule to convert .sql.template to .sql
%.sql: %.sql.template
	@echo "Processing template $< -> $@"
	@cd $(VALIDATION_DIR) && $(PYTHON) process_templates.py ../../$< ../../$@

# Process all templates
process-templates: $(GENERATED_SQL)
	@echo "All templates processed successfully"

# Generate validation SQL from templates
validate-templates: install process-templates
	@echo "Generating validation SQL from templates..."
	@for template in $(TEMPLATES); do \
		sql=$${template%.template}; \
		echo "Validating $$sql..."; \
		psql -f $$sql 2>&1 | grep -E "(PASS|FAIL): Results"; \
	done

# Install Python dependencies for validation
install-validation-deps:
	@if [ -f $(VALIDATION_DIR)/requirements.txt ]; then \
		$(PYTHON) -m pip install -r $(VALIDATION_DIR)/requirements.txt; \
	else \
		echo "Installing Python dependencies..."; \
		$(PYTHON) -m pip install psycopg2-binary rank-bm25; \
	fi

# Clean generated SQL files
clean-templates:
	@echo "Cleaning generated SQL files..."
	@rm -f $(GENERATED_SQL)

# Test the validation system with a sample template
test-validation: install install-validation-deps
	@echo "Testing BM25 validation system..."
	@cd $(VALIDATION_DIR) && $(PYTHON) test_template.py
	@echo "Validation test completed."

.PHONY: test clean-test-dirs installcheck test-concurrency test-recovery test-shell test-all lint-format format format-check format-diff format-single process-templates validate-templates install-validation-deps test-validation clean-templates
