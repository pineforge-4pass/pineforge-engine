#!/usr/bin/env python3
"""Fail closed if a native/generic include root reaches a source/Pine header.

This is the inexpensive textual companion to check_native_include_independence:
the latter proves the installed closure and object symbols after a build; this
guard runs in preflight and rejects a forbidden include before compilation.
"""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
HEADER_ROOTS = (
    "include/pineforge/engine.hpp",
    "include/pineforge/native_host.hpp",
    "include/pineforge/native_order.hpp",
    "include/pineforge/native_run_spec.hpp",
    "include/pineforge/native_calendar.hpp",
    "include/pineforge/execution.hpp",
    "include/pineforge/market_driver.hpp",
)
SOURCE_ROOTS = (
    "src/engine_execution.cpp",
    "src/engine_state_hash.cpp",
    "src/c_abi.cpp",
    "src/pending_order_mirror.cpp",
)
FORBIDDEN_INCLUDE = r'^\s*#\s*include\s*[<"][^>"]*(?:pineforge/source/|compat/pine/)'
FORBIDDEN_C_ABI = r'(?:pineforge::source|\bsource::|compat::pine|pineforge/source/|compat/pine/)'


def native_roots() -> list[Path]:
    roots = [ROOT / relative for relative in HEADER_ROOTS + SOURCE_ROOTS]
    roots.extend(sorted((ROOT / "src").glob("native_*.cpp")))
    roots.extend(sorted((ROOT / "src").glob("native_*.hpp")))
    return roots


def run_rg(pattern: str, paths: list[Path], label: str) -> int:
    result = subprocess.run(["rg", "-n", pattern, *map(str, paths)],
                            text=True, capture_output=True)
    if result.returncode == 1:
        return 0
    if result.returncode == 0:
        print("native source guard: forbidden " + label + " found", file=sys.stderr)
        print(result.stdout, end="", file=sys.stderr)
        return 1
    print("native source guard: rg failed while checking " + label, file=sys.stderr)
    print(result.stderr, end="", file=sys.stderr)
    return 1


def main() -> int:
    roots = native_roots()
    if not all(path.is_file() for path in roots):
        missing = [str(path) for path in roots if not path.is_file()]
        print("native source guard: missing roots: " + ", ".join(missing), file=sys.stderr)
        return 1
    if run_rg(FORBIDDEN_INCLUDE, roots, "source/Pine include"):
        return 1
    return run_rg(FORBIDDEN_C_ABI, [ROOT / "src/c_abi.cpp"], "C ABI source name")


if __name__ == "__main__":
    raise SystemExit(main())
