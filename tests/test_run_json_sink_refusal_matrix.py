"""CR-397 option (c): the report-sink refusal matrix, run through the real host entry.

This is a CHARACTERIZATION. Each row corrupts the sink a fake engine hands to
run_json.main (tests/test_run_json_sink.py's stand-in library, extended by
tests/test_run_json_sink_summary.py's SummaryLibrary) and pins the COMPLETE
refusal of the strict Python route: exception
type, code, message, args, exit status, receipt-query order, cleanup, and that
no report reached stdout. A native bulk verifier must reproduce every row
unchanged, including which refusal wins when one sink holds several defects.

Every expected value was derived by reading docker/pf_sink.py and the
_ReportSink / _SinkReportDiagnostics / _tagged_records code of engine 30e9f9a0.
None was copied from a run: this file was written on a machine that cannot run
it. A row that disagrees with a lab run on main is a wrong expectation in this
table (a finding), never a license to weaken the row. The line numbers of the
rules each row exercises are in notes/RM1.md, not here.

The `route` fixture runs this same table through "strict" and "native".
Native uses the real candidate-archive-linked codec from native_library;
the engine/receipt fault fixtures remain stand-ins. Missing module/symbol
fails native rows. Counted calls prove prepare and the T/E guards actually
ran wherever production can reach them. Header/open refusals and named
compatibility controls retain their production paths.

The docker discovery shim overrides only the route fixture to keep source
preflight strict-only, without loading a built module. The built CTest row
supplies PINEFORGE_CR397_GENERATED_SO and runs both routes. The two damage
hooks wrap the same host checkpoints on both routes; no row changes.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import sys
from typing import Any, Callable, Optional

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import test_run_json_sink as full
import test_run_json_sink_summary as summary_tests
from test_run_json_native_sink_json import native_library  # noqa: F401 (shared pytest fixture)

T0 = full.bars.T0
MAX_LINE = 1024 * 1024
I53_MAX = (1 << 53) - 1
INVALID = "the engine's report sink is invalid: "
CHANGED = "the report sink changed after verification"

HEADER_KEYS = "header keys are missing, extra or out of order"
BAD_TAG_ORDER = "unknown, repeated or out-of-order record type"
BAD_VERSION = "expected integer version 1"
BAD_STRING = "expected a Unicode string without unpaired surrogates"
WHITESPACE = "whitespace or control byte outside a JSON string"
NOT_ARRAY = "record must be a nonempty array with a string tag"
ABSENT_TAG = "record tag is absent from the header"
ARITY = "record field count differs from its descriptor"
BAD_I64 = "invalid i64 column"
BAD_U64 = "invalid u64 column"
BAD_F64 = "f64 column is not the Python repr of its double"
BAD_BOOL = "bool column requires true or false"
BAD_HEX = "hex64 requires 16 lowercase hex digits"
NULL_COLUMN = "null in a non-optional column"
BAD_SIDE = "trade side must be long or short"
BAD_INTEGER = "integer exceeds the exact JSON integer bound"
TRAILER_KEYS = "trailer keys are missing, extra or out of order"
COUNTS_KEYS = "trailer counts keys are missing, extra or out of order"
RUN_ID_DIFFERS = "the report sink run id differs from the request"
RECEIPT_INCOMPLETE = "the engine's report sink receipt is incomplete"
RECEIPT_DIFFERS = "the report sink trailer differs from its engine receipt"
TRADE_ORDER = "the report sink trade rows are out of report order"


# --- the sink as editable lines ------------------------------------------------

class Rendered:
    """A sealed sink: its bytes, its true counts and digest, and where each line sits."""

    def __init__(self, blob, counts, digest, lines, sink):
        self.blob, self.counts, self.digest = blob, counts, digest
        self.lines, self.sink = lines, sink

    def line(self, number):
        return self.lines[number - 1]

    def start(self, number):
        return sum(len(line) for line in self.lines[:number - 1])

    def end(self, number):
        return self.start(number) + len(self.lines[number - 1])


class Sink:
    """The fake engine's sink as editable lines. Line numbers are physical: the header is 1."""

    def __init__(self, header, rows):
        self.header = header
        self.order = [entry["type"] for entry in header["records"]]
        self.records = [[row[0], full.encoded(row)] for row in rows]
        self.blob = None            # replaces the whole file
        self.head_bytes = None      # replaces the encoded header line
        self.trailer_edit = None    # edits the correct trailer; may return a new dict
        self.trailer_bytes = None   # replaces the trailer line; b"" omits it
        self.tail = b""             # bytes after the trailer
        self.unterminated = False   # drops the LF that ends the file

    def copy(self):
        clone = Sink.__new__(Sink)
        clone.__dict__.update(self.__dict__)
        clone.header = json.loads(json.dumps(self.header))
        clone.records = [list(record) for record in self.records]
        clone.order = list(self.order)
        return clone

    def line(self, number):
        return self.records[number - 2][1]

    def set(self, number, data, tag=None):
        if tag is not None:
            self.records[number - 2][0] = tag
        self.records[number - 2][1] = data

    def delete(self, number):
        del self.records[number - 2]

    def insert(self, number, tag, data):
        self.records.insert(number - 2, [tag, data])

    def render(self):
        """Seal the lines as they stand: counts and digest are recomputed, then the trailer
        (and anything else the case damages) is written. The returned counts and digest are
        the truth about the body, which is what an honest engine receipt would carry."""
        head = self.head_bytes if self.head_bytes is not None else full.encoded(self.header)
        body = [head] + [data for _, data in self.records]
        digest = hashlib.sha256(b"".join(body)).hexdigest()
        counts = {tag: sum(1 for kind, _ in self.records if kind == tag) for tag in self.order}
        trailer = {"trailer": "pf-report-sink", "version": 1, "counts": dict(counts), "sha256": digest}
        if self.trailer_edit is not None:
            edited = self.trailer_edit(trailer)
            trailer = trailer if edited is None else edited
        closing = full.encoded(trailer) if self.trailer_bytes is None else self.trailer_bytes
        lines = body + [closing]
        blob = b"".join(lines)
        if self.unterminated:
            blob = blob[:-1]
        if self.blob is not None:
            blob = self.blob
        return Rendered(blob + self.tail, counts, digest, lines, self)


# --- edits: each returns a function that corrupts a Sink -------------------------

def sub(data, old, new):
    assert old in data, (old, data)
    return data.replace(old, new, 1)


def put(number, data):
    def edit(sink):
        sink.set(number, data)
    return edit


def edit_line(number, change):
    def edit(sink):
        sink.set(number, change(sink.line(number)))
    return edit


def swap(number, old, new):
    return edit_line(number, lambda data: sub(data, old, new))


def both(*edits):
    def edit(sink):
        for each in edits:
            each(sink)
    return edit


def swap_lines(first, second):
    """Exchange two whole records, tag included, so the counts stay truthful."""
    def edit(sink):
        sink.records[first - 2], sink.records[second - 2] = (
            sink.records[second - 2], sink.records[first - 2])
    return edit


def delete_line(number):
    def edit(sink):
        sink.delete(number)
    return edit


def duplicate_line(number):
    def edit(sink):
        sink.insert(number + 1, sink.records[number - 2][0], sink.line(number))
    return edit


def on_header(change):
    def edit(sink):
        change(sink.header)
    return edit


def descriptor_edit(index, change):
    return on_header(lambda header: change(header["records"][index]))


def raw_header(old, new):
    def edit(sink):
        sink.head_bytes = sub(full.encoded(sink.header), old, new)
    return edit


def head(data):
    def edit(sink):
        sink.head_bytes = data
    return edit


def head_map(change):
    def edit(sink):
        sink.head_bytes = change(full.encoded(sink.header))
    return edit


def whole(make):
    def edit(sink):
        sink.blob = make(sink)
    return edit


def on_trailer(change):
    def edit(sink):
        sink.trailer_edit = change
    return edit


def after_trailer(data):
    def edit(sink):
        sink.tail = data
    return edit


def raw_trailer(data):
    def edit(sink):
        sink.trailer_bytes = data
    return edit


def no_trailer(sink):
    sink.trailer_bytes = b""


def unterminated(sink):
    sink.unterminated = True


def long_trade(length):
    """Pad T1's exit_comment so that its whole line, LF included, is `length` bytes."""
    def edit(sink):
        base = sink.line(7)
        assert base.endswith(b',"",""]\n')
        padded = base[:-4] + b'"' + b"x" * (length - len(base)) + b'"]\n'
        assert len(padded) == length
        sink.set(7, padded)
    return edit


def reorder(mapping, *keys):
    values = {key: mapping[key] for key in keys}
    mapping.clear()
    mapping.update(values)


def wrong_digest(trailer):
    trailer["sha256"] = "0" * 64


def high_count_and_wrong_digest(trailer):
    trailer["counts"]["E"] = 6
    trailer["sha256"] = "0" * 64


# --- expectations -------------------------------------------------------------

def strict(number, message):
    """A refusal of the strict reader: the line number is the physical line it stopped at."""
    return f"{INVALID}line {number}: {message}"


def guard(message):
    """A refusal of the raw post-verification scan: it names no line."""
    return INVALID + message


def not_found(library):
    return f"[Errno 2] No such file or directory: {library.installed[0]!r}"


def json_error(message, line, token=None, delta=0):
    """The decoder's text for `line`: at the first `token` (+ delta), or at the end when None."""
    body = line[:-1].decode("utf-8")
    position = len(body) if token is None else body.index(token) + delta
    return f"invalid UTF-8 or JSON: {message}: line 1 column {position + 1} (char {position})"


def utf8_error(line, bad):
    position = line[:-1].index(bad)
    return (f"invalid UTF-8 or JSON: 'utf-8' codec can't decode byte 0x{bad[0]:02x} "
            f"in position {position}: invalid start byte")


@dataclass(frozen=True)
class Refusal:
    text: Any                       # a str, or a function of the library for texts that name a path
    code: str = "engine_invariant"
    args: dict = field(default_factory=dict)
    queries: int = 0                # how often the engine receipt was asked for before the refusal


@dataclass(frozen=True)
class Valid:
    check: Optional[Callable] = None


@dataclass(frozen=True)
class Case:
    name: str
    edit: Optional[Callable]
    want: Any
    shape: str = "narrow"           # narrow E,T | outputs E,T,O,V,K | wide E,T,O,V,K,X,H
    fault: Optional[str] = None     # SinkLibrary/SummaryLibrary receipt faults
    receipt: Optional[str] = None   # receipt faults added here
    pnls: tuple = (10.0, -7.0, -0.0)
    cli: tuple = ()
    named: bool = False
    removes_sink: bool = False      # the sink file is gone when strategy_free runs
    delete_at_run: bool = False
    after_verify: Optional[Callable] = None
    on_raw_open: Optional[tuple] = None


def refusal(name, edit, text, *, code="engine_invariant", args=None, queries=0, **options):
    return Case(name, edit, Refusal(text, code, dict(args or {}), queries), **options)


def valid(name, edit=None, check=None, **options):
    return Case(name, edit, Valid(check), **options)


# --- the library under test: engine/receipt fault stand-ins ---------------------

def build_sink(case, run_id, outputs, rows):
    header = full.header(outputs)
    header["run_id"] = run_id
    rows = list(rows)
    if case.shape == "wide":
        header["records"] += [
            full.descriptor("X", ["timestamp", "bar_index", "name", "value"],
                            ["i64", "i64", "str", "f64?"], ["unix_ms", "", "", ""]),
            full.descriptor("H", ["hash"], ["hex64"], [""]),
        ]
        rows += [["X", T0, 0, 'trace"\n漢', -0.0],
                 ["H", "0123456789abcdef"], ["H", "fedcba9876543210"]]
    sink = Sink(header, rows)
    if case.edit is not None:
        case.edit(sink)
    return sink


class MatrixLibrary(summary_tests.SummaryLibrary):
    """SummaryLibrary with the sink bytes under the case's control."""

    def __init__(self, case):
        super().__init__(outputs=case.shape != "narrow", fault=case.fault, pnls=case.pnls)
        self.case = case
        self.rendered = None
        self.native_json_calls = []

    def write_sink(self, path, rows, *, outputs=False, fault=None):
        self.rendered = build_sink(self.case, self.run_id, outputs, rows).render()
        Path(path).write_bytes(self.rendered.blob)
        self.path_expected_at_free = True
        return self.rendered.counts, self.rendered.digest

    def run(self, *args):
        super().run(*args)
        if self.case.delete_at_run:
            os.unlink(self.path)

    def receipt(self, state, pointer):
        status = super().receipt(state, pointer)
        out = pointer._obj
        mode = self.case.receipt
        if mode == "status":
            return -1
        if mode == "extra-tag":
            out.count_o = 1
        elif mode == "incomplete-and-count":
            out.complete = 0
            out.count_e += 1
        elif mode == "unlink":
            os.unlink(self.path)
        return status

    def free(self, state):
        if self.case.removes_sink:
            if self.path is not None:
                self.freed.append(self.path)
                self.path = None
            return
        super().free(state)


# --- route selection and the two shared host checkpoints -------------------------

ROUTES = ("strict", "native")


def select_route(name, monkeypatch, request):
    """Use the production opt-in selector and the real native entry point."""
    if name == "strict":
        monkeypatch.setenv("PF_REPORT_SINK_JSON_SPIKE", "0")
        return name
    if name == "native":
        monkeypatch.setenv("PF_REPORT_SINK_JSON_SPIKE", "1")
        _, _, function = request.getfixturevalue("native_library")

        def sink_json(self, request_pointer, result_pointer):
            status = function(request_pointer, result_pointer)
            native_request, result = request_pointer._obj, result_pointer._obj
            # Same observable as test_run_json_native_sink_json, plus the
            # descriptor return code. Delegate without altering either struct.
            self.native_json_calls.append((
                native_request.operation, result.status, result.complete,
                native_request.guard_tag, status))
            return status

        monkeypatch.setattr(MatrixLibrary, "pf_report_sink_json_v1", sink_json, raising=False)
        return name
    raise AssertionError(f"no such sink verification route: {name!r}")


def install_route_hooks(case, library, monkeypatch):
    """Cases that damage the sink BETWEEN verification and the later fresh read need a point
    in time. Both hooks wrap the shared host functions and still run them:
    after_verify runs once _ReportSink.verify has returned; on_raw_open runs as the Nth
    raw (tag-selecting) scan opens the file, i.e. between the trade scan and the equity scan."""
    if case.after_verify is not None:
        real_verify = run_json._ReportSink.verify

        def verify(self, lib, state, report):
            real_verify(self, lib, state, report)
            case.after_verify(self.path, library.rendered)

        monkeypatch.setattr(run_json._ReportSink, "verify", verify)
    if case.on_raw_open is not None:
        nth, change = case.on_raw_open
        real_open, opened = run_json._open_uncached_sink, []

        def open_sink(path, *, raw=False):
            if raw:
                opened.append(path)
                if len(opened) == nth:
                    change(path, library.rendered)
            return real_open(path, raw=raw)

        monkeypatch.setattr(run_json, "_open_uncached_sink", open_sink)


@pytest.fixture(params=ROUTES, ids=lambda name: "route-" + name)
def route(request, monkeypatch):
    return select_route(request.param, monkeypatch, request)


# --- running a case and checking its outcome -------------------------------------

class Outcome:
    def __init__(self, status, stdout, stderr, library, raised, published, spill, named):
        self.status, self.stdout, self.stderr = status, stdout, stderr
        self.library, self.raised, self.published = library, raised, published
        self.spill, self.named = spill, named

    def describe(self):
        return (f"status={self.status!r} stdout={self.stdout[:600]!r} stderr={self.stderr[:600]!r} "
                f"raised={[repr(error) for error in self.raised]} published={len(self.published)}")


def run_host(case, tmp_path, monkeypatch):
    spill = tmp_path / "spill"
    spill.mkdir()
    named = tmp_path / "named.sink"
    library = MatrixLibrary(case)
    raised, published = [], []
    real_main, real_write = run_json._main, run_json._write_report

    def recording_main(argv, binary_inputs):
        try:
            return real_main(argv, binary_inputs)
        except BaseException as error:
            raised.append(error)
            raise

    def recording_write(artifact, phases=None):
        published.append(1)
        return real_write(artifact, phases)

    monkeypatch.setattr(run_json, "_main", recording_main)
    monkeypatch.setattr(run_json, "_write_report", recording_write)
    install_route_hooks(case, library, monkeypatch)
    extra = ["--spill-dir", str(spill), *case.cli]
    if case.named:
        extra += ["--report-sink", str(named)]
    status, stdout, stderr, _ = full.invoke(tmp_path, monkeypatch, library=library, extra=extra)
    return Outcome(status, stdout, stderr, library, raised, published, spill, named)


def assert_clean(case, out):
    library = out.library
    assert len(library.installed) == 1, out.describe()
    assert library.freed == library.installed, out.describe()
    assert list(out.spill.iterdir()) == [], out.describe()
    if case.named:
        assert out.named.exists(), "a refusal or success must leave the caller's sink alone"
        assert out.named.read_bytes() == library.rendered.blob or case.removes_sink
    else:
        assert not Path(library.installed[0]).exists(), "the temporary sink must be unlinked"


def assert_refusal(case, out):
    want = case.want
    text = want.text(out.library) if callable(want.text) else want.text
    assert out.stdout, out.describe()
    assert out.status == 1, out.describe()
    assert out.stderr == "", out.describe()
    assert out.stdout.endswith("\n") and out.stdout.count("\n") == 1, out.describe()
    document = json.loads(out.stdout)
    assert list(document) == ["engine", "error", "code", "args"], out.describe()
    assert document == {"engine": "pineforge", "error": text, "code": want.code,
                        "args": want.args}, out.describe()
    assert len(out.raised) == 1, out.describe()
    error = out.raised[0]
    assert type(error) is run_json.RunFailure, out.describe()
    assert (str(error), error.code, error.code_args, error.exit_status) == (
        text, want.code, want.args, 1), out.describe()
    assert out.published == [], "a refused run must publish no report"
    assert out.library.queried == want.queries, out.describe()
    assert_clean(case, out)


def assert_valid(case, out):
    assert out.status == 0 and out.stderr == "", out.describe()
    assert out.raised == [], out.describe()
    assert len(out.published) == 1, out.describe()
    assert out.stdout.endswith("\n") and out.stdout.count("\n") == 1
    document = json.loads(out.stdout)
    assert "code" not in document and "error" not in document, out.describe()
    assert document["summary"]["total_trades"] == 3
    assert len(document["trades"]) == 3 and len(document["equity_curve"]) == 5
    assert out.library.queried == 1
    assert_clean(case, out)
    if case.want.check is not None:
        case.want.check(document, out.stdout)


def assert_route(name, case, out):
    """A route name alone cannot prove that native prepare/guards ran."""
    calls = out.library.native_json_calls
    detail = f"{case.name}: native calls={calls!r}; {out.describe()}"
    if name == "strict":
        assert calls == [], detail
        return
    assert name == "native", name
    if case.named or case in HEADER_ROWS or case.delete_at_run:
        # Named paths are strict compatibility controls. Prepare opens a
        # SinkReader (including its header validation) before entering C.
        assert calls == [], detail
        return
    assert calls, detail
    assert calls[0][0] == 1 and calls[0][3] == 0, detail
    assert all(call[0] == 2 and call[3] in (ord("T"), ord("E"))
               for call in calls[1:]), detail
    assert all(call[4] == 0 for call in calls), detail
    # Every shape in this table fits the complete codec. Strict fallback must
    # not turn a missing native refusal into a passing compatibility row.
    assert all(call[1] != 1 for call in calls), detail
    prepare = (1, 0, 1, 0, 0)
    if isinstance(case.want, Valid):
        assert calls == [prepare, (2, 0, 1, ord("T"), 0),
                         (2, 0, 1, ord("E"), 0)], detail
    elif case in GUARD_ROWS or case.name.startswith("priority_guard_"):
        assert calls[0] == prepare, detail
        if case.name == "guard_sink_removed":
            tags = []  # Fresh open fails before entering the native guard.
        elif case.on_raw_open is not None:
            tags = [ord("T"), ord("E")]
        else:
            tags = [ord("T")]
        assert [call[3] for call in calls[1:]] == tags, detail


# --- hooks for damage between verification and the later fresh read ---------------

def poke(path, offset, data):
    with open(path, "r+b") as file:
        file.seek(offset)
        file.write(data)


def replace_with(path, data):
    scratch = path + ".replacement"
    with open(scratch, "wb") as file:
        file.write(data)
    os.replace(scratch, path)


def poke_line(number, old, new):
    """Same-size, same-inode edit of one fragment of a line."""
    assert len(old) == len(new)

    def hook(path, rendered):
        poke(path, rendered.start(number) + rendered.line(number).index(old), new)
    return hook


def replace_same_bytes(path, rendered):
    replace_with(path, rendered.blob)


def append_byte(path, rendered):
    with open(path, "ab") as file:
        file.write(b"\n")


def shrink_by_one(path, rendered):
    os.truncate(path, len(rendered.blob) - 1)


def replace_with_garbage(path, rendered):
    replace_with(path, b"not a sink\n")


def remove_sink(path, rendered):
    os.unlink(path)


def clobber_final_lf(path, rendered):
    poke(path, len(rendered.blob) - 1, b" ")


def clobber_last_line_feed_of_trade(path, rendered):
    poke(path, rendered.end(7) - 1, b" ")


def rewrite_same_bytes_in_place(path, rendered):
    with open(path, "r+b") as file:
        file.write(rendered.blob)


def flip_trailer_digit(path, rendered):
    marker = b'"sha256":"'
    flipped = b"0" if rendered.digest[0] != "0" else b"1"
    poke(path, rendered.start(10) + rendered.line(10).index(marker) + len(marker), flipped)


def rewrite_as_another_valid_sink(path, rendered):
    variant = rendered.sink.copy()
    variant.set(13, b'["K",51.0]\n')
    Path(path).write_bytes(variant.render().blob)


def rewrite_with_wrong_digest(path, rendered):
    variant = rendered.sink.copy()
    variant.trailer_edit = wrong_digest
    Path(path).write_bytes(variant.render().blob)


# --- the rows ----------------------------------------------------------------

HEADER_ROWS = [
    refusal("header_empty_file", whole(lambda sink: b""), strict(1, "missing header or trailer")),
    refusal("header_without_lf", whole(lambda sink: full.encoded(sink.header)[:-1]),
            strict(1, "line does not end with LF")),
    refusal("header_blank_line", head(b"\n"), strict(1, "blank line")),
    refusal("header_utf8_bom", head_map(lambda data: b"\xef\xbb\xbf" + data),
            strict(1, "UTF-8 BOM is forbidden")),
    refusal("header_whitespace_outside_string",
            raw_header(b'"schema":"pf-report-sink"', b'"schema": "pf-report-sink"'),
            strict(1, WHITESPACE)),
    refusal("header_truncated_json", head(b'{"schema"\n'),
            strict(1, "invalid UTF-8 or JSON: Expecting ':' delimiter: line 1 column 10 (char 9)")),
    refusal("header_not_an_object", head(b"[]\n"), strict(1, HEADER_KEYS)),
    refusal("header_keys_out_of_order",
            on_header(lambda h: reorder(h, "schema", "version", "engine", "run_id", "records")),
            strict(1, HEADER_KEYS)),
    refusal("header_extra_key", on_header(lambda h: h.update(extra=1)), strict(1, HEADER_KEYS)),
    refusal("header_missing_key", on_header(lambda h: h.pop("engine")), strict(1, HEADER_KEYS)),
    refusal("header_duplicate_key", raw_header(b'{"schema":', b'{"version":1,"schema":'),
            strict(1, "repeated JSON object key")),
    refusal("header_unknown_schema", on_header(lambda h: h.update(schema="other-sink")),
            strict(1, "unknown report sink schema")),
    refusal("header_version_two", on_header(lambda h: h.update(version=2)), strict(1, BAD_VERSION)),
    refusal("header_version_float", raw_header(b'"version":1,', b'"version":1.0,'),
            strict(1, BAD_VERSION)),
    refusal("header_version_bool", raw_header(b'"version":1,', b'"version":true,'),
            strict(1, BAD_VERSION)),
    refusal("header_run_id_not_a_string", on_header(lambda h: h.update(run_id=5)),
            strict(1, BAD_STRING)),
    refusal("header_engine_lone_surrogate", raw_header(b'"engine":"test"', b'"engine":"\\ud800"'),
            strict(1, BAD_STRING)),
    refusal("header_records_not_an_array", on_header(lambda h: h.update(records={})),
            strict(1, "header records must be an array")),
    refusal("descriptor_missing_units", descriptor_edit(0, lambda d: d.pop("units")),
            strict(1, "descriptor keys are missing, extra or out of order")),
    refusal("descriptor_type_not_a_string", descriptor_edit(0, lambda d: d.update({"type": 5})),
            strict(1, BAD_STRING)),
    refusal("descriptor_unknown_tag", descriptor_edit(0, lambda d: d.update({"type": "Z"})),
            strict(1, BAD_TAG_ORDER)),
    refusal("descriptor_multi_character_tag", descriptor_edit(0, lambda d: d.update({"type": "ET"})),
            strict(1, BAD_TAG_ORDER)),
    refusal("descriptor_repeated_tag",
            on_header(lambda h: h["records"].insert(1, dict(h["records"][0]))), strict(1, BAD_TAG_ORDER)),
    refusal("descriptor_out_of_order", on_header(lambda h: h["records"].reverse()),
            strict(1, BAD_TAG_ORDER)),
    refusal("descriptor_arrays_not_lists", descriptor_edit(0, lambda d: d.update(fields="time_ms")),
            strict(1, "descriptor fields, types and units must be arrays")),
    refusal("descriptor_fixed_fields",
            descriptor_edit(0, lambda d: d.update(fields=["time", "equity", "open_profit"])),
            strict(1, "record E differs from the fixed v1 fields")),
    refusal("descriptor_fixed_types",
            descriptor_edit(0, lambda d: d.update(types=["i64", "f64", "f64?"])),
            strict(1, "record E differs from the fixed v1 types")),
    refusal("descriptor_fixed_units", descriptor_edit(1, lambda d: d["units"].__setitem__(4, "base_ccy")),
            strict(1, "record T differs from the fixed v1 units")),
    refusal("descriptor_output_missing_fixed_fields",
            descriptor_edit(2, lambda d: d.update(fields=["row", "open_ms"], types=["i64", "i64?"],
                                                  units=["", "unix_ms"])),
            strict(1, "output descriptor is missing its fixed fields"), shape="outputs"),
    refusal("descriptor_output_column_names", descriptor_edit(2, lambda d: d["fields"].__setitem__(4, "w1")),
            strict(1, "record O differs from the fixed v1 fields"), shape="outputs"),
    refusal("descriptor_constants_types", descriptor_edit(4, lambda d: d.update(types=["f64"])),
            strict(1, "record K differs from the fixed v1 types"), shape="outputs"),
    refusal("header_requires_e_and_t", on_header(lambda h: h["records"].pop()),
            strict(1, "E and T descriptors are required")),
    refusal("header_output_tags_come_together", on_header(lambda h: h["records"].pop()),
            strict(1, "O, V and K descriptors must be selected together"), shape="outputs"),
]

FRAMING_ROWS = [
    refusal("line_blank", put(3, b"\n"), strict(3, "blank line")),
    refusal("line_utf8_bom", edit_line(3, lambda data: b"\xef\xbb\xbf" + data),
            strict(3, "UTF-8 BOM is forbidden")),
    refusal("line_space_between_tokens", swap(3, b"1735776060000,10001.0", b"1735776060000, 10001.0"),
            strict(3, WHITESPACE)),
    refusal("line_crlf_terminator", edit_line(3, lambda data: data[:-1] + b"\r\n"), strict(3, WHITESPACE)),
    refusal("line_control_byte_inside_string", swap(7, b',"",""]\n', b',"","\t"]\n'),
            lambda lib: strict(7, json_error("Invalid control character at", lib.rendered.line(7), "\t"))),
    refusal("line_invalid_utf8", swap(7, b',"",""]\n', b',"","\xff"]\n'),
            lambda lib: strict(7, utf8_error(lib.rendered.line(7), b"\xff"))),
    refusal("line_last_record_without_lf", both(no_trailer, unterminated),
            strict(9, "line does not end with LF")),
    refusal("line_longer_than_cap_with_lf", long_trade(MAX_LINE + 1),
            strict(7, "line exceeds 1 MiB including LF")),
    refusal("line_longer_than_cap_without_lf", long_trade(MAX_LINE + 10),
            strict(7, "line does not end with LF")),
]

TOKEN_ROWS = [
    refusal("json_truncated_array", swap(3, b",-0.0]\n", b"\n"),
            lambda lib: strict(3, json_error("Expecting ',' delimiter", lib.rendered.line(3)))),
    refusal("json_extra_data", swap(3, b"]\n", b"]x\n"),
            lambda lib: strict(3, json_error("Extra data", lib.rendered.line(3), "x"))),
    refusal("json_missing_value", swap(3, b"1735776060000", b""),
            lambda lib: strict(3, json_error("Expecting value", lib.rendered.line(3), ",10001.0"))),
    refusal("json_unterminated_string", swap(3, b",-0.0]\n", b',"-0.0]\n'),
            lambda lib: strict(3, json_error("Unterminated string starting at",
                                             lib.rendered.line(3), '"-0.0'))),
    refusal("json_leading_zero_integer", swap(3, b"1735776060000", b"01735776060000"),
            lambda lib: strict(3, json_error("Expecting ',' delimiter", lib.rendered.line(3),
                                             "01735776060000", 1))),
    refusal("json_lowercase_nan", swap(3, b"10001.0", b"nan"),
            lambda lib: strict(3, json_error("Expecting value", lib.rendered.line(3), "nan"))),
    refusal("token_nan_constant", swap(3, b"10001.0", b"NaN"),
            strict(3, "non-JSON numeric constant NaN")),
    refusal("token_infinity_constant", swap(3, b"10001.0", b"Infinity"),
            strict(3, "non-JSON numeric constant Infinity")),
    refusal("token_negative_infinity_constant", swap(3, b"10001.0", b"-Infinity"),
            strict(3, "non-JSON numeric constant -Infinity")),
    refusal("token_float_overflow", swap(3, b"10001.0", b"1e999"), strict(3, "non-finite JSON number")),
    refusal("token_integer_past_2_53_minus_1", swap(3, b"1735776060000", b"9007199254740992"),
            strict(3, BAD_INTEGER)),
    refusal("token_negative_integer_past_bound", swap(3, b"1735776060000", b"-9007199254740992"),
            strict(3, BAD_INTEGER)),
    refusal("token_integer_of_seventeen_digits", swap(3, b"1735776060000", b"12345678901234567"),
            strict(3, BAD_INTEGER)),
    refusal("token_integer_in_f64_column", swap(3, b"10001.0", b"10001"), strict(3, BAD_F64)),
    refusal("token_negative_zero_integer_in_f64_column", swap(3, b",-0.0]", b",-0]"), strict(3, BAD_F64)),
    refusal("token_float_with_trailing_zero", swap(3, b"10001.0", b"10001.00"), strict(3, BAD_F64)),
    refusal("token_float_in_exponent_spelling", swap(3, b"10001.0", b"1.0001e4"), strict(3, BAD_F64)),
    refusal("string_lone_surrogate", swap(7, b',"",""]\n', b',"","\\ud800"]\n'), strict(7, BAD_STRING)),
    refusal("record_not_an_array", put(3, b"5\n"), strict(3, NOT_ARRAY)),
    refusal("record_empty_array", put(3, b"[]\n"), strict(3, NOT_ARRAY)),
    refusal("record_tag_not_a_string", put(3, b"[1,2]\n"), strict(3, NOT_ARRAY)),
    refusal("record_tag_unknown", put(3, b'["Q",1]\n'), strict(3, ABSENT_TAG)),
    refusal("record_tag_lowercase", swap(3, b'["E",', b'["e",'), strict(3, ABSENT_TAG)),
    refusal("record_tag_not_declared_by_the_header", put(3, b'["V",1]\n'), strict(3, ABSENT_TAG)),
    refusal("record_object_line_is_taken_for_the_trailer", put(3, b'{"a":1}\n'), strict(3, TRAILER_KEYS)),
]

# Physical line layout of the fake engine's sink: narrow is header 1, E 2-6, T 7-9, trailer 10;
# outputs add O 10-11, V 12, K 13 (trailer 14); wide adds X 14, H 15-16 (trailer 17).
TAG_ROWS = [
    refusal("E_arity_short", swap(3, b",-0.0]\n", b"]\n"), strict(3, ARITY)),
    refusal("E_arity_long", swap(3, b",-0.0]\n", b",-0.0,0]\n"), strict(3, ARITY)),
    refusal("E_time_string", swap(3, b'["E",1735776060000,', b'["E","1735776060000",'), strict(3, BAD_I64)),
    refusal("E_time_float", swap(3, b"1735776060000,10001.0", b"1735776060000.0,10001.0"),
            strict(3, BAD_I64)),
    refusal("E_time_bool", swap(3, b"1735776060000,10001.0", b"true,10001.0"), strict(3, BAD_I64)),
    refusal("E_time_null", swap(3, b"1735776060000,10001.0", b"null,10001.0"), strict(3, NULL_COLUMN)),
    refusal("E_equity_string", swap(3, b"10001.0,-0.0", b'"10001.0",-0.0'), strict(3, BAD_F64)),
    refusal("E_equity_bool", swap(3, b"10001.0,-0.0", b"true,-0.0"), strict(3, BAD_F64)),
    refusal("T_arity_short", swap(7, b',"",""]\n', b',""]\n'), strict(7, ARITY)),
    refusal("T_arity_long", swap(7, b',"",""]\n', b',"","",0]\n'), strict(7, ARITY)),
    refusal("T_n_null", swap(7, b'["T",1,', b'["T",null,'), strict(7, NULL_COLUMN)),
    refusal("T_side_neither_long_nor_short", swap(7, b'"long"', b'"buy"'), strict(7, BAD_SIDE)),
    refusal("T_side_number", swap(7, b'"long"', b"5"), strict(7, BAD_STRING)),
    refusal("T_entry_price_string", swap(7, b",100.0,101.0,", b',"100.0",101.0,'), strict(7, BAD_F64)),
    refusal("T_open_at_end_integer", swap(7, b",false,0,0,", b",0,0,0,"), strict(7, BAD_BOOL)),
    refusal("T_entry_incarnation_negative", swap(7, b",false,0,0,", b",false,-1,0,"), strict(7, BAD_U64)),
    refusal("T_close_cause_float", swap(7, b",false,0,0,", b",false,0,0.0,"), strict(7, BAD_I64)),
    refusal("T_exit_comment_number", swap(7, b',"",""]\n', b',"",5]\n'), strict(7, BAD_STRING)),
    refusal("V_arity_short", edit_line(12, lambda d: d[:d.rindex(b',"quote')] + b"]\n"),
            strict(12, ARITY), shape="wide"),
    refusal("V_sequence_negative", swap(12, b'["V",1,1,', b'["V",-1,1,'), strict(12, BAD_U64), shape="wide"),
    refusal("V_phase_null", swap(12, b",0,1,1,null,", b",0,null,1,null,"), strict(12, NULL_COLUMN),
            shape="wide"),
    refusal("V_value_integer_token", swap(12, b",1,1,null,\"0123", b",1,1,7,\"0123"),
            strict(12, BAD_F64), shape="wide"),
    refusal("V_hash_uppercase", swap(12, b'"0123456789abcdef"', b'"0123456789ABCDEF"'),
            strict(12, BAD_HEX), shape="wide"),
    refusal("V_hash_fifteen_digits", swap(12, b'"0123456789abcdef"', b'"0123456789abcde"'),
            strict(12, BAD_HEX), shape="wide"),
    refusal("V_hash_number", swap(12, b'"0123456789abcdef"', b"5"), strict(12, BAD_STRING), shape="wide"),
    refusal("V_message_number", edit_line(12, lambda d: d[:d.rindex(b',"quote')] + b",5]\n"),
            strict(12, BAD_STRING), shape="wide"),
    refusal("X_arity_short", swap(14, b",-0.0]\n", b"]\n"), strict(14, ARITY), shape="wide"),
    refusal("X_timestamp_null", swap(14, b'["X",1735776000000,', b'["X",null,'),
            strict(14, NULL_COLUMN), shape="wide"),
    refusal("X_name_number", edit_line(14, lambda d: d[:d.index(b',"trace')] + b",5,-0.0]\n"),
            strict(14, BAD_STRING), shape="wide"),
    refusal("X_value_string", swap(14, b",-0.0]\n", b',"-0.0"]\n'), strict(14, BAD_F64), shape="wide"),
    refusal("O_arity_short", swap(10, b",-0.0,4294967295.0]\n", b",-0.0]\n"), strict(10, ARITY),
            shape="wide"),
    refusal("O_row_null", swap(10, b'["O",0,', b'["O",null,'), strict(10, NULL_COLUMN), shape="wide"),
    refusal("O_row_string", swap(10, b'["O",0,', b'["O","0",'), strict(10, BAD_I64), shape="wide"),
    refusal("O_column_integer_token", swap(10, b",4294967295.0]", b",4294967295]"), strict(10, BAD_F64),
            shape="wide"),
    refusal("K_arity_short", put(13, b'["K"]\n'), strict(13, ARITY), shape="wide"),
    refusal("K_arity_long", put(13, b'["K",50.0,1.0]\n'), strict(13, ARITY), shape="wide"),
    refusal("K_value_string", put(13, b'["K","50.0"]\n'), strict(13, BAD_F64), shape="wide"),
    refusal("H_arity_long", put(15, b'["H","0123456789abcdef",1]\n'), strict(15, ARITY), shape="wide"),
    refusal("H_hash_uppercase", put(15, b'["H","0123456789ABCDEF"]\n'), strict(15, BAD_HEX), shape="wide"),
    refusal("H_hash_seventeen_digits", put(15, b'["H","0123456789abcdef0"]\n'), strict(15, BAD_HEX),
            shape="wide"),
    refusal("H_hash_null", put(15, b'["H",null]\n'), strict(15, NULL_COLUMN), shape="wide"),
]

TRADE_DOUBLES = (
    ("entry_price", b",100.0,101.0,", b",null,101.0,"),
    ("exit_price", b",101.0,2.0,", b",null,2.0,"),
    ("qty", b",2.0,10.0,", b",null,10.0,"),
    ("pnl", b",10.0,5.0,", b",null,5.0,"),
    ("pnl_pct", b",5.0,12.0,", b",null,12.0,"),
    ("max_runup", b",12.0,-8.0,", b",null,-8.0,"),
    ("max_drawdown", b",-8.0,0.5,", b",null,0.5,"),
    ("commission", b",0.5,0,1,", b",null,0,1,"),
)


def result_invalid(name, edit, field_name, **options):
    return refusal(name, edit, f"the report result {field_name} is not finite",
                   code="report_result_invalid", args={"field": field_name, "reason": "nonfinite"},
                   queries=1, **options)


RESULT_ROWS = [
    *[result_invalid(f"trade_{name}_null", swap(7, old, new), "trades." + name)
      for name, old, new in TRADE_DOUBLES],
    result_invalid("equity_null_in_the_first_point", swap(2, b",10000.0,-0.0]", b",null,-0.0]"),
                   "equity_curve.equity"),
    result_invalid("equity_null_in_a_middle_point", swap(4, b",10002.0,-0.0]", b",null,-0.0]"),
                   "equity_curve.equity"),
    result_invalid("equity_null_in_the_last_point", swap(6, b",10004.0,-0.0]", b",null,-0.0]"),
                   "equity_curve.equity"),
    result_invalid("equity_open_profit_null", swap(3, b",10001.0,-0.0]", b",10001.0,null]"),
                   "equity_curve.open_profit"),
    result_invalid("equity_both_fields_null_names_equity", swap(3, b",10001.0,-0.0]", b",null,null]"),
                   "equity_curve.equity"),
    result_invalid("trade_pnl_overflows_the_drawdown_fold",
                   both(swap(7, b",10.0,5.0,", b",1e+308,5.0,"), swap(8, b",-7.0,5.0,", b",1e+308,5.0,")),
                   "summary.max_drawdown"),
    refusal("trade_numbers_skip_one", swap(8, b'["T",2,', b'["T",3,'), TRADE_ORDER, queries=1),
    refusal("trade_numbers_start_at_two", swap(7, b'["T",1,', b'["T",2,'), TRADE_ORDER, queries=1),
    refusal("trade_numbers_repeat", swap(8, b'["T",2,', b'["T",1,'), TRADE_ORDER, queries=1),
]

TRAILER_ROWS = [
    refusal("trailer_missing", no_trailer, strict(10, "missing header or trailer")),
    refusal("trailer_without_lf", unterminated, strict(10, "line does not end with LF")),
    refusal("trailer_keys_out_of_order",
            on_trailer(lambda t: reorder(t, "trailer", "version", "sha256", "counts")),
            strict(10, TRAILER_KEYS)),
    refusal("trailer_extra_key", on_trailer(lambda t: t.update(extra=True)), strict(10, TRAILER_KEYS)),
    refusal("trailer_name_unknown", on_trailer(lambda t: t.update({"trailer": "other"})),
            strict(10, "unknown trailer")),
    refusal("trailer_version_two", on_trailer(lambda t: t.update(version=2)), strict(10, BAD_VERSION)),
    refusal("trailer_counts_not_an_object", on_trailer(lambda t: t.update(counts=[])),
            strict(10, COUNTS_KEYS)),
    refusal("trailer_counts_out_of_order", on_trailer(lambda t: t.update(counts={"T": 3, "E": 5})),
            strict(10, COUNTS_KEYS)),
    refusal("trailer_counts_missing_a_tag", on_trailer(lambda t: t.update(counts={"E": 5})),
            strict(10, COUNTS_KEYS)),
    refusal("trailer_count_too_high", on_trailer(lambda t: t["counts"].update(E=6)),
            strict(10, "trailer count mismatch for E")),
    refusal("trailer_count_too_low", on_trailer(lambda t: t["counts"].update(T=2)),
            strict(10, "trailer count mismatch for T")),
    refusal("trailer_count_is_a_float", on_trailer(lambda t: t["counts"].update(E=5.0)),
            strict(10, "trailer count mismatch for E")),
    refusal("trailer_count_negative", on_trailer(lambda t: t["counts"].update(E=-1)),
            strict(10, "trailer count mismatch for E")),
    refusal("trailer_count_past_2_53_minus_1", on_trailer(lambda t: t["counts"].update(E=9007199254740992)),
            strict(10, BAD_INTEGER)),
    refusal("trailer_duplicate_key",
            raw_trailer(b'{"trailer":"pf-report-sink","version":1,"version":1,'
                        b'"counts":{"E":5,"T":3},"sha256":"' + b"0" * 64 + b'"}\n'),
            strict(10, "repeated JSON object key")),
    refusal("trailer_digest_wrong", on_trailer(wrong_digest), strict(10, "trailer SHA-256 mismatch")),
    refusal("trailer_digest_uppercase", on_trailer(lambda t: t.update(sha256=t["sha256"].upper())),
            strict(10, "trailer SHA-256 mismatch")),
    refusal("trailer_digest_of_sixty_three_digits", on_trailer(lambda t: t.update(sha256=t["sha256"][:63])),
            strict(10, "trailer SHA-256 mismatch")),
    refusal("trailer_digest_not_a_string", on_trailer(lambda t: t.update(sha256=5)),
            strict(10, BAD_STRING)),
    refusal("trailer_followed_by_text", after_trailer(b"x"), strict(10, "bytes after the trailer")),
    refusal("trailer_followed_by_a_blank_line", after_trailer(b"\n"), strict(10, "bytes after the trailer")),
    refusal("trailer_followed_by_a_record", after_trailer(b'["E",1735776300000,10005.0,-0.0]\n'),
            strict(10, "bytes after the trailer")),
    refusal("trailer_followed_by_another_trailer",
            after_trailer(b'{"trailer":"pf-report-sink","version":1,"counts":{"E":5,"T":3},"sha256":"'
                          + b"0" * 64 + b'"}\n'),
            strict(10, "bytes after the trailer")),
]

BINDING_ROWS = [
    refusal("run_id_differs_from_the_request", on_header(lambda h: h.update(run_id="other")), RUN_ID_DIFFERS),
    refusal("receipt_incomplete", None, RECEIPT_INCOMPLETE, queries=1, fault="incomplete"),
    refusal("receipt_status_nonzero", None, RECEIPT_INCOMPLETE, queries=1, receipt="status"),
    refusal("receipt_digest_differs", None, RECEIPT_DIFFERS, queries=1, fault="receipt"),
    refusal("receipt_count_differs", None, RECEIPT_DIFFERS, queries=1, fault="receipt-count"),
    refusal("receipt_counts_a_tag_the_sink_lacks", None, RECEIPT_DIFFERS, queries=1, receipt="extra-tag"),
    refusal("receipt_trade_total_differs", delete_line(10), RECEIPT_DIFFERS, queries=1,
            pnls=(10.0, -7.0, -0.0, 1.0)),
    refusal("sink_is_gone_when_stat_runs", None, lambda lib: INVALID + not_found(lib), queries=1,
            receipt="unlink", removes_sink=True),
    refusal("sink_is_missing_when_verify_opens_it", None, lambda lib: strict(0, not_found(lib)),
            removes_sink=True, delete_at_run=True),
    refusal("named_sink_survives_a_refusal", on_trailer(wrong_digest), strict(10, "trailer SHA-256 mismatch"),
            named=True),
]

OUTPUT_ROWS = [
    refusal("outputs_constants_record_missing", delete_line(13),
            "the report sink must contain one output constants record", queries=1,
            shape="outputs", cli=("--outputs",)),
    refusal("outputs_constants_record_twice", duplicate_line(13),
            "the report sink must contain one output constants record", queries=1,
            shape="outputs", cli=("--outputs",)),
    refusal("outputs_rows_out_of_order", swap_lines(10, 11),
            "the report sink output rows are out of order", queries=1,
            shape="outputs", cli=("--outputs",)),
    refusal("outputs_summary_without_output_records", None,
            "the report sink is missing its output records", queries=1,
            named=True, cli=("--outputs",)),
    refusal("outputs_reread_sees_another_valid_sink", None, CHANGED, queries=1, shape="outputs",
            cli=("--outputs",), after_verify=rewrite_as_another_valid_sink),
    refusal("outputs_reread_refuses_a_wrong_digest", None, strict(14, "trailer SHA-256 mismatch"),
            queries=1, shape="outputs", cli=("--outputs",), after_verify=rewrite_with_wrong_digest),
]

GUARD_ROWS = [
    refusal("guard_new_inode_same_bytes", None, CHANGED + ": file identity differs", queries=1,
            after_verify=replace_same_bytes),
    refusal("guard_size_grew", None, CHANGED + ": file identity differs", queries=1,
            after_verify=append_byte),
    refusal("guard_size_shrank", None, CHANGED + ": file identity differs", queries=1,
            after_verify=shrink_by_one),
    refusal("guard_edit_in_a_skipped_record", None, CHANGED + ": digest differs", queries=1,
            after_verify=poke_line(6, b"10004.0", b"10005.0")),
    refusal("guard_edit_in_a_selected_record", None, CHANGED + ": digest differs", queries=1,
            after_verify=poke_line(7, b",10.0,5.0,", b",11.0,5.0,")),
    refusal("guard_equity_scan_skips_the_lexical_walk", None, CHANGED + ": digest differs", queries=1,
            on_raw_open=(2, poke_line(4, b",10002.0,-0.0]", b",1002.0, -0.0]"))),
    refusal("guard_equity_scan_json_error", None,
            lambda lib: guard(json_error(
                "Expecting value", sub(lib.rendered.line(4), b"10002.0", b"x0002.0"), "x0002.0")),
            queries=1, on_raw_open=(2, poke_line(4, b"10002.0", b"x0002.0"))),
    refusal("guard_equity_scan_checks_fields", None, guard(BAD_F64), queries=1,
            on_raw_open=(2, poke_line(4, b",10002.0,-0.0]", b",1000200,-0.0]"))),
    refusal("guard_trade_scan_checks_fields", None, guard(BAD_F64), queries=1,
            after_verify=poke_line(7, b",100.0,101.0,", b",10000,101.0,")),
    refusal("guard_trade_scan_checks_the_side", None, guard(BAD_SIDE), queries=1,
            after_verify=poke_line(7, b'"long"', b'"lonk"')),
    refusal("guard_header_is_reparsed", None, guard(WHITESPACE), queries=1,
            after_verify=poke_line(1, b'"schema":', b'"schema" ')),
    refusal("guard_escaped_tag_is_unterminated", None,
            guard("invalid UTF-8 or JSON: unterminated record tag"), queries=1,
            after_verify=poke_line(3, b'["E",', b'["\\E,')),
    refusal("guard_trailer_marker_gone", None, guard("missing header or trailer"), queries=1,
            after_verify=poke_line(10, b'{"trailer"', b'["trailer"')),
    refusal("guard_final_lf_gone", None, guard("line does not end with LF"), queries=1,
            after_verify=clobber_final_lf),
    refusal("guard_bytes_after_a_fake_trailer", None, guard("bytes after the trailer"), queries=1,
            after_verify=poke_line(3, b'["E",', b'{"E",')),
    refusal("guard_header_syntax_error", None,
            guard("invalid UTF-8 or JSON: Expecting ':' delimiter: line 1 column 10 (char 9)"), queries=1,
            after_verify=poke_line(1, b'{"schema":', b'{"schema";')),
    refusal("guard_trade_scan_arity", None, guard(ARITY), queries=1,
            after_verify=poke_line(7, b',"",""]', b',"xxx"]')),
    refusal("guard_equity_scan_arity", None, guard(ARITY), queries=1,
            on_raw_open=(2, poke_line(4, b",-0.0]", b"     ]"))),
    refusal("guard_sink_removed", None, lambda lib: guard(not_found(lib)), queries=1,
            removes_sink=True, after_verify=remove_sink),
]

PRIORITY_ROWS = [
    refusal("priority_record_defect_beats_incomplete_receipt", swap(3, b",-0.0]\n", b"]\n"),
            strict(3, ARITY), fault="incomplete"),
    refusal("priority_run_id_beats_incomplete_receipt", on_header(lambda h: h.update(run_id="other")),
            RUN_ID_DIFFERS, fault="incomplete"),
    refusal("priority_incomplete_receipt_beats_count_mismatch", None, RECEIPT_INCOMPLETE, queries=1,
            receipt="incomplete-and-count"),
    refusal("priority_later_record_defect_beats_earlier_trade_order",
            both(swap(8, b'["T",2,', b'["T",3,'), swap(9, b',"",""]\n', b',""]\n')),
            strict(9, ARITY)),
    result_invalid("priority_trade_failure_beats_an_earlier_equity_failure",
                   both(swap(2, b",10000.0,-0.0]", b",null,-0.0]"),
                        swap(7, b",100.0,101.0,", b",null,101.0,")),
                   "trades.entry_price"),
    refusal("priority_trade_order_beats_trade_field_in_one_row",
            both(swap(7, b'["T",1,', b'["T",2,'), swap(7, b",100.0,101.0,", b",null,101.0,")),
            TRADE_ORDER, queries=1),
    result_invalid("priority_first_bad_trade_wins_in_file_order",
                   both(swap(7, b",0.5,0,1,", b",null,0,1,"), swap(8, b'["T",2,', b'["T",5,')),
                   "trades.commission"),
    result_invalid("priority_first_bad_equity_point_wins_over_a_field_name",
                   both(swap(2, b",10000.0,-0.0]", b",10000.0,null]"),
                        swap(3, b",10001.0,-0.0]", b",null,-0.0]")),
                   "equity_curve.open_profit"),
    refusal("priority_token_error_beats_field_type_in_one_line",
            swap(3, b'["E",1735776060000,10001.0', b'["E","x",9007199254740992'),
            strict(3, BAD_INTEGER)),
    refusal("priority_field_error_beats_the_side_rule",
            both(swap(7, b'"long"', b'"buy"'), swap(7, b',"",""]\n', b',"",5]\n')),
            strict(7, BAD_STRING)),
    refusal("priority_trailer_count_beats_trailer_digest", on_trailer(high_count_and_wrong_digest),
            strict(10, "trailer count mismatch for E")),
    refusal("priority_deferred_trade_failure_loses_to_incomplete_receipt",
            swap(7, b",100.0,101.0,", b",null,101.0,"), RECEIPT_INCOMPLETE, queries=1,
            fault="incomplete"),
    refusal("priority_guard_identity_beats_a_malformed_replacement", None,
            CHANGED + ": file identity differs", queries=1, after_verify=replace_with_garbage),
    refusal("priority_guard_line_walk_beats_digest", None, guard(WHITESPACE), queries=1,
            after_verify=poke_line(7, b'["T",1,"long"', b'["T",1 "long"')),
    refusal("priority_guard_length_is_checked_before_lf", long_trade(MAX_LINE),
            guard("line exceeds 1 MiB including LF"), queries=1,
            after_verify=clobber_last_line_feed_of_trade),
]

REFUSALS = (HEADER_ROWS + FRAMING_ROWS + TOKEN_ROWS + TAG_ROWS + RESULT_ROWS + TRAILER_ROWS
            + BINDING_ROWS + OUTPUT_ROWS + GUARD_ROWS + PRIORITY_ROWS)


def entry_time_is_the_largest_exact_integer(document, text):
    assert document["trades"][0]["entry_time"] == I53_MAX


def negative_zero_integer_is_zero(document, text):
    value = document["trades"][0]["entry_bar_index"]
    assert value == 0 and type(value) is int


def exponent_float_reaches_the_report(document, text):
    assert document["equity_curve"][2]["equity"] == 1e16


VALID_ROWS = [
    valid("valid_baseline_narrow"),
    valid("valid_baseline_outputs", shape="outputs"),
    valid("valid_baseline_wide", shape="wide"),
    valid("valid_baseline_outputs_route", shape="outputs", cli=("--outputs",)),
    valid("valid_line_exactly_at_the_cap", long_trade(MAX_LINE)),
    valid("valid_integer_at_2_53_minus_1", swap(7, b'"long",1735776000000,', b'"long",9007199254740991,'),
          entry_time_is_the_largest_exact_integer),
    valid("valid_negative_zero_integer_in_an_i64_column", swap(7, b",0.5,0,1,false,", b",0.5,-0,1,false,"),
          negative_zero_integer_is_zero),
    valid("valid_escaped_record_tags", both(swap(3, b'["E",', b'["\\u0045",'), swap(8, b'["T",', b'["\\u0054",'))),
    valid("valid_canonical_exponent_spelling", swap(4, b",10002.0,-0.0]", b",1e+16,-0.0]"),
          exponent_float_reaches_the_report),
    valid("valid_surrogate_pair_string", swap(7, b',"",""]\n', b',"","\\ud83d\\ude00"]\n')),
    valid("valid_records_of_different_tags_interleave", swap_lines(6, 7)),
    valid("valid_null_in_an_optional_string",
          edit_line(12, lambda d: d[:d.rindex(b',"quote')] + b",null]\n"), shape="wide"),
    valid("valid_run_id_matches_the_request", cli=("--run-id", "caller-7")),
    valid("valid_guard_ignores_an_edit_inside_the_trailer", after_verify=flip_trailer_digit),
    valid("valid_guard_accepts_the_same_bytes_in_place", after_verify=rewrite_same_bytes_in_place),
]

CASES = REFUSALS + VALID_ROWS


# --- the tests ---------------------------------------------------------------

@pytest.mark.parametrize("case", REFUSALS, ids=lambda case: case.name)
def test_refusal_is_complete(route, case, tmp_path, monkeypatch):
    out = run_host(case, tmp_path, monkeypatch)
    assert_refusal(case, out)
    assert_route(route, case, out)


@pytest.mark.parametrize("case", VALID_ROWS, ids=lambda case: case.name)
def test_just_valid_twin_succeeds(route, case, tmp_path, monkeypatch):
    out = run_host(case, tmp_path, monkeypatch)
    assert_valid(case, out)
    assert_route(route, case, out)


def test_matrix_is_complete_and_unambiguous():
    names = [case.name for case in CASES]
    assert len(names) == len(set(names))
    assert "strict" in ROUTES
    assert "native" in ROUTES
    assert sum(case.name.startswith("priority_") for case in REFUSALS) >= 8
    assert len(VALID_ROWS) >= 5
    assert all(callable(case.want.text) or case.want.text for case in REFUSALS)
