#!/usr/bin/env python3
"""Compile/run the same replay at ab9714be and HEAD; enforce A30's 1.5x bound."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from cpp_abi_pairing import PairingError, enforce_receipt_mode, load_frozen_v16


LIMIT = 1.5
TIMING = re.compile(r"^PF_RUNTIME_SECONDS=(\d+(?:\.\d+)?)$", re.M)


def enforce_ratio(candidate: float, baseline: float, limit: float = LIMIT) -> float:
    if not candidate > 0.0 or not baseline > 0.0:
        raise ValueError("runtime samples must be positive")
    ratio = candidate / baseline
    if ratio > limit:
        raise ValueError(
            f"candidate runtime {candidate:.6f}s is {ratio:.3f}x ab9714be "
            f"{baseline:.6f}s (limit {limit:.3f}x)")
    return ratio


def run_sample(executable: Path) -> float:
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=120)
    diagnostic = result.stdout + result.stderr
    if result.returncode:
        raise RuntimeError(f"runtime witness {executable} exited {result.returncode}:\n{diagnostic}")
    match = TIMING.search(diagnostic)
    if not match:
        raise RuntimeError("runtime witness emitted no timing marker:\n" + diagnostic)
    return float(match.group(1))


def include_flags(compile_commands: Path, source: Path) -> list[str]:
    rows = json.loads(compile_commands.read_text())
    row = next((item for item in rows
                if Path(item.get("file", "")).resolve() == source.resolve()), None)
    if row is None:
        raise RuntimeError("compile_commands has no runtime-budget source")
    argv = row.get("arguments") or shlex.split(row["command"])
    result: list[str] = []
    index = 0
    while index < len(argv):
        value = argv[index]
        if value in {"-I", "-isystem", "-iframework", "-F", "-arch"} and index + 1 < len(argv):
            result.extend((value, argv[index + 1])); index += 2; continue
        if value.startswith(("-I", "-isystem", "-D", "-arch=")):
            result.append(value)
        index += 1
    return result


def compile_baseline(args: argparse.Namespace, root: Path) -> Path:
    frozen_root = root / "frozen-v16"
    frozen = load_frozen_v16(args.v16_frozen_receipt, frozen_root)
    output = root / "runtime-budget-ab9714be"
    flags = [flag for flag in include_flags(args.compile_commands, args.source)
             if not flag.startswith("-DPINEFORGE_L4G_TUTORIAL_CSV=")]
    flags.append('-DPINEFORGE_L4G_TUTORIAL_CSV="' + str(args.csv) + '"')
    command = [
        args.compiler, "-std=c++17", "-O2", "-DNDEBUG", "-ffp-contract=off",
        "-I", str(frozen_root / "include"), "-I", str(args.generated_include),
        *flags, str(args.source), str(frozen.archive), "-pthread", "-o", str(output),
    ]
    result = subprocess.run(command, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError("ab9714be runtime witness did not compile:\n"
                           + result.stdout + result.stderr)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--generated-include", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--v16-frozen-receipt", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--skip-if-receipt-missing", action="store_true")
    mode.add_argument("--require-receipts", action="store_true")
    parser.add_argument("--candidate-only", action="store_true")
    args = parser.parse_args()
    try:
        receipt = enforce_receipt_mode(
            [args.v16_frozen_receipt], skip=args.skip_if_receipt_missing,
            require=args.require_receipts, label="runtime budget")
        if receipt is not None: return receipt
        candidate = run_sample(args.candidate)
        if args.candidate_only:
            print(f"runtime budget: candidate correctness sample {candidate:.6f}s; "
                  "relative gate is Release-only")
            return 0
        with tempfile.TemporaryDirectory(prefix="pineforge-runtime-base-") as directory:
            baseline_binary = compile_baseline(args, Path(directory))
            # Run baseline second so both binaries observe the same warm host;
            # repeat each and take the minimum to discount scheduler noise.
            baseline = min(run_sample(baseline_binary), run_sample(baseline_binary))
            candidate = min(candidate, run_sample(args.candidate))
        ratio = enforce_ratio(candidate, baseline)
        print(f"runtime budget: candidate={candidate:.6f}s ab9714be={baseline:.6f}s "
              f"ratio={ratio:.3f}x limit={LIMIT:.3f}x")
        return 0
    except (OSError, PairingError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("runtime budget: " + str(error))


if __name__ == "__main__":
    raise SystemExit(main())
