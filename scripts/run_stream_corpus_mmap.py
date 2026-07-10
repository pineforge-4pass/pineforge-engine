#!/usr/bin/env python3
"""Whole-session corpus sweep over one contiguous memory-mapped tick tape.

Each probe receives exactly one strategy_stream_push_ticks() call spanning its
handoff through the common end timestamp. The tape is a direct binary mirror of
pf_trade_tick_t; mmap avoids constructing the full multi-gigabyte slice as
Python objects without introducing feed chunks or intermediate session ends.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import io
import json
import mmap
import os
import struct
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from run_strategy import ReportC, Strategy, TradeTickC, _report_to_dict  # noqa: E402
from run_stream_corpus import (  # noqa: E402
    DEFAULT_DATA_ROOT, DEFAULT_OHLCV, compare_reports, configure_state,
    discover_cases, engine_error, load_case_config, load_ohlcv_slice,
    parse_utc, run_batch, utc_ms,
)

TICK_STRUCT = struct.Struct("<qQdd")
assert TICK_STRUCT.size == ctypes.sizeof(TradeTickC) == 32


@dataclass
class LiveRun:
    case_label: str
    session_index: int
    strategy: Strategy
    state: int
    tick_pointer: ctypes.POINTER(TradeTickC)
    tick_count: int
    error: str | None = None


def iter_days(start: datetime, end: datetime):
    day = start.date()
    while datetime.combine(day, datetime.min.time(), tzinfo=timezone.utc) < end:
        yield day
        day += timedelta(days=1)


def expected_tick_count(data_root: Path, start_ms: int, end_ms: int) -> int:
    source = data_root / "ETHUSDT.P_1m_OHLCV_from_trades_2025-01-01_2026-07-08.csv"
    total = 0
    with source.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        timestamp_key = "open_time" if "open_time" in (reader.fieldnames or []) else "timestamp"
        for row in reader:
            timestamp = int(row[timestamp_key])
            if timestamp < start_ms:
                continue
            if timestamp >= end_ms:
                break
            total += int(float(row["count"]))
    return total


def build_tick_tape(data_root: Path, tape: Path, start_ms: int, end_ms: int) -> dict:
    metadata_path = tape.with_suffix(tape.suffix + ".json")
    expected = expected_tick_count(data_root, start_ms, end_ms)
    expected_size = expected * TICK_STRUCT.size
    if tape.is_file() and metadata_path.is_file() and tape.stat().st_size == expected_size:
        metadata = json.loads(metadata_path.read_text())
        if (metadata.get("start_ms") == start_ms
                and metadata.get("end_ms") == end_ms
                and metadata.get("count") == expected):
            return metadata

    tape.parent.mkdir(parents=True, exist_ok=True)
    with tape.open("w+b") as handle:
        handle.truncate(expected_size)
        view = mmap.mmap(handle.fileno(), expected_size, access=mmap.ACCESS_WRITE)
        index = 0
        previous_id = 0
        skipped_nonmonotonic = 0
        archives: list[str] = []
        try:
            start = datetime.fromtimestamp(start_ms / 1000, tz=timezone.utc)
            end = datetime.fromtimestamp(end_ms / 1000, tz=timezone.utc)
            for day in iter_days(start, end):
                stem = f"ETHUSDT-trades-{day.isoformat()}"
                archive = data_root / "raw" / "trades" / f"{stem}.zip"
                if not archive.is_file():
                    raise FileNotFoundError(f"missing raw trade archive: {archive}")
                archives.append(str(archive))
                with zipfile.ZipFile(archive) as zf:
                    members = [name for name in zf.namelist() if name.endswith(".csv")]
                    if len(members) != 1:
                        raise RuntimeError(
                            f"expected one CSV in {archive}, got {members}")
                    with zf.open(members[0]) as raw:
                        reader = csv.DictReader(io.TextIOWrapper(
                            raw, encoding="utf-8", newline=""))
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
                            if index >= expected:
                                raise RuntimeError(
                                    "raw trade count exceeds OHLCV count oracle")
                            TICK_STRUCT.pack_into(
                                view, index * TICK_STRUCT.size,
                                timestamp, sequence, float(row["price"]),
                                float(row["qty"]))
                            index += 1
                print(f"tape_archive={day.isoformat()} ticks={index}", flush=True)
            if index != expected:
                # The dataset builder intentionally drops non-monotonic ids;
                # its per-minute count oracle can include those few source rows.
                if index + skipped_nonmonotonic != expected:
                    raise RuntimeError(
                        f"tick tape count mismatch: wrote {index}, expected {expected}, "
                        f"skipped {skipped_nonmonotonic}")
                view.resize(index * TICK_STRUCT.size)
            view.flush()
        finally:
            view.close()

    metadata = {
        "start_ms": start_ms,
        "end_ms": end_ms,
        "count": index,
        "record_size": TICK_STRUCT.size,
        "skipped_nonmonotonic": skipped_nonmonotonic,
        "archives": archives,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    return metadata


def tape_timestamp(view: mmap.mmap, index: int) -> int:
    return struct.unpack_from("<q", view, index * TICK_STRUCT.size)[0]


def lower_bound_timestamp(view: mmap.mmap, count: int, timestamp_ms: int) -> int:
    lo, hi = 0, count
    while lo < hi:
        mid = (lo + hi) // 2
        if tape_timestamp(view, mid) < timestamp_ms:
            lo = mid + 1
        else:
            hi = mid
    return lo


def run_whole_session(live: LiveRun) -> str | None:
    try:
        lib = live.strategy.lib
        if lib.strategy_stream_push_ticks(
                live.state, live.tick_pointer, live.tick_count) != 0:
            return str(engine_error(live.strategy, live.state, "stream ticks"))
        return None
    except Exception as exc:
        return str(exc)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--handoff", action="append", required=True,
                        help="minute-aligned UTC ISO-8601; repeat three times")
    parser.add_argument("--end", required=True, help="exclusive UTC session end")
    parser.add_argument("--tape", type=Path, required=True)
    parser.add_argument("--cases", default="corpus/validation/*/strategy.dylib")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--workers", type=int, default=min(12, os.cpu_count() or 4))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    handoffs = [parse_utc(value) for value in args.handoff]
    if len(handoffs) != 3 or len(set(handoffs)) != 3:
        raise SystemExit("provide exactly three distinct --handoff values")
    if any(value.second or value.microsecond for value in handoffs):
        raise SystemExit("handoffs must be aligned to one-minute boundaries")
    end = parse_utc(args.end)
    if any(value >= end for value in handoffs):
        raise SystemExit("every handoff must precede end")
    handoffs.sort()
    handoff_ms = [utc_ms(value) for value in handoffs]
    end_ms = utc_ms(end)

    tape_meta = build_tick_tape(args.data_root, args.tape, handoff_ms[0], end_ms)
    ohlcv = args.data_root / DEFAULT_OHLCV
    warmups = [load_ohlcv_slice(ohlcv, end_ms=value - 1) for value in handoff_ms]
    full, n_full = load_ohlcv_slice(ohlcv, end_ms=end_ms - 1)
    cases = discover_cases(args.cases, args.limit)
    started = time.perf_counter()

    tape_handle = args.tape.open("r+b")
    tape_view = mmap.mmap(tape_handle.fileno(), 0, access=mmap.ACCESS_WRITE)
    tape_count = tape_meta["count"]
    starts = [lower_bound_timestamp(tape_view, tape_count, value)
              for value in handoff_ms]

    strategies: dict[str, Strategy] = {}
    configs: dict[str, dict] = {}
    live_runs: list[LiveRun] = []
    results = [{"handoff": value.isoformat(), "results": []} for value in handoffs]
    results_by_session = [dict() for _ in handoffs]

    for case_index, case in enumerate(cases, 1):
        label = str(case.relative_to(REPO_ROOT))
        so = case / ("strategy.dylib" if (case / "strategy.dylib").is_file()
                     else "strategy.so")
        strategy = Strategy(so)
        config = load_case_config(case)
        strategies[label] = strategy
        configs[label] = config
        for session_index, (warmup, n_warmup) in enumerate(warmups):
            item = {"case": label, "config": config}
            results_by_session[session_index][label] = item
            state = strategy.lib.strategy_create(None)
            try:
                configure_state(strategy, state, config)
                if strategy.lib.strategy_stream_begin(
                        state, warmup, n_warmup,
                        config["input_tf"].encode(),
                        config["script_tf"].encode()) != 0:
                    raise engine_error(strategy, state, "stream begin")
                byte_offset = starts[session_index] * TICK_STRUCT.size
                pointer = ctypes.cast(
                    ctypes.addressof(ctypes.c_char.from_buffer(tape_view, byte_offset)),
                    ctypes.POINTER(TradeTickC))
                live_runs.append(LiveRun(
                    label, session_index, strategy, state, pointer,
                    tape_count - starts[session_index]))
            except Exception as exc:
                item["status"] = "error"
                item["error"] = str(exc)
                strategy.lib.strategy_free(state)
        if case_index % 20 == 0 or case_index == len(cases):
            print(f"warmup=[{case_index}/{len(cases)}]", flush=True)

    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(run_whole_session, live): live for live in live_runs}
        completed = 0
        for future in as_completed(futures):
            live = futures[future]
            live.error = future.result()
            completed += 1
            if completed % 10 == 0 or completed == len(live_runs):
                print(f"whole_session=[{completed}/{len(live_runs)}]", flush=True)

    runs_by_case: dict[str, list[LiveRun]] = {}
    for live in live_runs:
        runs_by_case.setdefault(live.case_label, []).append(live)

    for case_index, case in enumerate(cases, 1):
        label = str(case.relative_to(REPO_ROOT))
        strategy = strategies[label]
        batch = run_batch(strategy, full, n_full, configs[label])
        for live in runs_by_case.get(label, []):
            item = results_by_session[live.session_index][label]
            report = ReportC()
            try:
                if live.error:
                    raise RuntimeError(live.error)
                lib = strategy.lib
                if lib.strategy_stream_advance_time(live.state, end_ms) != 0:
                    raise engine_error(strategy, live.state, "stream advance")
                if lib.strategy_stream_end(live.state, 0) != 0:
                    raise engine_error(strategy, live.state, "stream end")
                if lib.strategy_stream_fill_report(
                        live.state, ctypes.byref(report)) != 0:
                    raise engine_error(strategy, live.state, "stream report")
                stream = _report_to_dict(report)
                item["comparison"] = compare_reports(batch, stream)
                item["status"] = "ok"
            except Exception as exc:
                item["status"] = "error"
                item["error"] = str(exc)
            finally:
                strategy.lib.report_free(ctypes.byref(report))
                strategy.lib.strategy_free(live.state)
        if case_index % 20 == 0 or case_index == len(cases):
            print(f"report=[{case_index}/{len(cases)}]", flush=True)

    tape_view.close()
    tape_handle.close()
    session_payloads = []
    for session_index, handoff in enumerate(handoffs):
        session_results = [results_by_session[session_index][
            str(case.relative_to(REPO_ROOT))] for case in cases]
        ok = [item for item in session_results if item.get("status") == "ok"]
        session_payloads.append({
            "handoff": handoff.isoformat(),
            "tick_offset": starts[session_index],
            "tick_count": tape_count - starts[session_index],
            "summary": {
                "cases": len(session_results), "ok": len(ok),
                "errors": len(session_results) - len(ok),
                "input_bars_equal": sum(item["comparison"]["input_bars_equal"] for item in ok),
                "script_bars_equal": sum(item["comparison"]["script_bars_equal"] for item in ok),
                "trade_count_equal": sum(item["comparison"]["trade_count_equal"] for item in ok),
                "structural_equal": sum(item["comparison"]["structural_equal"] for item in ok),
            },
            "results": session_results,
        })

    payload = {
        "experiment": {
            "end": end.isoformat(), "ohlcv": str(ohlcv),
            "full_bars": n_full, "tape": str(args.tape),
            "tape_metadata": tape_meta, "workers": args.workers,
            "elapsed_seconds": time.perf_counter() - started,
        },
        "sessions": session_payloads,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(json.dumps([session["summary"] for session in session_payloads],
                     sort_keys=True), flush=True)
    print(f"report={args.output}", flush=True)
    return 0 if all(session["summary"]["errors"] == 0
                    for session in session_payloads) else 1


if __name__ == "__main__":
    raise SystemExit(main())
