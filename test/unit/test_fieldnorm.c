/*
 * test_fieldnorm.c - Unit tests for fieldnorm encoding/decoding
 *
 * Tests the Lucene SmallFloat encoding that maps document lengths to bytes.
 */

/* Include the actual source header - our postgres.h stub will be found first */
#include "segment/fieldnorm.h"

/* Test exact values (0-39) are preserved */
static int
test_exact_values(void)
{
	for (uint32 i = 0; i <= 39; i++)
	{
		uint8  encoded = encode_fieldnorm(i);
		uint32 decoded = decode_fieldnorm(encoded);
		TEST_ASSERT_EQ(decoded, i, "exact value roundtrip");
	}
	return 0;
}

/* Test that encode is monotonic (larger input -> larger or equal output) */
static int
test_encode_monotonic(void)
{
	uint8 prev = 0;
	for (uint32 i = 0; i < 10000; i += 7)
	{
		uint8 encoded = encode_fieldnorm(i);
		TEST_ASSERT(encoded >= prev, "encode should be monotonic");
		prev = encoded;
	}
	return 0;
}

/* Test that decode is monotonic (larger input -> larger output) */
static int
test_decode_monotonic(void)
{
	uint32 prev = 0;
	for (int i = 0; i < 256; i++)
	{
		uint32 decoded = decode_fieldnorm((uint8)i);
		TEST_ASSERT(decoded >= prev, "decode should be monotonic");
		prev = decoded;
	}
	return 0;
}

/* Test encode produces valid byte range */
static int
test_encode_range(void)
{
	/* Test various document lengths */
	uint32 test_values[] = {
		0,
		1,
		10,
		100,
		1000,
		10000,
		100000,
		1000000,
		10000000,
		100000000,
		1000000000,
		UINT32_MAX};

	for (size_t i = 0; i < sizeof(test_values) / sizeof(test_values[0]); i++)
	{
		uint8 encoded = encode_fieldnorm(test_values[i]);
		/* Result should always be 0-255 (guaranteed by uint8, but check logic)
		 */
		TEST_ASSERT(encoded <= 255, "encoded value in range");
	}
	return 0;
}

/* Test decode table boundaries */
static int
test_decode_boundaries(void)
{
	/* First value should be 0 */
	TEST_ASSERT_EQ(decode_fieldnorm(0), 0, "decode(0) == 0");

	/* Last value should be large */
	TEST_ASSERT(
			decode_fieldnorm(255) > 2000000000, "decode(255) > 2 billion");

	return 0;
}

/* Test roundtrip: encode(decode(x)) should equal x for all bytes */
static int
test_decode_encode_identity(void)
{
	for (int i = 0; i < 256; i++)
	{
		uint32 decoded = decode_fieldnorm((uint8)i);
		uint8  encoded = encode_fieldnorm(decoded);
		TEST_ASSERT_EQ(encoded, i, "decode then encode should be identity");
	}
	return 0;
}

/* Test encode finds correct bucket (decoded value <= input < next decoded) */
static int
test_encode_finds_correct_bucket(void)
{
	/* Test values in the step-2 range (40-55) */
	TEST_ASSERT_EQ(encode_fieldnorm(40), 40, "40 maps to bucket 40");
	TEST_ASSERT_EQ(encode_fieldnorm(41), 40, "41 maps to bucket 40");
	TEST_ASSERT_EQ(encode_fieldnorm(42), 41, "42 maps to bucket 41");
	TEST_ASSERT_EQ(encode_fieldnorm(43), 41, "43 maps to bucket 41");

	/* Test value exactly at a bucket boundary */
	TEST_ASSERT_EQ(encode_fieldnorm(56), 48, "56 maps to bucket 48");
	TEST_ASSERT_EQ(encode_fieldnorm(60), 49, "60 maps to bucket 49");

	return 0;
}

/* Test relative error stays reasonable for large values */
static int
test_relative_error(void)
{
	/*
	 * SmallFloat uses 4-bit mantissa giving ~6% typical error, but specific
	 * values near bucket boundaries can have up to ~12% error. We use 15%
	 * as a safe upper bound.
	 */
	for (uint32 len = 100; len < 1000000; len = len * 3 / 2)
	{
		uint8  encoded = encode_fieldnorm(len);
		uint32 decoded = decode_fieldnorm(encoded);

		/* decoded should be <= len (we find largest bucket <= len) */
		TEST_ASSERT(decoded <= len, "decoded <= original");

		/* Relative error check - allow up to 15% for edge cases */
		double error = (double)(len - decoded) / (double)len;
		TEST_ASSERT(error < 0.15, "relative error < 15%");
	}
	return 0;
}

int
main(void)
{
	int passed = 0, failed = 0, total = 0;

	printf("Running fieldnorm tests...\n");

	RUN_TEST(test_exact_values);
	RUN_TEST(test_encode_monotonic);
	RUN_TEST(test_decode_monotonic);
	RUN_TEST(test_encode_range);
	RUN_TEST(test_decode_boundaries);
	RUN_TEST(test_decode_encode_identity);
	RUN_TEST(test_encode_finds_correct_bucket);
	RUN_TEST(test_relative_error);

	printf("\nFieldnorm: %d/%d tests passed", passed, total);
	if (failed > 0)
		printf(" (%d FAILED)", failed);
	printf("\n");

	return failed > 0 ? 1 : 0;
}
