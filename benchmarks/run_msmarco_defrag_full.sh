#!/bin/bash
#
# Full end-to-end MS-MARCO v2 defrag benchmark.
#
# Run this on a fresh VM. It handles everything:
#   0. Install dependencies (PostgreSQL 17, build tools)
#   1. Build and install pg_textsearch from source
#   2. Download MS-MARCO v2 dataset (~20 GB)
#   3. Load 138M passages, build fragmented index
#   4. Benchmark scattered vs contiguous query latency
#
# Usage:
#   git clone <repo> && cd pg_textsearch
#   git checkout defrag-contiguous-segment-writes
#   ./benchmarks/run_msmarco_defrag_full.sh
#
# Environment variables:
#   PGPORT      — PostgreSQL port (default: 5432)
#   PGDATABASE  — database name (default: postgres)
#   PGDATA      — data directory (auto-detected if not set)
#   BATCH_SIZE  — rows per incremental INSERT batch (default: 1000000)
#   SKIP_DEPS   — set to 1 to skip dependency installation
#   SKIP_BUILD  — set to 1 to skip building pg_textsearch

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

header() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
    echo ""
}

# ==============================================================
# Step 0: Install dependencies
# ==============================================================

header "Step 0: Install dependencies"

if [ "${SKIP_DEPS:-0}" = "1" ]; then
    log "SKIP_DEPS=1 — skipping."
else
    if [ "$(uname)" = "Darwin" ]; then
        log "macOS detected — using Homebrew"
        if ! command -v brew >/dev/null 2>&1; then
            log "ERROR: Homebrew not installed. Install from https://brew.sh"
            exit 1
        fi
        brew install postgresql@17 wget python3 2>/dev/null || true
        brew services start postgresql@17 2>/dev/null || true
        sleep 3

        PG_BIN="$(brew --prefix postgresql@17)/bin"
        export PATH="$PG_BIN:$PATH"

    elif [ -f /etc/debian_version ] || [ -f /etc/ubuntu-release ]; then
        log "Debian/Ubuntu detected — using apt"

        # Add official PostgreSQL apt repository
        if [ ! -f /etc/apt/sources.list.d/pgdg.list ]; then
            log "Adding PostgreSQL apt repository..."
            sudo apt-get install -y -qq curl ca-certificates gnupg
            curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
                | sudo gpg --dearmor -o /usr/share/keyrings/postgresql-archive-keyring.gpg
            echo "deb [signed-by=/usr/share/keyrings/postgresql-archive-keyring.gpg] \
https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" \
                | sudo tee /etc/apt/sources.list.d/pgdg.list >/dev/null
        fi

        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            postgresql-17 postgresql-server-dev-17 \
            build-essential wget python3 libreadline-dev \
            zlib1g-dev pkg-config

        sudo systemctl enable postgresql 2>/dev/null || true
        sudo systemctl start postgresql 2>/dev/null || true

    elif [ -f /etc/redhat-release ]; then
        log "RHEL/Fedora detected — using dnf"
        sudo dnf install -y \
            postgresql17-server postgresql17-devel \
            gcc make wget python3 readline-devel \
            zlib-devel 2>/dev/null

        sudo postgresql-17-setup initdb 2>/dev/null || true
        sudo systemctl enable postgresql-17 2>/dev/null || true
        sudo systemctl start postgresql-17 2>/dev/null || true
    else
        log "Unknown OS — skipping package install."
        log "Ensure PostgreSQL 17, dev headers, gcc, make,"
        log "wget, and python3 are available."
    fi
fi

# On Debian/Ubuntu the PG 17 binaries live in /usr/lib/postgresql/17/bin
# and are not on PATH by default.
if [ -d /usr/lib/postgresql/17/bin ]; then
    export PATH="/usr/lib/postgresql/17/bin:$PATH"
fi

# Verify pg_config is available
if ! command -v pg_config >/dev/null 2>&1; then
    log "ERROR: pg_config not found. Add PostgreSQL bin to PATH."
    log "  e.g.: export PATH=/usr/lib/postgresql/17/bin:\$PATH"
    exit 1
fi

PG_VERSION=$(pg_config --version)
log "pg_config: $(which pg_config)"
log "PG version: $PG_VERSION"

# Set up PG env defaults.
# On Debian/Ubuntu the cluster runs as user "postgres" with peer auth
# over the local Unix socket, so we default PGUSER=postgres and use
# sudo -u postgres for admin commands when the current user is not
# postgres.
export PGPORT=${PGPORT:-5432}
export PGDATABASE=${PGDATABASE:-postgres}

if [ "$(uname)" = "Darwin" ]; then
    export PGHOST=${PGHOST:-/tmp}
    export PGUSER=${PGUSER:-$(whoami)}
    PSQL="psql"
else
    export PGHOST=${PGHOST:-/var/run/postgresql}
    export PGUSER=${PGUSER:-postgres}
    if [ "$(whoami)" = "postgres" ]; then
        PSQL="psql"
    else
        PSQL="sudo -u postgres psql"
    fi
fi

# Auto-detect PGDATA if not set
if [ -z "$PGDATA" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        PGDATA="$(brew --prefix)/var/postgresql@17"
    else
        PGDATA=$($PSQL -t -A -c "SHOW data_directory;" 2>/dev/null \
            || echo "/var/lib/postgresql/17/main")
    fi
    export PGDATA
fi
log "PGDATA: $PGDATA"

# Wait for PG to be ready
log "Waiting for PostgreSQL..."
for i in $(seq 1 30); do
    if $PSQL -c "SELECT 1" >/dev/null 2>&1; then break; fi
    sleep 1
done

if ! $PSQL -c "SELECT 1" >/dev/null 2>&1; then
    log "ERROR: Cannot connect to PostgreSQL."
    log "  PGHOST=$PGHOST PGPORT=$PGPORT PGUSER=$PGUSER"
    exit 1
fi
log "PostgreSQL is ready."

# ==============================================================
# Step 1: Build and install pg_textsearch
# ==============================================================

header "Step 1: Build and install pg_textsearch"

if [ "${SKIP_BUILD:-0}" = "1" ]; then
    log "SKIP_BUILD=1 — skipping."
else
    cd "$REPO_DIR"
    log "Building pg_textsearch..."
    make clean 2>/dev/null || true
    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" 2>&1
    if [ "$(uname)" = "Darwin" ]; then
        make install 2>&1
    else
        sudo make install 2>&1
    fi
    log "Build complete."
fi

# Add to shared_preload_libraries if needed
HAS_SPL=$($PSQL -t -A -c \
    "SHOW shared_preload_libraries;" 2>/dev/null || echo "")
if echo "$HAS_SPL" | grep -q pg_textsearch; then
    log "pg_textsearch already in shared_preload_libraries."
else
    log "Adding pg_textsearch to shared_preload_libraries..."

    # Use ALTER SYSTEM — this writes to postgresql.auto.conf which
    # is always read regardless of include_dir / conf.d setups.
    CUR_SPL=$($PSQL -t -A -c "SHOW shared_preload_libraries;" 2>&1)
    if [ -n "$CUR_SPL" ] && [ "$CUR_SPL" != "" ]; then
        NEW_SPL="${CUR_SPL},pg_textsearch"
    else
        NEW_SPL="pg_textsearch"
    fi
    $PSQL -c "ALTER SYSTEM SET shared_preload_libraries = '$NEW_SPL';" \
        2>&1
    log "ALTER SYSTEM done. New value: $NEW_SPL"

    log "Restarting PostgreSQL to load extension..."
    if [ "$(uname)" = "Darwin" ]; then
        brew services restart postgresql@17 2>/dev/null \
            || pg_ctl restart -D "$PGDATA" 2>/dev/null
    else
        sudo systemctl restart postgresql 2>&1 \
            || sudo pg_ctlcluster 17 main restart 2>&1 \
            || sudo -u postgres pg_ctl restart -D "$PGDATA" -m fast \
                2>&1
    fi
    sleep 3
    until $PSQL -c "SELECT 1" >/dev/null 2>&1; do sleep 1; done

    # Verify it took effect
    LOADED=$($PSQL -t -A -c "SHOW shared_preload_libraries;" 2>&1)
    log "shared_preload_libraries = $LOADED"
    if ! echo "$LOADED" | grep -q pg_textsearch; then
        log "ERROR: pg_textsearch still not loaded after restart."
        if [ "$(uname)" != "Darwin" ]; then
            sudo tail -20 /var/log/postgresql/postgresql-17-main.log \
                2>/dev/null || true
        fi
        exit 1
    fi
    log "PostgreSQL restarted with pg_textsearch loaded."
fi

log "Creating extension..."
if ! $PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch;" 2>&1; then
    log "ERROR: CREATE EXTENSION failed."
    log "Check that shared_preload_libraries includes pg_textsearch:"
    $PSQL -t -A -c "SHOW shared_preload_libraries;" 2>&1 || true
    log "Check PG log for details:"
    if [ "$(uname)" != "Darwin" ]; then
        sudo tail -20 /var/log/postgresql/postgresql-17-main.log \
            2>/dev/null || true
    fi
    exit 1
fi
log "Extension loaded."

# On Linux with peer auth, create a superuser role matching the
# current OS user so the benchmark script can connect without sudo.
if [ "$(uname)" != "Darwin" ] && [ "$(whoami)" != "postgres" ]; then
    CURRENT_USER="$(whoami)"
    log "Creating DB superuser role '$CURRENT_USER'..."
    $PSQL -c \
        "DO \$\$ BEGIN
           IF NOT EXISTS (SELECT FROM pg_roles
                          WHERE rolname = '$CURRENT_USER') THEN
             CREATE ROLE $CURRENT_USER LOGIN SUPERUSER;
           END IF;
         END \$\$;" 2>&1 || true
    log "Ensured DB role '$CURRENT_USER' exists."
fi

# ==============================================================
# Step 2: Run the benchmark
# ==============================================================

header "Step 2: Run benchmark"

# Reset PGUSER to the actual OS user — the full script used
# "postgres" with sudo for admin commands, but the benchmark
# script connects directly via peer auth.
export PGUSER="$(whoami)"

cd "$REPO_DIR"
exec bash "$SCRIPT_DIR/run_msmarco_defrag_bench.sh" \
    "${BATCH_SIZE:-1000000}"
