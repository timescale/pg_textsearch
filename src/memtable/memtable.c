/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable.c - In-memory inverted index implementation
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "memtable.h"
