# Critical Safety Issues Found in PR Review

## 1. Race Condition - Global Static Cache (CRITICAL)

**Location**: src/segment/segment.c:48
**Issue**: Static global `page_map_cache` accessed by multiple backends without locking
**Risk**: Use-after-free, memory corruption, crashes

## 2. Silent DSA Failures (CRITICAL)

**Locations**:
- src/segment/segment.c:657
- src/memtable/posting.c:301
- src/memtable/stringtable.c:138

**Issue**: dshash_attach() return value never checked for NULL
**Risk**: NULL pointer dereference, immediate crash

## 3. Incomplete Page Index (HIGH)

**Location**: src/segment/segment.c:213
**Issue**: Only logs WARNING on incomplete page map, continues execution
**Risk**: Array bounds violation, data corruption

## 4. No Overflow Checks (HIGH)

**Locations**: src/segment/segment.c:803, 815
**Issue**: Offset arithmetic can wrap/overflow silently
**Risk**: Buffer overruns, wrong memory access

## 5. Architectural Coupling (MEDIUM)

**Location**: src/operator.c:20
**Issue**: High-level operator imports low-level segment implementation
**Risk**: Tight coupling, hard to test/maintain

## Recommended Actions

1. **Remove global cache** or add proper LWLock protection
2. **Add NULL checks** after all dshash_attach calls
3. **Change WARNING to ERROR** for incomplete page index
4. **Add overflow validation** for all offset calculations
5. **Create abstraction layer** between operator and segment

These must be fixed before production use to prevent crashes and data corruption.
