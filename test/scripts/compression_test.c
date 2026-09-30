/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#include <postgres.h>

#undef NDEBUG
#include <assert.h>
#include <setjmp.h>

#include "segment/compression.h"

/* Use the real codec and format types without a running backend. */
#undef fprintf
#undef printf
#undef Assert
#define Assert(condition) assert(condition)
#undef ereport
#define ereport(level, details) decode_error()
#define palloc(size)			malloc(size)
#define pfree(pointer)			free(pointer)

static jmp_buf error_jump;
static bool	   expect_error;

static void
decode_error(void)
{
	if (expect_error)
		longjmp(error_jump, 1);
	fprintf(stderr, "unexpected codec error\n");
	abort();
}

#include "../../src/segment/compression.c"

static uint32
width_mask(uint8 bits)
{
	return bits == 32 ? UINT32_MAX : (UINT32_C(1) << bits) - 1;
}

/* Independent, bit-at-a-time encoder for the existing wire format. */
static uint32
pack_values(const uint32 *values, uint32 count, uint8 bits, uint8 *output)
{
	uint32 bit_offset = 0;

	for (uint32 i = 0; i < count; i++)
		for (uint8 bit = 0; bit < bits; bit++, bit_offset++)
			output[bit_offset / 8] |= ((values[i] >> bit) & 1U)
								   << (bit_offset % 8);
	return (bit_offset + 7) / 8;
}

static uint32
test_value(uint32 i, uint8 bits)
{
	uint32 mask = width_mask(bits);

	switch (i % 4)
	{
	case 0:
		return 0;
	case 1:
		return mask;
	case 2:
		return 1;
	default:
		return (i * UINT32_C(2654435761)) & mask;
	}
}

static void
check_block(uint8 doc_bits, uint8 freq_bits, uint32 count, uint32 alignment)
{
	uint8 *storage = calloc(1, TP_MAX_COMPRESSED_BLOCK_SIZE + alignment);
	uint8 *compressed;
	uint32 deltas[TP_BLOCK_SIZE];
	uint32 frequencies[TP_BLOCK_SIZE];
	TpBlockPosting postings[TP_BLOCK_SIZE + 1];
	uint32		   pos;
	uint32		   previous = UINT32_MAX - 15;

	assert(storage != NULL);
	compressed	  = storage + alignment;
	compressed[0] = doc_bits;
	compressed[1] = freq_bits;
	pos			  = sizeof(TpCompressedBlockHeader);
	for (uint32 i = 0; i < count; i++)
	{
		deltas[i]	   = test_value(i, doc_bits);
		frequencies[i] = test_value(i + 1, freq_bits);
	}
	pos += pack_values(deltas, count, doc_bits, compressed + pos);
	pos += pack_values(frequencies, count, freq_bits, compressed + pos);
	for (uint32 i = 0; i < count; i++)
		compressed[pos + i] = (uint8)(i * 37 + alignment);

	memset(postings, 0xcc, sizeof(postings));
	tp_decompress_block(compressed, count, previous, postings);
	for (uint32 i = 0; i < count; i++)
	{
		previous += deltas[i];
		assert(postings[i].doc_id == previous);
		assert(postings[i].frequency == frequencies[i]);
		assert(postings[i].fieldnorm == (uint8)(i * 37 + alignment));
		assert(postings[i].reserved == 0);
	}
	for (size_t i = 0; i < sizeof(TpBlockPosting); i++)
		assert(((uint8 *)&postings[count])[i] == 0xcc);
	free(storage);
}

static void
check_invalid(uint8 doc_bits, uint8 freq_bits, uint32 count)
{
	uint8		   compressed[TP_MAX_COMPRESSED_BLOCK_SIZE] = {0};
	TpBlockPosting postings[TP_BLOCK_SIZE];

	compressed[0] = doc_bits;
	compressed[1] = freq_bits;
	expect_error  = true;
	if (setjmp(error_jump) == 0)
	{
		tp_decompress_block(compressed, count, 0, postings);
		fprintf(stderr, "invalid compressed block was accepted\n");
		abort();
	}
	expect_error = false;
}

int
main(void)
{
	uint32 cases = 0;

	for (uint32 alignment = 0; alignment < 8; alignment++)
		for (uint32 count = 1; count <= TP_BLOCK_SIZE; count++)
			for (uint8 doc_bits = 1; doc_bits <= 32; doc_bits++)
				for (uint8 freq_bits = 1; freq_bits <= 16; freq_bits++)
				{
					check_block(doc_bits, freq_bits, count, alignment);
					cases++;
				}

	tp_decompress_block(NULL, 0, 0, NULL);
	check_invalid(1, 1, TP_BLOCK_SIZE + 1);
	check_invalid(0, 1, 1);
	check_invalid(33, 1, 1);
	check_invalid(1, 0, 1);
	check_invalid(1, 17, 1);
	printf("validated %u compression cases\n", cases);
	return 0;
}
