/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * vector.h - bm25vector data type interface
 *
 * Wire / on-disk format (v2 as of pg_textsearch 1.2.0).
 *
 * Each value carries a 4-byte ASCII magic ("BM25") immediately
 * after the varlena header, then a 1-byte version + 3 reserved
 * bytes, then the existing index_name + entries payload. The magic
 * is unambiguously not a valid v1 `index_name_len` value, so
 * readers can distinguish v2 from the legacy v1 format.
 *
 * v1 (legacy) layout, accepted by readers, never written:
 *   int32 vl_len_, int32 index_name_len, int32 entry_count, data...
 *   per-entry: int32 frequency, int32 lexeme_len, char lex[]
 *   entries MAXALIGN'd; ~16 bytes per 6-char lexeme
 *
 * v2 (current) layout:
 *   int32 vl_len_, char magic[4], uint8 version, uint8 reserved[3],
 *   int32 index_name_len, int32 entry_count, data...
 *   per-entry (variable-length, no padding):
 *     varint frequency       (1 byte for freq <= 127)
 *     varint lexeme_len      (1 byte for lex_len <= 127)
 *     char lexeme[lexeme_len]
 *   ~8 bytes per 6-char lexeme + freq=1
 *
 * Internal code only ever sees v2: operator entry points
 * (tpvector_recv, tpvector_eq, tpvector_send, tpvector_out)
 * canonicalize via tpvector_canonicalize() at the boundary,
 * converting v1 to v2 in palloc'd memory if needed.
 *
 * Entry access pattern: entries are byte streams, so the legacy
 * `entry->frequency` / `entry->lexeme_len` / `entry->lexeme` field
 * accesses are no longer possible. Use TpVectorEntryView (a decoded
 * snapshot) and the iterator helpers below.
 */
#pragma once

#include <postgres.h>

#include <fmgr.h>

/* v2 format constants */
#define TPVECTOR_V2_MAGIC0 'B'
#define TPVECTOR_V2_MAGIC1 'M'
#define TPVECTOR_V2_MAGIC2 '2'
#define TPVECTOR_V2_MAGIC3 '5'
#define TPVECTOR_VERSION   2

/*
 * Opaque entry handle. v2 entries are variable-length byte
 * sequences; field access goes through tpvector_entry_decode().
 */
typedef struct TpVectorEntry TpVectorEntry;

/*
 * Decoded snapshot of a single entry. Fields are normal C types,
 * `lexeme` points into the underlying TpVector buffer (not
 * separately allocated; do not free).
 */
typedef struct TpVectorEntryView
{
	uint32		frequency;
	uint32		lexeme_len;
	const char *lexeme;
} TpVectorEntryView;

/* tpvector data type structure (v2 layout) */
typedef struct TpVector
{
	int32 vl_len_;					   /* varlena header (must be first) */
	char  magic[4];					   /* "BM25" — discriminates v2 from v1 */
	uint8 version;					   /* TPVECTOR_VERSION (2) */
	uint8 reserved[3];				   /* zero; future flags / format bits */
	int32 index_name_len;			   /* length of index name */
	int32 entry_count;				   /* number of term/frequency pairs */
	char  data[FLEXIBLE_ARRAY_MEMBER]; /* payload: index name + entries */
} TpVector;

/* Macros for accessing tpvector variable-length data (v2). */
#define TPVECTOR_INDEX_NAME_PTR(x) (((TpVector *)(x))->data)
#define TPVECTOR_ENTRIES_PTR(x)                  \
	((TpVectorEntry *)(((TpVector *)(x))->data + \
					   MAXALIGN(((TpVector *)(x))->index_name_len + 1)))

/* Function declarations */
Datum tpvector_in(PG_FUNCTION_ARGS);
Datum tpvector_out(PG_FUNCTION_ARGS);
Datum tpvector_recv(PG_FUNCTION_ARGS);
Datum tpvector_send(PG_FUNCTION_ARGS);
Datum to_tpvector(PG_FUNCTION_ARGS);
Datum tpvector_eq(PG_FUNCTION_ARGS);

/* Constructor */
TpVector *create_tpvector_from_strings(
		const char	*index_name,
		int			 entry_count,
		const char **lexemes,
		const int32 *frequencies);

char *get_tpvector_index_name(TpVector *tpvec);

/*
 * Entry iteration. Pattern:
 *
 *   TpVectorEntry *e = get_tpvector_first_entry(vec);
 *   for (int i = 0; i < vec->entry_count; i++) {
 *       TpVectorEntryView v;
 *       tpvector_entry_decode(e, &v);
 *       // use v.frequency, v.lexeme_len, v.lexeme
 *       e = get_tpvector_next_entry(e);
 *   }
 */
TpVectorEntry *get_tpvector_first_entry(TpVector *vec);
TpVectorEntry *get_tpvector_next_entry(TpVectorEntry *current);
void tpvector_entry_decode(const TpVectorEntry *entry, TpVectorEntryView *out);

/*
 * Format detection + canonicalization.
 *
 * tpvector_is_v2(raw) returns true if `raw` carries the v2 magic.
 * tpvector_canonicalize(raw) returns a v2 TpVector — either `raw`
 *   itself if already v2, or a freshly palloc'd v2 copy converted
 *   from v1.
 */
bool	  tpvector_is_v2(const void *raw);
TpVector *tpvector_canonicalize(TpVector *raw);

/*
 * Varint helpers (internal but exposed for adjacent code).
 *
 * Encoding: little-endian base-128. Each byte's high bit indicates
 * whether more bytes follow. Values 0..127 take 1 byte.
 */
uint32 tpvector_varint_decode(const uint8 **cursor, const uint8 *end);
size_t tpvector_varint_encode(uint32 v, uint8 *out);
size_t tpvector_varint_size(uint32 v);
