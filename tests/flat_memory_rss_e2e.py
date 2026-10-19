#!/usr/bin/env python3
"""CR-150 lane L0 red acceptance tests: flat process rest, real run_json path.

Covers DESIGN.md section 7 (a), (b), (f), (g), (h), (j) and the INPUTS item 1
guard (run_json must not release a borrowed symbol-feed array before the run
ends).

Every measured case runs docker/run_json.py as a child over a real generated
strategy module and reads that child's peak RSS with os.wait4/ru_maxrss. It
then subtracts every mapped OHLCV byte (chart bars, feed bars, feed close
times), which is the design's "rest" (DESIGN.md 7): OHLCV may grow, the rest
must not.

RED ON 3fc010a8b, for the stated reason, on every case:
  (a) with no .bin support the same bars go in as CSV; rest grows about 477.5 B
      per chart bar at report time, about 455 MiB at 1M bars. Red on slope.
  (b) about 1,323.4 B per trade in the engine in-run, plus 922.2 B per trade
      in Python report dicts (measured, F39 box 6, 1.6.0-rc.1).
  (f) 56 B per feed bar copied in the engine plus Python tuples/arrays.
  (g) the curve three times in the engine (72 B per point) and 288.8 B per
      script bar in run_json's dicts (measured, F39 box 6, 1.6.0-rc.1).
  (h) the path vector and its transients, about 72-120 B per input bar.
  (j) every row stays in trades_ (240 B rows) and in the report copies;
      the measured engine in-run slope is 1,323.4 B per trade.
  borrowed: run_json never calls strategy_set_symbol_feed_borrowed_v1 and
      clears the feed arrays before the run.

Nothing here runs on a laptop: lab queue or AWS spot (DESIGN.md 7). Modules are
built with the release-shaped recipe; this script only loads them. It does not
build or fake the engine.

Usage (lab queue, after the modules are built):

  python3 tests/flat_memory_rss_e2e.py --module-dir build/flat_memory_modules

  # one case, with a module supplied explicitly
  python3 tests/flat_memory_rss_e2e.py --module-dir DIR --cases a \\
      --module macd=/path/to/macd.so

ctest registers the same command under the l4-pending, slow and mem labels
(tests/CMakeLists.txt), so a red row never breaks the current CI build.
"""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MIB = 1024 * 1024
KIB = 1024

# Q4 requires a measured rest(1k) plus 32 MiB, rounded up to 16 MiB.
# PROVISIONAL ESTIMATE ONLY: 64 MiB for CPython + ctypes + the module + 1k
# bars. L13 must replace this estimate with the recorded lab/spot measurement
# and its provenance. No measurement was made in this lane. The provisional
# bound remains 96 MiB; the 1 MiB slope assertion is the acceptance gate.
REST_1K_ESTIMATE_BYTES = 64 * MIB


def round_up(value: int, unit: int) -> int:
    return (value + unit - 1) // unit * unit


REST_1K_BOUND_BYTES = round_up(REST_1K_ESTIMATE_BYTES + 32 * MIB, 16 * MIB)

BASE_TS = 1_699_999_980_000  # divisible by 60000: calendar-aligned 1m opens
MINUTE_MS = 60_000
FEED_TF_MS = 240 * MINUTE_MS
SLOPE_LIMIT = 1 * MIB  # bytes, like peak_rss_bytes and mapped_bytes


class SetupError(RuntimeError):
    """The test could not run: a missing module, an unusable run_json call.

    Never reported as a red: a red must be the memory behaviour under test.
    """


class CaseFailure(AssertionError):
    """A measured assertion failed: this is the red."""


# ── input generation ────────────────────────────────────────────────────


def price_at(index: int, shape: str) -> float:
    if shape == "chop":
        return 100.0 + (1.0 if (index // 3) % 2 == 0 else -1.0)
    if shape == "sine":
        return 100.0 + 5.0 * math.sin(index / 50.0)
    if shape == "slow":
        phase = index % 20000
        triangle = phase if phase < 10000 else 20000 - phase
        return 100.0 + 0.0005 * triangle
    raise ValueError(shape)


def bar_row(index: int, shape: str, step_ms: int = MINUTE_MS):
    price = price_at(index, shape)
    return (BASE_TS + index * step_ms, price, price + 0.5, price - 0.5, price, 1000.0)


def write_chart_csv(path: Path, n: int, shape: str) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("timestamp", "open", "high", "low", "close", "volume"))
        for i in range(n):
            ts, o, h, l, c, v = bar_row(i, shape)
            writer.writerow((ts, repr(o), repr(h), repr(l), repr(c), repr(v)))


def bar_bytes(row) -> bytes:
    ts, o, h, l, c, v = row
    return struct.pack("<5dq", o, h, l, c, v, ts)


def canonical_sidecar(document: dict) -> bytes:
    return (
        json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode()


def write_chart_bin(path: Path, n: int, shape: str) -> int:
    """One .bin plus its sidecar (DESIGN.md 1.2-1.7). Returns the file size."""
    digest = hashlib.sha256()
    first_ts = last_ts = None
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("wb") as handle:
        for i in range(n):
            row = bar_row(i, shape)
            payload = bar_bytes(row)
            handle.write(payload)
            digest.update(payload)
            if first_ts is None:
                first_ts = row[0]
            last_ts = row[0]
    os.replace(tmp, path)
    sidecar = {
        "format": "pf_bar_t/v1",
        "record_size": 48,
        "symbol": "TEST:FLAT",
        "tf": "1",
        "count": n,
        "first_ts": first_ts,
        "last_ts": last_ts,
        "sha256": digest.hexdigest(),
    }
    path.with_name(path.name + ".json").write_bytes(canonical_sidecar(sidecar))
    return path.stat().st_size


def write_feed_csv(path: Path, n: int) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("timestamp", "open", "high", "low", "close", "volume", "time_close"))
        for i in range(n):
            ts = BASE_TS + i * FEED_TF_MS
            price = 200.0 + 3.0 * math.sin(i / 17.0)
            writer.writerow(
                (ts, repr(price), repr(price + 1.0), repr(price - 1.0), repr(price),
                 "100", ts + FEED_TF_MS)
            )


def write_feed_bin(bar_path: Path, close_path: Path, n: int) -> tuple[int, int]:
    bar_digest = hashlib.sha256()
    close_digest = hashlib.sha256()
    first_ts = last_ts = None
    bar_tmp = bar_path.with_name(bar_path.name + ".tmp")
    close_tmp = close_path.with_name(close_path.name + ".tmp")
    with bar_tmp.open("wb") as bars, close_tmp.open("wb") as closes:
        for i in range(n):
            ts = BASE_TS + i * FEED_TF_MS
            price = 200.0 + 3.0 * math.sin(i / 17.0)
            payload = struct.pack("<5dq", price, price + 1.0, price - 1.0, price, 100.0, ts)
            close = struct.pack("<q", ts + FEED_TF_MS)
            bars.write(payload)
            closes.write(close)
            bar_digest.update(payload)
            close_digest.update(close)
            if first_ts is None:
                first_ts = ts
            last_ts = ts
    os.replace(bar_tmp, bar_path)
    os.replace(close_tmp, close_path)
    sidecar = {
        "format": "pf_bar_t/v1",
        "record_size": 48,
        "symbol": "FEED:XS",
        "tf": "240",
        "count": n,
        "first_ts": first_ts,
        "last_ts": last_ts,
        "sha256": bar_digest.hexdigest(),
        "close_sha256": close_digest.hexdigest(),
    }
    bar_path.with_name(bar_path.name + ".json").write_bytes(canonical_sidecar(sidecar))
    close_path.with_name(close_path.name + ".json").write_bytes(
        canonical_sidecar({**sidecar, "sha256": close_digest.hexdigest()})
    )
    return bar_path.stat().st_size, close_path.stat().st_size


def write_feed_index(path: Path, feed_name: str) -> None:
    path.write_text(
        json.dumps(
            {"symbols": {"FEED:XS": {"syminfo": {}, "feeds": {"240": feed_name}}}},
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )


# ── measurement ─────────────────────────────────────────────────────────


class Run:
    def __init__(self, peak_rss_bytes: int, returncode: int, stdout: bytes,
                 stderr: bytes, wall_seconds: float):
        self.peak_rss_bytes = peak_rss_bytes
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.wall_seconds = wall_seconds

    def document(self) -> dict:
        try:
            return json.loads(self.stdout.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise SetupError(
                "run_json did not print a JSON report: "
                f"{error}; stderr={self.stderr.decode('utf-8', 'replace')[-4000:]}"
            ) from None

    def failure_text(self) -> str:
        return (self.stderr or self.stdout).decode("utf-8", "replace")[-4000:]


def measured(command: list[str], cwd: Path, env: dict) -> Run:
    """Run one child and read its own peak RSS (Linux KiB, Darwin bytes)."""
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        started = time.monotonic()
        child = subprocess.Popen(command, cwd=str(cwd), env=env, stdout=out, stderr=err)
        pid, status, usage = os.wait4(child.pid, 0)
        wall = time.monotonic() - started
        child.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        stdout = out.read()
        stderr = err.read()
    peak = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    return Run(peak, child.returncode, stdout, stderr, wall)


def parse_help(run_json: Path, cwd: Path, env: dict) -> str:
    result = measured([sys.executable, str(run_json), "--help"], cwd, env)
    return result.stdout.decode("utf-8", "replace")


class Harness:
    def __init__(self, run_json: Path, module_dir: Path, work: Path,
                 modules: dict[str, Path], keep: bool):
        self.run_json = run_json
        self.module_dir = module_dir
        self.work = work
        self.modules = modules
        self.keep = keep
        self.env = dict(os.environ)
        self.env.setdefault("PYTHONHASHSEED", "0")
        # A released borrowed array must be visible as a crash or a wrong
        # report, not hidden by a lucky allocator.
        self.env["PYTHONMALLOC"] = "debug"
        self.env["MALLOC_PERTURB_"] = "165"
        self.help_text = parse_help(run_json, self.run_json.parent.parent, self.env)
        self.has_bin = self._probe_bin_support()
        self.has_sink = "--report-sink" in self.help_text
        self.measurements: list[tuple[str, float]] = []

    # -- capability probe (DESIGN.md 7 (a)) -----------------------------

    def _probe_bin_support(self) -> bool:
        module = next(iter(self.modules.values()), None)
        if module is None:
            candidate = self.module_dir / "macd.so"
            module = candidate if candidate.is_file() else None
        if module is not None and module.is_file():
            result = measured(
                [sys.executable, str(self.run_json), "--so", str(module),
                 "--capabilities-json"],
                self.run_json.parent.parent, self.env)
            if result.returncode == 0:
                try:
                    capabilities = result.document().get("capabilities", {})
                except SetupError:
                    capabilities = {}
                if isinstance(capabilities, dict):
                    version = capabilities.get("bars_bin_version")
                    return (isinstance(version, int) and not isinstance(version, bool)
                            and version >= 1)
        # QC7a: the member certifies the whole .bin contract. A flag in
        # --help does not open this gate before the version member exists.
        return False

    # -- one run --------------------------------------------------------

    def run(self, args: list[str], label: str) -> tuple[Run, int]:
        command = [sys.executable, str(self.run_json), *args]
        result = measured(command, self.run_json.parent.parent, self.env)
        if result.returncode != 0:
            raise SetupError(f"{label}: run_json exited {result.returncode}: "
                             f"{result.failure_text()}")
        return result, 0

    def module(self, name: str) -> Path:
        path = self.modules.get(name)
        if path is None:
            path = self.module_dir / f"{name}.so"
        if not path.is_file():
            raise SetupError(
                f"module {name!r} is not present at {path}. Build it with the "
                "release-shaped MODULE recipe (tests/CMakeLists.txt:3014-3041) "
                "or pass --module {name}=/path/to/{name}.so"
            )
        return path

    def chart_input(self, tag: str, n: int, shape: str, *,
                    force_csv: bool = False) -> tuple[Path, int]:
        """CSV, or .bin when supported and permitted by the case's contract."""
        if self.has_bin and not force_csv:
            path = self.work / f"{tag}.bin"
            if not path.exists():
                write_chart_bin(path, n, shape)
            return path, 48 * n
        path = self.work / f"{tag}.csv"
        if not path.exists():
            write_chart_csv(path, n, shape)
        return path, 0

    def feed_input(self, tag: str, n: int) -> tuple[Path, int]:
        """The --symbol-feeds index plus mapped bytes (bars and close file)."""
        if self.has_bin:
            bars = self.work / f"{tag}.feed.bin"
            closes = self.work / f"{tag}.feed.close.bin"
            if not bars.exists():
                write_feed_bin(bars, closes, n)
            index = self.work / f"{tag}.symbols.json"
            if not index.exists():
                write_feed_index(index, bars.name)
            return index, 48 * n + 8 * n
        csv_path = self.work / f"{tag}.feed.csv"
        if not csv_path.exists():
            write_feed_csv(csv_path, n)
        index = self.work / f"{tag}.symbols.json"
        if not index.exists():
            write_feed_index(index, csv_path.name)
        return index, 0

    def input_args(self, path: Path) -> list[str]:
        flag = "--ohlcv-bin" if path.suffix == ".bin" else "--ohlcv"
        return [flag, str(path)]

    def sink_args(self, tag: str) -> tuple[list[str], Path | None]:
        if not self.has_sink:
            return [], None
        path = self.work / f"{tag}.sink"
        args = ["--report-sink", str(path), "--run-id", f"l0-{tag}"]
        if "--spill-dir" in self.help_text:
            args += ["--spill-dir", str(self.work)]
        return args, path

    def rest(self, label: str, args: list[str], sink_args: list[str],
             mapped_bytes: int) -> tuple[int, dict]:
        result, _ = self.run(args + sink_args, label)
        rest_bytes = result.peak_rss_bytes - mapped_bytes
        self.measurements.append((label, rest_bytes))
        print(f"  {label}: peak {result.peak_rss_bytes / MIB:.1f} MiB, "
              f"mapped {mapped_bytes / MIB:.1f} MiB, rest {rest_bytes / MIB:.1f} MiB, "
              f"{result.wall_seconds:.1f} s")
        return rest_bytes, result.document()

    def sink_records(self, path: Path | None) -> dict[str, list[list]]:
        if path is None or not path.exists():
            return {}
        lines = path.read_text(encoding="utf-8").splitlines()
        if len(lines) < 2:
            raise SetupError(f"{path}: the sink has no records")
        records: dict[str, list[list]] = {}
        for line in lines[1:-1]:
            record = json.loads(line)
            records.setdefault(record[0], []).append(record)
        trailer = json.loads(lines[-1])
        if trailer.get("trailer") != "pf-report-sink":
            raise SetupError(f"{path}: the last line is not a trailer")
        return records


# ── report comparison masks (DESIGN.md 4.2) ─────────────────────────────

MASKED_KEYS = {
    "elapsed_seconds", "timing", "throughput", "phase_timing", "fingerprint",
    "source_bytes_sha256", "evaluated_source_values_sha256", "ohlcv",
    "report_sink",
}


def masked(value):
    if isinstance(value, dict):
        return {key: masked(item) for key, item in value.items() if key not in MASKED_KEYS}
    if isinstance(value, list):
        return [masked(item) for item in value]
    return value


def comparable_report(document: dict) -> dict:
    """The parts both a full report and a summary report carry."""
    keys = ("summary", "metrics", "diagnostics", "report_shape", "report_window")
    return masked({key: document[key] for key in keys if key in document})


def assert_equal(label: str, left, right) -> None:
    if left != right:
        a = json.dumps(left, sort_keys=True, separators=(",", ":"))[:2000]
        b = json.dumps(right, sort_keys=True, separators=(",", ":"))[:2000]
        raise CaseFailure(f"{label}: reports differ\n  left:  {a}\n  right: {b}")


def assert_slope(label: str, values: list[tuple[str, int]], limit: int = SLOPE_LIMIT):
    worst = max(values, key=lambda item: item[1])[1] - min(
        values, key=lambda item: item[1])[1]
    if worst > limit:
        detail = ", ".join(f"{name}={size / MIB:.1f} MiB" for name, size in values)
        raise CaseFailure(
            f"{label}: rest is not flat: spread {worst / MIB:.2f} MiB > "
            f"{limit / MIB:.0f} MiB ({detail})")


# ── cases ───────────────────────────────────────────────────────────────


def base_args(module: Path, input_tf: str = "1", script_tf: str = "1") -> list[str]:
    return ["--so", str(module), "--input-tf", input_tf, "--script-tf", script_tf]


def case_a(harness: Harness) -> None:
    """(a) rest flat in bars, with and without --report-sink."""
    print("(a) rest flat in chart bars")
    module = harness.module("macd")
    for sink_on in ((False, True) if harness.has_sink else (False,)):
        values: list[tuple[str, int]] = []
        for n in (1000, 100_000, 1_000_000):
            path, mapped = harness.chart_input(f"a{n}", n, "slow")
            args = base_args(module) + harness.input_args(path)
            sink_args, _ = harness.sink_args(f"a{n}-{'sink' if sink_on else 'full'}")
            rest_bytes, _ = harness.rest(
                f"a/{n}/{'sink' if sink_on else 'full'}", args,
                sink_args if sink_on else [], mapped)
            values.append((str(n), rest_bytes))
        assert_slope("(a) bars", values)
        one_k = next(size for name, size in values if name == "1000")
        if one_k > REST_1K_BOUND_BYTES:
            raise CaseFailure(
                f"(a) rest(1k) {one_k / MIB:.1f} MiB > the estimated Q4 bound "
                f"{REST_1K_BOUND_BYTES / MIB:.0f} MiB")


def case_b(harness: Harness) -> None:
    """(b) rest flat in trades: about ten vs about ten thousand, same bars."""
    print("(b) rest flat in trades")
    module = harness.module("every_k")
    bars = 66_000
    path, mapped = harness.chart_input(f"b{bars}", bars, "chop")
    values: list[tuple[str, int]] = []
    counts: dict[str, int] = {}
    for k, label in ((6600, "10"), (6, "11k")):
        args = base_args(module) + harness.input_args(path) + [
            "--inputs", json.dumps({"k": k, "Bars per trade": k})]
        sink_args, _ = harness.sink_args(f"b-{label}")
        rest_bytes, document = harness.rest(
            f"b/{label}-trades", args, sink_args if harness.has_sink else [], mapped)
        values.append((label, rest_bytes))
        counts[label] = int(document.get("summary", {}).get("total_trades", -1))
    if counts["10"] > 100 or counts["11k"] < 1000:
        raise SetupError(
            f"(b) the k input was not applied: {counts}; the trades axis is not "
            "exercised (expected about 10 and about 11,000)")
    spread = abs(values[0][1] - values[1][1])
    if spread > SLOPE_LIMIT:
        raise CaseFailure(
            f"(b) trades: rest differs by {spread / MIB:.2f} MiB between "
            f"{values[0][0]} and {values[1][0]} trades")


def case_f(harness: Harness) -> None:
    """(f) symbol feeds flat: one request.security on another symbol."""
    print("(f) rest flat in feed bars")
    module = harness.module("xsym")
    chart_path, chart_mapped = harness.chart_input("f-chart", 1000, "sine")
    values: list[tuple[str, int]] = []
    for n in (1000, 100_000, 1_000_000):
        index, feed_mapped = harness.feed_input(f"f{n}", n)
        args = (base_args(module) + harness.input_args(chart_path) +
                ["--symbol-feeds", str(index)])
        sink_args, _ = harness.sink_args(f"f{n}")
        rest_bytes, _ = harness.rest(f"f/{n}", args,
                                     sink_args if harness.has_sink else [],
                                     chart_mapped + feed_mapped)
        values.append((str(n), rest_bytes))
    assert_slope("(f) feed bars", values)

    if harness.has_bin:
        # The .bin feed and the CSV feed must produce the same report.
        csv_feed = harness.work / "f-compare.feed.csv"
        if not csv_feed.exists():
            write_feed_csv(csv_feed, 100_000)
        csv_index = harness.work / "f-compare.csv.symbols.json"
        if not csv_index.exists():
            write_feed_index(csv_index, csv_feed.name)
        module_path = module
        common = base_args(module_path) + harness.input_args(chart_path)
        bin_index, _ = harness.feed_input("f100000", 100_000)
        bin_doc = harness.run(common + ["--symbol-feeds", str(bin_index)],
                              "f/bin")[0].document()
        csv_doc = harness.run(common + ["--symbol-feeds", str(csv_index)],
                              "f/csv")[0].document()
        assert_equal("(f) .bin vs CSV feed", masked(bin_doc), masked(csv_doc))


def case_g(harness: Harness) -> None:
    """(g) selected window flat over almost all data."""
    print("(g) rest flat in a selected window")
    module = harness.module("macd")
    inventory = harness.work / "selected.inventory.json"
    if not inventory.exists():
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
        inventory.write_text(json.dumps({
            "schema": "pineforge-request-feed-inventory/v1",
            "source_sha256": "0" * 64,
            "artifact_sha256": digest,
            "primary_chart_timeframe": None,
            "entries": [],
        }, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    values: list[tuple[str, int]] = []
    for n in (1000, 100_000, 1_000_000):
        # QC6a: selected-window runs stay on CSV in 1.7.0. This path owns
        # no mapping, so chart_input returns mapped = 0.
        path, mapped = harness.chart_input(f"g{n}", n, "slow", force_csv=True)
        start = BASE_TS + 1 * MINUTE_MS
        # E is exclusive: the planner refuses any supplied row at/after E.
        end = BASE_TS + n * MINUTE_MS
        args = base_args(module) + harness.input_args(path) + [
            "--report-policy", "selected-window/v1",
            "--window-start-ms", str(start),
            "--window-end-ms", str(end),
            "--preroll-bars", "0",
            "--fed-start-ms", str(BASE_TS),
            "--request-feed-inventory", str(inventory),
        ]
        sink_args, sink_path = harness.sink_args(f"g{n}")
        rest_bytes, document = harness.rest(
            f"g/{n}", args, sink_args if harness.has_sink else [], mapped)
        values.append((str(n), rest_bytes))
        if harness.has_sink:
            records = harness.sink_records(sink_path)
            full = harness.run(args, f"g/{n}/full")[0].document()
            assert_equal("(g) summary metrics", comparable_report(document),
                         comparable_report(full))
            curve = full.get("equity_curve", [])
            trades = full.get("trades", [])
            if len(records.get("E", [])) != len(curve):
                raise CaseFailure(
                    f"(g) sink has {len(records.get('E', []))} E records, the "
                    f"full report has {len(curve)} curve points")
            if len(records.get("T", [])) != len(trades):
                raise CaseFailure(
                    f"(g) sink has {len(records.get('T', []))} T records, the "
                    f"full report has {len(trades)} trades")
    assert_slope("(g) selected window", values)


def case_h(harness: Harness) -> None:
    """(h) magnifier with finer input is flat."""
    print("(h) rest flat in the magnifier path")
    module = harness.module("macd")
    values: list[tuple[str, int]] = []
    for n in (15_000, 150_000, 1_500_000):
        path, mapped = harness.chart_input(f"h{n}", n, "sine")
        args = base_args(module, input_tf="1", script_tf="15") + harness.input_args(path) + [
            "--bar-magnifier", "1", "--magnifier-samples", "4",
            "--magnifier-dist", "endpoints",
        ]
        sink_args, _ = harness.sink_args(f"h{n}")
        rest_bytes, _ = harness.rest(f"h/{n}", args,
                                     sink_args if harness.has_sink else [], mapped)
        values.append((str(n), rest_bytes))
    assert_slope("(h) magnifier", values)


def case_j(harness: Harness) -> None:
    """(j) scale-out of one open position is flat, trades equal."""
    print("(j) rest flat while a position scales out")
    module = harness.module("scale_out")
    chart_path, chart_mapped = harness.chart_input("j-chart", 100_000, "chop")
    values: list[tuple[str, int]] = []
    exit_counts: dict[int, int] = {}
    full_trades = None
    for exits, bars in ((1000, 1000), (100_000, 100_000)):
        path = chart_path if bars == 100_000 else harness.chart_input(
            f"j{bars}", bars, "chop")[0]
        mapped = chart_mapped if bars == 100_000 else 48 * bars if harness.has_bin else 0
        args = base_args(module) + harness.input_args(path) + [
            "--inputs", json.dumps({"addEvery": 1})]
        sink_args, sink_path = harness.sink_args(f"j{exits}")
        rest_bytes, document = harness.rest(
            f"j/{exits}", args, sink_args if harness.has_sink else [], mapped)
        values.append((str(exits), rest_bytes))
        exit_counts[exits] = int(document.get("summary", {}).get("total_trades", -1))
        if harness.has_sink and exits == 100_000:
            full = harness.run(args, f"j/{exits}/full")[0].document()
            full_trades = full.get("trades", [])
            records = harness.sink_records(sink_path)
            sink_trades = [
                {"n": row[1], "side": row[2], "entry_time": row[3],
                 "exit_time": row[4], "entry_price": row[5], "exit_price": row[6],
                 "qty": row[7], "pnl": row[8], "pnl_pct": row[9],
                 "max_runup": row[10], "max_drawdown": row[11],
                 "commission": row[12], "entry_bar_index": row[13],
                 "exit_bar_index": row[14], "open_at_end": row[15],
                 "entry_incarnation": row[16]}
                for row in records.get("T", [])
            ]
            assert_equal("(j) trades sink on vs off", full_trades, sink_trades)
    if exit_counts[1000] < 500 or exit_counts[100_000] < 50_000:
        raise SetupError(
            f"(j) the scale-out fixture produced {exit_counts} closed rows; the "
            "exits axis is not exercised")
    spread = abs(values[0][1] - values[1][1])
    if spread > SLOPE_LIMIT:
        raise CaseFailure(
            f"(j) scale-out: rest differs by {spread / MIB:.2f} MiB between "
            f"{values[0][0]} and {values[1][0]} exits")


# ── INPUTS item 1: a borrowed feed array may not be released early ──────

BORROWED_EXPORT = "strategy_set_symbol_feed_borrowed_v1"
FEED_CLEARS = ("bars", "close_ms")
RUN_CALLS = ("run_backtest_full", "_run", "run_backtest")


def _call_name(node: ast.AST) -> str | None:
    if isinstance(node, ast.Call):
        function = node.func
        if isinstance(function, ast.Name):
            return function.id
        if isinstance(function, ast.Attribute):
            return function.attr
    return None


def case_borrowed(harness: Harness) -> None:
    """INPUTS item 1: the adapter keeps a borrowed feed alive for the run."""
    print("borrowed: run_json keeps borrowed symbol-feed arrays alive")
    source_path = harness.run_json
    text = source_path.read_text(encoding="utf-8")
    if BORROWED_EXPORT not in text:
        raise CaseFailure(
            "run_json never calls strategy_set_symbol_feed_borrowed_v1: the "
            "1.7.0 borrowed symbol-feed path does not exist yet (DESIGN.md 2.3, "
            "lane L15b), so a feed array can still be released before the run "
            "ends.").with_traceback(None)
    tree = ast.parse(text, filename=str(source_path))
    parents: dict[ast.AST, ast.AST] = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            parents[child] = parent

    def enclosing_function(node: ast.AST) -> ast.AST | None:
        current = parents.get(node)
        while current is not None:
            if isinstance(current, (ast.FunctionDef, ast.AsyncFunctionDef)):
                return current
            current = parents.get(current)
        return None

    clears: dict[ast.AST, list[int]] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if (isinstance(target, ast.Subscript)
                    and isinstance(target.value, ast.Name)
                    and isinstance(target.slice, ast.Constant)
                    and target.slice.value in FEED_CLEARS
                    and isinstance(node.value, ast.Constant)
                    and node.value.value is None):
                function = enclosing_function(node)
                if function is not None:
                    clears.setdefault(function, []).append(node.lineno)

    if not clears:
        raise CaseFailure(
            "run_json no longer clears feed['bars']/feed['close_ms'] at all; "
            "this guard cannot see where the borrowed arrays are released")

    for function, lines in clears.items():
        run_lines = [
            node.lineno for node in ast.walk(function)
            if _call_name(node) in RUN_CALLS
        ]
        if not run_lines:
            continue
        last_run = max(run_lines)
        early = [line for line in lines if line < last_run]
        if early:
            raise CaseFailure(
                f"run_json releases a symbol-feed array at line(s) {early} of "
                f"{function.name}, before the run call at line {last_run}. "
                "Under 1.7.0 borrowed views the engine would read freed "
                "memory (INPUTS-after-rev4 item 1).")
    if not any("strategy_free" in line for line in text.splitlines()):
        raise CaseFailure("run_json has no strategy_free; cannot judge lifetime")


# ── main ────────────────────────────────────────────────────────────────


CASES = {
    "a": case_a,
    "b": case_b,
    "f": case_f,
    "g": case_g,
    "h": case_h,
    "j": case_j,
    "borrowed": case_borrowed,
}


def default_module_dir(repo_root: Path) -> Path:
    return repo_root / "build" / "flat_memory_modules"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-json", type=Path, default=None,
                        help="docker/run_json.py from the installed image")
    parser.add_argument("--module-dir", type=Path, default=None,
                        help="directory holding macd.so, every_k.so, xsym.so, "
                             "scale_out.so (and the F13 modules)")
    parser.add_argument("--module", action="append", default=[],
                        metavar="NAME=PATH", help="override one module path")
    parser.add_argument("--work-dir", type=Path, default=None,
                        help="where the generated bars, feeds and sinks live")
    parser.add_argument("--cases", default=",".join(CASES),
                        help=f"comma-separated subset of {','.join(CASES)}")
    parser.add_argument("--keep", action="store_true", help="keep the work dir")
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parents[1]
    run_json = args.run_json or (repo_root / "docker" / "run_json.py")
    if not run_json.is_file():
        print(f"SETUP: {run_json} is not present", file=sys.stderr)
        return 2
    module_dir = args.module_dir or default_module_dir(repo_root)
    modules: dict[str, Path] = {}
    for override in args.module:
        if "=" not in override:
            print(f"SETUP: --module wants NAME=PATH, got {override!r}", file=sys.stderr)
            return 2
        name, path = override.split("=", 1)
        modules[name] = Path(path)

    work = args.work_dir
    if work is None:
        work = Path(tempfile.mkdtemp(prefix="pf-flat-memory-"))
    else:
        work.mkdir(parents=True, exist_ok=True)

    selected = [name.strip() for name in args.cases.split(",") if name.strip()]
    unknown = [name for name in selected if name not in CASES]
    if unknown:
        print(f"SETUP: unknown cases {unknown}", file=sys.stderr)
        return 2

    try:
        harness = Harness(run_json, module_dir, work, modules, args.keep)
    except SetupError as error:
        print(f"SETUP: {error}", file=sys.stderr)
        return 2
    print(f"run_json: {run_json}")
    print(f"modules:  {module_dir} ({', '.join(sorted(modules)) or 'defaults'})")
    print(f"work:     {work}")
    print(f"features: --ohlcv-bin={'yes' if harness.has_bin else 'no'}, "
          f"--report-sink={'yes' if harness.has_sink else 'no'}")

    failures = 0
    setup_errors = 0
    errors = 0
    for name in selected:
        try:
            CASES[name](harness)
            print(f"PASS {name}")
        except CaseFailure as error:
            failures += 1
            print(f"FAIL {name}: {error}")
        except SetupError as error:
            print(f"SETUP {name}: {error}", file=sys.stderr)
            setup_errors += 1
        except Exception as error:  # noqa: BLE001 - a test driver reports all
            errors += 1
            print(f"ERROR {name}: {type(error).__name__}: {error}")

    if not args.keep and args.work_dir is None:
        shutil.rmtree(work, ignore_errors=True)
    passed = len(selected) - failures - setup_errors - errors
    print(f"flat-memory rss e2e: {passed}/{len(selected)} cases passed; "
          f"{failures} red, {setup_errors} setup, {errors} errors")
    # A measured/behavioral red takes precedence in a mixed run. Setup-only
    # and unexpected-error runs cannot be admitted as red acceptance evidence.
    if failures:
        return 1
    return 2 if setup_errors or errors else 0


if __name__ == "__main__":
    sys.exit(main())
