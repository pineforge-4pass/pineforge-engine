"""Replay converter tests; execution belongs on lab remote / SM-CX only.

The synthetic oracle is today's run_json.build_report_dict and
build_outputs_block, driven by generated native-layout values, not saved JSON
or expected hashes. Native differential mode uses one execution: it retains
the named sink while calling today's unchanged full-report builders. Its
stdout.json presentation is projected from THAT SAME report, then supplied to
the standalone converter. Whole documents and every derivable field must be
byte-equal. No literal-keyed strategy/result fixture or inline subprocess
program is used.

Remote differential command (missing/empty cases is a failure, never a skip):
  python3 -B docker/pineforge_sink_to_json_test.py --differential CASES ARTIFACTS
CASES is [{"name": STR, "so": ABS_PATH, "argv": [STR,...]}, ...], naming
already-built corpus subset modules and the exact ordinary/selected/outputs
run_json arguments. Artifact directories record the same-run oracle, retained
sink, bounded stdout.json, converted JSON, diagnostics and command argv.

The fresh stdlib-only RSS probe executes BEFORE pytest/run_json imports. It
generates retained inputs, drains output without storing it, and measures
just the converter's RUSAGE_CHILDREN peak. Converter child writes are under
RLIMIT_FSIZE=0. Small/large runs exercise interleaved E/T/O/V/K and every
column replay, with no RSS allowance proportional to the sink's size.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import resource
import subprocess
import sys

DOCKER = Path(__file__).resolve().parent
ROOT = DOCKER.parent
TOOL = DOCKER / "pineforge_sink_to_json.py"
LAUNCHER = DOCKER / "pineforge-sink-to-json"
SELF = Path(__file__).resolve()
RSS_LIMIT_BYTES = 48 * 1024 * 1024
RSS_GROWTH_BYTES = 8 * 1024 * 1024
LARGE_ROWS = 131072


def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8") + b"\n"


def report_bytes(value):
    # Also used by --differential before the pytest/run_json imports below.
    import run_json

    encoded = io.StringIO()
    run_json._stream_json(value, encoded)
    return encoded.getvalue().encode("ascii") + b"\n"


def no_file_writes():
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))


def generated_rows(count):
    for index in range(count):
        timestamp = 1700000000000 + index * 60000
        pnl = float(index % 7 - 3)
        yield ["E", timestamp, 10000.0 + float(index % 13), pnl]
        yield ["T", index + 1, "long" if index % 2 else "short", timestamp,
               timestamp + 60000, 100.0, 101.0, 2.0, pnl, pnl / 2.0, 4.0, -2.0,
               0.25, index, index + 1, index == count - 1, index + 1, 1,
               str(index) + "e" * 512, str(index) + "x" * 256, "c" * 128]
        yield ["O", index, timestamp, timestamp + 60000, float(index), float(index % 4294967296)]
        yield ["V", index + 1, 1, index, timestamp, timestamp + 60000, 0, index % 3,
               1, pnl, format(index, "016x"), f"row {index} 漢"]
    yield ["K", 50.0]


def write_stream(path, header, rows):
    counts = {entry["type"]: 0 for entry in header["records"]}
    digest = hashlib.sha256()
    with path.open("wb") as destination:
        line = wire(header)
        destination.write(line)
        digest.update(line)
        for row in rows:
            counts[row[0]] += 1
            line = wire(row)
            destination.write(line)
            digest.update(line)
        destination.write(wire({"trailer": "pf-report-sink", "version": 1,
                                "counts": counts, "sha256": digest.hexdigest()}))
    return counts, digest.hexdigest()


def projection(document):
    # Tests may retain their small reference document. Production never calls
    # this function and never loads an input-sized report or row array.
    result = json.loads(report_bytes(document))
    for key in ("trades", "equity_curve"):
        result.pop(key)
    if "outputs" in result:
        for key in ("bars", "series", "events"):
            result["outputs"].pop(key)
    return result


def rss_probe(directory, header_path, count):
    directory = Path(directory)
    directory.mkdir()
    sink, metadata = directory / "retained.sink", directory / "stdout.json"
    head = json.loads(Path(header_path).read_bytes())
    counts, digest = write_stream(sink, head, generated_rows(count))
    manifest = {"outputs": [{"index": 0, "id": "p", "kind": "plot"},
                            {"index": 1, "id": "a", "kind": "alert", "freq": "once"}],
                "series": [{"slot": 0, "output": "p"},
                           {"slot": 1, "output": "p", "encoding": "rgba-u32"}],
                "constants": [{"index": 0}]}
    metadata.write_bytes(report_bytes({"outputs": {"manifest": manifest}}))
    before = sorted(path.name for path in directory.iterdir())
    command = [sys.executable, "-I", "-B", "-S", str(LAUNCHER), "--in", str(sink),
               "--stdout-json", str(metadata), "--out", "-", "--max-bytes", str(sink.stat().st_size)]
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, preexec_fn=no_file_writes, cwd=directory)
    received, tail = 0, b""
    output_digest = hashlib.sha256()
    with process.stdout as stream:
        while chunk := stream.read(64 * 1024):
            received += len(chunk)
            output_digest.update(chunk)
            tail = (tail + chunk)[-2:]
    process.stdout = None
    _, stderr = process.communicate()
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    peak_bytes = int(peak if sys.platform == "darwin" else peak * 1024)
    measured = {"returncode": process.returncode, "stderr": stderr.decode(),
                "peak_bytes": peak_bytes, "sink_bytes": sink.stat().st_size,
                "received_bytes": received, "output_sha256": output_digest.hexdigest(),
                "complete_json_tail": tail == b"}\n", "counts": counts, "sha256": digest,
                "files_unchanged": before == sorted(path.name for path in directory.iterdir())}
    print(json.dumps(measured, separators=(",", ":")))
    return process.returncode


def capture_native(arguments):
    # Reporting-only test seam: ONE native run, the real current full-report
    # builders, and its retained named sink. No numeric report method is
    # patched and the strategy runs exactly once.
    sys.path.insert(0, str(DOCKER))
    import run_json

    module, sink_path, run_id, *argv = arguments
    original_builder = run_json._build_sink_report
    original_outputs = run_json._sink_outputs_block

    def full_builder(*args):
        sink = args[-1]
        named = sink.named
        try:
            sink.named = False
            return original_builder(*args)
        finally:
            sink.named = named

    def full_outputs(lib, state, sink, resources, *, summary_only=False):
        return original_outputs(lib, state, sink, resources, summary_only=False)

    run_json._build_sink_report = full_builder
    run_json._sink_outputs_block = full_outputs
    return run_json.main(["--so", module, *argv, "--report-sink", sink_path, "--run-id", run_id])


def compare_derivable(expected, actual):
    for key in ("trades", "equity_curve"):
        assert report_bytes(actual[key]) == report_bytes(expected[key]), key
    for key, value in expected["summary"].items():
        if key != "bars_processed":
            assert report_bytes(actual["summary"][key]) == report_bytes(value), key
    for group in ("all", "longs", "shorts"):
        for key in ("num_trades", "num_wins", "num_losses", "num_even",
                    "max_consecutive_wins", "max_consecutive_losses"):
            assert actual["metrics"][group][key] == expected["metrics"][group][key], (group, key)
    if "outputs" in expected:
        for key in ("bars", "series", "constants", "hlines", "events"):
            assert report_bytes(actual["outputs"][key]) == report_bytes(expected["outputs"][key]), key
    if "report_shape" in expected:
        assert report_bytes(actual["report_shape"]) == report_bytes(expected["report_shape"])


def differential(cases_path, artifacts_path):
    cases = json.loads(Path(cases_path).read_bytes())
    assert type(cases) is list and cases, "CORE-TOP must supply a nonempty corpus subset"
    artifacts = Path(artifacts_path).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    names = set()
    for index, case in enumerate(cases):
        name, module, argv = case["name"], Path(case["so"]).resolve(), case["argv"]
        assert type(name) is str and name and name not in names
        assert module.is_file(), case
        assert type(argv) is list and all(type(arg) is str for arg in argv)
        assert not any(arg.startswith(("--report-sink", "--run-id", "--bench")) for arg in argv)
        names.add(name)
        directory = artifacts / str(index)
        directory.mkdir()  # stale proof artifacts must not be mistaken for this run
        sink, stdout_json = directory / "retained.sink", directory / "stdout.json"
        run_id = "sink-json-" + str(index)
        command = [sys.executable, "-B", str(SELF), "--capture-native", str(module), str(sink), run_id, *argv]
        baseline = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True)
        (directory / "oracle.command.json").write_bytes(report_bytes(command))
        (directory / "oracle.stdout.json").write_bytes(baseline.stdout)
        (directory / "oracle.stderr.jsonl").write_bytes(baseline.stderr)
        assert baseline.returncode == 0, (case, baseline.stderr.decode(), baseline.stdout[:1024])
        expected = json.loads(baseline.stdout)
        stdout_json.write_bytes(report_bytes(projection(expected)))
        assert stdout_json.stat().st_size <= 2 * 1024 * 1024, "same-run metadata exceeds the contract"
        command = [sys.executable, "-I", "-B", "-S", str(LAUNCHER), "--in", str(sink),
                   "--stdout-json", str(stdout_json), "--out", "-", "--max-bytes",
                   str(sink.stat().st_size), "--run-id", run_id]
        converted = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True,
                                   preexec_fn=no_file_writes)
        (directory / "converter.command.json").write_bytes(report_bytes(command))
        (directory / "converter.stdout.json").write_bytes(converted.stdout)
        (directory / "converter.stderr.jsonl").write_bytes(converted.stderr)
        assert converted.returncode == 0, (case, converted.stderr.decode())
        actual = json.loads(converted.stdout)
        compare_derivable(expected, actual)
        assert converted.stdout == baseline.stdout, (case, "whole same-run document differs")
        lines = [json.loads(line) for line in converted.stderr.splitlines()]
        assert lines[-1]["event"] == "converter_complete" and lines[-1]["metadata_drops"] == 0, lines
        print(json.dumps({"case": name, "same_run": True, "derivable_bytes_equal": True,
                          "document_bytes_equal": True, "sink_bytes": sink.stat().st_size,
                          "artifacts": str(directory)}, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__" and len(sys.argv) > 1:
    if sys.argv[1] == "--rss-probe":
        raise SystemExit(rss_probe(sys.argv[2], sys.argv[3], int(sys.argv[4])))
    if sys.argv[1] == "--capture-native":
        raise SystemExit(capture_native(sys.argv[2:]))
    if sys.argv[1] == "--differential":
        raise SystemExit(differential(sys.argv[2], sys.argv[3]))


import base64
import ctypes
import itertools
import types

import pytest

sys.path.insert(0, str(DOCKER))
sys.path.insert(0, str(ROOT / "tests"))

import pf_sink
import pineforge_sink_to_json as converter
import run_json
import test_run_json_sink as fixtures


def finite(value):
    import math
    return value if math.isfinite(value) else None


def fill_metric_counts(stats, pnls):
    stats.num_trades = len(pnls)
    stats.num_wins = len([p for p in pnls if p > 0])
    stats.num_losses = len([p for p in pnls if p < 0])
    stats.num_even = len(pnls) - stats.num_wins - stats.num_losses
    # An independent, whole-test-input streak oracle, not the converter fold.
    runs = [(sign, len(list(group))) for sign, group in itertools.groupby(
        (1 if pnl > 0 else -1 if pnl < 0 else 0 for pnl in pnls))]
    stats.max_consecutive_wins = max((n for sign, n in runs if sign == 1), default=0)
    stats.max_consecutive_losses = max((n for sign, n in runs if sign == -1), default=0)


def synthetic(tmp_path, pnls=(13.25, -4.5, 0.0, -0.0, 2.0), *, outputs=False):
    report = run_json.ReportC()
    trades = (run_json.TradeC * len(pnls))()
    curve = (run_json.EquityPointC * (len(pnls) + 3))()
    rows, cumulative = [], 0.0
    for index, pnl in enumerate(pnls):
        trade = trades[index]
        trade.entry_time = 1700000000000 + index * 60000
        trade.exit_time = trade.entry_time + 60000 * (1 + index % 3)
        trade.entry_price, trade.exit_price = 100.0 + index, 101.0 + index
        trade.qty, trade.pnl, trade.pnl_pct = 1.5 + index, pnl, pnl / 2.0
        trade.is_long = int(index % 2 == 0)
        trade.max_runup, trade.max_drawdown, trade.commission = 3.0 + index, -2.0 - index, 0.25
        trade.entry_bar_index, trade.exit_bar_index = index, index + 2
        trade.open_at_end = int(index == len(pnls) - 1)
        cumulative += pnl  # deliberately not Python's version-dependent sum()
        rows.append(["T", index + 1, "long" if trade.is_long else "short", trade.entry_time,
                     trade.exit_time, trade.entry_price, trade.exit_price, trade.qty, trade.pnl,
                     trade.pnl_pct, trade.max_runup, trade.max_drawdown, trade.commission,
                     trade.entry_bar_index, trade.exit_bar_index, bool(trade.open_at_end),
                     index + 17, index % 4, f"entry {index} 漢", f"exit {index}", 'quote"\n'])
    for index, point in enumerate(curve):
        point.time_ms = 1700000000000 + (index // 2) * 60000  # repeat times are legal
        point.equity = 10000.0 + index if index % 3 else float("nan")
        point.open_profit = float(index % 5) if index % 2 else -0.0
        # E before T proves that report order is obtained through replay.
        rows.insert(index, ["E", point.time_ms, finite(point.equity), point.open_profit])
    report.total_trades, report.trades, report.trades_len = len(pnls), trades, len(pnls)
    report.net_profit, report.input_bars_processed = cumulative, len(curve) * 5
    report.script_bars_processed = len(curve)
    report.equity_curve, report.equity_curve_len = curve, len(curve)
    report.bar_magnifier_enabled = 1
    for group_index, name in enumerate(("all", "longs", "shorts")):
        stats = getattr(report.metrics, name)
        for field_index, (name_, ctype) in enumerate(stats._fields_):
            if ctype is ctypes.c_double:
                setattr(stats, name_, float(field_index + group_index) / 8.0)
        chosen = [pnl for index, pnl in enumerate(pnls)
                  if name == "all" or (name == "longs") == (index % 2 == 0)]
        fill_metric_counts(stats, chosen)
    for index, (name, _) in enumerate(report.metrics.equity._fields_):
        setattr(report.metrics.equity, name, float(index) / 16.0 if index % 2 else float("nan"))
    runtime = {"input_tf": "1", "script_tf": "5", "input_tf_seconds": 60,
               "script_tf_seconds": 300, "script_tf_ratio": 5, "needs_aggregation": True,
               "bar_magnifier": True, "magnifier_samples": 4, "magnifier_dist": "endpoints",
               "magnifier_volume_weighted": False, "trade_start_ms": None, "chart_tz": "UTC"}
    expected = run_json.build_report_dict(report, Path("chart 漢.csv"), report.input_bars_processed,
        curve[0].time_ms, curve[-1].time_ms, 0.123456, {"length": "14"}, {}, runtime,
        [index + 17 for index in range(len(pnls))])
    if outputs:
        runtime["outputs"] = True
        reader = fixtures.outputs_reader()
        expected["outputs"] = run_json.build_outputs_block(reader)
        opens, closes = reader.bar_times()
        for index, (open_ms, close_ms) in enumerate(zip(opens, closes)):
            rows.insert(index + 1, ["O", index, None if open_ms == -(2**63) else open_ms,
                None if close_ms == -(2**63) else close_ms,
                *(finite(reader.series(slot)[index]) for slot in range(reader.series_count()))])
        for event in reader.events():
            rows.insert(2, ["V", event.sequence, event.output_index, event.bar_index,
                event.bar_open_ms, event.bar_close_ms, event.ordinal_in_bar, event.phase, 1,
                finite(event.value), "0123456789abcdef", event.message])
        rows.insert(0, ["K", *reader.constants()])
    expected["fingerprint"] = None
    sink, metadata = tmp_path / "retained.sink", tmp_path / "stdout.json"
    write_stream(sink, fixtures.header(outputs=outputs), rows)
    metadata.write_bytes(report_bytes(projection(expected)))
    return sink, metadata, expected


def invoke(sink, metadata, *, extra=(), launcher=False):
    return subprocess.run([sys.executable, "-I", "-B", "-S", str(LAUNCHER if launcher else TOOL),
        "--in", str(sink), "--stdout-json", str(metadata), "--out", "-", "--max-bytes",
        str(sink.stat().st_size), *map(str, extra)], stdin=subprocess.DEVNULL,
        capture_output=True, preexec_fn=no_file_writes, cwd=sink.parent)


def diagnostics(result):
    lines = [json.loads(line) for line in result.stderr.splitlines()]
    assert lines and all(line["tool"] == "pineforge-sink-to-json" for line in lines)
    assert lines[-1]["exit_code"] == result.returncode
    return lines


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [(), (0.0,), (-0.0, 0.0),
    (13.25, -4.5, 0.0, -0.0, 2.0), (1e16, 1.0, -1e16)])
def test_clean_same_synthetic_report_is_byte_equal(tmp_path, outputs, pnls):
    sink, metadata, expected = synthetic(tmp_path, pnls, outputs=outputs)
    before = {path.name: path.read_bytes() for path in tmp_path.iterdir()}
    result = invoke(sink, metadata, launcher=True)
    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == report_bytes(expected)
    compare_derivable(expected, json.loads(result.stdout))
    lines = diagnostics(result)
    assert lines[-1]["metadata_drops"] == 0
    assert lines[-1]["billing"] == "normal" and lines[-1]["sink_verified"] is True
    assert before == {path.name: path.read_bytes() for path in tmp_path.iterdir()}


def refold(lines):
    header = json.loads(lines[0])
    counts = {entry["type"]: 0 for entry in header["records"]}
    digest = hashlib.sha256()
    for index, line in enumerate(lines):
        digest.update(line)
        if index:
            counts[json.loads(line)[0]] += 1
    return b"".join(lines) + wire({"trailer": "pf-report-sink", "version": 1,
                                  "counts": counts, "sha256": digest.hexdigest()})


@pytest.mark.parametrize("fault,code", [
    ("no-trailer", 6), ("truncated-trailer", 6), ("bad-line", 3), ("hash", 4),
    ("counts", 4), ("extra-byte", 4), ("bom", 3), ("whitespace", 3),
    ("duplicate-key", 3), ("integer-f64", 3), ("nonfinite", 3), ("oversize", 3)])
def test_verification_failure_has_zero_stdout(tmp_path, fault, code):
    sink, metadata, _ = synthetic(tmp_path)
    lines = sink.read_bytes().splitlines(keepends=True)
    if fault == "no-trailer":
        lines.pop()
    elif fault == "truncated-trailer":
        lines[-1] = lines[-1][:-7]
    elif fault == "bad-line":
        lines[1] = b"not JSON\n"
    elif fault in ("hash", "counts"):
        trailer = json.loads(lines[-1])
        if fault == "hash":
            trailer["sha256"] = ("1" if trailer["sha256"][0] != "1" else "2") + trailer["sha256"][1:]
        else:
            trailer["counts"]["T"] += 1
        lines[-1] = wire(trailer)
    elif fault == "extra-byte":
        lines.append(b"x")
    elif fault == "bom":
        lines[0] = b"\xef\xbb\xbf" + lines[0]
    elif fault == "whitespace":
        lines[1] = b" " + lines[1]
    elif fault == "duplicate-key":
        lines[0] = lines[0].replace(b'"version":1', b'"version":1,"version":1')
    elif fault in ("integer-f64", "nonfinite"):
        row = json.loads(lines[2])
        row[2] = 1 if fault == "integer-f64" else None
        lines[2] = wire(row)
        if fault == "nonfinite":
            lines[2] = lines[2].replace(b"null", b"NaN", 1)
    elif fault == "oversize":
        lines[1] = b'"' + b"x" * pf_sink.MAX_LINE_BYTES + b'"\n'
    sink.write_bytes(b"".join(lines))
    result = invoke(sink, metadata)
    assert result.returncode == code and result.stdout == b"", result.stderr.decode()
    lines = diagnostics(result)
    assert lines[-1]["event"] == "sink_refused" and lines[-1]["sink_verified"] is False
    assert not any(line["event"] == "metadata_drop" for line in lines)


@pytest.mark.parametrize("tag", ["E", "T", "O", "K"])
def test_receipt_valid_but_unusable_order_is_a_sink_defect(tmp_path, tag):
    sink, metadata, _ = synthetic(tmp_path, outputs=True)
    lines = sink.read_bytes().splitlines(keepends=True)[:-1]
    positions = [i for i, line in enumerate(lines[1:], 1) if json.loads(line)[0] == tag]
    if tag in ("E", "O"):
        first, last = positions[0], positions[-1]
        lines[first], lines[last] = lines[last], lines[first]
    elif tag == "T":
        row = json.loads(lines[positions[0]])
        row[1] += 1
        lines[positions[0]] = wire(row)
    else:
        lines.append(lines[positions[0]])
    sink.write_bytes(refold(lines))
    # Prove the changed receipt and grammar really are accepted by SinkReader.
    with pf_sink.SinkReader(sink) as reader:
        for _ in reader:
            pass
        assert reader.complete
    result = invoke(sink, metadata)
    assert result.returncode == 3 and result.stdout == b""


@pytest.mark.parametrize("fault", ["missing-constants", "null-trade", "overflow-summary"])
def test_unreportable_verified_values_are_defects_not_invented_rows(tmp_path, fault):
    sink, metadata, _ = synthetic(tmp_path, outputs=True)
    lines = sink.read_bytes().splitlines(keepends=True)[:-1]
    if fault == "missing-constants":
        lines = [line for index, line in enumerate(lines)
                 if index == 0 or json.loads(line)[0] != "K"]
    else:
        for index in range(1, len(lines)):
            row = json.loads(lines[index])
            if row[0] == "T":
                row[8] = None if fault == "null-trade" else 1e308
                lines[index] = wire(row)
    sink.write_bytes(refold(lines))
    with pf_sink.SinkReader(sink) as reader:
        for _ in reader:
            pass
        assert reader.complete
    result = invoke(sink, metadata)
    assert result.returncode == 3 and result.stdout == b"", result.stderr.decode()
    assert diagnostics(result)[-1]["event"] == "sink_refused"


@pytest.mark.parametrize("position", ["summary", "metric", "shape", "window", "receipt"])
def test_lying_count_is_dropped_logged_and_ignored(tmp_path, position):
    sink, metadata, expected = synthetic(tmp_path)
    source = json.loads(metadata.read_bytes())
    if position == "summary":
        source["summary"]["total_trades"] += 7
        path = "$.summary.total_trades"
    elif position == "metric":
        source["metrics"]["all"]["num_wins"] += 7
        path = "$.metrics.all.num_wins"
    elif position == "shape":
        import selected_window_report
        expected["report_shape"] = selected_window_report.report_shape_full(
            len(expected["equity_curve"]), len(expected["trades"]))
        source["report_shape"] = dict(expected["report_shape"], emitted_trades=len(expected["trades"]) + 7)
        path = "$.report_shape.emitted_trades"
    elif position == "window":
        expected["report_window"] = {"counts": {"equity_points": len(expected["equity_curve"])}}
        source["report_window"] = {"counts": {"equity_points": len(expected["equity_curve"]) + 7}}
        path = "$.report_window.counts.equity_points"
    else:
        with pf_sink.SinkReader(sink) as reader:
            for _ in reader:
                pass
            source["report_sink"] = {"counts": dict(reader.counts, T=reader.counts["T"] + 7)}
        path = "$.report_sink.counts"
    metadata.write_bytes(report_bytes(source))
    result = invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    compare_derivable(expected, json.loads(result.stdout))
    if position not in ("shape", "window"):
        assert result.stdout == report_bytes(expected)
    drops = [line for line in diagnostics(result) if line["event"] == "metadata_drop"]
    assert len([line for line in drops if line["path"] == path]) == 1
    assert next(line for line in drops if line["path"] == path)["reason"] == "contradicts_verified_sink"


@pytest.mark.parametrize("key", ["net_pnl", "avg_trade", "best_trade", "worst_trade", "max_drawdown"])
def test_contradicting_trade_number_is_dropped(tmp_path, key):
    sink, metadata, expected = synthetic(tmp_path)
    source = json.loads(metadata.read_bytes())
    source["summary"][key] += 123.5
    metadata.write_bytes(report_bytes(source))
    result = invoke(sink, metadata)
    assert result.returncode == 0 and result.stdout == report_bytes(expected)
    assert any(line.get("path") == "$.summary." + key and line["reason"] == "contradicts_verified_sink"
               for line in diagnostics(result))


def test_unknown_and_invalid_nested_keys_have_one_line_per_drop(tmp_path):
    sink, metadata, expected = synthetic(tmp_path)
    source = json.loads(metadata.read_bytes())
    source["unrecognized"] = {"count_t": 9000}
    source["summary"]["unrecognized"] = 100
    source["diagnostics"]["input_bars_processed"] = True  # bool is never a count
    source["applied_runtime"]["unrecognized"] = [1, 2, 3]
    source["metrics"]["equity"]["unrecognized"] = 42.0
    source["elapsed_seconds"] = "not a number"
    metadata.write_bytes(report_bytes(source))
    del expected["diagnostics"]["input_bars_processed"]
    del expected["elapsed_seconds"]
    result = invoke(sink, metadata)
    assert result.returncode == 0 and result.stdout == report_bytes(expected)
    drops = [line for line in diagnostics(result) if line["event"] == "metadata_drop"]
    assert {line["path"] for line in drops} == {"$.unrecognized", "$.summary.unrecognized",
        "$.diagnostics.input_bars_processed", "$.applied_runtime.unrecognized",
        "$.metrics.equity.unrecognized", "$.elapsed_seconds"}
    assert len(drops) == 6


@pytest.mark.parametrize("kind", ["bad-json", "duplicate", "non-object", "invalid-utf8", "deep", "nan", "over-cap"])
@pytest.mark.parametrize("outputs", [False, True])
def test_garbage_summary_is_success_and_normal_charging(tmp_path, kind, outputs):
    sink, metadata, expected = synthetic(tmp_path, outputs=outputs)
    payloads = {"bad-json": b"garbage summary", "duplicate": b'{"summary":{},"summary":{}}',
                "non-object": b"[]", "invalid-utf8": b"\xff", "deep": b"[" * 2000 + b"]" * 2000,
                "nan": b'{"elapsed_seconds":NaN}',
                "over-cap": b"x" * (converter.STDOUT_JSON_MAX_BYTES + 1)}
    metadata.write_bytes(payloads[kind])
    result = invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    actual = json.loads(result.stdout)
    for key in ("trades", "equity_curve"):
        assert report_bytes(actual[key]) == report_bytes(expected[key])
    assert actual["summary"] == {key: value for key, value in expected["summary"].items()
                                 if key != "bars_processed"}
    if outputs:
        assert actual["outputs"]["bars"] == expected["outputs"]["bars"]
        assert len(actual["outputs"]["events"]) == len(expected["outputs"]["events"])
    lines = diagnostics(result)
    assert any(line.get("path") == "$" and line["event"] == "metadata_drop" for line in lines)
    assert lines[-1]["event"] == "converter_complete" and lines[-1]["billing"] == "normal"
    assert lines[-1]["sink_verified"] is True


def call_in_process(monkeypatch, sink, metadata, *, extra=()):
    captured = {1: bytearray(), 2: bytearray()}
    original_write = os.write

    def write(fd, data):
        if fd in captured:
            captured[fd].extend(data)
            return len(data)
        return original_write(fd, data)

    monkeypatch.setattr(converter.os, "write", write)
    code = converter.main(["--in", str(sink), "--stdout-json", str(metadata), "--out", "-",
                           "--max-bytes", str(sink.stat().st_size), *map(str, extra)])
    return types.SimpleNamespace(returncode=code, stdout=bytes(captured[1]), stderr=bytes(captured[2]))


def test_exact_metadata_cap_and_cap_plus_one_read_bound(tmp_path, monkeypatch):
    sink, metadata, expected = synthetic(tmp_path)
    prefix = report_bytes(projection(expected)).rstrip(b"\n")
    metadata.write_bytes(prefix + b" " * (converter.STDOUT_JSON_MAX_BYTES - len(prefix)))
    real_pread, metadata_reads = os.pread, []
    identity = metadata.stat().st_ino

    def read(fd, size, offset):
        if os.fstat(fd).st_ino == identity:
            metadata_reads.append((offset, size))
        return real_pread(fd, size, offset)

    monkeypatch.setattr(converter.os, "pread", read)
    result = call_in_process(monkeypatch, sink, metadata)
    assert result.returncode == 0 and result.stdout == report_bytes(expected)
    assert max(offset + size for offset, size in metadata_reads) <= converter.STDOUT_JSON_MAX_BYTES + 1
    metadata_reads.clear()
    metadata.write_bytes(metadata.read_bytes() + b"x" * (converter.STDOUT_JSON_MAX_BYTES * 2))
    result = call_in_process(monkeypatch, sink, metadata)
    assert result.returncode == 0
    assert sum(min(size, max(0, metadata.stat().st_size - offset))
               for offset, size in metadata_reads) == converter.STDOUT_JSON_MAX_BYTES + 1
    assert any(line.get("reason") == "stdout_json_over_cap" for line in diagnostics(result))


@pytest.mark.parametrize("stage", ["document", "preflight"])
def test_converter_fault_after_verify_is_distinct_engine_fault(tmp_path, monkeypatch, stage):
    sink, metadata, _ = synthetic(tmp_path)

    def crash(*args):
        raise RuntimeError("injected converter fault on a verified sink")

    monkeypatch.setattr(converter, "_document" if stage == "document" else "_emit", crash)
    result = call_in_process(monkeypatch, sink, metadata)
    assert result.returncode == 7 and result.stdout == b""
    line = diagnostics(result)[-1]
    assert line["event"] == "engine_fault" and line["sink_verified"] is True
    assert line["billing"] == "refunded"


def test_verify_is_first_and_converter_failure_before_it_is_not_a_refund(tmp_path, monkeypatch):
    sink, metadata, _ = synthetic(tmp_path)

    def crash(self):
        raise RuntimeError("injected before verification")

    def forbidden(*args):
        raise AssertionError("untrusted stdout.json was read before verification")

    monkeypatch.setattr(converter._Sink, "verify", crash)
    monkeypatch.setattr(converter, "_load_stdout", forbidden)
    result = call_in_process(monkeypatch, sink, metadata)
    line = diagnostics(result)[-1]
    assert result.returncode == 7 and result.stdout == b""
    assert line["event"] == "converter_fault" and line["billing"] == "normal"
    assert line["sink_verified"] is False


def test_changed_retained_file_is_refused_before_stdout(tmp_path, monkeypatch):
    sink, metadata, _ = synthetic(tmp_path)
    original = converter._document

    def change(source, verified, log):
        result = original(source, verified, log)
        with sink.open("ab") as stream:
            stream.write(b"x")
        return result

    monkeypatch.setattr(converter, "_document", change)
    result = call_in_process(monkeypatch, sink, metadata)
    assert result.returncode == 4 and result.stdout == b""


def test_inherited_readonly_fds_are_not_closed_or_repositioned(tmp_path, monkeypatch):
    sink, metadata, expected = synthetic(tmp_path, outputs=True)
    captured = {1: bytearray(), 2: bytearray()}

    def write(fd, data):
        assert fd in captured, "converter wrote a third descriptor"
        captured[fd].extend(data)
        return len(data)

    real_open = converter.os.open

    def readonly(path, flags, *args):
        assert flags & os.O_ACCMODE == os.O_RDONLY
        return real_open(path, flags, *args)

    monkeypatch.setattr(converter.os, "write", write)
    monkeypatch.setattr(converter.os, "open", readonly)
    with sink.open("rb") as retained, metadata.open("rb") as summary:
        retained.seek(7)
        summary.seek(3)
        code = converter.main(["--in-fd", str(retained.fileno()), "--stdout-json-fd",
            str(summary.fileno()), "--out", "-", "--max-bytes", str(sink.stat().st_size)])
        assert retained.tell() == 7 and summary.tell() == 3
        assert os.fstat(retained.fileno()).st_size == sink.stat().st_size
    assert code == 0 and bytes(captured[1]) == report_bytes(expected)


@pytest.mark.parametrize("late", [False, True])
def test_stdout_epipe_is_exit_five_and_never_success(tmp_path, late):
    sink, metadata, _ = synthetic(tmp_path, outputs=True)
    if late:
        write_stream(sink, fixtures.header(outputs=True), generated_rows(1024))
        metadata.write_bytes(b"{}\n")
    process = subprocess.Popen([sys.executable, "-I", "-B", "-S", str(TOOL),
        "--in", str(sink), "--stdout-json", str(metadata), "--out", "-", "--max-bytes",
        str(sink.stat().st_size)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, preexec_fn=no_file_writes)
    prefix = process.stdout.read(4096) if late else b""
    process.stdout.close()
    process.stdout = None
    _, stderr = process.communicate()
    result = types.SimpleNamespace(returncode=process.returncode, stderr=stderr)
    assert process.returncode == 5, stderr.decode()
    line = diagnostics(result)[-1]
    assert line["event"] == "converter_io" and line["sink_verified"] is True
    if late:
        # A pipe cannot retract this prefix. The caller MUST discard it on 5;
        # this pins the exact remaining CORE-TOP contract question honestly.
        assert prefix.startswith(b"{") and not prefix.endswith(b"}\n")


def test_child_arrays_and_output_constants_cannot_override_sink(tmp_path):
    sink, metadata, expected = synthetic(tmp_path, outputs=True)
    lying = json.loads(report_bytes(expected))
    lying["trades"][0]["qty"] += 1000.0
    lying["equity_curve"][0]["open_profit"] = 1000.0
    lying["outputs"]["constants"][0] += 1000.0
    lying["outputs"]["series"][0]["values"][0] = 1000.0
    lying["outputs"]["bars"]["open_ms"].append(1000)
    lying["outputs"]["events"][0]["sequence"] += 1000
    metadata.write_bytes(report_bytes(lying))
    result = invoke(sink, metadata)
    assert result.returncode == 0 and result.stdout == report_bytes(expected)
    paths = {line.get("path") for line in diagnostics(result)}
    assert {"$.trades", "$.equity_curve", "$.outputs.constants", "$.outputs.series",
            "$.outputs.bars", "$.outputs.events"} <= paths


def test_fingerprint_provenance_is_checked_and_identity_tracks_drops(tmp_path):
    sink, metadata, expected = synthetic(tmp_path)
    provenance = {"engine": {"version_string": "test"},
                  "strategy": {"initial_capital": 10000.0},
                  "inputs": {"zero": {"type": "float", "default": -0.0, "value": 1.0},
                             "wide": {"type": "float", "default": 1e20, "value": 1e20}}}
    expected["fingerprint"] = run_json.build_fingerprint(provenance)
    source = projection(expected)
    metadata.write_bytes(report_bytes(source))
    result = invoke(sink, metadata)
    assert result.returncode == 0 and result.stdout == report_bytes(expected)
    source["fingerprint"]["provenance"]["unknown"] = {"receipt": 999}
    raw = report_bytes(source["fingerprint"]["provenance"]).rstrip(b"\n")
    source["fingerprint"]["token"] = base64.b64encode(raw).decode("ascii")
    source["fingerprint"]["digest"] = "sha256:" + hashlib.sha256(raw).hexdigest()
    metadata.write_bytes(report_bytes(source))
    result = invoke(sink, metadata)
    assert result.returncode == 0
    actual = json.loads(result.stdout)["fingerprint"]
    assert actual["provenance"] == provenance
    assert "token" not in actual and "digest" not in actual
    assert {"$.fingerprint.provenance.unknown", "$.fingerprint.token", "$.fingerprint.digest"} <= {
        line.get("path") for line in diagnostics(result)}


def test_invalid_presentation_cannot_make_a_verified_sink_an_engine_fault(tmp_path):
    sink, metadata, expected = synthetic(tmp_path, outputs=True)
    source = projection(expected)
    source["summary"] = "nonsense"
    source["outputs"]["manifest"]["series"][0]["slot"] = 2**53
    source["outputs"]["manifest"]["outputs"][0]["index"] = -1
    source["fingerprint"] = {"token": "invalid base64!", "digest": "not a digest",
                             "provenance": {"engine": {"version_string": "lie"}}}
    metadata.write_bytes(report_bytes(source))
    result = invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    actual = json.loads(result.stdout)
    assert actual["trades"] == expected["trades"]
    assert actual["summary"] == {key: value for key, value in expected["summary"].items()
                                 if key != "bars_processed"}
    assert all(entry.get("index", 0) >= 0 for entry in actual["outputs"]["manifest"]["outputs"])
    assert diagnostics(result)[-1]["billing"] == "normal"


def test_missing_stdout_capture_is_a_logged_metadata_drop(tmp_path):
    sink, metadata, expected = synthetic(tmp_path)
    metadata.unlink()
    result = invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    actual = json.loads(result.stdout)
    assert actual["trades"] == expected["trades"]
    assert any(line.get("reason") == "stdout_json_unreadable" for line in diagnostics(result))


def test_sink_cap_run_id_and_empty_sink_are_refusals(tmp_path):
    sink, metadata, _ = synthetic(tmp_path)
    result = invoke(sink, metadata, extra=("--max-bytes", sink.stat().st_size - 1))
    assert result.returncode == 8 and result.stdout == b""
    result = invoke(sink, metadata, extra=("--run-id", "different"))
    assert result.returncode == 3 and result.stdout == b""
    sink.write_bytes(b"")
    result = invoke(sink, metadata, extra=("--max-bytes", "1"))
    assert result.returncode == 6 and result.stdout == b""


def test_readwrite_sink_and_live_pipe_are_usage_refusals(tmp_path):
    sink, metadata, _ = synthetic(tmp_path)
    read_fd, write_fd = os.pipe()
    try:
        with sink.open("r+b") as writable:
            for fd in (read_fd, writable.fileno()):
                result = subprocess.run([sys.executable, "-I", "-B", "-S", str(TOOL),
                    "--in-fd", str(fd), "--stdout-json", str(metadata), "--out", "-",
                    "--max-bytes", str(sink.stat().st_size)], pass_fds=(fd,),
                    stdin=subprocess.DEVNULL, capture_output=True)
                assert result.returncode == 2 and result.stdout == b""
    finally:
        os.close(read_fd)
        os.close(write_fd)


@pytest.mark.parametrize("arguments", [[], ["--in-fd", "-1"],
    ["--in-fd", "3", "--stdout-json-fd", "3", "--out", "-", "--max-bytes", "1"],
    ["--in-fd", "3", "--stdout-json", "missing", "--out", "-", "--max-bytes", "0"],
    ["--in", "missing", "--stdout-json", "missing", "--out", "file", "--max-bytes", "1"]])
def test_bad_invocation_has_no_report(arguments):
    result = subprocess.run([sys.executable, "-I", "-B", "-S", str(LAUNCHER), *arguments],
                            stdin=subprocess.DEVNULL, capture_output=True)
    assert result.returncode == 2 and result.stdout == b""


def test_large_interleaved_sink_has_bounded_memory_across_replays(tmp_path):
    header_path = tmp_path / "header.json"
    header_path.write_bytes(wire(fixtures.header(outputs=True)))
    results = []
    for count in (1024, LARGE_ROWS):
        result = subprocess.run([sys.executable, "-B", "-S", str(SELF), "--rss-probe",
            str(tmp_path / str(count)), str(header_path), str(count)],
            stdin=subprocess.DEVNULL, capture_output=True)
        assert result.returncode == 0, result.stderr.decode()
        measured = json.loads(result.stdout)
        assert measured["returncode"] == 0, measured
        log = [json.loads(line) for line in measured["stderr"].splitlines()]
        assert log[-1]["event"] == "converter_complete" and log[-1]["sink_verified"] is True
        assert 0 < measured["peak_bytes"] < RSS_LIMIT_BYTES, measured
        assert measured["received_bytes"] > count * 100 and measured["complete_json_tail"], measured
        assert measured["files_unchanged"]
        assert measured["counts"]["E"] == measured["counts"]["T"] == measured["counts"]["O"] == count
        assert measured["counts"]["V"] == count and measured["counts"]["K"] == 1
        print(json.dumps({"probe": "sink_json_rss", **measured}, separators=(",", ":")), flush=True)
        results.append(measured)
    small, large = results
    assert large["sink_bytes"] >= 128 * 1024 * 1024, large
    assert large["peak_bytes"] <= small["peak_bytes"] + RSS_GROWTH_BYTES, results
    assert large["output_sha256"] != small["output_sha256"]
