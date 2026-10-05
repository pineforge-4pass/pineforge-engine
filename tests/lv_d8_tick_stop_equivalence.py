#!/usr/bin/env python3
import argparse
import csv
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from run_strategy import BarC, ReportC
from native_live_equivalence_e2e import Strategy, TradeTick, read_rows


class StopStrategy(Strategy):
    def __init__(self, library, long_side, pyramid, percent):
        super().__init__(library, BarC, ReportC)
        self.long_side = long_side
        self.pyramid = pyramid
        self.percent = percent
        self.library.strategy_set_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        self.library.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        self.library.lv_d8_export_events.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.library.lv_d8_export_events.restype = ctypes.c_int

    def create(self):
        handle = super().create()
        self.library.strategy_set_syminfo_metadata(handle, b"qty_step", 0.0001)
        self.library.strategy_set_input(handle, b"Long", str(self.long_side).lower().encode())
        for key, value in (("pyramiding", self.pyramid),
                           ("default_qty_type", "fixed" if self.percent == 0 else "percent_of_equity"),
                           ("default_qty_value", self.percent or 1)):
            self.library.strategy_set_override(handle, key.encode(), str(value).encode())
        return handle

    def export(self, handle, output):
        output.mkdir(parents=True)
        for name in ("actions", "receipts"):
            self.check(getattr(self.library, "equivalence_export_" + name)(handle, str(output / (name + ".jsonl")).encode()))
        self.check(self.library.lv_d8_export_events(handle, str(output / "events.jsonl").encode()))
        return {name: read_rows(output / (name + ".jsonl")) for name in ("actions", "receipts", "events")}

    def execute(self, bars, packets, output):
        handle = self.create()
        report = ReportC()
        try:
            self.check(self.library.equivalence_retain_events(handle))
            if packets is None:
                self.library.run_backtest_full(handle, bars, len(bars), b"1", b"5", 0, 4, 3, ctypes.byref(report))
                error = self.library.strategy_get_last_error(handle)
                if error:
                    raise RuntimeError(error.decode())
            else:
                self.check(self.library.strategy_stream_begin(handle, bars, len(bars), b"1", b"5"))
                for packet in packets:
                    if packet["type"] == "tick":
                        tick = TradeTick(packet["ts"], packet["seq"], packet["price"], packet["qty"])
                        self.check(self.library.strategy_stream_push_ticks(handle, ctypes.byref(tick), 1))
                    else:
                        self.check(self.library.strategy_stream_advance_time(handle, packet["ts"]))
                    self.drain_actions(handle)
            return self.export(handle, output)
        finally:
            self.library.report_free(ctypes.byref(report))
            self.library.strategy_free(handle)


def make_tape(long_side, variant):
    start = 1704067200000
    direction = 1 if long_side else -1
    level = 100.0 + direction
    crossed = level if variant == "touch" else 100.0 + direction * 1.5
    trigger_minute = 10 if variant == "gap-open" else 14 if variant == "script-close" else 11
    rows, packets = [], []
    sequence = 0
    for minute in range(20):
        opening = start + minute * 60000
        prices = [100.0] * 3
        if minute > trigger_minute or minute == trigger_minute and variant == "gap-open":
            prices = [crossed] * 3
        elif minute == trigger_minute:
            prices[2] = crossed
            if variant not in ("minute-close", "script-close"):
                prices[1] = crossed
        rows.append(dict(timestamp=opening, open=prices[0], high=max(prices), low=min(prices), close=prices[-1], volume=3))
        if minute < 5:
            continue
        for offset, price in zip((1, 15000, 59999), prices):
            sequence += 1
            packets.append(dict(type="tick", ts=opening + offset, seq=sequence, price=price, qty=1.0))
        packets.append(dict(type="time", ts=opening + 60000))
    return rows, packets


def bar_array(rows):
    return (BarC * len(rows))(*(BarC(row["open"], row["high"], row["low"], row["close"], row["volume"], row["timestamp"]) for row in rows))


def runner_actions(args, probe, rows, packets, output):
    output.mkdir(parents=True)
    warmup, feed, ledger = (output / name for name in ("warmup.csv", "ticks.jsonl", "ledger.sqlite"))
    with warmup.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("timestamp", "open", "high", "low", "close", "volume"))
        writer.writeheader()
        writer.writerows(rows[:5])
    feed.write_text("".join(json.dumps(packet) + "\n" for packet in packets))
    command = [str(args.runner), "run", "--strategy", str(args.library), "--warmup", str(warmup),
               "--input-tf", "1", "--script-tf", "5", "--mode", "ticks", "--feed", str(feed),
               "--ledger", str(ledger), "--symbol", "BINANCE:ETHUSDT.P", "--name", "lv-d8",
               "--session", "24x7", "--timezone", "UTC", "--chart-timezone", "UTC", "--input", f"Long={str(probe.long_side).lower()}"]
    for key, value in (("type", "crypto"), ("currency", "USDT"), ("basecurrency", "ETH"),
                       ("mintick", "0.01"), ("pointvalue", "1"), ("qty_step", "0.0001")):
        command += ["--syminfo", f"{key}={value}"]
    for key, value in (("pyramiding", probe.pyramid),
                       ("default_qty_type", "fixed" if probe.percent == 0 else "percent_of_equity"),
                       ("default_qty_value", probe.percent or 1)):
        command += ["--override", f"{key}={value}"]
    subprocess.run(command, check=True, capture_output=True, text=True)
    result = subprocess.run([str(args.runner), "actions", "--ledger", str(ledger)], check=True, capture_output=True, text=True)
    (output / "actions.jsonl").write_text(result.stdout)
    return [json.loads(line) for line in result.stdout.splitlines() if line]


def run(args, output):
    count = 0
    for percent in (0, 50, 100):
        for pyramid in (0, 1):
            for long_side in (True, False):
                for variant in ("cross", "touch", "gap-open", "minute-close", "script-close"):
                    case = f"percent{percent}-p{pyramid}-{'long' if long_side else 'short'}-{variant}"
                    folder = output / case
                    rows, packets = make_tape(long_side, variant)
                    probe = StopStrategy(args.library, long_side, pyramid, percent)
                    batch = probe.execute(bar_array(rows), None, folder / "batch")
                    mirror = probe.execute(bar_array(rows[:5]), packets, folder / "mirror")
                    live = runner_actions(args, probe, rows, packets, folder / "runner")
                    fields = ("id", "action", "leg", "contracts", "price", "reduce_only", "entry_incarnation")
                    assert [[row["order"][key] for key in fields] for row in live] == [
                        [row["order"][key] for key in fields] for row in mirror["actions"]], case
                    absent_order = percent == 100 and not long_side
                    refused = percent == 100 and long_side and variant != "touch"
                    expected_live = 0 if absent_order or refused else 1
                    expected_batch = 0 if absent_order or refused and variant == "gap-open" else 1
                    assert len(live) == expected_live, (case, live)
                    assert len(batch["actions"]) == expected_batch, (case, batch["actions"])
                    if live:
                        assert live[0]["order"]["contracts"] == batch["actions"][0]["order"]["contracts"], case
                        if variant in ("touch", "gap-open"):
                            assert [live[0]["order"][key] for key in fields] == [
                                batch["actions"][0]["order"][key] for key in fields], case
                    rejected = [row for row in mirror["events"] if row["kind"] == "match-rejected"]
                    assert bool(rejected) == refused, (case, rejected)
                    if refused:
                        assert rejected[0]["reason"] == 8, (case, rejected)
                    if args.require_terminal_receipts and variant == "gap-open" and refused:
                        for mode, result in (("batch", batch), ("ticks", mirror)):
                            assert any(row["kind"] == "match_rejected" for row in result["receipts"]), (
                                f"{case}: {mode} receipt omits native HostPrecommit terminal event")
                    if args.assume_fill_equivalence and percent == 100 and long_side and variant == "cross":
                        assert len(live) == len(batch["actions"]), (
                            f"{case}: runner TICK entries={len(live)} canonical ENDPOINTS entries={len(batch['actions'])}; native rejection={rejected}")
                    count += 1
    print(f"LV-D8 runner/ENDPOINTS: {count} stop rows passed; observed-price funding is not OHLC fill equivalence")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--require-terminal-receipts", action="store_true")
    parser.add_argument("--assume-fill-equivalence", action="store_true")
    args = parser.parse_args()
    if args.out:
        run(args, args.out)
    else:
        with tempfile.TemporaryDirectory(prefix="lv-d8-") as directory:
            run(args, Path(directory))


if __name__ == "__main__":
    main()
