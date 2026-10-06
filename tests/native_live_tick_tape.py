"""Genuine recorded-print qualification. All data and results remain local.

The historical batch API is not a tick reference. Array and single-print
continuations use the same public realtime ingress. Hashes are compared at
provider-message boundaries, exactly as the runner's atomic ledger does.
Runner and tick references use chart input and chart warmup; the separate
one-minute reconstruction control retains its original input clock.
"""

import base64
from collections import Counter
import copy
import csv
from decimal import Decimal
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import struct
import subprocess
import sys
import threading
import time
import traceback

from native_live_equivalence_e2e import (
    MockReceiver, Strategy, TradeTick, action_key, chart_bar_array, chart_rows, compile_library,
    first_difference, read_rows, write_json,
)
from native_live_tick_oracle import classify_first_divergence


SCENARIOS = (
    "file-single", "file-batch", "stdin-single", "stdin-batch",
    "http-single", "http-batch", "ws-single", "ws-batch",
    "restart-batch", "retry-batch", "combined-batch",
)
TIMESTAMP_CONTRACT = {
    "physical_R_A": "No mapping: physical action milliseconds and every message hash are exact.",
    "R_B": "Only action.timestamp and closed/terminal trade entry_time/exit_time are mapped to floor(ts/900000)*900000. The historical script-bar open labels the modeled fill; an observed fill carries print milliseconds. All prices, quantities, order metadata, other trade fields, curve points and metrics remain bitwise exact.",
    "magnifier": "off; run_backtest_full(..., 0, 4, 0, ...)",
    "sealing": "A time event at each next 1m boundary seals the preceding minute; final time seals the tape's last minute.",
}


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def csv_rows(path):
    with path.open(newline="") as source:
        yield from csv.DictReader(source)


def load_tick_tape(path, manifest_path=None):
    manifest_path = manifest_path or Path(str(path) + ".manifest.json")
    manifest = json.loads(manifest_path.read_text())
    for field in ("source", "sha256", "first_id", "last_id", "count", "start_ms", "end_ms"):
        if field not in manifest:
            raise ValueError("tick manifest missing " + field)
    if manifest["sha256"] != file_digest(path):
        raise ValueError("tick tape SHA256 mismatch")
    if "archive_file" in manifest:
        archive = path.parent / manifest["archive_file"]
        checksum = Path(str(archive) + ".CHECKSUM").read_text().split()[0]
        if checksum != manifest["archive_sha256"] or checksum != file_digest(archive):
            raise ValueError("archive does not match its supplied CHECKSUM")
    previous = None
    previous_timestamp = None
    archive_first = None
    archive_count = 0
    packets = []
    aggregates = {}
    for row in csv_rows(path):
        timestamp = int(row["transact_time"])
        sequence = int(row["agg_trade_id"])
        price = Decimal(row["price"])
        quantity = Decimal(row["quantity"])
        if not price.is_finite() or price <= 0 or not quantity.is_finite() or quantity <= 0:
            raise ValueError("nonpositive or nonfinite print at id " + str(sequence))
        if previous is not None and sequence != previous + 1:
            raise ValueError(f"aggregate id hole: {previous} -> {sequence}")
        if previous_timestamp is not None and timestamp < previous_timestamp:
            raise ValueError("print time regressed at id " + str(sequence))
        if archive_first is None:
            archive_first = sequence
        archive_count += 1
        previous = sequence
        previous_timestamp = timestamp
        if not manifest["start_ms"] <= timestamp < manifest["end_ms"]:
            continue
        minute = timestamp - timestamp % 60000
        if minute not in aggregates:
            if aggregates:
                packets.append({"type": "time", "ts": minute})
            aggregates[minute] = {"timestamp": str(minute), "open": price,
                "high": price, "low": price, "close": price, "volume": Decimal(0)}
        bar = aggregates[minute]
        bar["high"] = max(bar["high"], price)
        bar["low"] = min(bar["low"], price)
        bar["close"] = price
        bar["volume"] += quantity
        packets.append({"type": "tick", "ts": timestamp, "seq": sequence,
                        "price": float(price), "qty": float(quantity)})
    ticks = [packet for packet in packets if packet["type"] == "tick"]
    if not ticks or (ticks[0]["seq"], ticks[-1]["seq"], len(ticks)) != (
            manifest["first_id"], manifest["last_id"], manifest["count"]):
        raise ValueError("tick manifest id range/count mismatch")
    minutes = list(aggregates)
    if (manifest["start_ms"] % 60000 or manifest["end_ms"] % 60000
            or minutes != list(range(manifest["start_ms"], manifest["end_ms"], 60000))):
        raise ValueError("tick window must contain every minute and have minute-aligned bounds")
    packets.append({"type": "time", "ts": manifest["end_ms"]})
    manifest["verified_file"] = {"first_id": archive_first, "last_id": previous,
        "count": archive_count, "id_holes": 0, "nonpositive_quantities": 0,
        "timestamp_regressions": 0}
    bars = [{field: str(value) for field, value in bar.items()} for bar in aggregates.values()]
    return manifest, packets, bars


def message_groups(packets, batch_size):
    if not 1 <= batch_size <= 1024:
        raise ValueError("normalized batch size must be 1..1024")
    for start in range(0, len(packets), batch_size):
        events = packets[start:start + batch_size]
        yield events[0] if batch_size == 1 else {"type": "batch", "events": events}


def parse_cost(path):
    values = {}
    for line in path.read_text().splitlines():
        text = line.strip()
        for label, key in (("User time (seconds):", "user_seconds"),
                           ("System time (seconds):", "system_seconds"),
                           ("Maximum resident set size (kbytes):", "max_rss_kib")):
            if text.startswith(label):
                values[key] = float(text[len(label):].strip())
    if len(values) != 3:
        raise ValueError("incomplete /usr/bin/time -v receipt")
    values["cpu_seconds"] = values["user_seconds"] + values["system_seconds"]
    return values


class LocalFeed:
    def __init__(self, messages):
        self.messages = messages
        self.requests = []
        owner = self

        class Feed(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                owner.requests.append({"path": self.path, "messages": len(owner.messages)})
                try:
                    if self.headers.get("Upgrade", "").lower() == "websocket":
                        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"]
                            + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                        self.send_response(101)
                        self.send_header("Upgrade", "websocket")
                        self.send_header("Connection", "Upgrade")
                        self.send_header("Sec-WebSocket-Accept", accept)
                        self.end_headers()
                        self.wfile.flush()
                        def receive_exact(count):
                            received = b""
                            while len(received) < count:
                                block = self.connection.recv(count - len(received))
                                if not block:
                                    raise ConnectionResetError("subscription incomplete")
                                received += block
                            return received
                        header = receive_exact(2)
                        length = header[1] & 127
                        if length == 126:
                            length = struct.unpack("!H", receive_exact(2))[0]
                        elif length == 127:
                            length = struct.unpack("!Q", receive_exact(8))[0]
                        if not header[1] & 128 or length > 1024:
                            raise ValueError("invalid local subscription")
                        mask = receive_exact(4)
                        payload = receive_exact(length)
                        subscription = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
                        if subscription != b'{"op":"ready"}':
                            raise ValueError("unexpected local subscription")
                        owner.requests[-1]["subscription"] = subscription.decode()
                        for message in owner.messages:
                            payload = message.encode()
                            header = bytes((0x81, len(payload))) if len(payload) < 126 else (
                                bytes((0x81, 126)) + struct.pack("!H", len(payload)) if len(payload) < 65536
                                else bytes((0x81, 127)) + struct.pack("!Q", len(payload)))
                            self.wfile.write(header + payload)
                        self.wfile.flush()
                        self.connection.settimeout(30)
                        self.connection.recv(4096)
                    else:
                        payload = ("\n".join(owner.messages) + "\n").encode()
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(payload)))
                        self.end_headers()
                        self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError, TimeoutError):
                    pass

            def log_message(self, *unused):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Feed)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def finish(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def direct_tape(strategy, warmup, packets, array_size, output, hashes=False,
                observe_bars=False, retain=False, timeframe=15, message_size=None):
    handle = strategy.create()
    actions = []
    digests = []
    sealed = []
    try:
        if retain:
            strategy.check(strategy.library.equivalence_retain_events(handle))
        strategy.check(strategy.library.strategy_stream_begin(handle, warmup, len(warmup),
            str(timeframe).encode(), str(timeframe).encode()))
        position = 0
        while position < len(packets):
            packet = packets[position]
            count = 1
            if packet["type"] == "tick":
                limit = min(array_size, message_size - position % message_size) if message_size else array_size
                while (count < limit and position + count < len(packets)
                       and packets[position + count]["type"] == "tick"):
                    count += 1
                ticks = (TradeTick * count)(*[TradeTick(event["ts"], event["seq"], event["price"], event["qty"])
                    for event in packets[position:position + count]])
                strategy.check(strategy.library.strategy_stream_push_ticks(handle, ticks, count))
            else:
                strategy.check(strategy.library.strategy_stream_advance_time(handle, packet["ts"]))
            actions.extend(strategy.drain_actions(handle))
            if hashes:
                digests.append({"event_index": position + count - 1,
                    "hash": str(strategy.library.strategy_stream_state_hash(handle))})
            if observe_bars and packet["type"] == "time":
                sealed.append(strategy.source_bar(handle, timeframe))
            position += count
        result = {"actions": actions, "state": strategy.read_report(handle), "hashes": digests,
                  "source_bars": sealed}
        if retain:
            for kind in ("actions", "receipts"):
                strategy.check(getattr(strategy.library, "equivalence_export_" + kind)(handle,
                    str(output / (kind + ".jsonl")).encode()))
                result["observed_" + kind] = read_rows(output / (kind + ".jsonl"))
        return result
    finally:
        strategy.library.strategy_free(handle)


def message_hashes(event_hashes, total, batch_size):
    lookup = {row.get("event_index", index): row["hash"] for index, row in enumerate(event_hashes)}
    return [lookup[min(start + batch_size, total) - 1] for start in range(0, total, batch_size)]


def runner_command(strategy, output, runner, receiver, mode):
    return [str(runner), "run", "--strategy", str(strategy.path), "--warmup", str(output / "warmup.csv"),
        "--input-tf", "15", "--script-tf", "15", "--session", "24x7", "--timezone", "UTC",
        "--chart-timezone", "UTC", "--mode", mode, "--ledger", str(output / "orders.sqlite3"),
        "--poll-ms", "100",
        "--symbol", "BINANCE:ETHUSDT.P", "--name", strategy.path.stem,
        "--syminfo", "type=crypto", "--syminfo", "currency=USDT", "--syminfo", "basecurrency=ETH",
        "--syminfo", "mintick=0.01", "--syminfo", "pointvalue=1", "--syminfo", "qty_step=0.001",
        "--webhook-url", f"http://127.0.0.1:{receiver.server.server_address[1]}/actions", "--allow-insecure-http"]


def genuine_case(strategy, reference, warmup_rows, packets, output, runner, scenario, mode="ticks"):
    output.mkdir(parents=True)
    fail_first = scenario.startswith(("retry", "combined"))
    restart = scenario.startswith(("restart", "combined"))
    transport = scenario.split("-")[0] if scenario.split("-")[0] in ("stdin", "http", "ws") else "file"
    batch_size = 1 if scenario.endswith("single") else 1024
    messages = [json.dumps(message, separators=(",", ":")) for message in message_groups(packets, batch_size)]
    with (output / "warmup.csv").open("w", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(warmup_rows[0]))
        writer.writeheader()
        writer.writerows(warmup_rows)
    feed_path = output / "feed.jsonl"
    feed_path.write_text("\n".join(messages) + "\n")
    receiver = MockReceiver(output, fail_first)
    local_feed = LocalFeed(messages) if transport in ("http", "ws") else None
    process = None
    result = {"scenario": scenario, "probe": strategy.path.stem, "batch_size": batch_size,
        "transport": transport, "messages": len(messages), "feed_bytes": feed_path.stat().st_size,
        "fail_first": fail_first, "restart": restart}
    costs = []
    commands = []
    try:
        base = runner_command(strategy, output, runner, receiver, mode)
        if transport in ("http", "ws"):
            command = base + ["--feed-url", f"{transport}://127.0.0.1:{local_feed.server.server_address[1]}/feed"]
            command += ["--check"] if transport == "http" else ["--max-events", str(len(messages))]
            if transport == "ws":
                subscription_path = output / "subscribe.json"
                subscription_path.write_text('{"op":"ready"}')
                command += ["--subscribe", str(subscription_path)]
        else:
            command = base + ["--feed", "-" if transport == "stdin" else str(feed_path)]
        with (output / "runner.log").open("w") as log:
            for launch in range(2 if restart else 1):
                cost_path = output / f"time-{launch}.txt"
                timed = ["/usr/bin/time", "-v", "-o", str(cost_path)] + command
                commands.append(timed)
                with feed_path.open("rb") as source:
                    process = subprocess.Popen(timed, stdin=source if transport == "stdin" else subprocess.DEVNULL,
                        stdout=log, stderr=subprocess.STDOUT)
                    if restart and launch == 0:
                        deadline = time.monotonic() + 600
                        recorded = 0
                        while process.poll() is None and time.monotonic() < deadline:
                            try:
                                with sqlite3.connect(output / "orders.sqlite3", timeout=0.1) as ledger:
                                    recorded = ledger.execute("SELECT COUNT(*) FROM inputs").fetchone()[0]
                            except sqlite3.Error:
                                pass
                            if len(messages) // 2 <= recorded < len(messages):
                                children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
                                if len(children) != 1:
                                    raise RuntimeError("timed runner child is not uniquely identified")
                                os.kill(int(children[0]), signal.SIGKILL)
                                result["sigkill"] = {"signal": 9, "committed_before_kill": recorded}
                                break
                            time.sleep(0.01)
                        killed = process.wait(timeout=20)
                        if "sigkill" not in result or killed != 137:
                            raise RuntimeError(f"mid-tape SIGKILL not proven: {killed}")
                        result["sigkill"]["time_returncode"] = killed
                        with sqlite3.connect(output / "orders.sqlite3") as ledger:
                            resumed = ledger.execute("SELECT COUNT(*) FROM inputs").fetchone()[0]
                        feed_path = output / "resume.jsonl"
                        feed_path.write_text("\n".join(messages[resumed:]) + "\n")
                        command = base + ["--feed", str(feed_path), "--from-input", str(resumed)]
                        result["resume_from_input"] = resumed
                    else:
                        result["runner_returncode"] = process.wait(timeout=1800)
                costs.append(parse_cost(cost_path))
        attempts, effects = receiver.finish(output)
        receiver_errors = list(receiver.errors)
        receiver = None
        if receiver_errors:
            raise RuntimeError("webhook receiver errors: " + json.dumps(receiver_errors))
        expected = [action_key(action) for action in reference["actions"]]
        result["action_difference"] = first_difference(expected, [action_key(action) for action in effects])
        with sqlite3.connect(output / "orders.sqlite3") as ledger:
            actual_hashes = [record[0] for record in ledger.execute("SELECT state_hash FROM inputs ORDER BY input_index")]
            pending = ledger.execute("SELECT COUNT(*) FROM events WHERE acknowledged=0").fetchone()[0]
        result["hash_difference"] = first_difference(message_hashes(reference["hashes"], len(packets), batch_size), actual_hashes)
        result.update(actions=len(effects), expected_actions=len(expected), attempts=len(attempts),
            committed_inputs=len(actual_hashes), pending_events=pending,
            closed_trades=len(reference["state"]["closed_trades"]), equity_points=len(reference["state"]["equity_curve"]))
        if [effect["sequence"] for effect in effects] != list(range(1, len(effects) + 1)):
            raise RuntimeError("webhook effects are reordered, missing or duplicated")
        if len({effect["event_id"] for effect in effects}) != len(effects):
            raise RuntimeError("duplicate webhook effect")
        if fail_first:
            rejected = Counter(attempt["event_id"] for attempt in attempts if attempt["status"] == 503)
            if set(rejected) != {effect["event_id"] for effect in effects} or any(count != 1 for count in rejected.values()):
                raise RuntimeError("fail-first webhook coverage incomplete")
        result["status"] = "PASS" if (not result["action_difference"] and not result["hash_difference"]
            and result["runner_returncode"] == 0 and pending == 0 and len(attempts) >= len(effects)) else "FAIL"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        (output / "exception.log").write_text(traceback.format_exc())
    finally:
        if process is not None and process.poll() is None:
            children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
            for child in children:
                os.kill(int(child), signal.SIGKILL)
            process.wait(timeout=20)
        if receiver is not None:
            receiver.finish(output)
        if local_feed is not None:
            local_feed.finish()
            result["feed_requests"] = local_feed.requests
        write_json(output / "commands.json", commands)
        result["cost"] = {"cpu_seconds": sum(cost["cpu_seconds"] for cost in costs),
            "max_rss_kib": max((cost["max_rss_kib"] for cost in costs), default=0), "invocations": costs}
        write_json(output / "result.json", result)
    print(f'{result["status"]} {"R-A" if mode == "ticks" else "BAR-COST"} probe={strategy.path.stem} scenario={scenario} '
          f'actions={result.get("actions", 0)}/{result.get("expected_actions", 0)} '
          f'inputs={result.get("committed_inputs", 0)}/{len(messages)}', flush=True)
    return result


def mapped_actions(actions):
    mapped = copy.deepcopy(actions)
    for action in mapped:
        action["timestamp"] -= action["timestamp"] % 900000
    return [action_key(action) for action in mapped]


def mapped_report(state):
    mapped = copy.deepcopy(state)
    for trade in mapped["closed_trades"] + mapped["terminal_trades"]:
        for field in ("entry_time", "exit_time"):
            trade[field] -= trade[field] % 900000
    return mapped


def bar_differences(expected, actual):
    discrepancies = []
    for row in expected:
        timestamp = int(row["timestamp"])
        observed = actual.get(timestamp)
        for field in ("open", "high", "low", "close", "volume"):
            if observed is None or Decimal(str(observed[field])) != Decimal(row[field]):
                discrepancies.append({"minute": timestamp, "field": field, "expected": row[field],
                    "actual": None if observed is None else str(observed[field])})
    return discrepancies


def read_venue(path):
    if path.suffix == ".parquet":
        import pyarrow.parquet as parquet
        records = parquet.read_table(path).to_pylist()
    else:
        records = list(csv_rows(path))
    result = {}
    for row in records:
        timestamp = row.get("timestamp", row.get("ts", row.get("open_time")))
        if hasattr(timestamp, "timestamp"):
            timestamp = int(timestamp.timestamp() * 1000)
        result[int(timestamp)] = row
    return result


def run_genuine_tape(arguments):
    root = arguments.root.resolve()
    build = arguments.build_dir.resolve()
    output = arguments.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "timestamp-contract.json", TIMESTAMP_CONTRACT)
    manifest, packets, aggregate_rows = load_tick_tape(arguments.tick_tape, arguments.tick_manifest)
    write_json(output / "data-manifest.json", manifest)
    write_json(output / "decimal-bars.json", aggregate_rows)
    sys.path.insert(0, str(root / "scripts"))
    from run_strategy import BarC, ReportC
    from datetime import datetime, timezone
    start_date = datetime.fromisoformat(arguments.start)
    if start_date.tzinfo is None:
        start_date = start_date.replace(tzinfo=timezone.utc)
    start = int(start_date.timestamp() * 1000)
    corpus_rows = []
    with (root / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv").open() as source:
        for row in csv.DictReader(source):
            if start <= int(row["timestamp"]) < manifest["end_ms"]:
                corpus_rows.append(row)
    warmup_rows = [row for row in corpus_rows if int(row["timestamp"]) < manifest["start_ms"]]
    if not warmup_rows or any(int(row["timestamp"]) != start + index * 60000 for index, row in enumerate(warmup_rows)):
        raise RuntimeError("warmup must be nonempty and contiguous before the genuine tape")
    minute_warmup_rows = warmup_rows
    warmup_rows = chart_rows(minute_warmup_rows, 15)
    chart_live_rows = chart_rows(aggregate_rows, 15)
    venue = read_venue(arguments.venue_klines) if arguments.venue_klines else None
    corpus = {int(row["timestamp"]): row for row in corpus_rows}
    results = []
    references = []
    batch_results = []
    bar_checks = {}
    if arguments.tick_rb_only and arguments.tick_reference_only:
        raise ValueError("select either offline bar/path checks or runner-reference qualification")
    if arguments.tick_batch_only and any(scenario.endswith("single") for scenario in arguments.tick_scenarios or ()):
        raise ValueError("--tick-batch-only cannot qualify singleton messages")
    for probe in arguments.probes:
        directory = output / probe
        directory.mkdir()
        library = arguments.libraries_dir.resolve() / (probe + ".so")
        if not arguments.reuse_libraries:
            library = compile_library(root, build, arguments.libraries_dir.resolve(), probe)
        strategy = Strategy(library, BarC, ReportC)
        warmup = chart_bar_array(strategy, warmup_rows)
        combined = chart_bar_array(strategy, warmup_rows + chart_live_rows)
        print(f'START genuine probe={probe} prints={manifest["count"]}', flush=True)
        whole = direct_tape(strategy, warmup, packets, 1024, directory, hashes=True, retain=True)
        write_json(directory / "whole-tape.json", whole)
        incremental = None
        qualification = {}
        if not arguments.tick_rb_only:
            incremental = direct_tape(strategy, warmup, packets, 1024 if arguments.tick_batch_only else 1,
                directory, hashes=True, message_size=1024 if arguments.tick_batch_only else None)
            write_json(directory / "incremental.json", incremental)
            hash_lookup = incremental["hashes"]
            observed = [action for action in whole["observed_actions"] if action["origin_input_index"] >= len(warmup)]
            qualification = {"actions": first_difference(whole["actions"], incremental["actions"]),
                "report": first_difference(whole["state"], incremental["state"]),
                "observer": first_difference([action_key(action) for action in observed], [action_key(action) for action in whole["actions"]])}
            if not arguments.tick_batch_only:
                qualification["array_endpoint_hashes"] = first_difference(whole["hashes"], [hash_lookup[row["event_index"]] for row in whole["hashes"]])
        write_json(directory / "qualification.json", qualification)
        reference = {"probe": probe, "status": "NOT_RUN" if arguments.tick_rb_only else "FAIL" if any(qualification.values()) else "PASS",
                     "qualification": qualification, "library_sha256": strategy.sha256}
        references.append(reference)
        print(f'{reference["status"]} R-A-reference probe={probe} array=1024 '
              f'comparison={"not-run" if arguments.tick_rb_only else "batch-boundaries" if arguments.tick_batch_only else "singleton"}', flush=True)
        batch = strategy.batch(combined, 15, directory / "batch", input_tf=15, distribution=3)
        forward_actions = [action for action in batch["actions"] if action["origin_input_index"] >= len(warmup)]
        batch_checks = {"actions": first_difference(mapped_actions(forward_actions), mapped_actions(whole["actions"])),
            "report": first_difference(mapped_report(batch["state"]), mapped_report(whole["state"]))}
        generated = (root / "corpus/validation" / probe / "generated.cpp").read_text()
        slippage_match = re.search(r"cfg\.slippage\s*=\s*(\d+)", generated)
        slippage = int(slippage_match.group(1)) if slippage_match else 0
        oracle = classify_first_divergence(forward_actions, whole["actions"], batch["receipts"],
            whole["observed_receipts"], packets, aggregate_rows,
            batch_checks["actions"] or batch_checks["report"], slippage)
        write_json(directory / "first-divergence.json", oracle)
        path_insensitive = probe in ("ta-rsi14-cross-50-01", "order-entry-implicit-reversal-exit-01",
            "order-tranche-partial-market-close-01")
        batch_status = "FAIL" if (any(batch_checks.values()) if path_insensitive else oracle["status"] == "UNEXPLAINED") else "PASS"
        batch_results.append({"probe": probe, "reference": "R-B2" if path_insensitive else "R-B3",
            "status": batch_status, "classification": oracle["status"], "actions": len(whole["actions"]),
            "cumulative_closed_trades": len(whole["state"]["closed_trades"]), "checks": batch_checks})
        write_json(directory / "batch-comparison.json", batch_checks)
        print(f'{batch_status} {"R-B2" if path_insensitive else "R-B3"} probe={probe} '
              f'actions={len(whole["actions"])} classification={oracle["status"]} '
              f'difference={json.dumps(batch_checks["actions"], separators=(",", ":"))}', flush=True)
        if probe == arguments.probes[0]:
            rebuilt = direct_tape(strategy, chart_bar_array(strategy, minute_warmup_rows), packets,
                1024, directory, observe_bars=True, timeframe=1)["source_bars"]
            write_json(output / "tick-built-bars.json", rebuilt)
            engine_bars = {row["timestamp"]: row for row in rebuilt}
            bar_checks = {"engine_vs_decimal": bar_differences(aggregate_rows, engine_bars),
                "corpus_vs_decimal": bar_differences(aggregate_rows, corpus),
                "venue_vs_decimal": bar_differences(aggregate_rows, venue) if venue is not None else [{"error": "venue klines not supplied"}]}
            write_json(output / "bar-comparison.json", bar_checks)
            print(f'{"FAIL" if any(bar_checks.values()) else "PASS"} R-B1 minutes={len(aggregate_rows)} '
                  f'mismatches={ {key: len(value) for key, value in bar_checks.items()} }', flush=True)
        if not arguments.tick_reference_only and not arguments.tick_rb_only:
            scenarios = arguments.tick_scenarios or tuple(scenario for scenario in SCENARIOS if not arguments.tick_batch_only or scenario.endswith("batch"))
            for scenario in scenarios:
                if scenario not in SCENARIOS:
                    raise ValueError("unknown genuine scenario " + scenario)
                result = genuine_case(strategy, incremental, warmup_rows, packets, directory / scenario,
                    build / "bin/pineforge-live", scenario)
                results.append(result)
                write_json(output / "results.json", results)
            bar_packets = [{"type": "bar", "bar": {"ts_open": int(row["timestamp"]),
                **{short: float(row[field]) for short, field in (("o", "open"), ("h", "high"),
                   ("l", "low"), ("c", "close"), ("v", "volume"))}}} for row in chart_live_rows]
            bar_reference = strategy.stream(combined, len(warmup), 15, input_tf=15)
            bar_reference["hashes"] = [{"hash": digest} for digest in bar_reference["hashes"]]
            cost = genuine_case(strategy, bar_reference, warmup_rows, bar_packets, directory / "bar-cost",
                build / "bin/pineforge-live", "file-single", mode="bars")
            write_json(directory / "bar-cost-result.json", cost)
        write_json(output / "references.json", references)
        write_json(output / "batch-results.json", batch_results)
    category_failures = {"R-A": sum(result["status"] == "FAIL" for result in results + references),
        "R-B1": int(any(bar_checks.values())),
        "R-B2": sum(result["status"] == "FAIL" and result["reference"] == "R-B2" for result in batch_results),
        "R-B3": sum(result["status"] == "FAIL" and result["reference"] == "R-B3" for result in batch_results)}
    write_json(output / "summary.json", category_failures)
    failures = sum(category_failures.values())
    print(f'GENUINE_TICK_SUMMARY probes={len(references)} scenarios={len(results)} failed={failures} '
          f'prints={manifest["count"]} categories={json.dumps(category_failures, separators=(",", ":"))}', flush=True)
    return 1 if failures else 0
