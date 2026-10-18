#!/usr/bin/env python3
"""CR-150 L0B: live allocation-site census, for the Linux lab only.

No builds, code generation, feed derivation or corpus writes. Prerequisites:
a fresh complete build, Valgrind/Massif + vgdb, and prebuilt corpus modules.
Use --battery tests/fixtures/flat_memory --corpus corpus. Raw snapshots,
commands, module hashes, measured populations and every growing stack are kept
under --output (a NEW directory). Missing proof exits 2; non-flat proof exits 1.
Caller OHLCV and explicitly declared closed-trade history are the only admissions.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

ROOT = Path(__file__).resolve().parents[1]
MARKER = "PF_HEAP_CENSUS "
SCHEMA = "pineforge-heap-census/v1"


@dataclass
class Snapshot:
    heap_bytes: int
    sites: dict[str, int]


def _frame(text: str) -> str:
    text = re.sub(r"\b0x[0-9a-fA-F]+:\s*", "", text)
    text = re.sub(r"(\.(?:cpp|hpp|cc|c|h)):\d+(?::\d+)?", r"\1", text)
    return text.replace(str(ROOT) + "/", "").strip()


def parse_massif(text: str) -> Snapshot:
    """One explicit detailed_snapshot; automatic/peak/last snapshots are not evidence.

    Walk the complete tree, counting each allocation only at its leaf. Reject
    truncated trees and unaccounted bytes instead of silently losing a site.
    Threshold/depth leaves remain visible and fail the gate as L19j.
    """
    if len(re.findall(r"^snapshot=\d+$", text, re.M)) != 1:
        raise ValueError("expected exactly one checkpoint snapshot")
    totals = re.findall(r"^mem_heap_B=(\d+)$", text, re.M)
    if len(totals) != 1 or "heap_tree=detailed\n" not in text:
        raise ValueError("checkpoint has no detailed heap tree")
    nodes = []
    for line in text.splitlines():
        match = re.fullmatch(r"( *)n(\d+): (\d+) (.+)", line)
        if match:
            indent, children, size, label = match.groups()
            nodes.append((len(indent), int(children), int(size), _frame(label)))
    if not nodes or nodes[0][1] == 0:
        raise ValueError("unexpanded allocation root")
    sites: dict[str, int] = {}

    def walk(index: int, path: tuple[str, ...], parent_indent: int) -> tuple[int, int]:
        if index >= len(nodes):
            raise ValueError("truncated allocation tree")
        indent, children, size, label = nodes[index]
        if indent <= parent_indent:
            raise ValueError("invalid allocation-tree nesting")
        index += 1
        stack = path + (label,)
        if children == 0:
            site = " <- ".join(stack[1:])
            sites[site] = sites.get(site, 0) + size
        else:
            child_bytes = 0
            for _ in range(children):
                index, count = walk(index, stack, indent)
                child_bytes += count
            if child_bytes != size:
                raise ValueError("allocation-tree children do not sum to parent")
        return index, size

    end, total = walk(0, (), -1)
    if end != len(nodes) or total != int(totals[0]) or sum(sites.values()) != total:
        raise ValueError("snapshot live-byte total disagrees with allocation tree")
    return Snapshot(total, sites)


# Attribution is deliberately symbol-specific. A whole-file match would hide
# a newly discovered adapter allocation under an already assigned lane.
# Unknown, ambiguous, stripped and threshold leaves all go to L19j.
# Tokens name pushing functions or element types, never data members. A tuple
# is an all-of rule: try_emplace allocates both uint64_t unfolded entries and
# PlacementSnapshot nodes, and only the former belongs to L19h. The unsigned
# long spellings below are the demangled forms of that uint64_t element type.
OWNERS = (
    ("L19d", ("OriginRoster::push_back",)),
    ("L19h", ("BracketRoster::",
              ("PlacementTable::try_emplace", "vector<unsigned long,"),
              ("PlacementTable::try_emplace", "vector<unsigned long long,"),
              ("PlacementTable::replace", "vector<unsigned long,"),
              ("PlacementTable::replace", "vector<unsigned long long,"))),
    ("L19e", ("CommandObservation", "Journal::append", "Journal::reserve", "Journal::next_sequence")),
    ("L19c", ("CohortReceipt",)),
    ("L19b", ("ReceiptKey",)),
    ("L19a", ("WorkingRequestCore::index_committed",)),
    ("L19l", ("PineExecutionAdapter::cohort_for", "CohortFacts", "CohortRoster",
              "PineExecutionAdapter::note_exit_rank_key", "PlacementTable::fold_sequence",
              "PlacementTable::SequenceMinima",
              ("PineExecutionAdapter::on_applied", "basic_string<char", ", long"))),
    ("L19j", ("PlacementSnapshot", "PlacementTable::place")),
    ("L19i (or L3/L4)", ("NativeExecutionConsumer::begin_timeframe_subscriptions",)),
    ("L19k", ("tradingview_multi_day_stamps", "TimeframeAggregator::set_native_periods")),
    ("L3", ("PineScheduler::capture_begin", "native_module::run_batch")),
    ("L4", ("NativeExecutionConsumer::script_bucket_completions", "PineScheduler::run_begin")),
    ("L15a/L15b", ("PineStrategyHost::set_symbol_feed",)),
    ("L16", ("tradingview_magnifier_bars",)),
    ("L7b/L17b", ("compute_equity_stats",)),
    ("L8a/L8c/L18", ("vector<pineforge::Trade", "pineforge::Trade::Trade",
                     "BacktestEngine::record_close_trade", "NativeExecutionConsumer::closed_rows_digest",
                     "PineExecutionAdapter::set_exit_phase")),
    ("L6b-1/L6b-2/L17a", ("BacktestEngine::record_equity_point", "vector<pf_equity_point_s",
                         "vector<pf_equity_point_t")),
    ("L6b-3", ("NativeExecutionConsumer::record_script_report_point", "PineStrategyHost::scheduler_record_broker_hash",
               "BacktestEngine::output_bar", "BacktestEngine::record_output_event",
               "BacktestEngine::trace(", "BacktestEngine::intern_trace_name", "BacktestEngine::fill_trace_section")),
    ("L9a/L9s/L17a", ("BacktestEngine::fill_metrics_section", "BacktestEngine::fill_trades_section",
                     "NativeExecutionConsumer::capture_presentation")),
)


def owner(site: str) -> tuple[str, bool]:
    if "below massif's threshold" in site or "???" in site:
        return "L19j", False
    if "census_chart_ohlcv(" in site or "census_feed_close_ms(" in site:
        return "caller OHLCV", True
    for lane, symbols in OWNERS:
        if any(all(token in site for token in ((symbol,) if isinstance(symbol, str) else symbol))
               for symbol in symbols):
            return lane, False
    return "L19j", False


def growth(low: Snapshot, high: Snapshot, low_units: int, high_units: int) -> list[dict]:
    if high_units <= low_units:
        raise ValueError("comparison did not increase its measured population")
    rows = []
    for site in sorted(low.sites.keys() | high.sites.keys()):
        before, after = low.sites.get(site, 0), high.sites.get(site, 0)
        if after <= before:
            continue
        lane, admitted = owner(site)
        rows.append(dict(site=site, lane=lane, admitted=admitted,
                         low_bytes=before, high_bytes=after, delta_bytes=after - before,
                         bytes_per_unit=(after - before) / (high_units - low_units)))
    return sorted(rows, key=lambda row: (-row["delta_bytes"], row["site"]))


def flat(rows: list[dict]) -> bool:
    return all(row["admitted"] for row in rows)


def id_cost(reused_low: Snapshot, unique_low: Snapshot,
            reused_high: Snapshot, unique_high: Snapshot,
            low_ids: int, high_ids: int) -> dict:
    """Signed difference of differences. Ordinary trade work cancels, not just id strings.

    Both reused populations have ONE distinct id. Denominator is therefore
    (high_ids - 1) - (low_ids - 1). Keep negative sites too: per-id layout and
    capacity effects can offset a positive site and must not disappear.
    """
    if high_ids <= low_ids:
        raise ValueError("Q9 needs two increasing distinct-id populations")
    sites = []
    all_sites = (reused_low.sites.keys() | unique_low.sites.keys() |
                 reused_high.sites.keys() | unique_high.sites.keys())
    for site in sorted(all_sites):
        low = unique_low.sites.get(site, 0) - reused_low.sites.get(site, 0)
        high = unique_high.sites.get(site, 0) - reused_high.sites.get(site, 0)
        if high != low:
            sites.append(dict(site=site, lane=owner(site)[0], low_extra_bytes=low,
                              high_extra_bytes=high,
                              bytes_per_additional_id=(high - low) / (high_ids - low_ids)))
    delta = ((unique_high.heap_bytes - reused_high.heap_bytes) -
             (unique_low.heap_bytes - reused_low.heap_bytes))
    return dict(denominator_ids=high_ids - low_ids,
                bytes_per_additional_id=delta / (high_ids - low_ids), sites=sites)


def validate_sink_ids(path: Path, trades: int, period: int, long_id: bool) -> None:
    """Check Q9's identities after streaming, when old row access is unavailable.

    This small fixture check is not the version-1 format acceptance suite.
    It validates the trailer digest/count and each T's id, qty and closed bit.
    The production reference reader remains L6a/L0's responsibility.
    """
    digest, count, trailer = hashlib.sha256(), 0, None
    with path.open("rb") as source:
        first = source.readline()
        header = json.loads(first)
        if header.get("schema") != "pf-report-sink" or header.get("version") != 1:
            raise ValueError("Q9 sink has no version-1 header")
        digest.update(first)
        fields = next(record["fields"] for record in header["records"] if record["type"] == "T")
        for raw in source:
            record = json.loads(raw)
            if isinstance(record, dict):
                if trailer is not None or record.get("trailer") != "pf-report-sink":
                    raise ValueError("invalid Q9 sink trailer")
                trailer = record
                continue
            if trailer is not None:
                raise ValueError("Q9 sink has bytes after its trailer")
            digest.update(raw)
            if record[0] == "T":
                trade = dict(zip(fields, record[1:]))
                ordinal = count if period == 0 else count % period
                expected = ("census-entry-" if long_id else "E") + f"{ordinal:014d}"
                count += 1
                if (trade["n"] != count or trade["entry_id"] != expected or
                        trade["qty"] != 1 or trade["open_at_end"] is not False):
                    raise ValueError("Q9 sink changed entry identity or trade shape")
    if (trailer is None or trailer.get("sha256") != digest.hexdigest() or
            trailer.get("counts", {}).get("T") != trades or count != trades):
        raise ValueError("Q9 sink digest or trade population disagrees")


def _sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fresh(build: Path, executable: Path) -> None:
    library = build / "lib/libpineforge.a"
    if not library.is_file() or not executable.is_file():
        raise ValueError(f"missing complete lab build: {library} / {executable}")
    sources = [ROOT / "CMakeLists.txt"]
    sources += [path for directory in ("src", "include")
                for path in (ROOT / directory).rglob("*") if path.is_file()]
    if any(path.stat().st_mtime_ns > library.stat().st_mtime_ns for path in sources):
        raise ValueError("stale libpineforge.a; lab must complete an all-target build")
    if executable.stat().st_mtime_ns < library.stat().st_mtime_ns:
        raise ValueError(f"stale statically linked executable/module: {executable}")
    if executable.name == "test_heap_census_unique_ids" and any(
            path.stat().st_mtime_ns > executable.stat().st_mtime_ns
            for path in (ROOT / "tests/test_heap_census_unique_ids.cpp", ROOT / "tests/CMakeLists.txt")):
        raise ValueError("stale census fixture executable")


def profile(command: list[str], directory: Path, timeout: int) -> tuple[Snapshot, dict]:
    directory.mkdir()
    snapshot = directory / "checkpoint.massif"
    prefix = directory / "vgdb"
    argv = ["valgrind", "--tool=massif", "--vgdb=yes", f"--vgdb-prefix={prefix}",
            "--pages-as-heap=no", "--stacks=no", "--time-unit=B",
            "--depth=100", "--threshold=0.0", "--peak-inaccuracy=0.0",
            f"--massif-out-file={directory / 'automatic.massif'}",
            f"--log-file={directory / 'valgrind.log'}", *command]
    (directory / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
    env = dict(os.environ, PYTHONMALLOC="malloc", PYTHONDONTWRITEBYTECODE="1",
               PF_HEAP_CENSUS_SINK_PATH=str(directory / "report-sink.jsonl"))
    started = time.monotonic()
    output = bytearray()
    metadata = None
    with (directory / "stderr.log").open("wb") as stderr:
        child = subprocess.Popen(argv, cwd=ROOT, env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=stderr)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(child.stdout, selectors.EVENT_READ)
                while metadata is None:
                    if time.monotonic() - started > timeout:
                        raise ValueError(f"checkpoint timed out: {directory}")
                    if not ready.select(1):
                        continue
                    chunk = os.read(child.stdout.fileno(), 65536)
                    if not chunk:
                        raise ValueError(f"child exited before live checkpoint: {directory}")
                    output.extend(chunk)
                    for line in output.decode("utf-8", "replace").splitlines():
                        if line.startswith(MARKER) and output.endswith(b"\n"):
                            metadata = json.loads(line[len(MARKER):])
                            break
            if metadata.get("phase") != "native_return":
                raise ValueError("wrong census phase")
            monitor = ["vgdb", f"--vgdb-prefix={prefix}", f"--pid={child.pid}",
                       "detailed_snapshot", str(snapshot)]
            reply = subprocess.run(monitor, capture_output=True, timeout=60, check=False)
            (directory / "vgdb.stdout").write_bytes(reply.stdout)
            (directory / "vgdb.stderr").write_bytes(reply.stderr)
            if reply.returncode != 0 or not snapshot.is_file():
                raise ValueError(f"Massif checkpoint failed: {directory}")
            measured = parse_massif(snapshot.read_text())
            child.stdin.write(b"\n")
            child.stdin.flush()
            tail, _ = child.communicate(timeout=max(1, timeout - (time.monotonic() - started)))
            output.extend(tail)
            if child.returncode != 0:
                raise ValueError(f"profiled run failed ({child.returncode}): {directory}")
            if output.decode("utf-8", "replace").count(MARKER) != 1:
                raise ValueError("profiled run did not have exactly one checkpoint")
            metadata.update(command=argv, snapshot_sha256=_sha(snapshot),
                            live_heap_bytes=measured.heap_bytes)
            (directory / "measurement.json").write_text(json.dumps(metadata, indent=2) + "\n")
            return measured, metadata
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            (directory / "stdout.log").write_bytes(output)


def corpus_worker(probe: Path, bars_limit: int, output: Path) -> int:
    """Drive the real corpus CLI, preserving all of its per-probe settings.

    Wrap its documented on_report callback: after a successful engine call,
    before report_free/strategy_free. Prefix only the primary chart; other
    _load_bars feed loads use its final timestamp as their upper bound. This
    is a retained-heap census, not a peak/load-transient measurement.
    """
    sys.path.insert(0, str(ROOT / "scripts"))
    import run_strategy as runner

    original_load, original_run, original_init = runner._load_bars, runner.Strategy.run, runner.Strategy.__init__
    chart = {}
    sink_state = {"sink": False, "streamable_policy": False, "history_declared": None}
    mappings = []

    class Descriptor(ctypes.Structure):
        _fields_ = [("struct_size", ctypes.c_uint32), ("version", ctypes.c_uint32),
                    ("path", ctypes.c_char_p), ("run_id", ctypes.c_char_p), ("scratch_dir", ctypes.c_char_p)]

    def initialize(self, *args, **kwargs):
        original_init(self, *args, **kwargs)
        lib = self.lib
        if hasattr(lib, "strategy_reads_trade_history"):
            lib.strategy_reads_trade_history.restype = ctypes.c_int
            sink_state["history_declared"] = lib.strategy_reads_trade_history() != 0
            sink_state["streamable_policy"] = not sink_state["history_declared"]
        if hasattr(lib, "strategy_set_report_sink_v1"):
            setter = lib.strategy_set_report_sink_v1
            setter.argtypes = [ctypes.c_void_p, ctypes.POINTER(Descriptor)]
            setter.restype = ctypes.c_int
            original_full = lib.run_backtest_full

            def full(*call_args):
                descriptor = Descriptor(ctypes.sizeof(Descriptor), 1,
                    os.fsencode(output / "report-sink.jsonl"), b"L0B-corpus", None)
                if setter(call_args[0], ctypes.byref(descriptor)) != 0:
                    raise ValueError("corpus sink installation refused")
                sink_state["sink"] = True
                return original_full(*call_args)

            lib.run_backtest_full = full

    def load(path, **kwargs):
        if chart:
            previous = kwargs.get("ohlcv_end_ms")
            kwargs["ohlcv_end_ms"] = min(previous, chart["end"]) if previous is not None else chart["end"]
        bars, count, identity = original_load(path, **kwargs)
        if not chart:
            if count < bars_limit:
                raise ValueError(f"corpus prefix too short: {probe} has {count}, needs {bars_limit}")
            count = bars_limit
            chart.update(bars=count, end=int(bars[count - 1].timestamp))
        if count == 0:
            return bars, count, identity
        # Only caller OHLCV may grow. Put these exact bytes in a mapping so
        # Massif's malloc census does not mistake a ctypes input tape for an
        # engine leak. Engine-owned copies remain malloc allocations and fail.
        # Input-loading/transient RSS stays the L0 acceptance harness's job.
        path = output / f"caller-ohlcv-{len(mappings)}.bin"
        with path.open("w+b") as tape:
            tape.write(memoryview(bars).cast("B")[:ctypes.sizeof(runner.BarC) * count])
            tape.flush()
            mapping = mmap.mmap(tape.fileno(), 0)
        mappings.append(mapping)
        return (runner.BarC * count).from_buffer(mapping), count, identity

    def run(self, *args, **kwargs):
        previous = kwargs.get("on_report")

        def checkpoint(report):
            if not chart or report.input_bars_processed != bars_limit:
                raise ValueError("corpus run did not process the requested chart prefix")
            print(MARKER + json.dumps(dict(bars=int(report.input_bars_processed),
                  trades=int(report.total_trades), ids=None, phase="native_return",
                  report_copy_included=not sink_state["sink"], **sink_state)), flush=True)
            if os.read(sys.stdin.fileno(), 1) != b"\n":
                raise ValueError("profiler did not release corpus checkpoint")
            if previous is not None:
                previous(report)

        kwargs["on_report"] = checkpoint
        return original_run(self, *args, **kwargs)

    # Derived feeds must be made fresh ONCE by CORE-TOP before starting the
    # census. Refuse a derivation attempt rather than racing its fixed .new.
    def no_derive():
        import derive_corpus_feeds as feeds
        if (not feeds.SOURCE_1M.is_file() or not feeds._derived_cache_is_fresh(
                feeds.SOURCE_1M, (feeds.DERIVED_15M, feeds.DERIVED_15M_WINDOW), feeds.DERIVATION_STAMP)):
            raise ValueError("CORE-TOP must prepare derived corpus feeds before census")

    runner._load_bars, runner.Strategy.run, runner.ensure_derived = load, run, no_derive
    runner.Strategy.__init__ = initialize
    sys.argv = [str(ROOT / "scripts/run_strategy.py"), str(probe),
                "--output", str(output / "engine_trades.csv"), "--no-trim-output"]
    return runner.main()


def _markdown(report: dict) -> str:
    lines = ["# CR-150 L0B measured heap census", "", f"Status: **{report['status']}**.", "",
             f"Engine HEAD: `{report['head']}`. Schema: `{SCHEMA}`.", "",
             "Live Massif requested bytes at native_return; allocator overhead and RSS are separate.",
             "Every positive allocation-site delta is listed in census.json. Unknown sites belong to L19j.", "",
             "| Comparison | Unit | Growing sites | Non-admitted sites |", "|---|---|---:|---:|"]
    for case in report["comparisons"]:
        rows = case["sites"]
        lines.append(f"| {case['name']} | {case['unit']} | {len(rows)} | "
                     f"{sum(not row['admitted'] for row in rows)} |")
    lines += ["", "## Q9 bytes per additional distinct entry id", "",
              "Matched bars/trades/id lengths; signed difference of unique minus reused slopes.", "",
              "| ID length (bytes) | B/additional id | Denominator (ids) |", "|---:|---:|---:|"]
    for row in report["q9"]:
        lines.append(f"| {row['id_bytes']} | {row['bytes_per_additional_id']:.6f} | {row['denominator_ids']} |")
    for case in report["comparisons"]:
        lines += ["", f"## Sites: {case['name']}", ""]
        for row in case["sites"]:
            lines += [f"- {row['lane']}: +{row['delta_bytes']} B, "
                      f"{row['bytes_per_unit']:.6f} B/{case['unit']}; admitted={row['admitted']}.",
                      "", "```text", row["site"], "```", ""]
    if report["errors"]:
        lines += ["", "## Open proof", "", *[f"- {error}" for error in report["errors"]]]
    lines += ["", "No PASS is inferred from missing tools, missing modules, an empty corpus or a short prefix.", ""]
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--battery", type=Path, required=True)
    parser.add_argument("--corpus", type=Path)
    parser.add_argument("--bars", type=int, default=4000, help="bar population N; compared with 4N")
    parser.add_argument("--trade-bars", type=int, default=40000, help="fixed bars at both trade rungs")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args(argv)
    build = args.build_dir.resolve()
    output = (args.output or build / f"heap-census-{time.time_ns()}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(schema=SCHEMA, status="OPEN", head=subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        comparisons=[], q9=[], runs={}, errors=[], corpus_requested=args.corpus is not None)
    try:
        if sys.platform != "linux" or not all(shutil.which(tool) for tool in ("valgrind", "vgdb")):
            raise ValueError("heap census requires the Linux lab with valgrind and vgdb")
        if args.bars < 300 or args.trade_bars < 20000 or args.timeout <= 0:
            raise ValueError("need N >= 300, fixed trade bars >= 20000 and a positive timeout")
        battery = args.battery.resolve()
        if not (battery / "unique_entry_id.pine").is_file():
            raise ValueError("battery has no unique-entry-id fixture")
        manifest = json.loads((battery / "heap_census_battery.json").read_text())
        required = {"reused_id", "moved_exits", "oca_brackets", "pyramiding", "partial_exits",
                    "split_magnifier", "split_no_magnifier", "routed_security", "nday_security", "foreign_security"}
        if manifest.get("schema") != "pineforge-heap-census-battery/v1" or set(manifest.get("native_modes", [])) != required:
            raise ValueError("incomplete census battery")
        executable = build / "bin/test_heap_census_unique_ids"
        _fresh(build, executable)
        report.update(library_sha256=_sha(build / "lib/libpineforge.a"),
                      fixture_sha256=_sha(executable), battery_sha256=_sha(battery / "heap_census_battery.json"),
                      unique_pine_sha256=_sha(battery / "unique_entry_id.pine"))
        samples: dict[str, Snapshot] = {}

        def native(name, mode, bars, trades, period, long_id=False):
            print(f"heap census: {name}", flush=True)
            measured, metadata = profile([str(executable), mode, str(bars), str(trades),
                str(period), str(int(long_id)), "--wait", "yes"], output / name, args.timeout)
            if metadata["bars"] != bars:
                raise ValueError(f"wrong bar population: {name}")
            if metadata.get("sink") and mode in ("unique_ids", "reused_id"):
                validate_sink_ids(output / name / "report-sink.jsonl", trades, period, long_id)
            samples[name], report["runs"][name] = measured, metadata
            return name

        def compare(name, low, high, unit):
            before, after = report["runs"][low][unit], report["runs"][high][unit]
            sites = growth(samples[low], samples[high], before, after)
            for row in sites:
                if row["lane"] == "L19j" and any(token in row["site"] for token in ("PlacementSnapshot", "PlacementTable::place")):
                    if name == "moved_exits":
                        row["lane"] = "L19g"
                    elif name in ("split_magnifier", "split_no_magnifier"):
                        row["lane"] = "L19f"
            history = all(report["runs"][key].get("history_declared") is True for key in (low, high))
            if history:
                for row in sites:
                    if row["lane"] == "L8a/L8c/L18":
                        row.update(admitted=True, admission="explicit closed-trade history module")
            report["comparisons"].append(dict(name=name, low=low, high=high, unit=unit,
                low_units=before, high_units=after, history_exception=history, sites=sites))

        for mode in manifest["native_modes"]:
            low = native(mode + "-N", mode, args.bars, 10, 1)
            high = native(mode + "-4N", mode, 4 * args.bars, 10, 1)
            compare(mode, low, high, "bars")
        for long_id in (False, True):
            for mode, period in (("reused_id", 1), ("unique_ids", 0)):
                names = []
                for trades in (10, 10000):
                    name = f"{mode}-{'long' if long_id else 'short'}-{trades}"
                    native(name, mode, args.trade_bars, trades, period, long_id)
                    expected_ids = trades if period == 0 else 1
                    if report["runs"][name]["trades"] != trades or report["runs"][name]["ids"] != expected_ids:
                        raise ValueError(f"Q9 populations disagree: {name}")
                    names.append(name)
                compare(f"{mode}-{'long' if long_id else 'short'}", *names, "trades")
            kind = "long" if long_id else "short"
            keys = [f"{mode}-{kind}-{trades}" for trades in (10, 10000)
                    for mode in ("reused_id", "unique_ids")]
            lengths = {report["runs"][key]["id_bytes"] for key in keys}
            if len(lengths) != 1:
                raise ValueError("Q9 id lengths are not equal")
            cost = id_cost(*(samples[key] for key in keys), 10, 10000)
            report["q9"].append(dict(id_bytes=lengths.pop(), runs=keys, **cost))
        if args.corpus is not None:
            corpus = args.corpus.resolve()
            probes = sorted(path for path in (corpus / "validation").glob("*")
                            if (path / "strategy.pine").is_file() or (path / "generated.cpp").is_file())
            if not probes:
                raise ValueError("corpus has no source-marked validation probes")
            report["corpus_population"] = [str(probe.relative_to(corpus)) for probe in probes]
            for index, probe in enumerate(probes):
                try:
                    module = probe / "strategy.so"
                    _fresh(build, module)
                    names = []
                    for bars in (args.bars, 4 * args.bars):
                        name = f"corpus-{index:04d}-{bars}"
                        measured, metadata = profile([sys.executable, "-B", str(Path(__file__).resolve()),
                            "--worker", str(probe), str(bars), str(output / name)], output / name, args.timeout)
                        metadata.update(module_sha256=_sha(module), probe=str(probe))
                        samples[name], report["runs"][name] = measured, metadata
                        names.append(name)
                    compare(str(probe.relative_to(corpus)), *names, "bars")
                except (ValueError, OSError, subprocess.SubprocessError) as error:
                    report["errors"].append(f"{probe}: {error}")
        if not report["errors"]:
            is_flat = all(flat(case["sites"]) for case in report["comparisons"])
            streaming = all(run.get("sink") and (run.get("streamable_policy") or run.get("history_declared") is True)
                            for run in report["runs"].values())
            report["memory_verdict"] = "FLAT" if is_flat else "RED"
            if is_flat and not streaming:
                report["errors"].append("flat samples lack a sink or an explicit trade-history policy")
            else:
                report["status"] = ("PASS" if is_flat else "RED") if args.corpus is not None else "BATTERY_ONLY"
    except (ValueError, OSError, KeyError, StopIteration, TypeError, subprocess.SubprocessError) as error:
        report["errors"].append(str(error))
    finally:
        (output / "census.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        (output / "CENSUS.md").write_text(_markdown(report))
    print(f"heap census: {report['status']} — {output / 'CENSUS.md'}", flush=True)
    return 0 if report["status"] == "PASS" else (1 if report["status"] == "RED" else 2)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--worker":
        raise SystemExit(corpus_worker(Path(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4])))
    raise SystemExit(main())
