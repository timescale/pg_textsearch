/*
 * pg_stubs.h - Minimal stubs for unit testing without PostgreSQL
 *
 * Provides just enough to compile and test pure computational functions.
 */
#pragma once

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Basic types */
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef int16_t int16;
typedef float float4;

/* Memory allocation - map to standard malloc/free */
#define palloc(size) malloc(size)
#define palloc0(size) calloc(1, size)
#define pfree(ptr) free(ptr)
#define repalloc(ptr, size) realloc(ptr, size)

/* Assertions */
#define Assert(cond) assert(cond)

/* Memory context stub - not really used in unit tests */
typedef void *MemoryContext;
#define CurrentMemoryContext NULL
static inline MemoryContext
MemoryContextSwitchTo(MemoryContext ctx)
{
	(void)ctx;
	return NULL;
}

/* ItemPointer stubs for CTID handling */
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

/* Test framework macros */
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
