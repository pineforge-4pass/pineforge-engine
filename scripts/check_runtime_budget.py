#!/usr/bin/env python3
"""Compile/run the same replay at ab9714be and HEAD; enforce the A40 rev 2 relative bound."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from cpp_abi_pairing import PairingError, enforce_receipt_mode, load_frozen_v16


# A40 rev 5 (root, 2026-09-17): the slice-C ceiling is 15x of ab9714be on both
# profiles. Best-of-five measurements on an identical tree: 8.96x/9.07x on
# Apple Silicon (local), 10.24x on the hosted ubuntu-24.04 runner, 12.84x on
# the hosted macos-26 runner (0.602 s vs 0.047 s); the ceiling gates every
# host class we run with ~15 % headroom and still catches a 2x regression. L8f measured the generic kernel floor at ~5.7x (two live-leg
# matchings, request-core mutation plans and event history per bar, by design);
# the follow-up kernel lane lowers this constant toward that floor. The workload
# and the ab9714be side are frozen; only this constant may move, by root.
LIMIT = 15.0
SAMPLES = 5  # best-of-N per side (A40 rev 4)
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
            # Best-of-N, interleaved: shared CI runners (GitHub macOS/ubuntu)
            # scatter a single 0.05 s replay by 30 % or more, which moved the
            # ratio from 9x (local) to 10.2x and 13.3x on single shots. The
            # minimum of five alternating runs per side removes scheduler noise
            # without touching the workload or the ceiling.
            baseline = run_sample(baseline_binary)
            for _ in range(SAMPLES - 1):
                baseline = min(baseline, run_sample(baseline_binary))
                candidate = min(candidate, run_sample(args.candidate))
        ratio = enforce_ratio(candidate, baseline)
        print(f"runtime budget: candidate={candidate:.6f}s ab9714be={baseline:.6f}s "
              f"ratio={ratio:.3f}x limit={LIMIT:.3f}x")
        return 0
    except (OSError, PairingError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit("runtime budget: " + str(error))


if __name__ == "__main__":
    raise SystemExit(main())
