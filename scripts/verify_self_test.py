#!/usr/bin/env python3
"""verify_self_test.py — the validator-of-the-validator.

A skeptic's objection to "231/232 trade-for-trade": *does the scorer actually
catch a wrong trade, or does it silently drop mismatches and still say
"excellent"?* This self-test injects KNOWN-BAD trade lists into a copy of a
real probe and asserts that `verify_corpus.py` flips from excellent to a
failing verdict.

It also DISCLOSES the scorer's deliberate tolerance: a single dropped trade
inside a large probe stays under the 1% count gate, so it is NOT flagged. That
is by design (edge-trim + count tolerance), and naming it honestly is part of
the production-readiness story — see the p100/worst-case disclosure in
verify_corpus.py / the validation report.

Exit 0 iff every gross corruption is caught (and the baseline passes).
"""
from __future__ import annotations

import csv
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERIFY = REPO / "scripts" / "verify_corpus.py"
PROBE = REPO / "corpus" / "validation" / "ta-macd-12-26-9-line-signal-cross-01"


def run_verify(strategy_dir: Path) -> int:
    """Return verify_corpus.py's exit code (0 == excellent/strong/anomaly)."""
    proc = subprocess.run(
        [sys.executable, str(VERIFY), str(strategy_dir)],
        capture_output=True, text=True,
    )
    return proc.returncode


def read_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open(encoding="utf-8-sig") as f:
        r = list(csv.reader(f))
    return r[0], r[1:]


def write_rows(path: Path, header: list[str], rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def main() -> int:
    if not PROBE.is_dir():
        print(f"SKIP: probe not found ({PROBE}); corpus submodule not initialised?")
        return 0  # not a failure in a public clone without the corpus

    passed = 0
    failed = 0

    def check(name: str, cond: bool) -> None:
        nonlocal passed, failed
        if cond:
            passed += 1
            print(f"  OK   {name}")
        else:
            failed += 1
            print(f"  FAIL {name}")

    with tempfile.TemporaryDirectory() as td:
        work = Path(td) / PROBE.name
        shutil.copytree(PROBE, work)
        eng = work / "engine_trades.csv"
        price_col = None
        header, base_rows = read_rows(eng)
        for i, c in enumerate(header):
            if c == "Price" or c.startswith("Price "):
                price_col = i
                break
        type_col = header.index("Type")

        # Work in trade units (2 rows per trade) so corruptions are precise.
        pairs = [base_rows[i:i + 2] for i in range(0, len(base_rows), 2)]
        n_trades = len(pairs)

        def flatten(ps):
            return [r for p in ps for r in p]

        # 0. Baseline must verify as a pass (excellent/strong).
        check("baseline is a pass", run_verify(work) == 0)

        # 1. Gross entry-price corruption on every entry row -> entry delta huge
        #    / no match -> must FAIL.
        rows = [r[:] for r in base_rows]
        for r in rows:
            if len(r) > price_col and r[type_col].lower().startswith("entry"):
                try:
                    r[price_col] = f"{float(r[price_col]) + 100.0:.6f}"
                except ValueError:
                    pass
        write_rows(eng, header, rows)
        check("gross entry-price shift (+100) is caught", run_verify(work) != 0)

        # 2. Drop ~30% of trades from the INTERIOR -> count mismatch the common-
        #    window trim cannot hide -> must FAIL.
        lo, hi = int(n_trades * 0.35), int(n_trades * 0.65)
        interior_dropped = pairs[:lo] + pairs[hi:]
        write_rows(eng, header, flatten(interior_dropped))
        check("dropping ~30% of interior trades is caught", run_verify(work) != 0)

        # 3. Flip every direction -> directions mismatch -> no matches -> FAIL.
        rows = [r[:] for r in base_rows]
        for r in rows:
            t = r[type_col]
            if "long" in t.lower():
                r[type_col] = t.lower().replace("long", "short")
            elif "short" in t.lower():
                r[type_col] = t.lower().replace("short", "long")
        write_rows(eng, header, rows)
        check("flipping every direction is caught", run_verify(work) != 0)

        # DISCLOSURE (not gating): two known, by-design tolerances of the scorer.
        # (a) A single dropped trade stays under the 1% count gate.
        write_rows(eng, header, flatten(pairs[1:]))   # drop earliest trade
        single_caught = run_verify(work) != 0
        print(
            f"  NOTE single-trade drop (1/{n_trades} = {100.0/max(n_trades,1):.2f}%) "
            f"{'IS' if single_caught else 'is NOT'} caught "
            f"(sub-1% count tolerance is by design)"
        )
        # (b) A trailing contiguous block is trimmed away by the common-window
        #     alignment, so it is NOT counted as dropped — the deeper masking a
        #     p100/unmatched-count disclosure addresses.
        write_rows(eng, header, flatten(pairs[: n_trades // 2]))  # keep older half
        block_caught = run_verify(work) != 0
        print(
            f"  NOTE trailing-block drop (50%) "
            f"{'IS' if block_caught else 'is NOT'} caught "
            f"(align-then-trim masks end blocks; motivates the p100/unmatched "
            f"roll-up in the validation report)"
        )

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
