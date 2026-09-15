# CI SKIP_LOCKED Compatibility Design

## Scope

Fix the two remaining direct CI failures without changing managed compaction
policy:

- initialize the pg_durable owner OID before conditional discovery so Clang
  can prove it is never read uninitialized;
- preserve PostgreSQL's nonblocking `VACUUM (FULL, SKIP_LOCKED)` behavior
  during pg_textsearch lifecycle target discovery.

## Considered approaches

1. Disable lifecycle tracking for every `VACUUM ... SKIP_LOCKED`. This avoids
   blocking but can leave workflows stale for relations that PostgreSQL does
   rewrite.
2. Acquire PostgreSQL's final `AccessExclusiveLock` before invoking core. This
   identifies rewritten relations but changes lock duration and multi-relation
   VACUUM behavior.
3. Mirror PostgreSQL's initial nonblocking relation discovery and track only
   relations that can be inspected without waiting. This preserves normal
   VACUUM behavior and lets PostgreSQL remain authoritative about the final
   rewrite.

Approach 3 is selected.

## Design

Parse the `skip_locked` VACUUM option alongside `full`. During candidate
collection, use PostgreSQL's conditional relation-lock APIs when the option is
enabled. A relation whose inspection lock is unavailable is omitted from the
captured lifecycle state, matching core's decision to skip a relation rather
than wait. Relations that are immediately inspectable retain the existing
generation-capture and post-rewrite reconciliation flow.

The ordinary blocking path remains unchanged. PostgreSQL still performs all
authorization, final lock acquisition, warnings, and the physical rewrite.
If a relation becomes unavailable after initial discovery, core skips it and
reconciliation observes no physical identity change.

The pg_durable owner variable is initialized to `InvalidOid`; discovery still
raises the existing required-extension error whenever lookup does not produce
a valid owner.

## Error handling

`SKIP_LOCKED` remains skip-shaped rather than becoming an error or statement
timeout. Invalid objects and permission failures continue through PostgreSQL's
existing command handling. No errors are swallowed on the ordinary VACUUM
path.

## Testing

- Run the existing locked-relation `VACUUM FULL SKIP_LOCKED` lifecycle test on
  PostgreSQL 17 and 18.
- Confirm an unlocked `VACUUM FULL SKIP_LOCKED` still changes the physical
  generation and reconciles its workflow.
- Build PostgreSQL 19 beta with CI-equivalent Clang warnings-as-errors.
- Run formatting and source guards before committing the implementation.
