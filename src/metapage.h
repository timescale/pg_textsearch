/*-------------------------------------------------------------------------
 *
 * metapage.h
 *	  Tapir index metapage structures and operations
 *
 * This module defines the metapage structure and provides functions
 * for initializing, reading, and managing Tapir index metapages.
 * The metapage stores index configuration and statistics.
 *
 * IDENTIFICATION
 *	  src/metapage.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include "constants.h"
#include "postgres.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

/*
 * Tapir Index Metapage Structure
 *
 * The metapage is stored on block 0 of every Tapir index and contains
 * configuration parameters and global statistics needed for BM25 scoring.
 */
typedef struct TpIndexMetaPageData
{
	uint32 magic;				  /* Magic number for validation */
	uint32 version;				  /* Index format version */
	Oid text_config_oid;		  /* Text search configuration OID */
	uint64 total_docs;			  /* Total number of documents */
	uint64 total_terms;			  /* Total term occurrences across all docs */
	uint64 total_len;			  /* Total length of all documents */
	float4 k1;					  /* BM25 k1 parameter */
	float4 b;					  /* BM25 b parameter */
	BlockNumber root_blkno;		  /* Root page of the index tree */
	BlockNumber term_stats_root;  /* Root page of term statistics B-tree */
	BlockNumber first_docid_page; /* First page of docid chain for crash recovery */
} TpIndexMetaPageData;

typedef TpIndexMetaPageData *TpIndexMetaPage;

/*
 * Document ID page structure for crash recovery
 * Stores ItemPointerData (ctid) entries for documents in the index
 */
#define TP_DOCID_PAGE_MAGIC 0x54504449 /* "TPDI" in hex */

typedef struct TpDocidPageHeader
{
	uint32 magic;		   /* Magic number for validation */
	uint32 num_docids;	   /* Number of docids stored on this page */
	BlockNumber next_page; /* Next page in chain, or InvalidBlockNumber */
	uint32 reserved;	   /* Reserved for future use */
} TpDocidPageHeader;

/*
 * Metapage operations
 */
extern void tp_init_metapage(Page page, Oid text_config_oid);
extern TpIndexMetaPage tp_get_metapage(Relation index);

/*
 * Document ID operations for crash recovery
 */
extern void tp_add_docid_to_pages(Relation index, ItemPointer ctid);
extern void tp_recover_from_docid_pages(Relation index);