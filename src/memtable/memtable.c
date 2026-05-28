/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable.c — in-memory memtable cache top-level helpers
 *
 * Most cache primitives live in stringtable.c (string interning +
 * term->posting-list map) and posting.c (posting list mutation +
 * doclength table).  This file hosts the top-level lifecycle helpers
 * — currently just tp_cache_clear.
 *
 * See docs/memtable_cache.md.
 */
#include <postgres.h>

#include <storage/block.h>
#include <utils/dsa.h>
#include <utils/memutils.h>

#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"

void
tp_cache_clear(dsa_area *dsa, TpMemtable *memtable)
{
	if (dsa == NULL || memtable == NULL)
		return;

	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *string_table =
				tp_string_table_attach(dsa, memtable->string_hash_handle);

		/*
		 * tp_string_table_clear walks the entries, freeing each
		 * interned term string and its posting list back to the
		 * DSA, then deletes the entry.  After that, dshash_destroy
		 * frees the dshash table's own internal allocations.
		 */
		tp_string_table_clear(dsa, string_table);
		dshash_destroy(string_table);
		memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	}

	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *doclength_table =
				tp_doclength_table_attach(dsa, memtable->doc_lengths_handle);

		/*
		 * Doc-length entries are POD (ItemPointerData + int32) with
		 * no nested DSA allocations, so dshash_destroy alone is
		 * sufficient.
		 */
		dshash_destroy(doclength_table);
		memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	}

	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	pg_atomic_write_u64(&memtable->cursor_seq, 0);
	pg_atomic_write_u64(&memtable->estimated_bytes, 0);

	dsa_trim(dsa);
}
