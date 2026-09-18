#!/bin/bash
#
# Verify compaction ownership checks and maintenance/index lock ordering.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_FILE="${REPO_ROOT}/src/access/compaction_api.c"
BUILD_SOURCE="${REPO_ROOT}/src/access/build.c"
COMPACTION_SOURCE="${REPO_ROOT}/src/segment/compaction.c"
MERGE_SOURCE="${REPO_ROOT}/src/segment/merge.c"
TOMBSTONE_HEADER="${REPO_ROOT}/src/segment/tombstone.h"
TOMBSTONE_SOURCE="${REPO_ROOT}/src/segment/tombstone.c"
VACUUM_SOURCE="${REPO_ROOT}/src/access/vacuum.c"

open_body="$(
    sed -n '/^tp_open_bm25_index(Oid indexoid, LOCKMODE lockmode, bool need_owner)$/,/^}$/p' \
        "${SOURCE_FILE}"
)"

mapfile -t ownercheck_lines < <(
    grep -n "object_ownercheck(RelationRelationId, indexoid, GetUserId())" \
        <<<"${open_body}" |
        cut -d: -f1
)
relation_open_line="$(
    grep -n "index_rel = relation_open(indexoid, lockmode);" \
        <<<"${open_body}" |
        cut -d: -f1
)"

if [[ "${#ownercheck_lines[@]}" -ne 2 ]]; then
    echo "expected two compaction ownership checks, found ${#ownercheck_lines[@]}" >&2
    exit 1
fi

if [[ -z "${relation_open_line}" ||
      "${ownercheck_lines[0]}" -ge "${relation_open_line}" ||
      "${ownercheck_lines[1]}" -le "${relation_open_line}" ]]; then
    echo "compaction ownership checks must bracket relation_open" >&2
    exit 1
fi

maintenance_lock_body="$(
    sed -n '/^tp_compaction_lock(Relation index)$/,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
maintenance_unlock_body="$(
    sed -n '/^tp_compaction_unlock(Relation index)$/,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
if ! grep -Fq 'TP_COMPACTION_MAINTENANCE_LOCK_SUBID' \
        <<<"${maintenance_lock_body}" ||
   ! grep -Fq 'LockDatabaseObject' <<<"${maintenance_lock_body}" ||
   ! grep -Fq 'UnlockDatabaseObject' <<<"${maintenance_unlock_body}" ||
   grep -Fq 'LockRelationOid' <<<"${maintenance_lock_body}"; then
    echo "maintenance must use its private object lock, not managed relation admission" >&2
    exit 1
fi

check_spill_policy_order() {
    local function_name="$1"
    local function_body
    local acquire_line
    local spill_line
    local release_line
    local policy_line

    function_body="$(
        sed -n "/^${function_name}(/,/^}$/p" "${BUILD_SOURCE}"
    )"
    acquire_line="$(
        grep -n 'tp_acquire_index_lock(index_state, LW_EXCLUSIVE)' \
            <<<"${function_body}" | head -1 | cut -d: -f1 || true
    )"
    spill_line="$(
        grep -n 'tp_do_spill(index_state' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"
    release_line="$(
        grep -n 'tp_release_index_lock(index_state)' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"
    policy_line="$(
        grep -n 'tp_apply_compaction_policy' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"

    if [[ -z "${acquire_line}" || -z "${spill_line}" ||
          -z "${release_line}" || -z "${policy_line}" ||
          "${acquire_line}" -ge "${spill_line}" ||
          "${spill_line}" -ge "${release_line}" ||
          "${release_line}" -ge "${policy_line}" ]]; then
        echo "${function_name} must spill under LW_EXCLUSIVE, release it, \
then apply compaction policy" >&2
        exit 1
    fi
}

check_spill_policy_order tp_spill_memtable_if_needed
check_spill_policy_order tp_auto_spill_if_needed
check_spill_policy_order tp_spill_memtable

check_compaction_lock_order() {
    local function_name="$1"
    local function_body
    local maintenance_line

    function_body="$(
        sed -n "/^${function_name}(PG_FUNCTION_ARGS)$/,/^}$/p" \
            "${SOURCE_FILE}"
    )"
    maintenance_line="$(
        grep -n 'tp_compaction_lock(index_rel)' \
            <<<"${function_body}" | head -1 | cut -d: -f1 || true
    )"

    if [[ -z "${maintenance_line}" ]]; then
        echo "${function_name} must acquire the maintenance lock" >&2
        exit 1
    fi
    if grep -Fq 'tp_acquire_index_lock(index_state' <<<"${function_body}"; then
        echo "${function_name} must leave phase-specific index locking to compaction" >&2
        exit 1
    fi
}

check_compaction_lock_order tp_compact_index
check_compaction_lock_order tp_compact_index_step
check_compaction_lock_order tp_compact_index_step_if_current

current_step_body="$(
    sed -n '/^tp_compact_index_step_if_current(PG_FUNCTION_ARGS)$/,/^}$/p' \
        "${SOURCE_FILE}"
)"
current_step_precheck_line="$(
    grep -n 'tp_open_current_bm25_target' <<<"${current_step_body}" |
        head -1 | cut -d: -f1 || true
)"
current_step_maintenance_line="$(
    grep -n 'tp_compaction_lock(index_rel)' <<<"${current_step_body}" |
        head -1 | cut -d: -f1 || true
)"
current_step_recheck_line="$(
    grep -n 'tp_open_current_bm25_target' <<<"${current_step_body}" |
        tail -1 | cut -d: -f1 || true
)"
if [[ -z "${current_step_precheck_line}" ||
      -z "${current_step_maintenance_line}" ||
      -z "${current_step_recheck_line}" ||
      "${current_step_precheck_line}" -ge "${current_step_maintenance_line}" ||
      "${current_step_maintenance_line}" -ge "${current_step_recheck_line}" ||
      "${current_step_precheck_line}" -eq "${current_step_recheck_line}" ]]; then
    echo "generation-checked compaction must recheck its target after maintenance admission" >&2
    exit 1
fi

inline_body="$(
    sed -n '/^tp_compact_inline(TpLocalIndexState \*index_state, Relation index_rel)$/,/^}$/p' \
        "${BUILD_SOURCE}"
)"
if grep -Fq 'tp_acquire_index_lock(index_state' <<<"${inline_body}"; then
    echo "inline compaction must leave phase-specific index locking to compaction" >&2
    exit 1
fi

select_body="$(
    sed -n '/^tp_select_compaction_plan($/,/^}$/p' "${COMPACTION_SOURCE}"
)"
force_select_body="$(
    sed -n '/^tp_select_force_compaction_plan($/,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
build_body="$(
    sed -n '/^tp_build_compaction_output($/,/^}$/p' "${COMPACTION_SOURCE}"
)"
publish_body="$(
    sed -n '/^tp_publish_compaction_output($/,/^}$/p' "${COMPACTION_SOURCE}"
)"
prepare_body="$(
    sed -n '/^tp_prepare_compaction_publication($/,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
complete_body="$(
    sed -n '/^tp_complete_compaction_publication($/,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
validate_body="$(
    sed -n '/^tp_validate_selected_runs($/,/^}$/p' "${COMPACTION_SOURCE}"
)"
publish_acquire_line="$(
    grep -n 'tp_acquire_index_lock(index_state, LW_EXCLUSIVE)' \
        <<<"${publish_body}" | head -1 | cut -d: -f1 || true
)"
publish_restamp_line="$(
    grep -n 'tp_tombstone_restamp_detached' <<<"${complete_body}" |
        head -1 | cut -d: -f1 || true
)"
publish_output_validation_line="$(
    grep -n 'tp_validate_compaction_output' <<<"${complete_body}" |
        head -1 | cut -d: -f1 || true
)"
publish_attach_line="$(
    grep -n 'tp_tombstone_attach_detached' <<<"${publish_body}" |
        head -1 | cut -d: -f1 || true
)"
publish_finish_line="$(
    grep -n 'publication_lsn = GenericXLogFinish' <<<"${publish_body}" |
        head -1 | cut -d: -f1 || true
)"
publish_started_line="$(
    grep -n 'output->publication_started = true' <<<"${publish_body}" |
        head -1 | cut -d: -f1 || true
)"

if ! grep -Fq 'tp_acquire_index_lock(index_state, LW_SHARED)' \
    <<<"${select_body}" ||
   ! grep -Fq 'tp_release_index_lock(index_state)' <<<"${select_body}"; then
    echo "compaction selection must acquire and release LW_SHARED" >&2
    exit 1
fi
for selection_body in "${select_body}" "${force_select_body}"; do
    selection_release_line="$(
        grep -n 'tp_release_index_lock(index_state)' <<<"${selection_body}" |
            head -1 | cut -d: -f1 || true
    )"
    selection_collect_line="$(
        grep -nE 'tp_build_ordinary_plan|tp_collect_force_sources' \
            <<<"${selection_body}" | head -1 | cut -d: -f1 || true
    )"
    if [[ -z "${selection_release_line}" ||
          -z "${selection_collect_line}" ||
          "${selection_release_line}" -ge "${selection_collect_line}" ]]; then
        echo "compaction must release LW_SHARED before source collection" >&2
        exit 1
    fi
done
if grep -Fq 'tp_acquire_index_lock' <<<"${build_body}" ||
   grep -Fq 'tp_release_index_lock' <<<"${build_body}"; then
    echo "compaction output build must not manage the per-index lock" >&2
    exit 1
fi
publish_xid_line="$(
    grep -n 'merged_fxid = GetCurrentFullTransactionId()' \
        <<<"${complete_body}" | head -1 | cut -d: -f1 || true
)"
complete_prepare_line="$(
    grep -n 'tp_prepare_compaction_publication' <<<"${complete_body}" |
        head -1 | cut -d: -f1 || true
)"
complete_publish_line="$(
    grep -n 'tp_publish_compaction_output' <<<"${complete_body}" |
        head -1 | cut -d: -f1 || true
)"

if [[ -z "${publish_output_validation_line}" ||
      -z "${publish_xid_line}" || -z "${publish_restamp_line}" ||
      -z "${complete_prepare_line}" || -z "${complete_publish_line}" ||
      -z "${publish_acquire_line}" || -z "${publish_attach_line}" ||
      -z "${publish_finish_line}" || -z "${publish_started_line}" ||
      "${publish_output_validation_line}" -ge "${publish_xid_line}" ||
      "${publish_xid_line}" -ge "${publish_restamp_line}" ||
      "${publish_restamp_line}" -ge "${complete_prepare_line}" ||
      "${complete_prepare_line}" -ge "${complete_publish_line}" ||
      "${publish_acquire_line}" -ge "${publish_attach_line}" ||
      "${publish_finish_line}" -ge "${publish_started_line}" ]] ||
   ! grep -Fq 'output->tombstones, merged_fxid' <<<"${complete_body}" ||
   ! grep -Fq 'tp_prepare_compaction_publication' <<<"${complete_body}" ||
   ! grep -Fq 'tp_publish_compaction_output' <<<"${complete_body}" ||
   ! grep -Fq 'compaction publication retry' <<<"${complete_body}" ||
   ! grep -Fq 'GenericXLogAbort' <<<"${publish_body}" ||
   ! grep -Fq 'GenericXLogStart(index)' <<<"${publish_body}" ||
   ! grep -Fq 'tp_publication_identity_matches' <<<"${publish_body}" ||
   ! grep -Fq 'predecessor->next_segment = output->output_heads[0]' \
       <<<"${publish_body}" ||
   ! grep -Fq 'current_pending' <<<"${publish_body}" ||
   ! grep -Fq 'current_docs - output->removed_docs' <<<"${publish_body}"; then
    echo "compaction publication must own one exclusive WAL publication" >&2
    exit 1
fi
if ! grep -Fq 'tp_acquire_index_lock(index_state, LW_SHARED)' \
       <<<"${prepare_body}" ||
   ! grep -Fq 'tp_validate_selected_runs' <<<"${prepare_body}" ||
   ! grep -Fq 'tp_release_index_lock(index_state)' <<<"${prepare_body}" ||
   grep -Fq 'tp_validate_selected_runs' <<<"${publish_body}"; then
    echo "compaction prefix preparation must finish before LW_EXCLUSIVE" >&2
    exit 1
fi
if ! grep -Fq 'current_meta->level_counts[level] -' <<<"${validate_body}" ||
   ! grep -Fq 'snapshot->level_counts[level]' <<<"${validate_body}" ||
   ! grep -Fq '*l0_predecessor = current' <<<"${validate_body}"; then
    echo "compaction validation must preserve a spill-prepended L0 prefix" >&2
    exit 1
fi
if grep -Fq 'FlushRelationBuffers' <<<"${build_body}"; then
    echo "compaction build must not flush every relation buffer" >&2
    exit 1
fi

force_body="$(
    sed -n '/^tp_force_merge(PG_FUNCTION_ARGS)$/,/^}$/p' "${BUILD_SOURCE}"
)"
force_release_line="$(
    grep -n 'tp_release_index_lock(index_state)' <<<"${force_body}" |
        head -1 | cut -d: -f1
)"
force_compact_line="$(
    grep -n 'tp_force_compact(index_state, index_rel)' <<<"${force_body}" |
        cut -d: -f1
)"
truncate_lock_line="$(
    grep -n 'tp_acquire_index_lock(index_state, LW_EXCLUSIVE)' \
        <<<"${force_body}" | tail -1 | cut -d: -f1
)"
if [[ -z "${force_release_line}" || -z "${force_compact_line}" ||
      -z "${truncate_lock_line}" ||
      "${force_release_line}" -ge "${force_compact_line}" ||
      "${force_compact_line}" -ge "${truncate_lock_line}" ]]; then
    echo "force merge must build unlocked and reacquire only for truncation" >&2
    exit 1
fi

review_failures=0

bulkdelete_body="$(
    sed -n '/^tp_bulkdelete($/,/^}$/p' "${VACUUM_SOURCE}"
)"
vacuum_maintenance_line="$(
    grep -n 'tp_compaction_lock(info->index)' <<<"${bulkdelete_body}" |
        head -1 | cut -d: -f1 || true
)"
vacuum_index_line="$(
    grep -n 'tp_acquire_index_lock(index_state, LW_SHARED)' \
        <<<"${bulkdelete_body}" | head -1 | cut -d: -f1 || true
)"
if [[ -z "${vacuum_maintenance_line}" || -z "${vacuum_index_line}" ||
      "${vacuum_maintenance_line}" -ge "${vacuum_index_line}" ]] ||
   ! grep -Fq 'tp_compaction_unlock(info->index)' <<<"${bulkdelete_body}"; then
    echo "VACUUM segment mutation must hold maintenance before LW_SHARED" >&2
    review_failures=$((review_failures + 1))
fi

vacuum_replace_body="$(
    sed -n '/^tp_vacuum_replace_segment($/,/^}$/p' "${VACUUM_SOURCE}"
)"
vacuum_replace_xid_line="$(
    grep -n 'vacuum_fxid.*GetCurrentFullTransactionId()' \
        <<<"${vacuum_replace_body}" | head -1 | cut -d: -f1 || true
)"
vacuum_replace_tombstone_line="$(
    grep -n 'batch_head = tp_tombstone_enqueue_extend' \
        <<<"${vacuum_replace_body}" | head -1 | cut -d: -f1 || true
)"
if [[ -z "${vacuum_replace_xid_line}" ||
      -z "${vacuum_replace_tombstone_line}" ||
      "${vacuum_replace_xid_line}" -ge "${vacuum_replace_tombstone_line}" ]]; then
    echo "VACUUM replacement must assign its reclaim XID before tombstone build" >&2
    review_failures=$((review_failures + 1))
fi

discard_body="$(
    sed -n '/^tp_discard_compaction_output(Relation index, /,/^}$/p' \
        "${COMPACTION_SOURCE}"
)"
if ! grep -Fq 'owned_output_roots' <<<"${discard_body}" ||
   grep -Fq 'current = tp_discard_unpublished_segment' <<<"${discard_body}"; then
    echo "compaction cleanup must free only explicitly owned output roots" >&2
    review_failures=$((review_failures + 1))
fi

merge_batch_body="$(
    sed -n '/^tp_merge_segment_batch($/,/^}$/p' "${MERGE_SOURCE}"
)"
if ! grep -Fq 'sink.writer.pages' <<<"${merge_batch_body}" ||
   ! grep -Fq 'sink.page_index_pages' <<<"${merge_batch_body}" ||
   ! grep -Fq 'tp_discard_unpublished_pages' <<<"${merge_batch_body}"; then
    echo "merge cancellation must reclaim every tracked allocated page" >&2
    review_failures=$((review_failures + 1))
fi

if ! grep -Fq 'owned_pages' "${TOMBSTONE_HEADER}" ||
   ! grep -Fq 'TpDetachedTombstoneBatch *batch' "${TOMBSTONE_HEADER}" ||
   ! grep -Fq 'batch->owned_pages[batch->owned_count++]' \
       "${TOMBSTONE_SOURCE}"; then
    echo "detached tombstone build must expose incremental page ownership" >&2
    review_failures=$((review_failures + 1))
fi

drain_body="$(
    sed -n '/^tp_tombstone_drain($/,/^}$/p' "${TOMBSTONE_SOURCE}"
)"
drain_conflict_line="$(
    grep -n 'tombstone_log_reuse_conflict' <<<"${drain_body}" |
        head -1 | cut -d: -f1 || true
)"
drain_unlink_line="$(
    grep -n 'tombstone_unlink(index, victim_prev, victim, victim_next)' \
        <<<"${drain_body}" | head -1 | cut -d: -f1 || true
)"
drain_free_line="$(
    grep -n 'tp_record_free_index_page(index, victim_blocks' \
        <<<"${drain_body}" | head -1 | cut -d: -f1 || true
)"
if [[ -z "${drain_conflict_line}" || -z "${drain_unlink_line}" ||
      -z "${drain_free_line}" ||
      "${drain_conflict_line}" -ge "${drain_unlink_line}" ||
      "${drain_unlink_line}" -ge "${drain_free_line}" ]]; then
    echo "tombstone drain must WAL-log standby conflict before unlink and FSM reuse" >&2
    review_failures=$((review_failures + 1))
fi

attach_body="$(
    sed -n '/^tp_tombstone_attach_detached($/,/^}$/p' "${TOMBSTONE_SOURCE}"
)"
if ! grep -Fq 'PG_CATCH();' <<<"${attach_body}" ||
   ! grep -Fq 'UnlockReleaseBuffer(buf)' <<<"${attach_body}"; then
    echo "detached tombstone attachment must release its tail buffer on error" >&2
    review_failures=$((review_failures + 1))
fi

tombstone_alloc_body="$(
    sed -n '/^tombstone_alloc_page(Relation index, bool use_fsm)$/,/^}$/p' \
        "${TOMBSTONE_SOURCE}"
)"
if ! grep -Fq 'ExtendBufferedRel(' <<<"${tombstone_alloc_body}" ||
   ! grep -Fq 'BMR_REL(index)' <<<"${tombstone_alloc_body}" ||
   ! grep -Fq 'EB_LOCK_FIRST' <<<"${tombstone_alloc_body}" ||
   grep -Fq 'P_NEW' "${TOMBSTONE_SOURCE}"; then
    echo "extend-only tombstones must use the bulk extension reservation" >&2
    review_failures=$((review_failures + 1))
fi

validate_body="$(
    sed -n '/^tp_validate_selected_runs($/,/^}$/p' "${COMPACTION_SOURCE}"
)"
if ! grep -Fq 'HASH_ENTER' <<<"${validate_body}" ||
   ! grep -Fq 'current == snapshot->level_heads[level]' \
       <<<"${validate_body}"; then
    echo "L0 prefix validation must reject cycles and source overlap" >&2
    review_failures=$((review_failures + 1))
fi

if [[ "${review_failures}" -ne 0 ]]; then
    exit 1
fi

echo "Compaction ownership and lock ordering passed"
