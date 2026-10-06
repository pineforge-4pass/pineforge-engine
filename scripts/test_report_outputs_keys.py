#!/usr/bin/env python3
"""The `outputs` block of the run_json report: keys, order and value rules.

docker/run_json.py writes an `outputs` block only when --outputs asked for
one (see build_outputs_block). Its shape is a wire format, so these tests pin
it without a compiled engine: build_outputs_block reads the record through a
reader object of callables, which a fake stands in for here.

Pinned: the block's key order; the keys of a series, constant, hline and
event entry; the phase names; `freq` on the events of an alert output only;
null for a NaN or infinite double, for a time equal to INT64_MIN and for an
hline price no bar wrote (a run with no rows); integers for an `rgba-u32`
slot or constant; `manifest_sha256` over the raw bytes the library returned;
the ctypes mirror of pf_output_event_v1_t; and that build_report_dict, which
every run calls, writes no `outputs` key.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import math
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json  # noqa: E402

BLOCK_KEYS = ["schema_version", "message_format", "manifest_sha256", "manifest",
              "bars", "series", "constants", "hlines", "events"]
EVENT_KEYS = ["sequence", "output", "bar_index", "bar_open_ms", "bar_close_ms",
              "ordinal_in_bar", "phase", "value", "message"]
INT64_MIN = -(2 ** 63)

MANIFEST = {
    "schema_version": "pineforge-outputs-manifest/v1",
    "message_format": "pineforge/v1",
    "outputs": [
        {"index": 0, "id": "o0", "kind": "plot", "series": [0],
         "colors": {"color": {"slot": 1}}},
        {"index": 1, "id": "o1", "kind": "plotshape", "series": [2], "events": "mark"},
        {"index": 2, "id": "o2", "kind": "alert", "series": [], "events": "per-call",
         "freq": "once_per_bar"},
        {"index": 3, "id": "h0", "kind": "hline", "price": {"constant": 0},
         "colors": {"color": {"constant": 1}}},
        {"index": 4, "id": "h1", "kind": "hline", "price": {"constant": 2}},
    ],
    "series": [
        {"slot": 2, "output": "o1", "role": "value", "encoding": "bool-as-1-0"},
        {"slot": 0, "output": "o0", "role": "value"},
        {"slot": 1, "output": "o0", "role": "color", "encoding": "rgba-u32"},
    ],
    "constants": [
        {"index": 0, "output": "h0", "param": "price"},
        {"index": 1, "output": "h0", "param": "color", "encoding": "rgba-u32"},
        {"index": 2, "output": "h1", "param": "price"},
    ],
}
# Deliberately not the canonical spelling: the digest is over these bytes.
MANIFEST_BYTES = json.dumps(MANIFEST, indent=1).encode()


def event(**fields):
    base = dict(struct_version=1, size=80, sequence=1, output_index=1, bar_index=0,
                bar_open_ms=0, bar_close_ms=300000, ordinal_in_bar=0, phase=0,
                confirmed=1, value=1.0, message_hash64=0, message=None)
    base.update(fields)
    return SimpleNamespace(**base)


def reader(*, bars=((0, 300000), (300000, 600000)), series=None, constants=None,
           events=()):
    opens = [b[0] for b in bars]
    closes = [b[1] for b in bars]
    if series is None:
        series = [[101.5, 102.0], [1286557951.0, 4076222975.0], [1.0, 0.0]]
    if constants is None:
        constants = [50.0, 2021161215.0, math.nan]
    return SimpleNamespace(
        manifest=lambda: MANIFEST_BYTES,
        bar_times=lambda: (list(opens), list(closes)),
        series_count=lambda: len(series),
        series=lambda slot: list(series[slot]),
        constants=lambda: list(constants),
        events=lambda: list(events),
    )


class OutputsBlockTests(unittest.TestCase):
    def test_block_keys_in_order(self) -> None:
        block = run_json.build_outputs_block(reader())
        self.assertEqual(list(block), BLOCK_KEYS)
        self.assertEqual(block["schema_version"], "pineforge-outputs/v1")
        self.assertEqual(block["message_format"], "pineforge/v1")
        self.assertEqual(list(block["bars"]), ["open_ms", "close_ms"])

    def test_manifest_digest_is_over_the_raw_bytes(self) -> None:
        block = run_json.build_outputs_block(reader())
        self.assertEqual(block["manifest_sha256"], hashlib.sha256(MANIFEST_BYTES).hexdigest())
        self.assertNotEqual(block["manifest_sha256"], hashlib.sha256(
            json.dumps(MANIFEST, sort_keys=True, separators=(",", ":")).encode()).hexdigest())
        self.assertEqual(block["manifest"], MANIFEST)

    def test_series_entries_in_slot_order_with_their_encoding(self) -> None:
        block = run_json.build_outputs_block(reader())
        self.assertEqual([list(entry) for entry in block["series"]],
                         [["slot", "output", "values"]] * 3)
        self.assertEqual([entry["slot"] for entry in block["series"]], [0, 1, 2])
        self.assertEqual(block["series"][0], {"slot": 0, "output": "o0", "values": [101.5, 102.0]})
        colours = block["series"][1]["values"]
        self.assertEqual(colours, [1286557951, 4076222975])
        self.assertTrue(all(type(v) is int for v in colours))
        marks = block["series"][2]["values"]
        self.assertEqual(marks, [1.0, 0.0])
        self.assertTrue(all(type(v) is float for v in marks))

    def test_constants_and_hlines(self) -> None:
        block = run_json.build_outputs_block(reader())
        self.assertEqual(block["constants"], [50.0, 2021161215, None])
        self.assertIs(type(block["constants"][1]), int)
        self.assertEqual(block["hlines"], [{"output": "h0", "price": 50.0},
                                           {"output": "h1", "price": None}])
        self.assertEqual([list(h) for h in block["hlines"]], [["output", "price"]] * 2)

    def test_a_run_with_no_rows(self) -> None:
        block = run_json.build_outputs_block(reader(
            bars=(), series=[[], [], []], constants=[math.nan, math.nan, math.nan]))
        self.assertEqual(block["bars"], {"open_ms": [], "close_ms": []})
        self.assertEqual([entry["values"] for entry in block["series"]], [[], [], []])
        self.assertEqual(block["constants"], [None, None, None])
        self.assertEqual(block["hlines"], [{"output": "h0", "price": None},
                                           {"output": "h1", "price": None}])
        self.assertEqual(block["events"], [])

    def test_non_finite_doubles_are_null(self) -> None:
        block = run_json.build_outputs_block(reader(
            series=[[math.nan, math.inf], [math.nan, -math.inf], [-math.inf, 1.0]],
            constants=[math.inf, math.nan, -math.inf],
            events=[event(value=math.nan), event(sequence=2, value=math.inf)]))
        self.assertEqual([entry["values"] for entry in block["series"]],
                         [[None, None], [None, None], [None, 1.0]])
        self.assertEqual(block["constants"], [None, None, None])
        self.assertEqual([e["value"] for e in block["events"]], [None, None])
        json.dumps(block, allow_nan=False)

    def test_event_entries(self) -> None:
        events = [
            event(sequence=1, output_index=1, bar_index=0, value=1.0, phase=0),
            event(sequence=2, output_index=2, bar_index=1, bar_open_ms=300000,
                  bar_close_ms=INT64_MIN, ordinal_in_bar=1, phase=1, value=math.nan,
                  message=b"close=101.50", message_hash64=7),
            event(sequence=3, output_index=1, bar_index=1, bar_open_ms=INT64_MIN,
                  phase=2, confirmed=0),
        ]
        block = run_json.build_outputs_block(reader(events=events))
        first, second, third = block["events"]
        self.assertEqual(list(first), EVENT_KEYS)
        self.assertEqual(list(second), EVENT_KEYS + ["freq"])
        self.assertEqual(list(third), EVENT_KEYS)
        self.assertEqual(first, {"sequence": 1, "output": "o1", "bar_index": 0,
                                 "bar_open_ms": 0, "bar_close_ms": 300000,
                                 "ordinal_in_bar": 0, "phase": "batch", "value": 1.0,
                                 "message": None})
        self.assertEqual(second["output"], "o2")
        self.assertEqual(second["phase"], "warmup")
        self.assertEqual(second["message"], "close=101.50")
        self.assertIsNone(second["bar_close_ms"])
        self.assertIsNone(second["value"])
        self.assertEqual(second["freq"], "once_per_bar")
        self.assertEqual(third["phase"], "realtime")
        self.assertIsNone(third["bar_open_ms"])
        self.assertNotIn("confirmed", third)

    def test_mirror_layout_matches_the_c_header(self) -> None:
        # pf_output_event_v1_t; src/c_abi.cpp pins the same numbers.
        mirror = run_json.OutputEventC
        self.assertEqual(ctypes.sizeof(mirror), 80)
        self.assertEqual(mirror.size.offset, 4)
        self.assertEqual(mirror.sequence.offset, 8)
        self.assertEqual(mirror.value.offset, 56)
        self.assertEqual(mirror.message_hash64.offset, 64)
        self.assertEqual(mirror.message.offset, 72)

    def test_report_dict_writes_no_outputs_key(self) -> None:
        class Report:
            trades_len = 0
            equity_curve = None
            net_profit = 0.0
            input_bars_processed = 0
            script_bars_processed = 0
            magnifier_sub_bars_total = 0
            magnifier_sample_ticks_total = 0
            bar_magnifier_enabled = 0
            metrics = run_json.MetricsC()

        rep = run_json.build_report_dict(
            Report(), ohlcv_path="x.csv", n_bars=0, first_ts=0, last_ts=0,
            elapsed=0.0, applied_inputs={}, applied_overrides={}, applied_runtime={})
        self.assertNotIn("outputs", rep)
        self.assertNotIn("outputs", rep["applied_runtime"])


if __name__ == "__main__":
    unittest.main()
