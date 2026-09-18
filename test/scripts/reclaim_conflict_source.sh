#!/bin/bash
#
# Guard stock standby-conflict WAL before reclaimed page reuse.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FREEPAGE_SOURCE="${REPO_ROOT}/src/index/freepage.c"
TOMBSTONE_SOURCE="${REPO_ROOT}/src/segment/tombstone.c"
VACUUM_SOURCE="${REPO_ROOT}/src/access/vacuum.c"

helper_body="$(
    sed -n '/^tp_log_page_reuse_conflict(/,/^}/p' "${FREEPAGE_SOURCE}"
)"
reclaim_body="$(
    sed -n '/^tp_reclaim_dead_memtable_pages(/,/^}/p' "${VACUUM_SOURCE}"
)"

if [[ -z "${helper_body}" || -z "${reclaim_body}" ]]; then
    echo "could not extract reclaim conflict helpers" >&2
    exit 1
fi

if ! grep -Fq "XLOG_BTREE_REUSE_PAGE" <<<"${helper_body}" ||
   ! grep -Fq "snapshotConflictHorizon = horizon" <<<"${helper_body}"; then
    echo "shared free-page helper does not emit stock reuse conflict WAL" >&2
    exit 1
fi

if ! grep -Fq "tp_log_page_reuse_conflict(" "${TOMBSTONE_SOURCE}" ||
   grep -Fq "XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE)" \
        "${TOMBSTONE_SOURCE}"; then
    echo "tombstone reclaim does not use the shared conflict helper" >&2
    exit 1
fi

conflict_line="$(
    grep -nF "tp_log_page_reuse_conflict(" <<<"${reclaim_body}" |
        cut -d: -f1
)"
free_line="$(
    grep -nF "tp_record_free_index_page(indexrel, blk)" \
        <<<"${reclaim_body}" | cut -d: -f1
)"
if [[ -z "${conflict_line}" || -z "${free_line}" ||
      "${conflict_line}" -ge "${free_line}" ]] ||
   ! grep -Fq "dead_fxid" <<<"${reclaim_body}"; then
    echo "memtable reclaim does not log its dead_fxid before free stamping" >&2
    exit 1
fi

echo "Reclaimed-page standby conflict source guards passed"
