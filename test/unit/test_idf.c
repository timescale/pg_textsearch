/*
 * test_idf.c - Unit tests for BM25 IDF calculation
 *
 * Tests the tp_calculate_idf function that implements the BM25 IDF formula:
 *   IDF = log(1 + (N - df + 0.5) / (df + 0.5))
 *
 * where N = total documents, df = document frequency
 */
#include "pg_stubs.h"

/*
 * Copy the IDF function here to avoid PostgreSQL dependencies.
 * This must stay in sync with src/query/score.c.
 */
static float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	return (float4)log(1.0 + idf_ratio);
}

/* Test IDF is always non-negative (log(1 + x) >= 0 for x >= 0) */
static int
test_idf_non_negative(void)
{
	/* Various doc_freq and total_docs combinations */
	int32 test_cases[][2] = {
		{1, 1000},
		{100, 1000},
		{500, 1000},
		{999, 1000},
		{1000, 1000},
		{1, 1},
		{0, 100}, /* Edge case: term appears in 0 docs */
	};

	for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++)
	{
		float4 idf = tp_calculate_idf(test_cases[i][0], test_cases[i][1]);
		TEST_ASSERT(idf >= 0.0f, "IDF should be non-negative");
	}
	return 0;
}

/* Test IDF decreases as doc_freq increases (inverse relationship) */
static int
test_idf_inverse_relationship(void)
{
	int32  total_docs = 10000;
	float4 prev_idf	  = tp_calculate_idf(1, total_docs);

	for (int32 df = 10; df <= total_docs; df += 100)
	{
		float4 idf = tp_calculate_idf(df, total_docs);
		TEST_ASSERT(idf <= prev_idf, "IDF should decrease as df increases");
		prev_idf = idf;
	}
	return 0;
}

/* Test known IDF values against hand-calculated results */
static int
test_idf_known_values(void)
{
	/* IDF(1, 1000) = log(1 + (1000 - 1 + 0.5) / (1 + 0.5))
	 *              = log(1 + 999.5 / 1.5)
	 *              = log(1 + 666.333...)
	 *              = log(667.333...)
	 *              ≈ 6.503 */
	float4 idf1 = tp_calculate_idf(1, 1000);
	TEST_ASSERT_FLOAT_EQ(idf1, 6.503f, 0.01f, "IDF(1, 1000)");

	/* IDF(500, 1000) = log(1 + (1000 - 500 + 0.5) / (500 + 0.5))
	 *               = log(1 + 500.5 / 500.5)
	 *               = log(2)
	 *               ≈ 0.693 */
	float4 idf2 = tp_calculate_idf(500, 1000);
	TEST_ASSERT_FLOAT_EQ(idf2, 0.693f, 0.01f, "IDF(500, 1000)");

	/* IDF(1000, 1000) = log(1 + (1000 - 1000 + 0.5) / (1000 + 0.5))
	 *                = log(1 + 0.5 / 1000.5)
	 *                = log(1.0005)
	 *                ≈ 0.0005 */
	float4 idf3 = tp_calculate_idf(1000, 1000);
	TEST_ASSERT_FLOAT_EQ(idf3, 0.0005f, 0.001f, "IDF(1000, 1000)");

	return 0;
}

/* Test edge case: df = 0 (term appears nowhere) */
static int
test_idf_zero_df(void)
{
	/* IDF(0, 1000) = log(1 + (1000 - 0 + 0.5) / (0 + 0.5))
	 *             = log(1 + 1000.5 / 0.5)
	 *             = log(1 + 2001)
	 *             = log(2002)
	 *             ≈ 7.60 */
	float4 idf = tp_calculate_idf(0, 1000);
	TEST_ASSERT_FLOAT_EQ(idf, 7.60f, 0.01f, "IDF(0, 1000)");
	TEST_ASSERT(idf > 0.0f, "IDF with df=0 should still be positive");
	return 0;
}

/* Test edge case: df = N (term in all docs) - should be very small */
static int
test_idf_all_docs(void)
{
	float4 idf = tp_calculate_idf(1000, 1000);
	TEST_ASSERT(idf < 0.01f, "IDF when term is in all docs should be tiny");
	TEST_ASSERT(idf >= 0.0f, "IDF should still be non-negative");
	return 0;
}

/* Test IDF scales correctly with collection size */
static int
test_idf_scaling(void)
{
	/* Same ratio df/N should give similar IDF regardless of N */
	/* (not exactly the same due to +0.5 smoothing, but close) */
	float4 idf_small  = tp_calculate_idf(10, 1000);	  /* 1% */
	float4 idf_medium = tp_calculate_idf(100, 10000); /* 1% */
	float4 idf_large  = tp_calculate_idf(1000, 100000); /* 1% */

	/* All should be similar (within 10%) */
	TEST_ASSERT(fabs(idf_small - idf_medium) / idf_small < 0.1f,
				"IDF should scale with ratio");
	TEST_ASSERT(fabs(idf_medium - idf_large) / idf_medium < 0.1f,
				"IDF should scale with ratio");

	return 0;
}

/* Test numerical stability with large values */
static int
test_idf_large_values(void)
{
	/* Large collection */
	float4 idf1 = tp_calculate_idf(1000000, 100000000);
	TEST_ASSERT(idf1 > 0.0f, "IDF with large values should be positive");
	TEST_ASSERT(!isnan(idf1), "IDF should not be NaN");
	TEST_ASSERT(!isinf(idf1), "IDF should not be infinite");

	/* Very rare term in large collection */
	float4 idf2 = tp_calculate_idf(1, 100000000);
	TEST_ASSERT(idf2 > 0.0f, "Rare term IDF should be positive");
	TEST_ASSERT(idf2 > 10.0f, "Rare term IDF should be high");

	return 0;
}

int
main(void)
{
	int passed = 0, failed = 0, total = 0;

	printf("Running IDF tests...\n");

	RUN_TEST(test_idf_non_negative);
	RUN_TEST(test_idf_inverse_relationship);
	RUN_TEST(test_idf_known_values);
	RUN_TEST(test_idf_zero_df);
	RUN_TEST(test_idf_all_docs);
	RUN_TEST(test_idf_scaling);
	RUN_TEST(test_idf_large_values);

	printf("\nIDF: %d/%d tests passed", passed, total);
	if (failed > 0)
		printf(" (%d FAILED)", failed);
	printf("\n");

	return failed > 0 ? 1 : 0;
}
