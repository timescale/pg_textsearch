# Segments Design

## Overview

Segments are immutable, disk-based structures that store the inverted index when the memtable is flushed. This design uses a page mapping table to handle non-sequential page allocation in Postgres.

## Architecture

### Core Design: Page Mapping Table

Since Postgres doesn't guarantee sequential page allocation, we store a logical → physical page mapping directly in the segment header. This avoids page traversal on open and provides O(1) page access.

```
Physical Page Layout (non-sequential allocation in Postgres):
┌──────────────────────────────────────────────────────────────┐
│ Block 42: Segment Header Page                               │
│ ┌──────────────────────────────────────────────────────────┐│
│ │ TpSegmentHeader                                          ││
│ │ • magic, version, timestamps                             ││
│ │ • section offsets (dictionary, strings, entries, etc.)   ││
│ │ • corpus statistics (num_terms, num_docs, total_tokens)  ││
│ │ • page_index → Block 89 (first page index page)          ││
│ └──────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Block 89: Page Index Page #1                                │
│ ┌──────────────────────────────────────────────────────────┐│
│ │ PageHeaderData                                           ││
│ ├──────────────────────────────────────────────────────────┤│
│ │ BlockNumber[0] = 107   ─────┐                           ││
│ │ BlockNumber[1] = 213   ─────┼───┐                       ││
│ │ BlockNumber[2] = 156   ─────┼───┼───┐                   ││
│ │ ...                         │   │   │                   ││
│ │ BlockNumber[2038] = 431     │   │   │                   ││
│ ├──────────────────────────────────────────────────────────┤│
│ │ TpPageIndexSpecial:         │   │   │                   ││
│ │ • next_page = 245 ──────────┼───┼───┼──┐                ││
│ │ • num_entries = 2039        │   │   │  │                ││
│ └──────────────────────────────────────────────────────────┘│
└─────────────────────────────────│───│───│──│────────────────┘
                                  ↓   ↓   ↓  ↓
┌──────────────────────────────────────────────────────────────┐
│ Block 107: Data Page (logical page 0)                       │
│ ┌──────────────────────────────────────────────────────────┐│
│ │ Dictionary section begins here...                        ││
│ └──────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Block 213: Data Page (logical page 1)                       │
│ ┌──────────────────────────────────────────────────────────┐│
│ │ ...dictionary continues...                               ││
│ └──────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────┘
```

### Segment File Layout

The logical segment file contains these sections in order:

```
Logical File Layout (continuous address space):
┌────────────────────────────────────────────────────────────┐
│ Offset 0x0000: TpDictionary                               │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ num_terms = 3                                        │ │
│ │ string_offsets[0] = 0x0100  ──────┐                  │ │
│ │ string_offsets[1] = 0x0120  ──────┼──┐               │ │
│ │ string_offsets[2] = 0x0140  ──────┼──┼──┐            │ │
│ └──────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ Offset 0x0100: String Pool                                │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ ┌─String Entry "apple"──────────┐ ←──┘  │            │ │
│ │ │ length: 5                      │       │            │ │
│ │ │ text: "apple"                  │       │            │ │
│ │ │ dict_entry_offset: 0x0300 ─────┼───────┼────────┐   │ │
│ │ └────────────────────────────────┘       │        │   │ │
│ │ ┌─String Entry "banana"─────────┐  ←─────┘        │   │ │
│ │ │ length: 6                     │                 │   │ │
│ │ │ text: "banana"                │                 │   │ │
│ │ │ dict_entry_offset: 0x030C ────┼─────────────────┼─┐ │ │
│ │ └───────────────────────────────┘                 │ │ │ │
│ │ ┌─String Entry "cherry"─────────┐  ←──────────────┘ │ │ │
│ │ │ length: 6                     │                   │ │ │
│ │ │ text: "cherry"                │                   │ │ │
│ │ │ dict_entry_offset: 0x0318 ────┼───────────────────┼┼┐│ │
│ │ └───────────────────────────────┘                   ││││ │
│ └──────────────────────────────────────────────────────┘│││ │
├────────────────────────────────────────────────────────────┤
│ Offset 0x0300: Dictionary Entries (fixed 12 bytes each)   ││ │
│ ┌──────────────────────────────────────────────────────┐ ││ │
│ │ TpDictEntry[0]: "apple"  ←───────────────────────────┼─┘│ │
│ │   posting_offset: 0x0400 ────────┐                  │  │ │
│ │   posting_count: 2               │                  │  │ │
│ │   doc_freq: 2                    │                  │  │ │
│ │ TpDictEntry[1]: "banana" ←───────┼──────────────────┼──┘ │
│ │   posting_offset: 0x0420 ────────┼──┐               │    │
│ │   posting_count: 1               │  │               │    │
│ │   doc_freq: 1                    │  │               │    │
│ │ TpDictEntry[2]: "cherry" ←───────┼──┼───────────────┼────┘
│ │   posting_offset: 0x0430 ────────┼──┼──┐            │
│ │   posting_count: 3               │  │  │            │
│ │   doc_freq: 3                    │  │  │            │
│ └──────────────────────────────────┼──┼──┼────────────┘
├───────────────────────────────────────┼──┼──┼──────────────┤
│ Offset 0x0400: Posting Lists          ↓  ↓  ↓              │
│ ┌──────────────────────────────────────────────────────┐   │
│ │ "apple" postings:  ←─────────────────┘  │  │          │   │
│ │   [(0,1), freq=2]                       │  │          │   │
│ │   [(0,3), freq=1]                       │  │          │   │
│ │ "banana" postings: ←────────────────────┘  │          │   │
│ │   [(0,2), freq=3]                          │          │   │
│ │ "cherry" postings: ←───────────────────────┘          │   │
│ │   [(0,1), freq=1]                                     │   │
│ │   [(0,2), freq=2]                                     │   │
│ │   [(0,4), freq=1]                                     │   │
│ └──────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│ Offset 0x0500: Document Lengths (sorted by CTID)           │
│ ┌──────────────────────────────────────────────────────┐   │
│ │ [(0,1), length=15]                                    │   │
│ │ [(0,2), length=22]                                    │   │
│ │ [(0,3), length=8]                                     │   │
│ │ [(0,4), length=31]                                    │   │
│ └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

1. **Dictionary** - Sorted array of string offsets for binary search
2. **String Pool** - Variable-length strings with embedded dict_entry offsets
3. **Dictionary Entries** - Fixed-size posting metadata (offset, count, doc_freq)
4. **Posting Lists** - Arrays of (ctid, frequency) pairs
5. **Document Lengths** - Array of (ctid, length) for BM25 normalization

### Term Lookup Flow

```
Query: Find postings for "banana"

Step 1: Binary Search Dictionary
┌─────────────────────────┐
│ string_offsets[]:       │
│ [0] = 0x0100 ("apple")  │
│ [1] = 0x0120 ("banana") │ ← Binary search finds index 1
│ [2] = 0x0140 ("cherry") │
└─────────────────────────┘

Step 2: Read String & Compare
┌──────────────────────────────────┐
│ Read at offset 0x0120:           │
│ • length = 6                     │
│ • text = "banana" ✓ Match!       │
│ • dict_entry_offset = 0x030C     │
└──────────────────────────────────┘
                    │
                    ↓
Step 3: Load Dictionary Entry
┌──────────────────────────────────┐
│ Read TpDictEntry at 0x030C:      │
│ • posting_offset = 0x0420        │
│ • posting_count = 1              │
│ • doc_freq = 1                   │
└──────────────────────────────────┘
                    │
                    ↓
Step 4: Read Posting List
┌──────────────────────────────────┐
│ Read 1 posting at 0x0420:        │
│ • ctid = (0,2)                   │
│ • frequency = 3                  │
└──────────────────────────────────┘
                    │
                    ↓
Step 5: BM25 Scoring (if needed)
┌──────────────────────────────────┐
│ Lookup doc length for (0,2):     │
│ • Binary search doc lengths      │
│ • Find length = 22               │
│ • Calculate BM25 score           │
└──────────────────────────────────┘
```

### Logical to Physical Mapping

```
Example: Reading bytes at logical offset 0x2480 (9344)

Given: 8KB pages (8192 bytes each)
Logical page = 0x2480 / 8192 = 1 (second page)
Page offset = 0x2480 % 8192 = 1152

Step 1: Look up physical block in page_map[]
┌─────────────────────────────┐
│ page_map[1] = Block 213     │
└─────────────────────────────┘
              │
              ↓
Step 2: Read from Block 213 at offset 1152
┌──────────────────────────────────────────────┐
│ Block 213 (Physical)                        │
│ ┌──────────────────────────────────────────┐│
│ │ [0-1151]: (other data)                   ││
│ │ [1152]: Start of requested data ←────────││
│ │ ...                                       ││
│ └──────────────────────────────────────────┘│
└──────────────────────────────────────────────┘
```

```c
/* Segment header with embedded page map */
typedef struct TpSegmentHeader {
    /* Metadata */
    uint32 magic;              /* TP_SEGMENT_MAGIC */
    uint32 version;            /* Format version */
    TimestampTz created_at;    /* Creation time */
    uint32 num_pages;          /* Total pages in segment */
    uint32 data_size;          /* Total data bytes */

    /* Segment management */
    uint32 level;              /* Storage level (0 for memtable flush) */

    /* Section offsets in logical file */
    uint32 dictionary_offset;  /* Offset to TpDictionary */
    uint32 strings_offset;      /* Offset to string pool */
    uint32 entries_offset;      /* Offset to TpDictEntry array */
    uint32 postings_offset;     /* Offset to posting lists */
    uint32 doc_lengths_offset;  /* Offset to document lengths */

    /* Corpus statistics */
    uint32 num_terms;          /* Total unique terms */
    uint32 num_docs;           /* Total documents */
    uint64 total_tokens;       /* Sum of all document lengths */

    /* Ordered list of pages making up the segment file, used to resolve
     * logical file offsets to physical Postgres (page, offset) values
     */
    BlockNumber page_index;  /* First page of the page index */
} TpSegmentHeader;

/* Page types for segment pages */
typedef enum TpPageType {
    TP_PAGE_SEGMENT_HEADER = 1,  /* Segment header page */
    TP_PAGE_FILE_INDEX,           /* File index (page map) pages */
    TP_PAGE_DATA                  /* Data pages containing actual content */
} TpPageType;

/* Special area for page index pages (goes in page special area) */
typedef struct TpPageIndexSpecial {
    BlockNumber next_page;    /* Next page in index chain, InvalidBlockNumber if last */
    uint16      page_type;    /* TpPageType enum value */
    uint16      num_entries;  /* Number of BlockNumber entries on this page */
    uint16      flags;        /* Reserved for future use */
} TpPageIndexSpecial;

/*
 * Page Index Design
 *
 * The page index stores BlockNumbers directly in the page, packed tightly
 * for maximum efficiency. We manage the count in the special area.
 *
 * Layout of a page index page:
 * +-------------------+
 * | PageHeaderData    | <- Standard PG page header
 * +-------------------+
 * | BlockNumber[0]    | <- BlockNumbers stored contiguously
 * | BlockNumber[1]    |
 * | ...               |
 * | BlockNumber[n-1]  |
 * +-------------------+
 * | (free space)      |
 * +-------------------+
 * | TpPageIndexSpecial| <- Special area with next pointer and count
 * +-------------------+
 *
 * The first page index page is referenced by TpSegmentHeader.page_index.
 * When reading, we follow the chain and load all BlockNumbers into a
 * contiguous array in memory for O(1) logical-to-physical mapping.
 */

```

### Zero-Copy Data Structures

All structures are designed for direct access from disk:

```c
/*
 * Dictionary structure for fast term lookup
 *
 * The dictionary is a sorted array of string offsets, enabling binary search.
 * Each string is stored as: [length:uint32][text:char*][dict_entry_offset:uint32]
 * This allows us to quickly find a term and jump to its TpDictEntry.
 */
typedef struct TpDictionary {
    uint32 num_terms;           /* Number of terms in dictionary */
    uint32 string_offsets[];    /* Sorted array of offsets to strings */
} TpDictionary;

/* String entry in string pool */
typedef struct TpStringEntry {
    uint32 length;              /* String length */
    char text[];                /* String text (variable length) */
    /* Immediately after text: uint32 dict_entry_offset */
} TpStringEntry;

/* Dictionary entry - 12 bytes, more compact without string info */
typedef struct TpDictEntry {
    uint32 posting_offset;     /* Logical offset to posting list */
    uint32 posting_count;      /* Number of postings */
    uint32 doc_freq;           /* Document frequency for IDF */
} __attribute__((aligned(4))) TpDictEntry;

/* Posting entry - 8 bytes, packed */
typedef struct TpSegmentPosting {
    ItemPointerData ctid;      /* 6 bytes - heap tuple ID */
    uint16 frequency;          /* 2 bytes - term frequency */
} __attribute__((packed)) TpSegmentPosting;

/* Document length - 12 bytes (padded to 16) */
typedef struct TpDocLength {
    ItemPointerData ctid;      /* 6 bytes */
    uint16 length;             /* 2 bytes - total terms in doc */
    uint32 reserved;           /* 4 bytes padding */
} TpDocLength;
```

## Key Design Decisions

### Page Allocation
- Allocate all pages upfront during flush
- Store mapping in header for O(1) access
- No page chain traversal needed

### Zero-Copy Access
- All structures properly aligned
- Can be used directly from disk
- Pre-computed hashes for fast comparison

### Error Handling
- Atomic segment creation
- Mark valid only after complete write
- Return pages to FSM on failure

### Performance
- **Write**: Fast flush with sorting as main bottleneck
- **Read**: Sub-millisecond term lookup via binary search
- **Open**: O(n) where n = number of page index pages (typically 1-3)
- **Memory**: Page map uses 4 bytes per data page in segment

## Capacity

With 8KB pages using direct BlockNumber storage:
- **Page Index Pages**: Each can store ~2040 BlockNumbers
  - Page header: ~24 bytes (PageHeaderData)
  - Special area: ~12 bytes (TpPageIndexSpecial)
  - Available for BlockNumbers: 8192 - 24 - 12 = 8156 bytes
  - Entries per page: 8156 / 4 = 2039 BlockNumbers
- **Maximum segment size**:
  - Single index page: ~16MB (2039 pages × 8KB)
  - Two index pages: ~32MB (4078 pages × 8KB)
  - Three index pages: ~48MB (6117 pages × 8KB)
  - Scales linearly with additional index pages
- **Header page overhead**: ~100 bytes for TpSegmentHeader

This design scales well beyond v0.0.3 requirements. The page index can grow
as needed by adding more index pages to the chain.

## Implementation Status (v0.0.3)

### Completed Features
- ✅ **Dynamic segment sizing** - Segments sized based on actual data via `tp_segment_estimate_size()`
- ✅ **Page index chain** - Multi-page index support for large segments
- ✅ **Zero-copy reader** - Direct page access with `tp_segment_get_direct()`
- ✅ **Binary search** - Efficient term lookup and document length search
- ✅ **Auto-spilling** - Automatic flush when memory limits exceeded
- ✅ **Segment chaining** - `next_segment` field ready (not yet used)
- ✅ **BM25 integration** - Full scoring support with corpus statistics

### Implementation Notes
1. **Structure Naming**: Implementation uses `TpSegmentPosting` (not `TpPostingEntry`)
2. **Document Lengths**: Sorted by CTID for binary search efficiency
3. **Page Allocation**: All pages allocated upfront, returned to FSM on failure
4. **Memory Budget**: Dynamic spilling prevents out-of-memory errors

### Performance Characteristics
- **Write Speed**: ~100ms for 74-document Cranfield dataset
- **Term Lookup**: Sub-millisecond via binary search
- **Memory Usage**: ~20% overhead for alignment and page headers
- **Page Efficiency**: ~2040 BlockNumbers per index page

## Future Optimizations

1. **Compression** (v0.0.4)
   - Delta encoding for posting lists
   - Prefix compression for terms

2. **Column-Oriented Storage** (v0.0.4)
   - Split structures into parallel arrays to eliminate padding
   - E.g., posting lists as separate ctid[] and frequency[] arrays
   - Would save ~25% space on TpDocLength, improve compression ratios
   - Trade-off: slightly more complex access patterns vs. better space efficiency

3. **Skip Lists** (v0.0.4)
   - For faster posting list intersection

4. **Compaction** (v0.0.5)
   - Background worker to merge segments

5. **Indirect Blocks** (v1.0)
   - If segments grow beyond 16MB
