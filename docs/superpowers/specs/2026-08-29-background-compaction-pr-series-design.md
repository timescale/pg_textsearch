# Background Compaction Merge Series Design

## Status

Approved design for extracting the background-compaction proof of concept in
PR #471 into a sequence of independently reviewable, mergeable pull requests.

PR #471 remains the historical POC and umbrella. The mergeable implementation
will be reconstructed on clean branches from `main`; it will not preserve the
POC's 55-commit fixup history.

## Context

PR #471 proves that pg_textsearch can defer BM25 segment compaction to
pg_durable. It also contains storage correctness fixes, new SQL APIs,
transaction callback infrastructure, security-sensitive operator scripts,
packaging changes, documentation, a benchmark, and a demo. At more than
11,000 added lines across 42 files, it is too broad to review or merge as one
change.

The POC remains useful as:

- the final-state implementation from which focused changes can be extracted;
- a record of design discussion and measured behavior;
- an integration environment while the mergeable series is built; and
- an umbrella linking the resulting pull requests and upstream dependency.

## Goals

- Make each pull request independently useful, testable, and understandable.
- Merge extension-native foundations before pg_durable integration is ready.
- Keep storage correctness separate from scheduling and operator concerns.
- Consume the audited pg_durable node-failure policy required by the
  production backstop without changing `df.loop`.
- Preserve the final hardened behavior without replaying superseded POC
  designs through a long cherry-pick chain.

## Non-goals

This series does not include:

- out-of-band merge construction or lock-free publication;
- per-index schedule identifiers stored in the metapage;
- keyed schedule lifecycle support in pg_durable;
- automatic cluster role, authentication, or `pg_hba.conf` provisioning;
- segment splitting or a wider segment format beyond arithmetic and capacity
  checks needed for compaction correctness;
- productionizing the POC demo or benchmark; or
- changes to the existing two-argument `df.loop` contract; and
- any pg_durable API design beyond the seven-argument `df.start` node-failure
  policy in microsoft/pg_durable#354.

The broader segment format work remains tracked by issues #456 and #473.

## Considered Approaches

### Five-PR hybrid chain

Separate the work by independently valuable behavior while preserving a
linear dependency chain. Storage correctness lands first, followed by native
APIs, a scheduler-neutral dispatch seam, the pg_durable adapter, and finally
the recurring recovery backstop.

This is the selected approach. It keeps intermediate repository states
complete and testable without creating multi-thousand-line reviews that mix
unrelated trust boundaries.

### Three vertical slices

Combine native foundations, dispatch and integration, and operations into
three larger PRs. This reduces branch management but leaves storage, SQL,
transaction, and security concerns mixed in reviews that are still too large.

### Strict layer split

Create separate PRs for storage, C APIs, SQL, callbacks, scripts, tests,
packaging, and documentation. Individual diffs are smaller, but several
intermediate branches expose incomplete behavior and cannot be validated
meaningfully on their own.

## Pull Request Series

The dependency chain is:

```text
PR 1: engine correctness
  -> PR 2: native per-index APIs
    -> PR 3: scheduler-neutral dispatch
      -> PR 4: pg_durable per-index adapter
        -> PR 5: recovery backstop and operations
```

The required pg_durable change is microsoft/pg_durable#354 and proceeds in
parallel with PRs 1-3. Released v0.2.6 and current `main` do not contain the
capability; the audited #354 head does. PRs 4 and 5 do not merge until a
released pg_durable version contains that API, the setup scripts can preflight
it, and the live integration tests pass against the release.

### PR 1: Compaction Engine Correctness

#### Purpose

Make normal, stepped, and forced compaction obey segment-capacity and
single-result invariants without introducing background scheduling or new SQL
APIs. These are scheduler-independent correctness fixes and form a stable
base for all later work.

#### Scope

- Select capacity-aware merge candidates.
- Drain a blocking intermediate destination before retrying a lower level.
- Reject impossible terminal-level layouts before physical mutation.
- Use checked segment-count transitions during planning and execution.
- Separate spill preparation, finalization, and post-spill policy so force
  merge can spill without accidentally invoking normal compaction policy.
- Ensure successful force merge leaves zero segments for an empty or
  all-dead index and exactly one segment otherwise.
- Raise a production error, rather than relying only on assertions, if the
  force-merge planner and executor diverge or execution leaves multiple
  segments.

#### Primary files

- `src/access/build.c`
- `src/segment/merge.c`
- `src/segment/merge.h`
- focused additions to existing compaction, force-merge, and vacuum tests

The final POC commits around stranded levels, segment-count overflow, spill
capacity, full intermediate levels, and force-merge invariants are source
landmarks. They are not a cherry-pick list because several depend on later
fixups.

#### Acceptance gate

- Build and format checks pass.
- SQL regression tests cover full intermediate levels, terminal L7
  rejection, nonempty memtable preflight, zero/all-dead outcomes, and the
  single-segment postcondition.
- Rejected layouts are shown not to mutate the physical index.
- Segment, recovery, concurrent VACUUM/merge, and force-merge invariant tests
  pass.

### PR 2: Native Per-Index Compaction API

#### Purpose

Expose enough introspection and execution control to compact one index
manually or one merge batch at a time. This is useful without any external
scheduler and defines the stable contract consumed by later adapters.

#### Scope

- Add `bm25_level_counts(regclass)`.
- Add `bm25_needs_compaction(regclass)`.
- Add `bm25_compact_step(regclass)`, which performs exactly one merge batch
  and reports whether more work remains.
- Add `bm25_compact(regclass)`, which runs a complete cascade in the caller's
  transaction.
- Add fresh-install and upgrade SQL with matching definitions.
- Apply least-privilege ACLs and preserve supported local temporary-index
  behavior on public APIs.

Cross-index discovery and `bm25_compact_pending()` remain out of this PR
because their purpose is recovery backstop operation.

#### Primary files

- `src/access/compaction.c`
- SQL install and upgrade scripts
- per-index API, upgrade, ACL, temporary-index, and L7 regression tests
- the native API portion of `docs/background_compaction.md`

#### Acceptance gate

- One call to `bm25_compact_step()` performs exactly one merge batch.
- Repeated steps converge to the same valid layout as whole-cascade
  compaction.
- Terminal L7 capacity fails closed.
- Fresh-install and upgrade definitions are behaviorally identical.
- Non-owners cannot compact an index through the public APIs.

### PR 3: Scheduler-Neutral Transaction Dispatch

#### Purpose

Allow spill-time compaction debt to be handed to an external callback without
depending on pg_durable. The extension continues to treat physical index
state as the source of truth; requests are best-effort acceleration.

#### Scope

- Add `pg_textsearch.compaction_mode` with unchanged `inline` default and
  explicit `background` and `off` modes.
- Add syntax-only validation for
  `pg_textsearch.compaction_request_function`.
- Record and deduplicate per-index requests in backend-local transaction
  state only when physical debt requires work.
- Dispatch requests at PRE_COMMIT through a callback taking one `regclass`.
- Resolve and invoke callbacks inside protected internal subtransactions.
- Clear locks, counters, and requests on abort and PREPARE.
- Preserve interrupt semantics: ordinary callback errors warn and allow the
  user commit, while query cancellation and shutdown interrupts rethrow.

No pg_durable SQL, graph format, role, schedule, or package asset belongs in
this PR.

#### Primary files

- `src/index/compaction_request.c`
- `src/index/compaction_request.h`
- `src/index/state.c`
- `src/memtable/log.c`
- `src/mod.c`
- focused spill integration in `src/access/build.c`
- transaction lifecycle and callback regression tests
- scheduler-neutral design documentation

#### Acceptance gate

- Default inline behavior is unchanged.
- Commit dispatches each required index once; abort dispatches nothing.
- Savepoint, dropped-index, callback disappearance, and callback failure
  behavior is explicit and tested.
- PREPARE leaves the backend reusable without stale locks, counters, or
  requests.
- Callback lookup and invocation failures cannot corrupt PRE_COMMIT
  transaction state.

### PR 4: pg_durable Per-Index Adapter

#### Purpose

Connect the scheduler-neutral callback to pg_durable for immediate per-index
compaction tasks while enforcing a narrow, auditable privilege model.

#### Scope

- Preflight the supported pg_durable release, the seven-argument `df.start`
  signature, and the unchanged two-argument `df.loop` signature.
- Validate or create the dedicated passwordless `textsearch_compactor` login
  role without silently repairing hostile existing state.
- Validate inherited index-owner memberships and reject unsafe role
  attributes, settings, ownership, or grant delegation.
- Install and authenticate the exact managed SECURITY DEFINER wrapper.
- Submit one physical-target task using `transaction_mode => 'new'`,
  `max_attempts => 5`, `max_backoff => '16 seconds'`, and
  `on_failure => 'continue'`.
- Run a committed `SELECT 1` execution canary as the compactor role before
  enabling integration.
- Test hostile search paths, wrapper replacement, direct invocation ACLs,
  role reuse, and exact task completion.

Cluster authentication and socket configuration remain external
provisioning responsibilities. Extension installation must not create the
cluster-wide compactor role.

#### Primary files

- `scripts/durable_compaction/01_setup_role.sql`
- `scripts/durable_compaction/02_wrapper.sql`
- `scripts/durable_compaction/README.md`
- the per-index subset of `test/scripts/durable_compaction.sh`
- pg_durable integration sections of the canonical design document

#### Acceptance gate

- Setup is idempotent only for the exact managed role and wrapper state.
- Ambiguous or hostile existing state fails before grants or task
  registration.
- A committed canary and a real compaction task execute as the expected role.
- The wrapper targets the intended physical index identity and rejects
  temporary or invalid durable targets.
- A transient activity failure is retried under the exact adapter policy, and
  exhausted attempts do not terminate the enclosing stepped loop.
- Tests run against the released pg_durable capability rather than private
  catalog assumptions.

### PR 5: Recovery Backstop and Operations

#### Purpose

Guarantee eventual rediscovery of physical compaction debt when a best-effort
spill request is lost or fails, and provide the operational lifecycle needed
to run the integration.

#### Scope

- Add `bm25_indexes_needing_compaction()`.
- Add `bm25_compact_pending()` with failure isolation between indexes.
- Register one versioned canonical database-wide recurring backstop so its
  #354 failure policy is not confused with a legacy fail-stop instance.
- Serialize registration, reject duplicate canonical schedules, and verify
  the exact live schedule after commit.
- Keep `df.loop(body, condition)` unchanged and set recurrence resilience on
  the enclosing `df.start` with `max_attempts => 1` and
  `on_failure => 'continue'`, so one failed tick is recorded without
  terminating later ticks.
- Add monitoring queries, package contents, release assertions, and final
  operator documentation.

Per-index schedule identity in the metapage is a possible later design, not a
requirement for this global rescue backstop.

#### Primary files

- cross-index functions in `src/access/compaction.c`
- matching fresh-install and upgrade SQL
- `scripts/durable_compaction/03_backstop.sql`
- backstop and recurrence scenarios in `test/scripts/durable_compaction.sh`
- package and release workflow assets
- final `docs/background_compaction.md` and operator README updates

#### Acceptance gate

- A failed recurring tick has an observable failed node execution and a later
  tick still executes successfully in the live recurring instance.
- One bad index does not prevent other eligible indexes from compacting.
- Concurrent setup cannot create multiple canonical schedules.
- Setup fails with actionable schedule IDs if preexisting duplicates or a
  live legacy fail-stop backstop exist.
- Installed source, binary, and Debian packages contain valid documentation
  and operator-script paths.

## pg_durable Upstream Contract

pg_textsearch consumes the public node-failure policy implemented at the
audited head of microsoft/pg_durable#354. The graph builder remains:

```sql
df.loop(body text, condition text DEFAULT NULL)
```

The policy is configured on the seven-argument start function:

```sql
df.start(
    fut text,
    label text DEFAULT NULL,
    database text DEFAULT NULL,
    transaction_mode text DEFAULT 'caller',
    max_attempts integer DEFAULT 1,
    max_backoff interval DEFAULT '16 seconds',
    on_failure text DEFAULT 'fail')
```

`max_attempts` includes the first attempt. Eligible failed activity nodes are
retried with exponential backoff capped by `max_backoff`. Once attempts are
spent, `on_failure => 'continue'` abandons the remainder of the current
enclosing loop iteration and permits the next iteration to begin. Without an
enclosing loop, the instance still fails.

The immediate per-index adapter opts into bounded retries:

```sql
df.start(
    df.loop(body, condition),
    label => ...,
    transaction_mode => 'new',
    max_attempts => 5,
    max_backoff => '16 seconds',
    on_failure => 'continue')
```

The periodic backstop deliberately does not retry within a tick:

```sql
df.start(
    df.loop(scheduled_body, condition => NULL),
    label => ...,
    max_attempts => 1,
    on_failure => 'continue')
```

A scheduled tick begins a loop iteration in the live durable instance. If its
activity fails, the failed activity node and its error remain observable while
the instance stays live and eligible for the next tick. A later successful
iteration executes independently. Monitoring must therefore inspect failed
nodes under running instances rather than treating a running instance as
proof that every tick succeeded.

The compatibility defaults remain one attempt followed by fail-stop behavior.
Released v0.2.6 and current pg_durable `main` lack the seven-argument API; the
audited #354 head provides it. The pg_textsearch merge remains blocked until
the capability ships in a release and passes live adapter and recurrence
tests.

#354 stores the policy in orchestration input/history, not in the LOOP graph
or `df.instances`. The backstop therefore uses a new versioned canonical label
and refuses to overlap a live legacy-labeled instance rather than pretending
to authenticate the policy from catalog state.

pg_durable does not need to understand indexes, deduplicate pg_textsearch
work, manage per-index keys, or provision roles.

## Error and Recovery Semantics

- Storage capacity and invariant violations are hard errors.
- The extension never treats an enqueued task as proof that compaction
  completed; physical level state is authoritative.
- Ordinary PRE_COMMIT adapter failures emit a warning and do not abort the
  triggering user transaction.
- Query cancellation, administrative shutdown, and crash shutdown propagate.
- Operator setup fails closed on version, identity, ownership, ACL, or
  schedule ambiguity.
- The backstop isolates failures between indexes and uses
  `df.start(max_attempts => 1, on_failure => 'continue')` to preserve
  recurrence between ticks.
- A request discarded by savepoint or object lifecycle behavior is repaired
  by later index activity or the global backstop.

## Extraction Mechanics

1. Keep PR #471 and branch `task2-bm25-compact` intact as the POC reference.
2. Correct its description to use `transaction_mode => 'new'`, remove the
   missing report link, label it as an umbrella, and add links as extracted
   PRs open.
3. Build PR 1 from a clean branch off current `main`.
4. Build each subsequent PR on the preceding branch while it is under
   review, then rebase it onto updated `main` after its dependency merges.
5. Transplant the final POC implementation by concern. Do not replay the
   original commits wholesale: early commits contain behavior superseded by
   later transaction, capacity, security, and operations fixes.
6. Keep implementation, tests, SQL definitions, and directly relevant
   documentation together in the PR that introduces each contract.
7. Run each PR's focused gate plus the repository's required build,
   formatting, and regression checks before requesting review.

The original commit IDs are useful only as navigation landmarks. The final
POC tree is the behavioral source of truth.

## Completion

The effort is complete when:

- PRs 1-3 have merged independently;
- the required pg_durable behavior has shipped in a released version;
- PRs 4-5 have passed live integration tests against that release and merged;
- the canonical documentation describes only merged behavior;
- follow-up work remains tracked outside the series; and
- PR #471 is closed as superseded with links to the upstream change and all
  five resulting PRs.
