#!/usr/bin/env python3
"""Freeze trade-history artifacts using only a clean, pinned base engine.

Run from a clean checkout containing the committed script and test sources:

    python3 -B scripts/regenerate_trade_history_reference.py \
        --base-tree /lab/engine-base --jobs 4

The base tree must be a separate, clean worktree at the full commit recorded
in tests/data/trade_history_reference/BASE_COMMIT. --base-commit can assert
that pin explicitly; it cannot override it. A fresh temporary CMake harness
copies in only the new fixture and legacy test source. Its sole engine
dependency is built from --base-tree, never from this checkout or an existing
library. Use the same architecture and compiler as the identity-test cell.

Artifacts are generated into an empty staging directory, validated, measured
(including both manifests and BASE_COMMIT), then published to the data
directory. More than 5,000,000 bytes refuses publication. The script does not
write the Git index, commit, or push.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "tests/data/trade_history_reference"
PIN = REFERENCE / "BASE_COMMIT"
IDENTITY = ROOT / "tests/test_report_sink_trade_identity.py"
COPIED_SOURCES = (
    "tests/report_sink_trade_retirement_fixture.hpp",
    "tests/test_report_sink_trade_legacy.cpp",
)
COMMITTED_INPUTS = (
    "scripts/regenerate_trade_history_reference.py",
    "tests/test_report_sink_trade_identity.py",
    "tests/data/trade_history_reference/BASE_COMMIT",
    *COPIED_SOURCES,
)
MAX_BYTES = 5_000_000
ARTIFACTS = {
    f"{shape}.{suffix}"
    for shape in ("reversal", "shapes", "wide", "indexed")
    for suffix in ("sink", "hashes", "report")
}
REFERENCE_FILES = ARTIFACTS | {"manifest.json", "BASE_COMMIT", "MANIFEST.sha256"}

HARNESS = """\
cmake_minimum_required(VERSION 3.16)
project(TradeHistoryReference LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
set(PINEFORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_SOURCE_LAYER ON CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_TUTORIAL OFF CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_CORPUS_STRATEGIES OFF CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_LIVE_RUNNER OFF CACHE BOOL "" FORCE)
set(PINEFORGE_BUILD_BENCH_STRATEGIES OFF CACHE BOOL "" FORCE)
add_subdirectory("${TRADE_HISTORY_BASE_TREE}" engine EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
add_executable(test_report_sink_trade_legacy test_report_sink_trade_legacy.cpp)
target_link_libraries(test_report_sink_trade_legacy
    PRIVATE pineforge Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(test_report_sink_trade_legacy PRIVATE -ffp-contract=off -UNDEBUG)
"""


def run(arguments: list[str]) -> None:
    print("+ " + shlex.join(arguments), flush=True)
    subprocess.run(arguments, check=True)


def git(tree: Path, *arguments: str) -> str:
    return subprocess.check_output(
        ["git", "--no-optional-locks", "-C", str(tree), *arguments],
        text=True,
    ).strip()


def clean_head(tree: Path) -> str:
    if Path(git(tree, "rev-parse", "--show-toplevel")).resolve() != tree:
        raise ValueError(f"not a worktree root: {tree}")
    if git(tree, "status", "--porcelain=v1", "--untracked-files=all",
           "--ignore-submodules=none"):
        raise ValueError(f"refusing dirty worktree: {tree}")
    return git(tree, "rev-parse", "HEAD")


def require_heads(base_tree: Path, base_commit: str, source_commit: str) -> None:
    if clean_head(base_tree) != base_commit:
        raise ValueError("base-tree HEAD does not equal BASE_COMMIT")
    if clean_head(ROOT) != source_commit:
        raise ValueError("script/test checkout changed during regeneration")


def publish(directory: Path) -> None:
    # Copy and validate on the destination filesystem before swapping names.
    with tempfile.TemporaryDirectory(
            prefix=".trade-history-publish-", dir=REFERENCE.parent) as name:
        root = Path(name)
        candidate = root / "reference"
        shutil.copytree(directory, candidate)
        run([sys.executable, "-B", str(IDENTITY), "--verify-reference", str(candidate)])
        previous = root / "previous"
        REFERENCE.rename(previous)
        try:
            candidate.rename(REFERENCE)
        except OSError:
            previous.rename(REFERENCE)
            raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-tree", required=True, type=Path,
                        help="separate clean worktree of the pinned base engine")
    parser.add_argument("--base-commit",
                        help="full base commit; must match BASE_COMMIT (the default)")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be positive")

    source_commit = clean_head(ROOT)
    for relative in COMMITTED_INPUTS:
        git(ROOT, "cat-file", "-e", f"HEAD:{relative}")
    pin = PIN.read_bytes()
    if not re.fullmatch(rb"[0-9a-f]{40}\n", pin):
        raise ValueError("BASE_COMMIT must contain one full lowercase commit and LF")
    base_commit = pin.decode("ascii").strip()
    if args.base_commit is not None and args.base_commit != base_commit:
        raise ValueError("--base-commit does not match BASE_COMMIT")
    base_tree = args.base_tree.resolve()
    if base_tree == ROOT:
        raise ValueError("--base-tree must be separate from the script/test checkout")
    require_heads(base_tree, base_commit, source_commit)
    if REFERENCE.is_symlink() or any(
            path.name not in REFERENCE_FILES or path.is_symlink() or not path.is_file()
            for path in REFERENCE.iterdir()):
        raise ValueError("reference directory contains unexpected files or symlinks")

    print(f"base engine HEAD: {base_commit}\ncommitted test/script HEAD: {source_commit}",
          flush=True)
    with tempfile.TemporaryDirectory(prefix="pineforge-trade-reference-") as name:
        root = Path(name)
        harness = root / "harness"
        harness.mkdir()
        for relative in COPIED_SOURCES:
            shutil.copyfile(ROOT / relative, harness / Path(relative).name)
        (harness / "CMakeLists.txt").write_text(HARNESS, encoding="utf-8")
        build = root / "build"
        run(["cmake", "-S", str(harness), "-B", str(build),
             "-DCMAKE_BUILD_TYPE=Release", "-DPINEFORGE_VERSION_SOURCE=FILE",
             f"-DTRADE_HISTORY_BASE_TREE:PATH={base_tree}"])
        run(["cmake", "--build", str(build), "--config", "Release", "--target",
             "test_report_sink_trade_legacy", "--parallel", str(args.jobs)])
        require_heads(base_tree, base_commit, source_commit)
        legacy = build / "bin/test_report_sink_trade_legacy"
        if not legacy.is_file():
            raise ValueError("fresh base build did not produce the legacy executable")
        print(f"legacy executable SHA-256: {hashlib.sha256(legacy.read_bytes()).hexdigest()}",
              flush=True)
        directory = root / "trade_history_reference"
        run([sys.executable, "-B", str(IDENTITY), "--write-reference",
             str(legacy), str(directory)])
        (directory / "BASE_COMMIT").write_bytes(pin)
        paths = sorted(directory.iterdir())
        if {path.name for path in paths} != REFERENCE_FILES - {"MANIFEST.sha256"}:
            raise ValueError("legacy producer did not write the exact reference inventory")
        manifest = "".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
            for path in paths
        )
        (directory / "MANIFEST.sha256").write_text(manifest, encoding="ascii")
        total = sum(path.stat().st_size for path in directory.iterdir())
        manifest_bytes = (directory / "MANIFEST.sha256").stat().st_size
        print(f"reference total: {total} bytes (includes all 15 files)\n"
              f"MANIFEST.sha256: {manifest_bytes} bytes", flush=True)
        if total > MAX_BYTES:
            raise ValueError(
                f"reference exceeds {MAX_BYTES} bytes; refusing repository publication; "
                "the supervisor must use lab-only registration")
        require_heads(base_tree, base_commit, source_commit)
        publish(directory)
    print(f"published frozen reference: {REFERENCE}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"trade-history reference regeneration refused: {error}") from error
