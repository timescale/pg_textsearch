#!/usr/bin/env python3
"""Prepare the full CC-BY-SA English Wikipedia as an alternate scale corpus."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.request


REVISION = "b04c8d1ceb2f5cd4588862100d08de323dccfbaa"
CONFIG = "20231101.en"
EXPECTED_ROWS = 6_407_814
DATASET = "wikipedia-20231101-en"
SOURCE = "https://huggingface.co/datasets/wikimedia/wikipedia"
LICENSE = "https://creativecommons.org/licenses/by-sa/3.0/"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024**2), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, output, expected_sha):
    if output.exists():
        if sha256(output) != expected_sha:
            raise ValueError(f"Existing download has bad SHA256: {output.name}")
        return
    partial = output.with_suffix(output.suffix + ".partial")
    # curl resumes large public files and retries transient failures.
    subprocess.run(
        ["curl", "--fail", "--location", "--retry", "5", "--retry-all-errors",
         "--continue-at", "-", "--output", str(partial), url], check=True,
    )
    if sha256(partial) != expected_sha:
        raise ValueError(f"Download SHA256 mismatch: {output.name}")
    partial.replace(output)


def article_queries(source_id, body, seed):
    words = list(re.finditer(r"[A-Za-z0-9]+", body))
    result = []
    for span in range(2, 16):
        if len(words) < span:
            continue
        rank = hashlib.sha256(
            f"{seed}:{source_id}:{span}".encode(),
        ).digest()
        start = int.from_bytes(rank[:8], "big") % (len(words) - span + 1)
        # Reject punctuation-crossing spans rather than changing phrase meaning.
        original = body[words[start].start():words[start + span - 1].end()]
        if not re.fullmatch(r"[A-Za-z0-9]+(?:\s+[A-Za-z0-9]+)*", original):
            continue
        result.append({
            "id": f"wiki-{source_id}-{span}", "text": original.lower(),
            "source": "wikipedia-corpus-span", "source_doc_id": source_id,
            "span": span, "seed": seed,
        })
    return result


def convert_shard(source, output, *, seed):
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq

    queries = []
    titles = []
    rows = 0
    text_bytes = 0
    schema = pa.schema([
        pa.field("id", pa.int64(), nullable=False),
        pa.field("source_id", pa.string(), nullable=False),
        pa.field("body", pa.string(), nullable=False),
    ])
    partial = output.with_suffix(".parquet.partial")
    with pq.ParquetWriter(partial, schema, compression="zstd") as writer:
        for batch in pq.ParquetFile(source).iter_batches(batch_size=512):
            table = pa.Table.from_batches([batch])
            if any(table[name].null_count for name in ("id", "text", "title")):
                raise ValueError(f"Unexpected null article in {source.name}")
            ids = pc.cast(table["id"], pa.int64(), safe=True)
            source_ids = pc.cast(table["id"], pa.string(), safe=True)
            body = pc.cast(table["text"], pa.string(), safe=True)
            converted = pa.Table.from_arrays([ids, source_ids, body],
                                             schema=schema)
            writer.write_table(converted)
            rows += len(batch)
            text_bytes += pc.sum(pc.binary_length(body)).as_py() or 0
            # Stable, bounded sample spread throughout every source shard.
            for offset, record in enumerate(
                table.select(["id", "text", "title"]).to_pylist()
            ):
                identifier = str(record["id"])
                rank = hashlib.sha256(
                    f"{seed}:{identifier}".encode(),
                ).digest()
                first_article = rows == len(batch) and offset == 0
                if first_article or int.from_bytes(rank[:4], "big") % 1000 == 0:
                    queries.extend(article_queries(identifier, record["text"],
                                                   seed))
                    titles.append({
                        "id": f"wiki-title-{identifier}",
                        "text": record["title"],
                        "source": "wikipedia-title-derived-not-natural-query",
                        "source_doc_id": identifier,
                    })
    partial.replace(output)
    return {
        "rows": rows, "text_bytes": text_bytes,
        "sha256": sha256(output), "file": output.name,
        "titles": titles,
    }, queries


def write_json(path, value):
    partial = path.with_suffix(path.suffix + ".partial")
    partial.write_text(json.dumps(value, indent=2) + "\n")
    partial.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261119)
    args = parser.parse_args()
    root = args.output.resolve()
    for directory in ("raw", "data", "checkpoints", "k6"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    tree_path = root / "source-files.json"
    if not tree_path.exists():
        url = (f"https://huggingface.co/api/datasets/wikimedia/wikipedia/tree/"
               f"{REVISION}/{CONFIG}?recursive=false&limit=100")
        with urllib.request.urlopen(url, timeout=60) as response:
            files = json.load(response)
        if len(files) != 41:
            raise ValueError("Pinned Wikipedia source must have 41 shards")
        write_json(tree_path, files)
    files = json.loads(tree_path.read_text())
    queries, titles, shards = [], [], []
    for number, entry in enumerate(sorted(files, key=lambda e: e["path"])):
        if not re.fullmatch(
            r"20231101\.en/train-[0-9]{5}-of-00041\.parquet", entry["path"],
        ):
            raise ValueError("Unexpected filename in pinned source manifest")
        source = root / "raw" / Path(entry["path"]).name
        url = f"{SOURCE}/resolve/{REVISION}/{entry['path']}?download=true"
        download(url, source, entry["lfs"]["oid"])
        output = root / "data" / f"part-{number:05d}.parquet"
        checkpoint = root / "checkpoints" / f"{number:05d}.json"
        if checkpoint.exists():
            saved = json.loads(checkpoint.read_text())
            info, sampled = saved["info"], saved["queries"]
            if (saved["seed"] != args.seed or not output.exists()
                    or sha256(output) != info["sha256"]):
                raise ValueError("Incompatible or corrupted conversion checkpoint")
        else:
            info, sampled = convert_shard(source, output, seed=args.seed)
            write_json(checkpoint, {
                "seed": args.seed, "info": info, "queries": sampled,
            })
        titles.extend(info["titles"])
        queries.extend(sampled)
        shards.append({k: v for k, v in info.items() if k != "titles"})
        print(f"Converted {number + 1}/41 shards, "
              f"{sum(shard['rows'] for shard in shards):,} articles", flush=True)
    rows = sum(shard["rows"] for shard in shards)
    if rows != EXPECTED_ROWS:
        raise ValueError(f"Expected {EXPECTED_ROWS:,} articles, got {rows:,}")
    queries.sort(key=lambda q: hashlib.sha256(q["id"].encode()).hexdigest())
    titles.sort(key=lambda q: hashlib.sha256(q["id"].encode()).hexdigest())
    # Equal counts by span length; no spans cross article boundaries.
    selected = []
    for span in range(2, 16):
        selected.extend([q for q in queries if q["span"] == span][:100])
    if len(selected) != 1400:
        raise ValueError("Corpus did not yield enough controlled-span queries")
    write_json(root / "k6" / "spans.json", selected)
    write_json(root / "k6" / "natural.json", titles[:1000])
    write_json(root / "manifest.json", {
        "dataset": DATASET, "complete": True, "rows": rows,
        "text_bytes": sum(shard["text_bytes"] for shard in shards),
        "source": SOURCE, "revision": REVISION, "config": CONFIG,
        "license": LICENSE, "attribution": (
            "Wikipedia contributors; source article IDs preserved in source_id. "
            "Original article URLs and titles remain in the pinned raw shards."
        ),
        "columns": {"id": "int64", "source_id": "string", "body": "string"},
        "query_note": "Corpus spans and article titles, not natural queries; "
                      "no relevance judgments, no MS MARCO results.",
        "shards": shards, "seed": args.seed,
    })


if __name__ == "__main__":
    main()
