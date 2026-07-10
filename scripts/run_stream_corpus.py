#!/usr/bin/env python3
"""Compare bar-only and historical->tick-stream runs across corpus strategies.

Every experiment slices the canonical OHLCV CSV and the raw Binance daily
trade ZIP(s) at runtime. No pre-trimmed fixture is used. The same strategy
configuration is run twice:

  1. confirmed 1-minute OHLCV for the complete interval;
  2. confirmed OHLCV strictly before handoff, then raw trades until end.

The second run is cumulative: positions, equity, pending orders, Pine/TA state,
request.security state and partial timeframe aggregation all cross the handoff.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import difflib
import io
import json
import random
import sys
import time
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from run_strategy import (  # noqa: E402
    BarC, ReportC, Strategy, TradeTickC, _report_to_dict,
    _VALIDATION_META_KEYS,
)

DEFAULT_DATA_ROOT = Path("/Volumes/PineforgeData/binance_ethusdtp_1y")
DEFAULT_OHLCV = "ETHUSDT.P_1m_OHLCV_canonical_2025-01-01_2026-07-08.csv"
RANGE_START = datetime(2025, 2, 1, tzinfo=timezone.utc)
RANGE_END = datetime(2025, 4, 1, tzinfo=timezone.utc)


def utc_ms(dt: datetime) -> int:
    return int(dt.timestamp() * 1000)


def parse_utc(text: str) -> datetime:
    value = text.strip().replace("Z", "+00:00")
    dt = datetime.fromisoformat(value)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def choose_handoff(seed: int, duration_minutes: int) -> datetime:
    latest = RANGE_END - timedelta(minutes=duration_minutes)
    total_minutes = int((latest - RANGE_START).total_seconds() // 60)
    offset = random.Random(seed).randrange(total_minutes)
    return RANGE_START + timedelta(minutes=offset)


def iter_days(start: datetime, end: datetime):
    day = start.date()
    while datetime.combine(day, datetime.min.time(), tzinfo=timezone.utc) < end:
        yield day
        day += timedelta(days=1)


def load_ohlcv_slice(csv_path: Path, start_ms: int | None = None,
                     end_ms: int | None = None) -> tuple[ctypes.Array, int]:
    """Slice canonical dataset OHLCV directly from its source CSV."""
    rows: list[tuple[float, float, float, float, float, int]] = []
    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        timestamp_key = "timestamp" if "timestamp" in (reader.fieldnames or []) else "open_time"
        for row in reader:
            timestamp = int(row[timestamp_key])
            if start_ms is not None and timestamp < start_ms:
                continue
            if end_ms is not None and timestamp > end_ms:
                break
            rows.append((
                float(row["open"]), float(row["high"]), float(row["low"]),
                float(row["close"]), float(row["volume"]), timestamp))
    bars = (BarC * len(rows))()
    for i, values in enumerate(rows):
        bars[i] = BarC(*values)
    return bars, len(rows)


def load_tick_slice(data_root: Path, start_ms: int, end_ms: int) -> tuple[ctypes.Array, dict]:
    """Slice [start_ms, end_ms) directly from raw daily ZIPs.

    Binance archives are ordered by exchange trade id. The same monotonic-id
    filter used by the dataset builder is applied here so malformed/replayed
    rows never reach the engine.
    """
    start = datetime.fromtimestamp(start_ms / 1000, tz=timezone.utc)
    end = datetime.fromtimestamp(end_ms / 1000, tz=timezone.utc)
    rows: list[tuple[int, int, float, float, int]] = []
    previous_id = 0
    skipped_nonmonotonic = 0
    archives: list[str] = []

    for day in iter_days(start, end):
        stem = f"ETHUSDT-trades-{day.isoformat()}"
        archive = data_root / "raw" / "trades" / f"{stem}.zip"
        if not archive.is_file():
            raise FileNotFoundError(f"missing raw trade archive: {archive}")
        archives.append(str(archive))
        with zipfile.ZipFile(archive) as zf:
            members = [name for name in zf.namelist() if name.endswith(".csv")]
            if len(members) != 1:
                raise RuntimeError(f"expected one CSV in {archive}, got {members}")
            with zf.open(members[0]) as raw:
                reader = csv.DictReader(io.TextIOWrapper(raw, encoding="utf-8", newline=""))
                for row in reader:
                    timestamp = int(row["time"])
                    if timestamp < start_ms:
                        continue
                    if timestamp >= end_ms:
                        break
                    sequence = int(row["id"])
                    if sequence <= previous_id:
                        skipped_nonmonotonic += 1
                        continue
                    previous_id = sequence
                    rows.append((
                        timestamp,
                        sequence,
                        float(row["price"]),
                        float(row["qty"]),
                    ))

    ticks = (TradeTickC * len(rows))()
    for i, (timestamp, sequence, price, quantity) in enumerate(rows):
        ticks[i] = TradeTickC(timestamp, sequence, price, quantity)
    return ticks, {
        "count": len(rows),
        "skipped_nonmonotonic": skipped_nonmonotonic,
        "archives": archives,
    }


def load_case_config(case_dir: Path) -> dict:
    path = case_dir / "inputs.json"
    data = json.loads(path.read_text()) if path.is_file() else {}
    params = {
        key: value for key, value in data.items()
        if not key.startswith("_") and key not in _VALIDATION_META_KEYS
        and key not in {"validation_overrides", "expected_tier", "notes"}
    }
    return {
        "params": params,
        "strategy_overrides": data.get("strategy_overrides") or {},
        "runtime": data.get("runtime_overrides") or {},
        "chart_timezone": data.get("chart_timezone"),
        "input_tf": str(data.get("input_tf") or "1"),
        "script_tf": str(data.get("script_tf") or "1"),
        "ohlcv_start_ms": data.get("ohlcv_start_ms"),
    }


def configure_state(strategy: Strategy, state, config: dict) -> None:
    lib = strategy.lib
    for key, value in config["strategy_overrides"].items():
        lib.strategy_set_override(state, str(key).encode(), str(value).encode())
    for key, value in config["params"].items():
        lib.strategy_set_input(state, str(key).encode(), str(value).encode())
    runtime = config["runtime"]
    chart_tz = config["chart_timezone"]
    if chart_tz and hasattr(lib, "strategy_set_chart_timezone"):
        lib.strategy_set_chart_timezone(state, str(chart_tz).encode())
    if runtime.get("timezone") and hasattr(lib, "strategy_set_syminfo_timezone"):
        lib.strategy_set_syminfo_timezone(state, str(runtime["timezone"]).encode())
    if runtime.get("session") and hasattr(lib, "strategy_set_syminfo_session"):
        lib.strategy_set_syminfo_session(state, str(runtime["session"]).encode())
    if runtime.get("mintick") is not None:
        lib.strategy_set_syminfo_mintick(state, float(runtime["mintick"]))
    if runtime.get("pointvalue") is not None:
        lib.strategy_set_syminfo_pointvalue(state, float(runtime["pointvalue"]))
    for key, value in (runtime.get("syminfo_metadata") or {}).items():
        lib.strategy_set_syminfo_metadata(state, str(key).encode(), float(value))


def engine_error(strategy: Strategy, state, operation: str) -> RuntimeError:
    raw = strategy.lib.strategy_get_last_error(state)
    message = raw.decode("utf-8", "replace") if raw else "unknown engine error"
    return RuntimeError(f"{operation}: {message}")


def run_batch(strategy: Strategy, bars, n: int, config: dict) -> dict:
    lib = strategy.lib
    state = lib.strategy_create(None)
    report = ReportC()
    try:
        configure_state(strategy, state, config)
        lib.run_backtest_full(
            state, bars, n,
            config["input_tf"].encode(), config["script_tf"].encode(),
            0, 4, 3, ctypes.byref(report))
        raw = lib.strategy_get_last_error(state)
        if raw:
            message = raw.decode("utf-8", "replace")
            if message:
                raise RuntimeError("batch run: " + message)
        return _report_to_dict(report)
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)


def run_stream(strategy: Strategy, warmup, n_warmup: int, ticks,
               end_ms: int, config: dict) -> dict:
    lib = strategy.lib
    if not hasattr(lib, "strategy_stream_begin"):
        raise RuntimeError("strategy library does not export strategy_stream_begin")
    state = lib.strategy_create(None)
    report = ReportC()
    try:
        configure_state(strategy, state, config)
        if lib.strategy_stream_begin(
                state, warmup, n_warmup,
                config["input_tf"].encode(), config["script_tf"].encode()) != 0:
            raise engine_error(strategy, state, "stream begin")
        if lib.strategy_stream_push_ticks(state, ticks, len(ticks)) != 0:
            raise engine_error(strategy, state, "stream ticks")
        if lib.strategy_stream_advance_time(state, end_ms) != 0:
            raise engine_error(strategy, state, "stream advance")
        if lib.strategy_stream_end(state, 0) != 0:
            raise engine_error(strategy, state, "stream end")
        if lib.strategy_stream_fill_report(state, ctypes.byref(report)) != 0:
            raise engine_error(strategy, state, "stream report")
        return _report_to_dict(report)
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)


def compare_reports(batch: dict, stream: dict) -> dict:
    left = batch["trades"]
    right = stream["trades"]
    positional = 0
    for a, b in zip(left, right):
        same_minute = (
            a["entry_time"] // 60_000 == b["entry_time"] // 60_000
            and a["exit_time"] // 60_000 == b["exit_time"] // 60_000)
        same_shape = (
            a["is_long"] == b["is_long"]
            and a["entry_bar_index"] == b["entry_bar_index"]
            and a["exit_bar_index"] == b["exit_bar_index"]
            and abs(a["qty"] - b["qty"]) <= 1e-8 * max(1.0, abs(a["qty"])))
        if same_minute and same_shape:
            positional += 1
    denom = max(len(left), len(right))
    def signature(trade: dict) -> tuple:
        return (
            trade["entry_time"] // 60_000,
            trade["exit_time"] // 60_000,
            trade["is_long"],
            trade["entry_bar_index"],
            trade["exit_bar_index"],
            round(trade["qty"], 8),
        )

    matcher = difflib.SequenceMatcher(
        None,
        [signature(trade) for trade in left],
        [signature(trade) for trade in right],
        autojunk=False,
    )
    ordered_match = sum(block.size for block in matcher.get_matching_blocks())
    batch_profit = float(batch["net_profit"])
    stream_profit = float(stream["net_profit"])
    return {
        "batch_trades": len(left),
        "stream_trades": len(right),
        "trade_count_equal": len(left) == len(right),
        "positional_trade_match": positional,
        "positional_trade_match_pct": (
            100.0 if denom == 0 else 100.0 * positional / denom),
        "ordered_trade_match": ordered_match,
        "ordered_trade_match_pct": (
            100.0 if denom == 0 else 100.0 * ordered_match / denom),
        "structural_equal": len(left) == len(right) and ordered_match == denom,
        "batch_net_profit": batch_profit,
        "stream_net_profit": stream_profit,
        "net_profit_delta": stream_profit - batch_profit,
        "input_bars_equal": batch["input_bars_processed"]
            == stream["input_bars_processed"],
        "script_bars_equal": batch["script_bars_processed"]
            == stream["script_bars_processed"],
    }


def discover_cases(pattern: str, limit: int) -> list[Path]:
    cases = sorted(path.parent for path in REPO_ROOT.glob(pattern)
                   if path.name in {"strategy.so", "strategy.dylib"})
    return cases[:limit] if limit > 0 else cases


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--handoff", help="UTC ISO-8601; default is seeded random")
    parser.add_argument("--seed", type=int, default=20250710)
    parser.add_argument("--duration-minutes", type=int, default=10)
    parser.add_argument("--cases", default="corpus/validation/*/strategy.dylib")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument(
        "--honor-case-start", action="store_true",
        help="honor validation-only ohlcv_start_ms metadata (off by default)")
    parser.add_argument("--output", type=Path,
                        default=REPO_ROOT / "build" / "stream_corpus_report.json")
    args = parser.parse_args()

    handoff = parse_utc(args.handoff) if args.handoff else choose_handoff(
        args.seed, args.duration_minutes)
    if handoff.second or handoff.microsecond:
        raise SystemExit("handoff must be aligned to a one-minute boundary")
    if not (RANGE_START <= handoff < RANGE_END):
        raise SystemExit(
            f"handoff must be in [{RANGE_START.isoformat()}, {RANGE_END.isoformat()})")
    end = handoff + timedelta(minutes=args.duration_minutes)
    handoff_ms, end_ms = utc_ms(handoff), utc_ms(end)

    ohlcv = args.data_root / DEFAULT_OHLCV
    if not ohlcv.is_file():
        raise SystemExit(f"canonical OHLCV not found: {ohlcv}")

    started = time.perf_counter()
    # These are independent slices from the canonical source on every run.
    warmup, n_warmup = load_ohlcv_slice(ohlcv, end_ms=handoff_ms - 1)
    full, n_full = load_ohlcv_slice(ohlcv, end_ms=end_ms - 1)
    ticks, tick_meta = load_tick_slice(args.data_root, handoff_ms, end_ms)
    if n_warmup == 0 or len(ticks) == 0:
        raise SystemExit("experiment slice contains no warmup bars or no ticks")

    cases = discover_cases(args.cases, args.limit)
    if not cases:
        raise SystemExit(f"no compiled corpus strategies match {args.cases!r}")

    print(f"handoff={handoff.isoformat()} end={end.isoformat()} "
          f"warmup_bars={n_warmup} full_bars={n_full} ticks={len(ticks)} "
          f"cases={len(cases)}", flush=True)

    results = []
    for index, case in enumerate(cases, 1):
        so = case / ("strategy.dylib" if (case / "strategy.dylib").is_file()
                     else "strategy.so")
        config = load_case_config(case)
        if args.honor_case_start and config["ohlcv_start_ms"] is not None:
            # Re-slice from the canonical source for this case; never trim an
            # already-trimmed array in memory.
            case_start = int(config["ohlcv_start_ms"])
            case_warmup, case_n_warmup = load_ohlcv_slice(
                ohlcv, start_ms=case_start, end_ms=handoff_ms - 1)
            case_full, case_n_full = load_ohlcv_slice(
                ohlcv, start_ms=case_start, end_ms=end_ms - 1)
        else:
            case_warmup, case_n_warmup = warmup, n_warmup
            case_full, case_n_full = full, n_full
        item = {"case": str(case.relative_to(REPO_ROOT)), "config": config}
        try:
            strategy = Strategy(so)
            batch = run_batch(strategy, case_full, case_n_full, config)
            stream = run_stream(
                strategy, case_warmup, case_n_warmup, ticks, end_ms, config)
            item["comparison"] = compare_reports(batch, stream)
            item["status"] = "ok"
        except Exception as exc:  # corpus sweep records failures and continues
            item["status"] = "error"
            item["error"] = str(exc)
        results.append(item)
        if index % 20 == 0 or index == len(cases):
            print(f"[{index}/{len(cases)}]", flush=True)

    ok = [item for item in results if item["status"] == "ok"]
    summary = {
        "cases": len(results),
        "ok": len(ok),
        "errors": len(results) - len(ok),
        "trade_count_equal": sum(
            bool(item["comparison"]["trade_count_equal"]) for item in ok),
        "input_bars_equal": sum(
            bool(item["comparison"]["input_bars_equal"]) for item in ok),
        "script_bars_equal": sum(
            bool(item["comparison"]["script_bars_equal"]) for item in ok),
        "elapsed_seconds": time.perf_counter() - started,
    }
    payload = {
        "experiment": {
            "handoff": handoff.isoformat(),
            "end": end.isoformat(),
            "seed": args.seed,
            "ohlcv": str(ohlcv),
            "warmup_bars": n_warmup,
            "full_bars": n_full,
            "ticks": tick_meta,
        },
        "summary": summary,
        "results": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(json.dumps(summary, sort_keys=True))
    print(f"report={args.output}")
    return 0 if summary["errors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
