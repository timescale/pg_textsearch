#!/bin/bash
#
# Guard transaction-dispatch invariants that cannot be observed reliably from
# SQL without exposing test-only state.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REQUEST_SOURCE="${REPO_ROOT}/src/index/compaction_request.c"
MODULE_SOURCE="${REPO_ROOT}/src/mod.c"
STATE_SOURCE="${REPO_ROOT}/src/index/state.c"
JOB_SOURCE="${TP_JOB_SOURCE_OVERRIDE:-${REPO_ROOT}/src/index/compaction_job.c}"
FRESH_SQL="${REPO_ROOT}/sql/pg_textsearch--1.5.0-dev.sql"
UPGRADE_SQL="${REPO_ROOT}/sql/pg_textsearch--1.4.0--1.5.0-dev.sql"

if ! grep -Fq '{"off", TP_COMPACTION_MANUAL}' "${MODULE_SOURCE}"; then
    echo "legacy compaction=off does not map to manual mode" >&2
    exit 1
fi

strip_c_comments() {
    awk '
    BEGIN {
        in_block = 0
        in_string = 0
        in_char = 0
        escaped = 0
    }
    {
        output = ""
        for (i = 1; i <= length($0); i++) {
            current = substr($0, i, 1)
            next_char = substr($0, i + 1, 1)

            if (in_block) {
                if (current == "*" && next_char == "/") {
                    in_block = 0
                    i++
                }
                continue
            }
            if (in_string || in_char) {
                output = output current
                if (escaped) {
                    escaped = 0
                } else if (current == "\\") {
                    escaped = 1
                } else if ((in_string && current == "\"") ||
                           (in_char && current == "\047")) {
                    in_string = 0
                    in_char = 0
                }
                continue
            }
            if (current == "/" && next_char == "*") {
                in_block = 1
                i++
                continue
            }
            if (current == "/" && next_char == "/")
                break
            if (current == "\"")
                in_string = 1
            else if (current == "\047")
                in_char = 1
            output = output current
        }
        print output
    }'
}

comment_only_source='
/* df.wait_for_signal and df.wait_for_schedule */
// df.explain
'
comment_free_fixture="$(
    printf '%s\n' "${comment_only_source}" | strip_c_comments
)"
for comment_token in \
    "df.wait_for_signal" \
    "df.wait_for_schedule" \
    "df.explain"; do
    if grep -Fq "${comment_token}" <<<"${comment_free_fixture}"; then
        echo "C comment stripping retained ${comment_token}" >&2
        exit 1
    fi
done

job_code="$(strip_c_comments <"${JOB_SOURCE}")"
discovery_body="$(
    sed -n '/^tp_populate_job_objects_as_owner(/,/^}/p' <<<"${job_code}"
)"
job_graph_body="$(
    sed -n '/^tp_append_job_graph(/,/^}/p' <<<"${job_code}"
)"
validation_body="$(
    sed -n '/^tp_validate_graph_as_owner(/,/^}/p' <<<"${job_code}"
)"

for required in \
    '"wait_for_signal"' \
    '"wait_for_schedule"'; do
    if ! grep -Fq "${required}" <<<"${discovery_body}"; then
        echo "managed compaction discovery is missing ${required}" >&2
        exit 1
    fi
done

for required in \
    "objects->wait_signal_function" \
    "objects->wait_schedule_function"; do
    if ! grep -Fq "${required}" <<<"${job_graph_body}"; then
        echo "managed compaction graph is missing ${required}" >&2
        exit 1
    fi
done

if ! grep -Fq "objects->explain_function" <<<"${validation_body}"; then
    echo "managed compaction validation is missing df.explain" >&2
    exit 1
fi

if grep -Eq 'cancel_function|tp_cancel_' <<<"${job_code}"; then
    echo "transactional reconciliation still calls df.cancel" >&2
    exit 1
fi

if ! grep -Fq "candidate->nargs - candidate->ndargs == 4" \
    <<<"${job_code}"; then
    echo "df.start resolution ignores trailing defaulted arguments" >&2
    exit 1
fi

owner_predicate_count="$(
    grep -Fc "instance.submitted_by::pg_catalog.oid" <<<"${job_code}" || true
)"
if [ "${owner_predicate_count}" -lt 4 ]; then
    echo "managed instance queries lack explicit owner predicates" >&2
    exit 1
fi

for required in \
    "bm25_compact_step_if_current" \
    "{sys_instance_id}" \
    "DEPENDENCY_NORMAL" \
    "AccessMethodRelationId" \
    "REGROLEOID" \
    "OPERATOR(pg_catalog.=)" \
    "OPERATOR(pg_catalog.~~)" \
    "ANY (ARRAY["; do
    if ! grep -Fq "${required}" <<<"${job_code}"; then
        echo "managed compaction source is missing ${required}" >&2
        exit 1
    fi
done

if ! grep -Fq "tp_compaction_job_preflight" "${JOB_SOURCE}" ||
    ! grep -Fq "tp_compaction_job_preflight" "${MODULE_SOURCE}"; then
    echo "background CIC admission is not preflighted" >&2
    exit 1
fi

if grep -Fq " IN (" "${JOB_SOURCE}"; then
    echo "managed compaction SQL contains a search-path-sensitive IN" >&2
    exit 1
fi

if grep -Fq "bm25_compact_pending" "${FRESH_SQL}" "${UPGRADE_SQL}"; then
    echo "database-wide background compaction sweep remains installed" >&2
    exit 1
fi

if grep -Eq '#include[[:space:]]*[<"].*pg_durable' "${JOB_SOURCE}"; then
    echo "managed compaction has a build-time pg_durable header dependency" >&2
    exit 1
fi

if grep -Fq "tp_compaction_drop_request(objectId)" "${MODULE_SOURCE}"; then
    echo "DROP removes pending requests before subtransaction outcome" >&2
    exit 1
fi

if ! grep -Fq "SearchSysCacheExists1" "${REQUEST_SOURCE}" ||
    ! grep -Fq "RELOID, ObjectIdGetDatum(indexoid)" "${REQUEST_SOURCE}"; then
    echo "pending requests are not revalidated before dispatch" >&2
    exit 1
fi

# Pending requests must live in TopTransactionContext, which PostgreSQL
# frees at commit, prepare, and abort alike.  TopMemoryContext would
# outlive the transaction and leak stale OIDs into the next one, and would
# need a discard at every transaction end, including PREPARE.
request_body="$(
    sed -n '/^tp_compaction_request(Oid indexoid)/,/^}/p' "${REQUEST_SOURCE}"
)"
if ! grep -Fq "MemoryContextSwitchTo(TopTransactionContext)" \
    <<<"${request_body}"; then
    echo "pending requests are not allocated in TopTransactionContext" >&2
    exit 1
fi
if grep -Fq "TopMemoryContext" <<<"${request_body}"; then
    echo "pending requests outlive the recording transaction" >&2
    exit 1
fi
if ! grep -Fq "MemoryContextRegisterResetCallback" <<<"${request_body}"; then
    echo "pending request list pointer is not cleared with its context" >&2
    exit 1
fi

# Running callback SQL at PRE_PREPARE can leave transaction-global state
# (notably XACT_FLAGS_ACCESSEDTEMPNAMESPACE) that PostgreSQL validates
# after the event and that subtransaction rollback cannot clear, making an
# otherwise valid PREPARE TRANSACTION fail.
preprepare_body="$(
    sed -n '/case XACT_EVENT_PRE_PREPARE:/,/break;/p' "${MODULE_SOURCE}"
)"
if grep -Fq "tp_compaction_flush_requests" <<<"${preprepare_body}"; then
    echo "PRE_PREPARE dispatches callback SQL" >&2
    exit 1
fi

# REINDEX tracking state is allocated below TopMemoryContext before it is
# published.  Any construction error must delete that child context explicitly.
reindex_tracking_body="$(
    sed -n \
        '/^tp_reindex_tracking_begin(/,/^tp_reindex_target_refresh_identity(/p' \
        "${MODULE_SOURCE}"
)"
if ! grep -Fq "PG_CATCH()" <<<"${reindex_tracking_body}" ||
    [ "$(grep -Fc "MemoryContextDelete(context)" \
        <<<"${reindex_tracking_body}")" -lt 2 ]; then
    echo "REINDEX tracking construction does not clean up on error" >&2
    exit 1
fi
tracking_try_line="$(
    grep -n "PG_TRY()" <<<"${reindex_tracking_body}" | head -1 | cut -d: -f1
)"
tracking_alloc_line="$(
    grep -n "MemoryContextAllocZero" <<<"${reindex_tracking_body}" |
        head -1 | cut -d: -f1
)"
if [[ -z "${tracking_try_line}" || -z "${tracking_alloc_line}" ||
      "${tracking_try_line}" -ge "${tracking_alloc_line}" ]]; then
    echo "REINDEX tracking allocates outside its cleanup boundary" >&2
    exit 1
fi
reindex_tracking_begin_body="$(
    sed -n '/^tp_reindex_tracking_begin(/,/^}/p' "${MODULE_SOURCE}"
)"
if grep -Fq "if (indexoids == NIL)" <<<"${reindex_tracking_begin_body}"; then
    echo "REINDEX cannot discover a managed index created by an event trigger" >&2
    exit 1
fi
if ! grep -Eq \
    "state->scope_refresh_once[[:space:]]*=[[:space:]]*OidIsValid" \
    "${MODULE_SOURCE}" ||
    ! grep -Fq "if (state->scope_refresh_once)" "${MODULE_SOURCE}" ||
    ! grep -Eq "state->scope_oid[[:space:]]*=[[:space:]]*InvalidOid" \
        "${MODULE_SOURCE}"; then
    echo "broad REINDEX scopes are repeatedly rescanned" >&2
    exit 1
fi
reindex_candidates_body="$(
    sed -n '/^tp_reindex_collect_candidates(/,/^}/p' "${MODULE_SOURCE}"
)"
reindex_refresh_body="$(
    sed -n '/^tp_reindex_target_refresh_identity(/,/^}/p' "${MODULE_SOURCE}"
)"
reindex_finish_body="$(
    sed -n '/^tp_finish_reindex_pending(/,/^}/p' "${MODULE_SOURCE}"
)"
if ! grep -Fq "target->identity.tablespace_oid" \
        <<<"${reindex_candidates_body}" ||
    ! grep -Fq "target->identity.relfilenumber" \
        <<<"${reindex_candidates_body}" ||
    ! grep -Fq "target->pending" <<<"${reindex_candidates_body}" ||
    ! grep -Fq "target->completed" <<<"${reindex_candidates_body}" ||
    ! grep -Fq "pending_index_oid" <<<"${reindex_candidates_body}" ||
    ! grep -Fq "intermediate_targets" <<<"${reindex_candidates_body}" ||
    ! grep -Fq "final_pass ? state->targets" \
        <<<"${reindex_candidates_body}" ||
    ! grep -Fq "!final_pass" <<<"${reindex_candidates_body}" ||
    ! grep -Fq "target->current_index_oid" <<<"${reindex_refresh_body}" ||
    grep -Fq "identity->index_oid" <<<"${reindex_refresh_body}" ||
    ! grep -Fq "tp_reconciled_indexoids" <<<"${reindex_finish_body}" ||
    ! grep -Fq "target->pending_index_oid" <<<"${reindex_finish_body}" ||
    ! grep -Fq "tp_finish_reindex_pending" "${MODULE_SOURCE}" ||
    grep -Fq "pending_targets" "${MODULE_SOURCE}" ||
    grep -Fq "completed_targets" "${MODULE_SOURCE}"; then
    echo "broad REINDEX does not defer completed targets until its final pass" \
        >&2
    exit 1
fi

ensure_lineage_body="$(
    sed -n \
        '/^tp_ensure_index_compaction_lineage(Oid indexoid, bool \*created)/,/^}/p' \
        "${REQUEST_SOURCE}"
)"
if ! grep -Fq "AccessShareLock" <<<"${ensure_lineage_body}" ||
    ! grep -Fq "ShareUpdateExclusiveLock" <<<"${ensure_lineage_body}"; then
    echo "lineage lookup/backfill lacks its required relation locks" >&2
    exit 1
fi
ensure_read_line="$(
    grep -n "try_index_open(indexoid, AccessShareLock)" \
        <<<"${ensure_lineage_body}" | head -1 | cut -d: -f1
)"
ensure_write_line="$(
    grep -n "try_index_open(indexoid, ShareUpdateExclusiveLock)" \
        <<<"${ensure_lineage_body}" | head -1 | cut -d: -f1
)"
ensure_private_line="$(
    grep -n "tp_lock_compaction_index(indexoid)" \
        <<<"${ensure_lineage_body}" | head -1 | cut -d: -f1
)"
ensure_recheck_line="$(
    grep -n "existing = tp_index_compaction_lineage(index_rel)" \
        <<<"${ensure_lineage_body}" | tail -1 | cut -d: -f1
)"
if [[ -z "${ensure_read_line}" || -z "${ensure_write_line}" ||
      -z "${ensure_private_line}" ||
      "${ensure_read_line}" -ge "${ensure_write_line}" ||
      "${ensure_write_line}" -ge "${ensure_private_line}" ]]; then
    echo "legacy lineage backfill takes its private lock before the relation" \
        >&2
    exit 1
fi
if [[ -z "${ensure_recheck_line}" ||
      "${ensure_private_line}" -ge "${ensure_recheck_line}" ]]; then
    echo "legacy lineage backfill does not recheck options under both locks" \
        >&2
    exit 1
fi

prelock_requests_body="$(
    sed -n '/^tp_prelock_requests(List \*pending)/,/^}/p' "${REQUEST_SOURCE}"
)"
if ! grep -Fq "list_sort(sorted, list_oid_cmp)" \
    <<<"${prelock_requests_body}" ||
    ! grep -Fq "ConditionalLockRelationOid" \
    <<<"${prelock_requests_body}"; then
    echo "pending requests are not safely prelocked in OID order" >&2
    exit 1
fi
if ! grep -Fq "tp_take_compaction_lock(" \
    <<<"${prelock_requests_body}" ||
    [ "$(grep -Fc "foreach (lc, targets)" \
        <<<"${prelock_requests_body}")" -lt 1 ]; then
    echo "pending requests do not prelock every admission before signaling" \
        >&2
    exit 1
fi
if grep -Fq "tp_alter_index_ensure_lineage" "${MODULE_SOURCE}"; then
    echo "ALTER still injects a legacy lineage before serialized activation" \
        >&2
    exit 1
fi

if grep -Fq "SET_LOCKTAG_ADVISORY" "${REQUEST_SOURCE}" ||
    ! grep -Fq "LockDatabaseObject" "${REQUEST_SOURCE}" ||
    ! grep -Fq "AccessMethodRelationId" "${REQUEST_SOURCE}"; then
    echo "internal lineage locks share PostgreSQL's advisory-lock namespace" \
        >&2
    exit 1
fi
if grep -Fq "pg_advisory_xact_lock" "${JOB_SOURCE}" ||
    ! grep -Fq "tp_require_compaction_index_lock(indexoid)" \
        "${JOB_SOURCE}"; then
    echo "workflow admission shares PostgreSQL's advisory-lock namespace" \
        >&2
    exit 1
fi

if grep -Eq 'indexoid[[:space:]]*%|TP_COMPACTION_INDEX_LOCK_MAX' \
    "${REQUEST_SOURCE}"; then
    echo "per-index admission locks must preserve the full index OID" >&2
    exit 1
fi
index_lock_body="$(
    sed -n '/^tp_lock_compaction_index(Oid indexoid)/,/^}/p' \
        "${REQUEST_SOURCE}"
)"
compaction_lock_body="$(
    sed -n '/^tp_take_compaction_lock(/,/^}/p' "${REQUEST_SOURCE}"
)"
if ! grep -Fq \
    "indexoid, TP_COMPACTION_INDEX_LOCK_SUBID, ExclusiveLock, false" \
    <<<"${index_lock_body}"; then
    echo "per-index admission locks must use indexoid as the object ID" >&2
    exit 1
fi
if ! grep -Fq "LockHeldByMe" <<<"${compaction_lock_body}"; then
    echo "reentrant admission calls can reacquire after the dependency lock" \
        >&2
    exit 1
fi
if grep -Eq \
    'tp_compaction_admission_allowed|tp_check_compaction_admission_order|cannot acquire a new background compaction admission' \
    "${REQUEST_SOURCE}"; then
    echo "transaction-wide managed admission rejection remains installed" \
        >&2
    exit 1
fi

try_prelock_indexes_body="$(
    sed -n \
        '/^tp_try_prelock_compaction_indexes(List \*indexoids)/,/^}/p' \
        "${REQUEST_SOURCE}"
)"
for required in \
    "list_sort(sorted, list_oid_cmp)" \
    "tp_compaction_dependency_lock_held()" \
    "ConditionalLockRelationOid" \
    "tp_take_compaction_lock("; do
    if ! grep -Fq "${required}" <<<"${try_prelock_indexes_body}"; then
        echo "terminal index batching is missing ${required}" >&2
        exit 1
    fi
done
try_relation_line="$(
    grep -n "ConditionalLockRelationOid" \
        <<<"${try_prelock_indexes_body}" | head -1 | cut -d: -f1
)"
try_admission_line="$(
    grep -n "tp_take_compaction_lock(" \
        <<<"${try_prelock_indexes_body}" | head -1 | cut -d: -f1
)"
if [[ -z "${try_relation_line}" || -z "${try_admission_line}" ||
      "${try_relation_line}" -ge "${try_admission_line}" ]]; then
    echo "terminal batching does not take relations before admissions" >&2
    exit 1
fi

prelock_requests_body="$(
    sed -n '/^tp_prelock_requests(List \*pending)/,/^}/p' "${REQUEST_SOURCE}"
)"
request_relation_line="$(
    grep -n "ConditionalLockRelationOid" \
        <<<"${prelock_requests_body}" | head -1 | cut -d: -f1
)"
if ! grep -Fq "tp_compaction_dependency_lock_held()" \
    <<<"${prelock_requests_body}" ||
    ! grep -Fq "tp_compaction_lock_held(" <<<"${prelock_requests_body}" ||
    [ -z "${request_relation_line}" ]; then
    echo "pre-commit dispatch does not skip new admissions after dependency" \
        >&2
    exit 1
fi

option_reconcile_body="$(
    sed -n \
        '/^tp_reconcile_index_compaction_options(/,/^}/p' \
        "${REQUEST_SOURCE}"
)"
if grep -Fq "tp_lock_compaction_index" <<<"${option_reconcile_body}"; then
    echo "partition option reconciliation reacquires admission after discovery" \
        >&2
    exit 1
fi

reindex_candidate_body="$(
    sed -n '/^tp_reindex_try_relation_open(/,/^}/p;
             /^tp_reindex_relation_close(/,/^}/p;
             /^tp_reindex_target_capture(/,/^}/p;
             /^tp_reindex_target_live_original(/,/^}/p;
             /^tp_reindex_collect_candidates(/,/^}/p' "${MODULE_SOURCE}"
)"
reindex_intent_body="$(
    sed -n '/^tp_collect_reindex_state_intents(/,/^}/p' "${MODULE_SOURCE}"
)"
tracked_rewrite_body="$(
    sed -n '/^tp_report_nowait_relation_lock_error(/,/^}/p;
             /^tp_physical_bm25_indexes_for_rewrite(/,/^}/p;
             /^tp_process_tracked_rewrite(/,/^}/p' "${MODULE_SOURCE}"
)"
if grep -Fq "try_relation_open(target->index_oid, AccessShareLock)" \
        <<<"${reindex_candidate_body}" ||
    grep -Fq "try_relation_open(indexoid, AccessShareLock)" \
        <<<"${reindex_candidate_body}" ||
    ! grep -Fq "ConditionalLockRelationOid" \
        <<<"${reindex_candidate_body}" ||
    ! grep -Fq "state->nowait" <<<"${reindex_candidate_body}" ||
    ! grep -Fq "UnlockRelationOid" <<<"${reindex_candidate_body}" ||
    ! grep -Fq "tp_collect_prevalidated_managed_intent" \
        <<<"${reindex_intent_body}"; then
    echo "post-publication target discovery can block before the terminal batch" \
        >&2
    exit 1
fi
if grep -Fq "(void)nowait" <<<"${tracked_rewrite_body}" ||
    ! grep -Fq "ConditionalLockRelationOid" <<<"${tracked_rewrite_body}" ||
    ! grep -Fq "ERRCODE_OBJECT_IN_USE" <<<"${tracked_rewrite_body}" ||
    ! grep -Fq "tp_physical_bm25_indexes_for_rewrite(indexoids, nowait)" \
        <<<"${tracked_rewrite_body}"; then
    echo "tracked rewrite discovery does not preserve NOWAIT semantics" >&2
    exit 1
fi

utility_wrapper_body="$(
    sed -n '/^tp_process_utility(/,/^}/p' "${MODULE_SOURCE}"
)"
precommit_reconcile_body="$(
    sed -n '/^tp_reconcile_managed_intents_at_precommit(/,/^}/p' \
        "${MODULE_SOURCE}"
)"
managed_reconcile_body="$(
    sed -n '/^tp_reconcile_managed_intents(void)/,/^}/p' "${MODULE_SOURCE}"
)"
reindex_state_intent_body="$(
    sed -n '/^tp_collect_reindex_state_intents(/,/^}/p' "${MODULE_SOURCE}"
)"
if ! grep -Fq "tp_managed_reconciling" <<<"${utility_wrapper_body}" ||
    ! grep -Fq "GrantStmt" <<<"${utility_wrapper_body}" ||
    ! grep -Fq "tp_post_publication_reconciliation" \
        <<<"${reindex_state_intent_body}" ||
    ! grep -Fq "only_state != NULL && state != only_state" \
        <<<"${reindex_state_intent_body}" ||
    ! grep -Fq "tp_post_publication_reconciliation" \
        <<<"${precommit_reconcile_body}" ||
    ! grep -Fq "BeginInternalSubTransaction" \
        <<<"${precommit_reconcile_body}" ||
    ! grep -Fq "ERRCODE_QUERY_CANCELED" \
        <<<"${precommit_reconcile_body}" ||
    ! grep -Eq \
        "reconciled[[:space:]]*=[[:space:]]*tp_reconcile_managed_intents\\(\\)" \
        <<<"${precommit_reconcile_body}" ||
    ! grep -Fq "return !deferred;" <<<"${managed_reconcile_body}"; then
    echo "terminal reconciliation does not safely reject utility reentry" \
        >&2
    exit 1
fi

dependency_lock_body="$(
    sed -n '/^tp_lock_compaction_dependency(void)/,/^}/p' "${REQUEST_SOURCE}"
)"
if ! grep -Fq "ShareRowExclusiveLock" <<<"${dependency_lock_body}" ||
    ! grep -Fq "LockDatabaseObject" <<<"${dependency_lock_body}"; then
    echo "managed lifecycle dependency serialization is not explicit" >&2
    exit 1
fi
if ! grep -Fq "tp_require_compaction_dependency_lock" \
    "${JOB_SOURCE}"; then
    echo "dependency-backed job discovery lacks a lock assertion" >&2
    exit 1
fi

pin_dependency_body="$(
    sed -n '/^tp_pin_durable_dependency(/,/^}/p' "${JOB_SOURCE}"
)"
if grep -Fq "LockDatabaseObject" <<<"${pin_dependency_body}"; then
    echo "dependency catalog work still acquires its serialization lock" >&2
    exit 1
fi

object_bundle_body="$(
    sed -n '/^tp_compaction_job_try_lock_objects(bool invalid_is_error)/,/^}/p' \
        "${JOB_SOURCE}"
)"
durable_extension_line="$(
    grep -n "objects.durable_extension_oid" \
        <<<"${object_bundle_body}" | head -1 | cut -d: -f1
)"
textsearch_extension_line="$(
    grep -n "objects.textsearch_extension_oid" \
        <<<"${object_bundle_body}" | head -1 | cut -d: -f1
)"
bundle_dependency_line="$(
    grep -n "tp_try_lock_compaction_dependency_oid(" \
        <<<"${object_bundle_body}" | head -1 | cut -d: -f1
)"
bundle_member_line="$(
    grep -n "qsort(locks" \
        <<<"${object_bundle_body}" | head -1 | cut -d: -f1
)"
if [[ -z "${durable_extension_line}" ||
      -z "${textsearch_extension_line}" ||
      -z "${bundle_dependency_line}" ||
      -z "${bundle_member_line}" ||
      "${durable_extension_line}" -ge "${textsearch_extension_line}" ||
      "${textsearch_extension_line}" -ge "${bundle_dependency_line}" ||
      "${bundle_dependency_line}" -ge "${bundle_member_line}" ]] ||
    ! grep -Fq "ConditionalLockDatabaseObject" \
        <<<"${object_bundle_body}" ||
    ! grep -Fq "ConditionalLockRelationOid" \
        <<<"${object_bundle_body}" ||
    ! grep -Fq "tp_job_objects_still_match(&objects)" \
        <<<"${object_bundle_body}"; then
    echo "terminal object bundle violates extension/dependency/member order" \
        >&2
    exit 1
fi
if [ "$(grep -Fc "RowExclusiveLock" <<<"${object_bundle_body}")" -lt 3 ] ||
    ! grep -Fq "objects.instances_relation_oid" \
        <<<"${object_bundle_body}" ||
    ! grep -Fq "objects.nodes_relation_oid" <<<"${object_bundle_body}" ||
    ! grep -Fq "objects.vars_relation_oid" <<<"${object_bundle_body}"; then
    echo "pg_durable writable relations lack terminal write locks" >&2
    exit 1
fi
if ! grep -Fq "objects.bm25_am_oid" <<<"${object_bundle_body}" ||
    ! grep -Fq \
        "tp_try_lock_compaction_dependency_oid(objects.bm25_am_oid)" \
        <<<"${object_bundle_body}" ||
    ! grep -Fq "objects->bm25_am_oid" \
        <<<"$(sed -n '/^tp_job_objects_still_match(/,/^}/p' \
            "${JOB_SOURCE}")"; then
    echo "terminal dependency admission does not use the discovered AM OID" \
        >&2
    exit 1
fi

lineage_exists_body="$(
    sed -n '/^tp_compaction_job_lineage_exists(/,/^}/p' "${JOB_SOURCE}"
)"
if ! grep -Fq "tp_require_compaction_dependency_lock()" \
    <<<"${lineage_exists_body}"; then
    echo "durable lineage history is queried without the object bundle" >&2
    exit 1
fi

for entrypoint in \
    tp_compaction_job_activate \
    tp_compaction_job_activate_with_schedule \
    tp_compaction_job_signal; do
    entrypoint_body="$(
        sed -n "/^${entrypoint}(/,/^}/p" "${JOB_SOURCE}"
    )"
    target_lock_line="$(
        grep -n "tp_require_compaction_index_lock(indexoid)" \
            <<<"${entrypoint_body}" | cut -d: -f1
    )"
    dependency_lock_line="$(
        grep -n "tp_require_compaction_dependency_lock()" \
            <<<"${entrypoint_body}" | cut -d: -f1
    )"
    capture_line="$(
        grep -n "tp_capture_target(indexoid" \
            <<<"${entrypoint_body}" | cut -d: -f1
    )"
    if [[ -z "${target_lock_line}" || -z "${dependency_lock_line}" ||
          -z "${capture_line}" ||
          "${target_lock_line}" -ge "${dependency_lock_line}" ||
          "${dependency_lock_line}" -ge "${capture_line}" ]]; then
        echo "${entrypoint} enters discovery before managed lock ordering" >&2
        exit 1
    fi
done

capture_body="$(
    sed -n '/^tp_compaction_job_capture(/,/^}/p' "${JOB_SOURCE}"
)"
if ! grep -Fq "try_relation_open(indexoid, AccessShareLock)" \
    <<<"${capture_body}" ||
    grep -Eq \
        'tp_require_compaction_index_lock|tp_lock_compaction|tp_ensure_index_compaction_lineage' \
        <<<"${capture_body}"; then
    echo "managed identity capture is not read-only local collection" >&2
    exit 1
fi

schedule_body="$(
    sed -n '/^tp_compaction_job_resolve_schedule(/,/^}/p' "${JOB_SOURCE}"
)"
schedule_admission_line="$(
    grep -n "tp_require_compaction_index_lock(indexoid)" \
        <<<"${schedule_body}" | cut -d: -f1
)"
schedule_dependency_line="$(
    grep -n "tp_lock_compaction_dependency()" \
        <<<"${schedule_body}" | cut -d: -f1 || true
)"
schedule_query_line="$(
    grep -n "tp_schedule_as_trusted(objects" \
        <<<"${schedule_body}" | cut -d: -f1
)"
if [[ -z "${schedule_admission_line}" ||
      -z "${schedule_query_line}" ||
      "${schedule_admission_line}" -ge "${schedule_query_line}" ]] ||
    [ -n "${schedule_dependency_line}" ]; then
    echo "captured schedule resolution bypasses managed lock ordering" >&2
    exit 1
fi

preflight_body="$(
    sed -n '/^tp_compaction_job_preflight(/,/^}/p' "${JOB_SOURCE}"
)"
if ! grep -Fq "tp_preflight_job_objects(&objects)" <<<"${preflight_body}" ||
    ! grep -Fq "tp_preflight_job_objects(&objects)" \
        <<<"${object_bundle_body}" ||
    ! grep -Fq "tp_job_objects_still_match(&objects)" \
        <<<"${object_bundle_body}"; then
    echo "object preflight and locked identity validation are not split" \
        >&2
    exit 1
fi
for locked_entrypoint in \
    tp_activate_captured_target \
    tp_compaction_job_resolve_schedule \
    tp_compaction_job_signal; do
    locked_entrypoint_body="$(
        sed -n "/^${locked_entrypoint}(/,/^}/p" "${JOB_SOURCE}"
    )"
    if grep -Fq "tp_discover_locked_job_objects" \
        <<<"${locked_entrypoint_body}" ||
        ! grep -Fq "objects" <<<"${locked_entrypoint_body}"; then
        echo "${locked_entrypoint} does not consume the admitted object bundle" \
            >&2
        exit 1
    fi
done

activation_body="$(
    sed -n '/^tp_activate_captured_target(/,/^}/p' "${JOB_SOURCE}"
)"
if grep -Eq 'tp_take_admission_lock|tp_lock_durable_dependency' \
    <<<"${activation_body}"; then
    echo "captured activation can acquire locks after dependency discovery" >&2
    exit 1
fi

collect_intent_body="$(
    sed -n '/^tp_collect_managed_intent(/,/^}/p' "${MODULE_SOURCE}"
)"
if grep -Eq \
    'tp_lock_compaction|tp_compaction_job_|SPI_|LockRelation|LockDatabaseObject' \
    <<<"${collect_intent_body}"; then
    echo "managed intent collection performs terminal reconciliation work" \
        >&2
    exit 1
fi

managed_reconcile_body="$(
    sed -n '/^tp_reconcile_managed_intents(void)/,/^}/p' "${MODULE_SOURCE}"
)"
managed_batch_line="$(
    grep -n "tp_try_prelock_compaction_indexes(indexoids)" \
        <<<"${managed_reconcile_body}" | head -1 | cut -d: -f1
)"
managed_bundle_line="$(
    grep -n "tp_compaction_job_try_lock_objects(" \
        <<<"${managed_reconcile_body}" | head -1 | cut -d: -f1
)"
managed_lineage_line="$(
    grep -n "tp_compaction_job_lineage_exists(" \
        <<<"${managed_reconcile_body}" | head -1 | cut -d: -f1
)"
managed_activation_line="$(
    grep -n "tp_compaction_job_activate" \
        <<<"${managed_reconcile_body}" | head -1 | cut -d: -f1
)"
if [[ -z "${managed_batch_line}" || -z "${managed_bundle_line}" ||
      -z "${managed_lineage_line}" || -z "${managed_activation_line}" ||
      "${managed_batch_line}" -ge "${managed_bundle_line}" ||
      "${managed_bundle_line}" -ge "${managed_lineage_line}" ||
      "${managed_lineage_line}" -ge "${managed_activation_line}" ]]; then
    echo "managed intents bypass the terminal lock and validation batch" >&2
    exit 1
fi
if ! grep -Fq \
    "was deferred" \
    <<<"${managed_reconcile_body}"; then
    echo "contended managed reconciliation can be silently discarded" >&2
    exit 1
fi

if ! grep -Fq "RangeVarCallbackOwnsRelation" "${MODULE_SOURCE}"; then
    echo "CREATE INDEX locking does not preserve core authorization ordering" \
        >&2
    exit 1
fi

lineage_guard_body="$(
    sed -n '/^tp_reject_user_lineage_alter(/,/^}/p' "${MODULE_SOURCE}"
)"
for required in \
    "AlterTableGetLockLevel" \
    "RangeVarGetRelidExtended" \
    "RangeVarCallbackOwnsRelation" \
    "try_relation_open(indexoid, NoLock)"; do
    if ! grep -Fq "${required}" <<<"${lineage_guard_body}"; then
        echo "lineage mutation guard does not retain the resolved target" >&2
        exit 1
    fi
done

if ! grep -Fq "OAT_POST_CREATE" "${MODULE_SOURCE}" ||
    ! grep -Fq "tp_create_index_tracking_begin" "${MODULE_SOURCE}" ||
    grep -Fq "list_difference_oid(indexes_after" "${MODULE_SOURCE}"; then
    echo "CREATE INDEX tracking is not invocation-owned" >&2
    exit 1
fi

if grep -Fq "tp_prelock_compaction_indexes(" "${MODULE_SOURCE}"; then
    echo "utility collection still retains managed admissions before core" \
        >&2
    exit 1
fi

# A spill caused during dispatch must compact inline: its request would land
# in a list the running dispatch has already stopped reading.
if ! grep -Fq "tp_dispatch_active = true" "${REQUEST_SOURCE}" ||
    ! grep -Fq "tp_dispatch_active" "${REQUEST_SOURCE}"; then
    echo "managed dispatch re-entry is not routed to inline compaction" >&2
    exit 1
fi

flush_body="$(
    sed -n '/^tp_compaction_flush_requests(void)/,$p' "${REQUEST_SOURCE}"
)"
revalidate_line="$(grep -n "SearchSysCacheExists1" <<<"${flush_body}" |
    cut -d: -f1)"
signal_line="$(grep -n "tp_run_request(indexoid)" \
    <<<"${flush_body}" | cut -d: -f1)"
prelock_line="$(grep -n "tp_prelock_requests(pending)" \
    <<<"${flush_body}" | cut -d: -f1)"

if [[ -z "${prelock_line}" || -z "${revalidate_line}" ||
      -z "${signal_line}" ||
      "${prelock_line}" -ge "${revalidate_line}" ||
      "${revalidate_line}" -ge "${signal_line}" ]]; then
    echo "pending requests are not prelocked and revalidated before signaling" \
        >&2
    exit 1
fi

bulk_spill_body="$(
    sed -n '/^tp_bulk_load_spill_check(void)/,/^tp_reset_bulk_load_counters(void)/p' \
        "${STATE_SOURCE}"
)"
open_line="$(grep -n "index_rel = try_index_open" <<<"${bulk_spill_body}" |
    cut -d: -f1)"
acquire_line="$(grep -n "tp_acquire_index_lock(local_state" \
    <<<"${bulk_spill_body}" | cut -d: -f1)"
release_line="$(grep -n "tp_release_index_lock(local_state" \
    <<<"${bulk_spill_body}" | tail -1 | cut -d: -f1)"
close_line="$(grep -n "index_close(index_rel" <<<"${bulk_spill_body}" |
    cut -d: -f1)"

if [[ -z "${open_line}" || -z "${acquire_line}" ||
      "${open_line}" -ge "${acquire_line}" ]]; then
    echo "bulk spill does not open the relation before its index LWLock" >&2
    exit 1
fi

if [[ -z "${release_line}" || -z "${close_line}" ||
      "${release_line}" -ge "${close_line}" ]]; then
    echo "bulk spill does not release its index LWLock before relation close" >&2
    exit 1
fi

echo "Compaction request source guards passed"
