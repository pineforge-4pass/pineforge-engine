#!/usr/bin/env python3
"""No NativeRunSpec field is set by the Pine adapter and then unconditionally shadowed.

R5 gap lane N11 (audit D2). PineExecutionAdapter::project() declares the kernel
model the Pine run uses; the adapter's host hooks may then override parts of
what the kernel computes from that model. A field that project() sets and a
hook overrides on EVERY call is dead by construction and misdescribes the
model to a reader. This check reads src/source/pine_adapter.cpp and fails when
such a field is assigned.

A hook "always answers" when its override in the adapter contains no
`nullopt` return: the kernel treats nullopt as "keep the kernel's own value",
so a body that never spells it never yields. The rule table below names, per
hook, the spec fields whose ONLY kernel consumption is the value that hook
replaces (derived from src/native_execution_consumer.cpp; a field that is also
read elsewhere -- `basis` feeds the MarginCallEvent equity, `check` and
`level_base` drive the kernel's own scheduling and level -- is not listed).

Stdlib only. Exit 0 when clean, 1 when a shadowed field is assigned or the
source no longer has the shape this check reads, 2 on usage errors.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "src/source/pine_adapter.cpp"

# hook override -> spec fields (as assigned inside project()) that the kernel
# consumes only through the value this hook may replace.
#   resolve_margin_call_units: the kernel computes `units` from
#   NativeMarginModel::sizing / shortfall_multiple / liquidation_min_units and
#   then lets the hook's answer replace it (native_execution_consumer.cpp
#   "The host sees the kernel's own facts and has the last word on the size").
SHADOW_RULES: dict[str, tuple[str, ...]] = {
    "resolve_margin_call_units": (
        "margin.sizing",
        "margin.shortfall_multiple",
        "margin.liquidation_min_units",
    ),
}

# A spec field whose kernel consumer is gated by another spec value the
# adapter fixes: (gating field, gating value the adapter must NOT set for the
# gated field to be live, gated field). report_open_position_at_end is read
# only under NativeReportPolicy::KernelRecorded
# (native_execution_consumer.cpp record_open_position_report_rows).
GATED_RULES: tuple[tuple[str, str, str], ...] = (
    ("spec.report_policy", "NativeReportPolicy::KernelRecorded", "spec.report_open_position_at_end"),
)


class ShapeError(RuntimeError):
    """The adapter source does not have the shape this check reads."""


def function_body(text: str, signature: str) -> str:
    """The brace-balanced body of the first definition whose signature matches."""
    match = re.search(signature, text)
    if not match:
        raise ShapeError("definition not found: " + signature)
    start = text.find("{", match.end())
    if start < 0:
        raise ShapeError("definition has no body: " + signature)
    depth = 0
    for index in range(start, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ShapeError("unbalanced body: " + signature)


def strip_comments(body: str) -> str:
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return re.sub(r"//[^\n]*", "", body)


def assigned_fields(body: str) -> set[str]:
    """`x.y = ...;` and `x.y.emplace(...)` / `x.y.push_back(...)` targets, comments removed."""
    code = strip_comments(body)
    fields = set(re.findall(r"\b((?:spec|margin|risk)\.[A-Za-z_][A-Za-z0-9_]*)\s*=[^=]", code))
    fields |= set(re.findall(r"\b((?:spec|margin|risk)\.[A-Za-z_][A-Za-z0-9_]*)\.(?:emplace|push_back|reset)\s*\(", code))
    return fields


def always_answers(text: str, hook: str) -> bool | None:
    """True when the adapter overrides `hook` and its body never returns nullopt.

    None when the adapter does not define the hook at all.
    """
    signature = r"PineExecutionAdapter::" + re.escape(hook) + r"\s*\("
    if not re.search(signature, text):
        return None
    body = strip_comments(function_body(text, signature))
    return "nullopt" not in body


def shadowed_assignments(text: str) -> list[str]:
    project = function_body(text, r"NativeRunSpec\s+PineExecutionAdapter::project\s*\(")
    assigned = assigned_fields(project)
    findings: list[str] = []
    for hook, fields in SHADOW_RULES.items():
        answers = always_answers(text, hook)
        if answers is None:
            continue
        if not answers:
            continue
        for field in sorted(fields):
            if field in assigned:
                findings.append(f"{field} is set in project() but {hook}() always answers: "
                                "the kernel value is shadowed on every call")
    code = strip_comments(project)
    for gate, live_value, gated in GATED_RULES:
        if gated not in assigned:
            continue
        gate_values = re.findall(re.escape(gate) + r"\s*=\s*([A-Za-z_:0-9]+)\s*;", code)
        if gate_values and all(value != live_value for value in gate_values):
            findings.append(f"{gated} is set in project() but {gate} is {gate_values[-1]}, "
                            f"under which the kernel never reads it (needs {live_value})")
    return findings


def check(text: str) -> list[str]:
    """Findings for one adapter source; the shape errors propagate."""
    return shadowed_assignments(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--adapter", type=Path, default=ADAPTER,
                        help="path to pine_adapter.cpp (default: the repository's)")
    args = parser.parse_args(argv)
    try:
        text = args.adapter.read_text()
    except OSError as error:
        print(f"check_adapter_spec_shadowing: cannot read {args.adapter}: {error}", file=sys.stderr)
        return 2
    try:
        findings = check(text)
    except ShapeError as error:
        print(f"check_adapter_spec_shadowing: {error}", file=sys.stderr)
        return 1
    for finding in findings:
        print("check_adapter_spec_shadowing: " + finding)
    if findings:
        return 1
    hooks = ", ".join(f"{hook}={'always-answers' if always_answers(text, hook) else 'yields'}"
                      for hook in SHADOW_RULES)
    print(f"check_adapter_spec_shadowing: no spec field is set and then unconditionally shadowed ({hooks})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
