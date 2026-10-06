#!/usr/bin/env python3
"""`run_json --outputs` end to end, through docker/run_json.py as a process.

Usage: test_run_json_outputs.py <outputs library> <generated library>

<outputs library> is tests/outputs_fixture_strategy.cpp, a module that records
outputs; <generated library> is a frozen generated strategy, which records
none. Each case writes an OHLCV CSV, runs run_json.py on it and compares the
JSON it prints with values computed here from the same rows:

  * --outputs writes the `outputs` block: the bars' times, every series slot
    the manifest lists, the run constants, the hline read from its price
    constant, and every event in sequence order with its phase name;
  * without --outputs the report has no new key, and equals the flagged one
    less `outputs`, `applied_runtime.outputs`, `fingerprint` and
    `elapsed_seconds`;
  * --outputs on a library without outputs, or on one whose switch is
    refused, is the structured {"engine", "error"} failure, exit status 1;
  * a run that publishes no bar writes empty arrays and a null hline price;
  * --bench --outputs is accepted and records the same block.
"""
from __future__ import annotations

import copy
import csv
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN_JSON = ROOT / "docker" / "run_json.py"

STEP = 5 * 60 * 1000
T0 = 1704067200000
GREEN, RED, GRAY = 0xFF4CAF50, 0xFFF23645, 0xFF787B86

MANIFEST = {
    "changes_trading": [],
    "constants": [{"index": 0, "output": "h0", "param": "price"},
                  {"encoding": "rgba-u32", "index": 1, "output": "h0", "param": "color"}],
    "declaration": {"kind": "indicator", "overlay": True, "title": "Outputs fixture"},
    "message_format": "pineforge/v1",
    "not_outputs": [],
    "outputs": [
        {"colors": {"color": {"slot": 1}}, "id": "o0", "index": 0, "kind": "plot", "line": 3,
         "series": [0], "title": "Close"},
        {"events": "mark", "id": "o1", "index": 1, "kind": "plotshape", "line": 4,
         "mark_rule": "bool-true", "series": [2], "title": "Up"},
        {"events": "per-call", "freq": "once_per_bar_close", "id": "o2", "index": 2,
         "kind": "alert", "line": 5, "series": []},
        {"id": "o3", "index": 3, "kind": "plot", "line": 6, "series": [3], "title": "Change"},
        {"colors": {"color": {"constant": 1}}, "id": "h0", "index": 4, "kind": "hline",
         "line": 7, "price": {"constant": 0, "default": 50}, "title": "Mid"},
    ],
    "schema_version": "pineforge-outputs-manifest/v1",
    "series": [{"output": "o0", "role": "value", "slot": 0},
               {"encoding": "rgba-u32", "output": "o0", "role": "color", "slot": 1},
               {"encoding": "bool-as-1-0", "output": "o1", "role": "value", "slot": 2},
               {"output": "o3", "role": "value", "slot": 3}],
    "source_sha256": "0" * 64,
}
NO_OUTPUTS_ERROR = ("--outputs: this library records no outputs (compile the script as an "
                    "indicator, or with outputs on)")


def rgba(argb: int) -> int:
    return ((argb << 8) & 0xFFFFFFFF) | (argb >> 24)


def rows(n: int) -> list[tuple[int, float, float, float, float, float]]:
    out = []
    price = 100.0
    for i in range(n):
        close = price + (1.25 if i % 3 != 1 else -0.75)
        out.append((T0 + i * STEP, price, max(price, close) + 0.5, min(price, close) - 0.5,
                    close, 10.0 + i))
        price = close
    return out


def expected_block(bars) -> dict:
    """What the fixture records over `bars`, computed from the rows."""
    canonical = json.dumps(MANIFEST, sort_keys=True, separators=(",", ":")).encode()
    closes = [b[4] for b in bars]
    ups = [b[4] > b[1] for b in bars]
    events = []
    for k, bar in enumerate(bars):
        if ups[k]:
            events.append(("o1", k, 1.0, None, None))
        if k % 3 == 2:
            events.append(("o2", k, None, f"close={bar[4]:.2f}", "once_per_bar_close"))
    ordinals: dict[tuple[str, int], int] = {}
    event_rows = []
    for sequence, (output, k, value, message, freq) in enumerate(events, start=1):
        ordinal = ordinals.get((output, k), 0)
        ordinals[(output, k)] = ordinal + 1
        row = {"sequence": sequence, "output": output, "bar_index": k,
               "bar_open_ms": bars[k][0], "bar_close_ms": bars[k][0] + STEP,
               "ordinal_in_bar": ordinal, "phase": "batch", "value": value,
               "message": message}
        if freq is not None:
            row["freq"] = freq
        event_rows.append(row)
    return {
        "schema_version": "pineforge-outputs/v1",
        "message_format": "pineforge/v1",
        "manifest_sha256": hashlib.sha256(canonical).hexdigest(),
        "manifest": MANIFEST,
        "bars": {"open_ms": [b[0] for b in bars], "close_ms": [b[0] + STEP for b in bars]},
        "series": [
            {"slot": 0, "output": "o0", "values": closes},
            {"slot": 1, "output": "o0", "values": [rgba(GREEN if up else RED) for up in ups]},
            {"slot": 2, "output": "o1", "values": [1.0 if up else 0.0 for up in ups]},
            {"slot": 3, "output": "o3",
             "values": [None if k < 2 else closes[k] - closes[k - 1] for k in range(len(bars))]},
        ],
        "constants": [50.0, rgba(GRAY)] if bars else [None, None],
        "hlines": [{"output": "h0", "price": 50.0 if bars else None}],
        "events": event_rows,
    }


class RunJsonOutputsTests(unittest.TestCase):
    outputs_library: Path
    generated_library: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="pf-run-json-outputs-")
        cls.bars = rows(9)
        cls.csv = Path(cls.tmp.name) / "bars.csv"
        with cls.csv.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(["timestamp", "open", "high", "low", "close", "volume"])
            for bar in cls.bars:
                writer.writerow([bar[0], *(repr(v) for v in bar[1:])])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_json(self, library: Path, *args: str, env: dict | None = None):
        command = [sys.executable, str(RUN_JSON), "--so", str(library), "--ohlcv",
                   str(self.csv), "--input-tf", "5", *args]
        done = subprocess.run(command, capture_output=True, text=True, timeout=300,
                              env={**os.environ, **(env or {})})
        lines = [line for line in done.stdout.splitlines() if line.strip()]
        self.assertEqual(len(lines), 1, done.stdout + done.stderr)
        return done.returncode, json.loads(lines[0])

    def test_outputs_block(self) -> None:
        code, report = self.run_json(self.outputs_library, "--outputs")
        self.assertEqual(code, 0, report)
        self.assertEqual(report["outputs"], expected_block(self.bars))
        self.assertIs(report["applied_runtime"]["outputs"], True)
        self.assertIs(report["fingerprint"]["provenance"]["runtime"]["outputs"], True)
        self.assertEqual(list(report)[-1], "fingerprint")

    def test_without_the_flag_nothing_else_changes(self) -> None:
        code, flagged = self.run_json(self.outputs_library, "--outputs")
        self.assertEqual(code, 0, flagged)
        code, plain = self.run_json(self.outputs_library)
        self.assertEqual(code, 0, plain)
        self.assertNotIn("outputs", plain)
        self.assertNotIn("outputs", plain["applied_runtime"])
        self.assertNotIn("outputs", plain["fingerprint"]["provenance"]["runtime"])
        stripped = copy.deepcopy(flagged)
        del stripped["outputs"]
        del stripped["applied_runtime"]["outputs"]
        for report in (stripped, plain):
            del report["fingerprint"]
            del report["elapsed_seconds"]
        self.assertEqual(stripped, plain)

    def test_a_library_without_outputs_is_refused(self) -> None:
        code, report = self.run_json(self.generated_library, "--outputs")
        self.assertEqual(code, 1)
        self.assertEqual(report, {"engine": "pineforge", "error": NO_OUTPUTS_ERROR})
        code, report = self.run_json(self.generated_library)
        self.assertEqual(code, 0, report)
        self.assertNotIn("outputs", report)

    def test_a_refused_switch_is_the_structured_failure(self) -> None:
        code, report = self.run_json(self.outputs_library, "--outputs",
                                     env={"PF_OUTPUTS_FIXTURE_UNDECLARED": "1"})
        self.assertEqual(code, 1)
        self.assertEqual(report, {"engine": "pineforge",
                                  "error": "--outputs: outputs: this module declares no outputs"})

    def test_a_run_with_no_rows(self) -> None:
        code, report = self.run_json(self.outputs_library, "--outputs",
                                     "--inputs", json.dumps({"publish": "false"}))
        self.assertEqual(code, 0, report)
        self.assertEqual(report["outputs"], expected_block([]))
        self.assertEqual(report["outputs"]["hlines"], [{"output": "h0", "price": None}])

    def test_bench_records_the_same_block(self) -> None:
        code, report = self.run_json(self.outputs_library, "--outputs", "--bench",
                                     "--warmup", "1", "--repeats", "2")
        self.assertEqual(code, 0, report)
        self.assertEqual(report["outputs"], expected_block(self.bars))
        self.assertEqual(len(report["diagnostics"]["timing"]["samples_ns"]), 2)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(f"usage: {sys.argv[0]} <outputs library> <generated library>")
    RunJsonOutputsTests.outputs_library = Path(sys.argv.pop(1)).resolve()
    RunJsonOutputsTests.generated_library = Path(sys.argv.pop(1)).resolve()
    unittest.main()
