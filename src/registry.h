/*-------------------------------------------------------------------------
 *
 * registry.h
 *	  Tapir global registry for shared index states
 *
 * The registry provides a mapping from index OIDs to TpSharedIndexState
 * structures stored in DSA. It uses regular PostgreSQL shared memory
 * to ensure all backends can access the registry.
 *
 * IDENTIFICATION
 *	  src/registry.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TAPIR_REGISTRY_H
#define TAPIR_REGISTRY_H

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
	TpSharedIndexState *shared_state; /* Pointer to shared state in DSA */
} TpRegistryEntry;

/*
 * Global registry stored in regular shared memory
 */
typedef struct TpGlobalRegistry
{
	LWLock			lock;					 /* Protects the registry */
	TpRegistryEntry entries[TP_MAX_INDEXES]; /* Fixed-size array of entries */
	int				num_entries;			 /* Number of active entries */
} TpGlobalRegistry;

/* Registry management functions */
extern void tp_registry_init(void);
extern void tp_registry_shmem_startup(void);

/* Registry operations */
extern bool
tp_registry_register(Oid index_oid, TpSharedIndexState *shared_state);
extern TpSharedIndexState *tp_registry_lookup(Oid index_oid);
extern void				   tp_registry_unregister(Oid index_oid);

#endif /* TAPIR_REGISTRY_H */
