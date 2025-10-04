# DSA Cleanup Design for DROP INDEX

## Problem Statement

When an index is dropped, we need to clean up:
1. **Global state**: Registry entry, DSA contents (dshash tables), and unpin DSA
2. **Local state**: Backend-specific DSA attachments and cached local_state structures

The challenge: Only the dropping backend gets the object_access_hook, but all backends that have attached to the DSA need to detach to free memory.

## Solution: Two-Callback Design

### Object Access Hook (Dropping Backend Only)
**Purpose**: Clean up global/shared resources
**Runs in**: Backend executing DROP INDEX
**Timing**: During DROP INDEX execution, before drop commits

### Relcache Invalidation Callback (All Backends)
**Purpose**: Clean up local/backend-specific resources
**Runs in**: All backends that have the relation cached
**Timing**: After DROP INDEX commits and cache is invalidated

## Information Available in Each Callback

### Object Access Hook (`tp_object_access`)
- `objectId` (the index OID being dropped)
- Runs BEFORE the actual drop completes
- Can access registry (still has entry)
- Can access local state cache (if this backend has it)
- Only fires once per DROP INDEX, in one backend

**What we clean up**:
- Registry entry (shared state pointer + DSA handle)
- DSA contents (dshash tables, etc.)
- Call `dsa_unpin()` to allow DSA to be freed
- Our own local state (if we have it cached)

### Relcache Invalidation Callback (`tp_relcache_callback`)
- `relid` (the relation OID)
- Runs AFTER the drop completes (relation is gone)
- Registry entry is already gone (cleaned up by object hook)
- Local state cache may still have stale entry
- Fires in every backend that has touched this index

**What we clean up**:
- Local state (detach from DSA, free local_state struct)
- Remove from local cache

## Implementation Design

### Object Hook Cleanup (Dropping Backend Only)
```c
tp_object_access() {
    // 1. Get DSA info from registry BEFORE unregistering
    dsm_handle handle = tp_registry_get_dsm_handle(index_oid);
    dsa_pointer shared_dp = tp_registry_get_shared_dp(index_oid);

    // 2. Unregister from global registry
    tp_registry_unregister(index_oid);

    // 3. Attach to DSA (or use existing attachment)
    // 4. Destroy dshash tables
    // 5. dsa_unpin() - allows DSA to be freed when last backend detaches
    // 6. Detach (if we attached just for cleanup)

    // 7. Clean up our own local state if we have it
    if (local_state_cache has entry for index_oid) {
        tp_release_local_index_state(local_state);
    }
}
```

### Relcache Callback Cleanup (All Backends)
```c
tp_relcache_callback(Datum arg, Oid relid) {
    // 1. Check if we have local state cached
    local_state = tp_get_local_index_state_if_cached(relid);
    if (local_state == NULL)
        return;  // Nothing to clean up

    // 2. Try to open relation to distinguish drop from invalidation
    rel = try_relation_open(relid, NoLock);
    if (rel != NULL) {
        relation_close(rel, NoLock);
        return;  // Just a cache invalidation, not a drop
    }

    // 3. Relation is gone - clean up our local state
    // This calls dsa_detach() which decrements refcount
    tp_release_local_index_state(local_state);
}
```

## Correctness Argument

### Case 1: Dropping Backend
**Sequence**:
1. Object hook fires → cleans up registry + DSA contents + calls `dsa_unpin()` + cleans up local state
2. Relcache callback fires → finds no local state (already cleaned up) → returns early

**Why this works**: Relcache callback is idempotent - it's safe to clean up local state twice (second time finds nothing).

### Case 2: Other Backend (Has Local State)
**Sequence**:
1. Object hook fires in dropping backend → cleans up registry + DSA contents + `dsa_unpin()`
2. Eventually, relcache callback fires in this backend → cleans up local state → calls `dsa_detach()`

**Why this works**:
- Registry is already gone, but we don't need it
- DSA is unpinned but still valid (refcount > 0)
- We successfully detach, decrementing DSA refcount
- When last backend detaches, DSA is freed (because it was unpinned)

### Case 3: Other Backend (No Local State)
**Sequence**:
1. Object hook fires in dropping backend → cleans up everything
2. Relcache callback fires in this backend → finds no local state → returns early

**Why this works**: Nothing to do, callback is no-op.

### Case 4: Backend Never Gets Relcache Callback
**Scenario**: Backend terminates or never accesses the relation again

**Why this is okay**:
- At backend exit, PostgreSQL automatically detaches from all DSA areas
- Since we called `dsa_unpin()`, the DSA will be freed when last backend exits
- Some pinned mappings may linger until backend exit, but that's acceptable

## Potential Race Conditions (All Safe)

### Race 1: Relcache fires before object hook completes
**Can this happen?** No - object hook fires during DROP INDEX execution, before the drop commits. Relcache invalidation happens after the drop commits.

### Race 2: New backend attaches between unpin and last detach
**Can this happen?** Yes, but it's okay:
- Object hook: unregisters from registry
- New backend: tries tp_get_local_index_state() → no registry entry → rebuilds from disk or fails
- No crash, just a query failure (index doesn't exist anymore)

### Race 3: try_relation_open succeeds but relation is being dropped
**Can this happen?** No - if try_relation_open succeeds, the relation exists and has a lock. DROP INDEX takes exclusive lock.

## Implementation Safety Checks

1. **Relcache callback must check for cached state first** - Avoids dereferencing invalid pointers
2. **Relcache callback must distinguish drop from invalidation** - Use `try_relation_open()`
3. **Object hook must get DSA info before unregistering** - Registry entry is deleted
4. **Both must be idempotent** - Can be called multiple times safely

## Expected Outcome

After implementing both callbacks with this design:
- DSA segments will be properly freed when all backends detach
- No accumulation of DSA segments across repeated CREATE/DROP cycles
- Tests should pass >50 iterations without "too many dynamic shared memory segments" error
