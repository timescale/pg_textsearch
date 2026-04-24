/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * position.c - V6 position index reader implementation
 *
 * Provides functions for reading term positions from the V6
 * segment position index.  Position data is stored as delta-
 * varint encoded lists, one per document per term.
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "segment/format.h"
#include "segment/io.h"
#include "segment/varint.h"

/*
 * Maximum bytes to read at once when scanning position data.
 * Position lists are typically short (< 100 bytes), so this
 * buffer covers most cases in a single read.
 */
#define TP_POS_READ_BUF_SIZE 4096

/*
 * Maximum positions we'll decode per document.
 */
#define TP_MAX_POSITIONS_PER_DOC 4096

/*
 * Read a V6 dictionary entry.
 *
 * For V6 segments, reads the full TpDictEntryV6 including the
 * position_data_offset.  For V5 and earlier, reads the standard
 * TpDictEntry and sets position_data_offset to 0.
 */
void
tp_segment_read_dict_entry_v6(
		TpSegmentReader *reader,
		TpSegmentHeader *header,
		uint32			 index,
		TpDictEntryV6	*entry_v6)
{
	if (header->version >= 6)
	{
		uint64 offset = header->entries_offset +
						(uint64)index * sizeof(TpDictEntryV6);
		tp_segment_read(reader, offset, entry_v6,
						sizeof(TpDictEntryV6));
	}
	else
	{
		/*
		 * Pre-V6: read the version-appropriate entry size,
		 * then promote to V6 with position_data_offset = 0.
		 */
		uint64 offset;

		if (header->version <= TP_SEGMENT_FORMAT_VERSION_3)
		{
			TpDictEntryV3 v3;

			offset = header->entries_offset +
					 (uint64)index * sizeof(TpDictEntryV3);
			tp_segment_read(reader, offset, &v3,
							sizeof(TpDictEntryV3));

			entry_v6->skip_index_offset	   = v3.skip_index_offset;
			entry_v6->block_count		   = v3.block_count;
			entry_v6->doc_freq			   = v3.doc_freq;
		}
		else
		{
			TpDictEntry entry;

			offset = header->entries_offset +
					 (uint64)index * sizeof(TpDictEntry);
			tp_segment_read(reader, offset, &entry,
							sizeof(TpDictEntry));

			entry_v6->skip_index_offset = entry.skip_index_offset;
			entry_v6->block_count		= entry.block_count;
			entry_v6->doc_freq			= entry.doc_freq;
		}

		entry_v6->position_data_offset = 0;
	}
}

/*
 * Load positions for a specific document from the position index.
 *
 * The position data for a term is a stream of per-document position
 * lists in posting-list order:
 *
 *   Doc 0: [count:varint] [pos0:varint] [delta1:varint] ...
 *   Doc 1: [count:varint] [pos0:varint] [delta1:varint] ...
 *   ...
 *
 * We skip doc_ordinal entries to reach the target document,
 * then decode its position list.
 *
 * Returns a palloc'd array of positions (caller must pfree),
 * or NULL if positions are not available.
 */
uint32 *
tp_segment_load_positions(
		TpSegmentReader *reader,
		uint64			 position_data_offset,
		uint32			 doc_ordinal,
		uint32			*count_out)
{
	uint8	buf[TP_POS_READ_BUF_SIZE];
	uint64	offset = 0;
	uint32	count;
	uint32 *positions;
	uint32	bytes_to_read;
	uint32	i;

	*count_out = 0;

	/* No position data available */
	if (position_data_offset == 0)
		return NULL;

	/* Check segment version */
	if (reader->header->version < 6)
		return NULL;

	/*
	 * Skip position lists for documents before doc_ordinal.
	 * Each list starts with a varint count followed by count
	 * varint-encoded deltas.
	 */
	for (i = 0; i < doc_ordinal; i++)
	{
		uint32 skip_count;
		uint32 consumed;
		uint32 j;

		/* Read count varint */
		if (offset >= reader->header->position_data_size)
			return NULL;

		bytes_to_read = Min(TP_VARINT_MAX_BYTES,
							(uint32)(reader->header->position_data_size - offset));
		if (bytes_to_read == 0)
			return NULL;

		tp_segment_read(reader,
						position_data_offset + offset,
						buf, bytes_to_read);
		consumed = tp_decode_varint(buf, &skip_count);
		offset += consumed;

		/* Skip skip_count varint-encoded position deltas */
		for (j = 0; j < skip_count; j++)
		{
			if (offset >= reader->header->position_data_size)
				return NULL;

			bytes_to_read = Min(TP_VARINT_MAX_BYTES,
								(uint32)(reader->header->position_data_size - offset));
			if (bytes_to_read == 0)
				return NULL;

			tp_segment_read(reader,
							position_data_offset + offset,
							buf, bytes_to_read);

			/* Count bytes in this varint */
			consumed = 0;
			while (consumed < bytes_to_read && (buf[consumed] & 0x80))
				consumed++;
			consumed++;	 /* final byte (no continuation bit) */
			offset += consumed;
		}
	}

	/*
	 * Now read the target document's position list.
	 * Read a larger chunk to capture the whole list at once.
	 */
	if (offset >= reader->header->position_data_size)
		return NULL;

	bytes_to_read = Min((uint32)sizeof(buf),
						(uint32)(reader->header->position_data_size - offset));
	if (bytes_to_read == 0)
		return NULL;

	tp_segment_read(reader,
					position_data_offset + offset,
					buf, bytes_to_read);

	/* Allocate output array */
	positions = palloc(TP_MAX_POSITIONS_PER_DOC * sizeof(uint32));

	tp_decode_position_list(buf, positions,
							TP_MAX_POSITIONS_PER_DOC, &count);

	/* Trim to actual size */
	if (count > 0 && count < TP_MAX_POSITIONS_PER_DOC)
	{
		uint32 *trimmed = palloc(count * sizeof(uint32));
		memcpy(trimmed, positions, count * sizeof(uint32));
		pfree(positions);
		positions = trimmed;
	}
	else if (count == 0)
	{
		pfree(positions);
		return NULL;
	}

	*count_out = count;
	return positions;
}
