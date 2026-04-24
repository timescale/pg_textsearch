/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * varint.h - Variable-length integer encoding for position data
 *
 * Encodes unsigned 32-bit integers using 1-5 bytes. Each byte
 * stores 7 bits of data; the high bit indicates continuation.
 * Used for delta-encoded term positions in the V6 position index.
 *
 * Encoding format (little-endian, LSB first):
 *   0xxxxxxx                              (0-127:       1 byte)
 *   1xxxxxxx 0xxxxxxx                     (128-16383:   2 bytes)
 *   1xxxxxxx 1xxxxxxx 0xxxxxxx            (up to 2M:    3 bytes)
 *   1xxxxxxx 1xxxxxxx 1xxxxxxx 0xxxxxxx   (up to 268M:  4 bytes)
 *   1xxxxxxx 1xxxxxxx 1xxxxxxx 1xxxxxxx 0000xxxx (up to 4G: 5 bytes)
 */
#pragma once

#include <postgres.h>

/*
 * Maximum bytes needed to encode a uint32 as varint.
 */
#define TP_VARINT_MAX_BYTES 5

/*
 * Encode a uint32 as a variable-length integer.
 *
 * Writes 1-5 bytes to buf. Returns the number of bytes written.
 * Caller must ensure buf has at least TP_VARINT_MAX_BYTES space.
 */
static inline uint32
tp_encode_varint(uint32 value, uint8 *buf)
{
	uint32 n = 0;

	while (value >= 0x80)
	{
		buf[n++] = (uint8)(value | 0x80);
		value >>= 7;
	}
	buf[n++] = (uint8)value;
	return n;
}

/*
 * Decode a varint from buffer into *value.
 *
 * Returns the number of bytes consumed.  Caller must ensure
 * buf contains at least TP_VARINT_MAX_BYTES readable bytes
 * (or the actual encoded length, if known).
 */
static inline uint32
tp_decode_varint(const uint8 *buf, uint32 *value)
{
	uint32 result = 0;
	uint32 shift  = 0;
	uint32 n      = 0;
	uint8  byte;

	do
	{
		/*
		 * A uint32 varint is at most 5 bytes.  If we've consumed
		 * 5 bytes and the continuation bit is still set, the data
		 * is corrupt.
		 */
		if (n >= TP_VARINT_MAX_BYTES)
		{
			*value = 0;
			return n;
		}
		byte = buf[n++];
		result |= (uint32)(byte & 0x7F) << shift;
		shift += 7;
	} while (byte & 0x80);

	*value = result;
	return n;
}

/*
 * Compute the encoded size of a varint without writing it.
 */
static inline uint32
tp_varint_size(uint32 value)
{
	if (value < (1U << 7))
		return 1;
	if (value < (1U << 14))
		return 2;
	if (value < (1U << 21))
		return 3;
	if (value < (1U << 28))
		return 4;
	return 5;
}

/*
 * Encode a position list as delta-encoded varints.
 *
 * Input:  sorted array of positions [p0, p1, ..., pN-1]
 * Output: [count:varint] [p0:varint] [p1-p0:varint] ... [pN-pN-1:varint]
 *
 * Returns the number of bytes written to buf.  Caller must
 * ensure buf has enough space (worst case: (count+1) * 5 bytes).
 */
static inline uint32
tp_encode_position_list(
		const uint32 *positions,
		uint32 count,
		uint8 *buf)
{
	uint32 written = 0;
	uint32 prev    = 0;
	uint32 i;

	/* Write count */
	written += tp_encode_varint(count, buf + written);

	/* Write delta-encoded positions */
	for (i = 0; i < count; i++)
	{
		uint32 delta;

		Assert(i == 0 || positions[i] > prev);
		delta = positions[i] - prev;
		written += tp_encode_varint(delta, buf + written);
		prev = positions[i];
	}

	return written;
}

/*
 * Decode a position list from delta-encoded varints.
 *
 * Input:  buffer starting at a position list
 * Output: positions array (caller-allocated), count set
 *
 * Returns the number of bytes consumed from buf.
 */
static inline uint32
tp_decode_position_list(
		const uint8 *buf,
		uint32 *positions,
		uint32 max_positions,
		uint32 *count_out)
{
	uint32 consumed = 0;
	uint32 count;
	uint32 prev = 0;
	uint32 i;

	/* Read count */
	consumed += tp_decode_varint(buf + consumed, &count);

	if (count > max_positions)
		count = max_positions;  /* safety clamp */

	*count_out = count;

	/* Read delta-encoded positions */
	for (i = 0; i < count; i++)
	{
		uint32 delta;

		consumed += tp_decode_varint(buf + consumed, &delta);
		prev += delta;
		positions[i] = prev;
	}

	return consumed;
}

/*
 * Skip a position list without decoding positions.
 *
 * Reads the count, then skips count varint-encoded deltas.
 * Returns the number of bytes consumed.
 */
static inline uint32
tp_skip_position_list(const uint8 *buf)
{
	uint32 consumed = 0;
	uint32 count;
	uint32 i;

	consumed += tp_decode_varint(buf + consumed, &count);

	for (i = 0; i < count; i++)
	{
		/* Skip each varint */
		while (buf[consumed] & 0x80)
			consumed++;
		consumed++;  /* final byte */
	}

	return consumed;
}
