/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compression.c - Block compression for posting lists
 *
 * Implements delta encoding + bitpacking for posting list compression.
 * Decoding fuses bit extraction, delta reconstruction, and posting writes.
 */
#include <postgres.h>

#include <string.h>

#include "segment/compression.h"

#if defined(__x86_64__) && !defined(_MSC_VER) && defined(__has_builtin)
#if __has_attribute(target) && __has_builtin(__builtin_cpu_supports) && \
		__has_builtin(__builtin_cpu_init)
#define TP_HAVE_AVX2
#include <immintrin.h>
#define TP_AVX2 __attribute__((target("avx2")))
#endif
#endif

/*
 * Compute minimum bits needed to represent a value.
 * Returns 1 for 0 (need at least 1 bit), otherwise ceil(log2(value+1)).
 */
uint8
tp_compute_bit_width(uint32 max_value)
{
	uint8 bits = 1;

	if (max_value == 0)
		return 1;

	while (bits < 32 && (1U << bits) <= max_value)
		bits++;

	return bits;
}

/*
 * Pack an array of values into a bit stream.
 * Returns number of bytes written.
 */
static uint32
bitpack_encode(uint32 *values, uint32 count, uint8 bits, uint8 *out)
{
	uint64 buffer	= 0; /* Accumulator for bits */
	int	   buf_bits = 0; /* Bits currently in buffer */
	uint32 out_pos	= 0;
	uint32 i;
	uint32 mask = (bits == 32) ? UINT32_MAX : ((1U << bits) - 1);

	for (i = 0; i < count; i++)
	{
		/* Add value to buffer */
		buffer |= ((uint64)(values[i] & mask)) << buf_bits;
		buf_bits += bits;

		/* Flush complete bytes */
		while (buf_bits >= 8)
		{
			out[out_pos++] = (uint8)(buffer & 0xFF);
			buffer >>= 8;
			buf_bits -= 8;
		}
	}

	/* Flush remaining bits */
	if (buf_bits > 0)
		out[out_pos++] = (uint8)(buffer & 0xFF);

	return out_pos;
}

static inline uint32
bitpack_extract(const uint8 *in, uint32 bit_offset, uint32 mask)
{
	uint64 raw;

	/* The caller's full-size buffer permits reads past a packed section. */
	memcpy(&raw, in + (bit_offset >> 3), sizeof(raw));
	return (uint32)(raw >> (bit_offset & 7)) & mask;
}

/*
 * Compress a block of postings.
 *
 * Steps:
 * 1. Delta-encode doc IDs (first doc ID stored as-is, rest as deltas)
 * 2. Find max delta and max frequency to determine bit widths
 * 3. Bitpack deltas and frequencies
 * 4. Copy fieldnorms as-is
 */
uint32
tp_compress_block(TpBlockPosting *postings, uint32 count, uint8 *out_buf)
{
	TpCompressedBlockHeader *header;
	uint32					*doc_deltas;
	uint32					*frequencies;
	uint32					 max_delta = 0;
	uint32					 max_freq  = 0;
	uint32					 prev_doc  = 0;
	uint32					 out_pos;
	uint32					 i;

	Assert(count <= TP_BLOCK_SIZE);

	if (count == 0)
		return 0;

	/* Allocate temporary arrays for deltas and frequencies */
	doc_deltas	= palloc(count * sizeof(uint32));
	frequencies = palloc(count * sizeof(uint32));

	/* Delta-encode doc IDs and extract frequencies */
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

	/* Write header */
	header				= (TpCompressedBlockHeader *)out_buf;
	header->doc_id_bits = tp_compute_bit_width(max_delta);
	header->freq_bits	= tp_compute_bit_width(max_freq);
	out_pos				= sizeof(TpCompressedBlockHeader);

	/* Bitpack doc ID deltas */
	out_pos += bitpack_encode(
			doc_deltas, count, header->doc_id_bits, out_buf + out_pos);

	/* Bitpack frequencies */
	out_pos += bitpack_encode(
			frequencies, count, header->freq_bits, out_buf + out_pos);

	/* Copy fieldnorms as-is (1 byte each) */
	for (i = 0; i < count; i++)
		out_buf[out_pos++] = postings[i].fieldnorm;

	pfree(doc_deltas);
	pfree(frequencies);

	return out_pos;
}

typedef struct TpPackedBlock
{
	const uint8 *docs;
	const uint8 *freqs;
	const uint8 *fieldnorms;
	uint32		 doc_mask;
	uint32		 freq_mask;
	uint8		 doc_bits;
	uint8		 freq_bits;
} TpPackedBlock;

static inline TpPackedBlock
packed_block(const uint8 *compressed, uint32 count)
{
	const TpCompressedBlockHeader *header = (const TpCompressedBlockHeader *)
			compressed;
	TpPackedBlock block;

	block.doc_bits	 = header->doc_id_bits;
	block.freq_bits	 = header->freq_bits;
	block.docs		 = compressed + sizeof(TpCompressedBlockHeader);
	block.freqs		 = block.docs + (count * block.doc_bits + 7) / 8;
	block.fieldnorms = block.freqs + (count * block.freq_bits + 7) / 8;
	block.doc_mask	 = block.doc_bits == 32 ? UINT32_MAX
											: ((1U << block.doc_bits) - 1);
	block.freq_mask	 = (1U << block.freq_bits) - 1;
	return block;
}

static inline void
decompress_scalar_range(
		const TpPackedBlock *block,
		uint32				 count,
		uint32				 start,
		uint32				 prev_doc,
		TpBlockPosting		*out_postings)
{
	uint32 doc_bit_offset  = start * block->doc_bits;
	uint32 freq_bit_offset = start * block->freq_bits;
	uint32 i;

	for (i = start; i < count; i++)
	{
		uint32 doc_id =
				prev_doc +
				bitpack_extract(block->docs, doc_bit_offset, block->doc_mask);

		out_postings[i].doc_id	  = doc_id;
		out_postings[i].frequency = (uint16)bitpack_extract(
				block->freqs, freq_bit_offset, block->freq_mask);
		out_postings[i].fieldnorm = block->fieldnorms[i];
		out_postings[i].reserved  = 0;
		doc_bit_offset += block->doc_bits;
		freq_bit_offset += block->freq_bits;
		prev_doc = doc_id;
	}
}

static void
decompress_block_scalar(
		const uint8	   *compressed,
		uint32			count,
		uint32			first_doc_id,
		TpBlockPosting *out_postings)
{
	TpPackedBlock block = packed_block(compressed, count);

	decompress_scalar_range(&block, count, 0, first_doc_id, out_postings);
}

#ifdef TP_HAVE_AVX2
typedef struct TpUnpackPlan
{
	__m256i shuffle;
	__m256i shifts;
} TpUnpackPlan;

StaticAssertDecl(
		sizeof(TpBlockPosting) == 8 && offsetof(TpBlockPosting, doc_id) == 0 &&
				offsetof(TpBlockPosting, frequency) == 4 &&
				offsetof(TpBlockPosting, fieldnorm) == 6 &&
				offsetof(TpBlockPosting, reserved) == 7,
		"AVX2 stores require packed eight-byte postings");

static inline TP_AVX2 TpUnpackPlan
unpack_plan(uint8 bits, uint32 phase)
{
	TpUnpackPlan plan;
	__m256i		 positions = _mm256_setr_epi64x(
			 phase, phase + bits, phase + 2 * bits, phase + 3 * bits);
	__m256i bytes = _mm256_srli_epi64(positions, 3);
	__m256i repeated =
			_mm256_mul_epu32(bytes, _mm256_set1_epi64x(UINT64_C(0x01010101)));

	repeated	 = _mm256_or_si256(repeated, _mm256_slli_epi64(repeated, 32));
	plan.shuffle = _mm256_add_epi64(
			repeated, _mm256_set1_epi64x(UINT64_C(0x0706050403020100)));
	plan.shifts = _mm256_and_si256(positions, _mm256_set1_epi64x(7));
	return plan;
}

static inline TP_AVX2 __m128i
unpack_four(
		const uint8		  *in,
		uint32			   bit_offset,
		const TpUnpackPlan plans[2],
		__m128i			   mask)
{
	/* Four-value groups begin at bit offset 0 or 4 within a byte. */
	const TpUnpackPlan *plan = &plans[(bit_offset & 7) >> 2];
	__m128i bytes = _mm_loadu_si128((const __m128i *)(in + (bit_offset >> 3)));
	__m256i values = _mm256_broadcastsi128_si256(bytes);

	/* Wrapped high shuffle bytes are discarded by the shift and mask. */
	values = _mm256_shuffle_epi8(values, plan->shuffle);
	values = _mm256_srlv_epi64(values, plan->shifts);
	values = _mm256_permutevar8x32_epi32(
			values, _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0));
	return _mm_and_si128(_mm256_castsi256_si128(values), mask);
}

static TP_AVX2 void
decompress_block_avx2(
		const uint8	   *compressed,
		uint32			count,
		uint32			first_doc_id,
		TpBlockPosting *out_postings)
{
	TpPackedBlock block			  = packed_block(compressed, count);
	uint32		  doc_bit_offset  = 0;
	uint32		  freq_bit_offset = 0;
	uint32		  prev_doc		  = first_doc_id;
	uint32		  i				  = 0;

	if (count >= 4)
	{
		TpUnpackPlan doc_plans[2] =
				{unpack_plan(block.doc_bits, 0),
				 unpack_plan(block.doc_bits, 4)};
		TpUnpackPlan freq_plans[2] =
				{unpack_plan(block.freq_bits, 0),
				 unpack_plan(block.freq_bits, 4)};
		__m128i doc_vmask  = _mm_set1_epi32(block.doc_mask);
		__m128i freq_vmask = _mm_set1_epi32(block.freq_mask);
		__m128i carry	   = _mm_set1_epi32(prev_doc);

		for (; i + 4 <= count; i += 4)
		{
			__m128i ids = unpack_four(
					block.docs, doc_bit_offset, doc_plans, doc_vmask);
			__m128i frequencies = unpack_four(
					block.freqs, freq_bit_offset, freq_plans, freq_vmask);
			uint32	norm_bytes;
			__m128i metadata;

			ids	  = _mm_add_epi32(ids, _mm_slli_si128(ids, 4));
			ids	  = _mm_add_epi32(ids, _mm_slli_si128(ids, 8));
			ids	  = _mm_add_epi32(ids, carry);
			carry = _mm_shuffle_epi32(ids, _MM_SHUFFLE(3, 3, 3, 3));
			memcpy(&norm_bytes, block.fieldnorms + i, sizeof(norm_bytes));
			metadata = _mm_or_si128(
					frequencies,
					_mm_slli_epi32(
							_mm_cvtepu8_epi32(_mm_cvtsi32_si128(norm_bytes)),
							16));
			_mm_storeu_si128(
					(__m128i *)(out_postings + i),
					_mm_unpacklo_epi32(ids, metadata));
			_mm_storeu_si128(
					(__m128i *)(out_postings + i + 2),
					_mm_unpackhi_epi32(ids, metadata));
			doc_bit_offset += 4 * block.doc_bits;
			freq_bit_offset += 4 * block.freq_bits;
		}
		prev_doc = (uint32)_mm_cvtsi128_si32(carry);
	}
	decompress_scalar_range(&block, count, i, prev_doc, out_postings);
}
#endif

static void (*decompress_impl)(
		const uint8 *, uint32, uint32, TpBlockPosting *) = NULL;

/* Validate once, then unpack directly into the final postings. */
void
tp_decompress_block(
		const uint8	   *compressed,
		uint32			count,
		uint32			first_doc_id,
		TpBlockPosting *out_postings)
{
	const TpCompressedBlockHeader *header;

	if (count > TP_BLOCK_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted segment: block count %u exceeds "
						"maximum %u",
						count,
						(uint32)TP_BLOCK_SIZE)));

	if (count == 0)
		return;

	header = (const TpCompressedBlockHeader *)compressed;

	/* Validate header values to prevent buffer overruns */
	if (header->doc_id_bits < 1 || header->doc_id_bits > 32)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted segment: invalid doc_id bit "
						"width %u",
						header->doc_id_bits)));

	if (header->freq_bits < 1 || header->freq_bits > 16)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted segment: invalid frequency bit "
						"width %u",
						header->freq_bits)));

	if (unlikely(decompress_impl == NULL))
	{
		decompress_impl = decompress_block_scalar;
#ifdef TP_HAVE_AVX2
		/* The compiler's probe also checks OS support for saving YMM state. */
		__builtin_cpu_init();
		if (__builtin_cpu_supports("avx2"))
			decompress_impl = decompress_block_avx2;
#endif
	}
	decompress_impl(compressed, count, first_doc_id, out_postings);
}

/*
 * Get the size of compressed data.
 */
uint32
tp_compressed_block_size(const uint8 *compressed, uint32 count)
{
	const TpCompressedBlockHeader *header;
	uint32						   doc_id_bytes;
	uint32						   freq_bytes;

	if (count == 0)
		return 0;

	header		 = (const TpCompressedBlockHeader *)compressed;
	doc_id_bytes = (count * header->doc_id_bits + 7) / 8;
	freq_bytes	 = (count * header->freq_bits + 7) / 8;

	return sizeof(TpCompressedBlockHeader) + doc_id_bytes + freq_bytes + count;
}
