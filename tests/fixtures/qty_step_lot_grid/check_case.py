#!/usr/bin/env python3
"""Check an engine report against the lot-grid case (stdlib only).

    python3 check_case.py <report.json|-> with|without [--case-dir DIR]

<report.json> is the harness JSON report (the `report` object of a /backtest
response, or the harness stdout), or a response that wraps it as {"report": ...}.
`with` = the run whose syminfo carried `mincontract`, `without` = null/absent.

Asserts, per case.json: the number of rows, rows below one lot, the first trade
quantity, (with the grid) every quantity on the grid, row-by-row equality with
the recorded rows, and the `applied_runtime.syminfo` record. Exit 0 only when
every check passes.
"""
import json
import math
import os
import sys


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    case_dir = os.path.dirname(os.path.abspath(__file__))
    if "--case-dir" in argv:
        case_dir = argv[argv.index("--case-dir") + 1]
        args = [a for a in args if a != case_dir]
    if len(args) != 2 or args[1] not in ("with", "without"):
        print(__doc__)
        return 2
    path, variant = args
    case = json.load(open(os.path.join(case_dir, "case.json")))
    want = case["expected"]["with_grid" if variant == "with" else "without_grid"]
    lot = case["expected"]["lot"]
    doc = json.load(sys.stdin if path == "-" else open(path))
    report = doc.get("report", doc)
    trades = report["trades"]
    expected = json.load(open(os.path.join(case_dir, want["rows_file"])))
    failures = []

    def check(name, ok, detail=""):
        print(("  OK   " if ok else "  FAIL ") + name + (f"  [{detail}]" if detail and not ok else ""))
        if not ok:
            failures.append(name)

    below = sum(1 for t in trades if t["qty"] < lot)
    check(f"rows == {want['rows']}", len(trades) == want["rows"], f"got {len(trades)}")
    check(f"rows below one lot == {want['rows_below_one_lot']}", below == want["rows_below_one_lot"], f"got {below}")
    check(
        f"first trade qty == {want['first_trade_qty']} (1e-12)",
        bool(trades) and abs(trades[0]["qty"] - want["first_trade_qty"]) <= 1e-12,
        f"got {trades[0]['qty'] if trades else None}",
    )
    if variant == "with":
        off = [t["qty"] for t in trades if abs(t["qty"] / lot - round(t["qty"] / lot)) > 1e-6]
        check("every quantity is a whole number of lots", not off, f"off-grid: {off[:3]}")
    if len(trades) == len(expected):
        bad = []
        for got, exp in zip(trades, expected):
            for key in ("entry_time", "exit_time"):
                if got[key] != exp[key]:
                    bad.append((exp["n"], key, got[key], exp[key]))
            if got["side"] != exp["side"]:
                bad.append((exp["n"], "side", got["side"], exp["side"]))
            for key in ("entry_price", "exit_price", "qty", "pnl", "commission"):
                if not math.isclose(got[key], exp[key], rel_tol=1e-9, abs_tol=1e-12):
                    bad.append((exp["n"], key, got[key], exp[key]))
        check("rows equal the recorded rows (rel 1e-9)", not bad, f"first differences: {bad[:3]}")
    runtime = report.get("applied_runtime", {})
    recorded = runtime.get("syminfo")
    if variant == "with":
        check(
            "applied_runtime.syminfo records the grid",
            isinstance(recorded, dict) and recorded.get("qty_step") == lot,
            f"got {recorded}",
        )
    else:
        check("applied_runtime has no syminfo grid record", not (isinstance(recorded, dict) and "qty_step" in recorded), f"got {recorded}")
    print("CASE PASSED" if not failures else f"CASE FAILED ({len(failures)} failed)")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
