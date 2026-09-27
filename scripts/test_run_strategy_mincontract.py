#!/usr/bin/env python3
"""syminfo.mincontract is the lane template's quantity step.

TradingView reports ``syminfo.mincontract`` equal to each campaign symbol's
quantity step on every lane (tests/fixtures/syminfo_mincontract: 18 lab tv
read-outs of one synthetic script). run_strategy.py declares the fact from the
step the lane template names (PINEFORGE_VERIFY_QTY_STEP, which the case
runner hands every run of a lane that declares one) through
``syminfo_metadata["mincontract"]``, the channel generated code reads with
``get_syminfo_metadata("mincontract")``; a lane that names no step declares
nothing, and a probe's own value is kept.
"""

from __future__ import annotations

import contextlib
import csv
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_strategy
from run_strategy import LANE_QTY_STEP_ENV, inputs_run_kwargs

FIXTURES = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "syminfo_mincontract"

# Each read-out directory's lane template and the PINEFORGE_VERIFY_QTY_STEP
# its environment declares (registry lane_input_templates, read 2026-09-28);
# None: the template names no step (the ETH 15m lanes).
LANE_STEPS = {
    "eth-15": ("eth-scraped-15", None),
    "eth-1d": ("ethusdtp-1d", "0.0001"),
    "btcusdt-15": ("btcusdt-15", "1e-05"),
    "btcusdt-1d": ("btcusdt-1d", "1e-05"),
    "eurusd-15": ("eurusd-15", "0.01"),
    "eurusd-1d": ("eurusd-1d", "0.01"),
    "xauusd-15": ("xauusd-15", "0.01"),
    "xauusd-1d": ("xauusd-1d", "0.01"),
    "aapl-15": ("aapl-15", "1"),
    "aapl-1d": ("aapl-1d", "1"),
    "f-15": ("f-15", "1"),
    "f-1d": ("f-1d", "1"),
    "nifty-15": ("nifty-15", "1"),
    "nifty-1d": ("nifty-1d", "1"),
    "es1-15": ("es1-15", "1"),
    "es1-1d": ("es1-1d", "1"),
    "nq1-15": ("nq1-15", "1"),
    "nq1-1d": ("nq1-1d", "1"),
}


def _readout(directory: Path) -> tuple[float, float]:
    """TradingView's mincontract as its entry comment spells it, and the
    entry quantity (1000 * mincontract)."""
    with (directory / "tv_trades.csv").open(encoding="utf-8-sig") as f:
        entry = next(r for r in csv.DictReader(f) if r["Type"].startswith("Entry"))
    fields = dict(part.split("=", 1) for part in entry["Signal"].split("|"))
    return float(fields["mincontract"]), float(entry["Size (qty)"])


class TradingViewReadoutTests(unittest.TestCase):
    def test_every_lane_reads_its_quantity_step(self) -> None:
        scripts = set()
        self.assertEqual(sorted(p.name for p in FIXTURES.iterdir() if p.is_dir()),
                         sorted(LANE_STEPS))
        for name, (lane, step) in LANE_STEPS.items():
            with self.subTest(lane=lane):
                d = FIXTURES / name
                metrics = json.loads((d / "metrics.json").read_text(encoding="utf-8"))
                self.assertEqual(metrics["tapeChannel"], "ws-report-v1")
                self.assertEqual(metrics["wsProvenance"]["rangeProof"], "covered")
                self.assertEqual(metrics["tvTradesCsvHash"],
                                 hashlib.sha256((d / "tv_trades.csv").read_bytes()).hexdigest())
                scripts.add((d / "strategy.pine").read_bytes())
                mincontract, qty = _readout(d)
                self.assertAlmostEqual(qty, 1000 * mincontract, places=9)
                if step is None:
                    # ETHUSDT.P's step; the template names none.
                    self.assertEqual(mincontract, 0.0001)
                else:
                    self.assertEqual(mincontract, float(step))
        self.assertEqual(len(scripts), 1)


class InputsRunKwargsTests(unittest.TestCase):
    def kwargs(self, params: dict, env: dict) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            return inputs_run_kwargs(params, Path(tmp), Path(tmp) / "feed.csv", env=env)[1]

    def test_the_lane_step_becomes_mincontract(self) -> None:
        # The Lab hands a lane's step in twice: in the environment the case
        # runner set and as runtime_overrides.qty_step.
        for step in ("0.0001", "1e-05", "0.01", "1"):
            with self.subTest(step=step):
                kw = self.kwargs({"runtime_overrides": {"qty_step": float(step)}},
                                 {LANE_QTY_STEP_ENV: step})
                self.assertEqual(kw["syminfo_metadata"],
                                 {"qty_step": float(step), "mincontract": float(step)})

    def test_no_lane_step_declares_nothing(self) -> None:
        # An ETH 15m lane: the Lab's runtime_overrides.qty_step 0.0001 is its
        # own default, not a lane fact.
        kw = self.kwargs({"runtime_overrides": {"qty_step": 0.0001}}, {})
        self.assertEqual(kw["syminfo_metadata"], {"qty_step": 0.0001})
        for bad in ("", "0", "-1", "abc", "inf", "nan"):
            with self.subTest(bad=bad):
                kw = self.kwargs({}, {LANE_QTY_STEP_ENV: bad})
                self.assertIsNone(kw["syminfo_metadata"])

    def test_a_probes_own_mincontract_is_kept(self) -> None:
        kw = self.kwargs({"runtime_overrides": {"syminfo_metadata": {"mincontract": 0.5}}},
                         {LANE_QTY_STEP_ENV: "0.01"})
        self.assertEqual(kw["syminfo_metadata"], {"mincontract": 0.5})


class _FakeStrategy:
    calls: list[dict] = []
    declares_bar_magnifier = False

    def __init__(self, so_path: Path) -> None:
        self.lib = None

    def run(self, bars_csv: Path, params=None, **kwargs) -> dict:
        _FakeStrategy.calls.append(kwargs)
        return {"trades": [], "trace": [], "trace_names": [], "net_profit": 0.0,
                "input_bars_processed": 0}


class MainTests(unittest.TestCase):
    def test_a_run_carries_the_fact_to_the_engine(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            feed = d / "feed.csv"
            feed.write_text("timestamp,open,high,low,close,volume\n"
                            "1743465600000,10,11,9,10.5,1\n1743466500000,10,11,9,10.5,1\n",
                            encoding="utf-8")
            (d / "inputs.json").write_text(json.dumps(
                {"runtime_overrides": {"qty_step": 0.01}}), encoding="utf-8")
            argv = ["run_strategy.py", str(d), "--ohlcv", str(feed), "-o", str(d / "out.csv"),
                    "--no-trim-output"]
            for env, expected in (({LANE_QTY_STEP_ENV: "0.01"},
                                   {"qty_step": 0.01, "mincontract": 0.01}),
                                  ({}, {"qty_step": 0.01})):
                with self.subTest(env=env):
                    _FakeStrategy.calls.clear()
                    environ = {k: v for k, v in run_strategy.os.environ.items()
                               if k != LANE_QTY_STEP_ENV}
                    environ.update(env)
                    with mock.patch.object(run_strategy, "ensure_derived", lambda: None), \
                            mock.patch.object(run_strategy, "find_strategy_lib",
                                              lambda d, so_name="strategy.so": d / so_name), \
                            mock.patch.object(run_strategy, "Strategy", _FakeStrategy), \
                            mock.patch.dict(run_strategy.os.environ, environ, clear=True), \
                            mock.patch.object(sys, "argv", argv), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(run_strategy.main(), 0)
                    self.assertEqual(_FakeStrategy.calls[0]["syminfo_metadata"], expected)


if __name__ == "__main__":
    unittest.main()
