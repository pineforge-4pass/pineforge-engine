#!/usr/bin/env python3
"""Save a pf-report-sink/v1 vector captured from the real C++ writer.

Usage: python3 -I docker/capture_native_sink_vector.py --so STRATEGY.so --bars BARS.csv --out DIR
Supply a built hand-written strategy library and a small one-minute bar CSV
that produces at least one E record and one T record. Execution belongs on a
build host. The sink is copied verbatim; only expectations use JSON encoding.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

DOCKER = Path(__file__).resolve().parent
ROOT = DOCKER.parent
RUN_ID = "pf-sink-vector-v1-native"
RECORD_KINDS = "ETOVKXH"

# -I excludes the script directory. These helpers import the Python test
# fixtures only when generating Python vectors, never during native capture.
sys.path.insert(0, str(DOCKER))
from gen_sink_vectors import Parser, canonical_json, capture_parse, output_directory


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_json_command(library, bars, sink):
    # Direct subprocess: tests/test_run_json_sink_fd.py:904.
    # Named path, timeframes and run id: tests/test_run_json_sink_fd.py:881.
    # Explicit path-compatible profile: tests/test_run_json_sink_fd.py:1144.
    return [
        sys.executable, "-B", str(DOCKER / "run_json.py"),
        "--so", str(library), "--ohlcv", str(bars),
        "--input-tf", "1", "--script-tf", "1",
        "--profile", "unjailed", "--report-sink", str(sink),
        "--run-id", RUN_ID,
    ]


def capture(directory, library, bars):
    strategy_sha256, bars_sha256 = sha256_file(library), sha256_file(bars)
    with tempfile.TemporaryDirectory(prefix="pf-native-sink-vector-") as temporary:
        scratch = Path(temporary)
        sink = scratch / "writer.pfsink"
        command = run_json_command(library, bars, sink)
        # Match the direct native subprocess test's environment isolation
        # (tests/test_run_json_sink_fd.py:898); no inherited scratch arena.
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("PINEFORGE_", "PF_SCRATCH_"))}
        environment.update(PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(scratch))
        result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                cwd=ROOT, env=environment)
        if result.returncode:
            raise RuntimeError(
                f"run_json.py exited {result.returncode}: "
                + result.stderr.decode("utf-8", errors="replace")
                + result.stdout.decode("utf-8", errors="replace"))

        # capture_parse consumes pf_sink.SinkReader through trailer verification
        # and returns the same shape used by valid.expected.parse.json.
        parsed = capture_parse(sink)
        if not parsed["complete"]:
            raise RuntimeError("native stream is not complete")
        if any(parsed["counts"].get(tag, 0) == 0 for tag in "ET"):
            raise RuntimeError("native stream needs at least one E and one T record")
        header = parsed["header"]
        if header["run_id"] != RUN_ID:
            raise RuntimeError("native stream did not preserve the fixed --run-id")
        if not header["engine"] or header["engine"] == "test":
            raise RuntimeError("native stream is missing a real engine version string")

        # Normalize only the recorded argv. The subprocess receives real paths.
        command = run_json_command("STRATEGY.so", "BARS.csv", "SINK")
        command[:3] = ["python3", "-B", "docker/run_json.py"]
        provenance = {
            "captured_native": True,
            "origin": "captured from the real C++ writer",
            "strategy_sha256": strategy_sha256,
            "bars_sha256": bars_sha256,
            "run_json_command": command,
            # The writer puts PINEFORGE_VERSION_STRING in header.engine
            # (src/report_sink_writer.cpp:230).
            "engine_version_string": header["engine"],
            "header": {"run_id": header["run_id"], "engine": header["engine"]},
            "run_id_deterministic": True,
            "run_id_note": "Fixed by --run-id; readers must treat run_id as opaque.",
            # Tests freeze time.time in their in-process harness
            # (tests/test_run_json_sink_fd.py:1267). run_json.py has no clock
            # option; its elapsed_seconds is stdout metadata, not sink bytes.
            "clock": "Not overridden; run_json.py has no fixed-clock CLI option.",
            "record_kinds": [tag for tag in RECORD_KINDS
                             if parsed["counts"].get(tag, 0) > 0],
            "missing_record_kinds": [tag for tag in RECORD_KINDS
                                     if parsed["counts"].get(tag, 0) == 0],
        }
        # Preserve the C++ writer's exact bytes, including its header and LF.
        # Never serialize parsed["header"], records or trailer back to the sink.
        shutil.copyfile(sink, directory / "native.pfsink")
        (directory / "native.expected.parse.json").write_bytes(canonical_json(parsed))
        (directory / "native.provenance.json").write_bytes(canonical_json(provenance))
    return provenance


def main(argv=None):
    parser = Parser(prog="capture_native_sink_vector.py", description=__doc__,
                    allow_abbrev=False)
    parser.add_argument("--so", type=Path, required=True)
    parser.add_argument("--bars", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        library, bars = args.so.resolve(), args.bars.resolve()
        if not library.is_file():
            raise ValueError("--so must name a built strategy library")
        if not bars.is_file():
            raise ValueError("--bars must name a small bar CSV")
        directory = output_directory(args.out)
        provenance = capture(directory, library, bars)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    sys.stdout.buffer.write(canonical_json(provenance))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
