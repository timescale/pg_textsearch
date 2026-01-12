/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/handler.c - BM25 access method handler and options
 */
#include <postgres.h>

#include <access/amapi.h>
#include <access/htup_details.h>
#include <access/reloptions.h>
#include <catalog/pg_opclass.h>
#include <commands/vacuum.h>
#include <utils/syscache.h>

#include "am.h"
#include "planner/cost.h"

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

	amroutine->amstrategies	  = 0; /* No search strategies - ORDER BY only */
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
	amroutine->amcaninclude		  = false;		/* No INCLUDE columns */
	amroutine->amusemaintenanceworkmem = false; /* Use work_mem for builds */
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
			{{"text_config",
			  RELOPT_TYPE_STRING,
			  offsetof(TpOptions, text_config_offset),
			  0},
			 {"k1", RELOPT_TYPE_REAL, offsetof(TpOptions, k1), 0},
			 {"b", RELOPT_TYPE_REAL, offsetof(TpOptions, b), 0}};

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
	case BPCHAROID: /* char(n) */
		result = true;
		break;
	default:
		elog(WARNING,
			 "Tapir index can only be created on text, varchar, or char "
			 "columns (got type OID %u)",
			 opcintype);
		result = false;
		break;
	}

	ReleaseSysCache(tup);

	return result;
}
