/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * registry.h - Global registry for shared index states
 *
 * Uses a dshash (dynamic shared hash table) to map index OIDs to their
 * shared state DSA pointers. This allows unlimited indexes (bounded only
 * by available memory) with O(1) lookup performance.
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

#include "state/state.h"

/*
 * LWLock tranche ID for the registry dshash.
 * Uses LWTRANCHE_FIRST_USER_DEFINED + 1 to avoid conflict with string table.
 */
#define TP_REGISTRY_HASH_TRANCHE_ID (LWTRANCHE_FIRST_USER_DEFINED + 1)

/*
 * Registry entry stored in dshash
 * The key is the first field (index_oid), value is shared_state_dp
 */
typedef struct TpRegistryEntry
{
	Oid			index_oid;		 /* Hash key - must be first */
	dsa_pointer shared_state_dp; /* DSA pointer to TpSharedIndexState */
} TpRegistryEntry;

/*
 * Global registry control structure stored in shared memory.
 * The actual entries are in a dshash stored in DSA.
 */
typedef struct TpGlobalRegistry
{
	LWLock				lock;			 /* Protects initialization */
	dsa_handle			dsa_handle;		 /* Handle for shared DSA area */
	dshash_table_handle registry_handle; /* Handle for the registry dshash */
} TpGlobalRegistry;

/* Registry management functions */
extern void tp_registry_init(void);
extern void tp_registry_shmem_startup(void);

/* DSA management */
extern dsa_area *tp_registry_get_dsa(void);
extern void		 tp_registry_detach_dsa(void);
extern void		 tp_registry_reset_dsa(void);

/* Registry operations */
extern bool tp_registry_register(
		Oid					index_oid,
		TpSharedIndexState *shared_state,
		dsa_pointer			shared_dp);
extern TpSharedIndexState *tp_registry_lookup(Oid index_oid);
extern dsa_pointer		   tp_registry_lookup_dsa(Oid index_oid);
extern dsa_pointer		   tp_registry_get_shared_dp(Oid index_oid);
extern bool				   tp_registry_is_registered(Oid index_oid);
extern void				   tp_registry_unregister(Oid index_oid);
