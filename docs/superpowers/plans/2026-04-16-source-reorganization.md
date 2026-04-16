# Source Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure `src/` into a layered directory layout that
communicates architecture at a glance, with standardized include
paths.

**Architecture:** Rename three directories (`am/` -> `access/`,
`query/` -> `scoring/`, `state/` -> `index/`), move `source.c/h` into
`index/`, split two overstuffed headers (`segment.h`, `fieldnorm.h`),
rename `segment_io.h` -> `io.h`, rewrite all ~200 project-local
includes to use full `src/`-relative paths, update the Makefile and
CONTRIBUTING.md.

**Tech Stack:** C, Postgres PGXS build system, clang-format, git

**Prerequisite:** The four preliminary PRs described in the design
spec (extract business logic from types, split am/scan.c concerns,
deduplicate dictionary code) must be merged first. This plan assumes
that work is done.

---

## File Map

**New directories created:**
- `src/access/` (replaces `src/am/`)
- `src/scoring/` (replaces `src/query/`)
- `src/index/` (replaces `src/state/`)

**Files moved (git mv):**
- `src/am/*` -> `src/access/*`
- `src/query/bmw.c` -> `src/scoring/bmw.c`
- `src/query/bmw.h` -> `src/scoring/bmw.h`
- `src/query/score.c` -> `src/scoring/bm25.c`
- `src/query/score.h` -> `src/scoring/bm25.h`
- `src/state/*` -> `src/index/*`
- `src/source.c` -> `src/index/source.c`
- `src/source.h` -> `src/index/source.h`

**Files renamed (git mv):**
- `src/segment/segment_io.h` -> `src/segment/io.h`

**Files split:**
- `src/segment/segment.h` -> `src/segment/format.h` + `src/segment/segment.h`
- `src/segment/fieldnorm.h` -> `src/segment/fieldnorm.c` + `src/segment/fieldnorm.h`

**Files modified (include path updates only):**
- Every `.c` and `.h` file under `src/` that has project-local includes

---

### Task 1: Create branch and new directories

**Files:**
- Create: `src/access/` (directory)
- Create: `src/scoring/` (directory)
- Create: `src/index/` (directory)

- [ ] **Step 1: Create a feature branch**

```bash
git checkout -b refactor/source-reorganization
```

- [ ] **Step 2: Verify clean starting state**

```bash
git status
make clean && make && make installcheck
```

Expected: Clean working tree, build succeeds, all tests pass. This
is the baseline.

- [ ] **Step 3: Create destination directories**

```bash
mkdir -p src/access src/scoring src/index
```

---

### Task 2: Move files with git mv

All moves use `git mv` to preserve history. Order matters: move
whole directories first, then individual files, then renames.

**Files:**
- Move: `src/am/*` -> `src/access/`
- Move: `src/query/*` -> `src/scoring/`
- Move: `src/state/*` -> `src/index/`
- Move: `src/source.c` -> `src/index/source.c`
- Move: `src/source.h` -> `src/index/source.h`
- Rename: `src/scoring/score.c` -> `src/scoring/bm25.c`
- Rename: `src/scoring/score.h` -> `src/scoring/bm25.h`
- Rename: `src/segment/segment_io.h` -> `src/segment/io.h`

- [ ] **Step 1: Move am/ -> access/**

```bash
git mv src/am/handler.c src/access/handler.c
git mv src/am/am.h src/access/am.h
git mv src/am/build.c src/access/build.c
git mv src/am/build_context.c src/access/build_context.c
git mv src/am/build_context.h src/access/build_context.h
git mv src/am/build_parallel.c src/access/build_parallel.c
git mv src/am/build_parallel.h src/access/build_parallel.h
git mv src/am/scan.c src/access/scan.c
git mv src/am/vacuum.c src/access/vacuum.c
```

- [ ] **Step 2: Move query/ -> scoring/ (with rename)**

```bash
git mv src/query/bmw.c src/scoring/bmw.c
git mv src/query/bmw.h src/scoring/bmw.h
git mv src/query/score.c src/scoring/bm25.c
git mv src/query/score.h src/scoring/bm25.h
```

- [ ] **Step 3: Move state/ -> index/**

```bash
git mv src/state/state.c src/index/state.c
git mv src/state/state.h src/index/state.h
git mv src/state/registry.c src/index/registry.c
git mv src/state/registry.h src/index/registry.h
git mv src/state/metapage.c src/index/metapage.c
git mv src/state/metapage.h src/index/metapage.h
git mv src/state/memory.c src/index/memory.c
git mv src/state/memory.h src/index/memory.h
git mv src/state/limit.c src/index/limit.c
git mv src/state/limit.h src/index/limit.h
```

- [ ] **Step 4: Move source.c/h into index/**

```bash
git mv src/source.c src/index/source.c
git mv src/source.h src/index/source.h
```

- [ ] **Step 5: Rename segment_io.h -> io.h**

```bash
git mv src/segment/segment_io.h src/segment/io.h
```

- [ ] **Step 6: Remove empty old directories**

```bash
rmdir src/am src/query src/state
```

- [ ] **Step 7: Verify git status looks right**

```bash
git status
```

Expected: All files show as "renamed", no untracked files, no
deleted files that aren't matched by a rename.

---

### Task 3: Split segment/segment.h into format.h + segment.h

Extract on-disk format definitions into `segment/format.h`.
`segment.h` keeps runtime types and API, and includes `format.h`.

**Files:**
- Create: `src/segment/format.h`
- Modify: `src/segment/segment.h`

- [ ] **Step 1: Create segment/format.h**

Create `src/segment/format.h` with all on-disk format definitions
extracted from `segment.h`. This file gets:

- Copyright header and `#pragma once`
- Minimal includes: `<postgres.h>`, `"storage/itemptr.h"`,
  `"utils/timestamp.h"`
- `TpPageType` enum (lines 44-49 of original)
- `TpPageIndexSpecial` struct (lines 55-63)
- Format version constants (lines 68-70)
- `TpSegmentHeaderV3` struct (lines 76-97)
- `TpSegmentHeader` struct (lines 102-138)
- `TpDictionary` struct (lines 148-153)
- `TpStringEntry` struct (lines 166-171)
- `TpDictEntryV3` struct (lines 176-182)
- `TpDictEntry` struct (lines 197-202)
- `TP_BLOCK_SIZE` constant (line 223)
- `TpSkipEntryV3` struct (lines 228-237)
- `TpSkipEntry` struct (lines 245-254)
- Skip entry flag constants (lines 257-260)
- `TpBlockPosting` struct (lines 288-294)
- `TpCtidMapEntry` struct (lines 303-306)

```c
/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * format.h - On-disk segment format definitions
 *
 * Defines the structures that are serialized to disk in segment
 * files. This includes segment headers, dictionary entries, skip
 * index entries, block postings, and CTID mappings across all
 * format versions (V3 legacy through current V5).
 */
#pragma once

#include <postgres.h>

#include "storage/itemptr.h"
#include "utils/timestamp.h"

/*
 * Page types for segment pages.
 * Magic and version constants are defined in constants.h.
 */
typedef enum TpPageType
{
	TP_PAGE_SEGMENT_HEADER = 1, /* Segment header page */
	TP_PAGE_FILE_INDEX,         /* File index (page map) pages */
	TP_PAGE_DATA                /* Data pages containing actual
	                               content */
} TpPageType;

/*
 * Special area for page index pages (goes in page special area).
 * Contains magic/version for validation.
 */
typedef struct TpPageIndexSpecial
{
	uint32      magic;       /* TP_PAGE_INDEX_MAGIC */
	uint16      version;     /* TP_PAGE_INDEX_VERSION */
	uint16      page_type;   /* TpPageType enum value */
	BlockNumber next_page;   /* Next page in chain,
	                            InvalidBlockNumber if last */
	uint16      num_entries; /* Number of BlockNumber entries on
	                            this page */
	uint16      flags;       /* Reserved for future use */
} TpPageIndexSpecial;

/*
 * Segment format versions
 */
#define TP_SEGMENT_FORMAT_VERSION_3 3 /* Legacy: uint32 offsets */
#define TP_SEGMENT_FORMAT_VERSION_4 4 /* Legacy: no alive bitset */
#define TP_SEGMENT_FORMAT_VERSION   5 /* Current: alive bitset */

/* --- V3 legacy segment header --- */

typedef struct TpSegmentHeaderV3
{
	uint32      magic;
	uint32      version;
	TimestampTz created_at;
	uint32      num_pages;
	uint32      data_size;
	uint32      level;
	BlockNumber next_segment;
	uint32      dictionary_offset;
	uint32      strings_offset;
	uint32      entries_offset;
	uint32      postings_offset;
	uint32      skip_index_offset;
	uint32      fieldnorm_offset;
	uint32      ctid_pages_offset;
	uint32      ctid_offsets_offset;
	uint32      num_terms;
	uint32      num_docs;
	uint64      total_tokens;
	BlockNumber page_index;
} TpSegmentHeaderV3;

/* --- Current segment header (V4+: uint64 offsets) --- */

typedef struct TpSegmentHeader
{
	/* Metadata */
	uint32      magic;        /* TP_SEGMENT_MAGIC */
	uint32      version;      /* Format version */
	TimestampTz created_at;   /* Creation time */
	uint32      num_pages;    /* Total pages in segment */
	uint64      data_size;    /* Total data bytes */

	/* Segment management */
	uint32      level;          /* Storage level (0 for memtable
	                               flush) */
	BlockNumber next_segment;   /* Next segment in chain */

	/* Section offsets in logical file */
	uint64 dictionary_offset;   /* Offset to TpDictionary */
	uint64 strings_offset;      /* Offset to string pool */
	uint64 entries_offset;      /* Offset to TpDictEntry array */
	uint64 postings_offset;     /* Offset to posting data
	                               (blocks) */

	/* Block storage offsets */
	uint64 skip_index_offset;   /* Offset to skip index */
	uint64 fieldnorm_offset;    /* Offset to fieldnorm table */
	uint64 ctid_pages_offset;   /* Offset to BlockNumber array */
	uint64 ctid_offsets_offset; /* Offset to OffsetNumber array */

	/* Alive bitset for tombstone tracking (V5+) */
	uint64 alive_bitset_offset; /* Offset to alive bitset data */
	uint32 alive_count;         /* Number of alive docs (bits
	                               set) */

	/* Corpus statistics */
	uint32 num_terms;    /* Total unique terms */
	uint32 num_docs;     /* Total documents */
	uint64 total_tokens; /* Sum of all document lengths */

	/* Page index reference */
	BlockNumber page_index; /* First page of the page index */
} TpSegmentHeader;

/* --- Dictionary --- */

typedef struct TpDictionary
{
	uint32 num_terms;
	uint32 string_offsets[FLEXIBLE_ARRAY_MEMBER];
} TpDictionary;

typedef struct TpStringEntry
{
	uint32 length;
	char   text[FLEXIBLE_ARRAY_MEMBER];
	/* Immediately after text: uint32 dict_entry_offset */
} TpStringEntry;

/* V3 legacy dictionary entry - 12 bytes */
typedef struct TpDictEntryV3
{
	uint32 skip_index_offset;
	uint16 block_count;
	uint16 reserved;
	uint32 doc_freq;
} __attribute__((aligned(4))) TpDictEntryV3;

/*
 * Dictionary entry - 16 bytes, block-based storage
 * (V4: uint64 offset)
 */
typedef struct TpDictEntry
{
	uint64 skip_index_offset;
	uint32 block_count;
	uint32 doc_freq;
} __attribute__((aligned(8))) TpDictEntry;

/* --- Block storage --- */

#define TP_BLOCK_SIZE 128 /* Documents per block */

/* V3 legacy skip index entry - 16 bytes per block */
typedef struct TpSkipEntryV3
{
	uint32 last_doc_id;
	uint8  doc_count;
	uint16 block_max_tf;
	uint8  block_max_norm;
	uint32 posting_offset;
	uint8  flags;
	uint8  reserved[3];
} __attribute__((packed)) TpSkipEntryV3;

/* Skip index entry - 20 bytes per block (V4+: uint64 offset) */
typedef struct TpSkipEntry
{
	uint32 last_doc_id;
	uint8  doc_count;
	uint16 block_max_tf;
	uint8  block_max_norm;
	uint64 posting_offset;
	uint8  flags;
	uint8  reserved[3];
} __attribute__((packed)) TpSkipEntry;

/* Skip entry flags */
#define TP_BLOCK_FLAG_UNCOMPRESSED 0x00
#define TP_BLOCK_FLAG_DELTA        0x01
#define TP_BLOCK_FLAG_FOR          0x02
#define TP_BLOCK_FLAG_PFOR         0x03

/*
 * Block posting entry - 8 bytes, used in uncompressed blocks
 */
typedef struct TpBlockPosting
{
	uint32 doc_id;
	uint16 frequency;
	uint8  fieldnorm;
	uint8  reserved;
} TpBlockPosting;

/*
 * CTID map entry - 6 bytes per document
 */
typedef struct TpCtidMapEntry
{
	ItemPointerData ctid;
} __attribute__((packed)) TpCtidMapEntry;
```

- [ ] **Step 2: Rewrite segment/segment.h to include format.h**

Replace the entire file with the remaining runtime types and API:

```c
/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment.h - Segment operations and runtime types
 */
#pragma once

#include <postgres.h>

#include <access/htup_details.h>
#include <port/atomics.h>
#include <storage/buffile.h>

#include "constants.h"
#include "segment/format.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/timestamp.h"

/*
 * BufFile segment size (1 GB).  BufFile stores data across
 * multiple 1 GB physical files.  Composite offsets encode both
 * fileno and position: fileno * TP_BUFFILE_SEG_SIZE + file_offset.
 */
#define TP_BUFFILE_SEG_SIZE ((uint64)(1024 * 1024 * 1024))

static inline uint64
tp_buffile_composite_offset(int fileno, off_t file_offset)
{
	return (uint64)fileno * TP_BUFFILE_SEG_SIZE
		   + (uint64)file_offset;
}

static inline void
tp_buffile_decompose_offset(uint64   composite,
                            int     *fileno,
                            off_t   *offset)
{
	*fileno = (int)(composite / TP_BUFFILE_SEG_SIZE);
	*offset = (off_t)(composite % TP_BUFFILE_SEG_SIZE);
}

/*
 * Segment posting - output format for iteration
 */
typedef struct TpSegmentPosting
{
	ItemPointerData ctid;
	uint32          doc_id;
	uint16          frequency;
	uint16          doc_length;
} __attribute__((packed)) TpSegmentPosting;

/* Version-aware struct size helpers */
static inline size_t
tp_dict_entry_size(uint32 version)
{
	return (version <= TP_SEGMENT_FORMAT_VERSION_3)
		   ? sizeof(TpDictEntryV3)
		   : sizeof(TpDictEntry);
}

static inline size_t
tp_skip_entry_size(uint32 version)
{
	return (version <= TP_SEGMENT_FORMAT_VERSION_3)
		   ? sizeof(TpSkipEntryV3)
		   : sizeof(TpSkipEntry);
}

/*
 * Document length - 12 bytes (padded to 16)
 */
typedef struct TpDocLength
{
	ItemPointerData ctid;
	uint16          length;
	uint32          reserved;
} TpDocLength;

/* Look up doc_freq for a term from segments */
extern uint32 tp_segment_get_doc_freq(
		Relation index, BlockNumber first_segment,
		const char *term);

/* Batch lookup doc_freq for multiple terms */
extern void tp_batch_get_segment_doc_freq(
		Relation    index,
		BlockNumber first_segment,
		char      **terms,
		int         term_count,
		uint32     *doc_freqs);
```

Note: The exact formatting of these files will be fixed by
`make format` in Task 7. Focus on getting the content right.

- [ ] **Step 3: Run make format on the new files**

```bash
make format-single FILE=src/segment/format.h
make format-single FILE=src/segment/segment.h
```

---

### Task 4: Split segment/fieldnorm.h into fieldnorm.c + fieldnorm.h

Move the 256-entry static decode table from the header into a .c
file. The header shrinks from 348 lines to ~30.

**Files:**
- Create: `src/segment/fieldnorm.c`
- Modify: `src/segment/fieldnorm.h`

- [ ] **Step 1: Create segment/fieldnorm.c**

```c
/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * fieldnorm.c - Fieldnorm decode table and encode/decode functions
 */
#include <postgres.h>

#include "segment/fieldnorm.h"

/*
 * Precomputed decode table (Lucene SmallFloat.byte4ToInt)
 *
 * Generated by: for i in 0..255: decode_table[i] = byte4ToInt(i)
 * Values 0-39 map exactly, larger values use exponential encoding.
 */
const uint32 FIELDNORM_DECODE_TABLE[256] = {
	0, 1, 2, 3, 4, 5, 6, 7,
	8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 30, 31,
	32, 33, 34, 35, 36, 37, 38, 39,
	40, 42, 44, 46, 48, 50, 52, 54,
	56, 60, 64, 68, 72, 76, 80, 84,
	88, 96, 104, 112, 120, 128, 136, 144,
	152, 168, 184, 200, 216, 232, 248, 264,
	280, 312, 344, 376, 408, 440, 472, 504,
	536, 600, 664, 728, 792, 856, 920, 984,
	1048, 1176, 1304, 1432, 1560, 1688, 1816, 1944,
	2072, 2328, 2584, 2840, 3096, 3352, 3608, 3864,
	4120, 4632, 5144, 5656, 6168, 6680, 7192, 7704,
	8216, 9240, 10264, 11288, 12312, 13336, 14360, 15384,
	16408, 18456, 20504, 22552, 24600, 26648, 28696, 30744,
	32792, 36888, 40984, 45080, 49176, 53272, 57368, 61464,
	65560, 73752, 81944, 90136, 98328, 106520, 114712, 122904,
	131096, 147480, 163864, 180248, 196632, 213016, 229400,
	245784,
	262168, 294936, 327704, 360472, 393240, 426008, 458776,
	491544,
	524312, 589848, 655384, 720920, 786456, 851992, 917528,
	983064,
	1048600, 1179672, 1310744, 1441816, 1572888, 1703960,
	1835032, 1966104,
	2097176, 2359320, 2621464, 2883608, 3145752, 3407896,
	3670040, 3932184,
	4194328, 4718616, 5242904, 5767192, 6291480, 6815768,
	7340056, 7864344,
	8388632, 9437208, 10485784, 11534360, 12582936, 13631512,
	14680088, 15728664,
	16777240, 18874392, 20971544, 23068696, 25165848,
	27263000, 29360152, 31457304,
	33554456, 37748760, 41943064, 46137368, 50331672,
	54525976, 58720280, 62914584,
	67108888, 75497496, 83886104, 92274712, 100663320,
	109051928, 117440536, 125829144,
	134217752, 150994968, 167772184, 184549400, 201326616,
	218103832, 234881048, 251658264,
	268435480, 301989912, 335544344, 369098776, 402653208,
	436207640, 469762072, 503316504,
	536870936, 603979800, 671088664, 738197528, 805306392,
	872415256, 939524120, 1006632984,
	1073741848, 1207959576, 1342177304, 1476395032,
	1610612760, 1744830488, 1879048216, 2013265944
};

uint8
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
	return (uint8) lo;
}

uint32
decode_fieldnorm(uint8 norm_id)
{
	return FIELDNORM_DECODE_TABLE[norm_id];
}
```

- [ ] **Step 2: Rewrite segment/fieldnorm.h**

Replace the entire file with just the declarations:

```c
/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * fieldnorm.h - Document length quantization for BM25 scoring
 *
 * Uses Lucene's SmallFloat encoding scheme which maps document
 * lengths to a single byte (256 buckets).
 */
#pragma once

#include "postgres.h"

/* Number of bits for quantization (4-bit mantissa) */
#define FIELDNORM_QUANTIZE_BITS 4

/* Precomputed decode table (defined in fieldnorm.c) */
extern const uint32 FIELDNORM_DECODE_TABLE[256];

/* Encode document length to fieldnorm byte */
extern uint8 encode_fieldnorm(uint32 length);

/* Decode fieldnorm byte back to approximate document length */
extern uint32 decode_fieldnorm(uint8 norm_id);
```

- [ ] **Step 3: Run make format on the new files**

```bash
make format-single FILE=src/segment/fieldnorm.c
make format-single FILE=src/segment/fieldnorm.h
```

---

### Task 5: Rewrite all include paths

Every project-local `#include` must use a full `src/`-relative
path. This task rewrites all ~200 includes across all `.c` and `.h`
files under `src/`.

The rewrites fall into these categories:

1. **Directory renames**: `am/` -> `access/`, `query/` -> `scoring/`,
   `state/` -> `index/`
2. **File renames**: `query/score.h` -> `scoring/bm25.h`,
   `segment/segment_io.h` -> `segment/io.h`
3. **File moves**: `source.h` -> `index/source.h`
4. **Short-form to qualified**: `"segment.h"` -> `"segment/segment.h"`
5. **Relative to qualified**: `"../source.h"` -> `"index/source.h"`
6. **New split header**: Files including `segment/segment.h` that only
   need format defs can switch to `segment/format.h` (optional;
   `segment.h` includes `format.h` so either works)

**Files:** Every `.c` and `.h` under `src/`

- [ ] **Step 1: Run the include rewrite script**

Create and run a shell script that performs all the replacements.
The script uses `sed -i` with the mappings derived from the full
include catalog:

```bash
#!/bin/bash
# rewrite_includes.sh - Rewrite all project-local includes
set -euo pipefail

cd src

# --- Category 1: Directory renames ---
# am/ -> access/
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "am/|#include "access/|g'

# query/ -> scoring/
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "query/|#include "scoring/|g'

# state/ -> index/
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "state/|#include "index/|g'

# --- Category 2: File renames ---
# scoring/score.h -> scoring/bm25.h
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "scoring/score.h"|#include "scoring/bm25.h"|g'

# segment/segment_io.h -> segment/io.h
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "segment_io.h"|#include "segment/io.h"|g'
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "segment/segment_io.h"|#include "segment/io.h"|g'

# --- Category 3: File move ---
# source.h (root or relative) -> index/source.h
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "source.h"|#include "index/source.h"|g'
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "\.\./source.h"|#include "index/source.h"|g'

# --- Category 4: Short-form same-directory includes ---
# These are includes without a directory prefix, used within
# their own directory. Must qualify with directory name.

# segment/ short-form includes
for f in segment/*.c segment/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "alive_bitset.h"|#include "segment/alive_bitset.h"|g' \
    -e 's|#include "compression.h"|#include "segment/compression.h"|g' \
    -e 's|#include "dictionary.h"|#include "segment/dictionary.h"|g' \
    -e 's|#include "docmap.h"|#include "segment/docmap.h"|g' \
    -e 's|#include "fieldnorm.h"|#include "segment/fieldnorm.h"|g' \
    -e 's|#include "merge.h"|#include "segment/merge.h"|g' \
    -e 's|#include "merge_internal.h"|#include "segment/merge_internal.h"|g' \
    -e 's|#include "pagemapper.h"|#include "segment/pagemapper.h"|g' \
    -e 's|#include "segment.h"|#include "segment/segment.h"|g' \
    "$f"
done

# memtable/ short-form includes
for f in memtable/*.c memtable/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "arena.h"|#include "memtable/arena.h"|g' \
    -e 's|#include "expull.h"|#include "memtable/expull.h"|g' \
    -e 's|#include "memtable.h"|#include "memtable/memtable.h"|g' \
    -e 's|#include "posting.h"|#include "memtable/posting.h"|g' \
    -e 's|#include "posting_entry.h"|#include "memtable/posting_entry.h"|g' \
    -e 's|#include "scan.h"|#include "memtable/scan.h"|g' \
    -e 's|#include "stringtable.h"|#include "memtable/stringtable.h"|g' \
    "$f"
done

# access/ short-form includes (was am/)
for f in access/*.c access/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "am.h"|#include "access/am.h"|g' \
    -e 's|#include "build_context.h"|#include "access/build_context.h"|g' \
    -e 's|#include "build_parallel.h"|#include "access/build_parallel.h"|g' \
    "$f"
done

# index/ short-form includes (was state/)
# Note: index/ files already use qualified paths (state/...) which
# were rewritten to index/ by the directory rename step above.
# But check for any bare includes:
for f in index/*.c index/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "state.h"|#include "index/state.h"|g' \
    -e 's|#include "registry.h"|#include "index/registry.h"|g' \
    -e 's|#include "metapage.h"|#include "index/metapage.h"|g' \
    -e 's|#include "memory.h"|#include "index/memory.h"|g' \
    -e 's|#include "limit.h"|#include "index/limit.h"|g' \
    "$f"
done

# scoring/ short-form includes (was query/)
for f in scoring/*.c scoring/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "bmw.h"|#include "scoring/bmw.h"|g' \
    -e 's|#include "score.h"|#include "scoring/bm25.h"|g' \
    -e 's|#include "bm25.h"|#include "scoring/bm25.h"|g' \
    "$f"
done

# planner/ short-form includes
for f in planner/*.c planner/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "hooks.h"|#include "planner/hooks.h"|g' \
    -e 's|#include "cost.h"|#include "planner/cost.h"|g' \
    "$f"
done

# types/ short-form includes
for f in types/*.c types/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "array.h"|#include "types/array.h"|g' \
    -e 's|#include "query.h"|#include "types/query.h"|g' \
    -e 's|#include "vector.h"|#include "types/vector.h"|g' \
    "$f"
done

# debug/ short-form includes
for f in debug/*.c debug/*.h; do
  [ -f "$f" ] || continue
  sed -i '' \
    -e 's|#include "dump.h"|#include "debug/dump.h"|g' \
    "$f"
done

# --- Category 5: Relative path includes ---
# ../source.h already handled above
# ../state/state.h -> index/state.h
find . -name '*.c' -o -name '*.h' | xargs \
  sed -i '' 's|#include "\.\./state/state.h"|#include "index/state.h"|g'

# --- Fix: memtable/stringtable.c includes "memory.h" ---
# This is state/memory.h (now index/memory.h), not a
# same-directory include. The memtable/ loop above won't match it
# since there's no memtable/memory.h. Handle explicitly:
sed -i '' \
  's|#include "memory.h"|#include "index/memory.h"|g' \
  memtable/stringtable.c

cd ..
echo "Include rewrite complete."
```

Run the script:

```bash
chmod +x rewrite_includes.sh
bash rewrite_includes.sh
```

- [ ] **Step 2: Verify no stale includes remain**

```bash
# Check for any remaining relative includes
grep -rn '#include "\.\.' src/ || echo "No relative includes found"

# Check for any short-form includes that should be qualified
# (project headers without a / in the path, excluding postgres.h
# and constants.h which live at root)
grep -rn '#include "' src/ \
  | grep -v '<' \
  | grep -v 'postgres.h' \
  | grep -v 'constants.h' \
  | grep -v '/' \
  || echo "All includes are qualified"
```

Expected: No matches for relative includes. No unqualified
project includes except `constants.h` (which lives at `src/` root
and has no directory prefix — this is correct since `-I` points to
`src/`).

- [ ] **Step 3: Spot-check key files**

Manually inspect a few files to verify includes look right:

```bash
head -35 src/mod.c
head -25 src/scoring/bmw.c
head -20 src/segment/merge.c
head -20 src/memtable/source.c
```

Verify: all project includes use `"directory/file.h"` format with
the new directory names.

---

### Task 6: Update Makefile

Update the `OBJS` variable and add `fieldnorm.o` as a new
compilation unit.

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Rewrite OBJS variable**

Replace the OBJS block (lines 22-57) with new paths:

```makefile
# Source files organized by directory
OBJS = \
	src/mod.o \
	src/access/handler.o \
	src/access/build.o \
	src/access/build_context.o \
	src/access/build_parallel.o \
	src/access/scan.o \
	src/access/vacuum.o \
	src/memtable/arena.o \
	src/memtable/expull.o \
	src/memtable/memtable.o \
	src/memtable/posting.o \
	src/memtable/stringtable.o \
	src/memtable/scan.o \
	src/memtable/source.o \
	src/segment/segment.o \
	src/segment/dictionary.o \
	src/segment/scan.o \
	src/segment/merge.o \
	src/segment/docmap.o \
	src/segment/alive_bitset.o \
	src/segment/compression.o \
	src/segment/fieldnorm.o \
	src/scoring/bmw.o \
	src/scoring/bm25.o \
	src/types/array.o \
	src/types/vector.o \
	src/types/query.o \
	src/index/state.o \
	src/index/registry.o \
	src/index/metapage.o \
	src/index/limit.o \
	src/index/memory.o \
	src/index/source.o \
	src/planner/hooks.o \
	src/planner/cost.o \
	src/debug/dump.o
```

Key changes from the original:
- `src/source.o` removed (moved to `src/index/source.o`)
- `src/am/*` -> `src/access/*`
- `src/query/*` -> `src/scoring/*` (score.o -> bm25.o)
- `src/state/*` -> `src/index/*`
- `src/index/source.o` added
- `src/segment/fieldnorm.o` added (new .c file from split)

---

### Task 7: Update CONTRIBUTING.md

Add a "Source Code Architecture" section documenting the layered
structure, dependency rules, and include conventions.

**Files:**
- Modify: `CONTRIBUTING.md`

- [ ] **Step 1: Add architecture section after "Code Style"**

Insert the following after the Code Style section (after line 64,
before "### Testing Requirements"):

```markdown
### Source Code Architecture

The `src/` directory is organized into layers. The directory
structure communicates the dependency flow: upper layers depend on
lower layers, not the reverse.

**Layer 1 — Postgres interface:**
- `access/` — Access method (handler, build, scan, vacuum)
- `types/` — SQL types (`bm25query`, `bm25vector`) and operators
- `planner/` — Query optimizer hooks and cost estimation

**Layer 2 — Index coordination:**
- `scoring/` — BM25 score computation and Block-Max WAND
  optimization
- `index/` — Index lifecycle, shared state registry, metadata
  pages, posting source abstraction

**Layer 3 — Storage:**
- `memtable/` — In-memory inverted index (shared memory, DSA)
- `segment/` — On-disk segments, merge/compaction, compression

**Cross-cutting:**
- `debug/` — Index dump utilities (exempt from layering rules)
- `mod.c` — Extension init and GUC registration

**Dependency rules:**
- Layer 1 may depend on Layer 2 and Layer 3
- Layer 2 may depend on Layer 3
- Layer 3 should not depend on Layer 1 or Layer 2

These rules are enforced by convention and code review, not
mechanically.

### Include Path Convention

All project-local `#include` directives use full paths relative to
`src/`:

```c
#include "segment/segment.h"   /* correct */
#include "index/source.h"     /* correct */
#include "segment.h"          /* wrong: missing directory */
#include "../source.h"        /* wrong: relative path */
```

Postgres system includes (`<postgres.h>`, `<access/generic_xlog.h>`,
etc.) are unaffected.

### File Size

Split files based on responsibility, not line count. A 1500-line
file with a single clear purpose is fine. A 400-line file doing
three unrelated things should be split.
```

---

### Task 8: Update CLAUDE.md source structure

The project CLAUDE.md has a "Source Code Structure" section that
references the old directory names. Update it.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update the directory tree in CLAUDE.md**

Find the "Source Code Structure" section and update the directory
tree and descriptions to reflect the new layout:
- `am/` -> `access/`
- `query/` -> `scoring/`
- `state/` -> `index/`
- `source.c/h` now in `index/`
- `score.c/h` now `bm25.c/h` in `scoring/`
- `segment/segment_io.h` now `segment/io.h`
- Add `segment/format.h` and `segment/fieldnorm.c`

---

### Task 9: Build, test, and verify

**Files:** None (verification only)

- [ ] **Step 1: Clean build**

```bash
make clean && make
```

Expected: Compiles with no errors and no new warnings.

- [ ] **Step 2: Run all tests**

```bash
make installcheck
```

Expected: All regression tests pass. Check `test/regression.diffs`
is empty.

- [ ] **Step 3: Check formatting**

```bash
make format-check
```

Expected: No formatting issues.

- [ ] **Step 4: Final include audit**

```bash
# No relative includes
grep -rn '#include "\.\.' src/ && echo "FAIL: relative includes" \
  || echo "OK: no relative includes"

# No stale old directory names in includes
grep -rn '"am/' src/ && echo "FAIL: stale am/" \
  || echo "OK: no am/ includes"
grep -rn '"query/' src/ && echo "FAIL: stale query/" \
  || echo "OK: no query/ includes"
grep -rn '"state/' src/ && echo "FAIL: stale state/" \
  || echo "OK: no state/ includes"
grep -rn '"segment_io.h"' src/ && echo "FAIL: stale segment_io" \
  || echo "OK: no segment_io includes"
grep -rn '"score.h"' src/ && echo "FAIL: stale score.h" \
  || echo "OK: no score.h includes"

# No unqualified project includes (except constants.h)
grep -rn '#include "' src/ \
  | grep -v '<' \
  | grep -v 'postgres.h' \
  | grep -v 'constants.h' \
  | grep -Pv '"[^"]*/' \
  && echo "FAIL: unqualified includes found" \
  || echo "OK: all includes qualified"
```

Expected: All checks pass.

- [ ] **Step 5: Verify old directories are gone**

```bash
ls -d src/am src/query src/state 2>&1
```

Expected: "No such file or directory" for all three.

---

### Task 10: Commit

- [ ] **Step 1: Stage all changes**

```bash
git add -A
git status
```

Review the status: should show renames (recognized by git),
modified files (include path changes), new files (format.h,
fieldnorm.c), and modified Makefile/CONTRIBUTING.md/CLAUDE.md.

- [ ] **Step 2: Commit**

```bash
git commit -m "refactor: reorganize src/ into layered architecture

Rename directories to communicate architecture:
- am/ -> access/ (Postgres access method convention)
- query/ -> scoring/ (BM25 and BMW, not query parsing)
- state/ -> index/ (index coordination, not generic state)

Move source.c/h into index/ (posting source abstraction).
Rename score.c/h -> bm25.c/h, segment_io.h -> io.h.

Split segment/segment.h: on-disk format defs -> format.h.
Split segment/fieldnorm.h: decode table -> fieldnorm.c.

Standardize all project includes to full src/-relative paths.
Update Makefile, CONTRIBUTING.md, and CLAUDE.md."
```

- [ ] **Step 3: Create draft PR**

```bash
git push -u origin refactor/source-reorganization
gh pr create --draft \
  --title "refactor: reorganize src/ into layered architecture" \
  --body "$(cat <<'PREOF'
## Summary

Restructures the src/ directory into a layered architecture that
communicates the system design at a glance.

- Rename am/ -> access/, query/ -> scoring/, state/ -> index/
- Move source.c/h into index/
- Split segment/segment.h (format defs -> format.h)
- Split segment/fieldnorm.h (decode table -> fieldnorm.c)
- Standardize all includes to full src/-relative paths
- Document architecture and conventions in CONTRIBUTING.md

Purely structural: no behavioral changes. See the design spec at
docs/superpowers/specs/2026-04-16-source-reorganization-design.md.

## Testing

- make clean && make (compiles cleanly)
- make installcheck (all regression + shell tests pass)
- make format-check (formatting clean)
- Audited: no stale includes, no relative paths, no old directory
  names
PREOF
)"
```
