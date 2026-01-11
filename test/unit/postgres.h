/*
 * postgres.h stub for unit tests
 *
 * HOW THIS WORKS:
 * ---------------
 * When compiling unit tests, the Makefile specifies include paths as:
 *     -I$(UNIT_TEST_DIR) -Isrc
 *
 * Since test/unit comes BEFORE src in the include path, when source files
 * like segment/fieldnorm.h do `#include "postgres.h"`, the compiler finds
 * THIS file instead of the real PostgreSQL postgres.h header.
 *
 * This allows us to:
 *   1. Include actual source headers (e.g., segment/fieldnorm.h) in tests
 *   2. Test the real implementation without code duplication
 *   3. Avoid linking against PostgreSQL libraries
 *
 * WHAT THIS ENABLES:
 * ------------------
 * Test files can now do:
 *     #include "segment/fieldnorm.h"  // Uses real implementation!
 *
 * The fieldnorm.h header does `#include "postgres.h"`, which resolves to
 * this stub, providing the minimal types and macros needed to compile.
 *
 * LIMITATIONS:
 * ------------
 * This approach only works for "pure" functions that:
 *   - Don't call PostgreSQL backend functions (elog, heap_*, etc.)
 *   - Only use basic types (uint8, uint32, etc.) and simple macros
 *   - Don't require PostgreSQL memory contexts beyond palloc/pfree
 *
 * For functions needing more PostgreSQL infrastructure, you'd need to either:
 *   - Add more stubs to pg_stubs.h (e.g., stub out elog as fprintf)
 *   - Copy the function to the test file (loses the "test real code" benefit)
 *   - Use SQL-based regression tests instead
 *
 * EXTENDING THIS:
 * ---------------
 * To test more source files, you may need to add stubs for additional
 * PostgreSQL headers. Create them in test/unit/ with the same relative path:
 *   - test/unit/fmgr.h       -> stub for function manager macros
 *   - test/unit/utils/elog.h -> stub for error logging
 *
 * Each stub should provide minimal implementations that satisfy the compiler
 * without requiring the PostgreSQL runtime.
 */
#pragma once

#include "pg_stubs.h"
