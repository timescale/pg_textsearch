# Large-scale text-search benchmarks

Reproducible, single-host comparisons using
[ParadeDB's benchmarker](https://github.com/paradedb/benchmarker).
Workload shapes follow the [TIN launch](https://planetscale.com/blog/introducing-tin)
and [ParadeDB response](https://www.paradedb.com/blog/opening-a-closed-tin):
ranked disjunctions, conjunctions and phrases, counts, and concurrent updates.

**Lead is not TIN.** It deliberately stores no search index and rescans the
heap. By default it runs as a 10,000-document compatibility check, not as a
production-performance competitor. No TIN measurements are inferred from it.
Its published image is intentionally a debug build. Compatibility queries
use explicit quoted TinQL OR terms, not TinQL's default implicit AND.

[Full MS MARCO v2 baseline](reports/msmarco-v2-20261119.md).

## Source selection

Defaults reproduce the historical native build
`8923a5ba16df2731627dfbde6f6e3b62e5b8273b`, not the latest release.
Select a release or candidate explicitly with a full commit SHA:

```bash
git fetch origin --tags
# Example released baseline; select the intended release/candidate.
export PGTS_NATIVE_SOURCE="$(git rev-parse 'v1.6.0^{commit}')"
```

This pin is used for native image builds and recorded in plans/image locks.
It is independent of the **harness** `source_ref`. Moving native refs and
abbreviated commits are rejected. Keep new inputs in a new run/state directory;
changing source selection cannot resume completed fleet stages.

## Provision and run

Requires Azure CLI login, SSH, rsync, Python 3, and permission to create
DevTest Labs and compute/network resources in the chosen subscription.
Cloud identifiers and private keys are never stored in source.

```bash
export AZURE_SUBSCRIPTION_ID=...
export AZURE_RESOURCE_GROUP=searchbench-...
export CONTROLLER_IPV4=...        # controller's public IPv4, no CIDR suffix
export SSH_PRIVATE_KEY=...        # existing SSH key; .pub alongside it

# Full English Wikipedia, requiring no MS MARCO agreement
DATASET=wikipedia bash benchmarks/scale/launch.sh all
```

This allocates Ubuntu 24.04 on `Standard_L32as_v3`: 32 vCPUs, 256 GiB RAM,
four local NVMe disks. Azure DevTest Labs sets the VM's native
`expirationDate` to 14 days after provisioning starts. Azure's deletion sweep
can lag expiration by up to 24 hours; this is not a minute-precise billing
cutoff. It does not depend on the guest or controller staying alive.
Network/lab metadata is separate; remove the dedicated lab with `destroy`
when finished.

SSH is restricted to the supplied IPv4 `/32`; PostgreSQL binds only to
localhost. The launcher pins the first SSH host key in its private state
directory. Bootstrap checks Azure instance metadata and initializes only
unused Azure local NVMe disks, never the OS disk. Docker and benchmark data
live on their RAID0 at `/srv/searchbench`.

Both controller and guest require ownership markers and the exact underlying
Compute resource ID. Pre-existing unmarked VMs fail closed: verify their saved
provenance before explicitly repairing tags; status does not repair ownership.
Pinned Go is installed at `/opt/searchbench/toolchains/go1.26.11`.
Foreign Go shims are not replaced, and `/usr/local/go` is untouched.

**NVMe is ephemeral.** Do not deallocate/redeploy the VM expecting data to
survive. Copy results off the machine before expiration.

`launch.sh provision`, `setup`, and `run` run individual stages. `DATASET=smoke`
uses 10,000 synthetic documents to exercise the actual engines and driver.
`RUN_ID` selects a run; `STATE` selects a local, ignored provisioning record.
Set `RESUME=1` to retry provisioning with that record's original expiration.

After the service finishes, `RUN_ID=<run> bash benchmarks/scale/launch.sh collect`
copies its artifacts to the ignored `benchmarks/scale/artifacts/runs/`.
Set `RESULTS_DIR` to use another local destination.

To inspect an ARM deployment without allocating:

```bash
python3 benchmarks/scale/azure.py plan \
  --subscription "$AZURE_SUBSCRIPTION_ID" \
  --group "$AZURE_RESOURCE_GROUP" \
  --ssh-key "$SSH_PRIVATE_KEY.pub" --ssh-cidr "$CONTROLLER_IPV4/32"
```

Destroy early:

```bash
python3 benchmarks/scale/azure.py destroy \
  --subscription "$AZURE_SUBSCRIPTION_ID" --group "$AZURE_RESOURCE_GROUP"
```

## Corpora

MS MARCO passage v2 has **138,364,198 passages**. Its
[terms](https://microsoft.github.io/msmarco/) restrict use to non-commercial
research. Review applicability before explicitly setting
`MSMARCO_ACCEPT_TERMS=1`; the pipeline otherwise refuses to read/download it.
Do not treat a request to provision compute as acceptance of dataset terms.

With applicable terms accepted, `launch.sh all` downloads and verifies the
passage archive and both official dev-query sets. The gate is deliberately
not enabled by default:

```bash
MSMARCO_ACCEPT_TERMS=1 DATASET=msmarco-v2 \
  bash benchmarks/scale/launch.sh all
```

For an already downloaded archive, on the benchmark machine:

```bash
cd /opt/searchbench/pg_textsearch
VENV=/srv/searchbench/pg_textsearch-scale/venv
MSMARCO_ACCEPT_TERMS=1 "$VENV/bin/python" \
  -m benchmarks.scale.prepare_data corpus \
  --archive /path/to/msmarco_v2_passage.tar \
  --output /srv/searchbench/pg_textsearch-scale/msmarco-v2
```

Conversion verifies MD5 `05946bac48a8ffee62e160213eab3fda`, preserves passage
IDs, and checkpoints Parquet shards. Supply the official query TSV files to
`python -m benchmarks.scale.prepare_data queries --help`; real queries and
corpus-sampled spans are separate inputs. Span lengths are interleaved to
reduce prefix bias in slow, time-limited cells.
An incomplete or incorrectly sized corpus is not a full run.

The alternate **6,407,814-article English Wikipedia** snapshot is pinned to
Wikimedia's `20231101.en` dataset revision, with 19.6 GB of raw body text.
Downloads are SHA256-checked and checkpointed. Text is used unmodified;
original article IDs, URLs and titles remain available for attribution under
[CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).
Its queries are article-title-derived and actual document-bounded 2–15-word
spans, not real user queries. It has no relevance judgments and is never
reported as MS MARCO or as a relevance-quality evaluation.

## Method

Engine sources, images, benchmarker, k6 and Python dependencies are pinned or
resolved to recorded digests. Each engine runs alone on the same host, with
24 logical CPUs and 192 GiB RAM; the driver uses the remaining eight CPUs.
PostgreSQL settings, versions, plans and hardware provenance accompany runs.
Only `body` is searched. English stemming/stopwords and BM25 parameters are
explicit; tokenizer differences can still affect cross-engine results.

The standard matrix uses top-10/top-100, 1/8 clients, 60 seconds of excluded
warmup and five measured minutes per cell. `PGTS_INCLUDE_32VU=1` adds 32
clients. Mixed runs target 1,000 indexed updates/second and record achieved
throughput and dropped iterations in `k6-summary.json`, not just the
requested rate. They preallocate 32 writer clients and allow up to 64.
Updates toggle a suffix on indexed text in a deterministic 100,000-row
hotset spaced across the corpus; they are not uniform random writes over
all documents.

Current pg_textsearch supports native Boolean/phrase filtering and counts.
Filtered ranking currently uses standalone scoring and sorting after Boolean
filtering; it is not misrepresented as an integrated ranked index scan.
ParadeDB uses native, field-specific search operators. Phrase queries use
quoted `@@@` parser input: the pinned release's `###` operator incorrectly
collapses positions left by English stopwords. Bound phrase text is
JSON-quoted, not interpolated into SQL.

Errors/timeouts invalidate a cell. Raw driver outputs and result IDs are
retained; successful throughput is not a substitute for result validation.
Do not interpret zero `benchmark_timeouts` as no cancellations: the pinned
SDK's `deadlineReached` flag denotes a warmup boundary, not every request
deadline. SQL cancellations still count as `benchmark_errors` and invalidate
the cell.
Correctness probes have a 30-second per-query budget and incremental
checkpoints. Failed or censored cells do not prevent the remaining workloads
and engines from running; the aggregate run still reports failure.
Query inputs, data, cloud state and raw results are ignored by Git.

After collection, export search-only statistics:

```bash
python3 benchmarks/scale/summarize.py \
  benchmarks/scale/artifacts/runs/<RUN_ID> \
  --engine pg_textsearch --engine paradedb > summary.csv
```

The summary checks raw sample counts against k6 counters and leaves failed
cells' latency/throughput blank. Search QPS excludes updates and the metrics
collector. Elapsed time includes the last response timestamp: the pinned
dashboard can freeze `endTime` after an idle gap
([paradedb/benchmarker#115](https://github.com/paradedb/benchmarker/pull/115)).
An `ok` status records error-free driver completion, not proof of semantic
correctness or ranking quality; inspect the captured result probes too.

## Existing machine

### Expanded TIN scenarios

`tin.py` adds interleaved query families, AND/phrase counts, mixed-family
searches with updates, and a separate Wikipedia count campaign. The primary
analyzer retains English stemming without stopword removal; stopword-enabled
controls remain separate. ParadeDB is pinned to v0.27.0.

All new profiles use eight database vCPUs. Query RAM/buffers are
192/32, 32/24, and 16/12 GiB; build resources are recorded separately.
Both engines use `plan_cache_mode=force_custom_plan` and the same NVMe
planner setting, `random_page_cost=1.1`. These explicitly tuned runs are
separate from the original default-planner baseline.
Each profile starts from an offline clone of the same analyzer index.
The cache-reset hook evicts only that clone's files and verifies residency
with Linux `mincore`, preventing inherited host cache from bypassing the
smaller container limit. It does not drop the host's caches globally.

On the dedicated benchmark machine, after normal setup:

```bash
export MSMARCO_ACCEPT_TERMS=1
export PGTS_CACHE_RESET_HOOK="$PWD/benchmarks/scale/cache_reset.sh"
PYTHON=/srv/searchbench/pg_textsearch-scale/venv/bin/python
"$PYTHON" benchmarks/scale/tin.py plan --run-id tin-v1
"$PYTHON" benchmarks/scale/tin.py all --run-id tin-v1 \
  --sql-shape filter_first
```

Stages are `install`, `prepare`, `prepared`, `build`, and `run`, or `all`.
Keep selectors and SQL shape consistent across stages of a run.
`--profile`, `--analyzer`, `--engine`, `--pack`, `--operation`, and
`--family` select subsets; `--smoke` uses the synthetic corpus.
Use `--dataset wikipedia` with a separate run ID for the Wikipedia control.

Native SQL shapes are explicitly labeled: `original`, `rank_first`, and
`filter_first`. The latter two separate Boolean filtering from ranking
without an arbitrary candidate limit. Both passed independent result
oracles, including a match below the first 2,000 ranked candidates.
Historical full-corpus probes selected `filter_first`: `rank_first`
can exhaust the ranked stream when fewer than ten documents match.
Broad Boolean filters can still be expensive with either shape.
These are baseline choices, not current end-user recommendations. Builds
starting with v1.6.0 support combined Boolean filtering and ranked BM25 scans;
select `original` for their direct-query lane and verify the actual plan.
Native query packs also undergo an all-query plan audit before timing;
an empty analyzed query is retained as a zero-result case, not discarded.
Each concurrent-write cell starts from a separate pristine index clone.

Export expanded results with:

```bash
python3 benchmarks/scale/summarize_tin.py <RUN_DIRECTORY>/tin > summary.csv
```

The export reconciles read/write counters and reports unequal achieved
write pressure separately from SQL failures. Per-kind request counts are
available, but the SDK's aggregated query CSV cannot recover exact
per-kind latency percentiles.

#### Dedicated fleet workers

Split a prepared campaign across six identical machines: one per engine
and memory profile. Each worker runs cells sequentially; do not share a
host between timed workers. Lead is not included.

For controller-managed provisioning and execution:

```bash
mkdir -p benchmarks/scale/artifacts
python3 -m benchmarks.scale.fleet_control schema \
  --source-ref "$(git rev-parse HEAD)" \
  --native-source "$PGTS_NATIVE_SOURCE" --sql-shape original \
  > benchmarks/scale/artifacts/fleet.json
# Fill in private Azure/SSH settings; use a fresh state_dir/campaign_id.
export MSMARCO_ACCEPT_TERMS=1
python3 -m benchmarks.scale.fleet_control all \
  --config benchmarks/scale/artifacts/fleet.json
```

Keep the config and controller state private. Remove `existing_group`
for a fresh six-machine fleet, or set it to adopt a managed source host.
The common expiry is saved once and verified through Azure's native VM
expiration, not just tags; resume does not extend it or provision expired state.
Capacity failures preserve exact failed-attempt groups in `cleanup_required`.
Inspect and explicitly run their recorded marker-guarded `destroy_command`;
resume clears obligations only after Azure reports those groups absent.
No automatic group deletion occurs, and outstanding cleanup blocks completion.
Worker resumes check the campaign/source/command fingerprint before reusing
a service; foreign or legacy units require explicit operator reconciliation.
Individual stages
include `plan`, `provision`, `prepare`, `export`, `stage`, `distribute`,
`preflight`, `run`, `wait`, `status`, `collect`, and `retain`.

The controller stages exports off the source host before its queries
resume, and keeps the staging host idle until all fanout transfers end.
It retains original indexes rather than deleting them after measurement.
The expanded November 19 measurement sources are preserved at
the tag `benchmarks/tin-expanded-20261119-final-v1`; shard manifests also record
the deployed worker overlay's hash.

For an already provisioned worker:

Copy the frozen source, pinned images, benchmarker binaries, dataset
manifests, query packs, analyzer build proofs, and cleanly stopped analyzer
volumes to each worker. Preserve absolute paths, volume names and labels,
and the run ID. Import enriched query packs from the native build for
both engines. No corpus reload or index rebuild is needed.

```bash
"$PYTHON" -m benchmarks.scale.fleet \
  --engine pg_textsearch --profile constrained16 \
  --dataset msmarco-v2 --run-id tin-v1 \
  --artifacts /srv/searchbench/pg_textsearch-scale
```

Use each combination of `pg_textsearch`/`paradedb` and
`high`/`constrained32`/`constrained16` exactly once. Run Wikipedia separately
with its own imported builds and run ID. The worker preserves terminal
results, including failures, and archives interrupted cells before retry.
Per-shard manifests and summaries live under `tin/fleet/shards/`; exit
code 2 denotes measured failures and 3 an infrastructure error.
Record each host's hardware and verify the combined cell coverage before
comparing results. Keep transfer and calibration I/O outside timed phases.

### Retaining historical indexes

The fleet's `retain` stage serves the current campaign's analyzer volumes.
An adopted host may also hold older originals. Inventory those separately
with `docker volume inspect`, using each analyzer's `complete.json`,
`clone.json`, or saved `container-inspect.json` to resolve its exact image.
Do not infer TIN image pins from the baseline `provenance.json`.

On an idle host, pass an explicit private plan to:

```bash
"$PYTHON" -m benchmarks.scale.retain_originals \
  --plan /private/retained-originals.json \
  --report /private/retained-originals-report.json
```

The plan contains `idle_units` (benchmark systemd unit names) and
`databases`. Each database specifies `volume`, `engine`, immutable `image`
digest, unique localhost `port`, the volume's complete `labels`, and
Boolean `expect_search_index`. Exclude disposable write volumes and retired
engines. Only set `expect_search_index` false for a documented incomplete
build without a search index.

The helper verifies ownership, initialized PostgreSQL 18 data, image
layout, ports, and existing-container identity before starting databases.
It never removes containers or volumes. It checks valid BM25 indexes and
runs a small indexed query; these are queryability checks, not benchmarks.
Other containers referencing an original cause a refusal, even if stopped;
preserve and verify those containers separately rather than taking them over.
Use a separate current tools checkout when the measurement tree is frozen.

### Recovering a frozen audit decoder

Current sources handle PostgreSQL notices in EXPLAIN output. For a frozen
campaign stopped by the older decoder, keep its sources unchanged and run
the recovery script from a separate current checkout on the idle worker:

```bash
PYTHONPATH="$FROZEN_REPO" "$PYTHON" \
  "$CURRENT_REPO/benchmarks/scale/recover_audits.py" \
  --root "$FROZEN_REPO" --artifacts /srv/searchbench/pg_textsearch-scale \
  --run-id tin-v1 --profile high \
  --parser-source "$CURRENT_REPO/benchmarks/scale/tin.py"
```

Recovery requires the original recorded parse failure and exactly three
pending stopword-span controls. It preserves failed audit artifacts,
regenerates real EXPLAIN audits, records decoder/checkpoint hashes, and
verifies frozen source hashes. It does not relax plan gates. Restart the
same worker afterward; completed measurements are not repeated.

### Replaying queries on an existing native index

After installing the desired binary and restarting the database (no data
reload or index rebuild), replay a prepared query pack through the direct
Boolean-plus-ranked SQL. Use a build supporting combined Boolean filtering
and BM25 ordering; unsupported plans are rejected.

```bash
PGHOST=127.0.0.1 PGPORT=16432 PGUSER=postgres \
python -m benchmarks.scale.replay_queries \
  --queries /path/to/query-packs/english-no-stopwords/spans.json \
  --kind conjunction --kind phrase --ordinal 0 --ordinal -1 \
  --effective-cache-size 160GB --build-sha SOURCE_COMMIT \
  --output /path/to/new-replay.jsonl
```

Use standard libpq environment variables or a passfile for authentication.
This read-only, persistent single-client diagnostic defaults to the first
and last queries, a 60-second statement timeout, and three warm executions
after `EXPLAIN ANALYZE`. Filtered search plans must contain Boolean conditions and
ordering in the same BM25 Index Scan, without a Sort or materialized CTE.
Ranked OR uses the direct unfiltered ordering and a separate ordered-plan gate.
Returned IDs are checked independently with `to_tsvector`. Use `--all` for
the complete pack or `--operation count` to diagnose unchanged Boolean-only
counts. JSONL files are created exclusively; original results are not
overwritten. This is not a replacement for the concurrent benchmark matrix.

For automated in-place installation, run this on the benchmark host with
the existing worker inactive:

```bash
python -m benchmarks.scale.replace_binary \
  --container CONTAINER --port PORT \
  --expected-image IMAGE_ID --expected-volume VOLUME_NAME \
  --expected-original-sha256 ORIGINAL_HASH \
  --binary /path/to/pg_textsearch.so --binary-sha256 NEW_HASH \
  --source-sha SOURCE_COMMIT --backup-directory /path/to/new-backup \
  --worker-unit WORKER_UNIT
```

This requires the expected running single-volume container, a fixed host
port, and the original library hash. It saves the original library and index
identity, atomically replaces the library, and restarts only that container.
It checks readiness and identity. Failed restarts restore the original
library. Successful reruns require matching provenance and unchanged index
identity; incomplete journals require explicit reconciliation. Keep
the backup and journal: the Docker image ID no longer identifies the
installed runtime binary. For other PostgreSQL versions, pass `--library`.

### Paired diagnostics and CPU profiles

From a checkout on an idle benchmark host, use libpq service/passfiles for
the retained native and ParadeDB endpoints; do not put passwords in arguments.
Run the same prepared pack before and after a guarded native binary replacement:

```bash
python -m benchmarks.scale.compare_queries run \
  --native-dsn 'service=native' --parade-dsn 'service=parade' \
  --queries /path/to/query-pack.json --samples 16 \
  --effective-cache-size 160GB --provided-native-build-sha FULL_COMMIT \
  --output /path/to/before.jsonl
# Repeat as after.jsonl with the candidate installed.
python -m benchmarks.scale.compare_queries analyze \
  --before /path/to/before.jsonl --after /path/to/after.jsonl \
  --output /path/to/comparison.json
```

Use `--all`, explicit `--ordinal`, or `--operation count` as needed.
Sampling is deterministic; single-query packs are supported. EXPLAIN, settings,
index identity, ordered IDs, timings, timeouts, and completion counts are saved.
Analysis rejects incomplete/mismatched runs, reports native changes separately
from ParadeDB drift, and fails for native row changes or unsuccessful queries.
It never excuses changed top-K membership using a score tolerance: investigate
exact cutoff scores/ties independently. These warm sequential diagnostics are
not concurrent QPS measurements and do not evict caches.

For a backend-specific CPU profile with copied runtime symbols:

```bash
# Set absolute PGSERVICEFILE/PGPASSFILE paths first; PYTHON is the host venv.
sudo -A --preserve-env=PGSERVICEFILE,PGPASSFILE \
  "$PYTHON" -m benchmarks.scale.profile_queries \
  --dsn 'service=native' --container RETAINED_CONTAINER \
  --receipt /path/to/deployment.json \
  --queries /path/to/query-pack.json --ordinal 0 --kind disjunction \
  --seconds 10 --output /path/to/new-profile
```

Unprivileged execution works where perf permissions allow it. For an unchanged
initial build, use `--expected-binary-sha256` instead of a deployment receipt.
The profiler verifies ownership, proves its SQL connection belongs to that
container, maps its backend PID, rejects deleted/mismatched mapped binaries,
checks the index identity against the deployment receipt,
and stops only its own perf child. It records EXPLAIN and self/inclusive reports,
without reading Docker passwords or restarting PostgreSQL. Outputs are exclusive;
sudo-created artifacts are returned to the invoking user's ownership.
Keep all profiling and symbol-copy I/O outside timed benchmark phases.

### Post-merge clean-room acceptance

Perform this from a fresh checkout with no session artifacts or existing fleet:

1. Create a Python virtual environment, install `requirements.txt`, and run
   the tests below. Select full harness/native commits and the SQL lane explicitly.
2. Supply Azure login, regional SKU/quota, SSH keys, controller IP, and private
   state paths. Check ARM `plan` before allocation. Never use a shared host or
   adopt an existing group for this rehearsal.
3. Run `DATASET=smoke RUN_ID=clean-smoke bash benchmarks/scale/launch.sh all`.
   Require the named systemd service to finish successfully and all engine
   cells to complete. Collect its artifacts, rerun the same compatible stages,
   then explicitly destroy this smoke lab before allocating the fleet.
4. For the full matrix, review dataset terms before accepting them, generate
   fresh fleet config as above, and run `fleet_control all`. Keep six timed
   workers separate. Interrupt only a worker/controller process—not its VM—
   then resume with unchanged config; verify completed cells were preserved
   and interrupted attempts archived.
5. Require complete 174-cell coverage, resolved cleanup obligations, native
   expiration on every VM, collection, and `retain`. Run paired replay/profile
   commands against those originals; verify unchanged index identities.
6. Copy results and receipts off ephemeral NVMe, explicitly destroy each exact
   owned lab using `azure.py destroy`, and verify the groups are absent.
   Expiration is a backstop, not a substitute for deliberate teardown.

### Original baseline

```bash
cd /opt/searchbench/pg_textsearch
bash benchmarks/scale/tests/check_writer_capacity.sh
bash benchmarks/scale/tests/check_phrase_semantics.sh
bash benchmarks/scale/tests/check_native_rewrites.sh
bash benchmarks/scale/tests/check_tin_drain.sh
bash benchmarks/scale/run.sh --smoke --run-id smoke-1 all
bash benchmarks/scale/run.sh --dataset wikipedia --run-id wiki-1 all
```

The drain check ensures 35-second reads and writes finish accounting after
the measurement window closes. The capacity check uses an isolated 10K-row
database with a controlled 10-ms update delay. The phrase check exercises
the actual k6 driver against
an exact phrase containing a stopword. Run these checks before benchmarks,
with port 15432 free.

Artifacts default to `/srv/searchbench/pg_textsearch-scale`. The launcher runs
the pipeline as `pgts-benchmark-<RUN_ID>.service`, with a
`launch-<RUN_ID>.log` in that directory. Use a new run ID for changed inputs;
compatible completed cells can be resumed.
Retries retain previous logs/exports under the cell's `attempts/` directory,
outside the current attempt used for aggregate statistics.

Tests, after installing `requirements.txt` in a virtual environment:

```bash
python -m unittest discover -s benchmarks/scale/tests -v
python -m unittest discover -s benchmarks/scale -p 'test_*.py' -v
```
