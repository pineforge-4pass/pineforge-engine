#!/usr/bin/env python3
"""Exact corpus-only batch/stream/loopback equivalence; no venue connection.

Build pineforge-live with the CI-pinned curl first. Example:
  python3 tests/native_live_equivalence_e2e.py --build-dir build-live \
      --libraries-dir /tmp/equivalence-libs --out /tmp/equivalence-results

The observer is compiled once into each corpus strategy library. Qualification
compares reconstructed actions to the native accessor from the same stream run,
before comparing any runner output. All comparisons preserve physical order and
binary64 values. State-selected splits and completion filtering are restricted
to this contiguous 24x7 ETH tape, not shortened/session-based buckets.
Optional tick checks use explicitly synthetic O-H-L-C prints derived from the
shipped 1m corpus, not historical observed trades. They prove same-print replay
determinism across restart and exact 1m OHLCV reconstruction, not tick/batch fills.
Configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON and build all targets first;
the observer is a CMake OBJECT target, not a standalone test executable.
Runner scenarios aggregate prices to chart input and round minute splits down
to the preceding complete chart bar. One-minute source reconstruction remains
an independent direct-engine control.
"""

import argparse
from collections import Counter
import csv
import ctypes
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import shlex
import sqlite3
import struct
import subprocess
import sys
import threading
import traceback


PROBES = {
    "ta-rsi14-cross-50-01": {"market"},
    "order-keystone-limit-replace-01": {"limit"},
    "order-stop-entry-touch-boundary-01": {"stop"},
    "order-harbor-stop-limit-01": {"stop_limit"},
    "bracket-trailing-activation-offset-path-01": {"trail"},
    "pyramid-terrace-staged-entry-01": {"market"},
    "order-entry-implicit-reversal-exit-01": {"market"},
    "order-tranche-partial-market-close-01": {"market"},
}


class StreamAction(ctypes.Structure):
    _fields_ = [
        ("sequence", ctypes.c_uint64), ("timestamp_ms", ctypes.c_int64),
        ("bar_index", ctypes.c_int32), ("is_entry", ctypes.c_int32),
        ("is_long", ctypes.c_int32), ("quantity", ctypes.c_double),
        ("price", ctypes.c_double), ("order_id", ctypes.c_char_p),
        ("comment", ctypes.c_char_p), ("entry_incarnation", ctypes.c_uint64),
    ]


class TradeTick(ctypes.Structure):
    _fields_ = [("timestamp", ctypes.c_int64), ("sequence", ctypes.c_uint64),
                ("price", ctypes.c_double), ("quantity", ctypes.c_double)]


def live_action(origin, timeframe, split):
    # A script bucket not sealed in warmup is replayed in realtime.
    return origin - origin % timeframe + timeframe - 1 >= split


def chart_rows(rows, timeframe):
    if not rows or len(rows) % timeframe:
        raise ValueError("chart delivery requires complete input bars")
    start = int(rows[0]["timestamp"])
    if start % (timeframe * 60000) or any(int(row["timestamp"]) != start + index * 60000
                                         for index, row in enumerate(rows)):
        raise ValueError("chart delivery requires aligned, contiguous UTC minute prices")
    grouped = []
    for offset in range(0, len(rows), timeframe):
        children = rows[offset:offset + timeframe]
        volume = float(children[0]["volume"])
        for row in children[1:]:
            volume += float(row["volume"])
        grouped.append({"timestamp": int(children[0]["timestamp"]),
            "open": float(children[0]["open"]), "high": max(float(row["high"]) for row in children),
            "low": min(float(row["low"]) for row in children), "close": float(children[-1]["close"]),
            "volume": volume})
    return grouped


def chart_bar_array(strategy, rows):
    return (strategy.bar_type * len(rows))(*[strategy.bar_type(
        *[float(row[field]) for field in ("open", "high", "low", "close", "volume")],
        int(row["timestamp"])) for row in rows])


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def read_rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def scalar(value):
    if isinstance(value, ctypes.Structure):
        return {name: scalar(getattr(value, name)) for name, _ in value._fields_}
    if isinstance(value, float) and not math.isfinite(value):
        return {"binary64": struct.pack("!d", value).hex()}
    return value


def first_difference(expected, actual, path=""):
    if type(expected) is not type(actual):
        return {"path": path, "expected": expected, "actual": actual}
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            return {"path": path, "expected_keys": list(expected), "actual_keys": list(actual)}
        for key, value in expected.items():
            difference = first_difference(value, actual[key], f"{path}.{key}")
            if difference:
                return difference
    elif isinstance(expected, list):
        for index, (left, right) in enumerate(zip(expected, actual)):
            difference = first_difference(left, right, f"{path}[{index}]")
            if difference:
                return difference
        if len(expected) != len(actual):
            return {"path": path, "expected_length": len(expected), "actual_length": len(actual),
                    "expected_next": expected[len(actual):len(actual) + 1],
                    "actual_next": actual[len(expected):len(expected) + 1]}
    elif isinstance(expected, float):
        if struct.pack("!d", expected) != struct.pack("!d", actual):
            return {"path": path, "expected": expected, "actual": actual}
    elif expected != actual:
        return {"path": path, "expected": expected, "actual": actual}
    return None


def action_key(record):
    order = record["order"]
    return [record["timestamp"], record["bar_index"], order["action"], order["leg"],
            float(order["contracts"]), float(order["price"]), order["id"], order["entry_incarnation"],
            order["reduce_only"]]


class Strategy:
    def __init__(self, path, bar_type, report_type):
        self.path = path.resolve()
        self.library = ctypes.CDLL(str(self.path))
        self.bar_type = bar_type
        self.report_type = report_type
        signatures = {
            "pf_abi_version": ([], ctypes.c_int),
            "strategy_create": ([ctypes.c_char_p], ctypes.c_void_p),
            "strategy_free": ([ctypes.c_void_p], None),
            "strategy_get_last_error": ([ctypes.c_void_p], ctypes.c_char_p),
            "report_free": ([ctypes.POINTER(report_type)], None),
            "run_backtest_full": ([ctypes.c_void_p, ctypes.POINTER(bar_type), ctypes.c_int,
                ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                ctypes.POINTER(report_type)], None),
            "strategy_stream_begin": ([ctypes.c_void_p, ctypes.POINTER(bar_type), ctypes.c_int,
                ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            "strategy_stream_push_bar": ([ctypes.c_void_p, ctypes.POINTER(bar_type)], ctypes.c_int),
            "strategy_stream_push_ticks": ([ctypes.c_void_p, ctypes.POINTER(TradeTick), ctypes.c_int], ctypes.c_int),
            "strategy_stream_advance_time": ([ctypes.c_void_p, ctypes.c_int64], ctypes.c_int),
            "strategy_stream_fill_report": ([ctypes.c_void_p, ctypes.POINTER(report_type)], ctypes.c_int),
            "strategy_stream_state_hash": ([ctypes.c_void_p], ctypes.c_uint64),
            "strategy_stream_order_actions_len": ([ctypes.c_void_p], ctypes.c_int),
            "strategy_stream_order_action_get": ([ctypes.c_void_p, ctypes.c_int,
                ctypes.POINTER(StreamAction)], ctypes.c_int),
            "strategy_stream_order_actions_clear": ([ctypes.c_void_p], None),
            "strategy_position_size": ([ctypes.c_void_p], ctypes.c_double),
            "strategy_script_bars_processed": ([ctypes.c_void_p], ctypes.c_int64),
            "equivalence_retain_events": ([ctypes.c_void_p], ctypes.c_int),
            "equivalence_pending_priced": ([ctypes.c_void_p], ctypes.c_int),
            "equivalence_open_lots": ([ctypes.c_void_p], ctypes.c_int),
            "equivalence_source_bar": ([ctypes.c_void_p, ctypes.c_int64, ctypes.POINTER(bar_type)], ctypes.c_int),
            "equivalence_export_actions": ([ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            "equivalence_export_receipts": ([ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            "strategy_closed_trade_entry_incarnation": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_uint64),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.library, name)
            function.argtypes = arguments
            function.restype = result
        if self.library.pf_abi_version() != 4:
            raise RuntimeError("unsupported report ABI")
        self.sha256 = hashlib.sha256(self.path.read_bytes()).hexdigest()

    def create(self):
        handle = self.library.strategy_create(None)
        if not handle:
            raise RuntimeError("strategy_create failed")
        for name, value in [("strategy_set_syminfo_timezone", "UTC"),
                            ("strategy_set_chart_timezone", "UTC"),
                            ("strategy_set_syminfo_session", "24x7")]:
            setter = getattr(self.library, name)
            setter.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            setter(handle, value.encode())
        self.library.strategy_set_syminfo_string.argtypes = [ctypes.c_void_p,
            ctypes.c_char_p, ctypes.c_char_p]
        for key, value in [("tickerid", "BINANCE:ETHUSDT.P"), ("ticker", "ETHUSDT.P"),
                           ("type", "crypto"), ("currency", "USDT"), ("basecurrency", "ETH")]:
            self.check(self.library.strategy_set_syminfo_string(handle, key.encode(), value.encode()))
        for name, value in [("strategy_set_syminfo_mintick", 0.01),
                            ("strategy_set_syminfo_pointvalue", 1.0)]:
            setter = getattr(self.library, name)
            setter.argtypes = [ctypes.c_void_p, ctypes.c_double]
            setter(handle, value)
        self.library.strategy_set_syminfo_metadata.argtypes = [ctypes.c_void_p,
            ctypes.c_char_p, ctypes.c_double]
        self.library.strategy_set_syminfo_metadata(handle, b"qty_step", 0.001)
        return handle

    def check(self, result):
        if result != 0:
            raise RuntimeError(f"strategy operation returned {result}")

    def snapshot(self, handle, report):
        closed = []
        terminal = []
        for index in range(report.trades_len):
            trade = report.trades[index]
            record = scalar(trade)
            if trade.open_at_end:
                terminal.append(record)
                continue
            for field in ("entry_id", "exit_id", "exit_comment"):
                accessor = getattr(self.library, "strategy_closed_trade_" + field)
                accessor.argtypes = [ctypes.c_void_p, ctypes.c_int]
                accessor.restype = ctypes.c_char_p
                record[field] = (accessor(handle, index) or b"").decode()
            record["entry_incarnation"] = self.library.strategy_closed_trade_entry_incarnation(handle, index)
            closed.append(record)
        totals = {}
        for name, field_type in report._fields_:
            if field_type in (ctypes.c_int, ctypes.c_int64, ctypes.c_double):
                totals[name] = scalar(getattr(report, name))
        totals.pop("broker_state_hash_len", None)
        return {"totals": totals, "closed_trades": closed, "terminal_trades": terminal,
                "equity_curve": [scalar(report.equity_curve[index])
                                 for index in range(report.equity_curve_len)],
                "metrics": scalar(report.metrics)}

    def batch(self, bars, timeframe, output, input_tf=1, distribution=0):
        output.mkdir(parents=True)
        handle = self.create()
        report = self.report_type()
        try:
            self.check(self.library.equivalence_retain_events(handle))
            self.library.run_backtest_full(handle, bars, len(bars), str(input_tf).encode(), str(timeframe).encode(),
                0, 4, distribution, ctypes.byref(report))
            error = self.library.strategy_get_last_error(handle)
            if error:
                raise RuntimeError(error.decode())
            for name in ("actions", "receipts"):
                path = output / f"{name}.jsonl"
                self.check(getattr(self.library, "equivalence_export_" + name)(handle, str(path).encode()))
            state = self.snapshot(handle, report)
            write_json(output / "state.json", state)
            return {"state": state, "actions": read_rows(output / "actions.jsonl"),
                    "receipts": read_rows(output / "receipts.jsonl")}
        finally:
            self.library.report_free(ctypes.byref(report))
            self.library.strategy_free(handle)

    def stream(self, bars, split, timeframe, sample=False, observer_output=None, observe_bars=False, input_tf=1):
        handle = self.create()
        report = self.report_type()
        actions = []
        hashes = []
        selected = {}
        maximum_lots = 0
        source_bars = []
        try:
            if observer_output is not None:
                observer_output.mkdir(parents=True)
                self.check(self.library.equivalence_retain_events(handle))
            self.check(self.library.strategy_stream_begin(handle, bars, split, str(input_tf).encode(), str(timeframe).encode()))
            for index in range(split, len(bars)):
                self.check(self.library.strategy_stream_push_bar(handle, ctypes.byref(bars[index])))
                if observe_bars:
                    source_bars.append(self.source_bar(handle, timeframe))
                for offset in range(self.library.strategy_stream_order_actions_len(handle)):
                    action = StreamAction()
                    self.check(self.library.strategy_stream_order_action_get(handle, offset, ctypes.byref(action)))
                    actions.append({"timestamp": action.timestamp_ms, "bar_index": action.bar_index,
                        "order": {"action": "buy" if action.is_entry == action.is_long else "sell",
                            "leg": "entry" if action.is_entry else "exit", "contracts": action.quantity,
                            "price": action.price, "id": (action.order_id or b"").decode(),
                            "entry_incarnation": action.entry_incarnation, "reduce_only": not bool(action.is_entry)}})
                self.library.strategy_stream_order_actions_clear(handle)
                hashes.append(str(self.library.strategy_stream_state_hash(handle)))
                if sample:
                    maximum_lots = max(maximum_lots, self.library.equivalence_open_lots(handle))
                    position = self.library.strategy_position_size(handle)
                    priced = self.library.equivalence_pending_priced(handle)
                    if priced < 0:
                        raise RuntimeError("working request observation failed")
                    warmed = self.library.strategy_script_bars_processed(handle) >= 1
                    if warmed and index + 1 < len(bars):
                        for name, condition in (("open_position", position != 0), ("working_priced", priced > 0)):
                            if condition and name not in selected:
                                selected[name] = {"split": index + 1, "input_index": index,
                                    "timestamp": bars[index].timestamp, "position": position,
                                    "working_priced_count": priced}
            before = self.library.strategy_stream_state_hash(handle)
            self.check(self.library.strategy_stream_fill_report(handle, ctypes.byref(report)))
            if before != self.library.strategy_stream_state_hash(handle):
                raise RuntimeError("report read mutated continuation")
            result = {"state": self.snapshot(handle, report), "actions": actions,
                      "hashes": hashes, "selected_splits": selected, "maximum_lots": maximum_lots,
                      "source_bars": source_bars}
            if observer_output is not None:
                for name in ("actions", "receipts"):
                    path = observer_output / f"{name}.jsonl"
                    self.check(getattr(self.library, "equivalence_export_" + name)(handle, str(path).encode()))
                    result["observed_" + name] = read_rows(path)
            return result
        finally:
            self.library.report_free(ctypes.byref(report))
            self.library.strategy_free(handle)

    def source_bar(self, handle, timeframe):
        result = self.bar_type()
        before = self.library.strategy_stream_state_hash(handle)
        self.check(self.library.equivalence_source_bar(handle, timeframe * 60000, ctypes.byref(result)))
        if before != self.library.strategy_stream_state_hash(handle):
            raise RuntimeError("source bar read mutated continuation")
        return scalar(result)

    def drain_actions(self, handle):
        actions = []
        for offset in range(self.library.strategy_stream_order_actions_len(handle)):
            action = StreamAction()
            self.check(self.library.strategy_stream_order_action_get(handle, offset, ctypes.byref(action)))
            actions.append({"timestamp": action.timestamp_ms, "bar_index": action.bar_index,
                "order": {"action": "buy" if action.is_entry == action.is_long else "sell",
                    "leg": "entry" if action.is_entry else "exit", "contracts": action.quantity,
                    "price": action.price, "id": (action.order_id or b"").decode(),
                    "entry_incarnation": action.entry_incarnation, "reduce_only": not bool(action.is_entry)}})
        self.library.strategy_stream_order_actions_clear(handle)
        return actions

    def read_report(self, handle):
        report = self.report_type()
        before = self.library.strategy_stream_state_hash(handle)
        try:
            self.check(self.library.strategy_stream_fill_report(handle, ctypes.byref(report)))
            if before != self.library.strategy_stream_state_hash(handle):
                raise RuntimeError("report read mutated continuation")
            return self.snapshot(handle, report)
        finally:
            self.library.report_free(ctypes.byref(report))

    def tick_stream(self, bars, split, timeframe, packets, restart_at=0, observe_bars=False, input_tf=1):
        handle = self.create()
        actions = []
        hashes = []
        source_bars = []
        restart_receipt = None

        def begin(current):
            self.check(self.library.strategy_stream_begin(current, bars, split, str(input_tf).encode(), str(timeframe).encode()))

        def push(current, packet):
            if packet["type"] == "tick":
                tick = TradeTick(packet["ts"], packet["seq"], packet["price"], packet["qty"])
                self.check(self.library.strategy_stream_push_ticks(current, ctypes.byref(tick), 1))
            else:
                self.check(self.library.strategy_stream_advance_time(current, packet["ts"]))
            emitted = self.drain_actions(current)
            digest = str(self.library.strategy_stream_state_hash(current))
            bar = self.source_bar(current, timeframe) if observe_bars and packet["type"] == "time" else None
            return emitted, digest, bar

        try:
            begin(handle)
            for index, packet in enumerate(packets):
                emitted, digest, bar = push(handle, packet)
                actions.extend(emitted)
                hashes.append(digest)
                if bar is not None:
                    source_bars.append(bar)
                if restart_at and index + 1 == restart_at:
                    state = self.read_report(handle)
                    self.library.strategy_free(handle)
                    handle = self.create()
                    begin(handle)
                    replay_actions = []
                    replay_hashes = []
                    for replay_packet in packets[:restart_at]:
                        repeated_actions, repeated_hash, _ = push(handle, replay_packet)
                        replay_actions.extend(repeated_actions)
                        replay_hashes.append(repeated_hash)
                    differences = {"actions": first_difference(actions, replay_actions),
                        "hashes": first_difference(hashes, replay_hashes),
                        "report": first_difference(state, self.read_report(handle))}
                    restart_receipt = {"prefix_events": restart_at, "differences": differences}
                    if any(differences.values()):
                        raise RuntimeError("tick prefix restart divergence: " + json.dumps(restart_receipt))
            return {"state": self.read_report(handle), "actions": actions, "hashes": hashes,
                    "source_bars": source_bars, "restart_receipt": restart_receipt}
        finally:
            self.library.strategy_free(handle)


class MockReceiver:
    def __init__(self, output, fail_first=False, restart_action=0):
        self.fail_first = fail_first
        self.restart_action = restart_action
        self.accepted = threading.Event()
        self.release_ack = threading.Event()
        self.lock = threading.Lock()
        self.errors = []
        self.database = sqlite3.connect(output / "receiver.sqlite3", check_same_thread=False)
        self.database.execute("PRAGMA journal_mode=WAL")
        self.database.execute("PRAGMA synchronous=FULL")
        self.database.execute("CREATE TABLE attempts (ordinal INTEGER PRIMARY KEY, event_id TEXT, body BLOB, idempotency_header TEXT, event_header TEXT, status INTEGER)")
        self.database.execute("CREATE TABLE effects (ordinal INTEGER PRIMARY KEY, event_id TEXT UNIQUE, body BLOB)")
        owner = self

        class Receiver(BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers["Content-Length"]))
                    payload = json.loads(body)
                    event_id = payload["event_id"]
                    idempotency_header = self.headers.get("Idempotency-Key")
                    event_header = self.headers.get("X-PineForge-Event-Id")
                    if event_id != event_header or idempotency_header != payload.get("delivery_id", event_id):
                        raise RuntimeError("idempotency header/payload mismatch")
                    with owner.lock:
                        prior = owner.database.execute("SELECT body FROM attempts WHERE event_id=?", (event_id,)).fetchall()
                        if any(previous[0] != body for previous in prior):
                            raise RuntimeError("retry body changed")
                        rejected = owner.fail_first and not prior
                        owner.database.execute("INSERT INTO attempts VALUES(NULL,?,?,?,?,?)",
                            (event_id, body, idempotency_header, event_header, 503 if rejected else 200))
                        is_new = not owner.database.execute("SELECT 1 FROM effects WHERE event_id=?", (event_id,)).fetchone()
                        if not rejected:
                            owner.database.execute("INSERT OR IGNORE INTO effects VALUES(NULL,?,?)", (event_id, body))
                        owner.database.commit()
                        count = owner.database.execute("SELECT COUNT(*) FROM effects").fetchone()[0]
                        wait_ack = not rejected and is_new and count == owner.restart_action
                    if wait_ack:
                        owner.accepted.set()
                        owner.release_ack.wait(120)
                    self.send_response(503 if rejected else 200)
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception:
                    owner.errors.append(traceback.format_exc())
                    self.send_response(500)
                    self.end_headers()

            def log_message(self, *unused):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Receiver)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.routes = output / "routes.json"
        write_json(self.routes, {"schema_version": 1, "default_target": "default",
            "targets": {"default": {"url": f"http://127.0.0.1:{self.server.server_address[1]}/actions",
                                    "secret_env": ""}}, "rules": [],
            "delivery": {"max_in_flight": 1, "transport_retries": 2,
                         "retry_backoff_ms": [10, 20]}})

    def finish(self, output):
        self.release_ack.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        attempts = [{"event_id": event_id, "body": body.decode(), "idempotency_header": header,
                     "event_header": event_header, "status": status}
                    for event_id, body, header, event_header, status in self.database.execute(
                        "SELECT event_id,body,idempotency_header,event_header,status FROM attempts ORDER BY ordinal")]
        effects = [json.loads(body) for (body,) in self.database.execute("SELECT body FROM effects ORDER BY ordinal")]
        self.database.close()
        write_json(output / "attempts.json", attempts)
        write_json(output / "effects.json", effects)
        return attempts, effects


def tape_files(output, rows, split):
    with (output / "warmup.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows[:split])
    with (output / "tail.jsonl").open("w") as destination:
        for row in rows[split:]:
            destination.write(json.dumps({"type": "bar", "bar": {"ts_open": int(row["timestamp"]),
                "o": float(row["open"]), "h": float(row["high"]), "l": float(row["low"]),
                "c": float(row["close"]), "v": float(row["volume"])}}, separators=(",", ":")) + "\n")


def synthetic_prints(rows, split):
    packets = []
    sequence = 0
    for row in rows[split:]:
        timestamp = int(row["timestamp"])
        volume = float(row["volume"])
        if volume <= 0:
            raise ValueError("synthetic prints require positive corpus volume")
        quarter = volume * 0.25
        quantities = [quarter, quarter, quarter, volume - ((quarter + quarter) + quarter)]
        for index, (field, offset) in enumerate((("open", 0), ("high", 20000), ("low", 40000), ("close", 59999))):
            sequence += 1
            packets.append({"type": "tick", "ts": timestamp + offset, "seq": sequence,
                            "price": float(row[field]), "qty": quantities[index]})
        packets.append({"type": "time", "ts": timestamp + 60000})
    return packets


def run_case(strategy, batch, bars, rows, timeframe, split, output, runner,
             scenario, fail_first=False, restart=False, packets=None, modeled=None):
    output.mkdir(parents=True)
    result = {"scenario": scenario, "script_tf": timeframe, "split": split,
              "library_sha256": strategy.sha256, "fail_first": fail_first, "restart": restart}
    process = None
    receiver = None
    try:
        rows = chart_rows(rows, timeframe)
        bars = chart_bar_array(strategy, rows)
        minute_split = split
        split //= timeframe
        result.update(input_tf=timeframe, minute_split=minute_split, split=split, distribution=3)
        if packets is None:
            batch = strategy.batch(bars, timeframe, output / "chart-batch", input_tf=timeframe, distribution=3)
            modeled = strategy.stream(bars, split, timeframe, input_tf=timeframe)
        else:
            if minute_split % timeframe:
                raise ValueError("tick warmup must end on a complete chart bar")
            modeled = strategy.tick_stream(bars, split, timeframe, packets, input_tf=timeframe)
            batch = {"state": modeled["state"],
                     "actions": [dict(row, origin_input_index=split) for row in modeled["actions"]]}
        write_json(output / "forward-state.json", modeled["state"])
        expected = [action_key(row) for row in batch["actions"]
                    if live_action(row["origin_input_index"], 1, split)]
        result["modeled_action_difference"] = first_difference(expected, [action_key(row) for row in modeled["actions"]])
        result["report_difference"] = first_difference(batch["state"], modeled["state"])
        tape_files(output, rows, split)
        if packets is not None:
            result["tape_kind"] = "synthetic prints"
            with (output / "tail.jsonl").open("w") as destination:
                for packet in packets:
                    destination.write(json.dumps(packet, separators=(",", ":")) + "\n")
        crash_action = min(3, len(expected)) if restart else 0
        if not expected:
            raise RuntimeError("NOT EXERCISED: no live actions")
        receiver = MockReceiver(output, fail_first, crash_action)
        result["delivery_policy"] = "single-flight, two explicit transport retries"
        command = [str(runner), "run", "--strategy", str(strategy.path), "--warmup", str(output / "warmup.csv"),
            "--input-tf", str(timeframe), "--script-tf", str(timeframe), "--session", "24x7", "--timezone", "UTC",
            "--chart-timezone", "UTC", "--mode", "ticks" if packets is not None else "bars",
            "--poll-ms", "100",
            "--feed", str(output / "tail.jsonl"),
            "--ledger", str(output / "orders.sqlite3"), "--symbol", "BINANCE:ETHUSDT.P", "--name", strategy.path.stem,
            "--syminfo", "type=crypto", "--syminfo", "currency=USDT", "--syminfo", "basecurrency=ETH",
            "--syminfo", "mintick=0.01", "--syminfo", "pointvalue=1", "--syminfo", "qty_step=0.001",
            "--webhook-routes", str(receiver.routes), "--allow-insecure-http"]
        write_json(output / "command.json", command)
        with (output / "runner.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            if restart:
                if not receiver.accepted.wait(120):
                    raise RuntimeError("accepted-effect crash window not reached")
                process.kill()
                killed_returncode = process.wait(timeout=10)
                if killed_returncode != -9:
                    raise RuntimeError(f"runner was not SIGKILLed: {killed_returncode}")
                result["crash_receipt"] = {"accepted_effect": crash_action, "returncode": killed_returncode,
                    "ack_withheld": True, "same_command": True, "same_ledger": True}
                receiver.release_ack.set()
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            result["runner_returncode"] = process.wait(timeout=600)
        attempts, effects = receiver.finish(output)
        errors = receiver.errors
        receiver = None
        if errors:
            raise RuntimeError("receiver failure: " + errors[0])
        actual = [action_key(row) for row in effects]
        result["action_difference"] = first_difference(expected, actual)
        result["actions"] = len(effects)
        result["expected_actions"] = len(expected)
        result["attempts"] = len(attempts)
        result["closed_trades"] = len(modeled["state"]["closed_trades"])
        result["equity_points"] = len(modeled["state"]["equity_curve"])
        sequences = [row["sequence"] for row in effects]
        if sequences != list(range(1, len(effects) + 1)):
            raise RuntimeError("effect sequence gap/reordering")
        if len({row["event_id"] for row in effects}) != len(effects):
            raise RuntimeError("duplicate logical effect")
        if fail_first:
            rejected = Counter(row["event_id"] for row in attempts if row["status"] == 503)
            if set(rejected) != {row["event_id"] for row in effects} or any(count != 1 for count in rejected.values()):
                raise RuntimeError("fail-first coverage missing")
        if restart and len([row for row in attempts if row["event_id"] == effects[crash_action - 1]["event_id"]]) < 2:
            raise RuntimeError("accepted-but-ack-lost action was not replayed")
        with sqlite3.connect(output / "orders.sqlite3") as ledger:
            ledger_hashes = [row[0] for row in ledger.execute("SELECT state_hash FROM inputs ORDER BY input_index")]
            pending = ledger.execute("SELECT COUNT(*) FROM events WHERE acknowledged=0").fetchone()[0]
        result["replay_hash_difference"] = first_difference(modeled["hashes"], ledger_hashes)
        result["committed_inputs"] = len(ledger_hashes)
        result["pending_events"] = pending
        write_json(output / "comparison.json", {"expected": expected, "actual": actual})
        differences = [result.get(name) for name in ("modeled_action_difference", "action_difference",
            "report_difference", "replay_hash_difference")]
        result["status"] = "PASS" if not any(differences) and result["runner_returncode"] == 0 and pending == 0 else "FAIL"
    except Exception as error:
        result["status"] = "FAIL"
        result["error"] = str(error)
        (output / "exception.log").write_text(traceback.format_exc())
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        if receiver is not None:
            receiver.finish(output)
        write_json(output / "result.json", result)
    print(f'{result["status"]} case={strategy.path.stem} scenario={scenario} tf={timeframe} split={split} '
          f'actions={result.get("actions", 0)}/{result.get("expected_actions", 0)} '
          f'trades={result.get("closed_trades", 0)} points={result.get("equity_points", 0)}', flush=True)
    return result


def compile_library(root, build, libraries, probe):
    libraries.mkdir(parents=True, exist_ok=True)
    destination = libraries / (probe + ".so")
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(row for row in entries
                 if Path(row["file"]).resolve() == root / "tests/native_live_equivalence_observer.cpp")
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    flags = []
    object_file = None
    cursor = 0
    while cursor < len(arguments):
        argument = arguments[cursor]
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            if argument == "-o":
                object_file = Path(entry["directory"]) / arguments[cursor + 1]
            cursor += 2
            continue
        if argument not in ("-c", "-MD", "-MMD") and argument != entry["file"]:
            flags.append(argument)
        cursor += 1
    if object_file is None or not object_file.is_file():
        raise RuntimeError("build the native_live_equivalence_observer CMake target first")
    archive = str(build / "lib/libpineforge.a")
    if sys.platform == "darwin":
        link = ["-dynamiclib", "-Wl,-force_load," + archive]
    elif sys.platform.startswith("linux"):
        link = ["-shared", "-Wl,--whole-archive", archive, "-Wl,--no-whole-archive"]
    else:
        raise RuntimeError("observer linking supports native Linux and macOS builds")
    command = flags + [str(root / "corpus/validation" / probe / "generated.cpp"),
                       str(object_file)] + link + ["-o", str(destination)]
    write_json(libraries / (probe + "-compile.json"), command)
    with (libraries / (probe + "-compile.log")).open("w") as log:
        subprocess.run(command, cwd=entry["directory"], stdout=log, stderr=subprocess.STDOUT, check=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=Path("build-live"))
    parser.add_argument("--libraries-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--start", default="2025-04-01T00:00:00+00:00")
    parser.add_argument("--count", type=int, default=11520)
    parser.add_argument("--probes", nargs="+", choices=list(PROBES), default=list(PROBES))
    parser.add_argument("--timeframes", nargs="+", type=int, default=[15, 5, 1])
    parser.add_argument("--reuse-libraries", action="store_true")
    parser.add_argument("--qualification-only", action="store_true")
    parser.add_argument("--ticks", action="store_true", help="check explicitly synthetic-print replay and OHLCV reconstruction")
    parser.add_argument("--ticks-only", action="store_true", help="qualify observers and run only the tick checks")
    parser.add_argument("--tick-tape", type=Path, help="local genuine aggTrades CSV with a checked manifest")
    parser.add_argument("--tick-manifest", type=Path)
    parser.add_argument("--venue-klines", type=Path)
    parser.add_argument("--tick-scenarios", nargs="+")
    parser.add_argument("--tick-reference-only", action="store_true")
    parser.add_argument("--tick-rb-only", action="store_true", help="offline bar/path checks without claiming runner qualification")
    parser.add_argument("--tick-batch-only", action="store_true", help="supplemental large-tape runner checks at 1024-event message boundaries")
    arguments = parser.parse_args()
    if arguments.tick_tape:
        from native_live_tick_tape import run_genuine_tape
        return run_genuine_tape(arguments)
    root = arguments.root.resolve()
    build = arguments.build_dir.resolve()
    output = arguments.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(root / "scripts"))
    from run_strategy import BarC, ReportC
    start = int(datetime.fromisoformat(arguments.start).astimezone(timezone.utc).timestamp() * 1000)
    rows = []
    feed = root / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv"
    with feed.open() as source:
        for row in csv.DictReader(source):
            if int(row["timestamp"]) < start:
                continue
            rows.append(row)
            if len(rows) == arguments.count:
                break
    if len(rows) != arguments.count or any(int(row["timestamp"]) != start + index * 60000 for index, row in enumerate(rows)):
        raise RuntimeError("tape must contain the requested number of contiguous genuine 1m bars")
    if arguments.count <= 1454 or any(arguments.count % timeframe for timeframe in arguments.timeframes):
        raise RuntimeError("tape must cover all splits and end on a complete script bar")
    bars = (BarC * len(rows))(*[BarC(float(row["open"]), float(row["high"]), float(row["low"]),
        float(row["close"]), float(row["volume"]), int(row["timestamp"])) for row in rows])
    write_json(output / "manifest.json", {"feed": str(feed), "start_ms": start,
        "last_ms": int(rows[-1]["timestamp"]), "count": len(rows), "synthesized": False,
        "tape_sha256": hashlib.sha256(json.dumps(rows, separators=(",", ":")).encode()).hexdigest(),
        "symbol": "BINANCE:ETHUSDT.P", "input_tf": 1, "script_tfs": arguments.timeframes,
        "runner_sha256": hashlib.sha256((build / "bin/pineforge-live").read_bytes()).hexdigest()})
    strategies = {}
    batches = {}
    qualifications = {}
    samplers = {}
    results = []
    for probe in arguments.probes:
        path = arguments.libraries_dir.resolve() / (probe + ".so")
        if not arguments.reuse_libraries:
            path = compile_library(root, build, arguments.libraries_dir.resolve(), probe)
        strategies[probe] = Strategy(path, BarC, ReportC)
        batch = strategies[probe].batch(bars, 15, output / probe / "batch-tf15")
        batches[(probe, 15)] = batch
        sampler = strategies[probe].stream(bars, 1, 15, sample=True,
                                         observer_output=output / probe / "stream-observer")
        samplers[probe] = sampler
        observed = [row for row in sampler["observed_actions"] if row["origin_input_index"] >= 1]
        difference = first_difference([action_key(row) for row in observed],
                                      [action_key(row) for row in sampler["actions"]])
        product_difference = first_difference([action_key(row) for row in batch["actions"]],
                                              [action_key(row) for row in sampler["actions"]])
        executed = Counter(row["type"] for row in sampler["observed_receipts"] if row["kind"] == "executed")
        missing = sorted(PROBES[probe] - set(executed))
        opening_quantities = {row["order"]["entry_incarnation"]: row["order"]["contracts"]
                              for row in observed if row["order"]["leg"] == "entry"}
        partial_close = any(row["order"]["leg"] == "exit"
                            and row["order"]["contracts"] < opening_quantities.get(row["order"]["entry_incarnation"], 0)
                            for row in observed)
        if probe == "order-tranche-partial-market-close-01" and not partial_close:
            missing.append("partial_close")
        if probe == "pyramid-terrace-staged-entry-01" and sampler["maximum_lots"] < 2:
            missing.append("simultaneous_lots")
        if probe == "bracket-trailing-activation-offset-path-01" and not any(
                row["kind"] == "activated" and row["type"] == "trail" for row in sampler["observed_receipts"]):
            missing.append("trailing_activation")
        qualifications[probe] = {"status": "PASS" if not difference and not missing else "FAIL",
            "observer_action_difference": difference, "batch_stream_action_difference": product_difference,
            "executed_types": dict(executed), "partial_close_exercised": partial_close,
            "missing_types": missing, "maximum_lots": sampler["maximum_lots"],
            "selected_splits": sampler["selected_splits"], "actions": len(sampler["actions"]),
            "library_sha256": strategies[probe].sha256}
        write_json(output / probe / "qualification.json", qualifications[probe])
        print(f'{qualifications[probe]["status"]} observer={probe} actions={len(sampler["actions"])} '
              f'types={dict(executed)} max_lots={sampler["maximum_lots"]}', flush=True)
    write_json(output / "qualifications.json", qualifications)
    if arguments.qualification_only:
        failed_qualification = sum(row["status"] != "PASS" for row in qualifications.values())
        print(f'OBSERVER_SUMMARY probes={len(arguments.probes)} failed={failed_qualification}', flush=True)
        return 1 if failed_qualification else 0
    for probe in ([] if arguments.ticks_only else arguments.probes):
        strategy = strategies[probe]
        for timeframe in arguments.timeframes:
            if (probe, timeframe) not in batches:
                batches[(probe, timeframe)] = strategy.batch(bars, timeframe, output / probe / f"batch-tf{timeframe}")
            batch = batches[(probe, timeframe)]
            for split in (1440, 1447, 1454):
                results.append({"probe": probe, **run_case(strategy, batch, bars, rows, timeframe, split,
                    output / probe / f"tf{timeframe}-split{split}", build / "bin/pineforge-live", "bars")})
        batch = batches[(probe, 15)]
        for scenario, restart, fail_first in (("restart", True, False), ("fail-first", False, True),
                                              ("restart-fail-first", True, True)):
            results.append({"probe": probe, **run_case(strategy, batch, bars, rows, 15, 1447,
                output / probe / scenario, build / "bin/pineforge-live", scenario, fail_first, restart)})
        for name, receipt in samplers[probe]["selected_splits"].items():
            results.append({"probe": probe, **run_case(strategy, batch, bars, rows, 15, receipt["split"],
                output / probe / name, build / "bin/pineforge-live", name)})
        write_json(output / "results.json", results)
    tick_results = []
    if arguments.ticks or arguments.ticks_only:
        split = 1440
        packets = synthetic_prints(rows, split)
        write_json(output / "tick-manifest.json", {"tape_kind": "synthetic prints", "seconds_feed": None,
            "source_feed": str(feed), "source_bars": len(rows) - split, "split": split,
            "construction": "O-H-L-C at offsets 0/20000/40000/59999ms; positive quarter volumes with final exact residual; explicit 1m completeness",
            "tick_count": sum(packet["type"] == "tick" for packet in packets),
            "packet_sha256": hashlib.sha256(json.dumps(packets, separators=(",", ":")).encode()).hexdigest()})
        for probe in ("ta-rsi14-cross-50-01", "order-keystone-limit-replace-01"):
            if probe not in strategies:
                continue
            strategy = strategies[probe]
            tick_output = output / probe / "tick-reference"
            tick_output.mkdir(parents=True)
            try:
                modeled = strategy.tick_stream(bars, split, 15, packets)
                recovered = strategy.tick_stream(bars, split, 15, packets, restart_at=len(packets) // 3)
                determinism = {name: first_difference(modeled[name], recovered[name]) for name in ("actions", "hashes", "state")}
                from_ticks = strategy.tick_stream(bars, split, 1, packets, observe_bars=True)["source_bars"]
                from_bars = strategy.stream(bars, split, 1, observe_bars=True)["source_bars"]
                expected_bars = [scalar(bars[index]) for index in range(split, len(bars))]
                receipt = {"tape_kind": "synthetic prints", "determinism": determinism,
                    "prefix_restart": recovered["restart_receipt"], "bars": len(from_ticks),
                    "tick_bar_difference": first_difference(expected_bars, from_ticks),
                    "direct_bar_difference": first_difference(expected_bars, from_bars),
                    "tick_vs_direct_bar_difference": first_difference(from_bars, from_ticks)}
                write_json(tick_output / "reference.json", modeled)
                write_json(tick_output / "restarted.json", recovered)
                write_json(tick_output / "tick-built-bars.json", from_ticks)
                write_json(tick_output / "bar-pushed-bars.json", from_bars)
                write_json(tick_output / "receipt.json", receipt)
            except Exception as error:
                result = {"probe": probe, "scenario": "tick-reference", "status": "FAIL",
                    "tape_kind": "synthetic prints", "error": str(error)}
                tick_results.append(result)
                write_json(tick_output / "result.json", result)
                (tick_output / "exception.log").write_text(traceback.format_exc())
                write_json(output / "tick-results.json", tick_results)
                print(f'FAIL tick={probe} reference={error} tape="synthetic prints"', flush=True)
                continue
            reference = {"state": modeled["state"], "actions": [dict(row, origin_input_index=split) for row in modeled["actions"]]}
            for scenario, restart, fail_first in (("ticks", False, False), ("ticks-restart", True, False),
                                                 ("ticks-restart-fail-first", True, True)):
                result = {"probe": probe, **run_case(strategy, reference, bars, rows, 15, split,
                    output / probe / scenario, build / "bin/pineforge-live", scenario, fail_first, restart,
                    packets=packets, modeled=modeled)}
                result["tick_receipt"] = receipt
                differences = list(determinism.values()) + [receipt[name] for name in
                    ("tick_bar_difference", "direct_bar_difference", "tick_vs_direct_bar_difference")]
                if any(differences):
                    result["status"] = "FAIL"
                write_json(output / probe / scenario / "result.json", result)
                tick_results.append(result)
                print(f'{result["status"]} tick={probe} scenario={scenario} '
                      f'bars={receipt["bars"]} tape="synthetic prints"', flush=True)
            write_json(output / "tick-results.json", tick_results)
        print(f'TICK_EQUIVALENCE_SUMMARY scenarios={len(tick_results)} '
              f'failed={sum(result["status"] != "PASS" for result in tick_results)} tape="synthetic prints"', flush=True)
    failed = sum(result["status"] != "PASS" for result in results)
    failed_qualification = sum(row["status"] != "PASS" for row in qualifications.values())
    print(f'EQUIVALENCE_SUMMARY probes={len(arguments.probes)} scenarios={len(results)} '
          f'failed={failed} observer_failed={failed_qualification}', flush=True)
    return 1 if failed or failed_qualification or any(result["status"] != "PASS" for result in tick_results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
