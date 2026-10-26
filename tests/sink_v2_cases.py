"""Input fixtures and an independent v1 text oracle, never reader output."""
from __future__ import annotations

from dataclasses import dataclass
import base64
import hashlib
import io
from itertools import groupby
import json
import math
from pathlib import Path
import re
import types
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = ROOT / "docs" / "design" / "sink-v2-vectors"

# Literal schema from DESIGN 3.7; intentionally not taken from pf_sink2.
SCHEMA = {
    "E": (("time_ms", "equity", "open_profit"), ("i64", "f64?", "f64?"),
          ("unix_ms", "account_ccy", "account_ccy")),
    "T": (("n", "side", "entry_time", "exit_time", "entry_price", "exit_price", "qty",
           "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission", "entry_bar_index",
           "exit_bar_index", "open_at_end", "entry_incarnation", "close_cause", "entry_id",
           "exit_id", "exit_comment"),
          ("i64", "str", "i64", "i64") + ("f64?",) * 8 +
          ("i64", "i64", "bool", "u64", "i64", "str", "str", "str"),
          ("", "", "unix_ms", "unix_ms", "quote_ccy", "quote_ccy", "qty", "account_ccy",
           "percent", "account_ccy", "account_ccy", "account_ccy", "script_bar", "script_bar",
           "", "", "", "", "", "")),
    "V": (("sequence", "output_index", "bar_index", "bar_open_ms", "bar_close_ms", "ordinal_in_bar",
           "phase", "confirmed", "value", "message_hash64", "message"),
          ("u64", "i64", "i64", "i64?", "i64?", "i64", "i64", "i64", "f64?", "hex64", "str?"),
          ("", "", "", "unix_ms", "unix_ms", "", "", "", "", "", "")),
    "X": (("timestamp", "bar_index", "name", "value"), ("i64", "i64", "str", "f64?"),
          ("unix_ms", "", "", "")),
    "H": (("hash",), ("hex64",), ("",)),
}


def header(run_id: str = "test-v2", *, outputs: bool = False, slots: int = 2,
           constants: int = 2, extras: bool = False) -> dict[str, Any]:
    descriptors = []
    for tag in "ETOVKXH":
        if (tag in "OVK" and not outputs) or (tag in "XH" and not extras):
            continue
        if tag == "O":
            columns = (("row", "open_ms", "close_ms") + tuple(f"v{i}" for i in range(slots)),
                       ("i64", "i64?", "i64?") + ("f64?",) * slots,
                       ("", "unix_ms", "unix_ms") + ("",) * slots)
        elif tag == "K":
            columns = (tuple(f"c{i}" for i in range(constants)), ("f64?",) * constants, ("",) * constants)
        else:
            columns = SCHEMA[tag]
        descriptors.append({"type": tag, "fields": list(columns[0]), "types": list(columns[1]),
                            "units": list(columns[2])})
    return {"schema": "pf-report-sink", "version": 1, "run_id": run_id,
            "engine": "1.7.0", "records": descriptors}


def sink_folds(rows) -> dict[str, Any]:
    closed = opened = 0
    pnl = 0.0
    for row in rows:
        if row[0] == "T":
            if row[15]:
                opened += 1
                if row[8] is not None and math.isfinite(row[8]):
                    pnl = pnl + row[8]
            else:
                closed += 1
    return {"closed_count": closed, "open_at_end_count": opened, "open_at_end_pnl": pnl}


def metadata(name: str, rows=(), *, outputs: bool = False, selected: bool = False) -> dict[str, Any]:
    result = {"engine": "pineforge", "input": {"fixture": name}, "applied_inputs": {},
              "applied_overrides": {}, "applied_runtime": {}, "elapsed_seconds": -0.0,
              "summary": {}, "metrics": {"all": {}, "long": {}, "short": {}, "equity": {}},
              "diagnostics": {}, "fingerprint": None}
    if outputs:
        result["outputs"] = {"schema_version": 1, "message_format": "fixture",
                             "manifest_sha256": "", "manifest": [], "constants": [], "hlines": []}
    if selected:
        window = {"startMs": 1700000000000, "endMs": 1700002000000,
                  "additive_future_member": {"calendar": "UTC", "qualification": ["anchor", -0.0]}}
        result["applied_runtime"]["report_window"] = window
        result["fingerprint"] = {"version": 2, "provenance": {"runtime": {"report_window": window}}}
        result["report_window"] = window
        result["report_shape"] = {"anchor": True, "extra": {"retained": "完整"}}
        result["diagnostics"]["timing"] = {"samples_ns": [123456789, 234567890], "captured": True}
        result["diagnostics"]["throughput"] = {"bars_per_second": 1.0000000000000002}
        result["diagnostics"]["phase_timing"] = {"result_assembly": -0.0, "serialization": 1e-7}
    result["sink_folds"] = sink_folds(rows)
    return result


def normalized(rows: list[list[Any]], head: dict[str, Any]) -> list[list[Any]]:
    """Oracle normalization from declared input types; no encoder calls."""
    types = {d["type"]: d["types"] for d in head["records"]}
    result = []
    for row in rows:
        out = [row[0]]
        for value, wire in zip(row[1:], types[row[0]]):
            if wire.removesuffix("?") == "str" and isinstance(value, bytes):
                # surrogateescape exposes each invalid source byte separately.
                text = value.decode("utf-8", "surrogateescape")
                value = "".join("\ufffd" if 0xD800 <= ord(c) <= 0xDFFF else c for c in text)
            if wire == "f64?" and value is not None:
                value = float(value)
                if not math.isfinite(value):
                    value = None
            if row[0] in "OV" and wire == "i64?" and value == -(1 << 63):
                value = None
            out.append(value)
        result.append(out)
    return result


def v1_line(value: Any) -> bytes:
    # json.dumps provides shortest Python float repr. Replace only actual
    # short escapes; the regex consumes escaped backslashes as pairs first.
    text = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    control = {r"\b": r"\u0008", r"\f": r"\u000c", r"\n": r"\u000a",
               r"\r": r"\u000d", r"\t": r"\u0009"}
    text = re.sub(r'\\(?:\\|["/bfnrt])', lambda match: control.get(match[0], match[0]), text)
    return (text + "\n").encode("utf-8")


def v1_file(head: dict[str, Any], rows: list[list[Any]]) -> bytes:
    rows = normalized(rows, head)
    lines = [v1_line(head), *(v1_line(row) for row in rows)]
    counts = {d["type"]: sum(row[0] == d["type"] for row in rows) for d in head["records"]}
    digest = hashlib.sha256(b"".join(lines)).hexdigest()
    return b"".join(lines) + v1_line({"trailer": "pf-report-sink", "version": 1,
                                     "counts": counts, "sha256": digest})


def trade(n: int, *, key: float | None = 1.0) -> list[Any]:
    # Nonmonotone clocks and repeat/null keys exercise all comparator rules.
    return ["T", n, "short" if n % 2 == 0 else "long", 1700000000000 + (n * 11 % 37) * 60000,
            1700000000000 + (n * 7 % 37) * 60000, 100.25, 101.5, 2.0, key,
            None if n % 5 == 0 else key, -0.0 if n % 3 == 0 else key,
            None if n % 4 == 0 else -1.0, 0.5, n, n + 1, n % 3 == 0, n, 1,
            'entry\\n/"雪"', "", "\x00\b\f\n\r\t"]


def report_trade(n: int, *, key: float = 1.0) -> list[Any]:
    row = trade(n, key=key)
    for i in range(5, 13):
        if row[i] is None:
            row[i] = 0.0
    return row


def all_types_rows() -> list[list[Any]]:
    safe = (1 << 53) - 1
    rows = [
        ["E", -safe, -0.0, 0.0],
        ["O", 0, None, -(1 << 63), None, -0.0],
        ["V", safe, 0, safe, None, safe, 0, 1, 1, float("nan"), "ffffffffffffffff", None],
        ["T", *trade(1)[1:]],
        ["X", -safe, safe, 'say "雪"\\\n\x00', float("inf")],
        ["H", "8000000000000000"],
        ["E", 0, float("nan"), float("-inf")],
        ["O", 1, -safe, safe, 5e-324, float("inf")],
        ["V", 0, 1, -safe, -safe, None, 1, -7, -1, -0.0, "0000000000000000", ""],
        ["E", safe, 1.7976931348623157e308, -1.7976931348623157e308],
        ["V", 1, 0, 0, 0, 1, 2, 2, 1, 2.2250738585072014e-308,
         "ffffffffffffffff", 'controls:\x00\b\f\n\r\t / \\n 雪 🦊'],
        ["K", float("nan"), -0.0],
        ["X", safe, -safe, "", 1.0000000000000002],
        ["H", "ffffffffffffffff"],
    ]
    return rows


def admitted_equity_rows(rows: list[list[Any]]) -> list[list[Any]]:
    """Success-report inputs for main's finite-E contract, not normalization
    in the encoder. Other binary/query fixtures retain their nullable E."""
    result = [list(row) for row in rows]
    for row in result:
        if row[0] == "E":
            for column in (2, 3):
                if row[column] is None or not math.isfinite(row[column]):
                    row[column] = 0.0
    return result


def query_rows() -> list[list[Any]]:
    rows = []
    equities = (100.0, 100.0, 105.0, 105.0, 95.0, None, -0.0, 0.0, 1e308, -1e308)
    for i in range(37):
        if i < 23:
            value = equities[i % len(equities)]
            profit = -1e308 if value == 1e308 else None if i % 7 == 0 else float(i % 3)
            rows.append(["E", 1700000000000 + max(0, i - 1) * 60000, value, profit])
        key = None if i % 6 == 0 else -0.0 if i % 5 == 0 else float(i % 4 - 2)
        rows.append(trade(i + 1, key=key))
    return rows


def decode_frames(tag: str, column: int, raw: bytes) -> bytes | None:
    """Hand RFC8878 frames: raw timestamp and RLE+raw XOR byte planes."""
    if tag != "E" or column not in (0, 2):
        return None
    assert len(raw) < 256
    prefix = b"\x28\xb5\x2f\xfd\x20" + bytes((len(raw),))
    if column == 0:
        return prefix + ((len(raw) << 3) | 1).to_bytes(3, "little") + raw
    zeros = len(raw) - len(raw.lstrip(b"\x00"))
    assert zeros > 0 and zeros < len(raw)
    rle = ((zeros << 3) | 2).to_bytes(3, "little") + b"\x00"
    remainder = raw[zeros:]
    return prefix + rle + ((len(remainder) << 3) | 1).to_bytes(3, "little") + remainder


def summary_raw_frame(data: bytes) -> bytes:
    """Independent RFC8878 fixture, including the 257-byte section-13.1 frame.

    No admission policy here; J-only controls use summary_window_frame so an
    over-budget J does not add a single-segment window fault.
    This supplies decode-only expansion, not proof of incompressibility at a
    library's HEAD level.
    """
    size = len(data)
    if size < 256:
        prefix = b"\x28\xb5\x2f\xfd\x20" + bytes((size,))
    elif size < 65792:
        prefix = b"\x28\xb5\x2f\xfd\x60" + (size - 256).to_bytes(2, "little")
    else:
        prefix = b"\x28\xb5\x2f\xfd\xa0" + size.to_bytes(4, "little")
    parts = [prefix]
    for start in range(0, size, 131072):
        part = data[start:start + 131072]
        parts.extend((((len(part) << 3) | int(start + len(part) == size)).to_bytes(3, "little"), part))
    return b"".join(parts)


def summary_window_frame(data: bytes) -> bytes:
    """Independent raw/RLE frame with FCS=J and an admitted 1 MiB window.

    Unlike a single-segment frame, this can isolate J>S_MAX without a second
    window fault (17.4). Long constant padding also keeps C within its bound.
    """
    parts = [b"\x28\xb5\x2f\xfd\x80\x50" + len(data).to_bytes(4, "little")]
    for start in range(0, len(data), 131072):
        part = data[start:start + 131072]
        last = int(start + len(part) == len(data))
        rle = part == part[:1] * len(part)
        parts.extend((((len(part) << 3) | (2 if rle else 0) | last).to_bytes(3, "little"),
                      part[:1] if rle else part))
    return b"".join(parts)


def summary_frame_refusals(content: bytes) -> list[tuple[str, bytes, str]]:
    frame = summary_raw_frame(content)
    header_bytes = 6 if len(content) < 256 else 7 if len(content) < 65792 else 9
    blocks = frame[header_bytes:]
    return [
        ("raw-json", content, "zstd magic"),
        ("window", b"\x28\xb5\x2f\xfd\x80\x58" + len(content).to_bytes(4, "little") + blocks, "window exceeds"),
        ("dictionary", frame[:4] + bytes((frame[4] | 1,)) + b"\x01" + frame[5:], "dictionary"),
        ("missing-size", b"\x28\xb5\x2f\xfd\x00\x50" + blocks, "content size"),
        ("wrong-size", b"\x28\xb5\x2f\xfd\x80\x50" +
         (len(content) + 1).to_bytes(4, "little") + blocks, "content size"),
        ("extra-frame", frame + frame, "frame bytes"),
        ("trailing", frame + b"x", "frame bytes"),
    ]


@dataclass
class Case:
    name: str
    head: dict[str, Any]
    rows: list[list[Any]]
    summary: dict[str, Any]
    caps: dict[str, int] | None = None
    compress: bool = False
    frame_factory: Callable | None = None
    force_full: bool = False
    report: bytes = b""
    refusal: dict[str, Any] | None = None
    summary_frame_factory: Callable[[bytes], bytes] | None = None


def output_manifest() -> bytes:
    # Whitespace/non-ASCII make a reserialized-manifest hash visibly wrong.
    manifest = {
        "message_format": "pineforge/v1",
        "outputs": [{"index": 0, "id": "old"}, {"index": 1, "id": "plot-雪", "kind": "plot"},
                    {"index": 0, "id": 'alert-é雪😀\x00"\\', "kind": "alert", "freq": "all"},
                    {"index": 2, "id": "line", "kind": "hline", "price": {"constant": True}},
                    {"index": 3, "id": "absent-line", "kind": "hline", "price": {"constant": 99}}],
        "series": [{"slot": 1, "output": "float"}, {"slot": 0, "output": "color", "encoding": "rgba-u32"},
                   {"slot": 1, "output": "float-alias"}],
        "constants": [{"index": 0, "encoding": "rgba-u32"}, {"index": 1}],
    }
    return json.dumps(manifest, ensure_ascii=False, indent=1).encode("utf-8")


def v1_report(case: Case, *, manifest_factory=output_manifest) -> tuple[dict[str, Any], bytes, dict[str, Any] | None]:
    """Existing run_json PURE path, no library/engine invocation.

    Source contracts read: ReportC:2385, _build_sink_report:5497,
    build_outputs_block:3710, _stream_json:5408, failure_line:2202.
    No pf_sink2 or reference-encoder algorithm is used as an oracle.
    """
    import pf_sink
    import run_json as run

    rows = normalized(case.rows, case.head)
    descriptors = {d["type"]: d for d in case.head["records"]}
    records = [pf_sink.Record(row[0], tuple(descriptors[row[0]]["fields"]), tuple(row[1:])) for row in rows]
    sink = types.SimpleNamespace(records=lambda tag=None: (
        row for row in records if tag is None or row.tag == tag),
        counts={tag: sum(row.tag == tag for row in records) for tag in "ETOVKXH"},
        named=False)
    seed = case.summary
    report = run.ReportC()
    report.input_bars_processed = seed["diagnostics"].get(
        "input_bars_processed", sum(row[0] == "E" for row in rows))
    report.script_bars_processed = seed["diagnostics"].get(
        "script_bars_processed", report.input_bars_processed)
    report.metrics.all.profit_factor = float("nan")
    report.metrics.equity.sharpe_monthly = float("nan")
    for row in rows:
        if row[0] == "T" and row[8] is not None:
            report.net_profit = report.net_profit + row[8]
    # ReportC is fixture input to the existing producer. Its count fields
    # must describe these trades, rather than the zero-initialized struct.
    for stats, side in ((report.metrics.all, None), (report.metrics.longs, "long"),
                        (report.metrics.shorts, "short")):
        signs = [(row[8] > 0) - (row[8] < 0) for row in rows
                 if row[0] == "T" and row[8] is not None and (side is None or row[2] == side)]
        stats.num_trades = len(signs)
        stats.num_wins, stats.num_losses, stats.num_even = (signs.count(sign) for sign in (1, -1, 0))
        stats.max_consecutive_wins = max(
            (sum(1 for _ in streak) for sign, streak in groupby(signs) if sign == 1), default=0)
        stats.max_consecutive_losses = max(
            (sum(1 for _ in streak) for sign, streak in groupby(signs) if sign == -1), default=0)
    fixture_path = Path("fixtures") / (seed["input"].get("fixture", case.name) + ".csv")
    first_ts = seed["input"].get("first_ts", 1700000000000)
    last_ts = seed["input"].get("last_ts", 1700000000000 + max(0, report.input_bars_processed - 1) * 60000)
    # Like _ReportSink, fold the T rows once and keep the result or the first refusal;
    # _build_sink_report then reads sink.summary / sink.summary_failure.
    sink.summary = sink.summary_failure = None
    try:
        sink.summary = run._sink_summary(report, sink, records=sink.records("T"))
    except run.RunFailure as error:
        sink.summary_failure = error
        error.__traceback__ = None
    try:
        out = run._build_sink_report(
            report, Path(seed["input"].get("ohlcv", fixture_path)), report.input_bars_processed,
            first_ts, last_ts,
            seed["elapsed_seconds"], seed["applied_inputs"], seed["applied_overrides"],
            seed["applied_runtime"], sink)
        out["input"].update(seed["input"])
        out["diagnostics"].update(seed["diagnostics"])
        for key in ("report_window", "report_shape"):
            if key in seed:
                out[key] = seed[key]
        if "outputs" in seed:
            o_rows = [row[1:] for row in rows if row[0] == "O"]
            constants = next((row[1:] for row in rows if row[0] == "K"), [])
            events = [dict(zip(descriptors["V"]["fields"], row[1:])) for row in rows if row[0] == "V"]
            slots = len(descriptors["O"]["fields"]) - 3
            out["outputs"] = run.build_outputs_block(types.SimpleNamespace(
                manifest=manifest_factory,
                bar_times=lambda: ([run._INT64_MIN if row[1] is None else row[1] for row in o_rows],
                                   [run._INT64_MIN if row[2] is None else row[2] for row in o_rows]),
                series_count=lambda: slots,
                series=lambda slot: [float("nan") if row[3 + slot] is None else row[3 + slot] for row in o_rows],
                constants=lambda: [float("nan") if value is None else value for value in constants],
                events=lambda: events))
        out["fingerprint"] = seed["fingerprint"]
        # Exercise retained additive metadata in its captured position.
        for key, value in seed.items():
            if key not in out and key != "sink_folds":
                out[key] = value
        buffer = io.StringIO()
        run._stream_json(out, buffer)
        buffer.write("\n")
        raw = buffer.getvalue().encode("ascii")
        parsed = json.loads(raw)
        projection = {}
        for key, value in parsed.items():
            if key in ("trades", "equity_curve"):
                continue
            if key == "outputs":
                value = {field: value[field] for field in (
                    "schema_version", "message_format", "manifest_sha256", "manifest", "constants", "hlines")}
            projection[key] = value
        projection["sink_folds"] = sink_folds(rows)
        return projection, raw, None
    except run.RunFailure as error:
        refusal = {"code": error.code, "args": error.code_args}
        return seed, run.failure_line(str(error), error.code, error.code_args).encode("ascii"), refusal


def iter_population_rows(n: int, t: int, fraction: float = 0.5):
    """Reproducible size/vector population, not a claim about compression."""
    for i in range(n):
        active = i % 100 < round(100 * fraction)
        # Exact integer arithmetic and dyadic division, independent of libm.
        noise = ((i * 6364136223846793005 + 1442695040888963407) & ((1 << 53) - 1)) / float(1 << 53)
        other = ((i * 2862933555777941757 + 3037000493) & ((1 << 53) - 1)) / float(1 << 50)
        equity = 10000.0 + i * 0.125 + noise if active else 10000.0
        profit = other - 4.0 if active else 0.0
        yield ["E", 1700000000000 + (i + i // 33) * 60000, equity, profit]
    for i in range(t):
        yield report_trade(i + 1, key=float(i % 19 - 9))


def population_rows(n: int, t: int, fraction: float = 0.5) -> list[list[Any]]:
    return list(iter_population_rows(n, t, fraction))


def cases(*, large: bool = False) -> list[Case]:
    result = []
    ids = {"empty0": "vec-empty-0", "equity4": "vec-equity4", "trades3": "vec-trades3",
           "multiblock": "vec-multi00", "equity4-zstd": "vec-equity4"}
    for name, run_id in ids.items():
        path = BUNDLE / f"{name}.records.jsonl"
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        result.append(Case(name, header(run_id), rows, metadata(name, rows),
                           {"E": 2, "T": 1, "I": 4} if name == "multiblock" else None,
                           frame_factory=decode_frames if name == "equity4-zstd" else None))
    outputs = admitted_equity_rows(all_types_rows())
    result.append(Case("outputs", header("vec-outputs", outputs=True, extras=True), outputs,
                       metadata("outputs", outputs, outputs=True), {"E": 2, "T": 1, "O": 1, "V": 2, "I": 4}))
    empty_outputs = [["K", None, -0.0]]
    result.append(Case("outputs-empty", header("vec-outputs-empty", outputs=True), empty_outputs,
                       metadata("outputs-empty", empty_outputs, outputs=True)))
    valid_queries = [row if row[0] != "T" else [
        0.0 if value is None and 5 <= index <= 12 else value for index, value in enumerate(row)]
                     for row in query_rows()]
    valid_queries = admitted_equity_rows(valid_queries)
    result.append(Case("selected-anchor", header("vec-anchor"), valid_queries,
                       metadata("selected-anchor", valid_queries, selected=True), {"E": 1, "T": 3, "I": 7}))
    result.append(Case("unaligned", header("vec-buckets"), valid_queries, metadata("unaligned", valid_queries),
                       {"E": 7, "T": 4, "I": 11}))
    one = [report_trade(1)]
    result.append(Case("one-trade", header("vec-one-trade"), one, metadata("one-trade", one)))
    null_trade = trade(1)
    null_trade[5:13] = [None] * 8
    result.append(Case("all-null-trade", header("vec-null-trade"), [null_trade],
                       metadata("all-null-trade", [null_trade])))
    result.append(Case("nullable-ranks", header("vec-null-ranks"), query_rows(),
                       metadata("nullable-ranks", query_rows()), {"E": 7, "T": 4, "I": 11}))
    raw = b'\xc3\xa9\xe9\x9b\xaa\xf0\x9f\x98\x80\x00"\\\xe1\x80\xff'
    strings = admitted_equity_rows(all_types_rows())
    strings[2][-1] = raw
    strings[3][-1] = raw
    special = metadata("utf8-controls", strings, outputs=True)
    special["input"]["text"] = 'é雪😀\x00\b\f\n\r\t\x7f"\\\ufffd'
    result.append(Case("utf8-controls", header('vec-"\\\x00\né雪😀', outputs=True, extras=True),
                       strings, special))
    values = (5e-324, -5e-324, -0.0, 1e-7, 1e16, 1e-12, float((1 << 50) - 1))
    floating = [["E", i, value, 0.0] for i, value in enumerate(values)]
    result.append(Case("float-boundaries", header("vec-floats"), floating, metadata("float-boundaries")))
    decimal = [["E", i, value, 0.0] for i, value in enumerate(
        (1e-12, -1e-12, float((1 << 50) - 1) / 1e12, -float((1 << 50) - 1) / 1e12))]
    result.append(Case("decimal-endpoints", header("vec-decimals"), decimal, metadata("decimal-endpoints")))
    delta = [["E", value, 1.0, 0.0] for value in (
        -(1 << 52) + 1, -(1 << 52) + 2, -(1 << 52) + 3, -(1 << 52) + 4,
        (1 << 52) - 4, (1 << 52) - 3, (1 << 52) - 2, (1 << 52) - 1)]
    result.append(Case("delta-boundaries", header("vec-deltas"), delta, metadata("delta-boundaries"), {"E": 4}))
    safe = (1 << 53) - 1
    integers = [["E", value, 1.0, 0.0] for value in (-safe, -(1 << 52), -(1 << 52) + 1,
                                                   (1 << 52) - 1, 1 << 52, safe)]
    row = report_trade(1)
    row[16] = safe
    integers.append(row)
    result.append(Case("integer-boundaries", header("vec-integers"), integers, metadata("integer-boundaries", integers)))
    forced = [["E", 0, None, 0.0], ["E", 1, 4.0, None], ["E", 1, 3.0, 0.0],
              ["E", 2, 10.0, 0.0], ["E", 3, None, None], report_trade(1, key=20.0)]
    forced[-1][15] = True
    result.append(Case("forced-close", header("vec-forced-close"), forced, metadata("forced-close", forced),
                       {"E": 2, "I": 3}))
    folds = [report_trade(i + 1, key=value) for i, value in enumerate((1e16, 1.0, -1e16, -0.0))]
    for row in folds:
        row[15] = True
    result.append(Case("fold-order", header("vec-fold-order"), folds, metadata("fold-order", folds)))
    null_e = [["E", i, None, None if i % 2 else 1.0] for i in range(4)]
    result.append(Case("all-null-equity", header("vec-null-equity"), null_e, metadata("all-null-equity")))
    figure = population_rows(2401, 0)
    figure[1][1] = figure[0][1]
    for i in range(17, 2400, 17):
        figure[i][2] = None
    for i in range(0, 2400, 19):
        figure[i][3] = None
    figure[-1][2:] = [100000.0, 0.0]
    final_trade = report_trade(1, key=200000.0)
    final_trade[15] = True
    figure.append(final_trade)
    figure_meta = metadata("default-figure", figure)
    result.append(Case("default-figure", header("vec-default-figure"), figure, figure_meta))
    result.append(Case("default-figure-full", header("vec-default-figure"), figure, figure_meta, force_full=True))
    additive = metadata("additive-metadata")
    additive["future_metadata"] = {"captured_order": [-0.0, "é雪😀", {"extra": True}]}
    result.append(Case("additive-metadata", header("vec-additive"), [], additive))
    # Same captured run/head/metadata in both profiles, including the report bytes.
    equity = next(case for case in result if case.name == "equity4")
    result.append(Case("equity4-full", equity.head, equity.rows, equity.summary, force_full=True))
    result.append(Case("one-trade-full", header("vec-one-trade"), one, metadata("one-trade", one), force_full=True))
    if large:
        base_length = len(v1_line(["X", 0, 0, "", 1.0])) - 1
        x2 = [["X", 0, 0, "x" * (1048575 - base_length), 1.0]]
        result.append(Case("x2-1048575", header("vec-x2", extras=True), x2, metadata("x2-1048575")))
        rows = [["E", 1700000000000 + i * 60000, 10000.0 + i * 0.25, float(i % 7)] for i in range(4096)]
        result.append(Case("zstd-real", header("vec-zstd"), rows, metadata("zstd-real"), compress=True))
        rows = []
        for i in range(10000):
            rows.append(["E", 1700000000000 + i * 60000, 10000.0 + i * 0.25, float(i % 7)])
            if i % 4 == 3:
                rows.append(report_trade((i + 1) // 4, key=float(i % 19)))
        result.append(Case("default-caps", header("vec-default"), rows, metadata("default-caps", rows), compress=True))
        for name, n, t, caps in (
                ("compact-profile", 1000, 50, None), ("full-profile", 100000, 5000, None),
                ("boundary-e10979", 10979, 1, None), ("boundary-e10980", 10980, 1, None),
                ("boundary-t3313", 1200, 3313, None), ("boundary-t3314", 1200, 3314, None),
                ("boundary-d128", 127, 0, {"E": 1}), ("boundary-d129", 128, 0, {"E": 1}),
                ("boundary-raw-null", 10980, 0, None),
                ("finite-prefix-pages", 241, 0, {"E": 1})):
            rows = population_rows(n, t)
            if name == "boundary-raw-null":
                for row in rows:
                    row[2] = None
            if name == "finite-prefix-pages":
                for i in range(0, len(rows), 3):
                    rows[i][2] = None
            result.append(Case(name, header("vec-" + name), rows, metadata(name, rows), caps, compress=True))
        for name, full, raw in (("summary-max", False, False), ("summary-max-full", True, False),
                                ("summary-incompressible", False, True), ("summary-incompressible-full", True, True)):
            # Same captured population/identity in both profiles. The high
            # entropy raw-block pair tests expansion; its real library probe is
            # recorded separately and never fabricated as a nonshrinking PASS.
            seed_name = "summary-incompressible" if raw else "summary-max"
            result.append(Case(name, header("vec-" + seed_name), [], metadata(seed_name), force_full=full,
                               summary_frame_factory=summary_raw_frame if raw else None))
    for case in result:
        case.summary, case.report, case.refusal = v1_report(case)
        if case.name.startswith(("summary-max", "summary-incompressible")):
            case.summary["input"]["summary_payload"] = ""
            base = len(json.dumps(case.summary, separators=(",", ":")).encode("ascii"))
            length = 1048576 - base
            if case.name.startswith("summary-incompressible"):
                payload = base64.b85encode(hashlib.shake_256(
                    b"sink-v2-summary-entropy-round-6").digest(length)).decode("ascii")[:length]
            else:
                payload = "x" * length
            case.summary["input"]["summary_payload"] = payload
            case.summary, case.report, case.refusal = v1_report(case)
            if len(json.dumps(case.summary, separators=(",", ":")).encode("ascii")) != 1048576:
                raise ValueError("maximum SUMMARY fixture must be exactly 1048576 bytes")
    return result
