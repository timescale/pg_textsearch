/*
 * pg_stubs.h - Minimal stubs for unit testing without PostgreSQL
 *
 * This file provides stub implementations of PostgreSQL types and functions,
 * allowing pure computational code to be compiled and tested standalone.
 *
 * STUB CATEGORIES:
 * ----------------
 * 1. Basic types (uint8, uint32, etc.) - Direct C99 stdint mappings
 * 2. Memory allocation (palloc, pfree) - Map to malloc/free
 * 3. Assertions (Assert) - Map to standard assert()
 * 4. Memory contexts - No-op stubs (most code doesn't need real contexts)
 * 5. ItemPointer/CTID - Simplified struct for tuple identification
 *
 * ADDING NEW STUBS:
 * -----------------
 * When you want to test a new source file that uses PostgreSQL features:
 *
 * 1. Try compiling - the errors will tell you what's missing
 * 2. Add minimal stubs here that satisfy the compiler
 * 3. For complex features, consider if the function is really "testable"
 *
 * Example stubs you might add:
 *
 *   // Error logging - redirect to stderr
 *   #define elog(level, ...) fprintf(stderr, __VA_ARGS__)
 *   #define ereport(level, ...) fprintf(stderr, "error\n")
 *
 *   // String functions
 *   #define pstrdup(s) strdup(s)
 *
 *   // Numeric types
 *   typedef double float8;
 *
 *   // Buffer/page stubs (if testing buffer-aware code)
 *   #define BLCKSZ 8192
 *   typedef uint32 BlockNumber;
 *
 * WHAT NOT TO STUB:
 * -----------------
 * Some PostgreSQL features are too complex to stub meaningfully:
 *   - Actual buffer manager operations (reading/writing pages)
 *   - Lock manager
 *   - Transaction management
 *   - Catalog access
 *
 * Code using these should be tested via SQL regression tests instead.
 */
#pragma once

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Basic PostgreSQL integer types
 *
 * PostgreSQL defines these in c.h. We map them to C99 stdint types.
 * Add more as needed (e.g., int64 for 64-bit signed).
 */
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef int16_t int16;
typedef float float4;

/*
 * Memory allocation
 *
 * PostgreSQL uses palloc/pfree which allocate from memory contexts.
 * For unit tests, we just use standard malloc/free.
 *
 * Note: This means memory allocated in tests won't be automatically
 * freed on error like in PostgreSQL. Tests should explicitly free.
 */
#define palloc(size) malloc(size)
#define palloc0(size) calloc(1, size)
#define pfree(ptr) free(ptr)
#define repalloc(ptr, size) realloc(ptr, size)

/*
 * Assertions
 *
 * PostgreSQL's Assert() is only active in debug builds (USE_ASSERT_CHECKING).
 * We always enable assertions in unit tests for safety.
 */
#define Assert(cond) assert(cond)

/*
 * Memory context stubs
 *
 * Most pure functions don't actually use memory contexts, but some headers
 * reference them. These no-op stubs satisfy the compiler.
 *
 * If you need real context-switching behavior, you'd need more elaborate
 * stubs or should use SQL-based tests instead.
 */
typedef void *MemoryContext;
#define CurrentMemoryContext NULL
static inline MemoryContext
MemoryContextSwitchTo(MemoryContext ctx)
{
	(void)ctx;
	return NULL;
}

/*
 * ItemPointer (CTID) stubs
 *
 * ItemPointer identifies a tuple's physical location (block + offset).
 * This simplified version is enough for testing posting list operations.
 *
 * Real PostgreSQL ItemPointerData has a more complex bit-packed layout,
 * but this simplified struct works for testing comparison and storage.
 */
typedef struct ItemPointerData
{
	uint32 block;
	uint16 offset;
} ItemPointerData;

typedef ItemPointerData *ItemPointer;

#define ItemPointerSetInvalid(p) \
	do \
	{ \
		(p)->block	= 0xFFFFFFFF; \
		(p)->offset = 0; \
	} while (0)

#define ItemPointerIsValid(p) ((p)->block != 0xFFFFFFFF)

#define ItemPointerSet(p, blk, off) \
	do \
	{ \
		(p)->block	= (blk); \
		(p)->offset = (off); \
	} while (0)

static inline int
ItemPointerCompare(const ItemPointer a, const ItemPointer b)
{
	if (a->block != b->block)
		return (a->block < b->block) ? -1 : 1;
	if (a->offset != b->offset)
		return (a->offset < b->offset) ? -1 : 1;
	return 0;
}

/*
 * Test framework macros
 *
 * Simple test assertions and runner. Each test function returns 0 on success.
 * The RUN_TEST macro tracks pass/fail counts using variables that must be
 * declared in main(): int passed = 0, failed = 0, total = 0;
 */
#define TEST_ASSERT(cond, msg) \
	do \
	{ \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
			return 1; \
		} \
	} while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
	do \
	{ \
		if ((a) != (b)) \
		{ \
			fprintf(stderr, \
					"FAIL: %s (expected %ld, got %ld) at %s:%d\n", \
					msg, \
					(long)(b), \
					(long)(a), \
					__FILE__, \
					__LINE__); \
			return 1; \
		} \
	} while (0)

#define TEST_ASSERT_FLOAT_EQ(a, b, epsilon, msg) \
	do \
	{ \
		if (fabs((a) - (b)) > (epsilon)) \
		{ \
			fprintf(stderr, \
					"FAIL: %s (expected %f, got %f) at %s:%d\n", \
					msg, \
					(double)(b), \
					(double)(a), \
					__FILE__, \
					__LINE__); \
			return 1; \
		} \
	} while (0)

#define RUN_TEST(test_func) \
	do \
	{ \
		printf("  %s... ", #test_func); \
		fflush(stdout); \
		if (test_func() == 0) \
		{ \
			printf("OK\n"); \
			passed++; \
		} \
		else \
		{ \
			failed++; \
		} \
		total++; \
	} while (0)
