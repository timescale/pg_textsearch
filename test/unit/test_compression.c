/*
 * test_compression.c - Unit tests for block compression
 *
 * Tests delta encoding + bitpacking compression for posting lists.
 */
#include "pg_stubs.h"

/* Block size constant from segment.h */
#define TP_BLOCK_SIZE 128

/* Posting structure from segment.h */
typedef struct TpBlockPosting
{
	uint32 doc_id;
	uint16 frequency;
	uint8  fieldnorm;
	uint8  reserved;
} TpBlockPosting;

/* Compressed block header from compression.h */
typedef struct TpCompressedBlockHeader
{
	uint8 doc_id_bits;
	uint8 freq_bits;
} TpCompressedBlockHeader;

#define TP_MAX_COMPRESSED_BLOCK_SIZE 898

/*
 * Copy compression functions here to avoid postgres.h dependency.
 * These must stay in sync with src/segment/compression.c.
 */

static uint8
tp_compute_bit_width(uint32 max_value)
{
	uint8 bits = 1;

	if (max_value == 0)
		return 1;

	while ((1U << bits) <= max_value && bits < 32)
		bits++;

	return bits;
}

static uint32
bitpack_encode(uint32 *values, uint32 count, uint8 bits, uint8 *out)
{
	uint64 buffer   = 0;
	int    buf_bits = 0;
	uint32 out_pos  = 0;
	uint32 i;
	uint32 mask = (bits == 32) ? UINT32_MAX : ((1U << bits) - 1);

	for (i = 0; i < count; i++)
	{
		buffer |= ((uint64)(values[i] & mask)) << buf_bits;
		buf_bits += bits;

		while (buf_bits >= 8)
		{
			out[out_pos++] = (uint8)(buffer & 0xFF);
			buffer >>= 8;
			buf_bits -= 8;
		}
	}

	if (buf_bits > 0)
		out[out_pos++] = (uint8)(buffer & 0xFF);

	return out_pos;
}

static void
bitpack_decode(const uint8 *in, uint32 count, uint8 bits, uint32 *out)
{
	uint64 buffer   = 0;
	int    buf_bits = 0;
	uint32 in_pos   = 0;
	uint32 i;
	uint32 mask = (bits == 32) ? UINT32_MAX : ((1U << bits) - 1);

	for (i = 0; i < count; i++)
	{
		while (buf_bits < (int)bits)
		{
			buffer |= ((uint64)in[in_pos++]) << buf_bits;
			buf_bits += 8;
		}

		out[i] = (uint32)(buffer & mask);
		buffer >>= bits;
		buf_bits -= bits;
	}
}

static uint32
tp_compress_block(TpBlockPosting *postings, uint32 count, uint8 *out_buf)
{
	TpCompressedBlockHeader *header;
	uint32                  *doc_deltas;
	uint32                  *frequencies;
	uint32                   max_delta = 0;
	uint32                   max_freq  = 0;
	uint32                   prev_doc  = 0;
	uint32                   out_pos;
	uint32                   i;

	Assert(count <= TP_BLOCK_SIZE);

	if (count == 0)
		return 0;

	doc_deltas  = palloc(count * sizeof(uint32));
	frequencies = palloc(count * sizeof(uint32));

	for (i = 0; i < count; i++)
	{
		uint32 doc_id = postings[i].doc_id;
		uint32 delta  = doc_id - prev_doc;

		doc_deltas[i]  = delta;
		frequencies[i] = postings[i].frequency;

		if (delta > max_delta)
			max_delta = delta;
		if (frequencies[i] > max_freq)
			max_freq = frequencies[i];

		prev_doc = doc_id;
	}

	header              = (TpCompressedBlockHeader *)out_buf;
	header->doc_id_bits = tp_compute_bit_width(max_delta);
	header->freq_bits   = tp_compute_bit_width(max_freq);
	out_pos             = sizeof(TpCompressedBlockHeader);

	out_pos +=
			bitpack_encode(doc_deltas, count, header->doc_id_bits,
						   out_buf + out_pos);
	out_pos +=
			bitpack_encode(frequencies, count, header->freq_bits,
						   out_buf + out_pos);

	for (i = 0; i < count; i++)
		out_buf[out_pos++] = postings[i].fieldnorm;

	pfree(doc_deltas);
	pfree(frequencies);

	return out_pos;
}

static void
tp_decompress_block(const uint8    *compressed,
					uint32          count,
					uint32          first_doc_id,
					TpBlockPosting *out_postings)
{
	const TpCompressedBlockHeader *header;
	uint32                        *doc_deltas;
	uint32                        *frequencies;
	uint32                         doc_id_bytes;
	uint32                         freq_bytes;
	uint32                         pos;
	uint32                         prev_doc;
	uint32                         i;

	Assert(count <= TP_BLOCK_SIZE);

	if (count == 0)
		return;

	header = (const TpCompressedBlockHeader *)compressed;

	Assert(header->doc_id_bits >= 1 && header->doc_id_bits <= 32);
	Assert(header->freq_bits >= 1 && header->freq_bits <= 16);

	pos = sizeof(TpCompressedBlockHeader);

	doc_deltas  = palloc(count * sizeof(uint32));
	frequencies = palloc(count * sizeof(uint32));

	doc_id_bytes = (count * header->doc_id_bits + 7) / 8;
	freq_bytes   = (count * header->freq_bits + 7) / 8;

	bitpack_decode(compressed + pos, count, header->doc_id_bits, doc_deltas);
	pos += doc_id_bytes;

	bitpack_decode(compressed + pos, count, header->freq_bits, frequencies);
	pos += freq_bytes;

	prev_doc = first_doc_id;
	for (i = 0; i < count; i++)
	{
		uint32 doc_id = prev_doc + doc_deltas[i];

		out_postings[i].doc_id    = doc_id;
		out_postings[i].frequency = (uint16)frequencies[i];
		out_postings[i].fieldnorm = compressed[pos + i];
		out_postings[i].reserved  = 0;

		prev_doc = doc_id;
	}

	pfree(doc_deltas);
	pfree(frequencies);
}

static uint32
tp_compressed_block_size(const uint8 *compressed, uint32 count)
{
	const TpCompressedBlockHeader *header;
	uint32                         doc_id_bytes;
	uint32                         freq_bytes;

	if (count == 0)
		return 0;

	header       = (const TpCompressedBlockHeader *)compressed;
	doc_id_bytes = (count * header->doc_id_bits + 7) / 8;
	freq_bytes   = (count * header->freq_bits + 7) / 8;

	return sizeof(TpCompressedBlockHeader) + doc_id_bytes + freq_bytes + count;
}

/* ==================== Tests ==================== */

/* Test bit width computation for various values */
static int
test_bit_width(void)
{
	/* 0 needs 1 bit (by convention) */
	TEST_ASSERT_EQ(tp_compute_bit_width(0), 1, "bit_width(0)");

	/* 1 needs 1 bit */
	TEST_ASSERT_EQ(tp_compute_bit_width(1), 1, "bit_width(1)");

	/* 2-3 need 2 bits */
	TEST_ASSERT_EQ(tp_compute_bit_width(2), 2, "bit_width(2)");
	TEST_ASSERT_EQ(tp_compute_bit_width(3), 2, "bit_width(3)");

	/* 4-7 need 3 bits */
	TEST_ASSERT_EQ(tp_compute_bit_width(4), 3, "bit_width(4)");
	TEST_ASSERT_EQ(tp_compute_bit_width(7), 3, "bit_width(7)");

	/* 8-15 need 4 bits */
	TEST_ASSERT_EQ(tp_compute_bit_width(8), 4, "bit_width(8)");
	TEST_ASSERT_EQ(tp_compute_bit_width(15), 4, "bit_width(15)");

	/* Powers of 2 boundaries */
	TEST_ASSERT_EQ(tp_compute_bit_width(255), 8, "bit_width(255)");
	TEST_ASSERT_EQ(tp_compute_bit_width(256), 9, "bit_width(256)");
	TEST_ASSERT_EQ(tp_compute_bit_width(65535), 16, "bit_width(65535)");
	TEST_ASSERT_EQ(tp_compute_bit_width(65536), 17, "bit_width(65536)");

	/* Large values */
	TEST_ASSERT_EQ(tp_compute_bit_width((1U << 31) - 1), 31, "bit_width(2^31-1)");
	TEST_ASSERT_EQ(tp_compute_bit_width(1U << 31), 32, "bit_width(2^31)");
	TEST_ASSERT_EQ(tp_compute_bit_width(UINT32_MAX), 32, "bit_width(UINT32_MAX)");

	return 0;
}

/* Test bitpack roundtrip for various bit widths */
static int
test_bitpack_roundtrip(void)
{
	uint32 values[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	uint32 decoded[16];
	uint8  buffer[64];

	for (uint8 bits = 1; bits <= 16; bits++)
	{
		/* Mask values to fit in bit width */
		uint32 mask = (1U << bits) - 1;
		for (int i = 0; i < 16; i++)
			values[i] = i & mask;

		uint32 bytes = bitpack_encode(values, 16, bits, buffer);
		bitpack_decode(buffer, 16, bits, decoded);

		for (int i = 0; i < 16; i++)
		{
			TEST_ASSERT_EQ(decoded[i], values[i], "bitpack roundtrip");
		}

		/* Verify byte count matches expectation */
		uint32 expected_bytes = (16 * bits + 7) / 8;
		TEST_ASSERT_EQ(bytes, expected_bytes, "bitpack byte count");
	}

	return 0;
}

/* Test bitpack with 32-bit values */
static int
test_bitpack_32bit(void)
{
	uint32 values[4]  = {0xDEADBEEF, 0x12345678, 0xFFFFFFFF, 0x00000000};
	uint32 decoded[4] = {0};
	uint8  buffer[32];

	uint32 bytes = bitpack_encode(values, 4, 32, buffer);
	TEST_ASSERT_EQ(bytes, 16, "32-bit encode size");

	bitpack_decode(buffer, 4, 32, decoded);

	for (int i = 0; i < 4; i++)
	{
		TEST_ASSERT_EQ(decoded[i], values[i], "32-bit roundtrip");
	}

	return 0;
}

/* Test compress/decompress roundtrip with simple data */
static int
test_compress_roundtrip_simple(void)
{
	TpBlockPosting input[4];
	TpBlockPosting output[4];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	/* Create simple sequential postings */
	for (int i = 0; i < 4; i++)
	{
		input[i].doc_id    = i * 10; /* 0, 10, 20, 30 */
		input[i].frequency = i + 1;  /* 1, 2, 3, 4 */
		input[i].fieldnorm = 50 + i; /* 50, 51, 52, 53 */
		input[i].reserved  = 0;
	}

	uint32 size = tp_compress_block(input, 4, compressed);
	TEST_ASSERT(size > 0, "compress produced output");
	TEST_ASSERT(size < sizeof(input), "compression reduced size");

	tp_decompress_block(compressed, 4, 0, output);

	for (int i = 0; i < 4; i++)
	{
		TEST_ASSERT_EQ(output[i].doc_id, input[i].doc_id, "doc_id roundtrip");
		TEST_ASSERT_EQ(output[i].frequency, input[i].frequency,
					   "frequency roundtrip");
		TEST_ASSERT_EQ(output[i].fieldnorm, input[i].fieldnorm,
					   "fieldnorm roundtrip");
	}

	return 0;
}

/* Test compress/decompress with full block */
static int
test_compress_roundtrip_full_block(void)
{
	TpBlockPosting input[TP_BLOCK_SIZE];
	TpBlockPosting output[TP_BLOCK_SIZE];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	/* Create realistic posting data */
	for (int i = 0; i < TP_BLOCK_SIZE; i++)
	{
		input[i].doc_id    = i * 100 + (i % 17); /* Variable gaps */
		input[i].frequency = (i % 10) + 1;       /* 1-10 */
		input[i].fieldnorm = (uint8)(i % 256);
		input[i].reserved  = 0;
	}

	uint32 size = tp_compress_block(input, TP_BLOCK_SIZE, compressed);
	TEST_ASSERT(size > 0, "full block compress produced output");
	TEST_ASSERT(size <= TP_MAX_COMPRESSED_BLOCK_SIZE, "size within bounds");

	tp_decompress_block(compressed, TP_BLOCK_SIZE, 0, output);

	for (int i = 0; i < TP_BLOCK_SIZE; i++)
	{
		TEST_ASSERT_EQ(output[i].doc_id, input[i].doc_id, "full block doc_id");
		TEST_ASSERT_EQ(output[i].frequency, input[i].frequency,
					   "full block frequency");
		TEST_ASSERT_EQ(output[i].fieldnorm, input[i].fieldnorm,
					   "full block fieldnorm");
	}

	return 0;
}

/* Test with worst-case compression (maximum bit widths) */
static int
test_compress_worst_case(void)
{
	TpBlockPosting input[TP_BLOCK_SIZE];
	TpBlockPosting output[TP_BLOCK_SIZE];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	/* Create worst-case data: large deltas and max frequencies */
	uint32 doc_id = 0;
	for (int i = 0; i < TP_BLOCK_SIZE; i++)
	{
		/* Large variable gaps to force many bits */
		doc_id += (1U << 20) + i;
		input[i].doc_id    = doc_id;
		input[i].frequency = 65535; /* Max uint16 */
		input[i].fieldnorm = 255;
		input[i].reserved  = 0;
	}

	uint32 size = tp_compress_block(input, TP_BLOCK_SIZE, compressed);
	TEST_ASSERT(size > 0, "worst case compress produced output");
	TEST_ASSERT(size <= TP_MAX_COMPRESSED_BLOCK_SIZE,
				"worst case within buffer bounds");

	tp_decompress_block(compressed, TP_BLOCK_SIZE, 0, output);

	for (int i = 0; i < TP_BLOCK_SIZE; i++)
	{
		TEST_ASSERT_EQ(output[i].doc_id, input[i].doc_id,
					   "worst case doc_id roundtrip");
		TEST_ASSERT_EQ(output[i].frequency, input[i].frequency,
					   "worst case freq roundtrip");
	}

	return 0;
}

/* Test compressed_block_size calculation */
static int
test_compressed_block_size(void)
{
	TpBlockPosting input[16];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	for (int i = 0; i < 16; i++)
	{
		input[i].doc_id    = i;
		input[i].frequency = 1;
		input[i].fieldnorm = 100;
		input[i].reserved  = 0;
	}

	uint32 actual_size     = tp_compress_block(input, 16, compressed);
	uint32 calculated_size = tp_compressed_block_size(compressed, 16);

	TEST_ASSERT_EQ(actual_size, calculated_size, "size calculation matches");

	return 0;
}

/* Test empty block handling */
static int
test_empty_block(void)
{
	TpBlockPosting input[1];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	uint32 size = tp_compress_block(input, 0, compressed);
	TEST_ASSERT_EQ(size, 0, "empty block returns 0");

	uint32 calc_size = tp_compressed_block_size(compressed, 0);
	TEST_ASSERT_EQ(calc_size, 0, "empty block size is 0");

	return 0;
}

/* Test single posting block */
static int
test_single_posting(void)
{
	TpBlockPosting input[1];
	TpBlockPosting output[1];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	input[0].doc_id    = 12345;
	input[0].frequency = 42;
	input[0].fieldnorm = 200;
	input[0].reserved  = 0;

	uint32 size = tp_compress_block(input, 1, compressed);
	TEST_ASSERT(size > 0, "single posting compressed");

	tp_decompress_block(compressed, 1, 0, output);

	TEST_ASSERT_EQ(output[0].doc_id, 12345, "single posting doc_id");
	TEST_ASSERT_EQ(output[0].frequency, 42, "single posting frequency");
	TEST_ASSERT_EQ(output[0].fieldnorm, 200, "single posting fieldnorm");

	return 0;
}

/*
 * Test that first_doc_id parameter must be 0.
 *
 * This documents a subtle API contract: compression always encodes deltas
 * from 0, so decompression must also use first_doc_id=0. The parameter
 * exists but only one value is valid.
 */
static int
test_first_doc_id_must_be_zero(void)
{
	TpBlockPosting input[2];
	TpBlockPosting output[2];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	input[0].doc_id    = 1000;
	input[0].frequency = 1;
	input[0].fieldnorm = 50;
	input[0].reserved  = 0;

	input[1].doc_id    = 1010;
	input[1].frequency = 2;
	input[1].fieldnorm = 60;
	input[1].reserved  = 0;

	tp_compress_block(input, 2, compressed);

	/* Correct usage: first_doc_id = 0 */
	tp_decompress_block(compressed, 2, 0, output);
	TEST_ASSERT_EQ(output[0].doc_id, 1000, "first_doc_id=0 works");
	TEST_ASSERT_EQ(output[1].doc_id, 1010, "first_doc_id=0 works");

	/* Incorrect usage: first_doc_id != 0 produces wrong results */
	tp_decompress_block(compressed, 2, 500, output);
	/* This produces 1500 and 1510 instead of 1000 and 1010! */
	TEST_ASSERT(output[0].doc_id != 1000,
				"first_doc_id!=0 gives wrong result (by design)");

	return 0;
}

/*
 * Test that unsorted doc_ids don't crash (but waste bits).
 *
 * If doc_ids go backwards, the delta underflows to a huge value,
 * forcing 32-bit encoding. Decompression still works due to
 * unsigned wraparound, but this is wasteful and likely a caller bug.
 */
static int
test_unsorted_doc_ids_dont_crash(void)
{
	TpBlockPosting input[3];
	TpBlockPosting output[3];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	/* Intentionally unsorted - 100, 50, 200 */
	input[0].doc_id    = 100;
	input[0].frequency = 1;
	input[0].fieldnorm = 50;
	input[0].reserved  = 0;

	input[1].doc_id    = 50; /* SMALLER than previous! */
	input[1].frequency = 1;
	input[1].fieldnorm = 50;
	input[1].reserved  = 0;

	input[2].doc_id    = 200;
	input[2].frequency = 1;
	input[2].fieldnorm = 50;
	input[2].reserved  = 0;

	/* Should not crash */
	uint32 size = tp_compress_block(input, 3, compressed);
	TEST_ASSERT(size > 0, "unsorted compresses without crash");

	/* Check header - should require many bits due to underflow */
	TpCompressedBlockHeader *header = (TpCompressedBlockHeader *)compressed;
	TEST_ASSERT(header->doc_id_bits > 20,
				"unsorted forces large bit width due to underflow");

	/* Roundtrip still works due to unsigned wraparound */
	tp_decompress_block(compressed, 3, 0, output);
	TEST_ASSERT_EQ(output[0].doc_id, 100, "unsorted roundtrip works (0)");
	TEST_ASSERT_EQ(output[1].doc_id, 50, "unsorted roundtrip works (1)");
	TEST_ASSERT_EQ(output[2].doc_id, 200, "unsorted roundtrip works (2)");

	return 0;
}

/* Test compression ratio is reasonable */
static int
test_compression_ratio(void)
{
	TpBlockPosting input[TP_BLOCK_SIZE];
	uint8          compressed[TP_MAX_COMPRESSED_BLOCK_SIZE];

	/* Create typical posting data with small gaps and low frequencies */
	for (int i = 0; i < TP_BLOCK_SIZE; i++)
	{
		input[i].doc_id    = i + 1; /* Gaps of 1 */
		input[i].frequency = 1;     /* Min frequency */
		input[i].fieldnorm = 100;
		input[i].reserved  = 0;
	}

	uint32 uncompressed_size = TP_BLOCK_SIZE * sizeof(TpBlockPosting);
	uint32 compressed_size   = tp_compress_block(input, TP_BLOCK_SIZE, compressed);

	/* With gaps of 1 and freq of 1, should compress very well */
	/* Expected: 2 (header) + 16 (128 * 1 bit / 8) + 16 (128 * 1 bit / 8) + 128 */
	/* = 162 bytes vs 1024 bytes uncompressed = ~6x compression */
	TEST_ASSERT(compressed_size < uncompressed_size / 4,
				"good compression ratio for small values");

	return 0;
}

int
main(void)
{
	int passed = 0, failed = 0, total = 0;

	printf("Running compression tests...\n");

	RUN_TEST(test_bit_width);
	RUN_TEST(test_bitpack_roundtrip);
	RUN_TEST(test_bitpack_32bit);
	RUN_TEST(test_compress_roundtrip_simple);
	RUN_TEST(test_compress_roundtrip_full_block);
	RUN_TEST(test_compress_worst_case);
	RUN_TEST(test_compressed_block_size);
	RUN_TEST(test_empty_block);
	RUN_TEST(test_single_posting);
	RUN_TEST(test_first_doc_id_must_be_zero);
	RUN_TEST(test_unsorted_doc_ids_dont_crash);
	RUN_TEST(test_compression_ratio);

	printf("\nCompression: %d/%d tests passed", passed, total);
	if (failed > 0)
		printf(" (%d FAILED)", failed);
	printf("\n");

	return failed > 0 ? 1 : 0;
}
