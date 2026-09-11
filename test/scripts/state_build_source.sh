#!/bin/bash
#
# Guard lifecycle invariants that require inspecting backend-local wrapper
# construction rather than exposing pointer identities through SQL.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_SOURCE="${REPO_ROOT}/src/access/build.c"
STATE_SOURCE="${REPO_ROOT}/src/index/state.c"
STATE_HEADER="${REPO_ROOT}/src/index/state.h"
MOD_SOURCE="${REPO_ROOT}/src/mod.c"
REGISTRY_SOURCE="${REPO_ROOT}/src/index/registry.c"

if ! grep -Fq "index_create_subid = index->rd_createSubid" \
    "${BUILD_SOURCE}"; then
    echo "build ownership is not derived from PostgreSQL relation context" >&2
    exit 1
fi

if ! grep -Fq "index_create_subid = index_rel->rd_createSubid" \
    "${STATE_SOURCE}" ||
    ! grep -Fq \
        "index_oid, heap_oid, index_create_subid" "${STATE_SOURCE}"; then
    echo "cold rebuild ownership is not restored from relation context" >&2
    exit 1
fi

get_body="$(
    sed -n '/^tp_get_local_index_state(/,/^}/p' "${STATE_SOURCE}"
)"

if grep -Fq \
    "local_state->created_in_subxact = InvalidSubTransactionId" \
    <<<"${get_body}"; then
    echo "cold rebuild discards restored initial-CREATE ownership" >&2
    exit 1
fi

finalize_body="$(
    sed -n '/^tp_finalize_build_mode(/,/^}/p' "${STATE_SOURCE}"
)"

for required in \
    "chain_page_count, 0" \
    "chain_page_count_needs_reseed, 1"; do
    if ! grep -Fq "${required}" <<<"${finalize_body}"; then
        echo "build finalization does not invalidate chain count: ${required}" \
            >&2
        exit 1
    fi
done

if ! grep -Fq "tp_reseed_chain_page_count_if_needed(index_state, index_rel)" \
    "${BUILD_SOURCE}" ||
    ! grep -Fq "reseed_chain_page_count_locked(local_state, index_rel)" \
    "${STATE_SOURCE}" ||
    ! grep -Fq "chain_page_count_needs_reseed, 0" "${STATE_SOURCE}"; then
    echo "auto-spill does not lazily reseed the current relfilenode count" >&2
    exit 1
fi

for required in \
    "chain_page_count_spc_oid" \
    "chain_page_count_db_oid" \
    "chain_page_count_rel_number"; do
    if ! grep -Fq "${required}" "${STATE_HEADER}"; then
        echo "chain count is not tagged with current relfilenode: ${required}" \
            >&2
        exit 1
    fi
done

for required in \
    "cursor_locator" \
    "cursor_locator_valid"; do
    if ! grep -Fq "${required}" "${STATE_HEADER}"; then
        echo "cache cursor is not tagged with its relation file: ${required}" \
            >&2
        exit 1
    fi
done

if ! grep -Fq "cache_locator_matches(memtable, rel)" \
    "${REPO_ROOT}/src/memtable/cache.c" ||
    ! grep -Fq "cache_locator_set(memtable, rel)" \
    "${REPO_ROOT}/src/memtable/cache.c"; then
    echo "cache apply/build does not validate its relation file" >&2
    exit 1
fi

cache_source="$(
    sed -n '/^tp_cache_apply_to_tail(/,/^tp_cache_cold_build(/p' \
        "${REPO_ROOT}/src/memtable/cache.c"
)"
locator_line=$(grep -n "cache_drop_locator_mismatch" <<<"${cache_source}" |
    head -1 | cut -d: -f1)
cap_line=$(grep -n "global_cap_check" <<<"${cache_source}" |
    head -1 | cut -d: -f1)
if [ -z "${locator_line}" ] || [ -z "${cap_line}" ] ||
    [ "${locator_line}" -ge "${cap_line}" ]; then
    echo "cache locator is not invalidated before hard-cap admission" >&2
    exit 1
fi

reseed_body="$(
    sed -n '/^tp_reseed_chain_page_count_if_needed(/,/^}/p' "${STATE_SOURCE}"
)"

if grep -Fq "Assert(!local_state->lock_held)" <<<"${reseed_body}"; then
    echo "chain-count reseed rejects callers already holding LW_EXCLUSIVE" >&2
    exit 1
fi

for required in \
    "bool acquired_lock = false" \
    "Assert(local_state->lock_mode == LW_EXCLUSIVE)" \
    "if (acquired_lock)"; do
    if ! grep -Fq "${required}" <<<"${reseed_body}"; then
        echo "chain-count reseed does not preserve caller lock ownership: \
${required}" >&2
        exit 1
    fi
done

if ! grep -Fq \
    "chain_page_count_matches_relation(local_state, index_rel)" \
    <<<"${reseed_body}" ||
    ! grep -Fq \
        "tp_set_chain_page_count_for_relation(index_state, index_rel, 0)" \
        "${BUILD_SOURCE}"; then
    echo "chain count can survive a relfilenode change without reseeding" >&2
    exit 1
fi

create_body="$(
    sed -n '/^create_or_attach_index_state(/,/^}/p' "${STATE_SOURCE}"
)"

if grep -Eq "^[[:space:]]*local_state = entry->local_state;" \
    <<<"${create_body}" ||
    grep -Fq "entry->local_state->" <<<"${create_body}" ||
    grep -Fq "old_local_state->shared->" <<<"${create_body}"; then
    echo "build-state creation reuses or dereferences a possibly stale wrapper" >&2
    exit 1
fi

for required in \
    "old_local_state = entry->local_state" \
    "old_local_state->shared_dp == shared_dp" \
    "old_local_state->terms_added_this_xact" \
    "old_local_state->docs_since_global_check" \
    "LWLockHeldByMe(&shared_state->lock)" \
    "entry->local_state = local_state" \
    "pfree(old_local_state)"; do
    if ! grep -Fq "${required}" <<<"${create_body}"; then
        echo "build-state creation does not replace wrappers safely: ${required}" >&2
        exit 1
    fi
done

dropdb_body="$(
    sed -n '/if (IsA(parsetree, DropdbStmt))/,/^[[:space:]]*return;/p' \
        "${MOD_SOURCE}"
)"

if grep -Fq "get_database_oid(stmt->dbname" <<<"${dropdb_body}"; then
    echo "DROP DATABASE cleanup resolves a racy name before core execution" >&2
    exit 1
fi

for required in \
    "TpDropDatabaseContext *drop_context" \
    "palloc(sizeof(*drop_context))" \
    "active_drop_database_context = drop_context" \
    "PG_CATCH()" \
    "active_drop_database_context = drop_context->previous" \
    "drop_context->database_oid"; do
    if ! grep -Fq "${required}" <<<"${dropdb_body}"; then
        echo "DROP DATABASE cleanup does not track core's dropped OID: \
${required}" >&2
        exit 1
    fi
done

if grep -Fq "TpDropDatabaseContext drop_context" <<<"${dropdb_body}"; then
    echo "DROP DATABASE keeps mutable hook context across longjmp on stack" >&2
    exit 1
fi

object_access_body="$(
    sed -n '/^tp_object_access(/,/^}/p' "${MOD_SOURCE}"
)"

for required in \
    "classId == DatabaseRelationId" \
    "active_drop_database_context->database_oid = objectId"; do
    if ! grep -Fq "${required}" <<<"${object_access_body}"; then
        echo "database object hook does not capture the authoritative OID: \
${required}" >&2
        exit 1
    fi
done

registry_walk_body="$(
    sed -n '/^tp_registry_walk(/,/^}/p' "${REGISTRY_SOURCE}"
)"

for required in \
    "dshash_seq_status *status" \
    "palloc(sizeof(*status))" \
    "dshash_seq_next(status)" \
    "dshash_seq_term(status)"; do
    if ! grep -Fq "${required}" <<<"${registry_walk_body}"; then
        echo "registry walk keeps mutable iterator state across longjmp: \
${required}" >&2
        exit 1
    fi
done

echo "State build source guards passed"
