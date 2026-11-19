#!/usr/bin/env python3
"""MS MARCO v2 corpus validation and streaming conversion primitives."""

from dataclasses import dataclass
import gzip
import hashlib
import json
import os
from pathlib import Path
import random
import tarfile
from typing import Callable, Iterable, Iterator, Mapping, Protocol


MSMARCO_TERMS_URL = "https://microsoft.github.io/msmarco/"
MSMARCO_V2_PASSAGE_URL = (
    "https://msmarco.z22.web.core.windows.net/msmarcoranking/"
    "msmarco_v2_passage.tar"
)
MSMARCO_V2_PASSAGE_MD5 = "05946bac48a8ffee62e160213eab3fda"
MSMARCO_V2_PASSAGE_ROWS = 138_364_198


class DatasetError(RuntimeError):
    """A safe, actionable dataset preparation error."""


@dataclass(frozen=True)
class ArchiveMember:
    name: str
    rows: Iterable[tuple[int, str, str]]


class RowWriter(Protocol):
    def write_rows(self, rows: list[tuple[int, str, str]]) -> None: ...

    def close(self) -> None: ...


def require_terms_acceptance(
    accepted_flag: bool, environment: Mapping[str, str] | None = None
) -> None:
    """Refuse dataset access until the operator explicitly accepts its terms."""
    environment = os.environ if environment is None else environment
    accepted_env = environment.get("MSMARCO_ACCEPT_TERMS", "").lower()
    if not accepted_flag and accepted_env not in {"1", "true", "yes"}:
        raise DatasetError(
            "MS MARCO terms were not accepted. Review "
            f"{MSMARCO_TERMS_URL} and pass --accept-msmarco-terms or set "
            "MSMARCO_ACCEPT_TERMS=1. The dataset is for non-commercial "
            "research use."
        )


def verify_md5(path: Path, expected: str) -> str:
    """Verify a file without loading it into memory."""
    digest = hashlib.md5()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual != expected.lower():
        raise DatasetError(
            f"checksum mismatch for {path.name}: expected {expected.lower()}, "
            f"actual {actual}"
        )
    return actual


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _member_rows(
    compressed, member_name: str, next_id: list[int]
) -> Iterator[tuple[int, str, str]]:
    try:
        stream = gzip.GzipFile(fileobj=compressed, mode="rb")
        for line_number, raw in enumerate(stream, 1):
            try:
                record = json.loads(raw)
                source_id = record["pid"]
                body = record["passage"]
                if not isinstance(source_id, str) or not isinstance(body, str):
                    raise TypeError("pid and passage must be strings")
            except (json.JSONDecodeError, KeyError, TypeError) as error:
                raise DatasetError(
                    f"invalid passage record at {member_name}:{line_number} "
                    f"({type(error).__name__})"
                ) from error
            row_id = next_id[0]
            next_id[0] += 1
            yield row_id, source_id, body
    except (OSError, EOFError) as error:
        raise DatasetError(
            f"cannot decompress corpus member {member_name}: "
            f"{type(error).__name__}"
        ) from error


def iter_archive_members(
    path: Path,
    *,
    skip_members: Mapping[str, int] | None = None,
) -> Iterator[ArchiveMember]:
    """Yield each gzip JSONL member and its rows in archive order.

    The caller must fully consume one member's rows before requesting the next
    member. This keeps memory bounded while retaining deterministic numeric IDs.
    """
    next_id = [1]
    skip_members = {} if skip_members is None else skip_members
    found = False
    try:
        with tarfile.open(path, mode="r|*") as archive:
            for member in archive:
                if not member.isfile() or not member.name.endswith(".gz"):
                    continue
                compressed = archive.extractfile(member)
                if compressed is None:
                    raise DatasetError(
                        f"cannot read corpus member {member.name}"
                    )
                found = True
                if member.name in skip_members:
                    skipped = skip_members[member.name]
                    if not isinstance(skipped, int) or skipped < 0:
                        raise DatasetError(
                            f"invalid skipped row count for {member.name}"
                        )
                    next_id[0] += skipped
                    compressed.close()
                    yield ArchiveMember(member.name, ())
                    continue
                yield ArchiveMember(
                    member.name,
                    _member_rows(compressed, member.name, next_id),
                )
    except (tarfile.TarError, OSError) as error:
        raise DatasetError(
            f"cannot read corpus archive {path.name}: {type(error).__name__}"
        ) from error
    if not found:
        raise DatasetError(f"corpus archive {path.name} has no gzip members")


class _PyArrowWriter:
    def __init__(self, path: Path):
        try:
            import pyarrow as pa
            import pyarrow.parquet as pq
        except ImportError as error:
            raise DatasetError(
                "Parquet conversion requires the pinned Python dependencies; "
                "run python3 -m pip install -r "
                "benchmarks/scale/requirements.txt"
            ) from error
        self._pa = pa
        schema = pa.schema(
            [
                pa.field("id", pa.int64(), nullable=False),
                pa.field("source_id", pa.string(), nullable=False),
                pa.field("body", pa.string(), nullable=False),
            ]
        )
        self._schema = schema
        self._writer = pq.ParquetWriter(
            path,
            schema,
            compression="zstd",
            use_dictionary=["source_id"],
            write_statistics=True,
        )

    def write_rows(self, rows: list[tuple[int, str, str]]) -> None:
        columns = list(zip(*rows))
        table = self._pa.Table.from_arrays(
            [
                self._pa.array(columns[0], type=self._pa.int64()),
                self._pa.array(columns[1], type=self._pa.string()),
                self._pa.array(columns[2], type=self._pa.string()),
            ],
            schema=self._schema,
        )
        self._writer.write_table(table)

    def close(self) -> None:
        self._writer.close()


def _write_manifest(path: Path, manifest: dict[str, object]) -> None:
    temporary = path.with_suffix(".json.partial")
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _load_manifest(
    path: Path, expected_md5: str
) -> dict[str, object]:
    if not path.exists():
        return {
            "format": 1,
            "dataset": "msmarco-v2",
            "source_md5": expected_md5,
            "columns": {
                "id": "int64",
                "source_id": "string",
                "body": "string",
            },
            "members": [],
            "rows": 0,
            "complete": False,
        }
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DatasetError(
            f"cannot read conversion checkpoint {path.name}"
        ) from error
    if manifest.get("source_md5") != expected_md5:
        raise DatasetError(
            "conversion checkpoint belongs to a different source archive"
        )
    if not isinstance(manifest.get("members"), list):
        raise DatasetError("conversion checkpoint has invalid members")
    return manifest


def prepare_parquet(
    archive: Path,
    output: Path,
    *,
    expected_md5: str = MSMARCO_V2_PASSAGE_MD5,
    expected_rows: int = MSMARCO_V2_PASSAGE_ROWS,
    writer_factory: Callable[[Path], RowWriter] = _PyArrowWriter,
    batch_rows: int = 50_000,
) -> dict[str, object]:
    """Convert the archive into resumable member-aligned Parquet shards."""
    if batch_rows <= 0:
        raise DatasetError("batch_rows must be greater than zero")
    if expected_rows <= 0:
        raise DatasetError("expected_rows must be greater than zero")
    source_md5 = verify_md5(archive, expected_md5)
    data_dir = output / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "manifest.json"
    manifest = _load_manifest(manifest_path, source_md5)
    completed = manifest["members"]

    skipped = {
        member["archive_member"]: member["rows"] for member in completed
    }
    for index, archive_member in enumerate(
        iter_archive_members(archive, skip_members=skipped)
    ):
        if index < len(completed):
            checkpoint = completed[index]
            if checkpoint.get("archive_member") != archive_member.name:
                raise DatasetError(
                    "conversion checkpoint member order does not match archive"
                )
            shard = data_dir / checkpoint["file"]
            if not shard.is_file() or _sha256(shard) != checkpoint.get(
                "sha256"
            ):
                raise DatasetError(
                    f"converted shard failed checksum validation: {shard.name}"
                )
            continue

        shard_name = f"part-{index:05d}.parquet"
        shard = data_dir / shard_name
        temporary = data_dir / f"{shard_name}.partial"
        temporary.unlink(missing_ok=True)
        writer = writer_factory(temporary)
        rows = 0
        batch: list[tuple[int, str, str]] = []
        try:
            for row in archive_member.rows:
                batch.append(row)
                rows += 1
                if len(batch) == batch_rows:
                    writer.write_rows(batch)
                    batch = []
            if batch:
                writer.write_rows(batch)
            writer.close()
        except BaseException:
            try:
                writer.close()
            finally:
                temporary.unlink(missing_ok=True)
            raise
        temporary.replace(shard)
        completed.append(
            {
                "archive_member": archive_member.name,
                "file": shard_name,
                "rows": rows,
                "sha256": _sha256(shard),
            }
        )
        manifest["rows"] = sum(member["rows"] for member in completed)
        _write_manifest(manifest_path, manifest)

    converted_rows = sum(member["rows"] for member in completed)
    if converted_rows != expected_rows:
        raise DatasetError(
            f"expected {expected_rows} rows, converted {converted_rows}"
        )
    manifest["rows"] = converted_rows
    manifest["complete"] = True
    _write_manifest(manifest_path, manifest)
    return manifest


_SYNTHETIC_TERMS = (
    "archive benchmark browser cache catalog cluster document engine exact "
    "filter index language latency memory metric network passage query rank "
    "result search segment storage system telemetry token update vector "
    "workload"
).split()


def prepare_synthetic(
    output: Path,
    *,
    rows: int = 10_000,
    seed: int = 20260304,
    writer_factory: Callable[[Path], RowWriter] = _PyArrowWriter,
    batch_rows: int = 10_000,
) -> dict[str, object]:
    """Create a deterministic smoke corpus, never a scale result corpus."""
    if not 1 <= rows <= 1_000_000:
        raise DatasetError("synthetic rows must be between 1 and 1000000")
    if batch_rows <= 0:
        raise DatasetError("batch_rows must be greater than zero")
    data_dir = output / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    shard = data_dir / "part-00000.parquet"
    temporary = data_dir / "part-00000.parquet.partial"
    temporary.unlink(missing_ok=True)
    writer = writer_factory(temporary)
    rng = random.Random(seed)
    batch: list[tuple[int, str, str]] = []
    try:
        for row_id in range(1, rows + 1):
            term_count = 3 + rng.randrange(18)
            body = " ".join(
                rng.choice(_SYNTHETIC_TERMS)
                for _ in range(term_count)
            )
            batch.append(
                (row_id, f"synthetic-{row_id:09d}", body)
            )
            if len(batch) == batch_rows:
                writer.write_rows(batch)
                batch = []
        if batch:
            writer.write_rows(batch)
        writer.close()
    except BaseException:
        try:
            writer.close()
        finally:
            temporary.unlink(missing_ok=True)
        raise
    temporary.replace(shard)
    manifest = {
        "format": 1,
        "dataset": "synthetic-smoke",
        "seed": seed,
        "rows": rows,
        "complete": True,
        "comparable_to_full_scale": False,
        "warning": (
            "Synthetic smoke data validates the pipeline only and must not "
            "be reported as a performance comparison."
        ),
        "columns": {
            "id": "int64",
            "source_id": "string",
            "body": "string",
        },
        "members": [
            {
                "file": shard.name,
                "rows": rows,
                "sha256": _sha256(shard),
            }
        ],
    }
    _write_manifest(output / "manifest.json", manifest)
    return manifest
