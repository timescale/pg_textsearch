/*
 * test_topk.c - Unit tests for top-k min-heap implementation
 *
 * Tests the TpTopKHeap that maintains the k highest-scoring documents
 * with proper tie-breaking (lower CTID wins ties).
 */
#include "pg_stubs.h"

/*
 * Top-K Heap structure (from bmw.h)
 */
typedef struct TpTopKHeap
{
	ItemPointerData *ctids;
	float4			*scores;
	int				 capacity;
	int				 size;
} TpTopKHeap;

/*
 * Copy the heap implementation here to avoid PostgreSQL dependencies.
 * This must stay in sync with src/query/bmw.c.
 */
static void
tp_topk_init(TpTopKHeap *heap, int k, MemoryContext ctx)
{
	(void)ctx; /* Unused in stub */
	heap->ctids	   = palloc(k * sizeof(ItemPointerData));
	heap->scores   = palloc(k * sizeof(float4));
	heap->capacity = k;
	heap->size	   = 0;
}

static void
tp_topk_free(TpTopKHeap *heap)
{
	if (heap->ctids)
	{
		pfree(heap->ctids);
		heap->ctids = NULL;
	}
	if (heap->scores)
	{
		pfree(heap->scores);
		heap->scores = NULL;
	}
	heap->capacity = 0;
	heap->size	   = 0;
}

static inline void
heap_swap(TpTopKHeap *heap, int i, int j)
{
	ItemPointerData tmp_ctid  = heap->ctids[i];
	float4			tmp_score = heap->scores[i];

	heap->ctids[i]	= heap->ctids[j];
	heap->scores[i] = heap->scores[j];
	heap->ctids[j]	= tmp_ctid;
	heap->scores[j] = tmp_score;
}

static inline bool
heap_less(TpTopKHeap *heap, int a, int b)
{
	if (heap->scores[a] != heap->scores[b])
		return heap->scores[a] < heap->scores[b];
	return ItemPointerCompare(&heap->ctids[a], &heap->ctids[b]) > 0;
}

static void
heap_sift_up(TpTopKHeap *heap, int i)
{
	while (i > 0)
	{
		int parent = (i - 1) / 2;
		if (heap_less(heap, parent, i) ||
			(!heap_less(heap, i, parent) && parent <= i))
			break;
		heap_swap(heap, i, parent);
		i = parent;
	}
}

static void
heap_sift_down(TpTopKHeap *heap, int i)
{
	while (true)
	{
		int left	 = 2 * i + 1;
		int right	 = 2 * i + 2;
		int smallest = i;

		if (left < heap->size && heap_less(heap, left, smallest))
			smallest = left;
		if (right < heap->size && heap_less(heap, right, smallest))
			smallest = right;

		if (smallest == i)
			break;

		heap_swap(heap, i, smallest);
		i = smallest;
	}
}

static void
tp_topk_add(TpTopKHeap *heap, ItemPointerData ctid, float4 score)
{
	if (heap->size < heap->capacity)
	{
		int i			= heap->size++;
		heap->ctids[i]	= ctid;
		heap->scores[i] = score;
		heap_sift_up(heap, i);
	}
	else if (
			score > heap->scores[0] ||
			(score == heap->scores[0] &&
			 ItemPointerCompare(&ctid, &heap->ctids[0]) < 0))
	{
		heap->ctids[0]	= ctid;
		heap->scores[0] = score;
		heap_sift_down(heap, 0);
	}
}

static int
tp_topk_extract(TpTopKHeap *heap, ItemPointerData *ctids, float4 *scores)
{
	int count = heap->size;

	while (heap->size > 0)
	{
		heap->size--;
		heap_swap(heap, 0, heap->size);
		heap_sift_down(heap, 0);
	}

	for (int i = 0; i < count; i++)
	{
		ctids[i]  = heap->ctids[i];
		scores[i] = heap->scores[i];
	}

	return count;
}

static inline float4
tp_topk_threshold(TpTopKHeap *heap)
{
	if (heap->size < heap->capacity)
		return -INFINITY;
	return heap->scores[0];
}

/* Helper to create a CTID */
static ItemPointerData
make_ctid(uint32 block, uint16 offset)
{
	ItemPointerData ctid;
	ctid.block	= block;
	ctid.offset = offset;
	return ctid;
}

/* Test basic add and extract */
static int
test_basic_add_extract(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[3];
	float4			out_scores[3];
	int				count;

	tp_topk_init(&heap, 3, NULL);

	tp_topk_add(&heap, make_ctid(1, 1), 1.0f);
	tp_topk_add(&heap, make_ctid(2, 1), 2.0f);
	tp_topk_add(&heap, make_ctid(3, 1), 3.0f);

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 3, "should return 3 items");
	/* Should be descending order */
	TEST_ASSERT_FLOAT_EQ(out_scores[0], 3.0f, 0.001f, "first score");
	TEST_ASSERT_FLOAT_EQ(out_scores[1], 2.0f, 0.001f, "second score");
	TEST_ASSERT_FLOAT_EQ(out_scores[2], 1.0f, 0.001f, "third score");

	tp_topk_free(&heap);
	return 0;
}

/* Test that lower scores are evicted */
static int
test_eviction(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[3];
	float4			out_scores[3];
	int				count;

	tp_topk_init(&heap, 3, NULL);

	/* Add 5 items, heap size 3 */
	tp_topk_add(&heap, make_ctid(1, 1), 1.0f);
	tp_topk_add(&heap, make_ctid(2, 1), 5.0f);
	tp_topk_add(&heap, make_ctid(3, 1), 2.0f);
	tp_topk_add(&heap, make_ctid(4, 1), 4.0f);
	tp_topk_add(&heap, make_ctid(5, 1), 3.0f);

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 3, "should return 3 items");
	/* Top 3 should be 5, 4, 3 */
	TEST_ASSERT_FLOAT_EQ(out_scores[0], 5.0f, 0.001f, "first score");
	TEST_ASSERT_FLOAT_EQ(out_scores[1], 4.0f, 0.001f, "second score");
	TEST_ASSERT_FLOAT_EQ(out_scores[2], 3.0f, 0.001f, "third score");

	tp_topk_free(&heap);
	return 0;
}

/* Test tie-breaking: lower CTID wins */
static int
test_tie_breaking(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[3];
	float4			out_scores[3];
	int				count;

	tp_topk_init(&heap, 3, NULL);

	/* Add items with same score, different CTIDs */
	tp_topk_add(&heap, make_ctid(5, 1), 1.0f); /* Higher CTID */
	tp_topk_add(&heap, make_ctid(1, 1), 1.0f); /* Lower CTID - should win */
	tp_topk_add(&heap, make_ctid(3, 1), 1.0f);

	/* Add one more with same score but highest CTID - should be evicted */
	tp_topk_add(&heap, make_ctid(10, 1), 1.0f);

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 3, "should return 3 items");

	/* All scores should be 1.0 */
	for (int i = 0; i < 3; i++)
	{
		TEST_ASSERT_FLOAT_EQ(out_scores[i], 1.0f, 0.001f, "all scores equal");
	}

	/* CTIDs should be in ascending order (1, 3, 5) since lower CTID wins */
	TEST_ASSERT_EQ(out_ctids[0].block, 1, "first CTID block");
	TEST_ASSERT_EQ(out_ctids[1].block, 3, "second CTID block");
	TEST_ASSERT_EQ(out_ctids[2].block, 5, "third CTID block");

	tp_topk_free(&heap);
	return 0;
}

/* Test threshold function */
static int
test_threshold(void)
{
	TpTopKHeap heap;

	tp_topk_init(&heap, 3, NULL);

	/* Empty heap should have -infinity threshold */
	TEST_ASSERT(tp_topk_threshold(&heap) < -1e30f, "empty threshold");

	tp_topk_add(&heap, make_ctid(1, 1), 1.0f);
	TEST_ASSERT(tp_topk_threshold(&heap) < -1e30f, "partial fill threshold");

	tp_topk_add(&heap, make_ctid(2, 1), 2.0f);
	TEST_ASSERT(tp_topk_threshold(&heap) < -1e30f, "partial fill threshold");

	tp_topk_add(&heap, make_ctid(3, 1), 3.0f);
	/* Now full - threshold should be minimum score */
	TEST_ASSERT_FLOAT_EQ(tp_topk_threshold(&heap), 1.0f, 0.001f,
						 "full threshold");

	/* Add higher score */
	tp_topk_add(&heap, make_ctid(4, 1), 5.0f);
	/* Threshold should now be 2.0 (1.0 was evicted) */
	TEST_ASSERT_FLOAT_EQ(tp_topk_threshold(&heap), 2.0f, 0.001f,
						 "updated threshold");

	tp_topk_free(&heap);
	return 0;
}

/* Test with single element capacity */
static int
test_single_capacity(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[1];
	float4			out_scores[1];
	int				count;

	tp_topk_init(&heap, 1, NULL);

	tp_topk_add(&heap, make_ctid(1, 1), 1.0f);
	tp_topk_add(&heap, make_ctid(2, 1), 3.0f); /* Should replace */
	tp_topk_add(&heap, make_ctid(3, 1), 2.0f); /* Should not replace */

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 1, "should return 1 item");
	TEST_ASSERT_FLOAT_EQ(out_scores[0], 3.0f, 0.001f, "highest score kept");
	TEST_ASSERT_EQ(out_ctids[0].block, 2, "correct CTID");

	tp_topk_free(&heap);
	return 0;
}

/* Test with many insertions */
static int
test_many_insertions(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[10];
	float4			out_scores[10];
	int				count;

	tp_topk_init(&heap, 10, NULL);

	/* Insert 1000 items */
	for (int i = 0; i < 1000; i++)
	{
		tp_topk_add(&heap, make_ctid(i, 1), (float4)(i % 100));
	}

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 10, "should return 10 items");

	/* Top 10 should all have score 99 (items 99, 199, 299, ...) */
	for (int i = 0; i < 10; i++)
	{
		TEST_ASSERT_FLOAT_EQ(out_scores[i], 99.0f, 0.001f, "top score");
	}

	/* Should be sorted by CTID ascending (99, 199, 299, ...) */
	TEST_ASSERT_EQ(out_ctids[0].block, 99, "first CTID");
	TEST_ASSERT_EQ(out_ctids[1].block, 199, "second CTID");

	tp_topk_free(&heap);
	return 0;
}

/* Test descending insertion order */
static int
test_descending_order(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[5];
	float4			out_scores[5];
	int				count;

	tp_topk_init(&heap, 5, NULL);

	/* Insert in descending order - worst case for some heap impls */
	for (int i = 100; i >= 1; i--)
	{
		tp_topk_add(&heap, make_ctid(i, 1), (float4)i);
	}

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 5, "should return 5 items");
	TEST_ASSERT_FLOAT_EQ(out_scores[0], 100.0f, 0.001f, "first score");
	TEST_ASSERT_FLOAT_EQ(out_scores[1], 99.0f, 0.001f, "second score");
	TEST_ASSERT_FLOAT_EQ(out_scores[2], 98.0f, 0.001f, "third score");
	TEST_ASSERT_FLOAT_EQ(out_scores[3], 97.0f, 0.001f, "fourth score");
	TEST_ASSERT_FLOAT_EQ(out_scores[4], 96.0f, 0.001f, "fifth score");

	tp_topk_free(&heap);
	return 0;
}

/* Test CTID offset tie-breaking */
static int
test_ctid_offset_tiebreak(void)
{
	TpTopKHeap		heap;
	ItemPointerData out_ctids[2];
	float4			out_scores[2];
	int				count;

	tp_topk_init(&heap, 2, NULL);

	/* Same block, different offsets */
	tp_topk_add(&heap, make_ctid(1, 5), 1.0f);
	tp_topk_add(&heap, make_ctid(1, 1), 1.0f); /* Lower offset - should win */
	tp_topk_add(&heap, make_ctid(1, 10), 1.0f); /* Should be evicted */

	count = tp_topk_extract(&heap, out_ctids, out_scores);

	TEST_ASSERT_EQ(count, 2, "should return 2 items");
	/* Lower offsets should win */
	TEST_ASSERT_EQ(out_ctids[0].offset, 1, "first offset");
	TEST_ASSERT_EQ(out_ctids[1].offset, 5, "second offset");

	tp_topk_free(&heap);
	return 0;
}

int
main(void)
{
	int passed = 0, failed = 0, total = 0;

	printf("Running top-k heap tests...\n");

	RUN_TEST(test_basic_add_extract);
	RUN_TEST(test_eviction);
	RUN_TEST(test_tie_breaking);
	RUN_TEST(test_threshold);
	RUN_TEST(test_single_capacity);
	RUN_TEST(test_many_insertions);
	RUN_TEST(test_descending_order);
	RUN_TEST(test_ctid_offset_tiebreak);

	printf("\nTop-K Heap: %d/%d tests passed", passed, total);
	if (failed > 0)
		printf(" (%d FAILED)", failed);
	printf("\n");

	return failed > 0 ? 1 : 0;
}
