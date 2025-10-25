# Segments Design

## Overview

Segments are immutable, disk-based structures that store the inverted index when the memtable is flushed. This design uses a page mapping table to handle non-sequential page allocation in Postgres.

## Architecture

### Core Design: Page Mapping Table

Since Postgres doesn't guarantee sequential page allocation, we store a logical → physical page mapping directly in the segment header. This avoids page traversal on open and provides O(1) page access.

```c
/* Segment header with embedded page map */
typedef struct TpSegmentHeader {
    /* Metadata */
    uint32 magic;              /* TP_SEGMENT_MAGIC */
    uint32 version;            /* Format version */
    uint32 num_pages;          /* Total pages in segment */
    uint32 data_size;          /* Total data bytes */

    /* Statistics */
    uint32 num_terms;          /* Number of unique terms */
    uint32 num_docs;           /* Number of documents */

    /* Section offsets (logical byte offsets) */
    uint32 dict_offset;        /* Dictionary start */
    uint32 dict_size;          /* Dictionary size */
    uint32 postings_offset;    /* Posting lists start */
    uint32 postings_size;      /* Posting lists size */
    uint32 doclens_offset;     /* Document lengths start */
    uint32 doclens_size;       /* Document lengths size */
    uint32 strings_offset;     /* String pool start */
    uint32 strings_size;       /* String pool size */

    /* Segment management */
    BlockNumber next_segment;  /* Next segment in index */
    TimestampTz created_at;    /* Creation time */
    uint32 level;              /* LSM level (0 for memtable flush) */

    /* Page mapping table - maps logical page → physical BlockNumber */
    BlockNumber page_map[FLEXIBLE_ARRAY_MEMBER];
} TpSegmentHeader;
```

### Zero-Copy Data Structures

All structures are designed for direct access from disk:

```c
/* Dictionary entry - 24 bytes, aligned for direct access */
typedef struct TpDictEntry {
    uint32 term_hash;          /* Pre-computed hash for fast comparison */
    uint32 string_offset;      /* Logical offset in string pool */
    uint32 string_len;         /* Term length */
    uint32 posting_offset;     /* Logical offset to posting list */
    uint32 posting_count;      /* Number of postings */
    uint32 doc_freq;           /* Document frequency for IDF */
} __attribute__((aligned(8))) TpDictEntry;

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

## Implementation

### Segment Reader

```c
typedef struct TpSegmentReader {
    Relation index;
    BlockNumber root_block;

    /* Cached header with page map */
    TpSegmentHeader *header;
    Buffer header_buffer;

    /* Current data page */
    Buffer current_buffer;
    uint32 current_logical_page;
} TpSegmentReader;

/* Open segment - only reads header page */
TpSegmentReader* tp_segment_open(Relation index, BlockNumber root) {
    TpSegmentReader *reader = palloc0(sizeof(TpSegmentReader));
    reader->index = index;
    reader->root_block = root;

    /* Read header with page map */
    reader->header_buffer = ReadBuffer(index, root);
    Page page = BufferGetPage(reader->header_buffer);
    reader->header = (TpSegmentHeader *)PageGetContents(page);

    /* Validate */
    if (reader->header->magic != TP_SEGMENT_MAGIC) {
        elog(ERROR, "invalid segment magic");
    }

    return reader;
}

/* Get physical block for logical page - O(1) */
static inline BlockNumber
tp_segment_get_physical(TpSegmentReader *reader, uint32 logical_page) {
    Assert(logical_page < reader->header->num_pages);
    return reader->header->page_map[logical_page];
}

/* Read data at logical offset */
void tp_segment_read(TpSegmentReader *reader, uint32 logical_offset,
                    void *dest, uint32 len) {
    while (len > 0) {
        uint32 logical_page = logical_offset / BLCKSZ;
        uint32 page_offset = logical_offset % BLCKSZ;
        uint32 to_read = Min(len, BLCKSZ - page_offset);

        /* Get physical block from mapping */
        BlockNumber physical = tp_segment_get_physical(reader, logical_page);

        /* Pin page if needed */
        if (!BufferIsValid(reader->current_buffer) ||
            reader->current_logical_page != logical_page) {

            if (BufferIsValid(reader->current_buffer)) {
                ReleaseBuffer(reader->current_buffer);
            }

            reader->current_buffer = ReadBuffer(reader->index, physical);
            reader->current_logical_page = logical_page;
        }

        /* Copy data */
        Page page = BufferGetPage(reader->current_buffer);
        memcpy(dest, PageGetContents(page) + page_offset, to_read);

        /* Advance */
        dest = (char*)dest + to_read;
        logical_offset += to_read;
        len -= to_read;
    }
}
```

### Segment Writer

```c
/* Write memtable to segment */
BlockNumber tp_write_segment(TpLocalIndexState *state) {
    /* 1. Estimate size */
    uint32 data_size = estimate_segment_size(state);
    uint32 header_size = sizeof(TpSegmentHeader) +
                        (num_pages * sizeof(BlockNumber));
    uint32 total_size = header_size + data_size;
    uint32 num_pages = (total_size + BLCKSZ - 1) / BLCKSZ;

    /* 2. Allocate pages */
    BlockNumber *pages = palloc(sizeof(BlockNumber) * num_pages);
    for (int i = 0; i < num_pages; i++) {
        pages[i] = GetFreeIndexPage(state->index);
        if (!BlockNumberIsValid(pages[i])) {
            pages[i] = RelationGetNumberOfBlocks(state->index);
            /* Extend relation */
        }
    }

    /* 3. Build header with page map */
    TpSegmentHeader *header = palloc0(header_size);
    header->magic = TP_SEGMENT_MAGIC;
    header->version = 1;
    header->num_pages = num_pages;
    header->num_terms = count_terms(state);
    header->created_at = GetCurrentTimestamp();

    /* Copy page mapping */
    memcpy(header->page_map, pages, num_pages * sizeof(BlockNumber));

    /* 4. Create writer context */
    TpSegmentWriter writer = {
        .index = state->index,
        .pages = pages,
        .num_pages = num_pages,
        .current_offset = header_size,
        .header = header
    };

    /* 5. Write sections */
    header->dict_offset = writer.current_offset;
    write_sorted_dictionary(&writer, state);
    header->dict_size = writer.current_offset - header->dict_offset;

    header->postings_offset = writer.current_offset;
    write_posting_lists(&writer, state);
    header->postings_size = writer.current_offset - header->postings_offset;

    /* ... other sections ... */

    /* 6. Write header to first page */
    Buffer header_buf = ReadBuffer(state->index, pages[0]);
    Page header_page = BufferGetPage(header_buf);
    PageInit(header_page, BLCKSZ, 0);
    memcpy(PageGetContents(header_page), header, header_size);
    MarkBufferDirty(header_buf);
    UnlockReleaseBuffer(header_buf);

    pfree(pages);
    pfree(header);

    return pages[0];  /* Return root block */
}
```

### Query Processing

```c
/* Get posting list from segment */
TpPostingList* tp_segment_get_posting(BlockNumber root_block,
                                      const char *term) {
    /* 1. Open segment */
    TpSegmentReader *reader = tp_segment_open(index, root_block);

    /* 2. Binary search dictionary */
    uint32 dict_start = reader->header->dict_offset;
    uint32 dict_count = reader->header->dict_size / sizeof(TpDictEntry);
    uint32 term_hash = hash_any((unsigned char*)term, strlen(term));

    uint32 left = 0, right = dict_count - 1;
    while (left <= right) {
        uint32 mid = (left + right) / 2;

        /* Read dictionary entry */
        TpDictEntry entry;
        uint32 entry_offset = dict_start + mid * sizeof(TpDictEntry);
        tp_segment_read(reader, entry_offset, &entry, sizeof(entry));

        /* Quick hash check */
        if (entry.term_hash == term_hash) {
            /* Confirm with actual string */
            char *stored_term = palloc(entry.string_len + 1);
            tp_segment_read(reader,
                           reader->header->strings_offset + entry.string_offset,
                           stored_term, entry.string_len);
            stored_term[entry.string_len] = '\0';

            if (strcmp(term, stored_term) == 0) {
                /* Found - load posting list */
                TpPostingList *result = load_posting_list(reader, &entry);
                pfree(stored_term);
                tp_segment_close(reader);
                return result;
            }
            pfree(stored_term);
        }

        /* Binary search continue */
        if (entry.term_hash < term_hash) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    tp_segment_close(reader);
    return NULL;
}
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
- **Write**: ~100ms for typical flush (bottleneck: sorting)
- **Read**: < 1ms per term lookup
- **Open**: O(1) - only read header
- **Memory**: Page map uses 4 bytes per page

## Capacity

With 8KB pages:
- Header overhead: ~100 bytes
- Available for page map: ~8000 bytes
- Pages per header: ~2000
- Maximum segment size: ~16MB

This is sufficient for v0.0.3. Can add continuation pages later if needed.

## Future Optimizations

1. **Compression** (v0.0.4)
   - Delta encoding for posting lists
   - Prefix compression for terms

2. **Skip Lists** (v0.0.4)
   - For faster posting list intersection

3. **Compaction** (v0.0.5)
   - Background worker to merge segments

4. **Indirect Blocks** (v1.0)
   - If segments grow beyond 16MB
