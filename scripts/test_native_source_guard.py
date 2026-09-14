#!/usr/bin/env python3
"""Fail closed if a native/generic include root reaches a source/Pine header.

This is the inexpensive textual companion to check_native_include_independence:
the latter proves the installed closure and object symbols after a build; this
guard runs in preflight and rejects a forbidden include before compilation.
"""
from __future__ import annotations

from contextlib import redirect_stderr
import io
from pathlib import Path
import re
import sys
import tempfile


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
IDENTIFIER_ROOTS = (
    "include/pineforge/engine.hpp",
    "include/pineforge/native_host.hpp",
    "src/native_execution_consumer.cpp",
    "src/native_order.cpp",
    "src/engine_execution.cpp",
    "src/c_abi.cpp",
    "src/engine_state_hash.cpp",
)
FORBIDDEN_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"][^>"]*(?:pineforge/source/|compat/pine/)'
)
FORBIDDEN_C_ABI = re.compile(
    r'(?:pineforge::source|\bsource::|compat::pine|pineforge/source/|compat/pine/)'
)
FORBIDDEN_IDENTIFIER = re.compile(
    r'(?:compat::pine|\bpine_[A-Za-z0-9_]*\b|\b_src_[A-Za-z0-9_]*\b|'
    r'\bcoof_[A-Za-z0-9_]*\b|\bis_first_tick_\b|'
    r'\bpos_view_freeze[A-Za-z0-9_]*\b|\bprocess_pending_orders\b|'
    r'\btv_money[A-Za-z0-9_]*\b|\bmarket_admission_journal_\b|'
    r'\bpending_orders_\b)'
)
FROZEN_C_EXPORTS = (
    "strategy_pending_orders_len",
    "strategy_pending_orders_get",
)


def native_roots() -> list[Path]:
    roots = [ROOT / relative for relative in HEADER_ROOTS + SOURCE_ROOTS]
    roots.extend(sorted((ROOT / "src").glob("native_*.cpp")))
    roots.extend(sorted((ROOT / "src").glob("native_*.hpp")))
    return roots


def identifier_roots() -> list[Path]:
    return [ROOT / relative for relative in IDENTIFIER_ROOTS]


def excluded_identifier_line(line: str) -> bool:
    stripped = line.lstrip()
    return (stripped.startswith(("//", "/*", "*"))
            or any(export in line for export in FROZEN_C_EXPORTS))


def run_scan(pattern: re.Pattern[str], paths: list[Path], label: str,
             *, exclude_line=None) -> int:
    matches: list[str] = []
    for path in paths:
        try:
            with path.open(encoding="utf-8", errors="replace") as source:
                for line_number, line in enumerate(source, 1):
                    text = line.rstrip("\n")
                    if exclude_line is not None and exclude_line(text):
                        continue
                    if pattern.search(text):
                        matches.append(f"{path}:{line_number}:{text}\n")
        except OSError as error:
            print("native source guard: scan failed while checking " + label,
                  file=sys.stderr)
            print(f"{path}: {error}", file=sys.stderr)
            return 1
    if matches:
        print("native source guard: forbidden " + label + " found", file=sys.stderr)
        sys.stderr.writelines(matches)
        return 1
    return 0


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="pf-native-source-guard-") as temporary:
        fixture = Path(temporary) / "include/pineforge/engine.hpp"
        fixture.parent.mkdir(parents=True)
        fixture.write_text("int pending_orders_;\n", encoding="utf-8")
        diagnostic = io.StringIO()
        with redirect_stderr(diagnostic):
            result = run_scan(FORBIDDEN_IDENTIFIER, [fixture], "source/Pine identifier",
                              exclude_line=excluded_identifier_line)
    output = diagnostic.getvalue()
    if (result != 1
            or "native source guard: forbidden source/Pine identifier found" not in output
            or f"{fixture}:1:int pending_orders_;" not in output):
        print("native source guard: self-test failed", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    if self_test():
        return 1
    roots = native_roots()
    if not all(path.is_file() for path in roots):
        missing = [str(path) for path in roots if not path.is_file()]
        print("native source guard: missing roots: " + ", ".join(missing), file=sys.stderr)
        return 1
    if run_scan(FORBIDDEN_INCLUDE, roots, "source/Pine include"):
        return 1
    if run_scan(FORBIDDEN_C_ABI, [ROOT / "src/c_abi.cpp"], "C ABI source name"):
        return 1
    roots = identifier_roots()
    if not all(path.is_file() for path in roots):
        missing = [str(path) for path in roots if not path.is_file()]
        print("native source guard: missing roots: " + ", ".join(missing), file=sys.stderr)
        return 1
    return run_scan(FORBIDDEN_IDENTIFIER, roots, "source/Pine identifier",
                    exclude_line=excluded_identifier_line)


if __name__ == "__main__":
    raise SystemExit(main())
