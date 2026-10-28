#!/usr/bin/env python3
"""Default-off byte pin for the report replay extension.

Runs `docker/run_json.py` WITHOUT any replay switch on three fixed scripts
(a re-cut group in FIFO mode, a re-cut group in ANY mode, and an ordinary close
with a report-only terminal row) and compares, as raw bytes, the default JSON
report and the `pf-report-sink/v1` stream with digests recorded from the
pre-extension engine (`docker/replay_off_golden.json`). Nothing is parsed,
masked or re-encoded before the comparison; the only input fixed before
serialization is the producer's wall clock and the run identity.

A scenario whose recorded digest is empty fails and prints the digests it
measured, so the file can be filled from a build of the revision it names.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types

SCENARIOS = ("fifo", "any", "ordinary")


def chart_text(shape):
    lines = ["timestamp,open,high,low,close,volume\n"]
    for index in range(8):
        opening = high = low = close = 128
        if shape in ("fifo", "any") and index == 3:
            high = 136
        if shape == "ordinary" and index == 2:
            opening = high = low = close = 136
        if shape == "ordinary" and index == 7:
            opening = high = low = close = 144
        lines.append(f"{1736121600000 + index * 60000},{opening},{high},{low},{close},1\n")
    return "".join(lines)


def worker(runner_path, library, shape, primary):
    sys.path.insert(0, str(runner_path.parent))
    spec = importlib.util.spec_from_file_location("replay_off_producer", runner_path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.time = types.SimpleNamespace(time=lambda: 1.0)
    # The chart path is part of the report text: always the relative name below.
    argv = ["--so", str(library), "--ohlcv", "chart.csv", "--input-tf", "1",
            "--script-tf", "1", "--inputs", json.dumps({"shape": shape}),
            "--run-id", "replay-off-golden", "--profile", "unjailed"]
    if primary is not None:
        argv += ["--report-sink", str(primary)]
    return runner.main(argv)


def mask_build_identity(text, document):
    """The report names the build that produced it (engine.version_string, a git
    describe, and engine.commit_sha, the short commit). Two builds of different
    commits can never share those bytes, so the two strings are replaced by a
    fixed token; every other byte of the report is compared as produced."""
    engine = document.get("engine")
    if not isinstance(engine, dict):
        raise RuntimeError("default report has no engine identity block")
    masked = text
    # The longer string first: the describe text may contain the short commit.
    for key in ("version_string", "commit_sha"):
        value = engine.get(key)
        if not isinstance(value, str):
            raise RuntimeError(f"engine.{key} is missing from the default report")
        if not value:
            continue
        needle = json.dumps(value).encode("utf-8")
        if masked.count(needle) < 1:
            raise RuntimeError(f"engine.{key} not found in the raw report bytes")
        masked = masked.replace(needle, b'"<build-identity>"')
    return masked


def measure(runner, library, shape, directory):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "chart.csv").write_text(chart_text(shape), encoding="utf-8")
    environment = os.environ.copy()
    environment["PINEFORGE_RUN_JSON_PROFILE"] = "unjailed"
    for key in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES"):
        environment.pop(key, None)
    results = {}
    for mode in ("default", "sink"):
        primary = directory / "primary.ndjson"
        if primary.exists():
            primary.unlink()
        command = [sys.executable, "-B", str(Path(__file__).resolve()), "--worker",
                   "--runner", str(runner), "--library", str(library), "--shape", shape]
        if mode == "sink":
            command += ["--primary", str(primary)]
        process = subprocess.run(command, cwd=directory, env=environment,
                                 capture_output=True, check=False)
        if process.returncode != 0:
            raise RuntimeError(f"{shape} {mode} exited {process.returncode}: "
                               f"{process.stdout!r} {process.stderr!r}")
        document = json.loads(process.stdout)
        if "error" in document or "replay" in document:
            raise RuntimeError(f"{shape} {mode} did not publish an ordinary OFF report")
        for row in document.get("trades", ()):
            assert "recut" not in row and "settlement_index" not in row
        if mode == "default":
            results["default_json"] = mask_build_identity(process.stdout, document)
        else:
            data = primary.read_bytes()
            assert data.endswith(b"\n") and b'"pf-report-sink"' in data
            assert b'"pf-report-replay"' not in data
            results["primary_sink"] = data
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--shape", choices=SCENARIOS, help=argparse.SUPPRESS)
    parser.add_argument("--primary", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--golden", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.worker:
        return worker(args.runner.resolve(), args.library.resolve(), args.shape, args.primary)
    # CTest passes the built strategy module through the environment.
    if args.runner is None:
        args.runner = root / "docker" / "run_json.py"
    if args.library is None:
        args.library = Path(os.environ.get("PINEFORGE_REPLAY_CANDIDATE_LIBRARY", ""))
    if args.golden is None:
        args.golden = root / "docker" / "replay_off_golden.json"
    for key in ("runner", "library", "golden"):
        value = getattr(args, key)
        if not value.is_file():
            parser.error(f"--{key} must name a file: {value}")
    golden = json.loads(args.golden.read_text(encoding="utf-8"))
    failed = False
    with tempfile.TemporaryDirectory(prefix="pineforge-replay-off-") as scratch:
        for shape in SCENARIOS:
            measured = measure(args.runner.resolve(), args.library.resolve(), shape,
                               Path(scratch) / shape)
            for label, data in measured.items():
                digest, size = hashlib.sha256(data).hexdigest(), len(data)
                want = golden["scenarios"][shape][label]
                if want["sha256"] == digest and want["bytes"] == size:
                    print(f"PASS off-bytes {shape} {label}: {size} bytes, sha256 {digest}")
                else:
                    failed = True
                    print(f"FAIL off-bytes {shape} {label}: measured {size} bytes, sha256 {digest}; "
                          f"recorded {want['bytes']} bytes, sha256 {want['sha256']!r}",
                          file=sys.stderr)
                    # The raw text of a mismatch, so it can be diffed against the record.
                    print(f"DUMP off-bytes {shape} {label}: "
                          f"{data.decode('utf-8', 'backslashreplace')!r}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
