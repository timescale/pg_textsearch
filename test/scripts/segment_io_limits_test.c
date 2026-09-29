#include <postgres.h>

#include <assert.h>

#include "segment/alive_bitset.h"
#include "segment/io.h"

typedef void (*SegmentReadFn)(TpSegmentReader *, uint64, void *, uint64);

_Static_assert(
		_Generic(&tp_segment_read, SegmentReadFn: 1, default: 0),
		"tp_segment_read length must hold 2^32 bytes");

int
main(void)
{
	uint64 max = PG_UINT32_MAX;

	assert(!tp_string_pool_offset_overflows(0, max + 1));
	assert(tp_string_pool_offset_overflows(0, max + 2));
	assert(!tp_string_pool_offset_overflows(max, 1));
	assert(tp_string_pool_offset_overflows(max, 2));
	assert(tp_dictionary_offsets_fit(TP_MAX_DICTIONARY_TERMS));
	assert(!tp_dictionary_offsets_fit(TP_MAX_DICTIONARY_TERMS + 1));
	assert(tp_dictionary_size(TP_MAX_DICTIONARY_TERMS) == max + 1);
	assert(tp_posting_block_count(0) == 0);
	assert(tp_posting_block_count(TP_BLOCK_SIZE) == 1);
	assert(tp_posting_block_count(TP_BLOCK_SIZE + 1) == 2);
	assert(tp_posting_block_count(PG_UINT32_MAX) ==
		   PG_UINT32_MAX / TP_BLOCK_SIZE + 1);
	assert(tp_alive_bitset_size(PG_UINT32_MAX - 7) == 536870911);
	assert(tp_alive_bitset_size(PG_UINT32_MAX - 1) == 536870912);
	assert(tp_alive_bitset_size(PG_UINT32_MAX) == 536870912);
	assert(tp_document_count_fits(TP_MAX_GROWABLE_CAPACITY));
	assert(!tp_document_count_fits((uint64)TP_MAX_GROWABLE_CAPACITY + 1));

	return 0;
}
