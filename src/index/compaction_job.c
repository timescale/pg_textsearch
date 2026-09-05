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
#include <catalog/pg_am_d.h>
#include <catalog/pg_authid.h>
#include <catalog/pg_depend.h>
#include <catalog/pg_depend_d.h>
#include <catalog/pg_extension.h>
#include <catalog/pg_extension_d.h>
#include <catalog/pg_namespace_d.h>
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
#include <utils/fmgrprotos.h>
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
 * Required v0.2.7 entry points include df.wait_for_signal,
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
	char		 *index_name;
	char		 *schedule;
	char		 *family_prefix;
} TpCompactionJobTarget;

typedef struct TpCompactionJobObjects
{
	Oid	  durable_extension_oid;
	Oid	  durable_namespace_oid;
	Oid	  textsearch_extension_owner;
	Oid	  start_function_oid;
	Oid	  explain_function_oid;
	Oid	  signal_function_oid;
	Oid	  step_function_oid;
	Oid	  current_function_oid;
	Oid	  instances_relation_oid;
	Oid	  nodes_relation_oid;
	Oid	  vars_relation_oid;
	char *durable_schema;
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
} TpCompactionJobObjects;

static void
tp_durable_required(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("background compaction requires pg_durable 0.2.7 or "
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
tp_version_at_least_0_2_7(const char *version)
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
	return patch >= 7;
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
		Oid			extension_oid,
		const char *schema_name,
		const char *function_name,
		int			nargs,
		const Oid  *argtypes)
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

	LockDatabaseObject(ProcedureRelationId, function_oid, 0, AccessShareLock);
	if (getExtensionOfObject(ProcedureRelationId, function_oid) !=
		extension_oid)
		tp_durable_not_initialized(
				"a required pg_durable function changed during admission");

	return function_oid;
}

static Oid
tp_resolve_start_function(Oid extension_oid)
{
	Oid				  function_oid = InvalidOid;
	List			 *names;
	FuncCandidateList candidates;
	bool			  ambiguous = false;

	names = list_make2(
			makeString(pstrdup("df")), makeString(pstrdup("start")));
	candidates =
			FuncnameGetCandidates(names, 4, NIL, false, true, false, true);
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

	LockDatabaseObject(ProcedureRelationId, function_oid, 0, AccessShareLock);
	if (getExtensionOfObject(ProcedureRelationId, function_oid) !=
		extension_oid)
		tp_durable_not_initialized(
				"df.start(text,text,text,text) changed during admission");

	return function_oid;
}

static Oid
tp_resolve_extension_operator(
		Oid extension_oid, const char *schema_name, const char *operator_name)
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

	LockDatabaseObject(OperatorRelationId, operator_oid, 0, AccessShareLock);
	if (getExtensionOfObject(OperatorRelationId, operator_oid) !=
		extension_oid)
		tp_durable_not_initialized(psprintf(
				"required text operator %s changed during admission",
				operator_name));

	return operator_oid;
}

static Oid
tp_resolve_extension_relation(
		Oid extension_oid, Oid namespace_oid, const char *relation_name)
{
	Oid relation_oid = get_relname_relid(relation_name, namespace_oid);

	if (!OidIsValid(relation_oid) ||
		getExtensionOfObject(RelationRelationId, relation_oid) !=
				extension_oid)
		tp_durable_not_initialized(
				psprintf("df.%s is missing", relation_name));

	LockRelationOid(relation_oid, AccessShareLock);
	if (getExtensionOfObject(RelationRelationId, relation_oid) !=
		extension_oid)
		tp_durable_not_initialized(
				psprintf("df.%s changed during admission", relation_name));

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

static void
tp_discover_job_objects(TpCompactionJobObjects *objects)
{
	char	   *version;
	Oid			durable_oid;
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
	Oid			signal_args[3]		= {TEXTOID, TEXTOID, TEXTOID};
	Oid			wait_signal_args[2] = {TEXTOID, INT4OID};
	Oid			oid_args[5]			= {OIDOID, OIDOID, OIDOID, OIDOID, OIDOID};

	memset(objects, 0, sizeof(*objects));

	durable_oid = get_extension_oid("pg_durable", true);
	if (!OidIsValid(durable_oid))
		tp_durable_required();

	tp_lock_extension(durable_oid);
	rechecked_oid = get_extension_oid("pg_durable", true);
	if (rechecked_oid != durable_oid)
		tp_durable_required();

	if (!tp_extension_lookup(durable_oid, NULL, &version))
		tp_durable_required();
	if (version == NULL)
		tp_durable_required();
	if (!tp_version_at_least_0_2_7(version))
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

	start_oid			  = tp_resolve_start_function(durable_oid);
	durable_namespace_oid = get_func_namespace(start_oid);
	durable_schema		  = get_namespace_name(durable_namespace_oid);
	if (durable_schema == NULL)
		tp_durable_not_initialized("the pg_durable schema is missing");

	objects->durable_extension_oid = durable_oid;
	objects->durable_namespace_oid = durable_namespace_oid;
	objects->durable_schema		   = pstrdup(durable_schema);
	objects->start_function_oid	   = start_oid;
	objects->start_function		   = tp_qualified_function_name(start_oid);
	objects->explain_function_oid  = tp_resolve_extension_function(
			 durable_oid, durable_schema, "explain", 1, text_args);
	objects->explain_function = tp_qualified_function_name(
			objects->explain_function_oid);

	objects->signal_function_oid = tp_resolve_extension_function(
			durable_oid, durable_schema, "signal", 3, signal_args);
	objects->signal_function = tp_qualified_function_name(
			objects->signal_function_oid);
	objects->wait_signal_function = tp_qualified_function_name(
			tp_resolve_extension_function(
					durable_oid,
					durable_schema,
					"wait_for_signal",
					2,
					wait_signal_args));
	objects->wait_schedule_function = tp_qualified_function_name(
			tp_resolve_extension_function(
					durable_oid,
					durable_schema,
					"wait_for_schedule",
					1,
					text_args));
	objects->loop_function = tp_qualified_function_name(
			tp_resolve_extension_function(
					durable_oid, durable_schema, "loop", 2, text_args));
	objects->break_function = tp_qualified_function_name(
			tp_resolve_extension_function(
					durable_oid, durable_schema, "break", 1, text_args));
	operator_schema_oid = get_extension_schema(durable_oid);
	operator_schema		= get_namespace_name(operator_schema_oid);
	if (operator_schema == NULL)
		tp_durable_not_initialized(
				"the pg_durable operator schema is missing");
	objects->operator_schema = pstrdup(operator_schema);

	tp_resolve_extension_operator(durable_oid, operator_schema, "|=>");
	tp_resolve_extension_operator(durable_oid, operator_schema, "~>");
	tp_resolve_extension_operator(durable_oid, operator_schema, "?>");
	tp_resolve_extension_operator(durable_oid, operator_schema, "!>");
	tp_resolve_extension_operator(durable_oid, operator_schema, "|");

	objects->instances_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "instances");
	objects->nodes_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "nodes");
	objects->vars_relation_oid = tp_resolve_extension_relation(
			durable_oid, durable_namespace_oid, "vars");
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
	tp_lock_extension(textsearch_oid);
	textsearch_namespace_oid = get_extension_schema(textsearch_oid);
	textsearch_schema		 = get_namespace_name(textsearch_namespace_oid);
	if (textsearch_schema == NULL)
		elog(ERROR, "pg_textsearch extension schema is missing");

	objects->textsearch_extension_owner = tp_extension_owner(textsearch_oid);
	objects->step_function_oid			= tp_resolve_extension_function(
			 textsearch_oid,
			 textsearch_schema,
			 "bm25_compact_step_if_current",
			 5,
			 oid_args);
	objects->current_function_oid = tp_resolve_extension_function(
			textsearch_oid,
			textsearch_schema,
			"bm25_background_target_is_current",
			5,
			oid_args);
	objects->step_function = tp_qualified_function_name(
			objects->step_function_oid);
	objects->current_function = tp_qualified_function_name(
			objects->current_function_oid);
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
			  "%s%u:%s", target->family_prefix, target->owner_oid, encoded);

	pfree(encoded);
	return label;
}

static char *
tp_schedule_from_label(
		const TpCompactionJobTarget *target,
		const char					*label,
		MemoryContext				 result_context)
{
	char *owner_prefix =
			psprintf("%s%u:", target->family_prefix, target->owner_oid);
	Size		  prefix_length = strlen(owner_prefix);
	const char	 *encoded;
	Size		  encoded_length;
	char		 *schedule;
	MemoryContext old_context;

	if (strncmp(label, owner_prefix, prefix_length) != 0)
	{
		pfree(owner_prefix);
		return NULL;
	}
	pfree(owner_prefix);

	encoded		   = label + prefix_length;
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

static void
tp_capture_target(
		Oid indexoid, bool refresh_default, TpCompactionJobTarget *target)
{
	Relation	index_rel;
	const char *schedule;

	memset(target, 0, sizeof(*target));
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
	target->index_name	   = pstrdup(RelationGetRelationName(index_rel));

	if (refresh_default)
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
	tp_require_owner_login(target->owner_oid);
	target->family_prefix = tp_build_family_prefix(target);
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
			.objectId	 = get_index_am_oid("bm25", false),
			.objectSubId = 0,
	};
	ObjectAddress durable_ext = {
			.classId	 = ExtensionRelationId,
			.objectId	 = objects->durable_extension_oid,
			.objectSubId = 0,
	};

	/*
	 * Different indexes have different admission advisory locks.  Serialize
	 * the first dependency check/insert on the one stable dependent object.
	 */
	LockDatabaseObject(
			AccessMethodRelationId,
			bm25_am.objectId,
			0,
			ShareRowExclusiveLock);

	if (!tp_dependency_exists(&bm25_am, &durable_ext))
	{
		recordDependencyOn(&bm25_am, &durable_ext, DEPENDENCY_NORMAL);
		CommandCounterIncrement();
	}
}

static void
tp_take_admission_lock(const TpCompactionJobTarget *target)
{
	DirectFunctionCall2(
			pg_advisory_xact_lock_int4,
			Int32GetDatum((int32)target->database_oid),
			Int32GetDatum((int32)target->index_oid));
}

static void
tp_grant_helper_access(const TpCompactionJobObjects *objects, Oid owner_oid)
{
	AclResult	   step_acl;
	AclResult	   current_acl;
	Oid			   save_userid;
	int			   save_sec_context;
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

static void
tp_build_worker_queries(
		const TpCompactionJobObjects *objects,
		const TpCompactionJobTarget	 *target,
		char						**step_sql,
		char						**current_sql)
{
	char *family_literal = quote_literal_cstr(target->family_prefix);

	*step_sql = psprintf(
			"SELECT %s(%u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid) AS ran",
			objects->step_function,
			target->index_oid,
			target->database_oid,
			target->tablespace_oid,
			(Oid)target->relfilenumber,
			target->owner_oid);

	*current_sql = psprintf(
			"SELECT (%s(%u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid, %u::pg_catalog.oid, "
			"%u::pg_catalog.oid) AND coalesce(("
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
			objects->current_function,
			target->index_oid,
			target->database_oid,
			target->tablespace_oid,
			(Oid)target->relfilenumber,
			target->owner_oid,
			objects->instances_relation,
			strlen(target->family_prefix),
			family_literal,
			target->owner_oid);
	pfree(family_literal);
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
			"NULL::pg_catalog.text)",
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
			"), NULL::pg_catalog.text) "
			"OPERATOR(%s.~>) %s('stale'::pg_catalog.text)), "
			"NULL::pg_catalog.text)",
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
	bool		   spi_connected = false;
	StringInfoData sql;
	char		  *step_sql;
	char		  *current_sql;
	char		  *explanation = NULL;
	Oid			   argtypes[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum		   values[3];
	int			   rc;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
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
	char *schedule = NULL;
	char *label	   = NULL;

	if (refresh_default)
	{
		label = tp_build_label(target, target->schedule);
		instance_id =
				tp_find_exact_instance(objects, target, label, result_context);
		if (instance_id == NULL)
			instance_id = tp_start_job(
					objects, target, target->schedule, label, result_context);
		pfree(label);
		return instance_id;
	}

	instance_id = tp_find_family_instance(
			objects, target, false, &schedule, result_context);
	if (instance_id != NULL)
	{
		pfree(schedule);
		return instance_id;
	}

	instance_id = tp_find_family_instance(
			objects, target, true, &schedule, result_context);
	if (instance_id == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("background compaction for index \"%s\" requires "
						"explicit adoption",
						target->index_name)));

	pfree(instance_id);
	label = tp_build_label(target, schedule);
	instance_id =
			tp_find_exact_instance(objects, target, label, result_context);
	if (instance_id == NULL)
		instance_id =
				tp_start_job(objects, target, schedule, label, result_context);
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
	bool  spi_connected = false;
	char *instance_id	= NULL;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			target->owner_oid,
			save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		spi_connected = true;
		instance_id	  = tp_reconcile_job(
				  objects, target, refresh_default, result_context);
		if (signal)
			tp_signal_instance(objects, instance_id);
		SPI_finish();
		spi_connected = false;
	}
	PG_FINALLY();
	{
		if (spi_connected)
			SPI_finish();
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	return instance_id;
}

void
tp_compaction_job_preflight(Oid owner_oid, const char *schedule)
{
	TpCompactionJobTarget  target;
	TpCompactionJobObjects objects;

	if (schedule == NULL)
		elog(ERROR, "background compaction schedule is not initialized");

	tp_require_owner_login(owner_oid);
	tp_discover_job_objects(&objects);
	tp_require_owner_superuser_policy(owner_oid);
	tp_require_owner_durable_privileges(&objects, owner_oid);

	memset(&target, 0, sizeof(target));
	target.database_oid	 = MyDatabaseId;
	target.owner_oid	 = owner_oid;
	target.schedule		 = pstrdup(schedule);
	target.family_prefix = tp_build_family_prefix(&target);

	tp_validate_graph_as_owner(&objects, &target);
}

void
tp_compaction_job_activate(Oid indexoid, bool refresh_default)
{
	TpCompactionJobTarget  target;
	TpCompactionJobObjects objects;
	char *instance_id	   PG_USED_FOR_ASSERTS_ONLY;

	tp_capture_target(indexoid, refresh_default, &target);
	tp_discover_job_objects(&objects);
	tp_require_owner_superuser_policy(target.owner_oid);
	tp_require_owner_durable_privileges(&objects, target.owner_oid);
	tp_pin_durable_dependency(&objects);
	tp_grant_helper_access(&objects, target.owner_oid);
	tp_take_admission_lock(&target);
	instance_id = tp_reconcile_as_owner(
			&objects, &target, refresh_default, false, CurrentMemoryContext);
	Assert(instance_id != NULL);

	ereport(WARNING,
			(errmsg("pg_textsearch background compaction is a preview "
					"feature"),
			 errdetail(
					 "pg_durable v0.2.7 jobs fail permanently on a node "
					 "error, receive no autonomous idle recovery, and stop "
					 "after 100,000 loop iterations.")));
}

void
tp_compaction_job_signal(Oid indexoid)
{
	TpCompactionJobTarget  target;
	TpCompactionJobObjects objects;
	char *instance_id	   PG_USED_FOR_ASSERTS_ONLY;

	tp_capture_target(indexoid, false, &target);
	tp_discover_job_objects(&objects);
	tp_grant_helper_access(&objects, target.owner_oid);
	tp_take_admission_lock(&target);
	instance_id = tp_reconcile_as_owner(
			&objects, &target, false, true, CurrentMemoryContext);
	Assert(instance_id != NULL);
}
