/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compression.c - Block compression for posting lists
 *
 * Implements delta encoding + bitpacking for posting list compression.
 * This is a scalar implementation; SIMD optimization can be added later.
 */
#include <postgres.h>

#include "compression.h"

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

	while ((1U << bits) <= max_value && bits < 32)
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

/*
 * Unpack a bit stream into an array of values.
 */
static void
bitpack_decode(const uint8 *in, uint32 count, uint8 bits, uint32 *out)
{
	uint64 buffer	= 0; /* Accumulator for bits */
	int	   buf_bits = 0; /* Bits currently in buffer */
	uint32 in_pos	= 0;
	uint32 i;
	uint32 mask = (bits == 32) ? UINT32_MAX : ((1U << bits) - 1);

	for (i = 0; i < count; i++)
	{
		/* Load more bytes if needed */
		while (buf_bits < bits)
		{
			buffer |= ((uint64)in[in_pos++]) << buf_bits;
			buf_bits += 8;
		}

		/* Extract value */
		out[i] = (uint32)(buffer & mask);
		buffer >>= bits;
		buf_bits -= bits;
	}
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

/*
 * Decompress a block of postings.
 *
 * first_doc_id is the base for delta decoding. For the first block of a term,
 * pass 0. For subsequent blocks, pass (previous block's last_doc_id + 1) or
 * simply 0 if storing absolute first doc ID in each block's delta stream.
 *
 * Note: We store deltas from the previous doc within the block, so
 * first_doc_id should be 0 for proper decoding (the first delta IS the first
 * absolute doc ID).
 */
void
tp_decompress_block(
		const uint8	   *compressed,
		uint32			count,
		uint32			first_doc_id,
		TpBlockPosting *out_postings)
{
	const TpCompressedBlockHeader *header;
	uint32						  *doc_deltas;
	uint32						  *frequencies;
	uint32						   doc_id_bytes;
	uint32						   freq_bytes;
	uint32						   pos;
	uint32						   prev_doc;
	uint32						   i;

	Assert(count <= TP_BLOCK_SIZE);

	if (count == 0)
		return;

	header = (const TpCompressedBlockHeader *)compressed;

	/* Validate header values to prevent buffer overruns */
	Assert(header->doc_id_bits >= 1 && header->doc_id_bits <= 32);
	Assert(header->freq_bits >= 1 && header->freq_bits <= 16);

	pos = sizeof(TpCompressedBlockHeader);

	/* Allocate temporary arrays */
	doc_deltas	= palloc(count * sizeof(uint32));
	frequencies = palloc(count * sizeof(uint32));

	/* Calculate sizes for seeking */
	doc_id_bytes = (count * header->doc_id_bits + 7) / 8;
	freq_bytes	 = (count * header->freq_bits + 7) / 8;

	/* Decode doc ID deltas */
	bitpack_decode(compressed + pos, count, header->doc_id_bits, doc_deltas);
	pos += doc_id_bytes;

	/* Decode frequencies */
	bitpack_decode(compressed + pos, count, header->freq_bits, frequencies);
	pos += freq_bytes;

	/* Reconstruct postings with absolute doc IDs */
	prev_doc = first_doc_id;
	for (i = 0; i < count; i++)
	{
		uint32 doc_id = prev_doc + doc_deltas[i];

		out_postings[i].doc_id	  = doc_id;
		out_postings[i].frequency = (uint16)frequencies[i];
		out_postings[i].fieldnorm = compressed[pos + i];
		out_postings[i].reserved  = 0;

		prev_doc = doc_id;
	}

	pfree(doc_deltas);
	pfree(frequencies);
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
