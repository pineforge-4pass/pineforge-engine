"""CR-150 L2: binary load contract, ownership and CSV/report equivalence.

The red commit keeps the L1 implementation unchanged. The CLI success row
therefore fails because --ohlcv-bin cannot select a chart, and the mapped
reader rows fail because load_bars_bin is missing. No test is run on the Mac.
Native comparison uses a real CMake-built module, never the Python stand-in.
"""
from __future__ import annotations

import contextlib
import ctypes
import hashlib
import io
import json
import mmap
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest
import pf_bars
import run_json


ROOT = Path(__file__).resolve().parents[1]
T0 = 1735776000000
BAR = struct.Struct("<5dq")
CLOSE = struct.Struct("<q")
PREFIX = b"pineforge:ohlcv:barc-le:v1\0"
FEED_PREFIX = b"pineforge:symbol-feed:barc-close-le:v1\0"
BORROWED = "strategy_set_symbol_feed_borrowed_v1"
HEADER = "timestamp,open,high,low,close,volume\n"


def csv_file(tmp_path, name="chart.csv", count=5, step=60000):
    path = tmp_path / name
    path.write_text(HEADER + "".join(
        f"{T0 + i * step},{100 + i},{102 + i},{99 + i},{101 + i},{i}\n"
        for i in range(count)), encoding="utf-8")
    return path


def binary_file(tmp_path, *, feed=False, values=None, closes=None, name="bars.bin"):
    values = ([(100., 102., 99., 101., 3., T0),
               (101., 103., 100., 102., 4., T0 + 60000)] if values is None else values)
    path = tmp_path / name
    payload = b"".join(BAR.pack(*row) for row in values)
    path.write_bytes(payload)
    metadata = {"format": "pf_bar_t/v1", "record_size": 48, "symbol": "descriptive",
                "tf": "1", "count": len(values), "first_ts": values[0][5] if values else 0,
                "last_ts": values[-1][5] if values else 0,
                "sha256": hashlib.sha256(payload).hexdigest()}
    if feed:
        closes = [row[5] + 60000 for row in values] if closes is None else closes
        close_bytes = b"".join(CLOSE.pack(close) for close in closes)
        path.with_suffix(".close.bin").write_bytes(close_bytes)
        metadata["close_sha256"] = hashlib.sha256(close_bytes).hexdigest()
    write_sidecar(path, metadata)
    return path, metadata


def write_sidecar(path, metadata):
    Path(str(path) + ".json").write_text(
        json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


def index_file(tmp_path, *, chart=None, feeds=None):
    doc = {"symbols": {"EXCHANGE:OTHER": {"feeds": feeds or {}}}}
    if chart is not None:
        doc["chart"] = {"symbol": "LAKE", "tf": "1", "file": str(chart)}
    path = tmp_path / "symbols.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


class Library:
    """Only a C-call stand-in; snapshots bytes, never retains exported buffers."""

    def __init__(self, *, borrowed=True, fail_run=False):
        self.calls, self.rows, self.feeds = [], [], []
        self.fail_run = fail_run
        exports = {
            "pf_abi_version": lambda: run_json.EXPECTED_PF_ABI,
            "strategy_create": lambda params: 7,
            "strategy_set_input": lambda *args: None,
            "strategy_set_override": lambda *args: None,
            "strategy_set_symbol_facts": lambda *args: 0,
            "strategy_set_symbol_feed": lambda *args: self.feed("copy", *args),
            "run_backtest_full": self.run,
            "report_free": lambda *args: None,
            "strategy_free": lambda *args: None,
        }
        if borrowed:
            exports[BORROWED] = lambda *args: self.feed("borrow", *args)
        for name, body in exports.items():
            setattr(self, name, self.export(name, body))

    def export(self, name, body):
        def call(*args):
            self.calls.append(name)
            try:
                return body(*args)
            finally:
                args = None
        return call

    def feed(self, kind, state, symbol, tf, bars, closes, count):
        self.feeds.append((kind, symbol, tf, ctypes.string_at(bars, count * 48),
                           ctypes.string_at(closes, count * 8)))
        return 0

    def run(self, state, bars, count, *args):
        self.rows.append(ctypes.string_at(bars, count * 48))
        if self.fail_run:
            # Drop argument references before raising, as a C call does.
            bars = args = None
            raise RuntimeError("injected execution failure")


def invoke(monkeypatch, *args, library=None):
    library = Library() if library is None else library
    monkeypatch.setattr(run_json, "load_strategy", lambda path: library)
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = run_json.main(["--so", "unused.so", *map(str, args)])
    return status, json.loads(out.getvalue()), err.getvalue(), library


def mask_report(document):
    document = json.loads(json.dumps(document))
    document.pop("elapsed_seconds", None)
    document["input"].pop("ohlcv", None)
    diagnostics = document.get("diagnostics", {})
    diagnostics.pop("timing", None)
    diagnostics.pop("throughput", None)
    return document


def test_binary_chart_cli_is_a_chart_source(tmp_path, monkeypatch):
    """Fail-before: the L1 parser still requires --ohlcv and has no binary flag."""
    source = csv_file(tmp_path)
    binary = tmp_path / "chart.bin"
    pf_bars.convert_csv(source, binary, symbol="LAKE", tf="1")
    csv_result = invoke(monkeypatch, "--ohlcv", source)
    bin_result = invoke(monkeypatch, "--ohlcv-bin", binary)
    assert csv_result[0] == bin_result[0] == 0
    assert mask_report(csv_result[1]) == mask_report(bin_result[1])
    assert csv_result[3].rows == bin_result[3].rows == [binary.read_bytes()]


@pytest.mark.parametrize("volume", ["0", "-0", "nan", "-nan", "NaN"])
def test_mapped_bytes_and_source_identity_preserve_float_bits(tmp_path, volume):
    source = tmp_path / "bits.csv"
    source.write_text(HEADER + f"{T0},1,2,0.5,1.5,{volume}\n", encoding="utf-8")
    binary = tmp_path / "bits.bin"
    pf_bars.convert_csv(source, binary, symbol="LAKE", tf="1")
    csv_bars, count, digest = run_json.load_bars(source)
    with run_json.load_bars_bin(binary) as mapped:
        assert mapped["n"] == count
        assert mapped["source_values_sha256"] == digest
        assert ctypes.string_at(mapped["bars"], count * 48) == ctypes.string_at(csv_bars, count * 48)
        before = binary.read_bytes()
        mapped["bars"][0].close = 1.75
        assert binary.read_bytes() == before  # private COW; never writes the file
    assert mapped["bars"] is None


@pytest.mark.parametrize("verify", ["full", "sidecar"])
@pytest.mark.parametrize("feed", [False, True])
def test_order_is_checked_even_without_digest_verification(tmp_path, verify, feed):
    path, _ = binary_file(tmp_path, feed=feed, values=[
        (1, 2, .5, 1.5, 3, T0), (1, 2, .5, 1.5, 3, T0)])
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.load_bars_bin(path, verify_bin=verify, feed=feed)
    assert caught.value.code == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert caught.value.code_args == {"reason": "feed_not_increasing" if feed else "order"}


@pytest.mark.parametrize("feed", [False, True])
@pytest.mark.parametrize("fault,reason", [
    ("size", "size"), ("zero", "empty"), ("count", "count"),
    ("missing-sidecar", "sidecar"), ("count-sidecar", "sidecar"),
    ("endpoint", "sidecar"), ("digest", "sidecar"), ("format", "sidecar"),
])
def test_binary_refusals_and_mapping_cleanup(tmp_path, monkeypatch, feed, fault, reason):
    path, metadata = binary_file(tmp_path, feed=feed)
    if fault == "size":
        path.write_bytes(path.read_bytes() + b"x")
    elif fault == "zero":
        path.write_bytes(b"")
    elif fault == "count":
        monkeypatch.setattr(pf_bars, "MAX_COUNT", 1)
    elif fault == "missing-sidecar":
        path.rename(tmp_path / "without-sidecar.bin")
        path = tmp_path / "without-sidecar.bin"
    else:
        key, value = {"count-sidecar": ("count", 1), "endpoint": ("last_ts", T0),
                      "digest": ("sha256", "0" * 64), "format": ("format", "future/v9")}[fault]
        metadata[key] = value
        write_sidecar(path, metadata)
    maps, real_mmap = [], mmap.mmap
    def track(*args, **kwargs):
        assert kwargs["access"] == mmap.ACCESS_COPY
        result = real_mmap(*args, **kwargs)
        maps.append(result)
        return result
    monkeypatch.setattr(run_json.mmap, "mmap", track)
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.load_bars_bin(path, feed=feed)
    assert caught.value.code_args == {"reason": reason}
    assert caught.value.code == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert all(mm.closed for mm in maps)


@pytest.mark.parametrize("feed", [False, True])
def test_sidecar_mode_omits_both_digest_comparisons(tmp_path, feed):
    path, metadata = binary_file(tmp_path, feed=feed)
    metadata["sha256"] = "0" * 64
    if feed:
        metadata["close_sha256"] = "f" * 64
    write_sidecar(path, metadata)
    with run_json.load_bars_bin(path, verify_bin="sidecar", feed=feed) as mapped:
        expected = hashlib.sha256(FEED_PREFIX if feed else PREFIX)
        if feed:
            for i in range(mapped["n"]):
                expected.update(path.read_bytes()[48 * i:48 * (i + 1)])
                expected.update(CLOSE.pack(mapped["close_ms"][i]))
        else:
            expected.update(path.read_bytes())
        assert mapped["source_values_sha256"] == expected.hexdigest()


@pytest.mark.parametrize("feed", [False, True])
def test_hash_scan_crosses_the_chunk_boundary(tmp_path, feed):
    source = csv_file(tmp_path, count=23000)
    binary = tmp_path / "large.bin"
    pf_bars.convert_csv(source, binary, symbol="LAKE", tf="1", feed=feed)
    expected = (run_json._load_symbol_feed(source, "OTHER", "1")["record"]["source_values_sha256"]
                if feed else run_json.load_bars(source)[2])
    with run_json.load_bars_bin(binary, feed=feed) as mapped:
        assert mapped["n"] == 23000
        assert mapped["source_values_sha256"] == expected


@pytest.mark.parametrize("feed", [False, True])
@pytest.mark.parametrize("field,bad", [(0, float("nan")), (3, float("inf")),
                                      (4, -1.0), (4, float("inf"))])
def test_binary_value_checks_are_not_bypassed_in_sidecar_mode(tmp_path, feed, field, bad):
    row = [1, 2, .5, 1.5, 3, T0]
    row[field] = bad
    path, _ = binary_file(tmp_path, feed=feed, values=[row])
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.load_bars_bin(path, verify_bin="sidecar", feed=feed)
    assert caught.value.code_args == {"reason": "feed_value_invalid" if feed else "value"}


@pytest.mark.parametrize("feed", [False, True])
def test_unreadable_binary_uses_the_mode_code(tmp_path, feed):
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.load_bars_bin(tmp_path / "missing.bin", feed=feed)
    assert caught.value.code == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert caught.value.code_args == {"reason": "feed_unreadable" if feed else "io"}


@pytest.mark.parametrize("fault,reason", [
    ("absent", "close_file"), ("size", "close_file"), ("hash", "sidecar"),
    ("before", "feed_close_time_invalid"), ("overlap", "feed_close_time_invalid"),
    ("range", "feed_time_out_of_range"),
])
def test_feed_close_contract(tmp_path, fault, reason):
    closes = {"before": [T0, T0 + 120000], "overlap": [T0 + 60001, T0 + 120000],
              "range": [T0 + 60000, 2**53]}.get(fault)
    path, metadata = binary_file(tmp_path, feed=True, closes=closes)
    close = path.with_suffix(".close.bin")
    if fault == "absent":
        close.rename(tmp_path / "saved.close.bin")
    elif fault == "size":
        close.write_bytes(b"x")
    elif fault == "hash":
        metadata["close_sha256"] = "0" * 64
        write_sidecar(path, metadata)
    with pytest.raises(run_json.RunFailure) as caught:
        run_json.load_bars_bin(path, feed=True)
    assert (caught.value.code, caught.value.code_args) == ("symbol_feeds_refused", {"reason": reason})


def test_mixed_feeds_use_borrowed_and_copied_setters(tmp_path, monkeypatch):
    chart = csv_file(tmp_path)
    feed_csv = csv_file(tmp_path, "feed.csv", step=120000)
    feed_bin = tmp_path / "feed.bin"
    pf_bars.convert_csv(feed_csv, feed_bin, symbol="OTHER", tf="1", feed=True)
    index = index_file(tmp_path, feeds={"1": feed_bin.name, "2": feed_csv.name})
    library = Library()
    status, report, _, _ = invoke(monkeypatch, "--ohlcv", chart, "--symbol-feeds", index,
                                  "--bench", "--warmup", "1", "--repeats", "2", library=library)
    assert status == 0, report
    assert [feed[0] for feed in library.feeds] == ["borrow", "copy"] * 4
    assert library.feeds[0][3] == feed_bin.read_bytes()
    assert library.feeds[0][4] == feed_bin.with_suffix(".close.bin").read_bytes()


def test_binary_feed_identity_equals_csv_and_requires_borrowed_export(tmp_path):
    source = csv_file(tmp_path)
    binary = tmp_path / "feed.bin"
    pf_bars.convert_csv(source, binary, symbol="descriptive only", tf="1", feed=True)
    expected = run_json._load_symbol_feed(source, "EXCHANGE:OTHER", "1")["record"]
    index = index_file(tmp_path, feeds={"1": binary.name})
    symbols = run_json.load_symbol_feeds(index)
    try:
        assert symbols[0]["feeds"][0]["record"] == expected
        with pytest.raises(run_json.SymbolFeedsError) as caught:
            run_json.install_symbol_feeds(Library(borrowed=False), 7, symbols)
        assert caught.value.code_args == {"reason": "library_without_symbol_feeds"}
        assert BORROWED in str(caught.value)
    finally:
        run_json.close_symbol_feeds(symbols)


def test_zero_bar_feed_stays_a_header_only_csv_in_a_binary_index(tmp_path, monkeypatch):
    path, _ = binary_file(tmp_path)
    source = csv_file(tmp_path, "empty.csv", count=0)
    index = index_file(tmp_path, chart=path.name, feeds={"1": source.name})
    status, report, _, library = invoke(monkeypatch, "--symbol-feeds", index)
    assert status == 0, report
    assert library.feeds == [("copy", b"EXCHANGE:OTHER", b"1", b"", b"")]
    record = report["applied_runtime"]["symbol_feeds"]["symbols"]["EXCHANGE:OTHER"]["feeds"]["1"]
    assert record == {"bars": 0, "source_values_sha256": hashlib.sha256(FEED_PREFIX).hexdigest()}


@pytest.mark.parametrize("feed", [False, True])
def test_verify_flag_reaches_chart_and_feed_readers(tmp_path, monkeypatch, feed):
    source = csv_file(tmp_path)
    path, metadata = binary_file(tmp_path, feed=feed)
    metadata["sha256"] = "0" * 64
    write_sidecar(path, metadata)
    args = (["--ohlcv", source, "--symbol-feeds", index_file(tmp_path, feeds={"1": path.name})]
            if feed else ["--ohlcv-bin", path])
    full = invoke(monkeypatch, *args)
    assert full[0] == 1
    assert (full[1]["code"], full[1]["args"]) == (
        "symbol_feeds_refused" if feed else "chart_bars_unreadable", {"reason": "sidecar"})
    sidecar = invoke(monkeypatch, *args, "--verify-bin", "sidecar")
    assert sidecar[0] == 0, sidecar[1]


def test_partial_feed_load_closes_already_mapped_inputs(tmp_path, monkeypatch):
    path, _ = binary_file(tmp_path, feed=True)
    index = index_file(tmp_path, feeds={"1": path.name, "2": "missing.bin"})
    maps, real_mmap = [], mmap.mmap
    def track(*args, **kwargs):
        result = real_mmap(*args, **kwargs)
        maps.append(result)
        return result
    monkeypatch.setattr(run_json.mmap, "mmap", track)
    with pytest.raises(run_json.SymbolFeedsError) as caught:
        run_json.load_symbol_feeds(index)
    assert caught.value.code_args == {"reason": "feed_unreadable"}
    assert len(maps) == 2 and all(mm.closed for mm in maps)


def test_chart_index_resolves_relative_paths_and_matching_explicit_source(tmp_path, monkeypatch):
    path, _ = binary_file(tmp_path)
    index = index_file(tmp_path, chart=path.name)
    first = invoke(monkeypatch, "--symbol-feeds", index)
    second = invoke(monkeypatch, "--symbol-feeds", index, "--ohlcv-bin", path.parent / "." / path.name)
    assert first[0] == second[0] == 0
    assert mask_report(first[1]) == mask_report(second[1])
    assert first[1]["input"]["ohlcv"] == str(path)


@pytest.mark.parametrize("source", ["none", "both", "index-conflict", "csv-index", "selected"])
def test_chart_source_conflicts_are_request_failures(tmp_path, monkeypatch, source):
    path, _ = binary_file(tmp_path)
    csv = csv_file(tmp_path)
    index = index_file(tmp_path, chart=path.name)
    options = {"none": [], "both": ["--ohlcv", csv, "--ohlcv-bin", path],
               "index-conflict": ["--symbol-feeds", index, "--ohlcv-bin", tmp_path / "other.bin"],
               "csv-index": ["--symbol-feeds", index, "--ohlcv", csv],
               "selected": ["--ohlcv-bin", path, "--report-policy", "selected-window/v1"]}[source]
    status, report, _, library = invoke(monkeypatch, *options)
    assert status in (1, 2)
    assert report["code"] == "run_request_invalid"
    assert report["args"]["option"] in {"ohlcv", "ohlcv_bin"}
    assert not library.calls


def test_legacy_ohlcv_abbreviation_and_new_verify_option(tmp_path, monkeypatch):
    source = csv_file(tmp_path)
    status, _, _, _ = invoke(monkeypatch, "--ohl", source)
    assert status == 0  # formerly unique before --ohlcv-bin was added
    status, report, _, _ = invoke(monkeypatch, "--ohlcv", source, "--verify-bin", "wrong")
    assert status == 2
    assert report["args"] == {"option": "verify_bin"}


def test_maps_outlive_every_native_cleanup_and_close_on_error(tmp_path, monkeypatch):
    path, _ = binary_file(tmp_path)
    feed, _ = binary_file(tmp_path, feed=True, name="feed.bin")
    index = index_file(tmp_path, feeds={"1": feed.name})
    maps, real_mmap = [], mmap.mmap
    def track(*args, **kwargs):
        result = real_mmap(*args, **kwargs)
        maps.append(result)
        return result
    monkeypatch.setattr(run_json.mmap, "mmap", track)
    for fail in (False, True):
        maps.clear()
        library = Library(fail_run=fail)
        def freed(*args):
            assert len(maps) == 3 and all(not mm.closed for mm in maps)
        library.report_free = library.export("report_free", freed)
        library.strategy_free = library.export("strategy_free", freed)
        status, _, _, _ = invoke(monkeypatch, "--ohlcv-bin", path, "--symbol-feeds", index,
                                  library=library)
        assert status == int(fail)
        assert library.calls[-2:] == ["report_free", "strategy_free"]
        assert all(mm.closed for mm in maps)


def test_big_endian_map_is_refused(tmp_path, monkeypatch):
    path, _ = binary_file(tmp_path)
    monkeypatch.setattr(run_json.sys, "byteorder", "big")
    with pytest.raises(run_json.ChartBarsError) as caught:
        run_json.load_bars_bin(path)
    assert caught.value.code_args == {"reason": "value"}


def test_borrowed_ctypes_signature(tmp_path, monkeypatch):
    artifact = tmp_path / "strategy.so"
    artifact.write_bytes(b"C calls are replaced only in this binding test")
    library = Library()
    monkeypatch.setattr(ctypes, "CDLL", lambda path: library)
    loaded = run_json.load_strategy(artifact)
    assert getattr(loaded, BORROWED).argtypes == [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.POINTER(run_json.BarC), ctypes.POINTER(ctypes.c_int64), ctypes.c_int]
    assert getattr(loaded, BORROWED).restype is ctypes.c_int


@pytest.mark.parametrize("borrowed", [False, True])
def test_binary_capability_requires_the_borrowed_runtime(monkeypatch, borrowed):
    status, document, _, library = invoke(monkeypatch, "--capabilities-json",
                                         library=Library(borrowed=borrowed))
    assert status == 0
    assert document["capabilities"].get("bars_bin_version") == (1 if borrowed else None)
    assert not library.calls  # load_strategy is the stand-in; no construction/run


def test_native_csv_binary_report_bytes(tmp_path):
    """Real outputs module; CTest supplies its target path in normal builds."""
    module = os.environ.get("PINEFORGE_BIN_STRATEGY_SO")
    if not module:
        pytest.skip("SKIP native module not supplied: set PINEFORGE_BIN_STRATEGY_SO; CTest wires outputs_fixture_strategy")
    assert Path(module).is_file(), module
    source = csv_file(tmp_path, count=97)
    binary = tmp_path / "chart.bin"
    pf_bars.convert_csv(source, binary, symbol="LAKE", tf="1")
    documents = []
    for flag, path in (("--ohlcv", source), ("--ohlcv-bin", binary)):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "docker/run_json.py"),
                                 "--so", module, flag, str(path), "--input-tf", "1",
                                 "--script-tf", "1", "--outputs"], capture_output=True, text=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert len(result.stdout.splitlines()) == 1
        document = json.loads(result.stdout)
        assert document["input"]["bars"] == 97
        assert document["outputs"]["bars"]  # engine execution produced the body
        documents.append(mask_report(document))
    assert json.dumps(documents[0], separators=(",", ":")) == json.dumps(documents[1], separators=(",", ":"))
