/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * layout_check.c - Compile-time assertions for layout-sensitive structs
 *
 * The dictionary, skip index, CTID map and block posting structs are
 * stored as raw bytes in segments, so their layout is part of the
 * on-disk format.  TpExpullEntry (memtable arena) and TpSegmentPosting
 * (postings handed to the scorer) are never written to disk, but the
 * code depends on their packed stride all the same.
 *
 * The reference layout is the one GCC produces on x86-64: a packed
 * struct is the plain sum of its members, and the two aligned() structs
 * keep their natural layout because the requested alignment already is
 * the natural one.  The supported 64-bit ABIs agree on these values.
 *
 * A compiler that laid the persisted structs out differently would
 * read and write segment data incorrectly; for the in-memory structs
 * it would silently lose the compact representation the code is sized
 * around.  Pinning sizeof and offsetof here turns either into a build
 * failure.  The translation unit emits no code.
 */
#include <postgres.h>

#include "memtable/expull.h"
#include "segment/format.h"
#include "segment/segment.h"

/*
 * Alignment probes: offsetof(probe, entry) equals the alignment the
 * compiler gives the struct, without needing C11 _Alignof.
 */
struct TpDictEntryV3AlignProbe
{
	char		  c;
	TpDictEntryV3 entry;
};

struct TpDictEntryAlignProbe
{
	char		c;
	TpDictEntry entry;
};

/* Posting entry accumulated in memtable arena blocks: packed, 7 bytes */
StaticAssertDecl(sizeof(TpExpullEntry) == 7, "TpExpullEntry size");
StaticAssertDecl(offsetof(TpExpullEntry, doc_id) == 0, "TpExpullEntry.doc_id");
StaticAssertDecl(
		offsetof(TpExpullEntry, frequency) == 4, "TpExpullEntry.frequency");
StaticAssertDecl(
		offsetof(TpExpullEntry, fieldnorm) == 6, "TpExpullEntry.fieldnorm");

/* V3 dictionary entry: aligned(4) is the natural layout, 12 bytes */
StaticAssertDecl(sizeof(TpDictEntryV3) == 12, "TpDictEntryV3 size");
StaticAssertDecl(
		offsetof(struct TpDictEntryV3AlignProbe, entry) == 4,
		"TpDictEntryV3 alignment");
StaticAssertDecl(
		offsetof(TpDictEntryV3, skip_index_offset) == 0,
		"TpDictEntryV3.skip_index_offset");
StaticAssertDecl(
		offsetof(TpDictEntryV3, block_count) == 4,
		"TpDictEntryV3.block_count");
StaticAssertDecl(
		offsetof(TpDictEntryV3, reserved) == 6, "TpDictEntryV3.reserved");
StaticAssertDecl(
		offsetof(TpDictEntryV3, doc_freq) == 8, "TpDictEntryV3.doc_freq");

/* Dictionary entry: aligned(8) is the natural layout, 16 bytes */
StaticAssertDecl(sizeof(TpDictEntry) == 16, "TpDictEntry size");
StaticAssertDecl(
		offsetof(struct TpDictEntryAlignProbe, entry) == 8,
		"TpDictEntry alignment");
StaticAssertDecl(
		offsetof(TpDictEntry, skip_index_offset) == 0,
		"TpDictEntry.skip_index_offset");
StaticAssertDecl(
		offsetof(TpDictEntry, block_count) == 8, "TpDictEntry.block_count");
StaticAssertDecl(
		offsetof(TpDictEntry, doc_freq) == 12, "TpDictEntry.doc_freq");

/* V3 skip index entry: packed, 16 bytes */
StaticAssertDecl(sizeof(TpSkipEntryV3) == 16, "TpSkipEntryV3 size");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, last_doc_id) == 0,
		"TpSkipEntryV3.last_doc_id");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, doc_count) == 4, "TpSkipEntryV3.doc_count");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, block_max_tf) == 5,
		"TpSkipEntryV3.block_max_tf");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, block_max_norm) == 7,
		"TpSkipEntryV3.block_max_norm");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, posting_offset) == 8,
		"TpSkipEntryV3.posting_offset");
StaticAssertDecl(offsetof(TpSkipEntryV3, flags) == 12, "TpSkipEntryV3.flags");
StaticAssertDecl(
		offsetof(TpSkipEntryV3, reserved) == 13, "TpSkipEntryV3.reserved");

/* Skip index entry: packed, 20 bytes */
StaticAssertDecl(sizeof(TpSkipEntry) == 20, "TpSkipEntry size");
StaticAssertDecl(
		offsetof(TpSkipEntry, last_doc_id) == 0, "TpSkipEntry.last_doc_id");
StaticAssertDecl(
		offsetof(TpSkipEntry, doc_count) == 4, "TpSkipEntry.doc_count");
StaticAssertDecl(
		offsetof(TpSkipEntry, block_max_tf) == 5, "TpSkipEntry.block_max_tf");
StaticAssertDecl(
		offsetof(TpSkipEntry, block_max_norm) == 7,
		"TpSkipEntry.block_max_norm");
StaticAssertDecl(
		offsetof(TpSkipEntry, posting_offset) == 8,
		"TpSkipEntry.posting_offset");
StaticAssertDecl(offsetof(TpSkipEntry, flags) == 16, "TpSkipEntry.flags");
StaticAssertDecl(
		offsetof(TpSkipEntry, reserved) == 17, "TpSkipEntry.reserved");

/* Doc ID to CTID map entry: packed, 6 bytes */
StaticAssertDecl(sizeof(TpCtidMapEntry) == 6, "TpCtidMapEntry size");
StaticAssertDecl(offsetof(TpCtidMapEntry, ctid) == 0, "TpCtidMapEntry.ctid");

/* Posting handed to the scorer: packed, 14 bytes */
StaticAssertDecl(sizeof(TpSegmentPosting) == 14, "TpSegmentPosting size");
StaticAssertDecl(
		offsetof(TpSegmentPosting, ctid) == 0, "TpSegmentPosting.ctid");
StaticAssertDecl(
		offsetof(TpSegmentPosting, doc_id) == 6, "TpSegmentPosting.doc_id");
StaticAssertDecl(
		offsetof(TpSegmentPosting, frequency) == 10,
		"TpSegmentPosting.frequency");
StaticAssertDecl(
		offsetof(TpSegmentPosting, doc_length) == 12,
		"TpSegmentPosting.doc_length");

/*
 * Unpacked types the format embeds.  They need no attribute to reach
 * the reference layout, but the layout is just as load-bearing.
 */
StaticAssertDecl(sizeof(ItemPointerData) == 6, "ItemPointerData size");
StaticAssertDecl(
		offsetof(ItemPointerData, ip_posid) == 4, "ItemPointerData.ip_posid");
StaticAssertDecl(sizeof(TpBlockPosting) == 8, "TpBlockPosting size");
StaticAssertDecl(
		offsetof(TpBlockPosting, doc_id) == 0, "TpBlockPosting.doc_id");
StaticAssertDecl(
		offsetof(TpBlockPosting, frequency) == 4, "TpBlockPosting.frequency");
StaticAssertDecl(
		offsetof(TpBlockPosting, fieldnorm) == 6, "TpBlockPosting.fieldnorm");
StaticAssertDecl(
		offsetof(TpBlockPosting, reserved) == 7, "TpBlockPosting.reserved");
