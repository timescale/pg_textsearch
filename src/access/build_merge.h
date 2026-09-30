/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_merge.h - Streaming final merge for parallel index builds
 */
#pragma once

#include <postgres.h>

#include "segment/merge.h"
#include "segment/merge_internal.h"

extern void tp_write_parallel_build_merge(
		TpMergeSink	  *sink,
		TpMergeSource *sources,
		uint32		   num_sources,
		uint64		   total_docs,
		uint64		   total_tokens);
