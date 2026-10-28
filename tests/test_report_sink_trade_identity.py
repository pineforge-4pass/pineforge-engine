"""Compare real producers and independently validate the version-one wire."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
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
ARTIFACT_NAMES = {f"{shape}.{suffix}" for shape in SHAPES for suffix in SUFFIXES}
COMMITTED_REFERENCE = Path(__file__).resolve().parent / "data/trade_history_reference"
MAX_REFERENCE_BYTES = 5_000_000
MISSING_REFERENCE = (
    "PINEFORGE_TRADE_HISTORY_REFERENCE must name artifacts made with "
    "--write-reference and the unchanged-engine legacy executable")


class ReferenceError(ValueError):
    """A frozen reference is absent, incomplete, or changed."""


def unique_manifest(pairs: list[tuple[str, str]]) -> dict[str, str]:
    result = {}
    for name, value in pairs:
        if name in result:
            raise ReferenceError(f"duplicate reference manifest entry: {name}")
        result[name] = value
    return result


def verify_reference(directory: Path, *, committed: bool = True) -> dict[str, str]:
    """Check every expected byte before starting any producer.

    Repository data requires the complete SHA-256 manifest, including the base
    pin and JSON manifest. Explicit lab overrides also accept the original
    --write-reference format (twelve artifacts and manifest.json only).
    """
    try:
        if directory.is_symlink() or not directory.is_dir():
            raise ReferenceError(f"reference directory is absent: {directory}")
        paths = {path.name: path for path in directory.iterdir()}
        full_manifest = "MANIFEST.sha256" in paths
        expected = ARTIFACT_NAMES | {"manifest.json"}
        if committed or full_manifest:
            expected |= {"BASE_COMMIT", "MANIFEST.sha256"}
        if set(paths) != expected:
            raise ReferenceError(
                f"reference inventory mismatch: missing {sorted(expected - paths.keys())}, "
                f"unexpected {sorted(paths.keys() - expected)}")
        if any(path.is_symlink() or not path.is_file() for path in paths.values()):
            raise ReferenceError("reference files must be regular files, not symlinks")
        if committed:
            total = sum(path.stat().st_size for path in paths.values())
            if total > MAX_REFERENCE_BYTES:
                raise ReferenceError(f"committed reference exceeds {MAX_REFERENCE_BYTES} bytes")

        if full_manifest:
            checksums = {}
            for line in paths["MANIFEST.sha256"].read_bytes().splitlines(keepends=True):
                match = re.fullmatch(rb"([0-9a-f]{64})  ([A-Za-z0-9_.-]+)\n", line)
                if match is None:
                    raise ReferenceError("malformed MANIFEST.sha256 entry")
                digest, name = (part.decode("ascii") for part in match.groups())
                if name in checksums:
                    raise ReferenceError(f"duplicate MANIFEST.sha256 entry: {name}")
                checksums[name] = digest
            if set(checksums) != expected - {"MANIFEST.sha256"}:
                raise ReferenceError("MANIFEST.sha256 must cover every other reference file")
            for name, digest in checksums.items():
                if hashlib.sha256(paths[name].read_bytes()).hexdigest() != digest:
                    raise ReferenceError(f"reference SHA-256 mismatch: {name}")
            if not re.fullmatch(rb"[0-9a-f]{40}\n", paths["BASE_COMMIT"].read_bytes()):
                raise ReferenceError("BASE_COMMIT must contain one full lowercase commit and LF")

        manifest = json.loads(paths["manifest.json"].read_text(encoding="utf-8"),
                              object_pairs_hook=unique_manifest)
        if not isinstance(manifest, dict) or set(manifest) != ARTIFACT_NAMES:
            raise ReferenceError("manifest.json must cover exactly the twelve artifacts")
        for name, digest in manifest.items():
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ReferenceError(f"invalid reference SHA-256: {name}")
            data = paths[name].read_bytes()
            if not data:
                raise ReferenceError(f"empty reference artifact: {name}")
            if hashlib.sha256(data).hexdigest() != digest:
                raise ReferenceError(f"reference SHA-256 mismatch: {name}")
        return manifest
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReferenceError(f"unreadable reference: {directory}: {error}") from error


def load_reference() -> tuple[Path, dict[str, str]]:
    override = os.environ.get("PINEFORGE_TRADE_HISTORY_REFERENCE")
    if override:
        directory = Path(override)
        return directory, verify_reference(directory, committed=False)
    if not (COMMITTED_REFERENCE / "MANIFEST.sha256").is_file():
        raise ReferenceError(MISSING_REFERENCE)
    return COMMITTED_REFERENCE, verify_reference(COMMITTED_REFERENCE)


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


# Match only the top-level engine string in the canonical v1 header. Matching
# the preceding run_id as a JSON string prevents its contents from looking
# like another key. Never search record bodies or reserialize a JSON object.
JSON_STRING = rb'"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
HEADER_ENGINE = re.compile(
    rb'\A\{"schema":"pf-report-sink","version":1,"run_id":' + JSON_STRING
    + rb',"engine":(?P<engine>' + JSON_STRING + rb')(?=,"records":\[)')
TRAILER_DIGEST = re.compile(
    rb'\A\{"trailer":"pf-report-sink","version":1,"counts":\{[^{}\r\n]*\},'
    rb'"sha256":"(?P<digest>[0-9a-f]{64})"\}\n\Z')


def comparison_bytes(name: str, data: bytes) -> bytes:
    """Normalize only the sink header's engine and its dependent checksum.

    Both sides retain every other byte. Verify the original checksum before
    recomputing it over a header with a fixed engine token; never mask a bad
    checksum. Reference manifests always verify the unmodified frozen bytes.
    See docs/trade-history-reference.md for the comparison contract.
    """
    if not name.endswith(".sink"):
        return data
    header_end = data.find(b"\n")
    trailer_start = data.rfind(b"\n", 0, len(data) - 1) + 1
    if not data.endswith(b"\n") or not 0 < header_end < trailer_start:
        raise AssertionError(f"trade-history artifact mismatch: {name}: incomplete sink")
    engine = HEADER_ENGINE.match(data[:header_end])
    prefix, trailer = data[:trailer_start], data[trailer_start:]
    digest = TRAILER_DIGEST.fullmatch(trailer)
    if engine is None or digest is None:
        raise AssertionError(f"trade-history artifact mismatch: {name}: invalid sink framing")
    if digest["digest"] != hashlib.sha256(prefix).hexdigest().encode("ascii"):
        raise AssertionError(f"trade-history artifact mismatch: {name}: sink SHA-256 mismatch")
    prefix = (prefix[:engine.start("engine")] + b'"<engine-version>"'
              + prefix[engine.end("engine"):])
    trailer = (trailer[:digest.start("digest")]
               + hashlib.sha256(prefix).hexdigest().encode("ascii")
               + trailer[digest.end("digest"):])
    return prefix + trailer


def compare_artifact(name: str, expected: bytes, actual: bytes) -> None:
    if comparison_bytes(name, expected) != comparison_bytes(name, actual):
        raise AssertionError(f"trade-history artifact mismatch: {name}")


def main() -> None:
    if len(sys.argv) == 4 and sys.argv[1] == "--write-reference":
        record_reference(sys.argv[2], Path(sys.argv[3]))
        return
    if len(sys.argv) == 3 and sys.argv[1] == "--verify-reference":
        verify_reference(Path(sys.argv[2]))
        return
    assert len(sys.argv) == 4
    reference, manifest = load_reference()
    with tempfile.TemporaryDirectory(prefix="pineforge-trade-identity-") as name:
        root = Path(name)
        outputs = []
        for index, executable in enumerate(sys.argv[1:]):
            directory = root / str(index)
            directory.mkdir()
            subprocess.run([executable, "--artifacts", str(directory)], check=True)
            outputs.append(directory)
        for shape in SHAPES:
            for suffix in SUFFIXES:
                name = f"{shape}.{suffix}"
                expected = (reference / name).read_bytes()
                assert expected and hashlib.sha256(expected).hexdigest() == manifest[name]
                for output in outputs:
                    compare_artifact(name, expected, (output / name).read_bytes())
            for output in outputs:
                wire(output / f"{shape}.sink", simple=shape != "shapes")
                rows = (output / f"{shape}.hashes").read_bytes().splitlines()
                assert len(rows) == (600 if shape == "shapes" else 4001)
                assert all(len(row.split()) == 2 for row in rows)


if __name__ == "__main__":
    main()
