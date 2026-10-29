#!/usr/bin/env python3
"""MARKET C4: compare OFF default JSON and OFF primary sink as raw bytes.

DELTA.md:205-212. The caller builds BOTH revisions with the same test strategy
source and toolchain. This script neither builds nor discovers a baseline.
Only the producer's wall-clock input is fixed, BEFORE serialization. No byte,
key, number, whitespace, line ending or trailer is normalized after emission.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types


def worker(runner_path, library, chart, primary):
    sys.path.insert(0, str(runner_path.parent))
    spec = importlib.util.spec_from_file_location("replay_c4_producer", runner_path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.time = types.SimpleNamespace(time=lambda: 1.0)
    argv = ["--so", str(library), "--ohlcv", str(chart), "--input-tf", "1",
            "--script-tf", "1", "--inputs", '{"shape":"fifo"}',
            "--run-id", "replay-c4-off", "--profile", "unjailed"]
    if primary is not None:
        argv += ["--report-sink", str(primary)]
    # Neither binary is asked to recognize a switch absent in the old build.
    # OFF is the existing default. This is also checked in the resulting wire.
    return runner.main(argv)


def first_difference(before, after):
    for offset, (left, right) in enumerate(zip(before, after)):
        if left != right:
            return offset
    return None if len(before) == len(after) else min(len(before), len(after))


def compare(label, before, after):
    offset = first_difference(before, after)
    if offset is None:
        print(f"PASS C4 {label}: {len(before)} bytes identical")
        return True
    left = "EOF" if offset == len(before) else f"0x{before[offset]:02x}"
    right = "EOF" if offset == len(after) else f"0x{after[offset]:02x}"
    print(f"FAIL C4 {label}: first differing offset {offset}; "
          f"baseline={left}, candidate={right}; lengths={len(before)}/{len(after)}",
          file=sys.stderr)
    return False


def run_pair(args, directory):
    chart, primary = directory / "chart.csv", directory / "primary.ndjson"
    chart.write_text("timestamp,open,high,low,close,volume\n" + "".join(
        f"{1736121600000 + index * 60000},128,{136 if index == 3 else 128},128,128,1\n"
        for index in range(8)), encoding="utf-8")
    environment = os.environ.copy()
    environment["PINEFORGE_RUN_JSON_PROFILE"] = "unjailed"
    for key in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES"):
        environment.pop(key, None)
    default_json, primary_bytes = [], []
    for label, runner, library in (
            ("baseline", args.baseline_run_json, args.baseline_library),
            ("candidate", args.candidate_run_json, args.candidate_library)):
        for mode in ("default", "sink"):
            if primary.exists():
                primary.unlink()
            command = [sys.executable, "-B", str(Path(__file__).resolve()),
                       "--worker", "--runner", str(runner), "--library", str(library),
                       "--chart", str(chart)]
            if mode == "sink":
                command += ["--primary", str(primary)]
            process = subprocess.run(command, cwd=directory, env=environment,
                                     capture_output=True, check=False)
            (directory / f"{label}.{mode}.stdout").write_bytes(process.stdout)
            (directory / f"{label}.{mode}.stderr").write_bytes(process.stderr)
            if process.returncode != 0:
                raise RuntimeError(f"{label} {mode} exited {process.returncode}: "
                                   f"{process.stdout!r} {process.stderr!r}")
            document = json.loads(process.stdout)
            if "error" in document or "replay" in document:
                raise RuntimeError(f"{label} {mode} did not publish an ordinary OFF report")
            if mode == "default":
                assert len(document["trades"]) == 3, "fixed FIFO scenario did not recut"
                for row in document["trades"]:
                    assert "recut" not in row and "settlement_index" not in row
                default_json.append(process.stdout)
            else:
                data = primary.read_bytes()
                assert data.endswith(b"\n") and b'"pf-report-sink"' in data
                assert b'"pf-report-replay"' not in data
                primary_bytes.append(data)
                (directory / f"{label}.primary.ndjson").write_bytes(data)
    # Evaluate both comparisons, even when the first one fails.
    primary_ok = compare("primary sink", *primary_bytes)
    json_ok = compare("default JSON", *default_json)
    return 0 if primary_ok and json_ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--runner", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--library", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--chart", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--primary", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--baseline-run-json", type=Path)
    parser.add_argument("--candidate-run-json", type=Path)
    parser.add_argument("--baseline-library", type=Path)
    parser.add_argument("--candidate-library", type=Path)
    parser.add_argument("--artifacts", type=Path,
                        help="Keep raw stdout, stderr and primary artifacts here")
    args = parser.parse_args()
    if args.worker:
        for key in ("runner", "library", "chart"):
            value = getattr(args, key)
            if value is None or not value.is_file():
                parser.error(f"--{key} must name a file")
        return worker(args.runner.resolve(), args.library.resolve(),
                      args.chart.resolve(), args.primary)
    # CTest passes the two build locations through the environment (a test
    # registration may not carry an unresolved path argument).
    baseline_build = os.environ.get("PINEFORGE_REPLAY_BASELINE_BUILD")
    if baseline_build and args.baseline_run_json is None:
        build = Path(baseline_build)
        args.baseline_run_json = build / "replay-rq" / "docker" / "run_json.py"
        args.baseline_library = build / "replay-rq" / "replay_rq_strategy.so"
        args.candidate_run_json = Path(__file__).resolve().parent.parent / "docker" / "run_json.py"
        args.candidate_library = Path(os.environ.get("PINEFORGE_REPLAY_CANDIDATE_LIBRARY", ""))
    for key in ("baseline_run_json", "candidate_run_json", "baseline_library", "candidate_library"):
        value = getattr(args, key)
        if value is None or not value.is_file():
            parser.error("--" + key.replace("_", "-") + " must name a built input file")
        setattr(args, key, value.resolve())
    if args.baseline_library == args.candidate_library:
        parser.error("baseline and candidate libraries must be separate build artifacts")
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        return run_pair(args, args.artifacts.resolve())
    with tempfile.TemporaryDirectory(prefix="pineforge-replay-c4-") as directory:
        return run_pair(args, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())
