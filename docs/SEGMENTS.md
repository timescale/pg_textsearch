# Segments Design

## Overview

Segments are immutable, disk-based structures that store the inverted index when the memtable is flushed. This design uses a page mapping table to handle non-sequential page allocation in Postgres.

## Architecture

### Core Design: Page Mapping Table

Since Postgres doesn't guarantee sequential page allocation, we store a logical → physical page mapping directly in the segment header. This avoids page traversal on open and provides O(1) page access.

### Segment File Layout

The logical segment file contains these sections in order:

1. **Dictionary** - Sorted array of string offsets for binary search
2. **String Pool** - Variable-length strings with embedded dict_entry offsets
3. **Dictionary Entries** - Fixed-size posting metadata (offset, count, doc_freq)
4. **Posting Lists** - Arrays of (ctid, frequency) pairs
5. **Document Lengths** - Array of (ctid, length) for BM25 normalization

Term lookup flow:
1. Binary search dictionary for string offset
2. Read string and compare
3. If match, read embedded dict_entry offset
4. Load TpDictEntry
5. Use posting_offset to read posting list

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
typedef struct TpPostingEntry {
    ItemPointerData ctid;      /* 6 bytes - heap tuple ID */
    uint16 frequency;          /* 2 bytes - term frequency */
} __attribute__((packed)) TpPostingEntry;

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
