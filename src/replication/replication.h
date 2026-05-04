/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * replication.h - Custom WAL resource manager for pg_textsearch
 *
 * pg_textsearch's memtable lives in DSA shared memory and is
 * not directly WAL-logged by Postgres. This rmgr emits two record
 * types so that physical-replication standbys (and primary crash
 * recovery) can apply memtable mutations:
 *
 *   INSERT_TERMS — appends a tokenized term list (v2 bm25vector
 *                  payload) for one document's worth of postings.
 *                  Carries: index_oid, ctid, vector bytes.
 *   SPILL        — signals that the primary spilled the memtable
 *                  to a segment. Standby redo clears the in-memory
 *                  memtable; the segment data itself replicates via
 *                  the existing GenericXLog records emitted by
 *                  segment.c.
 */
#ifndef PG_TEXTSEARCH_REPLICATION_H
#define PG_TEXTSEARCH_REPLICATION_H

#include <postgres.h>

#include <access/xlogreader.h>
#include <storage/itemptr.h>
#include <utils/relcache.h>

#include "types/vector.h"

#define TP_RMGR_ID	 149
#define TP_RMGR_NAME "pg_textsearch"

/* Record info bits (XLOG_TP_*) — distinguish the two record types. */
#define XLOG_TP_INSERT_TERMS 0x10
#define XLOG_TP_SPILL		 0x20

/*
 * INSERT_TERMS payload (followed by `vector_size` bytes of v2
 * bm25vector wire data — see types/vector.h for the layout).
 */
typedef struct xl_tp_insert_terms
{
	Oid				index_oid;
	ItemPointerData ctid;
	uint32			vector_size; /* bytes of v2 bm25vector following */
} xl_tp_insert_terms;

/* SPILL payload. */
typedef struct xl_tp_spill
{
	Oid index_oid;
} xl_tp_spill;

/* Registration — called once from _PG_init. */
extern void tp_register_rmgr(void);

/* Rmgr callbacks. */
extern void		   tp_rmgr_redo(XLogReaderState *record);
extern void		   tp_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *tp_rmgr_identify(uint8 info);

/*
 * Emission helpers — called from primary insert/spill paths.
 * Return the LSN of the emitted record. The caller is responsible
 * for being inside a critical section.
 */
extern XLogRecPtr
tp_xlog_insert_terms(Oid index_oid, ItemPointer ctid, const TpVector *vec);
extern XLogRecPtr tp_xlog_spill(Oid index_oid);

#endif /* PG_TEXTSEARCH_REPLICATION_H */
