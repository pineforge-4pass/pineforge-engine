#!/usr/bin/env python3
"""CI guardrail: runtime-side PF_API implementations stay documented.

Two inventories, pinned independently so neither can drift into the other:

  1. The compiled-strategy surface. include/pineforge/pineforge.h lists the
     harness-facing symbols; only those in EXPECTED_RUNTIME are defined in the
     static runtime (src/c_abi.cpp) — the rest are emitted per-strategy by the
     transpiler (see comment in src/c_abi.cpp). If that split changes, update
     EXPECTED_RUNTIME below and the comment block in c_abi.cpp together.

  2. The C-level native host API (R5 lane L13). Its declarations live in
     include/pineforge/native_c_api.h and its implementations in
     src/native_c_host.cpp, so they do not move the counts above; they are
     pinned by EXPECTED_NATIVE_C_API instead. Adding or removing a symbol
     there without updating that list fails every CI matrix job at the "C ABI
     runtime source check" step, exactly as it does for c_abi.cpp.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EXPECTED_RUNTIME = frozenset({
    "strategy_closed_trade_entry_incarnation",
    "strategy_set_trace_enabled",
    "strategy_set_trade_start_time",
    "strategy_set_chart_timezone",
    "strategy_set_syminfo_timezone",
    "strategy_set_syminfo_session",
    "strategy_set_syminfo_type",
    "strategy_set_syminfo_string",
    "strategy_set_syminfo_mintick",
    "strategy_set_syminfo_pointvalue",
    "strategy_set_syminfo_metadata",
    "strategy_set_account_currency_fx_series",
    "strategy_set_aux_security_feed",
    "strategy_set_native_security_feed",
    "strategy_get_last_error",
    "strategy_stream_begin",
    "strategy_stream_api_version",
    "strategy_stream_push_bar",
    "strategy_stream_order_actions_len",
    "strategy_stream_order_action_get",
    "strategy_stream_order_actions_clear",
    "strategy_stream_state_hash",
    "strategy_stream_push_tick",
    "strategy_stream_push_ticks",
    "strategy_stream_advance_time",
    "strategy_stream_end",
    "strategy_stream_fill_report",
    "strategy_request_abort",
    "strategy_last_run_status",
    "strategy_set_realtime_tail",
    "strategy_set_probe_suppress_tail_logic",
    "strategy_set_path_order",
    "strategy_last_bar_dual_entry_path",
    "strategy_set_broker_state_hash_recording",
    "strategy_broker_state_hash",
    "strategy_pending_orders_len",
    "strategy_pending_order_get",
    "strategy_pending_order_layout",
    "strategy_pending_order_fill_qty",
    "strategy_pending_order_level_resolved",
    "strategy_pending_order_effective_levels",
    "strategy_trail_best_price",
    "strategy_position_avg_price",
    "strategy_position_cycle_seq",
    "strategy_closed_trade_entry_id",
    "strategy_closed_trade_exit_id",
    "strategy_closed_trade_exit_comment",
    "strategy_closed_trade_close_cause",
    "strategy_position_size",
    "strategy_current_equity",
    "strategy_script_bars_processed",
    "pf_version_get",
    "pf_version_string",
    "pf_abi_version",
    "strategy_execution_contract",
    "strategy_configure_native_v1",
    "strategy_configure_native_fx_curve_v1",
})

EXPECTED_PUBLIC_DECLARATIONS = 65
EXPECTED_RUNTIME_IMPLEMENTATIONS = 57

# The C-level native host API. Additive to the 57 above: every symbol here is
# declared in include/pineforge/native_c_api.h and implemented in
# src/native_c_host.cpp, and neither file contributes to the two counts above.
EXPECTED_NATIVE_C_API = frozenset({
    "strategy_native_api_version",
    "strategy_native_host_create_v1",
    "strategy_native_host_free",
    "strategy_native_run_v1",
    "strategy_native_report_free_v1",
    "strategy_native_submit_v1",
    "strategy_native_replace_v1",
    "strategy_native_cancel_v1",
    "strategy_native_cancel_all_v1",
    "strategy_native_cancel_where_v1",
    "strategy_native_execute_current_v1",
    "strategy_native_position_v1",
    "strategy_native_working_len_v1",
    "strategy_native_working_get_v1",
    "strategy_native_open_lot_count_v1",
    "strategy_native_open_lot_get_v1",
    "strategy_native_events_v1",
    "strategy_native_state_v1",
    "strategy_native_cancel_where_v1",
    "strategy_native_declare_subscriptions_v1",
    "strategy_native_partial_bar_v1",
    "strategy_native_recalculations_v1",
    "strategy_native_trail_state_v1",
    "strategy_native_series_bar_v1",
    "strategy_native_marked_equity_v1",
    "strategy_native_liquidation_price_v1",
    "strategy_native_risk_state_v1",
    "strategy_native_continuation_hash_v1",
    "strategy_native_cohort_open_v1",
    "strategy_native_cohort_add_v1",
    "strategy_native_cohort_remove_v1",
    "strategy_configure_native_ext_v1",
    "strategy_native_append_auxiliary_bars_v1",
})

EXPECTED_NATIVE_C_API_DECLARATIONS = 32
EXPECTED_NATIVE_C_API_IMPLEMENTATIONS = 32

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

    if len(EXPECTED_RUNTIME) != EXPECTED_RUNTIME_IMPLEMENTATIONS:
        print(
            "check_c_abi_runtime: EXPECTED_RUNTIME count "
            f"{len(EXPECTED_RUNTIME)} != {EXPECTED_RUNTIME_IMPLEMENTATIONS}",
            file=sys.stderr,
        )
        return 1

    if len(header_funcs) != len(hdr_set):
        print("check_c_abi_runtime: duplicate PF_API lines in pineforge.h", file=sys.stderr)
        return 1

    if len(c_abi_funcs) != len(abi_set):
        print("check_c_abi_runtime: duplicate PF_API lines in c_abi.cpp", file=sys.stderr)
        return 1

    if len(header_funcs) != EXPECTED_PUBLIC_DECLARATIONS:
        print(
            "check_c_abi_runtime: pineforge.h PF_API declaration count "
            f"{len(header_funcs)} != {EXPECTED_PUBLIC_DECLARATIONS}",
            file=sys.stderr,
        )
        return 1

    if len(c_abi_funcs) != EXPECTED_RUNTIME_IMPLEMENTATIONS:
        print(
            "check_c_abi_runtime: c_abi.cpp runtime implementation count "
            f"{len(c_abi_funcs)} != {EXPECTED_RUNTIME_IMPLEMENTATIONS}",
            file=sys.stderr,
        )
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

    return _check_native_c_api(hdr_set, abi_set)


def _check_native_c_api(hdr_set: set[str], abi_set: set[str]) -> int:
    """Pin the L13 C-level native host API, the second runtime inventory."""
    header = ROOT / "include" / "pineforge" / "native_c_api.h"
    impl = ROOT / "src" / "native_c_host.cpp"
    if not header.is_file() or not impl.is_file():
        print("check_c_abi_runtime: missing native_c_api.h or native_c_host.cpp",
              file=sys.stderr)
        return 2

    if len(EXPECTED_NATIVE_C_API) != EXPECTED_NATIVE_C_API_DECLARATIONS:
        print(
            "check_c_abi_runtime: EXPECTED_NATIVE_C_API count "
            f"{len(EXPECTED_NATIVE_C_API)} != {EXPECTED_NATIVE_C_API_DECLARATIONS}",
            file=sys.stderr,
        )
        return 1

    if EXPECTED_NATIVE_C_API_DECLARATIONS != EXPECTED_NATIVE_C_API_IMPLEMENTATIONS:
        print(
            "check_c_abi_runtime: every native C API declaration must be "
            "implemented in the runtime",
            file=sys.stderr,
        )
        return 1

    header_funcs = _pf_api_names(header)
    impl_funcs = _pf_api_names(impl)
    header_names = set(header_funcs)
    impl_names = set(impl_funcs)

    if len(header_funcs) != len(header_names):
        print("check_c_abi_runtime: duplicate PF_API lines in native_c_api.h", file=sys.stderr)
        return 1

    if len(impl_funcs) != len(impl_names):
        print("check_c_abi_runtime: duplicate PF_API lines in native_c_host.cpp",
              file=sys.stderr)
        return 1

    if header_names != EXPECTED_NATIVE_C_API:
        print(
            f"check_c_abi_runtime: native_c_api.h PF_API set {sorted(header_names)} "
            f"!= expected {sorted(EXPECTED_NATIVE_C_API)}",
            file=sys.stderr,
        )
        return 1

    if impl_names != EXPECTED_NATIVE_C_API:
        print(
            f"check_c_abi_runtime: native_c_host.cpp PF_API set {sorted(impl_names)} "
            f"!= expected {sorted(EXPECTED_NATIVE_C_API)}",
            file=sys.stderr,
        )
        return 1

    # The two inventories are disjoint by construction: the native C API is
    # additive, so it may never redefine or shadow a compiled-strategy symbol.
    overlap = EXPECTED_NATIVE_C_API & (hdr_set | abi_set)
    if overlap:
        print(
            "check_c_abi_runtime: native C API symbols also appear in the "
            f"compiled-strategy surface: {sorted(overlap)}",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
