#!/usr/bin/env python3
"""Run the committed reversing probe through the production run-json CLI.

The corpus harness supplies its existing settings, windowing and CSV export.
Only its container launcher is replaced: execution is a subprocess running the
unmodified docker/run_json.py against the CMake-built real strategy module.
Missing fixtures, modules or fresh feeds are failures, never skips.
H2-9: on the lab, run `python3 scripts/derive_corpus_feeds.py` once before
ctest and before parallel feed consumers. This E2E never derives feeds.
"""
from __future__ import annotations

import argparse
import csv
import difflib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "corpus/validation/bracket-atr-trailing-stop-state-01"
TRADE_FIELDS = ("Trade #", "Type", "Date and time", "Price", "Qty")

sys.path.insert(0, str(ROOT / "scripts"))
import derive_corpus_feeds as derived
import pf_release_run
import run_strategy


def trade_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        assert set(TRADE_FIELDS) <= set(reader.fieldnames or ()), (
            f"{path}: missing trade identity/time/price/quantity columns")
        rows = [tuple(row[field] for field in TRADE_FIELDS) for row in reader]
    assert rows, f"{path}: empty trade tape"
    return rows


def require_reversing_tape(rows, label):
    directions = [row[1].removeprefix("Entry ") for row in rows
                  if row[1].startswith("Entry ")]
    assert set(directions) == {"long", "short"}, (
        f"{label}: reference/run must contain both a long and a short trade")
    assert any(a != b for a, b in zip(directions, directions[1:])), (
        f"{label}: trade tape must flip direction at least once")


def require_fresh_derived():
    # The E2E reads the lab's existing feeds. Never materialize or race a
    # writer, and never modify the corpus submodule from this test.
    preparation = (
        "prepare the corpus before ctest: python3 scripts/derive_corpus_feeds.py "
        "(run once before parallel consumers)")
    if not derived.SOURCE_1M.is_file() or derived.SOURCE_1M.stat().st_size < 1_000_000:
        raise RuntimeError(
            f"E2E needs the full corpus 1m feed, not an LFS pointer: {derived.SOURCE_1M}; "
            f"fetch the corpus LFS data, then {preparation}")
    if not derived._derived_cache_is_fresh(
            derived.SOURCE_1M, (derived.DERIVED_15M, derived.DERIVED_15M_WINDOW),
            derived.DERIVATION_STAMP):
        raise RuntimeError(
            f"E2E needs fresh derived feeds: {derived.DERIVED_15M}, "
            f"{derived.DERIVED_15M_WINDOW}, stamp {derived.DERIVATION_STAMP}; {preparation}")


def run_probe(module, artifacts):
    # Check even when run_strategy's selected metadata needs no derivation.
    require_fresh_derived()
    reference = PROBE / "engine_trades.csv"
    for path in (module, reference, PROBE / "strategy.pine", PROBE / "generated.cpp",
                 PROBE / "inputs.json"):
        assert path.is_file(), f"required E2E input is missing: {path}"
    frozen_reference = reference.read_bytes()
    expected = trade_rows(reference)
    require_reversing_tape(expected, "reference")
    artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts))
    print(f"reversal scratch E2E artifacts: {work}", flush=True)
    actual_csv = work / "actual_trades.csv"
    runs = []

    def launch_run_json(generated_cpp, ohlcv, *, inputs, overrides, input_tf,
                        script_tf, bar_magnifier, magnifier_samples,
                        magnifier_dist, magnifier_volume_weighted,
                        trade_start_ms, chart_tz, syminfo, image=None):
        # The corpus runner has already normalized these knobs and trimmed
        # the input feed. No engine export or report producer is substituted.
        assert Path(generated_cpp).resolve() == PROBE / "generated.cpp"
        prefix = work / f"run-json-{len(runs) + 1}"
        command = [
            sys.executable, "-B", str(ROOT / "docker/run_json.py"),
            "--so", str(module), "--ohlcv", str(ohlcv),
            "--generated-cpp", str(generated_cpp),
            "--inputs", json.dumps(inputs), "--overrides", json.dumps(overrides),
            "--input-tf", input_tf, "--script-tf", script_tf,
            "--bar-magnifier", str(bar_magnifier).lower(),
            "--magnifier-samples", str(magnifier_samples),
            "--magnifier-dist", magnifier_dist.lower(),
            "--chart-tz", chart_tz,
        ]
        if magnifier_volume_weighted:
            command.append("--magnifier-volume-weighted")
        if trade_start_ms is not None:
            command += ["--trade-start-ms", str(trade_start_ms)]
        if syminfo:
            syminfo_path = prefix.with_suffix(".syminfo.json")
            syminfo_path.write_text(json.dumps(syminfo), encoding="utf-8")
            command += ["--syminfo", str(syminfo_path)]
        prefix.with_suffix(".argv.json").write_text(
            json.dumps(command, indent=2) + "\n", encoding="utf-8")
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                timeout=1800)
        runs.append(result)
        prefix.with_suffix(".stdout.json").write_text(result.stdout, encoding="utf-8")
        prefix.with_suffix(".stderr.txt").write_text(result.stderr, encoding="utf-8")
        detail = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        assert result.returncode == 0, (
            f"run_json.py must exit 0; got {result.returncode}\n{detail}")
        for forbidden in ("engine_invariant", "did not provision trade scratch",
                          "did not provision dependency scratch"):
            assert forbidden not in result.stdout + result.stderr, (
                f"run_json.py reported {forbidden}\n{detail}")
        report = json.loads(result.stdout)
        assert not report.get("error"), f"run_json.py returned an error report: {report}"
        assert isinstance(report.get("trades"), list), "run_json.py omitted its trade array"
        return report

    try:
        # Reuse the real corpus entry point's metadata/window/export handling.
        # Its launcher alone dispatches to the existing local production CLI.
        # The --output target is a new artifact, never engine_trades.csv.
        with mock.patch.object(pf_release_run, "run_release", launch_run_json), \
                mock.patch.object(run_strategy, "ensure_derived", require_fresh_derived), \
                mock.patch.object(sys, "argv", [
                    str(ROOT / "scripts/run_strategy.py"), str(PROBE),
                    "--runner", "docker", "--output", str(actual_csv),
                ]):
            status = run_strategy.main()
        assert status == 0, f"corpus harness must exit 0; got {status}"
        assert runs, "corpus harness did not invoke run_json.py"
        actual = trade_rows(actual_csv)
        require_reversing_tape(actual, "run")
        difference = "".join(difflib.unified_diff(
            [",".join(row) + "\n" for row in expected],
            [",".join(row) + "\n" for row in actual],
            fromfile=str(reference), tofile=str(actual_csv)))
        assert actual == expected, (
            "STOP: reversing probe trade mismatch; do not edit or rebaseline "
            f"engine_trades.csv\n{difference}")
    finally:
        assert reference.read_bytes() == frozen_reference, (
            "read-only reference engine_trades.csv changed during E2E")
    print(f"PASS: {len(actual) // 2} trades; both directions; every trade row matches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--so", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    run_probe(args.so.resolve(), args.artifacts.resolve())


if __name__ == "__main__":
    main()
