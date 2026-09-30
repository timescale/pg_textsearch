/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include <tsearch/ts_type.h>

extern TSVector
tp_make_tsvector(text *input, Oid text_config_oid, int max_token_length);
