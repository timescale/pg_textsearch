"""Download pinned MS MARCO v2 assets only after explicit terms acceptance."""

import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess

from benchmarks.scale.dataset import (
    DatasetError,
    MSMARCO_TERMS_URL,
    MSMARCO_V2_PASSAGE_MD5,
    MSMARCO_V2_PASSAGE_URL,
    require_terms_acceptance,
    verify_md5,
)


BASE = "https://msmarco.z22.web.core.windows.net/msmarcoranking/"
ASSETS = {
    "archive": {
        "filename": "msmarco_v2_passage.tar", "url": MSMARCO_V2_PASSAGE_URL,
        "md5": MSMARCO_V2_PASSAGE_MD5, "bytes": None,
    },
    "dev1": {
        "filename": "passv2_dev_queries.tsv",
        "url": BASE + "passv2_dev_queries.tsv",
        "md5": "0fa4c6d64a653142ade9fc61d7484239", "bytes": 164507,
    },
    "dev2": {
        "filename": "passv2_dev2_queries.tsv",
        "url": BASE + "passv2_dev2_queries.tsv",
        "md5": "565b84dfa7ccd2f4251fa2debea5947a", "bytes": 179603,
    },
}


def download_assets(
    output: Path, *, accepted: bool = False
) -> dict[str, Path]:
    require_terms_acceptance(accepted)
    output.mkdir(parents=True, exist_ok=True)
    paths = {}
    records = {}
    for name, asset in ASSETS.items():
        target = output / asset["filename"]
        candidate = target
        if not target.exists():
            candidate = target.with_suffix(target.suffix + ".partial")
            subprocess.run([
                "curl", "--fail", "--location", "--silent", "--show-error",
                "--retry", "5", "--retry-all-errors", "--continue-at", "-",
                "--output", str(candidate), asset["url"],
            ], check=True)
        verify_md5(candidate, asset["md5"])
        size = candidate.stat().st_size
        if asset["bytes"] is not None and size != asset["bytes"]:
            raise DatasetError(f"Unexpected byte length for {target.name}")
        if candidate != target:
            candidate.replace(target)
        paths[name] = target
        records[name] = asset | {"bytes": size}
    record = {
        "dataset": "msmarco-passage-v2",
        "terms_acceptance": {
            "accepted": True, "url": MSMARCO_TERMS_URL,
            "recorded_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
        "assets": records,
    }
    manifest = output / "downloads.json"
    partial = manifest.with_suffix(".json.partial")
    partial.write_text(json.dumps(record, indent=2) + "\n")
    partial.replace(manifest)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--accept-msmarco-terms", action="store_true")
    args = parser.parse_args()
    paths = download_assets(args.output, accepted=args.accept_msmarco_terms)
    print(json.dumps(
        {key: str(path) for key, path in paths.items()}, indent=2
    ))


if __name__ == "__main__":
    main()
