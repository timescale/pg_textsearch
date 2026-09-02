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

if grep -Fq "tp_compaction_drop_request(objectId)" "${MODULE_SOURCE}"; then
    echo "DROP removes pending requests before subtransaction outcome" >&2
    exit 1
fi

if ! grep -Fq "SearchSysCacheExists1" "${REQUEST_SOURCE}" ||
    ! grep -Fq "RELOID, ObjectIdGetDatum(indexoid)" "${REQUEST_SOURCE}"; then
    echo "pending requests are not revalidated before dispatch" >&2
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
if ! grep -Fq "tp_compaction_reset_requests" <<<"${preprepare_body}"; then
    echo "PRE_PREPARE does not discard pending requests" >&2
    exit 1
fi

# A one-part callback name would resolve through the committing backend's
# search_path and run as that backend's user.
if ! grep -Fq "list_length(names) == 2" "${REQUEST_SOURCE}"; then
    echo "callback GUC does not require a schema-qualified name" >&2
    exit 1
fi

# A spill caused by the callback must compact inline: its request would
# land in a list the running dispatch has already stopped reading.
if ! grep -Fq "tp_dispatch_active = true" "${REQUEST_SOURCE}" ||
    ! grep -Fq "tp_dispatch_active" "${REQUEST_SOURCE}"; then
    echo "callback re-entry is not routed to inline compaction" >&2
    exit 1
fi

flush_body="$(
    sed -n '/^tp_compaction_flush_requests(void)/,$p' "${REQUEST_SOURCE}"
)"
revalidate_line="$(grep -n "SearchSysCacheExists1" <<<"${flush_body}" |
    cut -d: -f1)"
lookup_line="$(grep -n "function = tp_lookup_request_function" \
    <<<"${flush_body}" | cut -d: -f1)"

if [[ -z "${revalidate_line}" || -z "${lookup_line}" ||
      "${revalidate_line}" -ge "${lookup_line}" ]]; then
    echo "callback lookup is not lazy on the first live request" >&2
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
