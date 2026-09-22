#!/usr/bin/env python3
"""Every NativeStrategyHost member has a C spelling or a stated exclusion.

R5 lane N8. The audit's G1-b finding was that fourteen C++ capabilities had no
C spelling and that the header's own exclusion list named three unrelated ones,
so a reader could not tell "not exposed" from "nobody looked".  This guard
removes that possibility mechanically:

  * the public surface of `class NativeStrategyHost`
    (include/pineforge/native_host.hpp) is the authority for WHAT exists;
  * the COVERAGE block in include/pineforge/native_c_api.h is the authority for
    what each member's C spelling is, or why it has none;
  * the two must agree EXACTLY -- no member missing from the block, no row in
    the block naming a member that no longer exists.

A `[C]` row must also name a symbol or field that really is declared in the C
headers, so a spelling cannot be claimed for something that was never added.
Exclusion rows carry their reason in the same line and are free text.

R5 lane E15 adds the members that census cannot see: a protected member of
the base, `class BacktestEngine` (include/pineforge/engine.hpp), that a host
is documented to call or override from its callbacks. Those are opt-in: a
`// @host-seam` line marks each one's declaration, and the block's
BASE-CLASS SEAMS list must name exactly the marked set, under the same row
rules. A marker outside the class, or one that does not precede a member
function, fails too, so a marker cannot quietly mark nothing.

R5 lane F4 adds the enumerations (audit P4-b). Every `typedef enum` in the two
C headers is classified here, once: either it is the C twin of a kernel
`enum class` (ENUM_TWINS names the header and the enumeration), or it has no
enumeration to mirror (C_ONLY says why -- a std::variant's alternative index, a
bit mask, a bool, a C-only protocol). For a twin, the kernel enumeration is
parsed out of its header and compared by VALUE: every kernel enumerator needs
a C enumerator of the same value unless it is excluded by name with a reason,
an excluded one must stay unspelled, and every C enumerator must name a kernel
value. So a kernel enumerator appended without a C name fails here -- the
static_asserts in src/native_c_host.cpp pin the names they list, and cannot see
one that nobody listed -- and so does a C enumeration nobody classified.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "pineforge"
HOST = INCLUDE / "native_host.hpp"
ENGINE = INCLUDE / "engine.hpp"
C_API = INCLUDE / "native_c_api.h"
PUBLIC_C = INCLUDE / "pineforge.h"

# Members that are not part of the host's own surface: the copy/move deletions
# the class spells out, and the friend declaration.
_SKIP = frozenset({"NativeStrategyHost", "operator="})

_ROW = re.compile(r"^\s*\*\s+\[(C|--)\]\s+(\w+)\s+(\S.*?)\s*$")

# Every C enumeration that mirrors a kernel `enum class`: C tag -> (the header
# under include/pineforge that declares the kernel enumeration, its name, the
# kernel enumerators deliberately left without a C name -> why).
ENUM_TWINS: dict[str, tuple[str, str, dict[str, str]]] = {
    "pf_magnifier_distribution_e": ("magnifier.hpp", "MagnifierDistribution", {}),
    "pf_close_cause_e": ("execution.hpp", "CloseCause", {}),
    "pf_native_scope_claim_e": ("native_order.hpp", "ScopeClaim", {}),
    "pf_native_side_e": ("native_order.hpp", "Side", {}),
    "pf_native_size_time_e": ("native_order.hpp", "SizeTime", {}),
    "pf_native_grid_policy_e": ("native_order.hpp", "ExecutionGridPolicy", {}),
    "pf_native_anchor_rounding_e": ("native_order.hpp", "NativeAnchorRounding", {}),
    "pf_native_arm_visibility_e": ("native_order.hpp", "NativeArmVisibility", {}),
    "pf_native_group_effect_e": ("native_order.hpp", "GroupEffect", {}),
    "pf_native_request_field_e": ("native_host.hpp", "NativeRequestField", {}),
    "pf_native_price_rule_e": ("native_host.hpp", "NativeCurrentPriceRule", {}),
    "pf_native_refusal_e": ("native_host.hpp", "NativeCurrentRefusal", {}),
    "pf_native_lifecycle_e": ("native_host.hpp", "NativeLifecycleKind", {}),
    "pf_native_risk_limit_e": ("native_order.hpp", "RiskLimitKind", {}),
    "pf_native_risk_day_e": ("native_run_spec.hpp", "NativeRiskDay", {}),
    "pf_native_risk_action_e": ("native_run_spec.hpp", "NativeRiskAction", {}),
    "pf_native_fee_kind_e": ("native_run_spec.hpp", "NativeFeeKind", {}),
    "pf_native_close_execution_e": ("native_run_spec.hpp", "NativeCloseExecution", {}),
    "pf_native_open_directions_e": ("native_run_spec.hpp", "NativeOpenDirections", {}),
    "pf_native_report_policy_e": ("native_run_spec.hpp", "NativeReportPolicy", {
        "KernelRecordedAtHostMarks":
            "its host names each report point from inside its own callbacks, and "
            "the C callback table has no call that marks one (lane E22)",
    }),
    "pf_native_price_grid_e": ("native_run_spec.hpp", "NativePriceGrid", {}),
    "pf_native_grid_rounding_e": ("native_run_spec.hpp", "NativeGridRounding", {}),
    "pf_native_calc_trigger_e": ("native_run_spec.hpp", "NativeCalculationTrigger", {}),
    "pf_native_open_bar_view_e": ("native_run_spec.hpp", "NativeOpenBarView", {}),
    "pf_native_liquidation_sizing_e": ("native_run_spec.hpp", "NativeLiquidationSizing", {}),
    "pf_native_series_source_e": ("native_run_spec.hpp", "NativeSeriesSource", {}),
    "pf_native_size_price_e": ("native_order.hpp", "SizePrice", {}),
    "pf_native_scope_basis_e": ("native_order.hpp", "ScopeBasis", {}),
    "pf_native_liquidation_check_e": ("native_run_spec.hpp", "NativeLiquidationCheck", {}),
    "pf_native_margin_equity_basis_e": ("native_run_spec.hpp", "NativeMarginEquityBasis", {}),
    "pf_native_margin_level_base_e": ("native_run_spec.hpp", "NativeLiquidationLevelBase", {}),
    "pf_native_sample_eligibility_e": ("native_run_spec.hpp", "SampleEligibility", {}),
    "pf_native_slot_label_e": ("native_run_spec.hpp", "NativeSlotLabelPolicy", {}),
    "pf_native_feed_tolerance_e": ("native_run_spec.hpp", "NativeFeedTolerance", {}),
    "pf_native_path_order_e": ("native_run_spec.hpp", "NativePathOrder", {}),
    "pf_native_abort_reporting_e": ("native_run_spec.hpp", "NativeAbortReporting", {}),
    "pf_native_calc_reason_e": ("native_host.hpp", "NativeCalculationReason", {}),
    "pf_native_margin_check_kind_e": ("native_host.hpp", "NativeMarginCheckKind", {}),
    "pf_native_opened_lot_fill_point_e": ("engine.hpp", "OpenedLotFillPoint", {}),
    "pf_native_price_provenance_e": ("market_driver.hpp", "NativePriceProvenance", {}),
    "pf_native_path_phase_e": ("market_driver.hpp", "NativePathPhase", {}),
    "pf_native_completion_kind_e": ("market_driver.hpp", "NativeCompletionKind", {}),
    "pf_native_quote_kind_e": ("native_host.hpp", "NativeCurrentQuoteKind", {}),
    "pf_native_terminal_reason_e": ("native_order.hpp", "AppliedTerminalReason", {}),
    "pf_native_reject_reason_e": ("native_order.hpp", "RequestRejectReason", {}),
    "pf_native_cancel_reason_e": ("native_order.hpp", "CancelReason", {}),
    "pf_native_match_reject_e": ("native_order.hpp", "MatchRejectReason", {}),
    "pf_native_activation_e": ("native_order.hpp", "ActivationKind", {}),
    "pf_native_origin_e": ("native_order.hpp", "RequestOrigin", {}),
    "pf_native_failure_code_e": ("native_host.hpp", "NativeFailureCode", {}),
    "pf_native_failure_operation_e": ("native_host.hpp", "NativeFailureOperation", {}),
    "pf_native_run_phase_e": ("native_host.hpp", "NativeRunPhase", {}),
    "pf_native_completion_e": ("native_host.hpp", "NativeCompletion", {}),
    "pf_native_spec_error_e": ("native_run_spec.hpp", "NativeRunSpecError", {}),
    "pf_native_spec_field_e": ("native_run_spec.hpp", "NativeRunSpecField", {}),
    "pf_native_append_error_e": ("native_host.hpp", "NativeAuxiliaryAppendError", {}),
}

# Every C enumeration with no kernel `enum class` to mirror, and why. A
# variant's alternative index is pinned in src/native_c_host.cpp by
# std::variant_size and translated by a std::visit that names every
# alternative, so it cannot drift silently either.
C_ONLY: dict[str, str] = {
    "pf_execution_contract_e": "BacktestEngine::execution_contract() answers an int",
    "pf_native_spec_optional_e": "bit mask of optional_mask; each bit gates one std::optional "
                                 "field of NativeRunSpec",
    "pf_fill_qty_partition_e": "the Pine source host's pending-order probe writes an int; "
                               "no kernel enumeration exists",
    "pf_native_intent_e": "alternative index of the std::variant native_order::OrderIntent",
    "pf_native_reduction_e": "alternative index of the std::variant native_order::ReductionSize",
    "pf_native_size_basis_e": "alternative index of the std::variant native_order::SizeBasis",
    "pf_native_trigger_e": "alternative index of the std::variant native_order::Trigger",
    "pf_native_anchor_e": "alternative index of the std::variant native_order::TriggerAnchor",
    "pf_native_capacity_e": "alternative index of the std::variant native_order::Capacity",
    "pf_native_owner_e": "alternative index of the std::variant native_order::Owner",
    "pf_native_group_e": "alternative index of the std::variant native_order::Group",
    "pf_native_execute_outcome_e": "the non-refusal alternatives of the std::variant "
                                   "NativeCurrentExecutionResult",
    "pf_native_event_kind_e": "the std::variant native_order::CommandEvent's alternatives plus "
                              "the driver and account rows",
    "pf_native_remaining_e": "alternative index of the std::variant "
                             "native_order::RemainingProjection",
    "pf_native_trigger_state_e": "alternative index of the std::variant native_order::TriggerState",
    "pf_native_spec_ext_mask_e": "bit mask of the present blocks of pf_native_run_spec_ext_v1",
    "pf_native_lookahead_e": "NativeTimeframeSubscription::lookahead is a bool",
    "pf_native_gaps_e": "NativeTimeframeSubscription::gaps is a bool",
    "pf_native_intrabar_kind_e": "alternative index of the std::variant IntrabarPath::value",
    "pf_native_answer_e": "the C answering-hook protocol; a C++ hook answers a std::optional",
}

_C_ENUM = re.compile(r"typedef\s+enum\s+(\w+)\s*\{(.*?)\}\s*\w+\s*;", re.DOTALL)
_ENUMERATOR = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?:=\s*(.+?))?\s*$", re.DOTALL)


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def _value(expr: str, known: dict[str, int]) -> int:
    """An enumerator's value: an integer literal (decimal or hex, any u/l
    suffix, optionally negated), a `1u << n` shift, or an earlier
    enumerator's name."""
    expr = expr.strip()
    shift = re.fullmatch(r"(\d+)[uUlL]*\s*<<\s*(\d+)[uUlL]*", expr)
    if shift:
        return int(shift.group(1)) << int(shift.group(2))
    number = re.fullmatch(r"(-?)\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", expr)
    if number:
        magnitude = int(number.group(2), 0)
        return -magnitude if number.group(1) else magnitude
    if expr in known:
        return known[expr]
    raise ValueError(f"cannot evaluate enumerator value {expr!r}")


def _enumerators(body: str) -> list[tuple[str, int]]:
    known: dict[str, int] = {}
    out: list[tuple[str, int]] = []
    following = 0
    for item in _strip_comments(body).split(","):
        if not item.strip():
            continue
        match = _ENUMERATOR.match(item)
        if not match:
            raise ValueError(f"cannot parse enumerator {item.strip()!r}")
        name, expr = match.group(1), match.group(2)
        value = _value(expr, known) if expr is not None else following
        known[name] = value
        out.append((name, value))
        following = value + 1
    return out


def c_enumerations(*texts: str) -> dict[str, list[tuple[str, int]]]:
    """Every `typedef enum <tag> { ... } <name>;` of the C headers, by tag."""
    out: dict[str, list[tuple[str, int]]] = {}
    for text in texts:
        for match in _C_ENUM.finditer(_strip_comments(text)):
            out[match.group(1)] = _enumerators(match.group(2))
    return out


def kernel_enumeration(text: str, name: str) -> list[tuple[str, int]] | None:
    """The enumerators of `enum class <name>` in one header, or None."""
    code = _strip_comments(text)
    match = re.search(r"\benum\s+class\s+" + re.escape(name) + r"\b[^{;]*\{(.*?)\}\s*;",
                      code, re.DOTALL)
    return _enumerators(match.group(1)) if match else None


def enum_twin_failures(c_texts: tuple[str, ...]) -> tuple[list[str], int, int]:
    """(failures, twinned enumerations, kernel enumerators covered)."""
    failures: list[str] = []
    try:
        c_enums = c_enumerations(*c_texts)
    except ValueError as error:
        return [f"a C enumeration does not parse: {error}"], 0, 0
    for tag in sorted(set(c_enums) - set(ENUM_TWINS) - set(C_ONLY)):
        failures.append(f"{tag} is neither twinned to a kernel enumeration nor ruled C-only")
    for tag in sorted((set(ENUM_TWINS) | set(C_ONLY)) - set(c_enums)):
        failures.append(f"{tag} is classified but no C header declares it")
    for tag in sorted(set(ENUM_TWINS) & set(C_ONLY)):
        failures.append(f"{tag} is classified twice")
    covered = 0
    for tag, (header, name, excluded) in sorted(ENUM_TWINS.items()):
        if tag not in c_enums:
            continue
        path = INCLUDE / header
        try:
            kernel = kernel_enumeration(path.read_text(encoding="utf-8"), name)
        except (OSError, ValueError) as error:
            failures.append(f"{tag}: cannot read `enum class {name}` in {header}: {error}")
            continue
        if kernel is None:
            failures.append(f"{tag}: no `enum class {name}` in {header}")
            continue
        c_values = {value: c_name for c_name, value in c_enums[tag]}
        kernel_values = {value for _, value in kernel}
        kernel_names = {kernel_name for kernel_name, _ in kernel}
        for stale in sorted(set(excluded) - kernel_names):
            failures.append(f"{tag}: the exclusion {name}::{stale} names no enumerator")
        for kernel_name, value in kernel:
            if kernel_name in excluded:
                if value in c_values:
                    failures.append(
                        f"{name}::{kernel_name} is excluded ({excluded[kernel_name]}) but "
                        f"{c_values[value]} spells its value {value}")
                continue
            covered += 1
            if value not in c_values:
                failures.append(f"{name}::{kernel_name} ({value}) has no C name in {tag}")
        for c_name, value in c_enums[tag]:
            if value not in kernel_values:
                failures.append(f"{c_name} ({value}) names no enumerator of {name}")
    return failures, len(set(ENUM_TWINS) & set(c_enums)), covered


# The opt-in marker for a base-class seam, and the heading of its list.
_SEAM_MARK = re.compile(r"^\s*//\s*@host-seam\b")
_SEAMS_HEADING = "BASE-CLASS SEAMS"


def host_members(text: str) -> set[str]:
    start = text.index("class NativeStrategyHost : public BacktestEngine {")
    end = text.index("\n};", start)
    body = text[start:end]
    # Only the public section: the class has one, ending at `friend class`.
    body = body.split("friend class", 1)[0]
    names: set[str] = set()
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "*", "#")) or not stripped:
            continue
        # A declaration is `<stuff> name(` at paren depth 0 for this line.
        match = re.search(r"(~?\w+)\s*\(", stripped)
        if not match:
            continue
        name = match.group(1)
        if name in _SKIP or name.startswith("~"):
            continue
        # Skip default-argument and initializer noise inside a body.
        if stripped.startswith(("return", "if", "for", "while", "}")):
            continue
        names.add(name)
    return names


def base_seams(text: str) -> tuple[set[str], list[str]]:
    """The BacktestEngine members engine.hpp marks `@host-seam`, and misuses."""
    start = text.index("class BacktestEngine {")
    end = text.index("\n};", start)
    first = text.count("\n", 0, start)
    last = text.count("\n", 0, end)
    lines = text.splitlines()
    names: set[str] = set()
    errors: list[str] = []
    for number, line in enumerate(lines):
        if not _SEAM_MARK.match(line):
            continue
        if not first < number <= last:
            errors.append(f"engine.hpp:{number + 1}: a @host-seam marker outside class "
                          "BacktestEngine")
            continue
        name = None
        for follow in lines[number + 1:last + 1]:
            stripped = follow.strip()
            if not stripped or stripped.startswith(("//", "/*", "*")):
                continue
            match = re.search(r"(~?\w+)\s*\(", stripped)
            name = match.group(1) if match else None
            break
        if name is None:
            errors.append(f"engine.hpp:{number + 1}: a @host-seam marker is not followed by a "
                          "member function declaration")
            continue
        names.add(name)
    return names, errors


def coverage_bounds(text: str) -> tuple[int, int]:
    try:
        start = text.index("COVERAGE")
        return start, text.index("HARDENING RULES", start)
    except ValueError:  # pragma: no cover - the block is required
        print("check_native_c_api_surface: no COVERAGE block in native_c_api.h",
              file=sys.stderr)
        raise SystemExit(2)


def _rows(block: str, seen: set[str]) -> tuple[dict[str, str], dict[str, str]]:
    spelled: dict[str, str] = {}
    excluded: dict[str, str] = {}
    for line in block.splitlines():
        match = _ROW.match(line)
        if not match:
            continue
        kind, name, detail = match.groups()
        if name in seen:
            print(f"check_native_c_api_surface: {name} listed twice", file=sys.stderr)
            raise SystemExit(1)
        seen.add(name)
        (spelled if kind == "C" else excluded)[name] = detail
    return spelled, excluded


def coverage_rows(text: str) -> tuple[tuple[dict[str, str], dict[str, str]],
                                      tuple[dict[str, str], dict[str, str]]]:
    """(spelled, excluded) of the host rows, then of the BASE-CLASS SEAMS rows."""
    start, end = coverage_bounds(text)
    block = text[start:end]
    split = block.find(_SEAMS_HEADING)
    if split < 0:
        print(f"check_native_c_api_surface: no {_SEAMS_HEADING} list in the COVERAGE block",
              file=sys.stderr)
        raise SystemExit(2)
    seen: set[str] = set()
    return _rows(block[:split], seen), _rows(block[split:], seen)


def main() -> int:
    host_text = HOST.read_text(encoding="utf-8")
    c_api_text = C_API.read_text(encoding="utf-8")
    public_text = PUBLIC_C.read_text(encoding="utf-8")
    engine_text = ENGINE.read_text(encoding="utf-8")

    members = host_members(host_text)
    seams, marker_errors = base_seams(engine_text)
    (spelled, excluded), (seam_spelled, seam_excluded) = coverage_rows(c_api_text)
    listed = set(spelled) | set(excluded)
    seams_listed = set(seam_spelled) | set(seam_excluded)
    # A claimed spelling is resolved against the DECLARATIONS only: the
    # COVERAGE block itself is cut out first, so a row can never be the sole
    # evidence for the symbol it names.
    block_start, block_end = coverage_bounds(c_api_text)
    declarations = c_api_text[:block_start] + c_api_text[block_end:] + public_text

    failures: list[str] = []
    missing = sorted(members - listed)
    if missing:
        failures.append(
            "members with neither a C spelling nor an exclusion: " + ", ".join(missing))
    stale = sorted(listed - members)
    if stale:
        failures.append(
            "COVERAGE rows naming members that no longer exist: " + ", ".join(stale))

    failures.extend(marker_errors)
    missing_seams = sorted(seams - seams_listed)
    if missing_seams:
        failures.append("marked BacktestEngine seams with neither a C spelling nor an "
                        "exclusion: " + ", ".join(missing_seams))
    stale_seams = sorted(seams_listed - seams)
    if stale_seams:
        failures.append(f"{_SEAMS_HEADING} rows naming members engine.hpp does not mark "
                        "@host-seam: " + ", ".join(stale_seams))

    enum_failures, twinned, covered = enum_twin_failures((c_api_text, public_text))
    failures.extend(enum_failures)

    # Every claimed spelling must name something the C headers declare.
    for name, detail in sorted({**spelled, **seam_spelled}.items()):
        tokens = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", detail)
        if not any(token in declarations for token in tokens
                   if token.startswith(("strategy_", "pf_", "PF_"))):
            failures.append(f"{name}: the C spelling '{detail}' names no declared symbol")

    if failures:
        for failure in failures:
            print("check_native_c_api_surface: " + failure, file=sys.stderr)
        return 1

    print(f"check_native_c_api_surface: {len(members)} NativeStrategyHost members, "
          f"{len(spelled)} with a C spelling, {len(excluded)} excluded with a reason; "
          f"{len(seams)} marked BacktestEngine seams, {len(seam_spelled)} with a C spelling, "
          f"{len(seam_excluded)} excluded with a reason; {twinned + len(C_ONLY)} C enumerations, "
          f"{twinned} twinned to a kernel enumeration ({covered} enumerators, each with a C "
          f"name), {len(C_ONLY)} C-only with a reason")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
