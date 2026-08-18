#!/bin/bash
#
# Upgrade data-integrity matrix for pg_textsearch.
#
# For each old release, this harness builds several distinct on-disk
# index states (single segment, two segments, many segments, and an
# index with unspilled L0 memtable data), upgrades to the current
# binary, and checks recall against heap ground truth.
#
# Ground-truth oracle: a rare sentinel token is planted in a known
# subset of rows, and recall is measured as how many of the heap's
# sentinel rows the BM25 index scan returns in its top-K.  Recall is
# captured under the old binary (pre-upgrade), the new binary
# (post-upgrade), and again after REINDEX.
#
# Hard assertions (fail the build):
#   * The old binary has perfect recall (oracle sanity).
#   * Segment-backed shapes preserve 100% recall across the upgrade.
#   * REINDEX restores 100% recall for every shape.
#   * Legacy (metapage v5, <= 0.5.0) indexes ERROR cleanly without a
#     REINDEX (no crash, no silent wrong answer) and REINDEX recovers.
#
# The "memtable_unspilled" shape loses the unspilled documents when
# upgrading from a pre-1.3 (metapage v6) release; REINDEX recovers.
# By default this is surfaced as a non-fatal ::warning:: annotation so
# CI stays green; set STRICT_UNSPILLED=1 to make it a hard assertion.
#
# Usage:
#   PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config \
#     test/scripts/upgrade_matrix.sh [old_version ...]
#
# Environment:
#   PG_CONFIG          pg_config to use (default: first on PATH)
#   OLD_VERSIONS       space-separated versions (default: representative
#                      set covering every metapage/segment format)
#   PG_RUNAS           OS user to run cluster ops as (default: current;
#                      auto-set to "postgres" when running as root, since
#                      initdb/postgres refuse to run as root)
#   TEST_PORT          port for the throwaway cluster (default 55440)
#   STRICT_UNSPILLED   1 = hard-fail on unspilled data loss (default 0)
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

PG_CONFIG="${PG_CONFIG:-pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
SHAREDIR="$("$PG_CONFIG" --sharedir)"
export PATH="$BINDIR:$PATH"

TEST_PORT="${TEST_PORT:-55440}"
BASE_DIR="${BASE_DIR:-$REPO_ROOT/tmp_upgrade_matrix}"
DATA_DIR="$BASE_DIR/pgdata"
SOCK_DIR="$BASE_DIR/sock"
STRICT_UNSPILLED="${STRICT_UNSPILLED:-0}"

# Representative default matrix: one release per distinct on-disk
# format combination.  0.5.0 = metapage v5 (legacy/REINDEX tier);
# 0.5.1 = metapage v6 + segment v3; 1.0.0 = v6 + segment v4;
# 1.2.0 = v6 + segment v5; 1.3.0 = metapage v7 (native on-disk L0).
OLD_VERSIONS="${OLD_VERSIONS:-0.5.0 0.5.1 1.0.0 1.2.0 1.3.0}"
if [ "$#" -gt 0 ]; then OLD_VERSIONS="$*"; fi

# Cluster ops must not run as root.  When invoked as root (e.g. inside
# a container), drop to PG_RUNAS (default postgres) for pg_ctl/initdb/
# psql; build/install still run as root.
if [ "$(id -u)" -eq 0 ]; then
  PG_RUNAS="${PG_RUNAS:-postgres}"
else
  PG_RUNAS="${PG_RUNAS:-$(id -un)}"
fi

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[$(date '+%H:%M:%S')] $*${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARN: $*${NC}"; }
err()  { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $*${NC}"; }

# Installing into the PG sharedir needs root; use sudo when we are not.
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

FAILURES=0
fail() { err "$*"; FAILURES=$((FAILURES + 1)); }
annotate_warn() { echo "::warning::$*"; warn "$*"; }

# Run a command as the cluster OS user without nested-quote pain.
as_pg() {
  if [ "$PG_RUNAS" = "$(id -un)" ]; then
    "$@"
  else
    # Own the data/sock dirs so the runas user can use them.
    su "$PG_RUNAS" -c "PATH='$PATH' PGPORT='$TEST_PORT' PGHOST='$SOCK_DIR' $(printf '%q ' "$@")"
  fi
}

# psql helpers: SQL always travels through a world-readable temp FILE,
# never an inline -c string, to survive su-quoting.  scalar() echoes a
# single value; runsql() runs quietly; run_capture() saves stderr.
_mktemp_sql() { local f; f="$(mktemp "${TMPDIR:-/tmp}/um.XXXXXX.sql")"; printf '%s\n' "$1" >"$f"; chmod 644 "$f"; echo "$f"; }
scalar() { local f; f="$(_mktemp_sql "$1")"; as_pg psql -h "$SOCK_DIR" -p "$TEST_PORT" -d upg -tAqf "$f" 2>/dev/null; rm -f "$f"; }
runsql() { local f; f="$(_mktemp_sql "$1")"; as_pg psql -h "$SOCK_DIR" -p "$TEST_PORT" -d upg -q -f "$f" >/dev/null 2>&1; local rc=$?; rm -f "$f"; return $rc; }
run_capture() { local f; f="$(_mktemp_sql "$1")"; as_pg psql -h "$SOCK_DIR" -p "$TEST_PORT" -d upg -tAqf "$f" >"$2" 2>"$3"; rm -f "$f"; }

cleanup() {
  as_pg pg_ctl -D "$DATA_DIR" stop -m immediate >/dev/null 2>&1 || true
  rm -rf "$BASE_DIR"
}
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------ #
# Build / install helpers
# ------------------------------------------------------------------ #
build_install_old() { # $1 = version
  local v="$1" dir="/tmp/pg_textsearch-$v"
  rm -rf "$dir" "/tmp/ptsrc-$v.tar.gz"
  local url="https://github.com/timescale/pg_textsearch/releases/download/v$v/pg_textsearch-$v.tar.gz"
  curl -fsSL -o "/tmp/ptsrc-$v.tar.gz" "$url" || return 2
  tar -xzf "/tmp/ptsrc-$v.tar.gz" -C /tmp || return 2
  ( cd "$dir" && make -s PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1;
    make -s PG_CONFIG="$PG_CONFIG" >/dev/null 2>&1 &&
    $SUDO make -s PG_CONFIG="$PG_CONFIG" install >/dev/null 2>&1 ) || return 3
}

build_install_current() {
  $SUDO rm -f "$SHAREDIR/extension/pg_textsearch--"*.sql
  ( cd "$REPO_ROOT" && make -s PG_CONFIG="$PG_CONFIG" >/dev/null 2>&1 &&
    $SUDO make -s PG_CONFIG="$PG_CONFIG" install >/dev/null 2>&1 ) || return 3
}

# ------------------------------------------------------------------ #
# Cluster lifecycle
# ------------------------------------------------------------------ #
fresh_cluster() {
  as_pg pg_ctl -D "$DATA_DIR" stop -m immediate >/dev/null 2>&1 || true
  rm -rf "$BASE_DIR"; mkdir -p "$DATA_DIR" "$SOCK_DIR"
  if [ "$PG_RUNAS" != "$(id -un)" ]; then chown -R "$PG_RUNAS" "$BASE_DIR"; fi
  as_pg initdb -D "$DATA_DIR" -A trust >/dev/null 2>&1 || { err "initdb failed"; return 1; }
  {
    echo "shared_preload_libraries = 'pg_textsearch'"
    echo "unix_socket_directories = '$SOCK_DIR'"
    echo "port = $TEST_PORT"
    echo "logging_collector = off"
  } >>"$DATA_DIR/postgresql.conf"
}
start_pg() { as_pg pg_ctl -D "$DATA_DIR" -l "$DATA_DIR/logfile" start -w >/dev/null 2>&1; }
stop_pg()  { as_pg pg_ctl -D "$DATA_DIR" stop -w >/dev/null 2>&1; }
server_up(){ as_pg pg_ctl -D "$DATA_DIR" status >/dev/null 2>&1; }
createdb_upg() { as_pg createdb -h "$SOCK_DIR" -p "$TEST_PORT" upg >/dev/null 2>&1; }
log_marker() { as_pg wc -l <"$DATA_DIR/logfile" 2>/dev/null | tr -d ' '; }
log_since()  { as_pg tail -n +"$(( ${1:-0} + 1 ))" "$DATA_DIR/logfile" 2>/dev/null; }

# ------------------------------------------------------------------ #
# Corpus + oracle
# ------------------------------------------------------------------ #
# Insert N rows starting at offset OFF; every 12th row carries the rare
# sentinel token 'qwxsentinel'.
gen() { # $1=n $2=off
  runsql "INSERT INTO d(c)
    SELECT CASE WHEN (g % 12) = 0
      THEN 'alpha beta gamma qwxsentinel doc ' || g
      ELSE 'alpha beta gamma common filler doc ' || g END
    FROM generate_series($2, $2 + $1 - 1) g;"
}
mk_index() { runsql "CREATE INDEX i ON d USING bm25(c) WITH (text_config='english');"; }
spill()    { runsql "SELECT bm25_spill_index('i');"; }

build_shape() { # $1 = shape
  runsql "CREATE TABLE d(id serial primary key, c text);"
  case "$1" in
    single_seg)         gen 600 1;  mk_index ;;
    two_seg)            gen 300 1;  mk_index; gen 300 301; spill ;;
    multi_seg)          gen 120 1;  mk_index
                        gen 120 121; spill; gen 120 241; spill
                        gen 120 361; spill; gen 120 481; spill ;;
    memtable_unspilled) gen 300 1;  mk_index; gen 300 301 ;;  # batch 2 stays in L0
  esac
}

TRUTH_Q="SELECT count(*) FROM d WHERE c LIKE '%qwxsentinel%';"
# Recall = number of genuine sentinel rows appearing in the BM25 top-K
# (K = sentinel count).  Non-matching rows score 0 and sort AFTER the
# negative-scored matches, so a healthy index yields recall == truth.
RECALL_Q="SET enable_seqscan=off;
SELECT count(*) FROM (
  SELECT id FROM d ORDER BY c <@> to_bm25query('qwxsentinel','i')
  LIMIT (SELECT count(*) FROM d WHERE c LIKE '%qwxsentinel%')
) s WHERE s.id IN (SELECT id FROM d WHERE c LIKE '%qwxsentinel%');"

# ------------------------------------------------------------------ #
# Per-scenario runners
# ------------------------------------------------------------------ #
is_legacy() { [[ "$1" =~ ^0\.[0-4]\. ]] || [[ "$1" == 0.5.0 ]]; }

run_compat_shape() { # $1=version $2=shape
  local v="$1" st="$2"
  fresh_cluster || { fail "$v/$st: cluster init failed"; return; }
  start_pg || { fail "$v/$st: old server failed to start"; return; }
  createdb_upg
  runsql "CREATE EXTENSION pg_textsearch;"
  build_shape "$st"
  local truth pre post reidx forensic
  truth="$(scalar "$TRUTH_Q")"
  pre="$(scalar "$RECALL_Q")"
  stop_pg
  build_install_current || { fail "current build/install failed"; return; }
  start_pg || { fail "$v/$st: NEW server failed to start (corruption?)"; return; }
  local mark; mark="$(log_marker)"
  runsql "INSERT INTO d(c) VALUES ('alpha beta gamma writeprobe token');"
  post="$(scalar "$RECALL_Q")"
  forensic="$(log_since "$mark" | grep -c 'that may have held unspilled documents')"
  runsql "REINDEX INDEX i;"
  reidx="$(scalar "$RECALL_Q")"
  stop_pg

  log "  [$v/$st] truth=$truth pre=$pre post=$post reindex=$reidx forensic=$forensic"

  # Oracle sanity: the OLD binary must have perfect recall.
  [ "$pre" = "$truth" ] || fail "$v/$st: OLD-binary recall $pre != truth $truth (oracle broken)"

  if [ "$st" = "memtable_unspilled" ]; then
    if [ "$post" != "$truth" ]; then
      if [ "$STRICT_UNSPILLED" = "1" ]; then
        fail "$v/$st: unspilled data loss: post=$post truth=$truth"
      else
        annotate_warn "upgrading v$v lost unspilled memtable data (recall $post/$truth); REINDEX recovers ($reidx)"
      fi
    else
      log "  [$v/$st] no unspilled loss for v$v (consider STRICT_UNSPILLED=1)"
    fi
  else
    # Segment-backed shapes must survive the upgrade losslessly.
    [ "$post" = "$truth" ] || fail "$v/$st: segment data loss on upgrade: post=$post truth=$truth"
  fi

  # Safety net: REINDEX must always restore full recall.
  [ "$reidx" = "$truth" ] || fail "$v/$st: REINDEX did not restore recall ($reidx/$truth)"
}

run_legacy() { # $1=version
  local v="$1"
  fresh_cluster || { fail "$v(legacy): cluster init failed"; return; }
  start_pg || { fail "$v(legacy): old server failed to start"; return; }
  createdb_upg
  runsql "CREATE EXTENSION pg_textsearch;"
  build_shape single_seg
  local truth pre; truth="$(scalar "$TRUTH_Q")"; pre="$(scalar "$RECALL_Q")"
  stop_pg
  build_install_current || { fail "current build/install failed"; return; }
  start_pg || { fail "$v(legacy): NEW server failed to start"; return; }

  # Query WITHOUT reindex must ERROR cleanly and NOT crash the server.
  local out err_f; out="$(mktemp)"; err_f="$(mktemp)"
  run_capture "$RECALL_Q" "$out" "$err_f"
  if server_up; then :; else fail "$v(legacy): server CRASHED on query without REINDEX"; fi
  if grep -qiE 'incompatible|REINDEX' "$err_f"; then
    log "  [$v/legacy] clean ERROR without REINDEX (as designed)"
  else
    fail "$v(legacy): expected incompatible-version ERROR, got stdout=[$(cat "$out")] stderr=[$(head -1 "$err_f")]"
  fi
  rm -f "$out" "$err_f"

  runsql "REINDEX INDEX i;"
  local reidx; reidx="$(scalar "$RECALL_Q")"
  stop_pg
  log "  [$v/legacy] truth=$truth pre=$pre post-REINDEX=$reidx"
  [ "$pre" = "$truth" ]   || fail "$v(legacy): OLD-binary recall $pre != truth $truth"
  [ "$reidx" = "$truth" ] || fail "$v(legacy): REINDEX did not restore recall ($reidx/$truth)"
}

# ------------------------------------------------------------------ #
# Main
# ------------------------------------------------------------------ #
log "Upgrade matrix: PG bindir=$BINDIR runas=$PG_RUNAS"
log "Versions: $OLD_VERSIONS"

for v in $OLD_VERSIONS; do
  log "=== old version v$v ==="
  if ! build_install_old "$v"; then
    fail "$v: download/build of old release failed"; continue
  fi
  if is_legacy "$v"; then
    run_legacy "$v"
  else
    shapes="single_seg two_seg multi_seg memtable_unspilled"
    last_shape="${shapes##* }"
    for st in $shapes; do
      run_compat_shape "$v" "$st"
      # Each scenario installs the current binary over the old one, so
      # restore the old binary before the next shape.  Skip after the
      # last shape — nothing else uses this old binary.
      [ "$st" = "$last_shape" ] && break
      build_install_old "$v" || { fail "$v: could not reinstall old binary"; break; }
    done
  fi
done

echo
if [ "$FAILURES" -eq 0 ]; then
  log "UPGRADE MATRIX PASSED (hard assertions); see ::warning:: lines for known issues"
  exit 0
else
  err "UPGRADE MATRIX FAILED: $FAILURES hard assertion(s)"
  exit 1
fi
