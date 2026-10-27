"""Compare real producers and independently validate the version-one wire."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def wire(path: Path, *, simple: bool) -> None:
    digest = hashlib.sha256()
    counts: dict[str, int] = {}
    trades = hashes = trailers = 0
    with path.open("rb") as source:
        for number, line in enumerate(source):
            assert line.endswith(b"\n")
            row = json.loads(line)
            if number == 0:
                assert row["schema"] == "pf-report-sink" and row["version"] == 1
                counts = {item["type"]: 0 for item in row["records"]}
            elif isinstance(row, dict):
                assert row["trailer"] == "pf-report-sink" and row["version"] == 1
                assert row["counts"] == counts
                assert row["sha256"] == digest.hexdigest()
                trailers += 1
                assert source.read() == b""
                break
            else:
                counts[row[0]] += 1
                if row[0] == "T":
                    trades += 1
                    assert len(row) == 21 and row[1] == trades
                    assert row[5:9] == [100, 100, 1, 0]
                    assert row[9:13] == [0, 0, 0, 0]
                    if simple:
                        # 4,001 close-on-close entries, 4,000 reversals, and
                        # one reporting-only open lot. These are tape facts.
                        assert row[2] == ("long" if trades % 2 else "short")
                        assert row[13] == trades - 1
                        assert row[14] == min(trades, 4000)
                        assert row[15] is (trades == 4001)
                        assert row[18] == "flip"
                elif row[0] == "H":
                    hashes += 1
                    assert len(row[1]) == 16 and int(row[1], 16) != 0
            digest.update(line)
    assert trades > 0 and hashes > 0 and trailers == 1
    if simple:
        assert trades == counts["E"] == hashes == 4001
    else:
        assert trades == 200 and counts["E"] == hashes == 600


SHAPES = ("reversal", "shapes", "wide", "indexed")
SUFFIXES = ("sink", "hashes", "report")


def record_reference(executable: str, directory: Path) -> None:
    """The supervisor supplies a binary linked against the unchanged engine."""
    directory.mkdir(parents=True, exist_ok=True)
    assert not list(directory.iterdir()), "reference directory must start empty"
    subprocess.run([executable, "--artifacts", str(directory)], check=True)
    manifest = {}
    for shape in SHAPES:
        wire(directory / f"{shape}.sink", simple=shape != "shapes")
        for suffix in SUFFIXES:
            name = f"{shape}.{suffix}"
            manifest[name] = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    (directory / "manifest.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")


def main() -> None:
    if len(sys.argv) == 4 and sys.argv[1] == "--write-reference":
        record_reference(sys.argv[2], Path(sys.argv[3]))
        return
    assert len(sys.argv) == 4
    reference = os.environ.get("PINEFORGE_TRADE_HISTORY_REFERENCE")
    assert reference, (
        "PINEFORGE_TRADE_HISTORY_REFERENCE must name artifacts made with "
        "--write-reference and the unchanged-engine legacy executable")
    reference = Path(reference)
    manifest = json.loads((reference / "manifest.json").read_text())
    assert set(manifest) == {f"{shape}.{suffix}" for shape in SHAPES for suffix in SUFFIXES}
    with tempfile.TemporaryDirectory(prefix="pineforge-trade-identity-") as name:
        root = Path(name)
        outputs = []
        for index, executable in enumerate(sys.argv[1:]):
            directory = root / str(index)
            directory.mkdir()
            subprocess.run([executable, "--artifacts", str(directory)], check=True)
            outputs.append(directory)
        for shape in SHAPES:
            for output in outputs:
                wire(output / f"{shape}.sink", simple=shape != "shapes")
                rows = (output / f"{shape}.hashes").read_bytes().splitlines()
                assert len(rows) == (600 if shape == "shapes" else 4001)
                assert all(len(row.split()) == 2 for row in rows)
            for suffix in SUFFIXES:
                name = f"{shape}.{suffix}"
                expected = (reference / name).read_bytes()
                assert expected and hashlib.sha256(expected).hexdigest() == manifest[name]
                for output in outputs:
                    assert (output / f"{shape}.{suffix}").read_bytes() == expected


if __name__ == "__main__":
    main()
