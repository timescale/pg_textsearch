# Large-scale text-search benchmarks

Reproducible, single-host comparisons using
[ParadeDB's benchmarker](https://github.com/paradedb/benchmarker).
Workload shapes follow the [TIN launch](https://planetscale.com/blog/introducing-tin)
and [ParadeDB response](https://www.paradedb.com/blog/opening-a-closed-tin):
ranked disjunctions, conjunctions and phrases, counts, and concurrent updates.

**Lead is not TIN.** It deliberately stores no search index and rescans the
heap. It runs only as a 10,000-document compatibility check, not as a
production-performance competitor. No TIN measurements are inferred from it.

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
corpus-sampled spans are separate inputs. An incomplete or incorrectly sized
corpus is not a full run.

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
24 CPU cores and 192 GiB RAM; the driver uses the remaining eight cores.
PostgreSQL settings, versions, plans and hardware provenance accompany runs.
Only `body` is searched. English stemming/stopwords and BM25 parameters are
explicit; tokenizer differences can still affect cross-engine results.

The standard matrix uses top-10/top-100, 1/8 clients, 60 seconds of excluded
warmup and five measured minutes per cell. `PGTS_INCLUDE_32VU=1` adds 32
clients. Mixed runs target 1,000 indexed updates/second and record achieved
throughput and dropped iterations, not just the requested rate.

Current pg_textsearch supports native Boolean/phrase filtering and counts.
Filtered ranking currently uses standalone scoring and sorting after Boolean
filtering; it is not misrepresented as an integrated ranked index scan.
ParadeDB uses native, field-specific search operators.

Errors/timeouts invalidate a cell. Raw driver outputs and result IDs are
retained; successful throughput is not a substitute for result validation.
Query inputs, data, cloud state and raw results are ignored by Git.

## Existing machine

```bash
cd /opt/searchbench/pg_textsearch
bash benchmarks/scale/run.sh --smoke --run-id smoke-1 all
bash benchmarks/scale/run.sh --dataset wikipedia --run-id wiki-1 all
```

Artifacts default to `/srv/searchbench/pg_textsearch-scale`. The launcher runs
the pipeline as `pgts-benchmark-<RUN_ID>.service`, with a
`launch-<RUN_ID>.log` in that directory. Use a new run ID for changed inputs;
compatible completed cells can be resumed.

Tests, after installing `requirements.txt` in a virtual environment:

```bash
python -m unittest discover -s benchmarks/scale/tests -v
python -m unittest discover -s benchmarks/scale -p 'test_*.py' -v
```
