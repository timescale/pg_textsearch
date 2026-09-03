/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compat.h - Compiler and server-version portability helpers
 */
#pragma once

#include <postgres.h>

/*
 * Packed struct marker.  c.h defines pg_attribute_packed() only for
 * compilers that spell it as an attribute, and notes that MSVC can pack
 * a struct only by wrapping its whole definition.  TP_PACKED therefore
 * expands to nothing on MSVC, and every struct that uses it wraps its
 * definition in #pragma pack(push, 1) / #pragma pack(pop).
 */
#ifdef _MSC_VER
#define TP_PACKED
#else
#define TP_PACKED pg_attribute_packed()
#endif

/*
 * PG19 requires a hand-built TupleDesc to be finalized before it is
 * blessed or otherwise used; on a cassert build BlessTupleDesc() TRAPs
 * otherwise.  Older servers have no such call, so define it away and
 * let call sites use the PG19 API unconditionally.
 */
#if PG_VERSION_NUM < 190000
#define TupleDescFinalize(tupdesc) ((void)(tupdesc))
#endif

/*
 * post_parse_analyze_hook's JumbleState parameter became const in PG19.
 * A hook implementation has to match the typedef exactly — C does not
 * consider `const JumbleState *` and `JumbleState *` compatible
 * parameter types — so the qualifier must track the server version.
 */
#if PG_VERSION_NUM >= 190000
#define TP_JUMBLE_STATE const JumbleState
#else
#define TP_JUMBLE_STATE JumbleState
#endif
