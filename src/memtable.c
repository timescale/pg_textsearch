/*-------------------------------------------------------------------------
 *
 * memtable.c
 *	  Tapir BM25 in-memory table implementation
 *
 * IDENTIFICATION
 *	  src/memtable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "constants.h"
#include "stringtable.h"
#include "index.h"
#include "memtable.h"
#include "posting.h"

/* Global variables */
TpSharedState *tp_shared_state = NULL;
HTAB	   *tp_query_limits_hash = NULL;

/* GUC variables */
int			tp_shared_memory_size = TP_DEFAULT_SHARED_MEMORY_SIZE;
int			tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

/* Transaction-level lock tracking (per-backend) */
bool tp_transaction_lock_held = false;

/* Previous hooks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Forward declarations - none needed */

/*
 * Calculate shared memory size needed for Tp memtable
 */
Size
tp_calculate_shared_memory_size(void)
{
	Size		string_hash_size;
	Size		shared_state_size;
	Size		string_pool_size;
	Size		total_size;
	uint32		initial_buckets;
	uint32		max_entries;

	/* Use same constants as actual allocation in tp_shmem_startup() */
	initial_buckets = TP_HASH_DEFAULT_BUCKETS;
	max_entries = (uint32)(initial_buckets * TP_HASH_MAX_LOAD_FACTOR);
	string_pool_size = TP_HASH_DEFAULT_STRING_POOL_SIZE;

	/* Calculate string hash table size using same logic as actual allocation */
	string_hash_size = tp_hash_table_shmem_size(max_entries, string_pool_size);

	/* Shared state structure */
	shared_state_size = sizeof(TpSharedState);

	total_size = add_size(string_hash_size, shared_state_size);

	elog(
		 DEBUG2,
		 "Tapir shared memory size: string_hash=%zu (buckets=%u, max_entries=%u, pool=%zu), shared_state=%zu, total=%zu",
		 string_hash_size,
		 initial_buckets,
		 max_entries,
		 string_pool_size,
		 shared_state_size,
		 total_size);

	return total_size;
}

/*
 * shmem_request hook: request shared memory space
 */
void
tp_shmem_request(void)
{
	Size		memtable_size;

	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	memtable_size = tp_calculate_shared_memory_size();
	RequestAddinShmemSpace(memtable_size);
	RequestNamedLWLockTranche("tp_memtable", 2);	/* 2 locks needed */

	elog(DEBUG1, "Tapir requested %zu bytes of shared memory", memtable_size);
}

/*
 * shmem_startup hook: allocate or attach to shared memory
 */
void
tp_shmem_startup(void)
{
	bool		found;
	LWLockPadded *tranche;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Reset global variables in case this is a restart */
	tp_shared_state = NULL;
	tp_query_limits_hash = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* Initialize shared state */
	tp_shared_state =
		ShmemInitStruct("tp_shared_state", sizeof(TpSharedState), &found);

	if (!found)
	{
		/* First time initialization */
		tranche = GetNamedLWLockTranche("tp_memtable");
		tp_shared_state->string_interning_lock = &tranche[0].lock;
		tp_shared_state->posting_lists_lock = &tranche[1].lock;
		tp_shared_state->string_hash_size = 0;
		tp_shared_state->total_terms = 0;

		/* Initialize statistics */
		memset(&tp_shared_state->stats, 0, sizeof(TpCorpusStatistics));
		tp_shared_state->stats.k1 = 1.2f;
		tp_shared_state->stats.b = 0.75f;
		tp_shared_state->stats.last_checkpoint = GetCurrentTimestamp();
		tp_shared_state->stats.segment_threshold = TP_DEFAULT_SEGMENT_THRESHOLD;

		elog(DEBUG1, "Tapir shared state initialized");
	}

	/* Initialize custom string interning hash table */
	{
		uint32		max_entries = tp_calculate_max_hash_entries();
		Size		string_pool_size = max_entries * (TP_AVG_TERM_LENGTH + 1);  /* Calculate string pool from max entries */
		uint32		initial_buckets = TP_HASH_DEFAULT_BUCKETS;  /* Use consistent bucket count */
		Size		hash_table_size = tp_hash_table_shmem_size(max_entries, string_pool_size);
		bool		hash_found = false;
		
		tp_string_hash_table = (TpStringHashTable *) ShmemInitStruct("tp_string_hash",
																	 hash_table_size,
																	 &hash_found);
		
		if (!tp_string_hash_table)
			elog(PANIC, "Failed to allocate shared memory for string hash table");
		
		if (!hash_found)
		{
			/* Zero out the allocated hash table memory for sanitizer safety */
			memset(tp_string_hash_table, 0, hash_table_size);
			
			/* Initialize new hash table */
			tp_hash_table_init(tp_string_hash_table, initial_buckets, string_pool_size, max_entries);
			elog(DEBUG1, "Initialized new string hash table: %u buckets, %zu string pool, %u max entries",
				 initial_buckets, string_pool_size, max_entries);
		}
		else
		{
			elog(DEBUG1, "Attached to existing string hash table");
		}
	}

	/* Query limits are now per-backend, not shared */
	tp_query_limits_hash = NULL;

	if (!found)
	{
		elog(DEBUG1, "Tapir string interning hash table created");
	}
	else
	{
		elog(DEBUG1, "Tapir attached to existing shared memory");
	}

	/* Initialize posting list shared state while we still hold the lock */
	tp_posting_init_shared_state();

	LWLockRelease(AddinShmemInitLock);
}


/* String interning functions removed - now handled per-index in posting.c */

/*
 * Transaction-level lock management
 * Acquire exclusive lock on all shared memory structures for the duration 
 * of the current transaction to eliminate per-operation locking overhead
 */
void
tp_transaction_lock_acquire(void)
{
	if (tp_transaction_lock_held)
	{
		elog(DEBUG2, "Tapir transaction lock already held by this backend");
		return;  /* Already holding the lock */
	}

	if (!tp_shared_state)
		elog(ERROR, "Tapir shared state not initialized");

	/* Acquire exclusive lock on all shared memory operations */
	LWLockAcquire(tp_shared_state->string_interning_lock, LW_EXCLUSIVE);
	
	tp_transaction_lock_held = true;
	
	elog(DEBUG1, "Tapir transaction-level lock acquired");
}

/*
 * Release transaction-level lock
 * Should be called at transaction end (commit or abort)
 */
void
tp_transaction_lock_release(void)
{
	if (!tp_transaction_lock_held)
	{
		elog(DEBUG2, "Tapir transaction lock not held by this backend");
		return;  /* Not holding the lock */
	}

	if (!tp_shared_state)
	{
		elog(WARNING, "Tapir shared state not available during lock release");
		tp_transaction_lock_held = false;
		return;
	}

	/* Release the exclusive lock */
	LWLockRelease(tp_shared_state->string_interning_lock);
	
	tp_transaction_lock_held = false;
	
	elog(DEBUG1, "Tapir transaction-level lock released");
}



/*
 * Initialize memtable hooks
 */
void
tp_init_memtable_hooks(void)
{
	/* Save previous hooks */
	prev_shmem_request_hook = shmem_request_hook;
	prev_shmem_startup_hook = shmem_startup_hook;

	/* Install our hooks */
	shmem_request_hook = tp_shmem_request;
	shmem_startup_hook = tp_shmem_startup;

	elog(DEBUG1, "Tapir memtable hooks installed");
}
