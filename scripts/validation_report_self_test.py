#!/usr/bin/env python3
"""Assert the Markdown report and canonical corpus verifier stay lock-step.

The default fixtures are the three probes that exposed distinct stale-report
paths in July 2026. Pass ``--all`` to audit every initialized validation probe.
The test compares both tiers and report metrics, so a copied/stale rubric
cannot silently change the headline or its supporting numbers.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCRIPTS = REPO / "scripts"
sys.path.insert(0, str(SCRIPTS))

import regen_validation_report as report  # noqa: E402
import verify_corpus as verifier  # noqa: E402

DRIFT_FIXTURES = {
    "composite-trendmaster-three-tier-ema-state-01",
    "pyramid-deferred-flip-close-all-01",
    "ta-stochastic-rsi-cross-01",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--all", action="store_true", help="compare every validation probe")
    args = ap.parse_args()

    root = REPO / "corpus" / "validation"
    if not root.is_dir():
        print(f"SKIP: validation corpus not found ({root})")
        return 0

    probes = sorted(
        p for p in root.iterdir()
        if p.is_dir()
        and p.name != "symbol-specified"
        and (args.all or p.name in DRIFT_FIXTURES)
    )
    if not args.all and {p.name for p in probes} != DRIFT_FIXTURES:
        missing = sorted(DRIFT_FIXTURES - {p.name for p in probes})
        print(f"SKIP: drift fixtures not found: {', '.join(missing)}")
        return 0
    mismatches: list[str] = []
    verifier_counts: dict[str, int] = {}
    report_counts: dict[str, int] = {}

    for probe in probes:
        analysis = verifier.analyze_strategy(probe)
        canonical_row = analysis.report_row()
        report_row = report._verify_probe(probe)
        canonical_tier = analysis.label
        report_tier = report_row["tier"]
        verifier_counts[canonical_tier] = verifier_counts.get(canonical_tier, 0) + 1
        report_counts[report_tier] = report_counts.get(report_tier, 0) + 1
        if canonical_tier != report_tier:
            mismatches.append(
                f"{probe.name}: verifier={canonical_tier}, report={report_tier}"
            )
        if report_row != canonical_row:
            mismatches.append(f"{probe.name}: report metrics differ from canonical analysis")

    if mismatches:
        print("FAIL: report tier drift detected")
        for mismatch in mismatches:
            print(f"  {mismatch}")
        print(f"  verifier counts: {verifier_counts}")
        print(f"  report counts:   {report_counts}")
        return 1

    scope = "all probes" if args.all else "drift fixtures"
    print(f"OK: {len(probes)} {scope} match canonical tiers and metrics")
    print(f"  tier counts: {verifier_counts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
