#!/usr/bin/env python3
"""CI guardrail: runtime-side PF_API implementations in c_abi.cpp stay documented.

The header lists the harness-facing symbols; only those in EXPECTED_RUNTIME are
defined in the static runtime — the rest are emitted per-strategy by the
transpiler (see comment in src/c_abi.cpp). If that split changes, update
EXPECTED_RUNTIME below and the comment block in c_abi.cpp together.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EXPECTED_RUNTIME = frozenset({
    "strategy_set_trace_enabled",
    "strategy_set_trade_start_time",
    "strategy_set_chart_timezone",
    "strategy_set_syminfo_timezone",
    "strategy_set_syminfo_session",
    "strategy_set_syminfo_mintick",
    "strategy_set_syminfo_pointvalue",
    "strategy_set_syminfo_metadata",
    "strategy_get_last_error",
    "strategy_stream_begin",
    "strategy_stream_push_tick",
    "strategy_stream_push_ticks",
    "strategy_stream_advance_time",
    "strategy_stream_end",
    "strategy_stream_fill_report",
    "pf_version_get",
    "pf_version_string",
    "pf_abi_version",
})

_PF_API_DECL = re.compile(r"^\s*PF_API\b.+\b(\w+)\s*\(")


def _pf_api_names(path: Path) -> list[str]:
    out: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _PF_API_DECL.match(line)
        if m:
            out.append(m.group(1))
    return out


def main() -> int:
    header = ROOT / "include" / "pineforge" / "pineforge.h"
    c_abi = ROOT / "src" / "c_abi.cpp"
    if not header.is_file() or not c_abi.is_file():
        print("check_c_abi_runtime: missing pineforge.h or c_abi.cpp", file=sys.stderr)
        return 2

    header_funcs = _pf_api_names(header)
    c_abi_funcs = _pf_api_names(c_abi)

    hdr_set = set(header_funcs)
    abi_set = set(c_abi_funcs)

    if len(header_funcs) != len(hdr_set):
        print("check_c_abi_runtime: duplicate PF_API lines in pineforge.h", file=sys.stderr)
        return 1

    if abi_set != EXPECTED_RUNTIME:
        print(
            f"check_c_abi_runtime: c_abi.cpp PF_API set {sorted(abi_set)} "
            f"!= expected {sorted(EXPECTED_RUNTIME)}",
            file=sys.stderr,
        )
        return 1

    if not abi_set <= hdr_set:
        print(
            "check_c_abi_runtime: c_abi.cpp implements PF_API symbols "
            f"not declared in pineforge.h: {sorted(abi_set - hdr_set)}",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
