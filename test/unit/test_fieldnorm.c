/*
 * test_fieldnorm.c - Unit tests for fieldnorm encoding/decoding
 *
 * Tests the Lucene SmallFloat encoding that maps document lengths to bytes.
 */
#include "pg_stubs.h"

/*
 * Copy the fieldnorm decode table and functions here to avoid postgres.h
 * dependency. This must stay in sync with src/segment/fieldnorm.h.
 */
static const uint32 FIELDNORM_DECODE_TABLE[256] = {
	0,			1,			2,			3,			4,			5,
	6,			7,			8,			9,			10,			11,
	12,			13,			14,			15,			16,			17,
	18,			19,			20,			21,			22,			23,
	24,			25,			26,			27,			28,			29,
	30,			31,			32,			33,			34,			35,
	36,			37,			38,			39,			40,			42,
	44,			46,			48,			50,			52,			54,
	56,			60,			64,			68,			72,			76,
	80,			84,			88,			96,			104,		112,
	120,		128,		136,		144,		152,		168,
	184,		200,		216,		232,		248,		264,
	280,		312,		344,		376,		408,		440,
	472,		504,		536,		600,		664,		728,
	792,		856,		920,		984,		1048,		1176,
	1304,		1432,		1560,		1688,		1816,		1944,
	2072,		2328,		2584,		2840,		3096,		3352,
	3608,		3864,		4120,		4632,		5144,		5656,
	6168,		6680,		7192,		7704,		8216,		9240,
	10264,		11288,		12312,		13336,		14360,		15384,
	16408,		18456,		20504,		22552,		24600,		26648,
	28696,		30744,		32792,		36888,		40984,		45080,
	49176,		53272,		57368,		61464,		65560,		73752,
	81944,		90136,		98328,		106520,		114712,		122904,
	131096,		147480,		163864,		180248,		196632,		213016,
	229400,		245784,		262168,		294936,		327704,		360472,
	393240,		426008,		458776,		491544,		524312,		589848,
	655384,		720920,		786456,		851992,		917528,		983064,
	1048600,	1179672,	1310744,	1441816,	1572888,	1703960,
	1835032,	1966104,	2097176,	2359320,	2621464,	2883608,
	3145752,	3407896,	3670040,	3932184,	4194328,	4718616,
	5242904,	5767192,	6291480,	6815768,	7340056,	7864344,
	8388632,	9437208,	10485784,	11534360,	12582936,	13631512,
	14680088,	15728664,	16777240,	18874392,	20971544,	23068696,
	25165848,	27263000,	29360152,	31457304,	33554456,	37748760,
	41943064,	46137368,	50331672,	54525976,	58720280,	62914584,
	67108888,	75497496,	83886104,	92274712,	100663320,	109051928,
	117440536,	125829144,	134217752,	150994968,	167772184,	184549400,
	201326616,	218103832,	234881048,	251658264,	268435480,	301989912,
	335544344,	369098776,	402653208,	436207640,	469762072,	503316504,
	536870936,	603979800,	671088664,	738197528,	805306392,	872415256,
	939524120,	1006632984, 1073741848, 1207959576, 1342177304, 1476395032,
	1610612760, 1744830488, 1879048216, 2013265944};

static inline uint8
encode_fieldnorm(uint32 length)
{
	int lo = 0;
	int hi = 255;

	while (lo < hi)
	{
		int mid = (lo + hi + 1) / 2;
		if (FIELDNORM_DECODE_TABLE[mid] <= length)
			lo = mid;
		else
			hi = mid - 1;
	}
	return (uint8)lo;
}

static inline uint32
decode_fieldnorm(uint8 norm_id)
{
	return FIELDNORM_DECODE_TABLE[norm_id];
}

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
