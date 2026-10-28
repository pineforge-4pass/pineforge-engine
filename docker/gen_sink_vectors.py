#!/usr/bin/env python3
"""Generate deterministic pf-report-sink/v1 vectors.

The Python vectors are produced by the Python test fixtures that mirror the
writer's framing, not captured from the C++ writer. Optional native files are
captured from the real C++ writer by capture_native_sink_vector.py.

Usage: python3 -I docker/gen_sink_vectors.py --out DIR --engine-sha SHA [--native-dir DIR]
Requires pytest, as do the Python test fixtures. Generation belongs on a build host.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

DOCKER = Path(__file__).resolve().parent
ROOT = DOCKER.parent
WIRE_VERSION = "pf-report-sink/v1"
COMMITTED_DIRECTORY = "tests/data/sink_vectors_v1"
BAD_STEMS = (
    "bad-trailer-counts",
    "bad-trailer-sha256",
    "truncated-mid-record",
    "truncated-at-line",
)
NATIVE_FILES = (
    "native.pfsink",
    "native.expected.parse.json",
    "native.provenance.json",
)

# -I excludes the script directory. These are the same local fixture imports
# used by pineforge_sink_to_json_test.py, with bytecode writes disabled above.
sys.path.insert(0, str(DOCKER))
sys.path.insert(0, str(ROOT / "tests"))


def canonical_json(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False,
                      separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"{self.prog}: {' '.join(message.splitlines())}\n")


def output_directory(path):
    path = path.resolve()
    if path.is_relative_to(DOCKER):
        raise ValueError("--out must be outside the repository's docker/ directory")
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise ValueError("--out must be a new or empty directory")
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_valid(path, scratch):
    import pineforge_sink_to_json_test as writer
    from test_run_json_sink_summary import SummaryLibrary

    # The synthetic fixture builds E/T plus requested O/V/K. Reuse the existing
    # summary fixture's all_tags branch for X/H and their descriptors; no row
    # values or schema columns are defined in this generator.
    synthetic, _, _ = writer.synthetic(scratch, outputs=True)
    lines = synthetic.read_bytes().splitlines()
    library = SummaryLibrary(all_tags=True)
    library.run_id = json.loads(lines[0])["run_id"]
    augmented = scratch / "all-tags.sink"
    library.write_sink(augmented, [json.loads(line) for line in lines[1:-1]],
                       outputs=True)
    lines = augmented.read_bytes().splitlines()
    writer.write_stream(path, json.loads(lines[0]),
                        (json.loads(line) for line in lines[1:-1]))


def capture_parse(path, *, refusal=False):
    from pf_sink import SinkFormatError, SinkReader

    records, exception = [], None
    with SinkReader(path) as reader:
        try:
            for record in reader:
                records.append({"tag": record.tag, "fields": list(record.fields),
                                "values": list(record.values)})
        except SinkFormatError as error:
            if not refusal:
                raise
            exception = {"class": type(error).__name__, "message": str(error)}
        if refusal and exception is None:
            raise RuntimeError(f"{path.name}: reader accepted a refusal vector")
        result = {"header": reader.header, "records": records,
                  "counts": reader.counts, "trailer": reader.trailer,
                  "sha256": reader.sha256, "complete": reader.complete}
    if exception is not None:
        result["exception"] = exception
    return result


def write_bad_streams(directory):
    from pineforge_sink_to_json_test import wire

    lines = (directory / "valid.pfsink").read_bytes().splitlines(keepends=True)
    trailer = json.loads(lines[-1])
    trailer["counts"]["T"] += 1
    (directory / "bad-trailer-counts.pfsink").write_bytes(
        b"".join(lines[:-1]) + wire(trailer))

    trailer = json.loads(lines[-1])
    digest = trailer["sha256"]
    trailer["sha256"] = ("0" if digest[0] != "0" else "1") + digest[1:]
    (directory / "bad-trailer-sha256.pfsink").write_bytes(
        b"".join(lines[:-1]) + wire(trailer))

    # Both are prefixes of the Python fixture's bytes. The last record loses
    # its LF and part of its body in the first case; only the trailer is cut
    # in the second. Earlier records remain provisional in both cases.
    (directory / "truncated-mid-record.pfsink").write_bytes(
        b"".join(lines[:-2]) + lines[-2][:len(lines[-2]) // 2])
    (directory / "truncated-at-line.pfsink").write_bytes(b"".join(lines[:-1]))


def converter_stdout(path, scratch):
    metadata = scratch / "empty-stdout.json"
    metadata.write_bytes(canonical_json({}))
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-S", str(DOCKER / "pineforge-sink-to-json"),
         "--in", str(path), "--stdout-json", str(metadata), "--out", "-",
         "--max-bytes", str(path.stat().st_size)],
        stdin=subprocess.DEVNULL, capture_output=True)
    if result.returncode:
        raise RuntimeError("converter refused valid vector: "
                           + result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def generate(directory, engine_sha, native_dir=None):
    with tempfile.TemporaryDirectory(prefix="pf-sink-vectors-") as temporary:
        scratch = Path(temporary)
        valid = directory / "valid.pfsink"
        write_valid(valid, scratch)
        parsed = capture_parse(valid)
        if (not parsed["complete"] or set(parsed["counts"]) != set("ETOVKXH")
                or not all(count > 0 for count in parsed["counts"].values())):
            raise RuntimeError("Python test fixtures did not produce all seven record kinds")
        (directory / "valid.expected.parse.json").write_bytes(canonical_json(parsed))
        (directory / "valid.expected.to-json.json").write_bytes(
            converter_stdout(valid, scratch))
        write_bad_streams(directory)
        for stem in BAD_STEMS:
            refusal = capture_parse(directory / (stem + ".pfsink"), refusal=True)
            (directory / (stem + ".expected.refusal.json")).write_bytes(
                canonical_json(refusal))

    if native_dir is not None:
        for name in NATIVE_FILES:
            shutil.copyfile(native_dir / name, directory / name)

    # The manifest describes every other file. Its own digest belongs in an
    # external checksum receipt; including it here would be self-referential.
    files = {}
    for path in sorted(directory.iterdir()):
        data = path.read_bytes()
        files[path.name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if native_dir is not None and path.name in NATIVE_FILES:
            files[path.name]["captured_native"] = True
    generator_command = [
        "python3", "-I", "docker/gen_sink_vectors.py",
        "--out", COMMITTED_DIRECTORY, "--engine-sha", engine_sha,
    ]
    if native_dir is not None:
        generator_command.extend(("--native-dir", "NATIVE_DIR"))
    manifest = {
        "wire_version": WIRE_VERSION,
        "engine_sha": engine_sha,
        # Normalize paths/interpreter so temporary capture and generation
        # paths never enter the manifest.
        "generator_command": generator_command,
        "files": files,
    }
    (directory / "MANIFEST.json").write_bytes(canonical_json(manifest))


def main(argv=None):
    parser = Parser(prog="gen_sink_vectors.py", description=__doc__, allow_abbrev=False)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--engine-sha", required=True)
    parser.add_argument("--native-dir", type=Path,
                        help="Directory containing the three captured native files.")
    args = parser.parse_args(argv)
    if re.fullmatch(r"[0-9a-fA-F]{40}", args.engine_sha) is None:
        parser.error("--engine-sha must contain exactly 40 hexadecimal digits")
    try:
        native_dir = args.native_dir.resolve() if args.native_dir is not None else None
        if native_dir is not None:
            for name in NATIVE_FILES:
                if not (native_dir / name).is_file():
                    raise ValueError(f"--native-dir is missing captured file: {name}")
        directory = output_directory(args.out)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    generate(directory, args.engine_sha.lower(), native_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
