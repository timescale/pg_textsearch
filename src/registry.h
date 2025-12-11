/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * registry.h - Global registry for shared index states
 */
#pragma once

#include <postgres.h>

#include <storage/lwlock.h>
#include <utils/dsa.h>

#include "index.h"

/* Maximum number of concurrent Tapir indexes */
#define TP_MAX_INDEXES 64

/*
 * Registry entry mapping an index OID to its shared state
 */
typedef struct TpRegistryEntry
{
	Oid					index_oid; /* Index OID (InvalidOid if not in use) */
	TpSharedIndexState *shared_state;	 /* Pointer to shared state in DSA */
	dsa_pointer			shared_state_dp; /* DSA pointer for recovery */
} TpRegistryEntry;

/*
 * Global registry stored in regular shared memory
 */
typedef struct TpGlobalRegistry
{
	LWLock			lock;					 /* Protects the registry */
	dsa_handle		dsa_handle;				 /* Handle for shared DSA area */
	TpRegistryEntry entries[TP_MAX_INDEXES]; /* Fixed-size array of entries */
	int				num_entries;			 /* Number of active entries */
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
