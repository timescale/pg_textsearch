#!/bin/bash

snapshot_reader_conflict_output_is_valid() {
    local output=$1

    if grep -Eqi \
        'invalid (segment|memtable|page|magic)|magic mismatch|index corrupted|data corrupted' \
        "${output}"; then
        return 1
    fi
    grep -Fq "canceling statement due to conflict with recovery" "${output}"
}
