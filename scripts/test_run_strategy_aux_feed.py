#!/usr/bin/env python3
"""Focused runner proof for the optional auxiliary request.security feed."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_strategy as rs

from run_strategy import (
    Strategy,
    build_fingerprint,
    build_runtime_provenance,
    inputs_run_kwargs,
)


def _write_bars(path: Path, closes: list[float]) -> None:
    rows = ["timestamp,open,high,low,close,volume"]
    for index, close in enumerate(closes):
        timestamp = 1_704_205_800_000 + index * 60_000
        rows.append(
            f"{timestamp},{close},{close + 1},{close - 1},{close},10")
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


class _FakeLibrary:
    def __init__(self, *, aux_result: int | None = 0) -> None:
        self.events = []
        self.aux_result = aux_result

    def strategy_create(self, _params):
        self.events.append(("create",))
        return 7

    def run_backtest_full(self, _state, bars, count, *_rest):
        self.events.append((
            "run", count,
            [float(bars[index].close) for index in range(count)],
        ))

    def strategy_get_last_error(self, _state):
        return (b"rejected auxiliary feed"
                if self.aux_result not in (None, 0) else b"")

    def report_free(self, _report):
        pass

    def strategy_free(self, _state):
        pass

    def strategy_set_aux_security_feed(self, _state, bars, count, input_tf):
        if self.aux_result is None:
            raise AssertionError("setter must not be called")
        self.events.append((
            "aux", count, input_tf.decode(),
            [float(bars[index].close) for index in range(count)],
        ))
        return self.aux_result


class _FeatureAbsentLibrary(_FakeLibrary):
    strategy_set_aux_security_feed = None

    def __getattribute__(self, name):
        if name == "strategy_set_aux_security_feed":
            raise AttributeError(name)
        return super().__getattribute__(name)


def _strategy(fake) -> Strategy:
    strategy = Strategy.__new__(Strategy)
    strategy.lib = fake
    return strategy


class AuxSecurityRunnerTests(unittest.TestCase):
    def test_metadata_pair_resolves_relative_export(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            aux = root / "aux.csv"
            _write_bars(chart, [100.0])
            _write_bars(aux, [10.0])
            _, kwargs = inputs_run_kwargs({
                "aux_security_ohlcv_csv": "aux.csv",
                "aux_security_input_tf": "1",
            }, root, chart)
            self.assertEqual(kwargs["aux_security_ohlcv_csv"], aux.resolve())
            self.assertEqual(kwargs["aux_security_input_tf"], "1")
            self.assertEqual(
                len(kwargs["aux_security_source_file_sha256"]), 64)

            _, legacy = inputs_run_kwargs({}, root, chart)
            self.assertNotIn("aux_security_ohlcv_csv", legacy)
            self.assertNotIn("aux_security_input_tf", legacy)

    def test_aux_path_timeframe_and_content_mutate_runtime_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            aux_a = root / "aux-a.csv"
            aux_b = root / "aux-b.csv"
            _write_bars(chart, [100.0])
            _write_bars(aux_a, [10.0, 11.0])
            _write_bars(aux_b, [10.0, 11.0])

            _, kwargs_a = inputs_run_kwargs({
                "aux_security_ohlcv_csv": "aux-a.csv",
                "aux_security_input_tf": "1",
            }, root, chart)
            _, kwargs_path = inputs_run_kwargs({
                "aux_security_ohlcv_csv": "aux-b.csv",
                "aux_security_input_tf": "1",
            }, root, chart)
            _, kwargs_tf = inputs_run_kwargs({
                "aux_security_ohlcv_csv": "aux-a.csv",
                "aux_security_input_tf": "5",
            }, root, chart)
            provenance_a = build_runtime_provenance(kwargs_a, None)
            digest_a = build_fingerprint({"runtime": provenance_a})["digest"]
            self.assertNotEqual(
                provenance_a, build_runtime_provenance(kwargs_path, None))
            self.assertNotEqual(
                provenance_a, build_runtime_provenance(kwargs_tf, None))
            self.assertNotEqual(
                digest_a,
                build_fingerprint({
                    "runtime": build_runtime_provenance(kwargs_path, None),
                })["digest"])
            self.assertNotEqual(
                digest_a,
                build_fingerprint({
                    "runtime": build_runtime_provenance(kwargs_tf, None),
                })["digest"])

            _write_bars(aux_a, [10.0, 12.0])
            _, kwargs_content = inputs_run_kwargs({
                "aux_security_ohlcv_csv": "aux-a.csv",
                "aux_security_input_tf": "1",
            }, root, chart)
            self.assertNotEqual(
                provenance_a, build_runtime_provenance(kwargs_content, None))
            self.assertNotEqual(
                digest_a,
                build_fingerprint({
                    "runtime": build_runtime_provenance(
                        kwargs_content, None),
                })["digest"])

    def test_runner_loads_aux_bars_and_installs_them_before_chart_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            aux = root / "aux.csv"
            _write_bars(chart, [100.0, 200.0])
            _write_bars(aux, [10.0, 11.0, 12.0])
            fake = _FakeLibrary()

            report = _strategy(fake).run(
                chart, input_tf="1D", script_tf="1D",
                aux_security_ohlcv_csv=aux,
                aux_security_input_tf="1")

            self.assertEqual(fake.events[1],
                             ("aux", 3, "1", [10.0, 11.0, 12.0]))
            self.assertEqual(fake.events[2],
                             ("run", 2, [100.0, 200.0]))
            self.assertEqual(report["aux_security_input_tf"], "1")
            self.assertEqual(report["aux_security_ohlcv_csv"],
                             str(aux.resolve()))
            self.assertEqual(
                len(report["aux_security_source_file_sha256"]), 64)
            self.assertEqual(
                len(report["aux_security_source_feed_sha256"]), 64)

    def test_requested_aux_feed_requires_export_and_runtime_symbol(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            aux = root / "aux.csv"
            _write_bars(chart, [100.0])
            _write_bars(aux, [10.0])

            with self.assertRaisesRegex(RuntimeError, "lacks auxiliary"):
                _strategy(_FeatureAbsentLibrary()).run(
                    chart, aux_security_ohlcv_csv=aux,
                    aux_security_input_tf="1")

            with self.assertRaisesRegex(RuntimeError,
                                        "rejected auxiliary feed"):
                _strategy(_FakeLibrary(aux_result=-1)).run(
                    chart, aux_security_ohlcv_csv=aux,
                    aux_security_input_tf="1")

            with self.assertRaisesRegex(FileNotFoundError,
                                        "export not found"):
                inputs_run_kwargs({
                    "aux_security_ohlcv_csv": "missing.csv",
                    "aux_security_input_tf": "1",
                }, root, chart)


    def test_hash_then_replace_adversary_cannot_change_loaded_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chart = root / "chart.csv"
            aux = root / "aux.csv"
            _write_bars(chart, [100.0])
            _write_bars(aux, [10.0])
            real_hash = rs._sha256_file

            def hash_then_replace(path):
                digest = real_hash(path)
                _write_bars(Path(path), [99.0])
                return digest

            # Reproduce the old vulnerability: replacement occurs immediately
            # after the attested raw hash is returned. The run's one-read byte
            # snapshot sees 99 and must reject the stale hash for 10.
            with mock.patch.object(
                    rs, "_sha256_file", side_effect=hash_then_replace):
                _, resolved = inputs_run_kwargs({
                    "aux_security_ohlcv_csv": "aux.csv",
                    "aux_security_input_tf": "1",
                }, root, chart)
            with self.assertRaisesRegex(RuntimeError,
                                        "changed after metadata resolution"):
                _strategy(_FakeLibrary()).run(chart, **resolved)

    def test_feature_absent_without_aux_request_keeps_legacy_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            chart = Path(tmp) / "chart.csv"
            _write_bars(chart, [100.0])
            fake = _FeatureAbsentLibrary()
            _strategy(fake).run(chart)
            self.assertEqual(fake.events,
                             [("create",), ("run", 1, [100.0])])


if __name__ == "__main__":
    unittest.main()
