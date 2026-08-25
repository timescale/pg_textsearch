/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compat.h - Compiler portability helpers
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
