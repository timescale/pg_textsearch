/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_job.c - Managed pg_durable compaction jobs
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relation.h>
#include <access/stratnum.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/dependency.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/objectaddress.h>
#include <catalog/partition.h>
#include <catalog/pg_am_d.h>
#include <catalog/pg_authid.h>
#include <catalog/pg_class.h>
#include <catalog/pg_database_d.h>
#include <catalog/pg_depend.h>
#include <catalog/pg_depend_d.h>
#include <catalog/pg_extension.h>
#include <catalog/pg_extension_d.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_namespace_d.h>
#include <catalog/pg_operator.h>
#include <catalog/pg_operator_d.h>
#include <catalog/pg_proc_d.h>
#include <catalog/pg_type_d.h>
#include <commands/dbcommands.h>
#include <commands/defrem.h>
#include <commands/extension.h>
#include <executor/spi.h>
#include <lib/stringinfo.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <parser/parse_func.h>
#include <storage/lmgr.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <utils/varlena.h>

#include "access/am.h"
#include "index/compaction_job.h"
#include "index/compaction_request.h"

#define TP_JOB_LABEL_PREFIX "pg_textsearch:bg:v1:"

/*
 * Required v0.2.8 entry points include df.wait_for_signal,
 * df.wait_for_schedule, and df.explain.  Their catalog identities are resolved
 * below rather than trusting search_path.
 */
typedef struct TpCompactionJobTarget
{
	Oid			  index_oid;
	Oid			  database_oid;
	Oid			  tablespace_oid;
	RelFileNumber relfilenumber;
	Oid			  owner_oid;
	Oid			  heap_oid;
	char		 *index_name;
	char		 *schedule;
	char		 *lineage;
	bool		  lineage_backfilled;
	char		 *history_prefix;
	char		 *family_prefix;
} TpCompactionJobTarget;

struct TpCompactionJobObjects
{
	Oid	  durable_extension_oid;
	Oid	  durable_extension_owner;
	Oid	  durable_namespace_oid;
	Oid	  operator_namespace_oid;
	Oid	  textsearch_extension_oid;
	Oid	  textsearch_namespace_oid;
	Oid	  textsearch_extension_owner;
	Oid	  bm25_am_oid;
	Oid	  start_function_oid;
	Oid	  explain_function_oid;
	Oid	  signal_function_oid;
	Oid	  wait_signal_function_oid;
	Oid	  wait_schedule_function_oid;
	Oid	  loop_function_oid;
	Oid	  break_function_oid;
	Oid	  step_function_oid;
	Oid	  current_function_oid;
	Oid	  operator_oids[5];
	Oid	  instances_relation_oid;
	Oid	  nodes_relation_oid;
	Oid	  vars_relation_oid;
	char *durable_schema;
	char *textsearch_schema;
	char *operator_schema;
	char *start_function;
	char *explain_function;
	char *signal_function;
	char *wait_signal_function;
	char *wait_schedule_function;
	char *loop_function;
	char *break_function;
	char *instances_relation;
	char *step_function;
	char *current_function;
};

typedef struct TpHistoryHeapOwner
{
	Oid heap_oid;
	Oid owner_oid;
} TpHistoryHeapOwner;

typedef enum TpJobObjectLookupMode
{
	TP_JOB_OBJECTS_PREFLIGHT,
	TP_JOB_OBJECTS_LOCKED
} TpJobObjectLookupMode;

typedef struct TpJobObjectLock
{
	Oid		 class_id;
	Oid		 object_id;
	LOCKMODE mode;
} TpJobObjectLock;

static char *tp_copy_spi_text(
		HeapTuple	  tuple,
		TupleDesc	  tuple_desc,
		int			  column,
		MemoryContext context);

static int
tp_set_safe_elevated_gucs(void)
{
	int save_nestlevel = NewGUCNestLevel();

	/* Never inherit caller-controlled name resolution across a user switch. */
	(void)set_config_option(
			"search_path",
			"pg_catalog, pg_temp",
			PGC_USERSET,
			PGC_S_SESSION,
			GUC_ACTION_SAVE,
			true,
			0,
			false);
	/* pg_dump emits row_security=off, which cannot be inherited here. */
	(void)set_config_option(
			"row_security",
			"on",
			PGC_USERSET,
			PGC_S_SESSION,
			GUC_ACTION_SAVE,
			true,
			0,
			false);
	return save_nestlevel;
}

static void
tp_durable_required(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("background compaction requires pg_durable 0.2.8 or "
					"newer")));
}

static void
tp_durable_not_initialized(const char *detail)
{
	if (detail != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_durable is not initialized for this database"),
				 errdetail("%s", detail)));

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("pg_durable is not initialized for this database")));
}

static bool
tp_version_at_least_0_2_8(const char *version)
{
	unsigned int major;
	unsigned int minor;
	unsigned int patch;
	char		 extra;

	if (sscanf(version, "%u.%u.%u%c", &major, &minor, &patch, &extra) != 3)
		return false;

	if (major != 0)
		return major > 0;
	if (minor != 2)
		return minor > 2;
	return patch >= 8;
}

static bool
tp_library_is_preloaded(const char *library)
{
	const char *preloads;
	char	   *raw;
	List	   *names = NIL;
	bool		found = false;

	preloads = GetConfigOption("shared_preload_libraries", false, false);
	raw		 = pstrdup(preloads);
	if (!SplitIdentifierString(raw, ',', &names))
	{
		pfree(raw);
		return false;
	}

	foreach_ptr(char, name, names)
	{
		if (strcmp(name, library) == 0)
		{
			found = true;
			break;
		}
	}

	list_free(names);
	pfree(raw);
	return found;
}

static void
tp_lock_extension(Oid extension_oid)
{
	LockDatabaseObject(ExtensionRelationId, extension_oid, 0, AccessShareLock);
}

static Oid
tp_resolve_extension_function(
		Oid					  extension_oid,
		const char			 *schema_name,
		const char			 *function_name,
		int					  nargs,
		const Oid			 *argtypes,
		TpJobObjectLookupMode mode)
{
	List *names;
	Oid	  function_oid;

	names = list_make2(
			makeString(pstrdup(schema_name)),
			makeString(pstrdup(function_name)));
	function_oid = LookupFuncName(names, nargs, argtypes, true);
	list_free_deep(names);

	if (!OidIsValid(function_oid) ||
		getExtensionOfObject(ProcedureRelationId, function_oid) !=
				extension_oid)
		tp_durable_not_initialized(
				"a required pg_durable function is missing");

	if (mode == TP_JOB_OBJECTS_LOCKED)
	{
		LockDatabaseObject(
				ProcedureRelationId, function_oid, 0, AccessShareLock);
		if (getExtensionOfObject(ProcedureRelationId, function_oid) !=
			extension_oid)
			tp_durable_not_initialized(
					"a required pg_durable function changed during admission");
	}

	return function_oid;
}

static Oid
tp_resolve_start_function(Oid extension_oid, TpJobObjectLookupMode mode)
{
	Oid				  function_oid = InvalidOid;
	List			 *names;
	FuncCandidateList candidates;
	bool			  ambiguous = false;

	names = list_make2(
			makeString(pstrdup("df")), makeString(pstrdup("start")));
#if PG_VERSION_NUM >= 190000
	candidates = FuncnameGetCandidates(
			names, 4, NIL, false, true, false, true, NULL);
#else
	candidates =
			FuncnameGetCandidates(names, 4, NIL, false, true, false, true);
#endif
	for (FuncCandidateList candidate = candidates; candidate != NULL;
		 candidate					 = candidate->next)
	{
		bool matches = candidate->nominalnargs >= 4 && candidate->nargs >= 4 &&
					   candidate->nargs - candidate->ndargs == 4;

		for (int i = 0; matches && i < 4; i++)
			matches = candidate->args[i] == TEXTOID;
		if (!matches)
			continue;

		if (!OidIsValid(candidate->oid))
		{
			ambiguous = true;
			continue;
		}
		if (get_func_rettype(candidate->oid) != TEXTOID ||
			getExtensionOfObject(ProcedureRelationId, candidate->oid) !=
					extension_oid)
			continue;
		if (OidIsValid(function_oid) && function_oid != candidate->oid)
			ambiguous = true;
		function_oid = candidate->oid;
	}
	list_free_deep(names);

	if (ambiguous)
		tp_durable_not_initialized(
				"df.start(text,text,text,text) is ambiguous");
	if (!OidIsValid(function_oid))
		tp_durable_not_initialized("df.start(text,text,text,text) is missing");

	if (mode == TP_JOB_OBJECTS_LOCKED)
	{
		LockDatabaseObject(
				ProcedureRelationId, function_oid, 0, AccessShareLock);
		if (getExtensionOfObject(ProcedureRelationId, function_oid) !=
			extension_oid)
			tp_durable_not_initialized(
					"df.start(text,text,text,text) changed during admission");
	}

	return function_oid;
}

static Oid
tp_resolve_extension_operator(
		Oid					  extension_oid,
		const char			 *schema_name,
		const char			 *operator_name,
		TpJobObjectLookupMode mode)
{
	List *names;
	Oid	  operator_oid;

	names = list_make2(
			makeString(pstrdup(schema_name)),
			makeString(pstrdup(operator_name)));
	operator_oid = OpernameGetOprid(names, TEXTOID, TEXTOID);
	list_free_deep(names);

	if (!OidIsValid(operator_oid))
		tp_durable_not_initialized(psprintf(
				"required text operator %s is missing", operator_name));
	if (getExtensionOfObject(OperatorRelationId, operator_oid) !=
		extension_oid)
		tp_durable_not_initialized(psprintf(
				"required text operator %s (OID %u) is not owned by "
				"pg_durable",
				operator_name,
				operator_oid));

	if (mode == TP_JOB_OBJECTS_LOCKED)
	{
		LockDatabaseObject(
				OperatorRelationId, operator_oid, 0, AccessShareLock);
		if (getExtensionOfObject(OperatorRelationId, operator_oid) !=
			extension_oid)
			tp_durable_not_initialized(psprintf(
					"required text operator %s changed during admission",
					operator_name));
	}

	return operator_oid;
}

static Oid
tp_resolve_extension_relation(
		Oid					  extension_oid,
		Oid					  namespace_oid,
		const char			 *relation_name,
		TpJobObjectLookupMode mode)
{
	Oid relation_oid = get_relname_relid(relation_name, namespace_oid);

	if (!OidIsValid(relation_oid) ||
		getExtensionOfObject(RelationRelationId, relation_oid) !=
				extension_oid)
		tp_durable_not_initialized(
				psprintf("df.%s is missing", relation_name));

	if (mode == TP_JOB_OBJECTS_LOCKED)
	{
		LockRelationOid(relation_oid, AccessShareLock);
		if (getExtensionOfObject(RelationRelationId, relation_oid) !=
			extension_oid)
			tp_durable_not_initialized(
					psprintf("df.%s changed during admission", relation_name));
	}

	return relation_oid;
}

static char *
tp_qualified_function_name(Oid function_oid)
{
	char *function_name = get_func_name(function_oid);
	char *schema_name	= get_namespace_name(get_func_namespace(function_oid));

	if (function_name == NULL || schema_name == NULL)
		tp_durable_not_initialized(
				"a required extension function has no catalog identity");

	return quote_qualified_identifier(schema_name, function_name);
}

/*
 * Read the owner and version of a pg_extension row by OID.
 *
 * The EXTENSIONOID syscache was added in a later minor release, so scan the
 * catalog directly to stay portable across supported servers.  Returns false
 * when no row exists.  A requested version is palloc'd, or NULL when the
 * column is null.
 */
static bool
tp_extension_lookup(Oid extension_oid, Oid *owner_out, char **version_out)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData entry[1];
	HeapTuple	tuple;
	bool		found = false;

	rel = table_open(ExtensionRelationId, AccessShareLock);
	ScanKeyInit(
			&entry[0],
			Anum_pg_extension_oid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(extension_oid));
	scan = systable_beginscan(rel, ExtensionOidIndexId, true, NULL, 1, entry);

	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
	{
		found = true;

		if (owner_out != NULL)
			*owner_out = ((Form_pg_extension)GETSTRUCT(tuple))->extowner;

		if (version_out != NULL)
		{
			Datum datum;
			bool  isnull;

			datum = heap_getattr(
					tuple,
					Anum_pg_extension_extversion,
					RelationGetDescr(rel),
					&isnull);
			*version_out = isnull ? NULL : TextDatumGetCString(datum);
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}

static Oid
tp_extension_owner(Oid extension_oid)
{
	Oid owner_oid = InvalidOid;

	if (!tp_extension_lookup(extension_oid, &owner_oid, NULL))
		tp_durable_not_initialized("an extension catalog row disappeared");

	return owner_oid;
}

static List *
tp_history_heap_owners(Oid heap_oid, Oid expected_owner_oid)
{
	List	 *relation_oids;
	List	 *identities = NIL;
	ListCell *lc;
	bool	  partitioned;

	partitioned	  = get_rel_relkind(heap_oid) == RELKIND_PARTITIONED_TABLE;
	relation_oids = partitioned ? find_all_inheritors(heap_oid, NoLock, NULL)
								: list_make1_oid(heap_oid);
	foreach (lc, relation_oids)
	{
		Oid					relation_oid = lfirst_oid(lc);
		HeapTuple			tuple;
		Form_pg_class		relation_form;
		TpHistoryHeapOwner *identity;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
		if (!HeapTupleIsValid(tuple))
			continue;
		relation_form = (Form_pg_class)GETSTRUCT(tuple);
		if ((partitioned &&
			 (!relation_form->relispartition ||
			  relation_form->relkind == RELKIND_PARTITIONED_TABLE)) ||
			(!partitioned && relation_form->relowner != expected_owner_oid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		identity			= palloc(sizeof(*identity));
		identity->heap_oid	= relation_oid;
		identity->owner_oid = relation_form->relowner;
		identities			= lappend(identities, identity);
		ReleaseSysCache(tuple);
	}
	list_free(relation_oids);
	return identities;
}

static bool
tp_label_has_lineage(
		const char *label,
		const char *lineage,
		Oid			submitted_by,
		List	   *identities)
{
	unsigned int database_oid;
	unsigned int index_oid;
	unsigned int tablespace_oid;
	unsigned int relfilenumber;
	unsigned int label_owner_oid;
	unsigned int label_heap_oid;
	char		 parsed[TP_COMPACTION_LINEAGE_LENGTH + 1];
	int			 consumed = 0;
	ListCell	*lc;

	if (sscanf(label,
			   TP_JOB_LABEL_PREFIX "%u:%u:%u:%u:%u:%u:%32[0-9a-f]:%n",
			   &database_oid,
			   &index_oid,
			   &tablespace_oid,
			   &relfilenumber,
			   &label_owner_oid,
			   &label_heap_oid,
			   parsed,
			   &consumed) != 7 ||
		consumed <= 0 || database_oid != MyDatabaseId ||
		label_owner_oid != submitted_by || strcmp(parsed, lineage) != 0)
		return false;

	foreach (lc, identities)
	{
		TpHistoryHeapOwner *identity = lfirst(lc);

		if (identity->heap_oid == label_heap_oid &&
			identity->owner_oid == label_owner_oid)
			return true;
	}
	return false;
}

bool
tp_compaction_job_lineage_exists(
		const TpCompactionJobObjects *objects,
		const char					 *lineage,
		Oid							  heap_oid,
		Oid							  owner_oid)
{
	Oid			   save_userid;
	int			   save_sec_context;
	int			   save_nestlevel;
	bool		   spi_connected = false;
	bool		   found		 = false;
	StringInfoData sql;
	Oid			   argtypes[1] = {TEXTOID};
	Datum		   values[1];
	List		  *identities;
	char		  *schema;
	char		  *relation;
	char		  *prefix;

	tp_require_compaction_dependency_lock();
	schema	 = get_namespace_name(objects->durable_namespace_oid);
	relation = get_rel_name(objects->instances_relation_oid);
	if (schema == NULL || relation == NULL)
		return false;

	identities = tp_history_heap_owners(heap_oid, owner_oid);
	prefix	   = psprintf(TP_JOB_LABEL_PREFIX "%u:", MyDatabaseId);
	values[0]  = CStringGetTextDatum(prefix);
	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.label, "
			"instance.submitted_by::pg_catalog.oid "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.~~) "
			"($1 OPERATOR(pg_catalog.||) '%%')",
			quote_qualified_identifier(schema, relation));

	/* RLS must not hide a retained lineage from this internal collision check.
	 */
	save_nestlevel = tp_set_safe_elevated_gucs();
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			objects->durable_extension_owner,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		int rc;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;
		rc			  = SPI_execute_with_args(
				   sql.data, 1, argtypes, values, NULL, true, 0);
		if (rc != SPI_OK_SELECT)
			elog(ERROR, "could not inspect pg_durable lineage history");

		for (uint64 i = 0; i < SPI_processed; i++)
		{
			bool  isnull;
			Datum submitted_by;
			char *label = tp_copy_spi_text(
					SPI_tuptable->vals[i],
					SPI_tuptable->tupdesc,
					1,
					CurrentMemoryContext);

			submitted_by = SPI_getbinval(
					SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull);
			found = !isnull && tp_label_has_lineage(
									   label,
									   lineage,
									   DatumGetObjectId(submitted_by),
									   identities);
			pfree(label);
			if (found)
				break;
		}

		SPI_finish();
		spi_connected = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		AtEOXact_GUC(false, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	pfree(prefix);
	pfree(sql.data);
	pfree(schema);
	pfree(relation);
	list_free_deep(identities);
	return found;
}

static void
tp_populate_job_objects_as_owner(
		TpCompactionJobObjects *objects, TpJobObjectLookupMode mode)
{
	char	   *version;
	Oid			durable_oid;
	Oid			durable_owner;
	Oid			rechecked_oid;
	Oid			start_oid;
	Oid			durable_namespace_oid;
	char	   *durable_schema;
	const char *configured_database;
	char	   *database_name;
	Oid			textsearch_oid;
	Oid			textsearch_namespace_oid;
	char	   *textsearch_schema;
	Oid			operator_schema_oid;
	char	   *operator_schema;
	AttrNumber	submitted_by_attnum;
	Oid			text_args[2]		= {TEXTOID, TEXTOID};
	Oid			loop_args[3]		= {TEXTOID, TEXTOID, BOOLOID};
	Oid			signal_args[3]		= {TEXTOID, TEXTOID, TEXTOID};
	Oid			wait_signal_args[2] = {TEXTOID, INT4OID};
	Oid			oid_args[5]			= {OIDOID, OIDOID, OIDOID, OIDOID, OIDOID};

	memset(objects, 0, sizeof(*objects));

	durable_oid = get_extension_oid("pg_durable", true);
	if (!OidIsValid(durable_oid))
		tp_durable_required();

	if (mode == TP_JOB_OBJECTS_LOCKED)
	{
		tp_lock_extension(durable_oid);
		rechecked_oid = get_extension_oid("pg_durable", true);
		if (rechecked_oid != durable_oid)
			tp_durable_required();
	}

	if (!tp_extension_lookup(durable_oid, &durable_owner, &version))
		tp_durable_required();
	if (version == NULL)
		tp_durable_required();
	if (!tp_version_at_least_0_2_8(version))
	{
		pfree(version);
		tp_durable_required();
	}
	pfree(version);

	if (!tp_library_is_preloaded("pg_durable"))
		tp_durable_not_initialized(
				"pg_durable is not present in shared_preload_libraries");

	configured_database = GetConfigOption("pg_durable.database", true, false);
	database_name		= get_database_name(MyDatabaseId);
	if (configured_database == NULL || database_name == NULL ||
		strcmp(configured_database, database_name) != 0)
		tp_durable_not_initialized(
				"pg_durable.database does not name the current database");

	start_oid			  = tp_resolve_start_function(durable_oid, mode);
	durable_namespace_oid = get_func_namespace(start_oid);
	durable_schema		  = get_namespace_name(durable_namespace_oid);
	if (durable_schema == NULL)
		tp_durable_not_initialized("the pg_durable schema is missing");

	objects->durable_extension_oid	 = durable_oid;
	objects->durable_extension_owner = durable_owner;
	objects->durable_namespace_oid	 = durable_namespace_oid;
	objects->durable_schema			 = pstrdup(durable_schema);
	objects->start_function_oid		 = start_oid;
	objects->start_function			 = tp_qualified_function_name(start_oid);
	objects->explain_function_oid	 = tp_resolve_extension_function(
			   durable_oid, durable_schema, "explain", 1, text_args, mode);
	objects->explain_function = tp_qualified_function_name(
			objects->explain_function_oid);

	objects->signal_function_oid = tp_resolve_extension_function(
			durable_oid, durable_schema, "signal", 3, signal_args, mode);
	objects->signal_function = tp_qualified_function_name(
			objects->signal_function_oid);
	objects->wait_signal_function_oid = tp_resolve_extension_function(
			durable_oid,
			durable_schema,
			"wait_for_signal",
			2,
			wait_signal_args,
			mode);
	objects->wait_signal_function = tp_qualified_function_name(
			objects->wait_signal_function_oid);
	objects->wait_schedule_function_oid = tp_resolve_extension_function(
			durable_oid,
			durable_schema,
			"wait_for_schedule",
			1,
			text_args,
			mode);
	objects->wait_schedule_function = tp_qualified_function_name(
			objects->wait_schedule_function_oid);
	objects->loop_function_oid = tp_resolve_extension_function(
			durable_oid, durable_schema, "loop", 3, loop_args, mode);
	objects->loop_function = tp_qualified_function_name(
			objects->loop_function_oid);
	objects->break_function_oid = tp_resolve_extension_function(
			durable_oid, durable_schema, "break", 1, text_args, mode);
	objects->break_function = tp_qualified_function_name(
			objects->break_function_oid);
	operator_schema_oid = get_extension_schema(durable_oid);
	operator_schema		= get_namespace_name(operator_schema_oid);
	if (operator_schema == NULL)
		tp_durable_not_initialized(
				"the pg_durable operator schema is missing");
	objects->operator_schema		= pstrdup(operator_schema);
	objects->operator_namespace_oid = operator_schema_oid;

	objects->operator_oids[0] = tp_resolve_extension_operator(
			durable_oid, operator_schema, "|=>", mode);
	objects->operator_oids[1] = tp_resolve_extension_operator(
			durable_oid, operator_schema, "~>", mode);
	objects->operator_oids[2] = tp_resolve_extension_operator(
			durable_oid, operator_schema, "?>", mode);
	objects->operator_oids[3] = tp_resolve_extension_operator(
			durable_oid, operator_schema, "!>", mode);
	objects->operator_oids[4] = tp_resolve_extension_operator(
			durable_oid, operator_schema, "|", mode);

	objects->instances_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "instances", mode);
	objects->nodes_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "nodes", mode);
	objects->vars_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "vars", mode);
	objects->instances_relation = quote_qualified_identifier(
			durable_schema, get_rel_name(objects->instances_relation_oid));

	submitted_by_attnum =
			get_attnum(objects->instances_relation_oid, "submitted_by");
	if (submitted_by_attnum == InvalidAttrNumber ||
		get_atttype(objects->instances_relation_oid, submitted_by_attnum) !=
				REGROLEOID)
		tp_durable_not_initialized(
				"df.instances.submitted_by is not pg_catalog.regrole");

	textsearch_oid = get_extension_oid("pg_textsearch", false);
	if (mode == TP_JOB_OBJECTS_LOCKED)
		tp_lock_extension(textsearch_oid);
	textsearch_namespace_oid = get_extension_schema(textsearch_oid);
	textsearch_schema		 = get_namespace_name(textsearch_namespace_oid);
	if (textsearch_schema == NULL)
		elog(ERROR, "pg_textsearch extension schema is missing");

	objects->textsearch_extension_oid	= textsearch_oid;
	objects->textsearch_extension_owner = tp_extension_owner(textsearch_oid);
	objects->textsearch_namespace_oid	= textsearch_namespace_oid;
	objects->textsearch_schema			= pstrdup(textsearch_schema);
	objects->bm25_am_oid				= get_index_am_oid("bm25", false);
	objects->step_function_oid			= tp_resolve_extension_function(
			 textsearch_oid,
			 objects->textsearch_schema,
			 "bm25_compact_step_if_current",
			 5,
			 oid_args,
			 mode);
	objects->current_function_oid = tp_resolve_extension_function(
			textsearch_oid,
			objects->textsearch_schema,
			"bm25_background_target_is_current",
			5,
			oid_args,
			mode);
	objects->step_function = tp_qualified_function_name(
			objects->step_function_oid);
	objects->current_function = tp_qualified_function_name(
			objects->current_function_oid);
}

static void
tp_lookup_job_objects(
		TpCompactionJobObjects *objects, TpJobObjectLookupMode mode)
{
	Oid durable_oid;
	Oid durable_owner = InvalidOid;
	Oid save_userid;
	int save_sec_context;

	durable_oid = get_extension_oid("pg_durable", true);
	if (!OidIsValid(durable_oid) ||
		!tp_extension_lookup(durable_oid, &durable_owner, NULL))
		tp_durable_required();

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			durable_owner, save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		tp_populate_job_objects_as_owner(objects, mode);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
}

static void
tp_preflight_job_objects(TpCompactionJobObjects *objects)
{
	tp_lookup_job_objects(objects, TP_JOB_OBJECTS_PREFLIGHT);
}

static bool
tp_job_object_is_member(Oid class_id, Oid object_id, Oid extension_oid)
{
	return SearchSysCacheExists1(
				   class_id == ProcedureRelationId	? PROCOID
				   : class_id == OperatorRelationId ? OPEROID
													: RELOID,
				   ObjectIdGetDatum(object_id)) &&
		   getExtensionOfObject(class_id, object_id) == extension_oid;
}

static bool
tp_job_objects_still_match(const TpCompactionJobObjects *objects)
{
	const char *function_names[] = {
			"start",
			"explain",
			"signal",
			"wait_for_signal",
			"wait_for_schedule",
			"loop",
			"break",
			"bm25_compact_step_if_current",
			"bm25_background_target_is_current",
	};
	Oid function_oids[] = {
			objects->start_function_oid,
			objects->explain_function_oid,
			objects->signal_function_oid,
			objects->wait_signal_function_oid,
			objects->wait_schedule_function_oid,
			objects->loop_function_oid,
			objects->break_function_oid,
			objects->step_function_oid,
			objects->current_function_oid,
	};
	const char *operator_names[] = {"|=>", "~>", "?>", "!>", "|"};
	const char *relation_names[] = {"instances", "nodes", "vars"};
	Oid			relation_oids[]	 = {
			 objects->instances_relation_oid,
			 objects->nodes_relation_oid,
			 objects->vars_relation_oid,
	 };
	Oid	  durable_oid;
	Oid	  durable_owner;
	Oid	  textsearch_oid;
	Oid	  textsearch_owner;
	char *durable_schema;
	char *operator_schema;
	char *textsearch_schema;

	durable_oid	   = get_extension_oid("pg_durable", true);
	textsearch_oid = get_extension_oid("pg_textsearch", true);
	if (durable_oid != objects->durable_extension_oid ||
		textsearch_oid != objects->textsearch_extension_oid ||
		get_index_am_oid("bm25", true) != objects->bm25_am_oid ||
		!SearchSysCacheExists1(
				AMOID, ObjectIdGetDatum(objects->bm25_am_oid)) ||
		getExtensionOfObject(AccessMethodRelationId, objects->bm25_am_oid) !=
				textsearch_oid ||
		!tp_extension_lookup(durable_oid, &durable_owner, NULL) ||
		durable_owner != objects->durable_extension_owner ||
		!tp_extension_lookup(textsearch_oid, &textsearch_owner, NULL) ||
		textsearch_owner != objects->textsearch_extension_owner ||
		get_extension_schema(textsearch_oid) !=
				objects->textsearch_namespace_oid)
		return false;
	durable_schema	  = get_namespace_name(objects->durable_namespace_oid);
	operator_schema	  = get_namespace_name(objects->operator_namespace_oid);
	textsearch_schema = get_namespace_name(objects->textsearch_namespace_oid);
	if (durable_schema == NULL || operator_schema == NULL ||
		textsearch_schema == NULL ||
		strcmp(durable_schema, objects->durable_schema) != 0 ||
		strcmp(operator_schema, objects->operator_schema) != 0 ||
		strcmp(textsearch_schema, objects->textsearch_schema) != 0)
		return false;

	for (Size i = 0; i < lengthof(function_oids); i++)
	{
		Oid	  extension_oid = i < 7 ? durable_oid : textsearch_oid;
		Oid	  namespace_oid = i < 7 ? objects->durable_namespace_oid
									: objects->textsearch_namespace_oid;
		char *name;

		if (!tp_job_object_is_member(
					ProcedureRelationId, function_oids[i], extension_oid) ||
			get_func_namespace(function_oids[i]) != namespace_oid)
			return false;
		name = get_func_name(function_oids[i]);
		if (name == NULL || strcmp(name, function_names[i]) != 0)
			return false;
	}

	for (Size i = 0; i < lengthof(objects->operator_oids); i++)
	{
		HeapTuple		 tuple;
		Form_pg_operator operator_form;
		char			*name;

		if (!tp_job_object_is_member(
					OperatorRelationId,
					objects->operator_oids[i],
					durable_oid))
			return false;
		tuple = SearchSysCache1(
				OPEROID, ObjectIdGetDatum(objects->operator_oids[i]));
		if (!HeapTupleIsValid(tuple))
			return false;
		operator_form = (Form_pg_operator)GETSTRUCT(tuple);
		if (operator_form->oprnamespace != objects->operator_namespace_oid)
		{
			ReleaseSysCache(tuple);
			return false;
		}
		ReleaseSysCache(tuple);
		name = get_opname(objects->operator_oids[i]);
		if (name == NULL || strcmp(name, operator_names[i]) != 0)
			return false;
	}

	for (Size i = 0; i < lengthof(relation_oids); i++)
	{
		char *name;

		if (!tp_job_object_is_member(
					RelationRelationId, relation_oids[i], durable_oid) ||
			get_rel_namespace(relation_oids[i]) !=
					objects->durable_namespace_oid)
			return false;
		name = get_rel_name(relation_oids[i]);
		if (name == NULL || strcmp(name, relation_names[i]) != 0)
			return false;
	}
	return true;
}

static int
tp_job_object_lock_cmp(const void *left, const void *right)
{
	const TpJobObjectLock *a = left;
	const TpJobObjectLock *b = right;

	if (a->class_id < b->class_id)
		return -1;
	if (a->class_id > b->class_id)
		return 1;
	if (a->object_id < b->object_id)
		return -1;
	if (a->object_id > b->object_id)
		return 1;
	return 0;
}

TpCompactionJobObjects *
tp_compaction_job_try_lock_objects(bool invalid_is_error)
{
	TpCompactionJobObjects	objects;
	TpCompactionJobObjects *result;
	TpJobObjectLock			locks[20];
	MemoryContext			old_context = CurrentMemoryContext;
	ResourceOwner			old_owner	= CurrentResourceOwner;
	int						acquired	= 0;
	int						count		= 0;
	bool					dependency_was_held;

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		MemoryContextSwitchTo(old_context);
		tp_preflight_job_objects(&objects);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
	}
	PG_CATCH();
	{
		ErrorData *edata;

		MemoryContextSwitchTo(old_context);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
		if (invalid_is_error)
			ReThrowError(edata);
		FreeErrorData(edata);
		return NULL;
	}
	PG_END_TRY();

	if (!ConditionalLockDatabaseObject(
				ExtensionRelationId,
				objects.durable_extension_oid,
				0,
				AccessShareLock))
		return NULL;
	if (!ConditionalLockDatabaseObject(
				ExtensionRelationId,
				objects.textsearch_extension_oid,
				0,
				AccessShareLock))
	{
		UnlockDatabaseObject(
				ExtensionRelationId,
				objects.durable_extension_oid,
				0,
				AccessShareLock);
		return NULL;
	}

	dependency_was_held = tp_compaction_dependency_oid_lock_held(
			objects.bm25_am_oid);
	if (!tp_try_lock_compaction_dependency_oid(objects.bm25_am_oid))
		goto unavailable;

#define TP_ADD_JOB_OBJECT_LOCK(classid, objectid, lockmode) \
	do                                                      \
	{                                                       \
		locks[count].class_id  = (classid);                 \
		locks[count].object_id = (objectid);                \
		locks[count].mode	   = (lockmode);                \
		count++;                                            \
	} while (0)

	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId, objects.start_function_oid, AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId,
			objects.explain_function_oid,
			AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId, objects.signal_function_oid, AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId,
			objects.wait_signal_function_oid,
			AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId,
			objects.wait_schedule_function_oid,
			AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId, objects.loop_function_oid, AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId, objects.break_function_oid, AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId, objects.step_function_oid, AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			ProcedureRelationId,
			objects.current_function_oid,
			AccessShareLock);
	for (Size i = 0; i < lengthof(objects.operator_oids); i++)
		TP_ADD_JOB_OBJECT_LOCK(
				OperatorRelationId, objects.operator_oids[i], AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			RelationRelationId,
			objects.instances_relation_oid,
			RowExclusiveLock);
	TP_ADD_JOB_OBJECT_LOCK(
			RelationRelationId, objects.nodes_relation_oid, RowExclusiveLock);
	TP_ADD_JOB_OBJECT_LOCK(
			RelationRelationId, objects.vars_relation_oid, RowExclusiveLock);
	TP_ADD_JOB_OBJECT_LOCK(
			NamespaceRelationId,
			objects.durable_namespace_oid,
			AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			NamespaceRelationId,
			objects.textsearch_namespace_oid,
			AccessShareLock);
	TP_ADD_JOB_OBJECT_LOCK(
			NamespaceRelationId,
			objects.operator_namespace_oid,
			AccessShareLock);

#undef TP_ADD_JOB_OBJECT_LOCK

	qsort(locks, count, sizeof(locks[0]), tp_job_object_lock_cmp);
	for (acquired = 0; acquired < count; acquired++)
	{
		if (locks[acquired].class_id == RelationRelationId)
		{
			if (!ConditionalLockRelationOid(
						locks[acquired].object_id, locks[acquired].mode))
				goto unavailable;
		}
		else if (!ConditionalLockDatabaseObject(
						 locks[acquired].class_id,
						 locks[acquired].object_id,
						 0,
						 locks[acquired].mode))
			goto unavailable;
	}

	if (!tp_job_objects_still_match(&objects))
		goto unavailable;

	result = palloc(sizeof(*result));
	memcpy(result, &objects, sizeof(*result));
	return result;

unavailable:
	while (acquired > 0)
	{
		acquired--;
		if (locks[acquired].class_id == RelationRelationId)
			UnlockRelationOid(locks[acquired].object_id, locks[acquired].mode);
		else
			UnlockDatabaseObject(
					locks[acquired].class_id,
					locks[acquired].object_id,
					0,
					locks[acquired].mode);
	}
	if (!dependency_was_held &&
		tp_compaction_dependency_oid_lock_held(objects.bm25_am_oid))
		tp_unlock_compaction_dependency_oid(objects.bm25_am_oid);
	UnlockDatabaseObject(
			ExtensionRelationId,
			objects.textsearch_extension_oid,
			0,
			AccessShareLock);
	UnlockDatabaseObject(
			ExtensionRelationId,
			objects.durable_extension_oid,
			0,
			AccessShareLock);
	return NULL;
}

static void
tp_require_owner_login(Oid owner_oid)
{
	HeapTuple	   tuple;
	Form_pg_authid role;
	bool		   can_login;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(owner_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index owner role with OID %u does not exist",
						owner_oid)));

	role	  = (Form_pg_authid)GETSTRUCT(tuple);
	can_login = role->rolcanlogin;
	ReleaseSysCache(tuple);

	if (!can_login)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index owner must have LOGIN for background "
						"compaction")));
}

static void
tp_require_owner_database_connect(Oid owner_oid)
{
	if (object_aclcheck(
				DatabaseRelationId, MyDatabaseId, owner_oid, ACL_CONNECT) !=
		ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("index owner cannot connect for background "
						"compaction"),
				 errdetail(
						 "Role \"%s\" lacks CONNECT privilege on database "
						 "\"%s\".",
						 GetUserNameFromId(owner_oid, false),
						 get_database_name(MyDatabaseId))));
}

static void
tp_require_owner_superuser_policy(Oid owner_oid)
{
	HeapTuple	   tuple;
	Form_pg_authid role;
	bool		   is_superuser;
	const char	  *superuser_setting;
	bool		   superuser_enabled;
	char		  *owner_name;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(owner_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index owner role with OID %u does not exist",
						owner_oid)));

	role		 = (Form_pg_authid)GETSTRUCT(tuple);
	is_superuser = role->rolsuper;
	ReleaseSysCache(tuple);

	superuser_setting = GetConfigOption(
			"pg_durable.enable_superuser_instances", true, false);
	if (is_superuser && superuser_setting != NULL &&
		(!parse_bool(superuser_setting, &superuser_enabled) ||
		 !superuser_enabled))
	{
		owner_name = GetUserNameFromId(owner_oid, false);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_durable superuser instances are disabled"),
				 errdetail(
						 "Index owner \"%s\" is a superuser, but "
						 "pg_durable.enable_superuser_instances is off.",
						 owner_name)));
	}
}

static void
tp_owner_privilege_error(
		Oid owner_oid, const char *privilege, const char *object_name)
{
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("index owner lacks required pg_durable privileges"),
			 errdetail(
					 "Role \"%s\" lacks %s privilege on %s.",
					 GetUserNameFromId(owner_oid, false),
					 privilege,
					 object_name),
			 errhint("Grant the role access with df.grant_usage().")));
}

static void
tp_owner_textsearch_schema_privilege_error(
		Oid owner_oid, const char *schema_name)
{
	const char *owner_name = GetUserNameFromId(owner_oid, false);

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("index owner lacks required pg_textsearch schema "
					"privilege"),
			 errdetail(
					 "Role \"%s\" lacks USAGE privilege on schema %s.",
					 owner_name,
					 quote_identifier(schema_name)),
			 errhint("Grant access with GRANT USAGE ON SCHEMA %s TO %s.",
					 quote_identifier(schema_name),
					 quote_identifier(owner_name))));
}

static bool
tp_owner_has_column_privilege(
		Oid			relation_oid,
		Oid			owner_oid,
		AclMode		privilege,
		const char *column_name)
{
	AttrNumber attnum;

	if (pg_class_aclcheck(relation_oid, owner_oid, privilege) == ACLCHECK_OK)
		return true;

	attnum = get_attnum(relation_oid, column_name);
	if (attnum == InvalidAttrNumber)
		tp_durable_not_initialized(psprintf(
				"required column %s.%s is missing",
				get_rel_name(relation_oid),
				column_name));

	return pg_attribute_aclcheck(relation_oid, attnum, owner_oid, privilege) ==
		   ACLCHECK_OK;
}

static void
tp_require_owner_column_privileges(
		Oid				   relation_oid,
		Oid				   owner_oid,
		AclMode			   privilege,
		const char		  *privilege_name,
		const char *const *columns,
		Size			   ncolumns)
{
	for (Size i = 0; i < ncolumns; i++)
	{
		if (!tp_owner_has_column_privilege(
					relation_oid, owner_oid, privilege, columns[i]))
			tp_owner_privilege_error(
					owner_oid,
					privilege_name,
					quote_qualified_identifier(
							"df", get_rel_name(relation_oid)));
	}
}

static void
tp_require_owner_function_privilege(
		Oid owner_oid, Oid function_oid, const char *function_name)
{
	if (object_aclcheck(
				ProcedureRelationId, function_oid, owner_oid, ACL_EXECUTE) !=
		ACLCHECK_OK)
		tp_owner_privilege_error(owner_oid, "EXECUTE", function_name);
}

static void
tp_require_owner_durable_privileges(
		const TpCompactionJobObjects *objects, Oid owner_oid)
{
	static const char *const instance_select_columns[] =
			{"id", "label", "status", "submitted_by", "created_at"};
	static const char *const instance_insert_columns[] =
			{"id", "label", "root_node", "submitted_by", "database"};
	static const char *const node_insert_columns[] =
			{"id",
			 "instance_id",
			 "node_type",
			 "query",
			 "result_name",
			 "left_node",
			 "right_node",
			 "submitted_by",
			 "database"};
	static const char *const vars_select_columns[] =
			{"name", "value", "owner"};

	if (object_aclcheck(
				NamespaceRelationId,
				objects->textsearch_namespace_oid,
				owner_oid,
				ACL_USAGE) != ACLCHECK_OK)
		tp_owner_textsearch_schema_privilege_error(
				owner_oid, objects->textsearch_schema);

	if (object_aclcheck(
				NamespaceRelationId,
				objects->durable_namespace_oid,
				owner_oid,
				ACL_USAGE) != ACLCHECK_OK)
		tp_owner_privilege_error(owner_oid, "USAGE", "schema df");

	tp_require_owner_function_privilege(
			owner_oid, objects->start_function_oid, objects->start_function);
	tp_require_owner_function_privilege(
			owner_oid,
			objects->explain_function_oid,
			objects->explain_function);
	tp_require_owner_function_privilege(
			owner_oid, objects->signal_function_oid, objects->signal_function);
	tp_require_owner_column_privileges(
			objects->instances_relation_oid,
			owner_oid,
			ACL_SELECT,
			"SELECT",
			instance_select_columns,
			lengthof(instance_select_columns));
	tp_require_owner_column_privileges(
			objects->instances_relation_oid,
			owner_oid,
			ACL_INSERT,
			"INSERT",
			instance_insert_columns,
			lengthof(instance_insert_columns));
	tp_require_owner_column_privileges(
			objects->nodes_relation_oid,
			owner_oid,
			ACL_INSERT,
			"INSERT",
			node_insert_columns,
			lengthof(node_insert_columns));
	tp_require_owner_column_privileges(
			objects->vars_relation_oid,
			owner_oid,
			ACL_SELECT,
			"SELECT",
			vars_select_columns,
			lengthof(vars_select_columns));
}

static char *
tp_build_history_prefix(const TpCompactionJobTarget *target)
{
	return psprintf(TP_JOB_LABEL_PREFIX "%u:", target->database_oid);
}

static char *
tp_build_family_prefix(const TpCompactionJobTarget *target)
{
	return psprintf(
			TP_JOB_LABEL_PREFIX "%u:%u:%u:%u:",
			target->database_oid,
			target->index_oid,
			target->tablespace_oid,
			(Oid)target->relfilenumber);
}

static char *
tp_hex_encode(const char *value)
{
	static const char digits[] = "0123456789abcdef";
	Size			  length   = strlen(value);
	char			 *encoded;

	if (length > (MaxAllocSize - 1) / 2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("background compaction schedule is too long")));

	encoded = palloc(length * 2 + 1);
	for (Size i = 0; i < length; i++)
	{
		unsigned char byte = (unsigned char)value[i];

		encoded[i * 2]	   = digits[byte >> 4];
		encoded[i * 2 + 1] = digits[byte & 0x0f];
	}
	encoded[length * 2] = '\0';
	return encoded;
}

static int
tp_hex_value(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	return -1;
}

static char *
tp_build_label(const TpCompactionJobTarget *target, const char *schedule)
{
	char *encoded = tp_hex_encode(schedule);
	char *label	  = psprintf(
			  "%s%u:%u:%s:%s",
			  target->family_prefix,
			  target->owner_oid,
			  target->heap_oid,
			  target->lineage,
			  encoded);

	pfree(encoded);
	return label;
}

static char *
tp_decode_schedule(const char *encoded, MemoryContext result_context)
{
	Size		  encoded_length = strlen(encoded);
	char		 *schedule;
	MemoryContext old_context;

	if ((encoded_length & 1) != 0)
		return NULL;

	old_context = MemoryContextSwitchTo(result_context);
	schedule	= palloc(encoded_length / 2 + 1);
	MemoryContextSwitchTo(old_context);

	for (Size i = 0; i < encoded_length; i += 2)
	{
		int high = tp_hex_value(encoded[i]);
		int low	 = tp_hex_value(encoded[i + 1]);

		if (high < 0 || low < 0 || (high == 0 && low == 0))
		{
			pfree(schedule);
			return NULL;
		}
		schedule[i / 2] = (char)((high << 4) | low);
	}
	schedule[encoded_length / 2] = '\0';
	return schedule;
}

static char *
tp_schedule_from_label(
		const TpCompactionJobTarget *target,
		const char					*label,
		MemoryContext				 result_context)
{
	char *owner_prefix =
			psprintf("%s%u:", target->family_prefix, target->owner_oid);
	Size		 prefix_length = strlen(owner_prefix);
	unsigned int heap_oid;
	char		 lineage[TP_COMPACTION_LINEAGE_LENGTH + 1];
	int			 consumed = 0;

	if (strncmp(label, owner_prefix, prefix_length) != 0)
	{
		pfree(owner_prefix);
		return NULL;
	}
	pfree(owner_prefix);

	if (sscanf(label + prefix_length,
			   "%u:%32[0-9a-f]:%n",
			   &heap_oid,
			   lineage,
			   &consumed) != 2 ||
		consumed <= 0 || heap_oid != target->heap_oid ||
		strcmp(lineage, target->lineage) != 0)
		return NULL;

	return tp_decode_schedule(
			label + prefix_length + consumed, result_context);
}

static char *
tp_schedule_from_legacy_label(
		const TpCompactionJobTarget *target,
		const char					*label,
		MemoryContext				 result_context)
{
	char *owner_prefix =
			psprintf("%s%u:", target->family_prefix, target->owner_oid);
	Size		prefix_length = strlen(owner_prefix);
	const char *encoded;

	if (strncmp(label, owner_prefix, prefix_length) != 0)
	{
		pfree(owner_prefix);
		return NULL;
	}
	pfree(owner_prefix);

	encoded = label + prefix_length;
	if (strchr(encoded, ':') != NULL)
		return NULL;
	return tp_decode_schedule(encoded, result_context);
}

static void
tp_capture_target(
		Oid indexoid, bool refresh_default, TpCompactionJobTarget *target)
{
	Relation	index_rel;
	const char *schedule;
	char	   *lineage;
	bool		lineage_backfilled;

	memset(target, 0, sizeof(*target));
	lineage =
			tp_ensure_index_compaction_lineage(indexoid, &lineage_backfilled);
	index_rel = try_relation_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation with OID %u does not exist", indexoid)));

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX)
	{
		char *index_name = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a physical bm25 index", index_name)));
	}

	if (RelationUsesLocalBuffers(index_rel))
	{
		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background compaction is not supported for "
						"temporary indexes")));
	}

	if (index_rel->rd_index == NULL || !index_rel->rd_index->indisvalid ||
		!index_rel->rd_index->indisready || !index_rel->rd_index->indislive)
	{
		char *index_name = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not ready for background "
						"compaction",
						index_name)));
	}

	if (tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND)
	{
		char *index_name = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not configured for background "
						"compaction",
						index_name)));
	}

	target->index_oid	   = indexoid;
	target->database_oid   = MyDatabaseId;
	target->tablespace_oid = index_rel->rd_locator.spcOid;
	target->relfilenumber  = index_rel->rd_locator.relNumber;
	target->owner_oid	   = index_rel->rd_rel->relowner;
	target->heap_oid	   = index_rel->rd_index->indrelid;
	target->index_name	   = pstrdup(RelationGetRelationName(index_rel));
	if (lineage == NULL)
	{
		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" has no background compaction lineage",
						target->index_name),
				 errhint("Recreate the index before using managed background "
						 "compaction.")));
	}
	target->lineage			   = lineage;
	target->lineage_backfilled = lineage_backfilled;

	if (refresh_default || lineage_backfilled)
	{
		schedule = tp_index_compaction_schedule(index_rel);
		if (schedule == NULL)
			schedule = tp_background_compaction_schedule;
		if (schedule == NULL)
			elog(ERROR, "background compaction schedule is not initialized");
		target->schedule = pstrdup(schedule);
	}

	/*
	 * Keep the relation lock until transaction end.  The workflow identity
	 * and dependency are admitted against this physical generation.
	 */
	relation_close(index_rel, NoLock);
	target->history_prefix = tp_build_history_prefix(target);
	target->family_prefix  = tp_build_family_prefix(target);
}

static bool
tp_dependency_exists(
		const ObjectAddress *dependent, const ObjectAddress *referenced)
{
	Relation	depend_rel;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;
	bool		found = false;

	depend_rel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(
			&keys[0],
			Anum_pg_depend_classid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(dependent->classId));
	ScanKeyInit(
			&keys[1],
			Anum_pg_depend_objid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(dependent->objectId));
	ScanKeyInit(
			&keys[2],
			Anum_pg_depend_objsubid,
			BTEqualStrategyNumber,
			F_INT4EQ,
			Int32GetDatum(dependent->objectSubId));

	scan = systable_beginscan(
			depend_rel, DependDependerIndexId, true, NULL, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dependency = (Form_pg_depend)GETSTRUCT(tuple);

		if (dependency->refclassid == referenced->classId &&
			dependency->refobjid == referenced->objectId &&
			dependency->refobjsubid == referenced->objectSubId &&
			dependency->deptype == DEPENDENCY_NORMAL)
		{
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depend_rel, AccessShareLock);
	return found;
}

static void
tp_pin_durable_dependency(const TpCompactionJobObjects *objects)
{
	ObjectAddress bm25_am = {
			.classId	 = AccessMethodRelationId,
			.objectId	 = objects->bm25_am_oid,
			.objectSubId = 0,
	};
	ObjectAddress durable_ext = {
			.classId	 = ExtensionRelationId,
			.objectId	 = objects->durable_extension_oid,
			.objectSubId = 0,
	};

	if (!tp_dependency_exists(&bm25_am, &durable_ext))
	{
		recordDependencyOn(&bm25_am, &durable_ext, DEPENDENCY_NORMAL);
		CommandCounterIncrement();
	}
}

static void
tp_grant_helper_access(const TpCompactionJobObjects *objects, Oid owner_oid)
{
	AclResult	   step_acl;
	AclResult	   current_acl;
	Oid			   save_userid;
	int			   save_sec_context;
	int			   save_nestlevel;
	bool		   spi_connected = false;
	StringInfoData sql;
	const char	  *owner_name;
	const char	  *quoted_owner;

	step_acl = object_aclcheck(
			ProcedureRelationId,
			objects->step_function_oid,
			owner_oid,
			ACL_EXECUTE);
	current_acl = object_aclcheck(
			ProcedureRelationId,
			objects->current_function_oid,
			owner_oid,
			ACL_EXECUTE);
	if (step_acl == ACLCHECK_OK && current_acl == ACLCHECK_OK)
		return;

	/*
	 * pg_durable executes SQL nodes by logging in as submitted_by.  PUBLIC
	 * remains revoked from the physical-target helpers, so grant only this
	 * index owner.  The helpers still enforce ownership and the complete
	 * captured physical identity on every call.
	 */
	owner_name	 = GetUserNameFromId(owner_oid, false);
	quoted_owner = quote_identifier(owner_name);
	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"GRANT EXECUTE ON FUNCTION "
			"%s(pg_catalog.oid, pg_catalog.oid, pg_catalog.oid, "
			"pg_catalog.oid, pg_catalog.oid), "
			"%s(pg_catalog.oid, pg_catalog.oid, pg_catalog.oid, "
			"pg_catalog.oid, pg_catalog.oid) TO %s",
			objects->step_function,
			objects->current_function,
			quoted_owner);

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	save_nestlevel = tp_set_safe_elevated_gucs();
	SetUserIdAndSecContext(
			objects->textsearch_extension_owner,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;
		if (SPI_execute(sql.data, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not grant managed compaction helper access");
		SPI_finish();
		spi_connected = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		AtEOXact_GUC(false, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	pfree(sql.data);
}

static char *
tp_copy_spi_text(
		HeapTuple	  tuple,
		TupleDesc	  tuple_desc,
		int			  column,
		MemoryContext context)
{
	bool		  isnull;
	Datum		  value;
	char		 *temporary;
	char		 *copy;
	MemoryContext old_context;

	value = SPI_getbinval(tuple, tuple_desc, column, &isnull);
	if (isnull)
		return NULL;
	temporary	= TextDatumGetCString(value);
	old_context = MemoryContextSwitchTo(context);
	copy		= pstrdup(temporary);
	MemoryContextSwitchTo(old_context);
	pfree(temporary);
	return copy;
}

static char *
tp_find_exact_instance(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		const char					 *label,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	Oid			   argtypes[2] = {TEXTOID, OIDOID};
	Datum		   values[2] =
			{CStringGetTextDatum(label), ObjectIdGetDatum(target->owner_oid)};
	char *instance_id = NULL;
	int	  rc;

	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.id::pg_catalog.text "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.=) $1 "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) $2 "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY['pending', 'running']::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC",
			objects->instances_relation);
	rc = SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 0);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT)
		elog(ERROR, "could not search pg_durable instances");

	if (SPI_processed > 0)
		instance_id = tp_copy_spi_text(
				SPI_tuptable->vals[0],
				SPI_tuptable->tupdesc,
				1,
				result_context);
	if (SPI_processed > 1)
		ereport(WARNING,
				(errmsg("multiple active pg_textsearch background "
						"compaction jobs share one canonical label"),
				 errdetail("The newest job was selected deterministically.")));

	return instance_id;
}

static char *
tp_find_family_instance(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		bool						  terminal,
		char						**schedule,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	Oid			   argtypes[2] = {TEXTOID, OIDOID};
	Datum		   values[2] =
			{CStringGetTextDatum(target->family_prefix),
			 ObjectIdGetDatum(target->owner_oid)};
	char  *instance_id	 = NULL;
	uint64 managed_count = 0;
	int	   rc;

	*schedule = NULL;
	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.id::pg_catalog.text, instance.label "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.~~) "
			"($1 OPERATOR(pg_catalog.||) '%%') "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) $2 "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY[%s]::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC",
			objects->instances_relation,
			terminal ? "'completed', 'failed', 'cancelled'"
					 : "'pending', 'running'");
	rc = SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 0);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT)
		elog(ERROR, "could not search pg_durable instance history");

	for (uint64 i = 0; i < SPI_processed; i++)
	{
		char *label;
		char *decoded_schedule;

		label = tp_copy_spi_text(
				SPI_tuptable->vals[i],
				SPI_tuptable->tupdesc,
				2,
				CurrentMemoryContext);
		decoded_schedule =
				tp_schedule_from_label(target, label, result_context);
		pfree(label);
		if (decoded_schedule == NULL)
			continue;

		managed_count++;
		if (instance_id == NULL)
		{
			instance_id = tp_copy_spi_text(
					SPI_tuptable->vals[i],
					SPI_tuptable->tupdesc,
					1,
					result_context);
			*schedule = decoded_schedule;
		}
		else
			pfree(decoded_schedule);
	}

	if (!terminal && managed_count > 1)
		ereport(WARNING,
				(errmsg("multiple active pg_textsearch background "
						"compaction jobs exist for one physical index"),
				 errdetail("The newest job was selected deterministically.")));

	return instance_id;
}

static char *
tp_find_current_family_instance(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	Oid			   argtypes[2] = {TEXTOID, OIDOID};
	Datum		   values[2] =
			{CStringGetTextDatum(target->family_prefix),
			 ObjectIdGetDatum(target->owner_oid)};
	char *instance_id = NULL;
	int	  rc;

	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.id::pg_catalog.text "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.~~) "
			"($1 OPERATOR(pg_catalog.||) '%%') "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) $2 "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY['pending', 'running']::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC "
			"LIMIT 1",
			objects->instances_relation);
	rc = SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 1);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT)
		elog(ERROR, "could not select current pg_durable instance");

	if (SPI_processed > 0)
		instance_id = tp_copy_spi_text(
				SPI_tuptable->vals[0],
				SPI_tuptable->tupdesc,
				1,
				result_context);
	return instance_id;
}

static char *
tp_find_legacy_family_instance(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		bool						  terminal,
		char						**schedule,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	Oid			   argtypes[2] = {TEXTOID, OIDOID};
	Datum		   values[2] =
			{CStringGetTextDatum(target->family_prefix),
			 ObjectIdGetDatum(target->owner_oid)};
	char *instance_id = NULL;
	int	  rc;

	*schedule = NULL;
	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.id::pg_catalog.text, instance.label "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.~~) "
			"($1 OPERATOR(pg_catalog.||) '%%') "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) $2 "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY[%s]::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC",
			objects->instances_relation,
			terminal ? "'completed', 'failed', 'cancelled'"
					 : "'pending', 'running'");
	rc = SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 0);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT)
		elog(ERROR, "could not search legacy pg_durable instance history");

	for (uint64 i = 0; i < SPI_processed; i++)
	{
		char *label;
		char *decoded_schedule;

		label = tp_copy_spi_text(
				SPI_tuptable->vals[i],
				SPI_tuptable->tupdesc,
				2,
				CurrentMemoryContext);
		decoded_schedule =
				tp_schedule_from_legacy_label(target, label, result_context);
		pfree(label);
		if (decoded_schedule == NULL)
			continue;

		instance_id = tp_copy_spi_text(
				SPI_tuptable->vals[i],
				SPI_tuptable->tupdesc,
				1,
				result_context);
		*schedule = decoded_schedule;
		break;
	}

	return instance_id;
}

static char *
tp_schedule_from_prior_label(
		const TpCompactionJobTarget *target,
		const char					*label,
		MemoryContext				 result_context)
{
	const char	 *suffix;
	const char	 *encoded;
	char		  lineage[TP_COMPACTION_LINEAGE_LENGTH + 1];
	unsigned int  index_oid;
	unsigned int  tablespace_oid;
	unsigned int  relfilenumber;
	unsigned int  owner_oid;
	unsigned int  heap_oid;
	int			  consumed = 0;
	Size		  encoded_length;
	char		 *schedule;
	MemoryContext old_context;

	if (strncmp(label,
				target->history_prefix,
				strlen(target->history_prefix)) != 0)
		return NULL;

	suffix = label + strlen(target->history_prefix);
	if (sscanf(suffix,
			   "%u:%u:%u:%u:%u:%32[0-9a-f]:%n",
			   &index_oid,
			   &tablespace_oid,
			   &relfilenumber,
			   &owner_oid,
			   &heap_oid,
			   lineage,
			   &consumed) != 6 ||
		consumed <= 0 || owner_oid != target->owner_oid ||
		heap_oid != target->heap_oid || strcmp(lineage, target->lineage) != 0)
		return NULL;

	if (index_oid == target->index_oid &&
		tablespace_oid == target->tablespace_oid &&
		relfilenumber == (unsigned int)target->relfilenumber)
		return NULL;

	encoded		   = suffix + consumed;
	encoded_length = strlen(encoded);
	if ((encoded_length & 1) != 0)
		return NULL;

	old_context = MemoryContextSwitchTo(result_context);
	schedule	= palloc(encoded_length / 2 + 1);
	MemoryContextSwitchTo(old_context);

	for (Size i = 0; i < encoded_length; i += 2)
	{
		int high = tp_hex_value(encoded[i]);
		int low	 = tp_hex_value(encoded[i + 1]);

		if (high < 0 || low < 0 || (high == 0 && low == 0))
		{
			pfree(schedule);
			return NULL;
		}
		schedule[i / 2] = (char)((high << 4) | low);
	}
	schedule[encoded_length / 2] = '\0';
	return schedule;
}

static char *
tp_find_prior_generation_schedule(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	Oid			   argtypes[2] = {TEXTOID, OIDOID};
	Datum		   values[2] =
			{CStringGetTextDatum(target->history_prefix),
			 ObjectIdGetDatum(target->owner_oid)};
	char *schedule = NULL;
	int	  rc;

	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT instance.label "
			"FROM %s AS instance "
			"WHERE instance.label OPERATOR(pg_catalog.~~) "
			"($1 OPERATOR(pg_catalog.||) '%%') "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) $2 "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY['pending', 'running', 'completed', 'failed', "
			"'cancelled']::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC",
			objects->instances_relation);
	rc = SPI_execute_with_args(sql.data, 2, argtypes, values, NULL, true, 0);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT)
		elog(ERROR, "could not search prior pg_durable instance history");

	for (uint64 i = 0; i < SPI_processed; i++)
	{
		char *label = tp_copy_spi_text(
				SPI_tuptable->vals[i],
				SPI_tuptable->tupdesc,
				1,
				CurrentMemoryContext);

		schedule = tp_schedule_from_prior_label(target, label, result_context);
		pfree(label);
		if (schedule != NULL)
			break;
	}

	return schedule;
}

static void
tp_build_worker_queries(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		char						**step_sql,
		char						**current_sql)
{
	char *family_literal = quote_literal_cstr(target->family_prefix);
	char *step_signature;
	char *step_signature_literal;
	char *current_signature;
	char *current_signature_literal;

	step_signature = psprintf(
			"%s(pg_catalog.oid,pg_catalog.oid,pg_catalog.oid,"
			"pg_catalog.oid,pg_catalog.oid)",
			objects->step_function);
	step_signature_literal = quote_literal_cstr(step_signature);
	current_signature	   = psprintf(
			 "%s(pg_catalog.oid,pg_catalog.oid,pg_catalog.oid,"
				 "pg_catalog.oid,pg_catalog.oid)",
			 objects->current_function);
	current_signature_literal = quote_literal_cstr(current_signature);

	*step_sql = psprintf(
			"SELECT coalesce((WITH helper_args("
			"index_oid, database_oid, tablespace_oid, relfilenumber, "
			"owner_oid) AS MATERIALIZED (SELECT %u::pg_catalog.oid, "
			"%u::pg_catalog.oid, %u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid WHERE "
			"pg_catalog.to_regprocedure(%s)::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) %u::pg_catalog.oid) SELECT "
			"%s(helper_args.index_oid, helper_args.database_oid, "
			"helper_args.tablespace_oid, helper_args.relfilenumber, "
			"helper_args.owner_oid) FROM helper_args), false) AS ran",
			target->index_oid,
			target->database_oid,
			target->tablespace_oid,
			(Oid)target->relfilenumber,
			target->owner_oid,
			step_signature_literal,
			objects->step_function_oid,
			objects->step_function);

	*current_sql = psprintf(
			"SELECT (coalesce((WITH helper_args("
			"index_oid, database_oid, tablespace_oid, relfilenumber, "
			"owner_oid) AS MATERIALIZED (SELECT %u::pg_catalog.oid, "
			"%u::pg_catalog.oid, %u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid WHERE "
			"pg_catalog.to_regprocedure(%s)::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) %u::pg_catalog.oid) SELECT "
			"%s(helper_args.index_oid, helper_args.database_oid, "
			"helper_args.tablespace_oid, helper_args.relfilenumber, "
			"helper_args.owner_oid) FROM helper_args), false) "
			"AND coalesce(("
			"SELECT instance.id OPERATOR(pg_catalog.=) "
			"'{sys_instance_id}' "
			"FROM %s AS instance "
			"WHERE pg_catalog.left(instance.label, %zu) "
			"OPERATOR(pg_catalog.=) %s "
			"AND instance.submitted_by::pg_catalog.oid "
			"OPERATOR(pg_catalog.=) %u::pg_catalog.oid "
			"AND instance.status OPERATOR(pg_catalog.=) "
			"ANY (ARRAY['pending', 'running']::pg_catalog.text[]) "
			"ORDER BY instance.created_at DESC, instance.id DESC "
			"LIMIT 1), false)) AS current",
			target->index_oid,
			target->database_oid,
			target->tablespace_oid,
			(Oid)target->relfilenumber,
			target->owner_oid,
			current_signature_literal,
			objects->current_function_oid,
			objects->current_function,
			objects->instances_relation,
			strlen(target->family_prefix),
			family_literal,
			target->owner_oid);
	pfree(family_literal);
	pfree(step_signature);
	pfree(step_signature_literal);
	pfree(current_signature);
	pfree(current_signature_literal);
}

static void
tp_append_guard(StringInfo sql, const TpCompactionJobObjects *objects)
{
	appendStringInfo(
			sql,
			"(($2::pg_catalog.text OPERATOR(%s.|=>) 'current') "
			"OPERATOR(%s.~>) "
			"('SELECT $current.current' OPERATOR(%s.?>) 'SELECT true' "
			"OPERATOR(%s.!>) %s('stale'::pg_catalog.text)))",
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			objects->break_function);
}

static void
tp_append_cascade(StringInfo sql, const TpCompactionJobObjects *objects)
{
	appendStringInfo(
			sql,
			"%s((($1::pg_catalog.text OPERATOR(%s.|=>) 'step') "
			"OPERATOR(%s.~>) "
			"('SELECT $step.ran' OPERATOR(%s.?>) 'SELECT true' "
			"OPERATOR(%s.!>) %s('false'::pg_catalog.text))), "
			"NULL::pg_catalog.text, false)",
			objects->loop_function,
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			quote_identifier(objects->operator_schema),
			objects->break_function);
}

static void
tp_append_job_graph(StringInfo sql, const TpCompactionJobObjects *objects)
{
	const char *operator_schema = quote_identifier(objects->operator_schema);

	appendStringInfo(sql, "%s((", objects->loop_function);
	tp_append_guard(sql, objects);
	appendStringInfo(sql, " OPERATOR(%s.~>) ", operator_schema);
	tp_append_cascade(sql, objects);
	appendStringInfo(
			sql,
			" OPERATOR(%s.~>) %s(("
			"%s('compact'::pg_catalog.text, NULL::pg_catalog.int4) "
			"OPERATOR(%s.|) "
			"%s($3::pg_catalog.text) "
			"OPERATOR(%s.~>) ",
			operator_schema,
			objects->loop_function,
			objects->wait_signal_function,
			operator_schema,
			objects->wait_schedule_function,
			operator_schema);
	tp_append_guard(sql, objects);
	appendStringInfo(sql, " OPERATOR(%s.~>) ", operator_schema);
	tp_append_cascade(sql, objects);
	appendStringInfo(
			sql,
			"), NULL::pg_catalog.text, true) "
			"OPERATOR(%s.~>) %s('stale'::pg_catalog.text)), "
			"NULL::pg_catalog.text, true)",
			operator_schema,
			objects->break_function);
}

static char *
tp_start_job(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		const char					 *schedule,
		const char					 *label,
		MemoryContext				  result_context)
{
	StringInfoData sql;
	char		  *step_sql;
	char		  *current_sql;
	Oid			   argtypes[4] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID};
	Datum		   values[4];
	char		  *instance_id;
	int			   rc;

	tp_build_worker_queries(objects, target, &step_sql, &current_sql);

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT %s(", objects->start_function);
	tp_append_job_graph(&sql, objects);
	appendStringInfo(
			&sql,
			", $4::pg_catalog.text, pg_catalog.current_database(), "
			"'caller'::pg_catalog.text)");

	values[0] = CStringGetTextDatum(step_sql);
	values[1] = CStringGetTextDatum(current_sql);
	values[2] = CStringGetTextDatum(schedule);
	values[3] = CStringGetTextDatum(label);
	rc = SPI_execute_with_args(sql.data, 4, argtypes, values, NULL, false, 1);
	if (rc != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "could not start pg_durable compaction workflow");
	instance_id = tp_copy_spi_text(
			SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, result_context);
	if (instance_id == NULL)
		elog(ERROR, "pg_durable returned no compaction workflow identifier");
	{
		Oid	  save_userid;
		int	  save_sec_context;
		Oid	  stamp_argtypes[1] = {TEXTOID};
		Datum stamp_values[1]	= {CStringGetTextDatum(instance_id)};

		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(
				objects->durable_extension_owner,
				save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
		PG_TRY();
		{
			resetStringInfo(&sql);
			appendStringInfo(
					&sql,
					"UPDATE %s SET created_at = "
					"pg_catalog.statement_timestamp(), updated_at = "
					"pg_catalog.statement_timestamp() "
					"WHERE id OPERATOR(pg_catalog.=) $1",
					objects->instances_relation);
			rc = SPI_execute_with_args(
					sql.data, 1, stamp_argtypes, stamp_values, NULL, false, 0);
			if (rc != SPI_OK_UPDATE || SPI_processed != 1)
				elog(ERROR,
					 "could not timestamp pg_durable compaction workflow");
		}
		PG_FINALLY();
		{
			SetUserIdAndSecContext(save_userid, save_sec_context);
		}
		PG_END_TRY();
	}

	pfree(sql.data);
	pfree(step_sql);
	pfree(current_sql);
	return instance_id;
}

static void
tp_validate_graph_as_owner(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target)
{
	static const char *failure_prefixes[] = {
			"Cannot explain input",
			"Expression returned NULL",
			"Failed to evaluate expression",
			"Failed to parse Durofut JSON",
			"Invalid durable function graph",
	};
	MemoryContext  result_context = CurrentMemoryContext;
	Oid			   save_userid;
	int			   save_sec_context;
	int			   save_nestlevel;
	bool		   spi_connected = false;
	StringInfoData sql;
	char		  *step_sql;
	char		  *current_sql;
	char		  *explanation = NULL;
	Oid			   argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		   values[3];
	int			   rc;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	save_nestlevel = tp_set_safe_elevated_gucs();
	SetUserIdAndSecContext(
			target->owner_oid,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;

		tp_build_worker_queries(objects, target, &step_sql, &current_sql);
		initStringInfo(&sql);
		appendStringInfo(&sql, "SELECT %s(", objects->explain_function);
		tp_append_job_graph(&sql, objects);
		appendStringInfoChar(&sql, ')');

		values[0] = CStringGetTextDatum(step_sql);
		values[1] = CStringGetTextDatum(current_sql);
		values[2] = CStringGetTextDatum(target->schedule);
		rc		  = SPI_execute_with_args(
				   sql.data, 3, argtypes, values, NULL, true, 1);
		if (rc != SPI_OK_SELECT || SPI_processed != 1)
			elog(ERROR, "could not validate pg_durable compaction workflow");
		explanation = tp_copy_spi_text(
				SPI_tuptable->vals[0],
				SPI_tuptable->tupdesc,
				1,
				result_context);

		pfree(sql.data);
		pfree(step_sql);
		pfree(current_sql);

		SPI_finish();
		spi_connected = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		AtEOXact_GUC(false, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	if (explanation == NULL)
		elog(ERROR, "pg_durable returned no workflow validation result");

	for (Size i = 0; i < lengthof(failure_prefixes); i++)
	{
		if (strncmp(explanation,
					failure_prefixes[i],
					strlen(failure_prefixes[i])) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_durable rejected the background compaction "
							"workflow"),
					 errdetail("%s", explanation)));
	}
	pfree(explanation);
}

static char *
tp_reconcile_job(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		bool						  refresh_default,
		MemoryContext				  result_context)
{
	char *instance_id;
	char *current_instance_id = NULL;
	char *schedule			  = NULL;
	char *label				  = NULL;
	bool  publish_new		  = false;

	if (refresh_default)
	{
		label = tp_build_label(target, target->schedule);
		instance_id =
				tp_find_exact_instance(objects, target, label, result_context);
		current_instance_id = tp_find_current_family_instance(
				objects, target, result_context);
		if (instance_id == NULL || current_instance_id == NULL ||
			strcmp(instance_id, current_instance_id) != 0)
		{
			if (instance_id != NULL)
				pfree(instance_id);
			instance_id = tp_start_job(
					objects, target, target->schedule, label, result_context);
		}
		if (current_instance_id != NULL)
			pfree(current_instance_id);
		pfree(label);
		return instance_id;
	}

	instance_id = tp_find_family_instance(
			objects, target, false, &schedule, result_context);
	if (instance_id != NULL)
	{
		current_instance_id = tp_find_current_family_instance(
				objects, target, result_context);
		if (current_instance_id != NULL &&
			strcmp(instance_id, current_instance_id) == 0)
		{
			pfree(current_instance_id);
			pfree(schedule);
			return instance_id;
		}
		if (current_instance_id != NULL)
			pfree(current_instance_id);
		pfree(instance_id);
		instance_id = NULL;
		publish_new = true;
	}
	else
	{
		instance_id = tp_find_family_instance(
				objects, target, true, &schedule, result_context);
		if (instance_id == NULL)
		{
			if (target->lineage_backfilled)
			{
				instance_id = tp_find_legacy_family_instance(
						objects, target, false, &schedule, result_context);
				if (instance_id == NULL)
					instance_id = tp_find_legacy_family_instance(
							objects, target, true, &schedule, result_context);
			}

			if (instance_id != NULL)
				pfree(instance_id);
			if (schedule == NULL)
				schedule = tp_find_prior_generation_schedule(
						objects, target, result_context);
			if (schedule == NULL && target->lineage_backfilled)
				schedule =
						MemoryContextStrdup(result_context, target->schedule);
			if (schedule == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("background compaction for index \"%s\" "
								"requires explicit adoption",
								target->index_name)));
		}
		else
			pfree(instance_id);
	}

	label = tp_build_label(target, schedule);
	if (publish_new)
		instance_id =
				tp_start_job(objects, target, schedule, label, result_context);
	else
	{
		instance_id =
				tp_find_exact_instance(objects, target, label, result_context);
		if (instance_id == NULL)
			instance_id = tp_start_job(
					objects, target, schedule, label, result_context);
	}
	pfree(label);
	pfree(schedule);
	return instance_id;
}

static void
tp_signal_instance(
		const TpCompactionJobObjects *objects, const char *instance_id)
{
	StringInfoData sql;
	Oid			   argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		   values[3] =
			{CStringGetTextDatum(instance_id),
			 CStringGetTextDatum("compact"),
			 CStringGetTextDatum("{}")};
	int rc;

	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT %s($1::pg_catalog.text, $2::pg_catalog.text, "
			"$3::pg_catalog.text)",
			objects->signal_function);
	rc = SPI_execute_with_args(sql.data, 3, argtypes, values, NULL, false, 1);
	pfree(sql.data);
	if (rc != SPI_OK_SELECT || SPI_processed != 1)
		elog(ERROR, "could not signal pg_durable compaction workflow");
}

static char *
tp_reconcile_as_owner(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		bool						  refresh_default,
		bool						  signal,
		MemoryContext				  result_context)
{
	Oid	  save_userid;
	int	  save_sec_context;
	int	  save_nestlevel;
	bool  spi_connected	  = false;
	bool  snapshot_pushed = false;
	char *instance_id	  = NULL;

	save_nestlevel = tp_set_safe_elevated_gucs();
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			target->owner_oid,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		PushActiveSnapshot(GetLatestSnapshot());
		snapshot_pushed = true;
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;
		instance_id	  = tp_reconcile_job(
				  objects, target, refresh_default, result_context);
		if (signal)
			tp_signal_instance(objects, instance_id);
		SPI_finish();
		spi_connected = false;
		PopActiveSnapshot();
		snapshot_pushed = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		if (snapshot_pushed)
			PopActiveSnapshot();
		AtEOXact_GUC(false, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	return instance_id;
}

static char *
tp_schedule_as_trusted(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		MemoryContext				  result_context)
{
	Oid	  save_userid;
	int	  save_sec_context;
	int	  save_nestlevel;
	bool  spi_connected	  = false;
	bool  snapshot_pushed = false;
	char *instance_id	  = NULL;
	char *schedule		  = NULL;

	save_nestlevel = tp_set_safe_elevated_gucs();
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			objects->durable_extension_owner,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		PushActiveSnapshot(GetLatestSnapshot());
		snapshot_pushed = true;
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;
		instance_id	  = tp_find_family_instance(
				  objects, target, false, &schedule, result_context);
		if (instance_id == NULL)
			instance_id = tp_find_family_instance(
					objects, target, true, &schedule, result_context);
		if (instance_id == NULL && target->lineage_backfilled)
		{
			instance_id = tp_find_legacy_family_instance(
					objects, target, false, &schedule, result_context);
			if (instance_id == NULL)
				instance_id = tp_find_legacy_family_instance(
						objects, target, true, &schedule, result_context);
		}
		if (instance_id != NULL)
			pfree(instance_id);
		SPI_finish();
		spi_connected = false;
		PopActiveSnapshot();
		snapshot_pushed = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		if (snapshot_pushed)
			PopActiveSnapshot();
		AtEOXact_GUC(false, save_nestlevel);
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	return schedule;
}

void
tp_compaction_job_preflight(Oid owner_oid, const char *schedule)
{
	TpCompactionJobTarget  target;
	TpCompactionJobObjects objects;

	if (schedule == NULL)
		elog(ERROR, "background compaction schedule is not initialized");

	tp_require_owner_login(owner_oid);
	tp_require_owner_database_connect(owner_oid);
	tp_preflight_job_objects(&objects);
	tp_require_owner_superuser_policy(owner_oid);
	tp_require_owner_durable_privileges(&objects, owner_oid);

	memset(&target, 0, sizeof(target));
	target.database_oid	 = MyDatabaseId;
	target.owner_oid	 = owner_oid;
	target.schedule		 = pstrdup(schedule);
	target.family_prefix = tp_build_family_prefix(&target);

	tp_validate_graph_as_owner(&objects, &target);
}

static void
tp_activate_captured_target(
		const TpCompactionJobObjects *objects,
		TpCompactionJobTarget		 *target,
		bool						  refresh_default)
{
	char *instance_id PG_USED_FOR_ASSERTS_ONLY;

	tp_require_owner_login(target->owner_oid);
	tp_require_owner_database_connect(target->owner_oid);
	tp_require_owner_superuser_policy(target->owner_oid);
	tp_require_owner_durable_privileges(objects, target->owner_oid);
	tp_pin_durable_dependency(objects);
	tp_grant_helper_access(objects, target->owner_oid);
	instance_id = tp_reconcile_as_owner(
			objects, target, refresh_default, false, CurrentMemoryContext);
	Assert(instance_id != NULL);

	ereport(WARNING,
			(errmsg("pg_textsearch background compaction is a preview "
					"feature")));
}

void
tp_compaction_job_activate(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		bool						  refresh_default)
{
	TpCompactionJobTarget target;

	tp_require_compaction_index_lock(indexoid);
	tp_require_compaction_dependency_lock();
	tp_capture_target(indexoid, refresh_default, &target);
	tp_activate_captured_target(objects, &target, refresh_default);
}

void
tp_compaction_job_activate_with_schedule(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		const char					 *schedule)
{
	TpCompactionJobTarget target;

	if (schedule == NULL)
		elog(ERROR, "background compaction schedule is not initialized");

	tp_require_compaction_index_lock(indexoid);
	tp_require_compaction_dependency_lock();
	tp_capture_target(indexoid, true, &target);
	pfree(target.schedule);
	target.schedule = pstrdup(schedule);
	tp_activate_captured_target(objects, &target, true);
}

void
tp_compaction_job_capture(Oid indexoid, TpCompactionJobIdentity *identity)
{
	Relation	index_rel;
	const char *lineage;
	const char *schedule;

	index_rel = try_relation_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation with OID %u does not exist", indexoid)));
	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX ||
		index_rel->rd_index == NULL || !index_rel->rd_index->indisvalid ||
		!index_rel->rd_index->indisready || !index_rel->rd_index->indislive ||
		tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND)
	{
		char *index_name = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not ready for background "
						"compaction",
						index_name)));
	}

	memset(identity, 0, sizeof(*identity));
	identity->heap_oid		 = index_rel->rd_index->indrelid;
	identity->namespace_oid	 = RelationGetNamespace(index_rel);
	identity->index_name	 = pstrdup(RelationGetRelationName(index_rel));
	identity->index_oid		 = indexoid;
	identity->tablespace_oid = index_rel->rd_locator.spcOid;
	identity->relfilenumber	 = index_rel->rd_locator.relNumber;
	identity->owner_oid		 = index_rel->rd_rel->relowner;
	lineage					 = tp_index_compaction_lineage(index_rel);
	if (lineage == NULL)
	{
		identity->lineage			 = tp_new_compaction_lineage();
		identity->lineage_backfilled = true;
	}
	else
		identity->lineage = pstrdup(lineage);
	schedule = tp_index_compaction_schedule(index_rel);
	if (schedule == NULL)
		schedule = tp_background_compaction_schedule;
	identity->schedule			= pstrdup(schedule);
	identity->schedule_resolved = false;
	relation_close(index_rel, AccessShareLock);
}

void
tp_compaction_job_resolve_schedule(
		const TpCompactionJobObjects *objects,
		Oid							  indexoid,
		TpCompactionJobIdentity		 *identity,
		MemoryContext				  result_context)
{
	TpCompactionJobTarget target;
	char				 *schedule;

	if (identity->schedule_resolved)
		return;

	tp_require_compaction_index_lock(indexoid);
	memset(&target, 0, sizeof(target));
	target.database_oid		  = MyDatabaseId;
	target.index_oid		  = identity->index_oid;
	target.tablespace_oid	  = identity->tablespace_oid;
	target.relfilenumber	  = identity->relfilenumber;
	target.owner_oid		  = identity->owner_oid;
	target.heap_oid			  = identity->heap_oid;
	target.lineage			  = identity->lineage;
	target.schedule			  = identity->schedule;
	target.lineage_backfilled = identity->lineage_backfilled;
	target.history_prefix	  = tp_build_history_prefix(&target);
	target.family_prefix	  = tp_build_family_prefix(&target);

	tp_require_compaction_dependency_lock();
	schedule = tp_schedule_as_trusted(objects, &target, result_context);
	if (schedule != NULL)
	{
		pfree(identity->schedule);
		identity->schedule = schedule;
	}
	identity->schedule_resolved = true;
	pfree(target.history_prefix);
	pfree(target.family_prefix);
}

void
tp_compaction_job_signal(const TpCompactionJobObjects *objects, Oid indexoid)
{
	TpCompactionJobTarget target;
	char *instance_id	  PG_USED_FOR_ASSERTS_ONLY;

	tp_require_compaction_index_lock(indexoid);
	tp_require_compaction_dependency_lock();
	tp_capture_target(indexoid, false, &target);

	tp_require_owner_login(target.owner_oid);
	tp_require_owner_database_connect(target.owner_oid);

	tp_require_owner_superuser_policy(target.owner_oid);
	tp_require_owner_durable_privileges(objects, target.owner_oid);
	tp_pin_durable_dependency(objects);
	tp_grant_helper_access(objects, target.owner_oid);
	instance_id = tp_reconcile_as_owner(
			objects, &target, false, true, CurrentMemoryContext);
	Assert(instance_id != NULL);
}
