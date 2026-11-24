/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#include <postgres.h>

#include "../../src/scoring/bmw.c"
#include "../../src/segment/fieldnorm.c"
#undef printf

static float4
reference_score(
		float4 idf,
		int32  tf,
		int32  length,
		float4 k1,
		float4 b,
		float4 average)
{
	float4 norm		  = 1.0f - b + b * ((float4)length / average);
	float4 saturation = ((float4)tf * (k1 + 1.0f)) / ((float4)tf + k1 * norm);

	return idf * saturation;
}

int
main(void)
{
	const float4   k1_values[]	 = {0.8f, 1.2f, 2.1f};
	const float4   b_values[]	 = {0.0f, 0.75f, 1.0f};
	const float4   averages[]	 = {1.0f, 7.0f, 1000.0f};
	const uint32   frequencies[] = {1, 4, 31, 1000};
	TpBlockPosting postings[2]	 = {0};
	TpTermState	   states[2]	 = {0};
	TpTermState	  *terms[]		 = {&states[0], &states[1]};
	float4		   norms[lengthof(FIELDNORM_DECODE_TABLE)];
	/* Keep the reference runtime-evaluated under FMA and LTO. */
	float4 (*volatile reference)(
			float4, int32, int32, float4, float4, float4) = reference_score;

	for (int i = 0; i < 2; i++)
	{
		states[i].found				  = true;
		states[i].idf				  = i == 0 ? 1.23f : 0.41f;
		states[i].query_freq		  = i == 0 ? 1 : 3;
		states[i].iter.block_postings = &postings[i];
	}

	for (int k = 0; k < (int)lengthof(k1_values); k++)
		for (int b = 0; b < (int)lengthof(b_values); b++)
			for (int a = 0; a < (int)lengthof(averages); a++)
			{
				for (int n = 0; n < (int)lengthof(norms); n++)
					norms[n] = 1.0f - b_values[b] +
							   b_values[b] * ((float4)(int32)decode_fieldnorm(
													  (uint8)n) /
											  averages[a]);

				for (int n = 0; n < (int)lengthof(norms); n++)
					for (int f = 0; f < (int)lengthof(frequencies); f++)
					{
						float4 expected = 0.0f;
						float4 actual;

						for (int i = 0; i < 2; i++)
						{
							float4 term_score;

							postings[i].fieldnorm = (uint8)(i == 0 ? n
																   : 255 - n);
							postings[i].frequency = frequencies[f];
							term_score =
									reference(
											states[i].idf,
											postings[i].frequency,
											(int32)decode_fieldnorm(
													postings[i].fieldnorm),
											k1_values[k],
											b_values[b],
											averages[a]) *
									states[i].query_freq;
							expected += term_score;
						}
						actual = score_pivot_document(
								terms, 2, k1_values[k], norms);
						if (memcmp(&actual, &expected, sizeof(actual)) != 0)
						{
							printf("k1=%a b=%a average=%a norm=%d tf=%u: "
								   "actual=%a expected=%a\n",
								   (double)k1_values[k],
								   (double)b_values[b],
								   (double)averages[a],
								   n,
								   frequencies[f],
								   (double)actual,
								   (double)expected);
							return 1;
						}
					}
			}
	puts("BMW normalization traversal tests passed");
	return 0;
}
