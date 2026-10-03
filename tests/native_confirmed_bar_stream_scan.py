#!/usr/bin/env python3
"""Compare each probe with itself on genuine confirmed bars, never grade TV.

Build all engine/test targets first, then run:
  python3 tests/native_confirmed_bar_stream_scan.py --build-dir build \
      --out /tmp/confirmed-scan --jobs 6

The batch runner's own setup, input resolution and report lifecycle are used
in both modes. Only its final run_backtest_full call is replaced for streaming.
One observer-equipped shared library is used for batch and both handoffs.
All floats are compared as binary64, in physical action/trade order. Corpus
chart bounds are expanded to the corresponding genuine 1m input buckets;
neither OHLCV nor ticks are fabricated. --manifest accepts population cases
with probe, directory, feed and optional input_tf fields. Run this on a build
host, not on the supervisor's Mac.
Saved evidence may be compressed as .archives/<case>.tar.gz under the output
directory. --recompare reads those modes without extracting them to disk.
The first stream runs before batch so an actual native refusal needs no batch
replay. Supported cases still compare all three fresh runs in one library.
"""

import argparse
from bisect import bisect_left, bisect_right
from concurrent.futures import ProcessPoolExecutor, as_completed
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import multiprocessing
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from run_strategy import (
    BarC, ReportC, Strategy, DEFAULT_OHLCV, inputs_run_kwargs,
    _apply_range_end_regime, _feed_timestamps, _infer_bar_interval_ms,
    _load_bars, _load_tv_entry_span, _tf_seconds, _tv_entry_emit_window,
    _tv_first_bar, ensure_derived,
)
from native_live_equivalence_e2e import (
    Strategy as ObserverStrategy, first_difference, read_rows, write_json,
)

SETTINGS = {}
FEED_CACHE = {}


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compiler_recipe(build):
    entries = json.loads((build / "compile_commands.json").read_text())
    entry = next(row for row in entries
                 if Path(row["file"]).name == "native_live_equivalence_observer.cpp")
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
        raise RuntimeError("build all targets, including native_live_equivalence_observer")
    return flags, object_file, entry["directory"]


def compile_probe(directory, destination):
    build = SETTINGS["build"]
    flags, observer, working_directory = compiler_recipe(build)
    archive = str(build / "lib/libpineforge.a")
    if sys.platform.startswith("linux"):
        link = ["-shared", "-Wl,--whole-archive", archive, "-Wl,--no-whole-archive"]
    elif sys.platform == "darwin":
        link = ["-dynamiclib", "-Wl,-force_load," + archive]
    else:
        raise RuntimeError("only Linux and macOS build hosts are supported")
    command = flags + [str(directory / "generated.cpp"), str(observer),
                       str(SETTINGS["pump"])] + link + ["-o", str(destination)]
    write_json(destination.with_suffix(".compile.json"), command)
    with destination.with_suffix(".compile.log").open("w") as log:
        subprocess.run(command, cwd=working_directory, stdout=log,
                       stderr=subprocess.STDOUT, check=True)


def load_feed(path, start=None, end=None):
    key = str(path)
    if key not in FEED_CACHE:
        bars, count, _ = _load_bars(path)
        FEED_CACHE[key] = (bars, [bars[index].timestamp for index in range(count)])
    full, timestamps = FEED_CACHE[key]
    begin = bisect_left(timestamps, start) if start is not None else 0
    finish = bisect_right(timestamps, end) if end is not None else len(timestamps)
    count = max(0, finish - begin)
    return (BarC * count).from_address(ctypes.addressof(full) + begin * ctypes.sizeof(BarC)), count


def resolve_case(case):
    directory = Path(case["directory"]).resolve()
    input_path = directory / "inputs.json"
    params = json.loads(input_path.read_text()) if input_path.is_file() else {}
    chart, kwargs = inputs_run_kwargs(params, directory, DEFAULT_OHLCV)
    span = _load_tv_entry_span(directory, params)
    if span:
        _apply_range_end_regime(directory, params, kwargs)
        timestamps = _feed_timestamps(chart, ohlcv_start_ms=kwargs.get("ohlcv_start_ms"),
                                      ohlcv_end_ms=kwargs.get("ohlcv_end_ms"))
        interval = _infer_bar_interval_ms(chart)
        first_bar = _tv_first_bar(directory, params)
        window = _tv_entry_emit_window(timestamps, span[0], span[1], interval,
                                      None if first_bar is None else first_bar[0])
        if window.tv_first_bar_ms is not None:
            signal = _tv_entry_emit_window(timestamps, span[0], span[1], interval).start_ms
            if signal - interval > timestamps[0]:
                kwargs["trade_start_time_ms"] = signal
    chart_bars, count = load_feed(chart, kwargs.get("ohlcv_start_ms"),
                                 kwargs.get("ohlcv_end_ms"))
    if not count:
        raise RuntimeError("resolved chart feed is empty")
    original_input_tf = kwargs.get("input_tf") or case.get("input_tf") or "15"
    script_tf = kwargs.get("script_tf") or case.get("script_tf") or original_input_tf
    feed = Path(case.get("feed", ROOT / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv")).resolve()
    input_tf = case.get("input_tf", "1")
    if "feed" in case:
        bars, count = load_feed(feed, kwargs.get("ohlcv_start_ms"), kwargs.get("ohlcv_end_ms"))
    else:
        start = chart_bars[0].timestamp
        end = chart_bars[count - 1].timestamp + _tf_seconds(original_input_tf) * 1000 - 60000
        if SETTINGS.get("start_ms") is not None:
            start = max(start, SETTINGS["start_ms"])
        if SETTINGS.get("count") is not None:
            end = min(end, start + (SETTINGS["count"] - 1) * 60000)
        bars, count = load_feed(feed, start, end)
    kwargs.update(input_tf=input_tf, script_tf=script_tf, preloaded_bars=(bars, count))
    return directory, params, feed, kwargs, bars, count


def stream_splits(bars, count, script_tf, chart_opens=None):
    period = _tf_seconds(script_tf) * 1000
    candidates = []
    for fraction in SETTINGS["splits"]:
        index = max(1, min(count - 1, int(count * fraction)))
        if chart_opens:
            boundary = bisect_left(chart_opens, bars[index].timestamp)
            if boundary == len(chart_opens):
                raise RuntimeError("no chart boundary after the requested handoff")
            index = bisect_left(bars, chart_opens[boundary], key=lambda bar: bar.timestamp)
            if index >= count:
                raise RuntimeError("chart handoff is beyond the supplied input feed")
        else:
            while index < count - 1 and bars[index].timestamp % period:
                index += 1
        if index not in candidates:
            candidates.append(index)
    if len(candidates) != len(SETTINGS["splits"]):
        raise RuntimeError("feed is too short for distinct aligned handoffs")
    return candidates


def check_library_freshness(build):
    archive = build / "lib/libpineforge.a"
    paths = [ROOT / "CMakeLists.txt"]
    for directory in (ROOT / "src", ROOT / "include"):
        paths.extend(path for path in directory.rglob("*") if path.is_file())
    stale = [str(path) for path in paths if path.stat().st_mtime_ns > archive.stat().st_mtime_ns]
    if stale:
        raise RuntimeError("stale static library; rebuild all targets: " + ", ".join(stale[:5]))


def make_observer(library):
    library.equivalence_retain_events.argtypes = [ctypes.c_void_p]
    library.equivalence_retain_events.restype = ctypes.c_int
    library.equivalence_export_actions.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    library.equivalence_export_actions.restype = ctypes.c_int
    return SimpleNamespace(library=library)


def error_text(library, state, operation, result):
    detail = library.strategy_get_last_error(state)
    return f"{operation}: rc={result}: {(detail or b'').decode()}"


def is_stream_refusal(message):
    return ("strategy_stream_begin: rc=" in message
            or ("strategy_stream_push_bar[" in message and "]: rc=" in message)
            or "bar_magnifier argument" in message)


def run_mode(strategy, observer, directory, params, kwargs, destination, split=None):
    library = strategy.lib
    original_create = library.strategy_create
    original_run = library.run_backtest_full
    snapshot = {}
    created_state = []

    def create(parameter_bytes):
        state = original_create(parameter_bytes)
        if not state or library.equivalence_retain_events(state) != 0:
            raise RuntimeError("could not enable native readback")
        created_state.append(state)
        return state

    def stream_run(state, bars, count, input_tf, script_tf, magnifier, samples, distribution, report):
        if magnifier:
            raise RuntimeError("stream API has no run_backtest_full bar_magnifier argument")
        result = library.strategy_stream_begin(state, bars, split, input_tf, script_tf)
        if result:
            raise RuntimeError(error_text(library, state, "strategy_stream_begin", result))
        failed_index = ctypes.c_int(-1)
        result = library.confirmed_scan_push_bars(state, bars, split, count, ctypes.byref(failed_index))
        if result:
            raise RuntimeError(error_text(library, state,
                               f"strategy_stream_push_bar[{failed_index.value}]", result))
        result = library.strategy_stream_fill_report(state, report)
        if result:
            raise RuntimeError(error_text(library, state, "strategy_stream_fill_report", result))

    def capture(report):
        state = created_state[-1]
        result = library.equivalence_export_actions(state, str(destination / "actions.jsonl").encode())
        if result:
            raise RuntimeError(error_text(library, state, "equivalence_export_actions", result))
        snapshot.update(ObserverStrategy.snapshot(observer, state, report))

    destination.mkdir(parents=True, exist_ok=True)
    library.strategy_create = create
    if split is not None:
        library.run_backtest_full = stream_run
    try:
        strategy.run(directory / "unused.csv", params=params, on_report=capture, **kwargs)
        write_json(destination / "state.json", snapshot)
        return {"actions": read_rows(destination / "actions.jsonl"), "state": snapshot}
    finally:
        library.strategy_create = original_create
        library.run_backtest_full = original_run


def describe_difference(batch, stream, split, bars):
    split_time = bars[split].timestamp
    return compare_saved(batch, stream, split, split_time)


def semantic_actions(rows, split_time):
    fields = ("action", "leg", "contracts", "price", "id")
    return [{"timestamp": row["timestamp"],
             "order": {field: row["order"][field] for field in fields}}
            for row in rows if row["timestamp"] >= split_time]


def semantic_state(state):
    return {key: [{field: value for field, value in row.items() if field != "entry_incarnation"}
                  for row in state[key]] if key == "closed_trades" else state[key]
            for key in ("closed_trades", "terminal_trades", "totals", "equity_curve", "metrics")}


def compare_saved(batch, stream, split, split_time):
    expected = {"actions": semantic_actions(batch["actions"], split_time),
                "state": semantic_state(batch["state"])}
    actual = {"actions": semantic_actions(stream["actions"], split_time),
              "state": semantic_state(stream["state"])}
    for section in ("actions", "state"):
        if json.dumps(expected[section], sort_keys=True, separators=(",", ":")) == json.dumps(
                actual[section], sort_keys=True, separators=(",", ":")):
            continue
        difference = first_difference(expected[section], actual[section], section)
        if difference:
            timestamp = None
            if section == "actions":
                path = difference["path"]
                if "[" in path:
                    index = int(path.split("[", 1)[1].split("]", 1)[0])
                    for rows in (expected[section], actual[section]):
                        if index < len(rows):
                            timestamp = rows[index]["timestamp"]
                            break
                elif difference.get("expected_next"):
                    timestamp = difference["expected_next"][0]["timestamp"]
                elif difference.get("actual_next"):
                    timestamp = difference["actual_next"][0]["timestamp"]
            elif "closed_trades[" in difference["path"] or "terminal_trades[" in difference["path"]:
                trade_section = "closed_trades" if "closed_trades[" in difference["path"] else "terminal_trades"
                index = int(difference["path"].split("[", 1)[1].split("]", 1)[0])
                for state in (expected["state"], actual["state"]):
                    if index < len(state[trade_section]):
                        timestamp = state[trade_section][index]["exit_time"]
                        break
            return {"split": split, "split_time_ms": split_time,
                    "time_ms": timestamp, **difference}
    return None


def read_saved_mode(directory, mode):
    saved = directory / mode
    if (saved / "state.json").is_file():
        return {"actions": read_rows(saved / "actions.jsonl"),
                "state": json.loads((saved / "state.json").read_text())}
    archive_path = directory.parent / ".archives" / (directory.name + ".tar.gz")
    with tarfile.open(archive_path, "r:gz") as archive:
        prefix = directory.name + "/" + mode + "/"
        actions = archive.extractfile(prefix + "actions.jsonl")
        state = archive.extractfile(prefix + "state.json")
        if actions is None or state is None:
            raise RuntimeError("incomplete saved mode: " + prefix)
        return {"actions": [json.loads(line) for line in actions if line.strip()],
                "state": json.load(state)}


def recompare(output):
    results = []
    for path in sorted(output.glob("*/result.json")):
        result = json.loads(path.read_text())
        if result.get("proof_gap") and is_stream_refusal(result.get("error", "")):
            result.update(result="STREAM-UNSUPPORTED", refusal=result.pop("error"))
            result.pop("proof_gap", None)
            write_json(path, result)
        if result.get("splits") and result["result"] != "STREAM-UNSUPPORTED":
            batch = read_saved_mode(path.parent, "batch")
            differences = []
            for row in result["splits"]:
                stream = read_saved_mode(path.parent, f"stream-{row['index']}")
                difference = compare_saved(batch, stream, row["index"], row["time_ms"])
                row.update(result="FAIL" if difference else "PASS", difference=difference)
                if difference:
                    differences.append(difference)
            result["result"] = "FAIL" if differences else "PASS"
            result.pop("first_divergence", None)
            if differences:
                result["first_divergence"] = min(differences,
                    key=lambda row: row["time_ms"] if row["time_ms"] is not None else float("inf"))
            write_json(path, result)
        results.append(result)
    write_json(output / "results.json", sorted(results, key=lambda row: row["probe"]))
    totals = {name: sum(row["result"] == name for row in results)
              for name in ("PASS", "FAIL", "STREAM-UNSUPPORTED")}
    write_json(output / "summary.json", {"total": len(results), **totals})
    print(json.dumps(totals), flush=True)


def scan_case(case):
    started = time.monotonic()
    name = case["probe"]
    destination = SETTINGS["out"] / name.replace("/", "__").replace(":", "_")
    destination.mkdir(parents=True, exist_ok=True)
    result = {"probe": name, "result": "FAIL", "splits": []}
    try:
        directory, params, feed, kwargs, bars, count = resolve_case(case)
        chart_opens = None
        if case.get("chart_feed"):
            chart_bars, chart_count = load_feed(Path(case["chart_feed"]).resolve())
            chart_opens = [chart_bars[index].timestamp for index in range(chart_count)]
        splits = stream_splits(bars, count, kwargs["script_tf"], chart_opens)
        library_path = destination / "strategy.so"
        if not SETTINGS["reuse"] or not library_path.is_file():
            compile_probe(directory, library_path)
        strategy = Strategy(library_path)
        library = strategy.lib
        library.strategy_stream_begin.argtypes = [ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
                                                 ctypes.c_char_p, ctypes.c_char_p]
        library.strategy_stream_begin.restype = ctypes.c_int
        library.strategy_stream_fill_report.argtypes = [ctypes.c_void_p, ctypes.POINTER(ReportC)]
        library.strategy_stream_fill_report.restype = ctypes.c_int
        library.confirmed_scan_push_bars.argtypes = [ctypes.c_void_p, ctypes.POINTER(BarC),
                                                    ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        library.confirmed_scan_push_bars.restype = ctypes.c_int
        observer = make_observer(library)
        result.update(library_sha256=sha256(library_path), generated_sha256=sha256(directory / "generated.cpp"),
                      feed=str(feed), count=count, first_ms=bars[0].timestamp,
                      last_ms=bars[count - 1].timestamp, input_tf=kwargs["input_tf"], script_tf=kwargs["script_tf"])
        batch = None
        differences = []
        for split in splits:
            try:
                stream = run_mode(strategy, observer, directory, params, kwargs,
                                  destination / f"stream-{split}", split)
            except RuntimeError as error:
                refusal = str(error)
                if not is_stream_refusal(refusal):
                    raise
                result.update(result="STREAM-UNSUPPORTED", refusal=refusal)
                break
            if batch is None:
                batch = run_mode(strategy, observer, directory, params, kwargs, destination / "batch")
                result["batch_actions"] = len(batch["actions"])
            difference = describe_difference(batch, stream, split, bars)
            result["splits"].append({"index": split, "time_ms": bars[split].timestamp,
                                     "result": "FAIL" if difference else "PASS",
                                     "difference": difference, "stream_actions": len(stream["actions"])})
            if difference:
                differences.append(difference)
        else:
            result["result"] = "FAIL" if differences else "PASS"
            if differences:
                result["first_divergence"] = min(differences,
                    key=lambda row: row["time_ms"] if row["time_ms"] is not None else float("inf"))
    except Exception as error:
        result.update(result="FAIL", error=f"{type(error).__name__}: {error}", proof_gap=True)
    result["elapsed_seconds"] = round(time.monotonic() - started, 3)
    write_json(destination / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--splits", nargs=2, type=float, default=[0.8, 0.9])
    parser.add_argument("--probes", nargs="+")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--reuse-libraries", action="store_true")
    parser.add_argument("--start-ms", type=int, help="explicit diagnostic window only")
    parser.add_argument("--count", type=int, help="explicit diagnostic window only")
    parser.add_argument("--recompare", action="store_true", help="regrade saved same-engine evidence without replay")
    arguments = parser.parse_args()
    if arguments.jobs < 1 or arguments.jobs > 12 or any(not 0 < value < 1 for value in arguments.splits):
        parser.error("jobs must be 1..12 and split fractions must be inside (0,1)")
    output = arguments.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if arguments.recompare:
        recompare(output)
        return 0
    build = arguments.build_dir.resolve()
    check_library_freshness(build)
    ensure_derived()
    flags, _, working_directory = compiler_recipe(build)
    pump = output / "confirmed_scan_pump.o"
    subprocess.run(flags + ["-c", str(ROOT / "tests/native_confirmed_bar_stream_scan.cpp"),
                            "-o", str(pump)], cwd=working_directory, check=True)
    SETTINGS.update(build=build, out=output, splits=arguments.splits, pump=pump,
                    reuse=arguments.reuse_libraries, start_ms=arguments.start_ms,
                    count=arguments.count)
    if arguments.manifest:
        cases = json.loads(arguments.manifest.read_text())
    else:
        cases = [{"probe": path.name, "directory": str(path)}
                 for path in sorted((ROOT / "corpus/validation").iterdir())
                 if (path / "generated.cpp").is_file()]
    if arguments.probes:
        cases = [case for case in cases if case["probe"] in arguments.probes]
    write_json(output / "manifest.json", {
        "started_utc": datetime.now(timezone.utc).isoformat(), "cases": cases,
        "archive_sha256": sha256(build / "lib/libpineforge.a"),
        "feed_sha256": sha256(ROOT / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv"),
        "split_fractions": arguments.splits, "jobs": arguments.jobs,
        "observed_ticks": False, "tv_grading": False,
        "diagnostic_start_ms": arguments.start_ms, "diagnostic_count": arguments.count,
    })
    results = []
    with ProcessPoolExecutor(max_workers=arguments.jobs,
                             mp_context=multiprocessing.get_context("fork")) as executor:
        futures = {executor.submit(scan_case, case): case for case in cases}
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            print(json.dumps(result), flush=True)
            write_json(output / "results.json", sorted(results, key=lambda row: row["probe"]))
    totals = {name: sum(row["result"] == name for row in results)
              for name in ("PASS", "FAIL", "STREAM-UNSUPPORTED")}
    write_json(output / "summary.json", {"total": len(results), **totals})
    print(json.dumps(totals), flush=True)
    return 1 if totals["FAIL"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
