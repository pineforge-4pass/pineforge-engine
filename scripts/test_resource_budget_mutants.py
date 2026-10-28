#!/usr/bin/env python3
"""Build isolated resource-budget variants and require their witnesses to fail.

Run on the verification host. This never edits the supplied source tree or
its index; the work directory must be new and is retained with all logs.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Match the inner alarms plus the one-second diagnostic margin used by CTest.
SINK_TIMEOUT = (2 * 30 * 1000 + 999) // 1000 + 1 + 1
BUDGET_TIMEOUT = (100 * 2 * 100 + 999) // 1000 + 1 + 1
VARIANTS = {
    "sink_poll_total_time": (
        ("test_report_sink_stall", "slow", SINK_TIMEOUT,
         "report sink reader stalled"),
        ("test_report_sink_stall", "socket_short_send", SINK_TIMEOUT,
         "accepted socket bytes must reset the deadline at the first wake"),
    ),
    "run_budget_end_only": (
        ("test_host_run_budget", "wall", BUDGET_TIMEOUT,
         "budget must stop before the next input callback, not at teardown"),
        ("test_host_run_budget", "cpu", BUDGET_TIMEOUT,
         "budget must stop before the next input callback, not at teardown"),
        ("test_host_run_budget", "real_cpu", BUDGET_TIMEOUT,
         "real CPU spin must stop at the next bar boundary"),
        ("test_host_run_budget", "sink", BUDGET_TIMEOUT,
         "budget must stop at an input boundary and abandon the sink trailer"),
    ),
}


def command(argv: list[str], cwd: Path, log: Path, timeout: int | None = None):
    with log.open("w", encoding="utf-8") as output:
        output.write("command: " + repr(argv) + "\n")
        output.flush()
        try:
            return subprocess.run(argv, cwd=cwd, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=timeout,
                                  check=False)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"timeout is a harness failure: {log}") from error


def checked(argv: list[str], cwd: Path, log: Path) -> None:
    result = command(argv, cwd, log)
    if result.returncode != 0:
        raise RuntimeError(f"setup/build failed ({result.returncode}): {log}")


def snapshot(source: Path, target: Path, log: Path) -> None:
    # Include source edits and new, non-ignored files before a supervisor commit.
    # Submodule directories are unnecessary: corpus strategies are disabled.
    result = subprocess.run(
        ["git", "-C", str(source), "ls-files", "--cached", "--others",
         "--exclude-standard", "-z"], capture_output=True, check=False)
    log.write_bytes(result.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"cannot inventory candidate sources: {log}")
    target.mkdir()
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        relative = Path(raw.decode("utf-8"))
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(f"invalid source path: {relative}")
        original = source / relative
        if original.is_dir():
            continue
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination, follow_symlinks=False)


def row(build: Path, case: tuple, phase: str, logs: Path) -> dict:
    executable, argument, timeout, witness = case
    log = logs / f"{phase}-{executable}-{argument}.log"
    result = command([str(build / "bin" / executable), argument], build, log, timeout)
    text = log.read_text(encoding="utf-8")
    expected = 0 if phase == "candidate" else 1
    if result.returncode != expected:
        raise RuntimeError(
            f"{phase} {executable}/{argument}: expected exit {expected}, "
            f"got {result.returncode}; {log}")
    if phase != "candidate" and witness not in text.splitlines():
        raise RuntimeError(f"missing required assertion witness: {log}")
    return {"phase": phase, "executable": executable, "row": argument,
            "exit": result.returncode, "log": str(log),
            "witness": witness if phase != "candidate" else None}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cmake-arg", action="append", default=[])
    args = parser.parse_args()
    source = args.source_root.resolve()
    work = args.work_dir.resolve()
    if not sys.platform.startswith("linux"):
        parser.error("the socket allocation witness requires Linux")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if work == source or source in work.parents:
        parser.error("--work-dir must be outside --source-root")
    if work.exists():
        parser.error("--work-dir must be new; no existing files are removed")
    work.mkdir(parents=True)
    logs = work / "logs"
    logs.mkdir()
    receipts = []
    try:
        candidate = work / "source"
        build = work / "build"
        snapshot(source, candidate, logs / "snapshot.log")
        checked(["cmake", "-S", str(candidate), "-B", str(build),
                 *args.cmake_arg,
                 "-DCMAKE_BUILD_TYPE=Release", "-DPINEFORGE_BUILD_TESTS=ON",
                 "-DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF"],
                work, logs / "configure.log")
        build_command = ["cmake", "--build", str(build), "--parallel", str(args.jobs)]
        # Rebuild ALL targets, including statically linked test executables.
        checked(build_command, work, logs / "candidate-build.log")
        for cases in VARIANTS.values():
            for case in cases:
                receipts.append(row(build, case, "candidate", logs))
        for variant, cases in VARIANTS.items():
            patch = candidate / "tests" / "mutants" / f"{variant}.patch"
            checked(["git", "apply", "--check", str(patch)], candidate,
                    logs / f"{variant}-patch-check.log")
            checked(["git", "apply", str(patch)], candidate,
                    logs / f"{variant}-patch.log")
            checked(build_command, work, logs / f"{variant}-build.log")
            for case in cases:
                receipts.append(row(build, case, variant, logs))
            checked(["git", "apply", "--reverse", str(patch)], candidate,
                    logs / f"{variant}-restore.log")
        (work / "summary.json").write_text(
            json.dumps(receipts, indent=2) + "\n", encoding="utf-8")
    except (OSError, RuntimeError) as error:
        print(f"resource budget mutants: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"resource budget mutants: candidate green; all required variants killed; {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
