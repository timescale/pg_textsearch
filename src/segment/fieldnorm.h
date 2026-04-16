/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * fieldnorm.h - Document length quantization for BM25 scoring
 *
 * Uses Lucene's SmallFloat encoding scheme which maps document lengths
 * to a single byte (256 buckets). This reduces storage while maintaining
 * good BM25 ranking quality because:
 * - BM25 uses the ratio dl/avgdl, not absolute length
 * - Small errors become smaller in the ratio
 * - The b parameter (0.75) further dampens length's influence
 *
 * Key properties:
 * - Lengths 0-39 stored exactly (covers most short documents)
 * - Larger lengths use 4-bit mantissa (~6% relative error max)
 * - 256 buckets cover lengths from 0 to 2+ billion
 */
#pragma once

#include "postgres.h"

extern const uint32 FIELDNORM_DECODE_TABLE[256];
extern uint8		encode_fieldnorm(uint32 length);
extern uint32		decode_fieldnorm(uint8 norm_id);
