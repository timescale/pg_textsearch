# pg_textsearch Source Reorganization

## Goal

Restructure the `src/` directory so that a newcomer can understand the
system architecture by reading the directory tree. The current layout
has accumulated layering violations, overstuffed files, inconsistent
include paths, and directory names that don't clearly communicate what
the code does. After this work, the directory structure should reflect
a layered architecture with clean dependency flow.

Target audiences: Postgres extension developers *and* experienced
C/systems developers new to pg_textsearch. Postgres naming conventions
take precedence where applicable.

## Current State (problems)

1. **Layering violations in `types/`** -- `query.c` (1227 lines, 40
   includes from 7 directories) contains IDF caching, index
   resolution, and segment traversal. `vector.c` reaches into
   memtable/state. These should be thin serialization layers.

2. **`am/scan.c` mixes concerns** -- index name resolution, query
   validation, score caching (global static), and the Postgres scan
   interface in one file.

3. **`segment/segment.h` kitchen sink** (329 lines) -- on-disk format
   definitions, in-memory iteration structs, legacy V3/V4/V5 compat,
   and exported functions.

4. **`fieldnorm.h`** is 348 lines, 90% static decode table.

5. **Code duplication** -- `read_term_at_index()` duplicated between
   `segment.c` and `merge.c`.

6. **Include path inconsistency** -- mix of short-form (`"segment.h"`),
   relative (`"../source.h"`), and full (`"segment/compression.h"`)
   styles.

7. **Directory names don't communicate architecture** -- `query/` is
   really scoring, `state/` is really index coordination, `am/` could
   be the Postgres-standard `access/`.

8. **`source.c/h` orphaned in root** -- the posting source abstraction
   sits alone, unconnected to either storage subsystem it abstracts.

## Design

### Phase 1: Preliminary PRs (behavioral changes)

These PRs change which file owns which logic. Each gets its own review
because they are not purely mechanical.

#### PR 1: Extract business logic from `types/query.c`

Move IDF caching, index resolution, and segment traversal for doc
frequency out of the type implementation into `index/` (currently
`state/`). After this, `query.c` is a thin serialization/operator
layer.

#### PR 2: Extract business logic from `types/vector.c`

Move memtable/state coupling out. After this, `vector.c` is pure type
I/O and operators.

#### PR 3: Split concerns in `am/scan.c`

Extract index name resolution and query validation into separate
functions or a separate file. The scan file should only contain the
Postgres scan interface (`beginscan`, `rescan`, `gettuple`,
`endscan`) and the orchestration that ties them together.

#### PR 4: Deduplicate `read_term_at_index()`

Extract the shared term-reading logic duplicated in `segment.c` and
`merge.c` into `dictionary.c`.

### Phase 2: Reorganization PR (one atomic PR, purely structural)

No behavioral changes. File moves, splits, renames, and include path
updates only.

#### Target directory structure

```
src/
├── mod.c                          # Extension init, GUC registration
├── constants.h                    # Shared constants
│
├── access/                        # Layer 1: Postgres AM interface
│   ├── handler.c                  #   AM handler registration
│   ├── am.h                       #   Public API header
│   ├── build.c                    #   Index build (sequential)
│   ├── build_context.c/h          #   Arena-based build context
│   ├── build_parallel.c/h         #   Parallel build workers
│   ├── scan.c                     #   Index scan (trimmed by PR 3)
│   └── vacuum.c                   #   VACUUM and ANALYZE
│
├── types/                         # Layer 1: SQL types & operators
│   ├── query.c/h                  #   bm25query (thinned by PR 1)
│   ├── vector.c/h                 #   bm25vector (thinned by PR 2)
│   └── array.c/h
│
├── planner/                       # Layer 1: Query optimizer hooks
│   ├── hooks.c/h
│   └── cost.c/h
│
├── scoring/                       # Layer 2: Ranking engine
│   ├── bm25.c/h                   #   BM25 score computation
│   └── bmw.c/h                    #   Block-Max WAND optimization
│
├── index/                         # Layer 2: Index state & coordination
│   ├── state.c/h                  #   Index lifecycle (local + shared)
│   ├── registry.c/h               #   Global index registry
│   ├── metapage.c/h               #   Persistent index metadata
│   ├── memory.c/h                 #   DSA memory management
│   ├── limit.c/h                  #   Query LIMIT tracking
│   └── source.c/h                 #   Abstract posting source interface
│
├── memtable/                      # Layer 3: In-memory storage
│   ├── memtable.c/h
│   ├── posting.c/h
│   ├── posting_entry.h
│   ├── stringtable.c/h
│   ├── arena.c/h
│   ├── expull.c/h
│   ├── scan.c/h
│   └── source.c/h                #   TpDataSource impl for memtable
│
├── segment/                       # Layer 3: On-disk storage
│   ├── format.h                   #   On-disk format definitions
│   ├── segment.c/h                #   Segment operations
│   ├── io.h                       #   Reader/writer/iterator
│   ├── scan.c                     #   Segment posting iteration
│   ├── compression.c/h
│   ├── dictionary.c/h
│   ├── docmap.c/h
│   ├── fieldnorm.c/h              #   Decode table moved to .c
│   ├── alive_bitset.c/h
│   ├── pagemapper.h
│   ├── merge.c/h
│   └── merge_internal.h
│
└── debug/                         # Utilities
    └── dump.c/h
```

#### Layering rules

The dependency flow is top-down:

- **Layer 1** (access/, types/, planner/) may depend on Layer 2 and
  Layer 3
- **Layer 2** (scoring/, index/) may depend on Layer 3
- **Layer 3** (memtable/, segment/) should not depend on Layer 1 or
  Layer 2
- `debug/` is exempt (it inspects everything)
- `mod.c` is exempt (it initializes everything)

These are not enforced mechanically but should be maintained by
convention and code review.

#### File moves and renames

**Directory renames:**

| Current | New | Rationale |
|---------|-----|-----------|
| `src/am/` | `src/access/` | Matches Postgres core `src/backend/access/` |
| `src/query/` | `src/scoring/` | Contents are BM25 scoring and BMW, not query parsing |
| `src/state/` | `src/index/` | Manages the index itself, not generic "state" |

**File renames:**

| Current | New | Rationale |
|---------|-----|-----------|
| `query/score.c/h` | `scoring/bm25.c/h` | Names what it computes |
| `segment/segment_io.h` | `segment/io.h` | Directory prefix provides context |

**File moves:**

| Current | New | Rationale |
|---------|-----|-----------|
| `src/source.c/h` | `src/index/source.c/h` | Posting source abstraction is index-level coordination |

**File splits:**

| Current | New files | What moves |
|---------|-----------|------------|
| `segment/segment.h` | `segment/format.h` + `segment/segment.h` | On-disk structs, version constants, legacy compat helpers go to `format.h`. Operation API stays in `segment.h`. `segment.h` includes `format.h` so callers that need both the API and format types only need one include. |
| `segment/fieldnorm.h` | `segment/fieldnorm.c` + `segment/fieldnorm.h` | 256-entry decode table moves to `.c`. Header keeps function signatures and `FIELDNORM_QUANTIZE_BITS` constant. Shrinks header from 348 to ~20 lines. |

#### `segment/segment.h` split detail

**`segment/format.h`** receives:
- `TpSegmentHeader`, `TpSegmentHeaderV3`
- `TpDictEntry`, `TpDictEntryV3`
- `TpSkipEntry`, `TpSkipEntryV3`
- `TpBlockPosting`, `TpCtidMapEntry`
- Format version constants (`TP_SEGMENT_VERSION_*`)
- Legacy version-aware inline helpers

**`segment/segment.h`** keeps:
- `#include "segment/format.h"` (so existing consumers keep working
  during the transition, and because the API needs the format types)
- Function declarations for segment operations
- Any remaining types tied to the API rather than the format

#### Include path standardization

All project-local includes use full `src/`-relative paths:

```c
/* Before (mixed styles) */
#include "segment.h"
#include "../source.h"
#include "segment/compression.h"

/* After (consistent) */
#include "segment/segment.h"
#include "index/source.h"
#include "segment/compression.h"
```

Rules:
- Every project `#include` uses its directory-qualified path from
  `src/`
- No relative `../` paths
- No short-form same-directory includes
- `-I` flag in Makefile points to `src/` (already the case via
  `PG_CPPFLAGS`)
- Postgres system includes are unaffected

#### Makefile updates

The `Makefile` lists source files in `SRCS` (or `OBJS`). This must be
updated to reflect new paths. The `-I$(srcdir)/src` flag should
already be present; verify and add if missing.

## Verification

For the reorg PR:

1. `make clean && make` -- builds successfully
2. `make installcheck` -- all tests pass
3. `make format-check` -- formatting clean
4. `git diff --stat` shows only renames/moves/include-path changes
   (no logic changes)
5. Spot-check: grep for any remaining `../` includes or short-form
   includes of project headers

## Out of scope

- Completing the `TpDataSource` abstraction for segments (separate
  future work)
- Splitting `planner/hooks.c` (2034 lines, but single-concern)
- Splitting `segment/merge.c` (1902 lines, but single-concern)
- Splitting `debug/dump.c` (1598 lines; could benefit from per-module
  dump helpers, but not required for the reorg)
- Renaming internal types or functions (this is a file-level reorg)
