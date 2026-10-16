/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * handler.c - BM25 access method handler and options
 */
#include <postgres.h>

#include <access/amapi.h>
#include <access/htup_details.h>
#include <access/relation.h>
#include <access/reloptions.h>
#include <catalog/index.h>
#include <catalog/pg_class.h>
#include <catalog/pg_opclass.h>
#include <catalog/pg_type.h>
#include <commands/vacuum.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "access/am.h"
#include "planner/cost.h"
#include "tsearch/ts_utils.h"

/* Relation options - initialized in mod.c */
extern relopt_kind tp_relopt_kind;

/*
 * Report index properties for pg_index_column_has_property() etc.
 */
static bool
tp_property(
		Oid				index_oid,
		int				attno,
		IndexAMProperty prop,
		const char	   *propname,
		bool		   *res,
		bool		   *isnull)
{
	(void)index_oid;
	(void)attno;
	(void)propname;
	(void)isnull;

	switch (prop)
	{
	case AMPROP_DISTANCE_ORDERABLE:
		*res = true;
		return true;
	default:
		return false; /* Let core handle other properties */
	}
}

/*
 * Access method handler - returns IndexAmRoutine with function pointers
 */
PG_FUNCTION_INFO_V1(tp_handler);

Datum
tp_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine;

	(void)fcinfo; /* unused */

	amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies	  = TSearchStrategyNumber;
	amroutine->amsupport	  = 8; /* 8 for distance */
	amroutine->amoptsprocnum  = 0;
	amroutine->amcanorder	  = false;
	amroutine->amcanorderbyop = true; /* Supports ORDER BY operators */
#if PG_VERSION_NUM >= 180000
	amroutine->amcanhash			= false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering =
			true; /* Support consistent ordering for ORDER BY */
#endif
	amroutine->amcanbackward	  = false; /* Cannot scan backwards */
	amroutine->amcanunique		  = false; /* Cannot enforce uniqueness */
	amroutine->amcanmulticol	  = false; /* Single column only */
	amroutine->amoptionalkey	  = true;  /* Can scan without search key */
	amroutine->amsearcharray	  = false; /* No array search support */
	amroutine->amsearchnulls	  = false; /* Cannot search for NULLs */
	amroutine->amstorage		  = false; /* No separate storage type */
	amroutine->amclusterable	  = false; /* Cannot cluster on this index */
	amroutine->ampredlocks		  = false; /* No predicate locking */
	amroutine->amcanparallel	  = false; /* No parallel scan support yet */
	amroutine->amcanbuildparallel = true;
	amroutine->amcaninclude		  = false; /* No INCLUDE columns */
	amroutine->amusemaintenanceworkmem =
			false; /* Vacuum does not use maintenance work mem */
	amroutine->amsummarizing		   = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype			   = InvalidOid;

	/* Interface functions */
	amroutine->ambuild			= tp_build;
	amroutine->ambuildempty		= tp_buildempty;
	amroutine->aminsert			= tp_insert;
	amroutine->aminsertcleanup	= NULL;
	amroutine->ambulkdelete		= tp_bulkdelete;
	amroutine->amvacuumcleanup	= tp_vacuumcleanup;
	amroutine->amcanreturn		= NULL;
	amroutine->amcostestimate	= tp_costestimate;
	amroutine->amoptions		= tp_options;
	amroutine->amproperty		= tp_property;
	amroutine->ambuildphasename = tp_buildphasename; /* No build phase names */
	amroutine->amvalidate		= tp_validate;
	amroutine->amadjustmembers	= NULL; /* No member adjustment */
	amroutine->ambeginscan		= tp_beginscan;
	amroutine->amrescan			= tp_rescan;
	amroutine->amgettuple		= tp_gettuple;
	amroutine->amgetbitmap		= NULL; /* No bitmap scans - ORDER BY only */
	amroutine->amendscan		= tp_endscan;
	amroutine->ammarkpos		= NULL; /* No mark/restore support */
	amroutine->amrestrpos		= NULL;
	amroutine->amestimateparallelscan = NULL; /* No parallel support yet */
	amroutine->aminitparallelscan	  = NULL;
	amroutine->amparallelrescan		  = NULL;

#if PG_VERSION_NUM >= 180000
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype  = NULL;
#endif

	PG_RETURN_POINTER(amroutine);
}

/*
 * Parse and validate index options
 */
bytea *
tp_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] =
			{{.optname = "text_config",
			  .opttype = RELOPT_TYPE_STRING,
			  .offset  = offsetof(TpOptions, text_config_offset)},
			 {.optname = "k1",
			  .opttype = RELOPT_TYPE_REAL,
			  .offset  = offsetof(TpOptions, k1)},
			 {.optname = "b",
			  .opttype = RELOPT_TYPE_REAL,
			  .offset  = offsetof(TpOptions, b)},
			 {.optname = "compaction_schedule",
			  .opttype = RELOPT_TYPE_STRING,
			  .offset  = offsetof(TpOptions, compaction_schedule_offset)},
			 {.optname = "compaction_lineage",
			  .opttype = RELOPT_TYPE_STRING,
			  .offset  = offsetof(TpOptions, compaction_lineage_offset)},
			 {.optname = "compaction",
			  .opttype = RELOPT_TYPE_ENUM,
			  .offset  = offsetof(TpOptions, compaction)}};

	return (bytea *)build_reloptions(
			reloptions,
			validate,
			tp_relopt_kind,
			sizeof(TpOptions),
			tab,
			lengthof(tab));
}

/*
 * Validate BM25 index definition
 */
bool
tp_validate(Oid opclassoid)
{
	HeapTuple		tup;
	Form_pg_opclass opclassform;
	Oid				opcintype;
	bool			result = true;

	/* Look up the opclass */
	tup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(tup))
	{
		elog(WARNING, "cache lookup failed for operator class %u", opclassoid);
		return false;
	}

	opclassform = (Form_pg_opclass)GETSTRUCT(tup);
	opcintype	= opclassform->opcintype;

	switch (opcintype)
	{
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID:	   /* char(n) */
	case TEXTARRAYOID: /* text[] */
	case 1015:		   /* varchar[] */
	case 1014:		   /* bpchar[] */
		result = true;
		break;
	default:
		elog(WARNING,
			 "Tapir index can only be created on text, varchar, "
			 "or char columns (or arrays thereof) "
			 "(got type OID %u)",
			 opcintype);
		result = false;
		break;
	}

	ReleaseSysCache(tup);

	return result;
}

/*
 * Report a relation OID that does not name an existing relation.  Used on
 * every path so that superusers and nonowners alike see a user-facing error
 * rather than an internal cache-lookup or relation-open failure.
 */
static void
tp_report_missing_relation(Oid relid)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("relation with OID %u does not exist", relid)));
}

/*
 * Whether the caller has the requested access.  For MAINTAIN, check the
 * index's table, as REINDEX does; heap_oid may be InvalidOid when the OID
 * does not name an index, in which case the relation itself is checked so
 * that the caller can learn it is not a bm25 index only if privileged.
 */
static bool
tp_has_index_access(Oid indexoid, Oid heap_oid, TpIndexAccess access)
{
	Oid target;

	if (access == TP_INDEX_ACCESS_ANY)
		return true;
	if (object_ownercheck(RelationRelationId, indexoid, GetUserId()))
		return true;
	if (access == TP_INDEX_ACCESS_OWNER)
		return false;

	target = OidIsValid(heap_oid) ? heap_oid : indexoid;
	return pg_class_aclcheck(target, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK;
}

/*
 * Raise an error unless the caller has the requested access.  The OWNER
 * error matches aclcheck_error(); the MAINTAIN error has the same form,
 * with a detail naming the privilege that would suffice.
 */
static void
tp_check_index_access(
		Oid indexoid, Oid heap_oid, TpIndexAccess access, const char *relname)
{
	if (tp_has_index_access(indexoid, heap_oid, access))
		return;

	if (access == TP_INDEX_ACCESS_OWNER)
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relname);
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied for index %s", relname),
			 errdetail(
					 "Only the owner of the index, or a role with the "
					 "MAINTAIN privilege on its table, may do this.")));
}

/*
 * Open a physical bm25 index by OID, validating that it is in fact a bm25
 * index with storage and that the caller has the requested access.
 */
Relation
tp_open_bm25_index(Oid indexoid, LOCKMODE lockmode, TpIndexAccess access)
{
	Relation index_rel;

	/*
	 * Reject unprivileged callers before queuing for a heavyweight lock.
	 * Recheck after opening because ALTER OWNER or GRANT/REVOKE can complete
	 * while this caller waits.  Check existence first: object_ownercheck()
	 * treats a missing OID as an internal error.
	 */
	if (access != TP_INDEX_ACCESS_ANY)
	{
		char *relname = get_rel_name(indexoid);

		if (relname == NULL)
			tp_report_missing_relation(indexoid);
		tp_check_index_access(
				indexoid, IndexGetRelation(indexoid, true), access, relname);
	}

	/* The relation may also be dropped while this caller waits. */
	index_rel = try_relation_open(indexoid, lockmode);
	if (index_rel == NULL)
		tp_report_missing_relation(indexoid);

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a bm25 index", relname)));
	}

	if (index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a partitioned bm25 index", relname),
				 errhint("Use a physical partition index instead.")));
	}

	/*
	 * The index is locked, so it and its table exist and these lookups
	 * cannot fail.  On error, transaction abort releases the relation.
	 */
	tp_check_index_access(
			indexoid,
			index_rel->rd_index->indrelid,
			access,
			RelationGetRelationName(index_rel));

	return index_rel;
}
