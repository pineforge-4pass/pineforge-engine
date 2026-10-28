#!/usr/bin/env python3
"""Measure one complete run_json automatic temporary-sink run on a lab host.

CSV, label and output directory are positional; --so names the lab-built module.
No build, sampling, warmup, repeat, named sink or alternate report builder runs.
Wall time covers the fresh producer process through stdout publication/cleanup.
Peak RSS is that process's wait4 high-water mark, in MiB. The parent's streaming
CSV census, JSON census and SHA-256 run afterwards, outside both measurements.
``trades`` and ``equity`` count the report's actual trade and curve array items.

The report is published unchanged as OUTPUT/LABEL.report.json only on success.
Use the same --elapsed-seconds value with both producers for a repeatable raw
report SHA: elapsed_seconds otherwise contains a real, variable run duration.
This option fixes only that diagnostic's clock; wall_s always uses perf_counter.
--producer can select a pre-change run_json.py with the existing sink ABI.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
WINDOW = 64 * 1024
_STRING_END = re.compile(r'["\\]')


def _existing_file(value):
    path = Path(value)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"input file does not exist: {value}")
    return path


def _label(value):
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value) is None:
        raise argparse.ArgumentTypeError("label must use letters, digits, '.', '_' or '-'")
    return value


def _elapsed(value):
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise argparse.ArgumentTypeError("elapsed seconds must be finite and non-negative")
    return result


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_csv", type=_existing_file)
    parser.add_argument("run_label", type=_label)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--so", required=True, type=_existing_file)
    parser.add_argument("--producer", type=_existing_file, default=ROOT / "docker/run_json.py")
    parser.add_argument("--expect-bars", type=int,
                        help="refuse if either the complete CSV or report has a different count")
    parser.add_argument("--expected-sha256", help="refuse a report that differs from this raw SHA-256")
    parser.add_argument("--elapsed-seconds", type=_elapsed,
                        help="fix only the report duration for a raw byte comparison on both versions")
    for name in ("input-tf", "script-tf", "bar-magnifier", "inputs", "overrides"):
        parser.add_argument("--" + name, default=None)
    parser.add_argument("--generated-cpp", type=_existing_file)
    parser.add_argument("--syminfo", type=_existing_file)
    parser.add_argument("--outputs", action="store_true")
    parser.add_argument("--_worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--_scratch", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.expect_bars is not None and args.expect_bars <= 0:
        parser.error("--expect-bars must be positive")
    if (args.expected_sha256 is not None
            and re.fullmatch(r"[0-9a-f]{64}", args.expected_sha256) is None):
        parser.error("--expected-sha256 must be 64 lowercase hex digits")
    if args._worker != (args._scratch is not None):
        parser.error("worker mode requires its private scratch directory")
    return args


def _producer_arguments(args, scratch):
    argv = ["--so", str(args.so), "--ohlcv", str(args.input_csv),
            "--spill-dir", str(scratch)]
    for name in ("input_tf", "script_tf", "bar_magnifier", "inputs", "overrides",
                 "generated_cpp", "syminfo"):
        value = getattr(args, name)
        if value is not None:
            argv.extend(("--" + name.replace("_", "-"), str(value)))
    if args.outputs:
        argv.append("--outputs")
    return argv


def _worker(args):
    # Load the selected producer with its own helper closure. The observers
    # below forward to its real functions; neither rows nor bytes are replaced.
    sys.path.insert(0, str(args.producer.resolve().parent))
    spec = importlib.util.spec_from_file_location("measured_run_json", args.producer)
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    original = runner._new_report_sink
    sink, calls = None, 0

    def observe_sink(*positional, **keywords):
        nonlocal sink, calls
        sink = original(*positional, **keywords)
        calls += 1
        if sink is None or sink.named:
            raise RuntimeError("the producer did not select the automatic temporary sink")
        return sink

    runner._new_report_sink = observe_sink
    clock_calls = 0
    if args.elapsed_seconds is not None:
        def report_clock():
            nonlocal clock_calls
            clock_calls += 1
            return 0. if clock_calls == 1 else args.elapsed_seconds
        runner.time = SimpleNamespace(time=report_clock, perf_counter_ns=time.perf_counter_ns,
                                      monotonic_ns=time.monotonic_ns)
    status = runner.main(_producer_arguments(args, args._scratch))
    if status:
        return status
    if calls != 1 or sink is None or not sink.closed or Path(sink.path).exists():
        raise RuntimeError("the single temporary sink was not closed and removed")
    if args.elapsed_seconds is not None and clock_calls != 2:
        raise RuntimeError("the producer's report clock no longer has exactly two calls")
    receipt = {"trades": sink.counts["T"], "equity": sink.counts["E"]}
    with (args._scratch / "receipt.json").open("w", encoding="ascii") as file:
        json.dump(receipt, file, separators=(",", ":"))
    return 0


class _ReportCensus:
    """Count top-level report arrays without retaining their rows or strings."""

    def __init__(self, file):
        self.file, self.buffer, self.position = file, "", 0
        self.sha256, self.report_bytes = hashlib.sha256(), 0

    def _peek(self):
        if self.position == len(self.buffer):
            raw = self.file.read(WINDOW)
            self.sha256.update(raw)
            self.report_bytes += len(raw)
            self.buffer, self.position = raw.decode("ascii"), 0
        return self.buffer[self.position:self.position + 1]

    def _space(self):
        while self._peek() in (" ", "\t", "\r", "\n"):
            self.position += 1

    def _expect(self, char):
        self._space()
        if self._peek() != char:
            raise ValueError(f"report JSON: expected {char!r}")
        self.position += 1

    def _string(self, keep=False):
        self._expect('"')
        pieces = ['"'] if keep else None
        while self._peek():
            match = _STRING_END.search(self.buffer, self.position)
            end = match.end() if match else len(self.buffer)
            if keep:
                pieces.append(self.buffer[self.position:end])
            self.position = end
            if match and match.group() == '"':
                return json.loads("".join(pieces)) if keep else None
            if match:  # consume the escaped character, including across windows
                escaped = self._peek()
                if not escaped:
                    break
                if keep:
                    pieces.append(escaped)
                self.position += 1
        raise ValueError("report JSON: unterminated string")

    def _scalar(self):
        self._space()
        pieces = []
        while (char := self._peek()) and char not in ",]} \t\r\n":
            pieces.append(char)
            self.position += 1
        return json.loads("".join(pieces))

    def _object(self, visit=None):
        self._expect("{")
        self._space()
        if self._peek() != "}":
            while True:
                key = self._string(keep=visit is not None)
                self._expect(":")
                if visit is not None:
                    visit(key)
                else:
                    self._skip()
                self._space()
                if self._peek() != ",":
                    break
                self.position += 1
        self._expect("}")

    def _array(self):
        self._expect("[")
        count = 0
        self._space()
        if self._peek() != "]":
            while True:
                self._skip()
                count += 1
                self._space()
                if self._peek() != ",":
                    break
                self.position += 1
        self._expect("]")
        return count

    def _skip(self):
        self._space()
        char = self._peek()
        if char == "{":
            self._object()
        elif char == "[":
            self._array()
        elif char == '"':
            self._string()
        else:
            self._scalar()

    def read(self):
        result = {}

        def input_field(key):
            if key == "bars":
                result["input_bars"] = self._scalar()
            else:
                self._skip()

        def field(key):
            if key in ("trades", "equity_curve"):
                result["trades" if key == "trades" else "equity"] = self._array()
            elif key == "input":
                self._object(input_field)
            elif key == "engine":
                result["engine"] = self._string(keep=True)
            else:
                self._skip()

        self._object(field)
        self._space()
        if self._peek() or result.get("engine") != "pineforge":
            raise ValueError("producer did not emit exactly one PineForge report")
        if (type(result.get("input_bars")) is not int
                or "trades" not in result or "equity" not in result):
            raise ValueError("producer did not emit full trade and equity arrays")
        result.update(report_bytes=self.report_bytes, sha256=self.sha256.hexdigest())
        return result


def _measure(command, stdout, stderr):
    started = time.perf_counter()
    with subprocess.Popen(command, stdout=stdout, stderr=stderr) as process:
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        wall_s = time.perf_counter() - started
    # Linux reports KiB; Darwin reports bytes.
    peak_rss_mb = usage.ru_maxrss / (1024 * 1024 if sys.platform == "darwin" else 1024)
    return process.returncode, wall_s, peak_rss_mb


def _print_failure(path):
    with path.open("r", encoding="utf-8", errors="replace") as file:
        while chunk := file.read(WINDOW):
            sys.stderr.write(chunk)


def _run(args):
    args.output_dir.mkdir(parents=True, exist_ok=True)
    destination = args.output_dir / (args.run_label + ".report.json")
    if os.path.lexists(destination):
        raise FileExistsError(f"refusing to replace an existing file: {destination}")
    with tempfile.TemporaryDirectory(prefix="." + args.run_label + "-", dir=args.output_dir) as directory:
        scratch = Path(directory)
        report, errors = scratch / "stdout.json", scratch / "stderr.log"
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   str(args.input_csv), args.run_label, str(args.output_dir),
                   "--so", str(args.so), "--producer", str(args.producer),
                   "--_worker", "--_scratch", str(scratch)]
        for name in ("input_tf", "script_tf", "bar_magnifier", "inputs", "overrides",
                     "generated_cpp", "syminfo", "elapsed_seconds"):
            value = getattr(args, name)
            if value is not None:
                command.extend(("--" + name.replace("_", "-"), str(value)))
        if args.outputs:
            command.append("--outputs")
        with report.open("wb", buffering=0) as stdout, errors.open("wb", buffering=0) as stderr:
            status, wall_s, peak_rss_mb = _measure(command, stdout, stderr)
        if status:
            print(f"temporary-sink producer failed with status {status}", file=sys.stderr)
            _print_failure(errors)
            _print_failure(report)
            return status if status > 0 else 1
        _print_failure(errors)
        with report.open("rb", buffering=0) as file:
            census = _ReportCensus(file).read()
        with (scratch / "receipt.json").open(encoding="ascii") as file:
            receipt = json.load(file)
        if any(census[key] != receipt[key] for key in ("trades", "equity")):
            raise ValueError("published report counts differ from the verified sink receipt")
        with args.input_csv.open(newline="", encoding="utf-8") as file:
            bars = sum(1 for _ in csv.DictReader(file))
        if census["input_bars"] != bars or (args.expect_bars is not None and bars != args.expect_bars):
            raise ValueError(f"CSV/report population mismatch: CSV={bars}, "
                             f"report={census['input_bars']}, requested={args.expect_bars}")
        if args.expected_sha256 is not None and census["sha256"] != args.expected_sha256:
            raise ValueError(f"report SHA-256 mismatch: expected {args.expected_sha256}, "
                             f"got {census['sha256']}")
        result = {"wall_s": wall_s, "peak_rss_mb": peak_rss_mb,
                  "report_bytes": census["report_bytes"], "trades": census["trades"],
                  "equity": census["equity"], "sha256": census["sha256"]}
        # A hard link publishes complete bytes atomically and refuses to
        # replace a caller's file, including one created during this run.
        os.link(report, destination)
    print(json.dumps(result, separators=(",", ":"), allow_nan=False))
    return 0


def main(argv=None):
    args = arguments(argv)
    try:
        if not hasattr(os, "wait4") or not (sys.platform.startswith("linux") or sys.platform == "darwin"):
            raise RuntimeError("this lab driver requires Linux or macOS wait4 RSS accounting")
        return _worker(args) if args._worker else _run(args)
    except Exception as error:
        print(f"temporary-sink measurement failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
