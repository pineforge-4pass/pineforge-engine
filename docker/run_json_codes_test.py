"""run_json's failure line: {"engine","error","code","args"}, one writer for every
failure path, the code from the engine's getter or run_json's own refusal.

Same harness style as run_json_syminfo_test.py: docker/ is on sys.path, the strategy
library is a fake whose exports record their calls. Every code and argument the
tests see is also checked against the catalog, docker/run_failure_codes.json.
"""
import contextlib
import ctypes
import io
import json
import os
import re
import sys
from pathlib import Path

import pytest
import run_json  # docker/ is on sys.path in the engine test env

CATALOG = json.loads((Path(__file__).resolve().parent / "run_failure_codes.json")
                     .read_text(encoding="utf-8"))


def release_provenance(tmp_path, inputs=None, overrides=None):
    cpp = '''GeneratedStrategy() {
        pineforge::source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        configure_pine_strategy(cfg);
    }
    void init() {
        get_input_bool("armed", true);
        get_input_bool("disabled", false);
        get_input_int("length", 10);
        get_input_int64("time", 1000);
        get_input_double("mult", 2.0);
        get_input_string("text", std::string("base"));
        get_input_source("source", close);
    }'''
    path = tmp_path / "generated.cpp"
    path.write_text(cpp)
    inputs = inputs or {}
    overrides = overrides or {}
    document = run_json.build_provenance(
        {}, path, True, inputs, overrides, {}, source_feed_sha256="0" * 64)
    return document, cpp


@pytest.mark.parametrize("token,expected", [
    ("true", True), ("1", True), ("false", False), ("0", False),
    ("False", True), ("", True), (" true", True), ("2", True),
])
def test_release_input_boolean_tokens_and_default(tmp_path, token, expected):
    inputs = {"armed": token, "disabled": "unrecognized"}
    before = dict(inputs)
    document, cpp = release_provenance(tmp_path, inputs)
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    assert result["inputs"]["armed"]["value"] is expected
    assert result["inputs"]["disabled"]["value"] is False
    assert result["applied"]["inputs"]["armed"] is expected
    assert inputs == before
    assert set(result["applied"]["inputs"]) == set(inputs)
    assert result["applied"]["overrides"] == {}


def test_release_legacy_numbers_strings_unknowns_and_hash(tmp_path):
    inputs = {"length": "1.4e1", "time": "1001tail", "mult": "1.50e0",
              "text": "false", "source": "14", "unknown": "true"}
    document, cpp = release_provenance(tmp_path, inputs)
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    expected = {"length": 1, "time": 1001, "mult": 1.5,
                "text": "false", "unknown": "true"}
    for name, value in expected.items():
        assert type(result["inputs"][name]["value"]) is type(value)
        assert result["inputs"][name]["value"] == value
        assert result["applied"]["inputs"][name] == value
    # A source default outside the native selector vocabulary is refused, and
    # the override "14" is not echoed as effective: it stays wire text.
    assert result["inputs"]["source"] == {
        "type": "source", "default": None, "value": None,
        "resolution": {"status": "unresolved", "reason": "unsupported_default",
                       "raw_default": "close"}}
    assert result["applied"]["inputs"]["source"] == "14"
    fingerprint = run_json.build_fingerprint(result)
    assert json.loads(run_json.base64.b64decode(fingerprint["token"])) == result
    assert fingerprint["digest"] == "sha256:" + run_json.hashlib.sha256(
        run_json.base64.b64decode(fingerprint["token"])).hexdigest()
    assert inputs["length"] == "1.4e1"


@pytest.mark.parametrize("token", ["nonsense", "2147483648", "1e999"])
def test_release_legacy_integer_conversion_falls_back(tmp_path, token):
    document, cpp = release_provenance(tmp_path, {"length": token})
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    expected = 1 if token == "1e999" else 10
    assert result["inputs"]["length"]["value"] == expected
    assert type(result["inputs"]["length"]["value"]) is int


def test_release_all_override_types_and_legacy_fallbacks(tmp_path):
    overrides = {
        "initial_capital": "5e3", "commission_value": "0.04",
        "default_qty_value": "3.5", "pyramiding": "2.0", "slippage": "1e0",
        "process_orders_on_close": "1", "calc_on_order_fills": "True",
        "close_entries_rule": "true", "default_qty_type": "strategy.cash",
        "commission_type": "strategy.commission.cash_per_order", "unknown": "false",
    }
    before = dict(overrides)
    document, cpp = release_provenance(tmp_path, overrides=overrides)
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    expected = {
        "initial_capital": 5000.0, "commission_value": 0.04,
        "default_qty_value": 3.5, "pyramiding": 2, "slippage": 1,
        "process_orders_on_close": True, "calc_on_order_fills": False,
        "close_entries_rule": "FIFO", "default_qty_type": "cash",
        "commission_type": "cash_per_order", "unknown": "false",
    }
    for section in (result["strategy"], result["applied"]["overrides"]):
        for name, value in expected.items():
            assert type(section[name]) is type(value)
            assert section[name] == value
    assert overrides == before
    assert set(result["applied"]["overrides"]) == set(overrides)
    document, cpp = release_provenance(tmp_path, overrides={
        "pyramiding": "-1", "slippage": "-1", "initial_capital": "nan",
        "default_qty_type": "invalid", "commission_type": "invalid"})
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    for name in result["applied"]["overrides"]:
        assert result["applied"]["overrides"][name] == run_json.STRATEGY_SEED[name]


def test_release_checked_receipt_is_authoritative_or_fails(tmp_path):
    inputs = {"length": "1.4e1", "armed": "0", "text": "false"}
    document, cpp = release_provenance(tmp_path, inputs)
    receipt = {"version": 1, "inputs": [], "overrides": []}
    for name, metadata in document["inputs"].items():
        declared_type = {"double": "float", "int64": "int"}.get(
            metadata["type"], metadata["type"])
        default = str(metadata["default"]).lower()
        value = {"length": "14", "armed": "false", "text": "false"}.get(name, default)
        receipt["inputs"].append({
            "name": name, "type": declared_type, "kind": metadata["type"],
            "default": default, "effective_value": value, "supported": True})
    for name, declared_type in run_json._RELEASE_OVERRIDE_TYPES.items():
        value = str(run_json.STRATEGY_SEED.get(name, True))
        if declared_type == "bool":
            value = value.lower()
        receipt["overrides"].append({
            "name": name, "type": declared_type, "default": value,
            "effective_value": value, "supported": True})
    result = run_json.normalize_release_provenance(document, cpp, receipt, True)
    assert result["inputs"]["length"]["value"] == 14
    assert result["inputs"]["armed"]["value"] is False
    assert result["inputs"]["text"]["value"] == "false"
    assert result["inputs"]["source"]["value"] == "close"
    assert result["applied"]["overrides"] == {}
    assert inputs == {"length": "1.4e1", "armed": "0", "text": "false"}
    with pytest.raises(ValueError, match="unavailable"):
        run_json.normalize_release_provenance(document, cpp, None, True)
    receipt["inputs"][0]["supported"] = False
    result = run_json.normalize_release_provenance(document, cpp, receipt, True)
    assert result["inputs"]["armed"]["default"] is False
    receipt["inputs"].append(dict(receipt["inputs"][0]))
    with pytest.raises(ValueError, match="ambiguous"):
        run_json.normalize_release_provenance(document, cpp, receipt, True)

ST = 7
CORE = ("pf_abi_version", "strategy_create", "strategy_set_input", "strategy_set_override",
        "run_backtest_full", "strategy_free", "report_free")
TAPE = ("open,high,low,close,volume,timestamp\n"
        "1,2,0.5,1.5,10,1000\n1.5,3,1,2.5,20,2000\n")
PF_OK, PF_INVALID, PF_UNSUPPORTED, PF_EXCEPTION, PF_SMALL, PF_RUN_FAILED = range(6)


class FakeLib:
    """A strategy library stand-in: each named export records (name, *args) and
    returns its configured result, or runs its configured function; a name left
    out fails hasattr like a missing export."""

    def __init__(self, names=(), returns=None, impl=None):
        self.calls = []
        returns = returns or {}
        for name in names:
            setattr(self, name, self._fn(name, lambda *a, r=returns.get(name): r))
        for name, body in (impl or {}).items():
            setattr(self, name, self._fn(name, body))

    def _fn(self, name, body):
        def call(*args):
            self.calls.append((name,) + args)
            return body(*args)
        return call

    def names(self):
        return [c[0] for c in self.calls]


def fake_lib(*extra, returns=None, impl=None):
    rets = {"pf_abi_version": run_json.EXPECTED_PF_ABI, "strategy_create": ST,
            **(returns or {})}
    return FakeLib(CORE + tuple(extra), rets, impl)


def catalogued(code, args):
    """The code is in the catalog and args satisfy its declared arguments."""
    entry = CATALOG["codes"][code]
    declared = entry["args"]
    assert set(args) <= set(declared), (code, args)
    for name, spec in declared.items():
        assert spec.get("optional") or name in args, (code, name)
    for name, value in args.items():
        kind = declared[name]["kind"]
        if kind == "vocab":
            assert value in declared[name]["values"], (code, name, value)
        elif kind == "integer":
            assert isinstance(value, int) and not isinstance(value, bool), (code, name)
        elif kind == "number":
            assert isinstance(value, (int, float)) and not isinstance(value, bool)
        else:
            assert isinstance(value, str), (code, name)
    return True


def line_of(out):
    """The one failure line: one line, keys in order, code and args catalogued."""
    assert out.endswith("\n") and out.count("\n") == 1, out
    assert out.startswith('{"engine":"pineforge","error":"')
    assert out.isascii()
    doc = json.loads(out)
    if "code" in doc:
        assert list(doc) == ["engine", "error", "code", "args"]
        assert catalogued(doc["code"], doc["args"])
    else:
        assert list(doc) == ["engine", "error"]
    return doc


@pytest.fixture
def harness(tmp_path, monkeypatch):
    tape = tmp_path / "tape.csv"
    tape.write_text(TAPE)

    def run(lib, *extra, ohlcv=None, cdll=None):
        argv = ["run_json.py", "--so", "fake.so", "--ohlcv", str(ohlcv or tape), *extra]
        monkeypatch.setattr(ctypes, "CDLL", cdll or (lambda path: lib))
        monkeypatch.setattr(sys, "argv", argv)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = run_json.main()
        run.stderr = err.getvalue()
        return status, out.getvalue()

    return run


def report_of(text):
    rep = json.loads(text)
    rep["elapsed_seconds"] = 0
    return rep


# --- the line ----------------------------------------------------------------

def test_line_shape_and_key_order():
    out = run_json.failure_line("boom", "engine_invariant", {})
    assert out == '{"engine":"pineforge","error":"boom","code":"engine_invariant","args":{}}\n'
    out = run_json.failure_line("x", "setting_rejected",
                                {"entrypoint": "strategy_set_input", "reason": "unknown_key"})
    assert out == ('{"engine":"pineforge","error":"x","code":"setting_rejected",'
                   '"args":{"entrypoint":"strategy_set_input","reason":"unknown_key"}}\n')


def test_a_line_without_code_is_the_old_line_byte_for_byte():
    text = 'x "q" \\ \u00e9\n\U0001F600\x01'
    assert run_json.failure_line(text) == (
        '{"engine":"pineforge","error":"x \\"q\\" \\\\ \\u00e9\\n\\ud83d\\ude00\\u0001"}\n')


def test_the_old_engine_line_from_a_library_without_the_code_getter(harness):
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   returns={"strategy_get_last_error": "bar[1].high is NaN \u00e9".encode(),
                            "strategy_last_run_status": 1})
    status, out = harness(lib)
    assert status == 1
    assert out == '{"engine":"pineforge","error":"bar[1].high is NaN \\u00e9"}\n'
    assert line_of(out) == {"engine": "pineforge", "error": "bar[1].high is NaN \u00e9"}


def test_the_engine_code_and_args_ride_the_line(harness):
    lib = fake_lib("strategy_get_last_error", "strategy_get_last_error_code",
                   "strategy_get_last_error_args",
                   returns={"strategy_get_last_error": b"matrix.get: row index out of range",
                            "strategy_get_last_error_code": b"pine_matrix_error",
                            "strategy_get_last_error_args":
                                b'{"function":"matrix.get","reason":"row_index_out_of_range"}'})
    status, out = harness(lib)
    assert status == 1
    assert line_of(out) == {
        "engine": "pineforge", "error": "matrix.get: row index out of range",
        "code": "pine_matrix_error",
        "args": {"function": "matrix.get", "reason": "row_index_out_of_range"}}
    assert lib.names().count("run_backtest_full") == 1


@pytest.mark.parametrize("raw,expected", [
    (b'{"a":"x","b":3,"c":1.5,"d":true,"e":null}',
     {"a": "x", "b": 3, "c": 1.5, "d": True, "e": None}),
    (b"{}", {}),
    (b'["a"]', {}),
    (b'{"a":{"b":1}}', {}),
    (b'{"a":[1]}', {}),
    (b'{"a":NaN}', {}),
    (b'{"a":Infinity}', {}),
    (b"{not json", {}),
    (b"", {}),
    (None, {}),
    (b'{"a":"\xff"}', {}),
], ids=["scalars", "empty", "array", "nested-object", "nested-array", "nan", "infinity",
        "not-json", "empty-text", "null-pointer", "not-utf8"])
def test_engine_args_are_reparsed_as_an_object_of_scalars(raw, expected):
    assert run_json._engine_args(raw) == expected


def test_engine_code_getter_reads():
    assert run_json.engine_failure_code(FakeLib(), ST) is None
    lib = FakeLib(("strategy_get_last_error_code",), {"strategy_get_last_error_code": b""})
    assert run_json.engine_failure_code(lib, ST) == ("", {})
    lib = FakeLib(("strategy_get_last_error_code",), {"strategy_get_last_error_code": b"x Y"})
    assert run_json.engine_failure_code(lib, ST) == ("engine_unclassified_error", {})
    lib = FakeLib(("strategy_get_last_error_code",),
                  {"strategy_get_last_error_code": b"out_of_memory"})
    assert run_json.engine_failure_code(lib, ST) == ("out_of_memory", {})


# --- caps --------------------------------------------------------------------

def test_a_20k_message_is_cut_to_16k():
    doc = json.loads(run_json.failure_line("a" * 20480, "strategy_runtime_error", {}))
    assert doc["error"] == "a" * 16384


@pytest.mark.parametrize("char", ["\u00e9", "\u20ac", "\U0001F600"], ids=["2-byte", "3-byte",
                                                                          "4-byte"])
def test_the_text_cut_backs_off_to_a_character_boundary(char):
    text = "a" * 16383 + char + "b" * 10
    doc = json.loads(run_json.failure_line(text, "strategy_runtime_error", {}))
    assert doc["error"] == "a" * 16383
    fits = "a" * (16384 - len(char.encode())) + char
    doc = json.loads(run_json.failure_line(fits + "b", "strategy_runtime_error", {}))
    assert doc["error"] == fits


def test_a_line_without_code_is_capped_too():
    assert json.loads(run_json.failure_line("z" * 17000))["error"] == "z" * 16384


@pytest.mark.parametrize("value,expected", [
    ("x" * 2048, "x" * 1024),
    ("\u00e9" * 1024, "\u00e9" * 512),
    ("\u20ac" * 700, "\u20ac" * 341),
    ("\U0001F600" * 300, "\U0001F600" * 256),
], ids=["ascii", "2-byte", "3-byte", "4-byte"])
def test_a_string_arg_is_cut_to_1k(value, expected):
    doc = json.loads(run_json.failure_line(
        "t", "setting_rejected",
        {"entrypoint": "strategy_set_input", "reason": "unknown_key", "input": value}))
    assert doc["args"]["input"] == expected
    assert doc["args"]["entrypoint"] == "strategy_set_input"


def test_non_string_args_are_kept():
    doc = json.loads(run_json.failure_line("t", "strategy_library_incompatible",
                                           {"reason": "abi_mismatch", "abi": 3}))
    assert doc["args"] == {"reason": "abi_mismatch", "abi": 3}


def test_escape_heavy_text_still_fits_the_line():
    text = "\x01" * 20000  # six bytes each once escaped
    out = run_json.failure_line(text, "strategy_runtime_error", {})
    assert len(out) <= run_json.ERROR_LINE_MAX + 1
    doc = json.loads(out)
    assert doc["error"] and text.startswith(doc["error"])
    assert doc["code"] == "strategy_runtime_error"
    longer = run_json._cut_utf8(text, len(doc["error"]) + 1)
    assert len(run_json._dump_line({**doc, "error": longer})) > run_json.ERROR_LINE_MAX


def test_a_lone_surrogate_survives_the_cut():
    text = "a" * 10 + "\udcff"
    assert run_json._cut_utf8(text, 16384) == text
    assert run_json._cut_utf8(text, 12) == "a" * 10


def test_arguments_that_alone_overflow_the_line_are_dropped_before_the_text():
    args = {f"a{i}": "\x01" * 1024 for i in range(11)}  # 6 KiB each once escaped
    out = run_json.failure_line("the real reason", "pine_invalid_argument", args)
    assert json.loads(out) == {"engine": "pineforge", "error": "the real reason",
                               "code": "pine_invalid_argument", "args": {}}


def test_arguments_that_fit_alone_are_kept_and_the_text_is_cut():
    args = {f"a{i}": "\x01" * 1024 for i in range(9)}  # 54 KiB once escaped
    text = "x" * 16384
    out = run_json.failure_line(text, "pine_invalid_argument", args)
    doc = json.loads(out)
    assert len(out) <= run_json.ERROR_LINE_MAX + 1
    assert doc["args"] == args
    assert doc["error"] and text.startswith(doc["error"]) and len(doc["error"]) < len(text)


# A \ud800-\udfff escape: what json.dump writes for a lone surrogate, and what a
# strict JSON parser rejects (this line holds no character outside the BMP).
SURROGATE_ESCAPE = re.compile(r"\\ud[89a-f][0-9a-f]{2}")


def test_a_lone_surrogate_is_printed_as_u_fffd():
    out = run_json.failure_line("x\udcffy", "setting_rejected",
                                {"entrypoint": "strategy_set_input", "input": "a\ud800b"})
    assert not SURROGATE_ESCAPE.search(out)
    doc = json.loads(out)
    assert (doc["error"], doc["args"]["input"]) == ("x\ufffdy", "a\ufffdb")
    assert run_json.failure_line("p\udc80") == '{"engine":"pineforge","error":"p\\ufffd"}\n'


def test_a_path_with_a_non_utf8_byte_is_printed_with_u_fffd(harness, tmp_path):
    lib = fake_lib()
    status, out = harness(lib, ohlcv=tmp_path / "bars\udcff.csv")
    doc = line_of(out)
    assert status == 1
    assert (doc["code"], doc["args"]) == ("chart_bars_unreadable", {"reason": "io"})
    assert not SURROGATE_ESCAPE.search(out)
    assert doc["error"].startswith(f"--ohlcv: {tmp_path}/bars\ufffd.csv: ")


# --- the run's status ----------------------------------------------------------

def test_status_1_with_no_text_from_a_library_without_the_code_getter_has_no_code(harness):
    # A library built before the getters: the line of its era, with a fixed text.
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   returns={"strategy_get_last_error": b"", "strategy_last_run_status": 1})
    status, out = harness(lib)
    assert status == 1
    assert out == ('{"engine":"pineforge","error":"the run did not complete and the engine '
                   'reported no error"}\n')
    assert line_of(out) == {"engine": "pineforge", "error": run_json.RUN_STATUS_FAILED_TEXT}


def test_status_1_with_a_code_getter_that_names_nothing_fails(harness):
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   "strategy_get_last_error_code", "strategy_get_last_error_args",
                   returns={"strategy_get_last_error": b"", "strategy_last_run_status": 1,
                            "strategy_get_last_error_code": b"",
                            "strategy_get_last_error_args": b""})
    status, out = harness(lib)
    assert status == 1
    assert line_of(out) == {
        "engine": "pineforge",
        "error": "the run did not complete and the engine reported no error",
        "code": "engine_unclassified_error", "args": {}}


def test_a_code_with_an_empty_text_fails(harness):
    # runtime.error() with no message: text "", code strategy_runtime_error.
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   "strategy_get_last_error_code", "strategy_get_last_error_args",
                   returns={"strategy_get_last_error": b"", "strategy_last_run_status": 0,
                            "strategy_get_last_error_code": b"strategy_runtime_error",
                            "strategy_get_last_error_args": b"{}"})
    status, out = harness(lib)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge", "error": "",
                            "code": "strategy_runtime_error", "args": {}}


def test_a_text_the_code_getter_does_not_name_is_unclassified(harness):
    lib = fake_lib("strategy_get_last_error", "strategy_get_last_error_code",
                   returns={"strategy_get_last_error": b"boom",
                            "strategy_get_last_error_code": b""})
    status, out = harness(lib)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge", "error": "boom",
                            "code": "engine_unclassified_error", "args": {}}


def test_a_clean_run_is_a_report(harness):
    plain_status, plain = harness(fake_lib())
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   "strategy_get_last_error_code", "strategy_get_last_error_args",
                   returns={"strategy_get_last_error": b"", "strategy_last_run_status": 0,
                            "strategy_get_last_error_code": b"",
                            "strategy_get_last_error_args": b""})
    status, out = harness(lib)
    assert (status, plain_status) == (0, 0)
    assert report_of(out) == report_of(plain)
    assert "error" not in report_of(out)


class FailingStdout(io.StringIO):
    """A stdout that takes the first 100 characters of the report, then fails."""

    def __init__(self, error):
        super().__init__()
        self.error = error

    def write(self, text):
        if self.error is None:
            return super().write(text)
        error, self.error = self.error, None
        super().write(text[:100])
        raise error


@pytest.mark.parametrize("error", [BrokenPipeError(32, "Broken pipe"),
                                   OSError(28, "No space left on device")],
                         ids=["broken-pipe", "disk-full"])
def test_a_report_write_error_adds_no_failure_line(tmp_path, monkeypatch, error):
    tape = tmp_path / "tape.csv"
    tape.write_text(TAPE)
    lib = fake_lib()
    monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
    out, err = FailingStdout(error), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = run_json.main(["--so", "fake.so", "--ohlcv", str(tape)])
    assert status == 1
    assert out.getvalue().startswith('{"engine":"pineforge","input":')
    assert len(out.getvalue()) == 100  # the part written, and nothing after it
    assert err.getvalue() == f"run_json: the report could not be written: {error}\n"
    assert lib.names().count("strategy_create") == lib.names().count("strategy_free") == 1


def test_a_closed_pipe_leaves_nothing_for_the_flush_at_exit(tmp_path, monkeypatch):
    tape = tmp_path / "tape.csv"
    tape.write_text(TAPE)
    monkeypatch.setattr(ctypes, "CDLL", lambda path: fake_lib())
    read_end, write_end = os.pipe()
    os.close(read_end)  # the reader is gone: every write is EPIPE
    stdout, err = open(write_end, "w", encoding="ascii"), io.StringIO()
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(err):
            status = run_json.main(["--so", "fake.so", "--ohlcv", str(tape)])
        assert status == 1
        assert err.getvalue().startswith("run_json: the report could not be written: [Errno 32]")
        stdout.flush()  # what the interpreter does at exit: /dev/null takes the rest
    finally:
        stdout.close()


def test_load_strategy_declares_the_failure_getters_and_the_checked_api(monkeypatch):
    lib = checked_lib("strategy_get_last_error", "strategy_get_last_error_code",
                      "strategy_get_last_error_args", "strategy_last_run_status")
    monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
    run_json.load_strategy(Path("fake.so"))
    for name in ("strategy_get_last_error", "strategy_get_last_error_code",
                 "strategy_get_last_error_args"):
        assert getattr(lib, name).argtypes == [ctypes.c_void_p], name
        assert getattr(lib, name).restype is ctypes.c_char_p, name
    assert lib.strategy_last_run_status.argtypes == [ctypes.c_void_p]
    assert lib.strategy_last_run_status.restype is ctypes.c_int
    assert lib.strategy_settings_api_version.argtypes == []
    assert lib.strategy_settings_api_version.restype is ctypes.c_uint32
    assert lib.strategy_create_checked.argtypes == [
        ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_size_t]
    assert lib.strategy_create_checked.restype is ctypes.c_int
    for name in ("strategy_set_input_checked", "strategy_set_override_checked"):
        assert getattr(lib, name).argtypes == [ctypes.c_void_p, ctypes.c_char_p,
                                               ctypes.c_char_p, ctypes.c_char_p,
                                               ctypes.c_size_t], name
        assert getattr(lib, name).restype is ctypes.c_int, name
    assert lib.strategy_get_effective_settings.restype is ctypes.c_int


# --- the strategy handle -----------------------------------------------------

@pytest.mark.parametrize("with_feeds", [False, True], ids=["plain", "symbol-feeds"])
def test_a_null_handle_fails_before_any_setter(harness, tmp_path, with_feeds):
    extra = ["--inputs", '{"Length": "5"}', "--syminfo", str(tmp_path / "s.json")]
    (tmp_path / "s.json").write_text('{"mintick": 0.5}')
    if with_feeds:
        (tmp_path / "f.csv").write_text("timestamp,open,high,low,close\n1000,1,2,0.5,1.5\n")
        (tmp_path / "i.json").write_text(json.dumps(
            {"symbols": {"X:Y": {"feeds": {"60": "f.csv"}}}}))
        extra += ["--symbol-feeds", str(tmp_path / "i.json")]
    lib = fake_lib("strategy_set_syminfo_mintick", "strategy_set_symbol_facts",
                   "strategy_set_symbol_feed", returns={"strategy_create": None})
    status, out = harness(lib, *extra)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge", "error": "strategy_create failed",
                            "code": "strategy_create_failed", "args": {}}
    assert lib.names() == ["pf_abi_version", "strategy_create"]


def test_a_null_handle_in_bench_mode_does_not_crash(harness):
    lib = fake_lib(returns={"strategy_create": None})
    status, out = harness(lib, "--bench", "--warmup", "1", "--repeats", "1")
    assert status == 1
    assert line_of(out)["code"] == "strategy_create_failed"
    assert "run_backtest_full" not in lib.names()


# --- run_json's own refusals ---------------------------------------------------

@pytest.mark.parametrize("extra,text,option", [
    (["--inputs", "{nope"], "error: --inputs is not valid JSON: ", "inputs"),
    (["--overrides", "[1]"], "error: --overrides must be a JSON object, got list", "overrides"),
    (["--inputs", "[" * 100000], "error: --inputs is not valid JSON: ", "inputs"),
    (["--inputs", '{"\\ud800": "1"}'], "error: --inputs must hold UTF-8 text", "inputs"),
    (["--magnifier-dist", "spiral"],
     "error: --magnifier-dist must be one of ['back_loaded', 'cosine', 'endpoints', "
     "'front_loaded', 'triangle', 'uniform'] or 0-5, got 'spiral'", "magnifier_dist"),
    (["--magnifier-dist", "\u00b2"], "error: --magnifier-dist must be one of", "magnifier_dist"),
], ids=["inputs-json", "overrides-object", "inputs-deep", "inputs-surrogate", "dist",
        "dist-superscript"])
def test_bad_request_options_are_run_request_invalid(harness, extra, text, option):
    lib = fake_lib()
    status, out = harness(lib, *extra)
    doc = line_of(out)
    assert status == 1
    assert doc["error"].startswith(text)
    assert (doc["code"], doc["args"]) == ("run_request_invalid", {"option": option})
    assert lib.calls == []


@pytest.fixture
def int_digit_limit():
    """Python's default cap on an int's decimal digits (3.11+), whatever the
    environment set: json.loads raises a plain ValueError past it."""
    old = sys.get_int_max_str_digits()
    sys.set_int_max_str_digits(4300)
    yield
    sys.set_int_max_str_digits(old)


@pytest.mark.parametrize("label", ["inputs", "overrides"])
def test_an_integer_past_the_digit_cap_is_run_request_invalid(harness, int_digit_limit, label):
    lib = fake_lib()
    status, out = harness(lib, f"--{label}", '{"Length": 1' + "0" * 5000 + "}")
    doc = line_of(out)
    assert status == 1
    assert doc["error"].startswith(f"error: --{label} is not valid JSON: Exceeds the limit (4300")
    assert (doc["code"], doc["args"]) == ("run_request_invalid", {"option": label})
    assert lib.calls == []


@pytest.mark.parametrize("value,kind", [(True, "true"), (False, "false"), (None, "null"),
                                        ([1], "an array"), ({"a": 1}, "an object")],
                         ids=["true", "false", "null", "array", "object"])
@pytest.mark.parametrize("label", ["inputs", "overrides"])
@pytest.mark.parametrize("checked", [False, True], ids=["legacy", "checked"])
def test_a_value_neither_string_nor_number_is_run_request_invalid(harness, checked, label,
                                                                   value, kind):
    lib = checked_lib() if checked else fake_lib()
    status, out = harness(lib, f"--{label}", json.dumps({"A": "1", "Use Filter": value}))
    assert status == 1
    assert line_of(out) == {
        "engine": "pineforge",
        "error": f'error: --{label}: "Use Filter" must be a string or a number, got {kind}',
        "code": "run_request_invalid", "args": {"option": label}}
    assert lib.calls == []  # before the library is even loaded


def test_numbers_are_passed_as_str_spells_them_as_before(harness):
    raw = '{"Length": 5, "Mult": 0.5, "Big": 1e3, "Neg": -2, "Text": "7"}'
    sent = {"Length": "5", "Mult": "0.5", "Big": "1000.0", "Neg": "-2", "Text": "7"}
    lib = fake_lib()
    status, out = harness(lib, "--inputs", raw, "--overrides", '{"pyramiding": 2}')
    assert status == 0
    assert report_of(out)["applied_inputs"] == sent
    assert report_of(out)["applied_overrides"] == {"pyramiding": "2"}
    assert [c[2:] for c in lib.calls if c[0] == "strategy_set_input"] == [
        (k.encode(), v.encode()) for k, v in sent.items()]
    assert ("strategy_set_override", ST, b"pyramiding", b"2") in lib.calls
    lib = checked_lib()
    status, _ = harness(lib, "--inputs", raw)
    assert status == 0
    assert [c[2:4] for c in lib.calls if c[0] == "strategy_set_input_checked"] == [
        (k.encode(), v.encode()) for k, v in sent.items()]


@pytest.mark.parametrize("flag,value,option", [
    ("--input-tf", "15\udcff", "input_tf"),
    ("--script-tf", "\udcff60", "script_tf"),
    ("--chart-tz", "Europe/\udcff", "chart_tz"),
], ids=["input-tf", "script-tf", "chart-tz"])
def test_a_non_utf8_option_is_run_request_invalid(harness, flag, value, option):
    lib = fake_lib("strategy_set_chart_timezone")
    status, out = harness(lib, flag, value)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge", "error": f"error: {flag} must hold UTF-8 text",
                            "code": "run_request_invalid", "args": {"option": option}}
    assert lib.calls == []


@pytest.mark.parametrize("rows,stamp", [
    (f"1,2,0.5,1.5,10,{2**62}\n", 2**62),
    (f"1,2,0.5,1.5,10,1000\n1,2,0.5,1.5,10,{-2**62}\n", -2**62),
    ("1,2,0.5,1.5,10,253402300800000\n", 253402300800000),
], ids=["first", "last", "year-10000"])
def test_a_tape_timestamp_outside_the_calendar_is_chart_bars_unreadable(harness, tmp_path,
                                                                         rows, stamp):
    path = tmp_path / "bars.csv"
    path.write_text("open,high,low,close,volume,timestamp\n" + rows)
    lib = fake_lib()
    status, out = harness(lib, ohlcv=path)
    assert status == 1
    assert line_of(out) == {
        "engine": "pineforge",
        "error": f"--ohlcv: {path}: timestamp {stamp} is out of the calendar's range",
        "code": "chart_bars_unreadable", "args": {"reason": "value"}}
    # Refused once the run is over, when the report would spell the dates.
    assert "run_backtest_full" in lib.names()


def test_the_engine_refusal_of_a_tape_outside_the_calendar_keeps_its_line(harness, tmp_path):
    # Base printed the engine's own refusal for this tape; the calendar check
    # never pre-empts it.
    path = tmp_path / "bars.csv"
    path.write_text(f"open,high,low,close,volume,timestamp\n1,2,0.5,1.5,10,1000\n"
                    f"1,2,0.5,1.5,10,{-2**62}\n")
    lib = fake_lib("strategy_get_last_error", "strategy_last_run_status",
                   returns={"strategy_get_last_error": b"bar[1].timestamp must be strictly increasing",
                            "strategy_last_run_status": 0})
    status, out = harness(lib, ohlcv=path)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge",
                            "error": "bar[1].timestamp must be strictly increasing"}


def test_the_calendar_bounds_themselves_still_run(harness, tmp_path):
    path = tmp_path / "bars.csv"
    path.write_text("open,high,low,close,volume,timestamp\n"
                    "1,2,0.5,1.5,10,-62135596800000\n1,2,0.5,1.5,10,253402300799999\n")
    status, out = harness(fake_lib(), ohlcv=path)
    assert status == 0
    # The C library pads year 1 to four digits or not (macOS: 0001, glibc: 1).
    first, last = report_of(out)["input"]["first_time"], report_of(out)["input"]["last_time"]
    assert first in ("0001-01-01 00:00 UTC", "1-01-01 00:00 UTC")
    assert last == "9999-12-31 23:59 UTC"


@pytest.mark.parametrize("argv,option", [
    (["--ohlcv", "x.csv"], "so"),
    ([], "arguments"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--magnifier-samples", "four"], "magnifier_samples"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--trade-start-ms", "soon"], "trade_start_ms"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--warmup", "x"], "warmup"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--input-tf"], "input_tf"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--bench=1"], "bench"),
    (["--so", "a.so", "--ohlcv", "x.csv", "--nope"], "arguments"),
], ids=["no-so", "nothing", "samples", "trade-start", "warmup", "tf-no-value", "bench-value",
        "unknown-flag"])
def test_argparse_errors_are_run_request_invalid_with_exit_2(monkeypatch, argv, option):
    monkeypatch.setattr(sys, "argv", ["run_json.py", *argv])
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
            pytest.raises(SystemExit) as stop:
        run_json.main()
    assert stop.value.code == 2
    doc = line_of(out.getvalue())
    assert (doc["code"], doc["args"]) == ("run_request_invalid", {"option": option})
    assert doc["error"].startswith("run_json.py: error: ")
    assert err.getvalue().startswith("usage: run_json.py")
    assert err.getvalue().endswith(doc["error"] + "\n")


def test_help_is_not_a_failure(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["run_json.py", "--help"])
    out = io.StringIO()
    with contextlib.redirect_stdout(out), pytest.raises(SystemExit) as stop:
        run_json.main()
    assert stop.value.code == 0
    assert out.getvalue().startswith("usage: run_json.py")
    assert '{"engine":"pineforge","error":' not in out.getvalue().splitlines()[-1]


@pytest.mark.parametrize("content,reason,text", [
    (None, "io", "No such file or directory"),
    ("open,high,low,close,timestamp\n1,2,0.5,1.5,1000\n", "columns", ": no column volume"),
    ("open,high,low,close,volume,timestamp\n1,2,0.5,x,10,1000\n", "value",
     " line 2: not a number"),
    ("open,high,low,close,volume,timestamp\n1,2,0.5,1.5,10\n", "value", " line 2: not a number"),
    (b"open,high,low,close,volume,timestamp\n1,2,0.5,1.5,10,1000\xff\n", "value",
     ": not a UTF-8 CSV ("),
    ("open,high,low,close,volume,timestamp\n", "empty", ": no bars"),
    ("", "empty", ": no bars"),
], ids=["missing", "no-volume", "not-a-number", "short-row", "not-utf8", "header-only",
        "empty-file"])
def test_an_unreadable_ohlcv_is_chart_bars_unreadable(harness, tmp_path, content, reason, text):
    path = tmp_path / "bars.csv"
    if isinstance(content, bytes):
        path.write_bytes(content)
    elif content is not None:
        path.write_text(content)
    lib = fake_lib()
    status, out = harness(lib, ohlcv=path)
    doc = line_of(out)
    assert status == 1
    assert (doc["code"], doc["args"]) == ("chart_bars_unreadable", {"reason": reason})
    assert doc["error"].startswith(f"--ohlcv: {path}") and text in doc["error"]
    assert lib.calls == []


def test_load_bars_failures_stay_value_errors(tmp_path):
    with pytest.raises(ValueError):
        run_json.load_bars(tmp_path / "missing.csv")


@pytest.mark.parametrize("cdll,returns,names,expected,text", [
    (OSError("fake.so: cannot open shared object file"), {}, CORE,
     {"reason": "load_failed"},
     "cannot load the strategy library: fake.so: cannot open shared object file"),
    (None, {}, CORE[1:], {"reason": "abi_missing"},
     "strategy .so predates pf_abi_version (ABI v1); rebuild it against the current "
     "pineforge runtime (pf_report_t grew)."),
    (None, {"pf_abi_version": 3}, CORE, {"reason": "abi_mismatch", "abi": 3},
     "pineforge ABI mismatch: .so reports 3, harness expects 4; rebuild."),
    (None, {}, tuple(n for n in CORE if n != "run_backtest_full"),
     {"reason": "symbol_missing", "missing": "run_backtest_full"},
     "the strategy library has no run_backtest_full"),
], ids=["load-failed", "abi-missing", "abi-mismatch", "symbol-missing"])
def test_an_incompatible_library_is_coded(harness, cdll, returns, names, expected, text):
    lib = FakeLib(names, {"pf_abi_version": run_json.EXPECTED_PF_ABI, "strategy_create": ST,
                          **returns})

    def load(path):
        if cdll is not None:
            raise cdll
        return lib
    status, out = harness(lib, cdll=load)
    doc = line_of(out)
    assert status == 1
    assert doc == {"engine": "pineforge", "error": text,
                   "code": "strategy_library_incompatible", "args": expected}


def test_strategy_library_errors_stay_runtime_errors():
    with pytest.raises(RuntimeError):
        run_json.check_abi(FakeLib(("pf_abi_version",), {"pf_abi_version": 3}))


SYMINFO_SETTERS = ("strategy_set_syminfo_metadata", "strategy_set_syminfo_mintick",
                   "strategy_set_syminfo_pointvalue", "strategy_set_syminfo_timezone",
                   "strategy_set_syminfo_session")


@pytest.mark.parametrize("raw,code,args,text", [
    (None, "syminfo_unreadable", {"reason": "io"}, "No such file or directory"),
    ("{not json", "syminfo_unreadable", {"reason": "not_json"}, " is not JSON: "),
    (b'{"mintick": "\xff"}', "syminfo_unreadable", {"reason": "not_json"}, " is not JSON: "),
    ("[1, 2]", "syminfo_unreadable", {"reason": "not_object"},
     ": the syminfo is not a JSON object"),
    ('{"syminfo": 5}', "syminfo_unreadable", {"reason": "not_object"},
     ": the syminfo is not a JSON object"),
    ('{"mintick": "abc"}', "syminfo_unreadable", {"reason": "value_type"},
     'syminfo.mintick must be a number, got "abc"'),
    ('{"pointvalue": null}', "syminfo_unreadable", {"reason": "value_type"},
     "syminfo.pointvalue must be a number, got null"),
    ('{"mintick": 1' + "0" * 400 + "}", "syminfo_unreadable", {"reason": "value_type"},
     "syminfo.mintick must be a number, got 1000"),
    ('{"timezone": "\\ud800"}', "syminfo_unreadable", {"reason": "value_type"},
     'syminfo.timezone must be UTF-8 text, got "\\ud800"'),
    ('{"mincontract": 0}', "lot_grid_rejected", {},
     "syminfo.mincontract must be a positive finite number, got 0"),
], ids=["missing", "not-json", "not-utf8", "list", "wrapped-number", "mintick-text",
        "pointvalue-null", "mintick-huge", "timezone-surrogate", "mincontract"])
def test_an_unusable_syminfo_is_coded_before_any_setter(harness, tmp_path, raw, code, args,
                                                         text):
    path = tmp_path / "syminfo.json"
    if isinstance(raw, bytes):
        path.write_bytes(raw)
    elif raw is not None:
        path.write_text(raw)
    lib = fake_lib(*SYMINFO_SETTERS)
    status, out = harness(lib, "--syminfo", str(path))
    doc = line_of(out)
    assert status == 1
    assert (doc["code"], doc["args"]) == (code, args)
    assert text in doc["error"]
    assert not [n for n in lib.names() if n.startswith("strategy_set_syminfo")]
    assert lib.names().count("strategy_create") == lib.names().count("strategy_free") == 1


def test_a_syminfo_setter_the_library_lacks_is_coded(tmp_path):
    path = tmp_path / "syminfo.json"
    path.write_text('{"mintick": 0.5, "session": "24x7"}')
    lib = FakeLib(SYMINFO_SETTERS[:2] + SYMINFO_SETTERS[3:4])
    with pytest.raises(run_json.SyminfoError) as e:
        run_json.apply_syminfo(lib, ST, path)
    assert str(e.value) == ("the strategy library has no strategy_set_syminfo_session, "
                            "so syminfo.session cannot be applied")
    assert (e.value.code, e.value.code_args) == (
        "strategy_library_incompatible",
        {"reason": "setter_missing", "missing": "strategy_set_syminfo_session"})
    assert catalogued(e.value.code, e.value.code_args)
    assert lib.calls == []


def test_a_bool_mintick_is_still_applied_as_before(tmp_path):
    path = tmp_path / "syminfo.json"
    path.write_text('{"mintick": true, "timezone": 5}')
    lib = FakeLib(SYMINFO_SETTERS)
    assert run_json.apply_syminfo(lib, ST, path) == {}
    assert lib.calls == [("strategy_set_syminfo_mintick", ST, 1.0),
                         ("strategy_set_syminfo_timezone", ST, b"5")]


# Every --symbol-feeds refusal: (index, files) -> its one reason.
GOOD = "timestamp,open,high,low,close\n1000,1,2,0.5,1.5\n"
FEED_CASES = [
    ({"": {"feeds": {}}}, {}, "symbol_text_invalid"),
    ({"E": {"feeds": {"4h": "f.csv"}}}, {"f.csv": GOOD}, "timeframe_invalid"),
    ({"E": {"feeds": {"60": "f.csv"}}}, {"f.csv": "timestamp,open\n1,2\n"},
     "feed_columns_missing"),
    ({"E": {"feeds": {"60": "f.csv"}}}, {"f.csv": GOOD + "x,1,2,0.5,1.5\n"},
     "feed_value_not_number"),
    ({"E": {"feeds": {"60": "f.csv"}}}, {"f.csv": "timestamp,open,high,low,close\n1,inf,1,1,1\n"},
     "feed_value_invalid"),
    ({"E": {"feeds": {"60": "f.csv"}}},
     {"f.csv": f"timestamp,open,high,low,close\n{2**53},1,1,1,1\n"}, "feed_time_out_of_range"),
    ({"E": {"feeds": {"60": "nope.csv"}}}, {}, "feed_unreadable"),
    ({"E": {"feeds": {"60": "f.csv"}}}, {"f.csv": b"timestamp,open,high,low,close\n1,\xe9,1,1,1\n"},
     "feed_not_utf8_csv"),
    ({"E": {"feeds": {"60": "f.csv"}}},
     {"f.csv": "timestamp,open,high,low,close\n200000,1,1,1,1\n100000,1,1,1,1\n"},
     "feed_not_increasing"),
    ({"E": {"feeds": {"60": "f.csv"}}},
     {"f.csv": "timestamp,open,high,low,close,volume,time_close\n1000,1,1,1,1,1,500\n"},
     "feed_close_time_invalid"),
    ({"E": {"syminfo": [], "feeds": {}}}, {}, "syminfo_not_object"),
    ({"E": {"syminfo": {"mintick": 0}, "feeds": {}}}, {}, "syminfo_mintick_invalid"),
    ('{"symbols": {"E": {"feeds": {}}, "E": {"feeds": {}}}}', {}, "index_duplicate_key"),
    (None, {}, "index_unreadable"),
    ("{not json", {}, "index_not_json"),
    ("[]", {}, "index_shape"),
    ({f"S{i}": {"feeds": {}} for i in range(257)}, {}, "too_many_symbols"),
    ({"E": {"feed": {}}}, {}, "entry_shape"),
    ({"E": {"feeds": []}}, {}, "feeds_not_object"),
    ({"S0": {"feeds": {str(i): "f.csv" for i in range(1, 130)}},
      "S1": {"feeds": {str(i): "f.csv" for i in range(1, 129)}}}, {"f.csv": GOOD},
     "too_many_feeds"),
    ({"E": {"feeds": {"D": "f.csv", "1D": "f.csv"}}}, {"f.csv": GOOD}, "duplicate_timeframe"),
    ({"E": {"feeds": {"60": ""}}}, {}, "feed_path_invalid"),
    ({"E": {"feeds": {"60": "f.csv\x00.txt"}}}, {"f.csv": GOOD}, "feed_path_invalid"),
    ({"E": {"feeds": {"60": "\ud800.csv"}}}, {}, "feed_path_invalid"),
]


@pytest.mark.parametrize("symbols,files,reason", FEED_CASES,
                         ids=[f"{c[2]}-{i}" for i, c in enumerate(FEED_CASES)])
def test_every_symbol_feeds_refusal_has_its_reason(tmp_path, symbols, files, reason):
    for name, text in files.items():
        (tmp_path / name).write_bytes(text if isinstance(text, bytes) else text.encode())
    index = tmp_path / "symbols.json"
    if symbols is not None:
        index.write_text(symbols if isinstance(symbols, str) else json.dumps({"symbols": symbols}))
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.load_symbol_feeds(index)
    assert (e.value.code, e.value.code_args) == ("symbol_feeds_refused", {"reason": reason})
    assert catalogued(e.value.code, e.value.code_args)
    assert str(e.value).startswith("--symbol-feeds: ")


def feeds_index(tmp_path):
    (tmp_path / "f.csv").write_text(GOOD)
    index = tmp_path / "symbols.json"
    index.write_text(json.dumps({"symbols": {"X:Y": {"syminfo": {"type": "crypto"},
                                                     "feeds": {"60": "f.csv"}}}}))
    return index


def test_a_library_without_the_feed_setters_is_library_without_symbol_feeds(tmp_path):
    symbols = run_json.load_symbol_feeds(feeds_index(tmp_path))
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.install_symbol_feeds(FakeLib(), ST, symbols)
    assert e.value.code_args == {"reason": "library_without_symbol_feeds"}


def test_an_engine_refusal_without_the_code_getter_is_engine_refused(tmp_path):
    symbols = run_json.load_symbol_feeds(feeds_index(tmp_path))
    lib = FakeLib(("strategy_set_symbol_facts", "strategy_set_symbol_feed",
                   "strategy_get_last_error"),
                  {"strategy_set_symbol_facts": -1, "strategy_get_last_error": b"no"})
    with pytest.raises(run_json.SymbolFeedsError) as e:
        run_json.install_symbol_feeds(lib, ST, symbols)
    assert str(e.value) == "--symbol-feeds: the engine refused the type of X:Y: no"
    assert (e.value.code, e.value.code_args) == ("symbol_feeds_refused",
                                                 {"reason": "engine_refused"})


@pytest.mark.parametrize("code,args,expected", [
    (b"symbol_feeds_refused", b'{"reason":"times_not_increasing"}',
     ("symbol_feeds_refused", {"reason": "times_not_increasing"})),
    (b"out_of_memory", b"{}", ("out_of_memory", {})),
    (b"", b"", ("symbol_feeds_refused", {"reason": "engine_refused"})),
], ids=["engine-reason", "engine-other-code", "engine-names-nothing"])
def test_an_engine_refusal_carries_the_engine_code(harness, tmp_path, code, args, expected):
    lib = fake_lib("strategy_set_symbol_facts", "strategy_set_symbol_feed",
                   "strategy_get_last_error", "strategy_get_last_error_code",
                   "strategy_get_last_error_args",
                   returns={"strategy_set_symbol_facts": 0, "strategy_set_symbol_feed": -1,
                            "strategy_get_last_error": b"strategy_set_symbol_feed: x",
                            "strategy_get_last_error_code": code,
                            "strategy_get_last_error_args": args})
    status, out = harness(lib, "--symbol-feeds", str(feeds_index(tmp_path)))
    doc = line_of(out)
    assert status == 1
    assert doc["error"] == ("--symbol-feeds: the engine refused the feed X:Y@60: "
                            "strategy_set_symbol_feed: x")
    assert (doc["code"], doc["args"]) == expected
    assert "run_backtest_full" not in lib.names()
    assert lib.names().count("strategy_create") == lib.names().count("strategy_free") == 1


def test_an_unexpected_exception_is_harness_internal_error(harness, monkeypatch):
    def broken(*args, **kwargs):
        raise KeyError("trades")
    monkeypatch.setattr(run_json, "build_report_dict", broken)
    lib = fake_lib()
    status, out = harness(lib)
    assert status == 1
    assert line_of(out) == {"engine": "pineforge",
                            "error": "harness internal error: KeyError: 'trades'",
                            "code": "harness_internal_error", "args": {}}
    assert "Traceback" in harness.stderr
    assert lib.names().count("strategy_create") == lib.names().count("strategy_free") == 1


# --- checked settings ----------------------------------------------------------

RECEIPT = {"version": 1,
           "inputs": [{"name": "Length"}, {"name": "Threshold"}, {"name": "Twice"},
                      {"name": "Twice"}],
           "overrides": [{"name": "initial_capital"}]}


def checked_lib(*extra, version=1, create=(PF_OK, b""), inputs=None, overrides=None,
                receipt=RECEIPT, returns=None, drop=()):
    """A library exporting the checked settings API, without the exports named in
    drop. inputs / overrides map a key to the (status, message) its checked
    setter returns; any other key is OK."""
    def create_checked(params, out, error, capacity):
        status, message = create
        if status == PF_OK:
            out._obj.value = ST
        error.value = message
        return status

    def setter(table):
        def set_checked(strat, key, value, error, capacity):
            status, message = (table or {}).get(key.decode(), (PF_OK, b""))
            error.value = message
            return status
        return set_checked

    def effective(strat, json_buffer, capacity, required, error, error_capacity):
        doc = json.dumps(receipt).encode()
        required._obj.value = len(doc) + 1
        if json_buffer is None or capacity < len(doc) + 1:
            return PF_SMALL
        json_buffer.value = doc
        return PF_OK

    lib = fake_lib(*extra, returns=returns, impl={
        "strategy_settings_api_version": lambda: version,
        "strategy_create_checked": create_checked,
        "strategy_set_input_checked": setter(inputs),
        "strategy_set_override_checked": setter(overrides),
        "strategy_get_effective_settings": effective,
    })
    for name in drop:
        delattr(lib, name)
    return lib


def settings_args(**kw):
    return [a for k, v in kw.items() for a in (f"--{k}", json.dumps(v))]


def test_valid_settings_go_through_the_checked_api(harness):
    _, legacy = harness(fake_lib(), *settings_args(inputs={"Length": "5"},
                                                   overrides={"initial_capital": "20000"}))
    lib = checked_lib()
    status, out = harness(lib, *settings_args(inputs={"Length": "5"},
                                              overrides={"initial_capital": "20000"}))
    assert status == 0
    assert report_of(out) == report_of(legacy)
    names = lib.names()
    assert "strategy_create" not in names and "strategy_set_input" not in names
    assert [c[:4] for c in lib.calls if c[0] in ("strategy_set_input_checked",
                                                 "strategy_set_override_checked")] \
        == [("strategy_set_input_checked", ST, b"Length", b"5"),
            ("strategy_set_override_checked", ST, b"initial_capital", b"20000")]
    assert names.index("strategy_set_override_checked") < names.index("run_backtest_full")


def test_a_library_with_none_of_the_checked_api_keeps_the_legacy_setters(harness):
    lib = fake_lib()
    status, _ = harness(lib, *settings_args(inputs={"Length": "5"}))
    assert status == 0
    assert ("strategy_set_input", ST, b"Length", b"5") in lib.calls
    assert "strategy_set_input_checked" not in lib.names()


CHECKED_EXPORTS = ("strategy_settings_api_version", "strategy_create_checked",
                   "strategy_set_input_checked", "strategy_set_override_checked")


@pytest.mark.parametrize("version,drop", [
    (2, ()), (0, ()),
    *[(1, (name,)) for name in CHECKED_EXPORTS],
    (1, CHECKED_EXPORTS[1:]),
    (2, CHECKED_EXPORTS[1:]),
], ids=["version-2", "version-0", "no-version", "no-create", "no-set-input", "no-set-override",
        "version-only", "version-2-only"])
def test_another_checked_api_version_or_a_partial_set_is_refused(harness, version, drop):
    lib = checked_lib(version=version, drop=drop)
    status, out = harness(lib, *settings_args(inputs={"Length": "5"},
                                              overrides={"pyramiding": "2"}))
    assert status == 1
    assert line_of(out) == {
        "engine": "pineforge",
        "error": "checked settings API mismatch: the strategy library must export "
                 "strategy_settings_api_version() == 1, strategy_create_checked, "
                 "strategy_set_input_checked and strategy_set_override_checked, or none of "
                 "them; rebuild.",
        "code": "strategy_library_incompatible", "args": {"reason": "settings_api_mismatch"}}
    assert [n for n in lib.names() if n not in ("pf_abi_version",
                                                "strategy_settings_api_version")] == []


# The checked setters' messages (checked_settings.hpp, the generated setters) and
# the setting_rejected reason each one is.
EXPECTED_REASONS = [
    ("expected an integer", "expected_integer"),
    ("invalid integer exponent", "invalid_integer_exponent"),
    ("invalid integer or trailing bytes", "invalid_integer_or_trailing_bytes"),
    ("expected an integral value", "expected_integral_value"),
    ("integer out of range", "integer_out_of_range"),
    ("invalid integer sign", "invalid_integer_sign"),
    ("expected a finite decimal number", "expected_finite_decimal"),
    ("invalid numeric exponent", "invalid_numeric_exponent"),
    ("invalid number or trailing bytes", "invalid_number_or_trailing_bytes"),
    ("number out of finite range", "number_out_of_finite_range"),
    ("number underflows to zero", "number_underflows_to_zero"),
    ("number cannot be consumed by the strategy getter", "number_not_consumable"),
    ("invalid boolean", "invalid_boolean"),
    ("invalid enum option", "invalid_enum_option"),
    ("value below minimum", "value_below_minimum"),
    ("value above maximum", "value_above_maximum"),
    ("invalid input option", "invalid_input_option"),
    ("unknown input key", "unknown_key"),
    ("unknown override key", "unknown_key"),
    ("ambiguous input key", "ambiguous_key"),
]


def test_the_mapping_is_exactly_the_pinned_table():
    assert run_json.SETTING_REJECTED_REASONS == dict(EXPECTED_REASONS)


@pytest.mark.parametrize("message,reason", EXPECTED_REASONS)
def test_each_checked_message_maps_to_its_reason(harness, message, reason):
    status_code = PF_UNSUPPORTED if message == "ambiguous input key" else PF_INVALID
    lib = checked_lib(inputs={"Nope": (status_code, message.encode())})
    status, out = harness(lib, *settings_args(inputs={"Nope": "x"}))
    doc = line_of(out)
    assert status == 1
    assert doc["error"] == f"strategy_set_input: {message}"
    assert (doc["code"], doc["args"]) == (
        "setting_rejected", {"entrypoint": "strategy_set_input", "reason": reason})
    assert "run_backtest_full" not in lib.names()
    assert lib.names().count("strategy_create_checked") == lib.names().count("strategy_free") == 1


def test_a_rejected_input_title_the_receipt_lists_is_named(harness):
    lib = checked_lib(inputs={"Length": (PF_INVALID, b"invalid integer or trailing bytes")})
    status, out = harness(lib, *settings_args(inputs={"Length": "abc"}))
    assert line_of(out)["args"] == {"entrypoint": "strategy_set_input",
                                    "reason": "invalid_integer_or_trailing_bytes",
                                    "input": "Length"}
    lib = checked_lib(inputs={"Twice": (PF_UNSUPPORTED, b"ambiguous input key")})
    status, out = harness(lib, *settings_args(inputs={"Twice": "1"}))
    assert line_of(out)["args"] == {"entrypoint": "strategy_set_input",
                                    "reason": "ambiguous_key", "input": "Twice"}


def test_an_unknown_input_key_names_no_input(harness):
    lib = checked_lib(inputs={"Lenght": (PF_INVALID, b"unknown input key")})
    status, out = harness(lib, *settings_args(inputs={"Lenght": "5"}))
    assert line_of(out)["args"] == {"entrypoint": "strategy_set_input", "reason": "unknown_key"}


def test_without_a_receipt_no_input_is_named(harness):
    lib = checked_lib(inputs={"Length": (PF_INVALID, b"expected an integer")}, receipt=[1])
    status, out = harness(lib, *settings_args(inputs={"Length": ""}))
    assert line_of(out)["args"] == {"entrypoint": "strategy_set_input",
                                    "reason": "expected_integer"}


@pytest.mark.parametrize("key,value,message,reason", [
    ("bogus", "1", b"unknown override key", "unknown_key"),
    ("default_qty_type", "shares", b"invalid input option", "invalid_input_option"),
    ("commission_type", "bogus", b"invalid input option", "invalid_input_option"),
    ("initial_capital", "-5", b"value below minimum", "value_below_minimum"),
], ids=["unknown-key", "qty-type", "commission-type", "negative-capital"])
def test_a_rejected_override_is_setting_rejected(harness, key, value, message, reason):
    lib = checked_lib(overrides={key: (PF_INVALID, message)})
    status, out = harness(lib, *settings_args(overrides={key: value}))
    doc = line_of(out)
    assert status == 1
    assert doc["error"] == "strategy_set_override: " + message.decode()
    assert (doc["code"], doc["args"]) == (
        "setting_rejected", {"entrypoint": "strategy_set_override", "reason": reason})
    assert "run_backtest_full" not in lib.names()


@pytest.mark.parametrize("status_code,message,code", [
    (PF_UNSUPPORTED, b"input cannot be honoured by this compiled strategy", "setting_unsupported"),
    (PF_UNSUPPORTED, b"some future refusal", "setting_unsupported"),
    (PF_UNSUPPORTED, b"settings are frozen after execution begins", "engine_invariant"),
    (PF_UNSUPPORTED, b"input was not installed", "engine_invariant"),
    (PF_INVALID, b"null strategy, key or value", "engine_invariant"),
    (PF_EXCEPTION, b"std::bad_alloc", "engine_unclassified_error"),
    (PF_EXCEPTION, b"unknown input key", "engine_unclassified_error"),
    (PF_RUN_FAILED, b"strategy_set_override: stod", "engine_unclassified_error"),
    (9, b"", "engine_unclassified_error"),
], ids=["unsupported", "unsupported-unknown", "frozen", "not-installed", "null-args",
        "exception", "exception-known-text", "latched", "unknown-status"])
def test_other_checked_outcomes(harness, status_code, message, code):
    lib = checked_lib(inputs={"Length": (status_code, message)})
    status, out = harness(lib, *settings_args(inputs={"Length": "5"}))
    doc = line_of(out)
    assert status == 1
    assert doc == {"engine": "pineforge", "error": "strategy_set_input: " + message.decode(),
                   "code": code, "args": {}}


def test_an_unknown_invalid_argument_message_is_unparseable_value(harness):
    lib = checked_lib(inputs={"Length": (PF_INVALID, b"a message from a newer codegen")})
    status, out = harness(lib, *settings_args(inputs={"Length": "5"}))
    assert line_of(out)["args"] == {"entrypoint": "strategy_set_input",
                                    "reason": "unparseable_value", "input": "Length"}


def test_the_first_rejected_setting_stops_the_rest(harness):
    lib = checked_lib(inputs={"A": (PF_INVALID, b"invalid boolean")})
    status, _ = harness(lib, *settings_args(inputs={"A": "maybe", "B": "1"},
                                            overrides={"pyramiding": "2"}))
    assert status == 1
    assert [c[2] for c in lib.calls if c[0] == "strategy_set_input_checked"] == [b"A"]
    assert "strategy_set_override_checked" not in lib.names()


@pytest.mark.parametrize("create,text", [
    ((PF_EXCEPTION, b"std::bad_alloc"), "strategy_create failed: std::bad_alloc"),
    ((PF_UNSUPPORTED, b"params_json is reserved; use checked setters"),
     "strategy_create failed: params_json is reserved; use checked setters"),
    ((PF_EXCEPTION, b""), "strategy_create failed"),
], ids=["bad-alloc", "params", "no-message"])
def test_a_failed_checked_create_is_strategy_create_failed(harness, create, text):
    lib = checked_lib(create=create)
    status, out = harness(lib, *settings_args(inputs={"Length": "5"}))
    assert status == 1
    assert line_of(out) == {"engine": "pineforge", "error": text,
                            "code": "strategy_create_failed", "args": {}}
    assert [n for n in lib.names() if "set_" in n] == []
    create_call = next(c for c in lib.calls if c[0] == "strategy_create_checked")
    assert create_call[1] is None  # params_json is reserved: NULL


def test_every_own_code_is_catalogued():
    for code, args in [
            ("setting_rejected", {"entrypoint": e, "reason": r})
            for e in ("strategy_set_input", "strategy_set_override")
            for r in [*dict(EXPECTED_REASONS).values(), "unparseable_value"]]:
        assert catalogued(code, args)
    for option in run_json._RUN_REQUEST_OPTIONS | {"arguments"}:
        assert catalogued("run_request_invalid", {"option": option})
    for name in run_json._REQUIRED_EXPORTS:
        assert catalogued("strategy_library_incompatible",
                          {"reason": "symbol_missing", "missing": name})
    assert catalogued("strategy_library_incompatible", {"reason": "settings_api_mismatch"})
    assert catalogued("chart_bars_unreadable", {"reason": "value"})
    for code in ("strategy_create_failed", "setting_unsupported", "engine_invariant",
                 "engine_unclassified_error", "harness_internal_error", "lot_grid_rejected"):
        assert catalogued(code, {})


@pytest.mark.parametrize("spelling,native", [
    (r'path\\length', 'path\\length'),
    (r'armed \"yes\"', 'armed "yes"'),
    ('倍数\\\\ \\"quoted\\"', '倍数\\ "quoted"'),
    (r'caf\xc3\xa9', 'café'),
    (r'\u500d\u6570', '倍数'),
    (r'name\000suffix', 'name'),
    (r'name\x00suffix', 'name'),
    (r'\141\142\143', 'abc'),
])
def test_legacy_native_literal_identity(spelling, native):
    assert run_json._release_cpp_input_name(spelling) == native


@pytest.mark.parametrize("spelling", [r'bad\q', 'bad\\', r'bad\x100', r'bad\377'])
def test_legacy_unknown_literal_encoding_is_not_guessed(spelling):
    with pytest.raises(ValueError):
        run_json._release_cpp_input_name(spelling)


@pytest.mark.parametrize("definition", [
    'const int Side__long_ = 01;',
    'const int Side__long_ = 1 + 1;',
    'const int Side__long_ = 4294967297;',
    'const int Side__long_ = 1;\nconst int Side__long_ = 2;',
])
def test_legacy_numeric_default_requires_one_supported_literal(definition):
    with pytest.raises(ValueError):
        run_json._release_legacy_declarations(
            definition + '\nget_input_int("Side", Side__long_);')


@pytest.mark.parametrize("definition,reason", [
    ('const int Side__long_ = 1;\nclass Local { int Side__long_ = 2; };', "ambiguous_binding"),
    ('const int Side__long_ = 1;\nvoid f(int Side__long_);', "ambiguous_binding"),
    ('const int Side__long_ = 1 + 1;', "unsupported_default"),
    ('const int Side__long_ = 1;', "receipt_unavailable"),
])
def test_unproved_legacy_defaults_remain_in_the_fingerprint(tmp_path, definition, reason):
    inputs = {"Side": "2"}
    document, cpp = release_provenance(tmp_path, inputs)
    cpp += '\n' + definition + '\nget_input_int("Side", Side__long_);'
    result = run_json.normalize_release_provenance(document, cpp, None, False)
    row = result["inputs"]["Side"]
    assert row["default"] is None and row["value"] is None
    assert row["resolution"] == {"status": "unresolved", "reason": reason,
                                 "raw_default": "Side__long_"}
    assert result["applied"]["inputs"]["Side"] == "2"
    assert inputs == {"Side": "2"}
    fp = run_json.build_fingerprint(result)
    raw = run_json.base64.b64decode(fp["token"])
    assert json.loads(raw) == result
    assert fp["digest"] == "sha256:" + run_json.hashlib.sha256(raw).hexdigest()


# --- Positive allowlist: every relied-on token in a recognized form ---------

ENUM_TU = '''const int Side__long_ = 1;
class GeneratedStrategy {
    GeneratedStrategy() {
        pineforge::source::PineStrategyConfig cfg{};
        cfg.default_qty_type = static_cast<int>(QtyType::CASH);
        configure_pine_strategy(cfg);
    }
    void init() {
        side = get_input_int("Side", Side__long_);
        len = get_input_int("len", 10);
        src = get_input_source("Source", _src_close_)[0];
        _src_close_.clear();
    }
#ifdef PF_SETTINGS_API_VERSION
    std::vector<int> rows() const {
        return {
            {"Side", "enum", ::pineforge::checked_settings::number(Side__long_), {}},
        };
    }
#endif
};
'''


def receipt_row(name, kind, default, value=None, supported=True):
    return {"name": name, "type": kind, "kind": kind, "default": default,
            "effective_value": default if value is None else value,
            "supported": supported}


def legacy_receipt(*rows):
    return {"version": 1, "inputs": list(rows), "overrides": []}


ENUM_RECEIPT = legacy_receipt(receipt_row("Side", "enum", "1"), receipt_row("len", "int", "10"),
                              receipt_row("Source", "source", "close"))


def legacy_document(tmp_path, cpp, receipt=None, inputs=None, overrides=None):
    path = tmp_path / "generated.cpp"
    path.write_text(cpp)
    inputs = dict(inputs or {})
    overrides = dict(overrides or {})
    document = run_json.build_provenance(
        {}, path, True, inputs, overrides, {}, source_feed_sha256="0" * 64)
    result = run_json.normalize_release_provenance(document, cpp, receipt, False)
    fingerprint = run_json.build_fingerprint(result)
    raw = run_json.base64.b64decode(fingerprint["token"])
    assert json.loads(raw) == result
    return result


def refusal(reason, raw):
    return {"status": "unresolved", "reason": reason, "raw_default": raw}


def duplicate_title(raw, distinct):
    return dict(refusal("duplicate_title", raw), distinct_native_inputs=distinct)


def test_allowlist_certifies_only_recognized_symbol_occurrences(tmp_path):
    result = legacy_document(tmp_path, ENUM_TU, ENUM_RECEIPT)
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}
    assert result["inputs"]["Source"] == {"type": "source", "default": "close", "value": "close"}
    assert result["strategy"]["default_qty_type"] == "cash"


@pytest.mark.parametrize("extra", [
    "            int unused = 0, Side__long_(2);\n",
    "            int ((Side__long_)) = 2;\n",
    "            bool same = side == (Side__long_);\n",
    "            Side__long_;\n",
])
def test_any_other_symbol_occurrence_refuses(tmp_path, extra):
    cpp = ENUM_TU.replace("        side = get_input_int", extra + "        side = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT, {"Side": "2"})
    assert result["inputs"]["Side"]["default"] is None
    assert result["inputs"]["Side"]["value"] is None
    assert result["inputs"]["Side"]["resolution"] == refusal("ambiguous_binding", "Side__long_")
    assert result["applied"]["inputs"]["Side"] == "2"
    assert result["inputs"]["len"]["default"] == 10


@pytest.mark.parametrize("extra", [
    "            int \\u0053ide__long_ = 2;\n",
    "            int x = 1; \\\n",
    "// comment \\\n",
    "#define Side__long_ 2\n",
    "#define get_input_int(a, b) 7\n",
    '#include "local.h"\n',
    "#pragma once\n",
    "%:define true false\n",
    "#include </in/strategy_header.h>\n",
    "#include <pineforge/../../in/header.hpp>\n",
    "int table<:2:> = <%1, 2%>;\n",
])
def test_unrecognized_lexical_context_refuses_every_row(tmp_path, extra):
    cpp = extra + ENUM_TU
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT, {"len": "14"})
    for name in ("Side", "len", "Source"):
        assert result["inputs"][name]["resolution"]["reason"] == "unsupported_binding"
        assert result["inputs"][name]["value"] is None
    assert result["applied"]["inputs"]["len"] == "14"
    assert result["strategy"]["default_qty_type"] is None
    assert result["strategy_resolution"]["default_qty_type"]["reason"] == "unsupported_binding"


def test_conflicting_getter_occurrences_are_arbitrated_by_the_receipt(tmp_path):
    cpp = ENUM_TU.replace("    void init() {", '''    int unused_probe() { return get_input_int ("len", 1); }
    void init() {''')
    result = legacy_document(tmp_path, cpp, None, {"len": "2"})
    assert result["inputs"]["len"]["resolution"] == refusal("ambiguous_binding", "1")
    assert result["applied"]["inputs"]["len"] == "2"
    receipt = legacy_receipt(receipt_row("Side", "enum", "1"), receipt_row("len", "int", "10", "2"),
                             receipt_row("Source", "source", "close"))
    result = legacy_document(tmp_path, cpp, receipt, {"len": "2"})
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 2}
    assert result["applied"]["inputs"]["len"] == 2


def test_receipt_type_default_and_value_are_compared_without_coercion(tmp_path):
    cpp = ENUM_TU.replace('get_input_int("len", 10)', 'get_input_int("len", 2)')
    receipt = legacy_receipt(receipt_row("Side", "enum", "1"), receipt_row("len", "float", "2"),
                             receipt_row("Source", "source", "close"))
    result = legacy_document(tmp_path, cpp, receipt, {"len": "2"})
    assert result["inputs"]["len"]["resolution"] == refusal("ambiguous_binding", "2")
    receipt["inputs"][1] = receipt_row("len", "int", "3", "2")
    result = legacy_document(tmp_path, cpp, receipt, {"len": "2"})
    assert result["inputs"]["len"]["resolution"] == refusal("ambiguous_binding", "2")
    receipt["inputs"][1] = receipt_row("len", "int", "2", "2")
    result = legacy_document(tmp_path, cpp, receipt, {"len": "2"})
    assert result["inputs"]["len"] == {"type": "int", "default": 2, "value": 2}


@pytest.mark.parametrize("context", [
    'static const char* fake = R"(");get_input_int("ghost", 7);(")";\n',
    'static const char* fake = R"x(get_input_int("ghost", 7))x";\n',
    '// get_input_int("ghost", 7);\n',
    '/* get_input_int("ghost", 7); */\n',
])
def test_getter_text_outside_code_context_is_not_a_declaration(tmp_path, context):
    result = legacy_document(tmp_path, context + ENUM_TU, ENUM_RECEIPT, {"ghost": "9"})
    assert result["inputs"]["ghost"] == {"type": "unknown", "default": None, "value": "9"}
    assert result["inputs"]["Side"]["default"] == 1


@pytest.mark.parametrize("guard", ["#if 0", "#ifdef SOMETHING", "#ifndef PF_SETTINGS_API_VERSION"])
def test_getter_in_an_unrecognized_conditional_refuses_every_row(tmp_path, guard):
    cpp = ENUM_TU + guard + '\nint ghost = get_input_int("ghost", 7);\n#endif\n'
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"]["reason"] == "unsupported_binding"


@pytest.mark.parametrize("inputs,expected", [
    ({}, {"type": "source", "default": "close", "value": "close"}),
    ({"Source": "high"}, {"type": "source", "default": "close", "value": "high"}),
    ({"Source": "14"}, {"type": "source", "default": None, "value": None,
                        "resolution": refusal("unsupported_override", "_src_close_")}),
    ({"Source": ""}, {"type": "source", "default": None, "value": None,
                      "resolution": refusal("unsupported_override", "_src_close_")}),
])
def test_source_inputs_certify_only_canonical_selectors(tmp_path, inputs, expected):
    receipt = legacy_receipt(receipt_row("Side", "enum", "1"), receipt_row("len", "int", "10"),
                             receipt_row("Source", "source", "close", inputs.get("Source")))
    result = legacy_document(tmp_path, ENUM_TU, receipt, inputs)
    assert result["inputs"]["Source"] == expected
    if inputs:
        applied = result["applied"]["inputs"]["Source"]
        assert applied == (expected["value"] if expected["value"] else inputs["Source"])


@pytest.mark.parametrize("default,reason", [
    ('source_series(std::string("close"))', "unsupported_default"),
    ("close", "unsupported_default"),
    ("_src_typo_", "unsupported_default"),
])
def test_source_expressions_are_unresolved(tmp_path, default, reason):
    cpp = ENUM_TU.replace('get_input_source("Source", _src_close_)',
                          'get_input_source("Source", ' + default + ')')
    result = legacy_document(tmp_path, cpp, None)
    assert result["inputs"]["Source"]["resolution"] == refusal(reason, default)


def test_redeclared_source_series_is_unresolved(tmp_path):
    cpp = ENUM_TU.replace("    void init() {", "    Series<double> _src_close_;\n    void init() {")
    result = legacy_document(tmp_path, cpp, None)
    assert result["inputs"]["Source"]["resolution"] == refusal("unsupported_binding", "_src_close_")


@pytest.mark.parametrize("getter,receipt", [
    ('get_input_int64("Big", 9007199254740992LL)', receipt_row("Big", "int", "9007199254740992")),
    ('get_input_int64("Big", 1)', receipt_row("Big", "int", "1", "9007199254740993")),
    ('get_input_double("Big", 1.0f)', receipt_row("Big", "float", "inf")),
    ('get_input_double("Big", 1.0f)', receipt_row("Big", "float", "na")),
    ('get_input_double("Big", 1e400)', None),
    ('get_input_int64("Big", 9007199254740993)', None),
    # Zero-padded integer receipt text is read by its own type (cont1 finding 4).
    ('get_input_int64("Big", 9007199254740992LL)', receipt_row("Big", "int", "09007199254740992")),
])
def test_known_out_of_domain_scalars_keep_their_refusal(tmp_path, getter, receipt):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);', 'len = ' + getter + ';')
    rows = [row for row in ENUM_RECEIPT["inputs"] if row["name"] != "len"]
    path = tmp_path / "generated.cpp"
    path.write_text(cpp)
    document = run_json.build_provenance(
        {}, path, True, {}, {}, {}, source_feed_sha256="0" * 64)
    with pytest.raises(ValueError):
        run_json.normalize_release_provenance(
            document, cpp, legacy_receipt(*(rows + ([receipt] if receipt else []))), False)


def test_conflicting_getters_do_not_erase_out_of_domain_evidence(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int64("len", 9007199254740993); '
                          'int other = get_input_int64("len", 1);')
    path = tmp_path / "generated.cpp"
    path.write_text(cpp)
    document = run_json.build_provenance(
        {}, path, True, {}, {}, {}, source_feed_sha256="0" * 64)
    with pytest.raises(ValueError):
        run_json.normalize_release_provenance(document, cpp, None, False)


@pytest.mark.parametrize("literal", ['R"(a"b)"', 'R"x()" )x"', 'u8R"(q"q)"', 'LR"--(a)"b)--"'])
def test_valid_raw_literals_keep_typed_rows_and_the_fingerprint(tmp_path, literal):
    cpp = ENUM_TU + "const char* note = " + literal + ";\n"
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}


@pytest.mark.parametrize("tail", ['const char* note = R"(never closed;\n',
                                  'const char* note = "never closed;\n',
                                  "/* never closed\n", "#if 1\n"])
def test_unreadable_literal_context_is_explicitly_unsupported(tmp_path, tail):
    result = legacy_document(tmp_path, ENUM_TU + tail, ENUM_RECEIPT, {"len": "14"})
    assert result["inputs"]["len"] == {"type": "unknown", "default": None, "value": None,
                                       "resolution": refusal("unsupported_binding", None)}
    assert result["applied"]["inputs"]["len"] == "14"
    assert all(value is None for value in (result["strategy"][key] for key in
                                           run_json._RELEASE_OVERRIDE_TYPES))


@pytest.mark.parametrize("assignment,key,raw", [
    ("cfg.default_qty_type = q;", "default_qty_type", "q"),
    ("cfg.commission_type = c;", "commission_type", "c"),
    ("cfg.default_qty_type = static_cast<int>(QtyType::OTHER);", "default_qty_type",
     "static_cast<int>(QtyType::OTHER)"),
    ("cfg.close_entries_rule_any = 1;", "close_entries_rule", "1"),
    ("cfg.close_entries_rule_any = (true);", "close_entries_rule", "(true)"),
    ("cfg.process_orders_on_close = enabled;", "process_orders_on_close", "enabled"),
    ("cfg.initial_capital = 1000 * 2;", "initial_capital", "1000 * 2"),
])
def test_unrecognized_strategy_defaults_are_unresolved(tmp_path, assignment, key, raw):
    cpp = ENUM_TU.replace("        configure_pine_strategy(cfg);",
                          "        " + assignment + "\n        configure_pine_strategy(cfg);")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["strategy"][key] is None
    assert result["strategy_resolution"][key] == refusal("unsupported_default", raw)
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT, overrides={key: "1"})
    assert result["strategy"][key] is not None
    assert key not in result.get("strategy_resolution", {})


@pytest.mark.parametrize("change", [
    ("        configure_pine_strategy(cfg);",
     "        configure_pine_strategy(cfg);\n        cfg.pyramiding = 3;"),
    ("    void init() {", "    void init() { enum class QtyType { CASH }; }\n    void other() {"),
    ("        cfg.default_qty_type", "        if (side) cfg.pyramiding = 2;\n        cfg.default_qty_type"),
    ("    void init() {", "    void configure_pine_strategy(int) {}\n    void init() {"),
    ("    void init() {", "    struct PineStrategyConfig { int pyramiding; };\n    void init() {"),
    ("        configure_pine_strategy(cfg);", "        set_strategy_override(\"pyramiding\", \"3\");\n"
     "        configure_pine_strategy(cfg);"),
])
def test_unrecognized_strategy_flow_refuses_every_default(tmp_path, change):
    cpp = ENUM_TU.replace(*change)
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["strategy"]["default_qty_type"] is None
    assert "default_qty_type" in result["strategy_resolution"]


def test_recognized_strategy_defaults_and_enum_spellings(tmp_path):
    cpp = ENUM_TU.replace("static_cast<int>(QtyType::CASH)", "QtyType::PERCENT_OF_EQUITY").replace(
        "        configure_pine_strategy(cfg);",
        "        cfg.commission_type = 2;\n        cfg.close_entries_rule_any = true;\n"
        "        cfg.initial_capital = 5e3;\n        configure_pine_strategy(cfg);")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["strategy"]["default_qty_type"] == "percent_of_equity"
    assert result["strategy"]["commission_type"] == "cash_per_contract"
    assert result["strategy"]["close_entries_rule"] == "ANY"
    assert result["strategy"]["initial_capital"] == 5000.0
    assert "strategy_resolution" not in result


@pytest.mark.parametrize("declaration", [
    '            Probe unused, get_input_int("ghost", 5);\n',
    '            Probe (get_input_int("ghost", 5));\n',
    '            ns::Probe ((get_input_int("ghost", 5)));\n',
    '            std::vector<int> get_input_int("ghost", 5);\n',
    '            Probe<int> (get_input_int("ghost", 5));\n',
])
def test_getter_spellings_that_can_declare_a_shadow_refuse_every_row(tmp_path, declaration):
    cpp = ENUM_TU.replace("        side = get_input_int", declaration + "        side = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    for name in ("Side", "len", "Source"):
        assert result["inputs"][name]["resolution"]["reason"] == "unsupported_binding"


def test_getter_arguments_inside_calls_stay_recognized(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = std::max(1, get_input_int("len", 10)); '
                          'if (get_input_int("len", 10) > 0) len = ta::max(get_input_int("len", 10));')
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}



# --- TOP's refined rule 1 and dispositions 1-3 ---------------------------------

READS = ("            bool hit = ([&]{ auto _pna_l = (side); auto _pna_r = (Side__long_); "
         "return _pna_l == _pna_r; }());\n"
         "            if (__switch_val_0 == Side__long_) { hit = true; }\n")


def with_reads(extra=""):
    return ENUM_TU.replace("        len = get_input_int", READS + extra + "        len = get_input_int")


def test_producer_read_positions_keep_the_symbolic_default_typed(tmp_path):
    result = legacy_document(tmp_path, with_reads(), ENUM_RECEIPT)
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}
    receipt = legacy_receipt(receipt_row("Side", "enum", "1", "2"), *ENUM_RECEIPT["inputs"][1:])
    result = legacy_document(tmp_path, with_reads(), receipt, {"Side": "2"})
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 2}
    assert result["applied"]["inputs"]["Side"] == 2


@pytest.mark.parametrize("attack,reason", [
    ("            int unused = 0, Side__long_(2);\n", "ambiguous_binding"),
    ("            int ((Side__long_)) = 2;\n", "ambiguous_binding"),
    ("            int \\u0053ide__long_ = 2;\n", "unsupported_binding"),
    ("            int spliced = 1; \\\n", "unsupported_binding"),
    ("            auto _pna_r = (Side__long_) + 1;\n", "ambiguous_binding"),
    ("            if (other == Side__long_) { hit = true; }\n", "ambiguous_binding"),
    ("            if (__switch_val_0 == Side__long_ + 1) { hit = true; }\n", "ambiguous_binding"),
])
def test_declarator_and_spelling_attacks_still_refuse_with_reads_admitted(tmp_path, attack, reason):
    result = legacy_document(tmp_path, with_reads(attack), ENUM_RECEIPT)
    assert result["inputs"]["Side"]["resolution"] == refusal(reason, "Side__long_")


SECURITY_ARM = (
    "#ifdef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1\n"
    "        {\n"
    "            const std::string _pf_symbol = get_input_string(\"Sym\", std::string(\"X:Y\"));\n"
    "        }\n"
    "#else\n"
    "        ELSE_ARM\n"
    "#endif\n")


def with_security(else_arm="_pf_sec_missing_0 = true;", guard=None):
    arm = SECURITY_ARM.replace("ELSE_ARM", else_arm)
    if guard:
        arm = arm.replace("PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1", guard)
    return ENUM_TU.replace("    void init() {\n", "    void init() {\n" + arm)


def sym_receipt(default):
    return legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("Sym", "string", default))


def test_security_guard_getter_is_certified_through_the_receipt(tmp_path):
    result = legacy_document(tmp_path, with_security(), sym_receipt("X:Y"))
    assert result["inputs"]["Sym"] == {"type": "string", "default": "X:Y", "value": "X:Y"}
    assert result["inputs"]["len"]["default"] == 10
    result = legacy_document(tmp_path, with_security(), None)
    assert result["inputs"]["Sym"]["resolution"] == refusal(
        "ambiguous_binding", 'std::string("X:Y")')


def test_security_guard_branches_that_agree_need_no_receipt(tmp_path):
    same = 'const std::string _pf_symbol = get_input_string("Sym", std::string("X:Y"));'
    result = legacy_document(tmp_path, with_security(same), None)
    assert result["inputs"]["Sym"]["default"] == "X:Y"


@pytest.mark.parametrize("native,expected", [("A:B", "A:B"), ("X:Y", "X:Y"), ("Q", None)])
def test_security_guard_branches_that_differ_take_the_receipt_or_refuse(tmp_path, native, expected):
    other = 'const std::string _pf_symbol = get_input_string("Sym", std::string("A:B"));'
    result = legacy_document(tmp_path, with_security(other), sym_receipt(native))
    assert result["inputs"]["Sym"]["default"] == expected


def test_other_conditionals_around_a_getter_still_refuse(tmp_path):
    result = legacy_document(tmp_path, with_security(guard="SOME_OTHER_GUARD"), sym_receipt("X:Y"))
    for name in ("Side", "len"):
        assert result["inputs"][name]["resolution"]["reason"] == "unsupported_binding"
    assert "Sym" not in result["inputs"]


def test_producer_cast_before_a_getter_is_read(tmp_path):
    cpp = ENUM_TU.replace("        len = get_input_int",
                          '        n = (int)get_input_string("Name", std::string("ab")).size();\n'
                          "        len = get_input_int")
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("Name", "string", "ab"))
    result = legacy_document(tmp_path, cpp, receipt)
    assert result["inputs"]["Name"] == {"type": "string", "default": "ab", "value": "ab"}
    assert result["inputs"]["len"]["default"] == 10


@pytest.mark.parametrize("spelling", [
    "(side)get_input_int(\"len\", 10)",
    "(Probe)get_input_int(\"len\", 10)",
    "(float)get_input_int(\"len\", 10)",
    "int(get_input_int(\"len\", 10))",
    "static_cast<int>(get_input_int(\"len\", 10))",
])
def test_casts_off_the_producer_list_refuse_every_row(tmp_path, spelling):
    cpp = ENUM_TU.replace("        len = get_input_int",
                          "        n = " + spelling + ";\n        len = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"]["reason"] == "unsupported_binding"


def receipt_with_mult(kind, default):
    return legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("mult", kind, default))


REVIEW_WITNESS = ENUM_TU.replace(
    "    void init() {",
    '    int unused_probe() { return get_input_int ("mult", 1); }\n    void init() {').replace(
    '        len = get_input_int("len", 10);',
    '        len = get_input_int("len", 10);\n        mult = get_input_double("mult", 2.0);')


@pytest.mark.parametrize("receipt,expected", [
    (receipt_with_mult("float", "2"), {"type": "double", "default": 2.0, "value": 2.0}),
    (receipt_with_mult("int", "1"), {"type": "int", "default": 1, "value": 1}),
    (receipt_with_mult("float", "3"), None),
    (receipt_with_mult("string", "2"), None),
    (None, None),
])
def test_review_witness_is_arbitrated_never_first_wins(tmp_path, receipt, expected):
    result = legacy_document(tmp_path, REVIEW_WITNESS, receipt)
    if expected is None:
        assert result["inputs"]["mult"]["resolution"] == refusal("ambiguous_binding", "1")
    else:
        assert result["inputs"]["mult"] == expected


def test_two_matching_getters_stay_unresolved(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int("len", 10); big = get_input_int64("len", 10);')
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"]["reason"] == "ambiguous_binding"


def test_native_ambiguous_titles_never_certify_and_other_titles_keep_the_receipt(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int("len", 10); a = get_input_int("dup", 1); '
                          'b = get_input_int("dup", 2);')
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("dup", "int", "1"),
                             receipt_row("dup", "int", "2"))
    result = legacy_document(tmp_path, cpp, receipt)
    assert result["inputs"]["dup"]["resolution"] == duplicate_title("1", 2)
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("len", "int", "11"))
    result = legacy_document(tmp_path, ENUM_TU, receipt)
    assert result["inputs"]["len"]["resolution"] == duplicate_title("10", 2)
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("len", "int", "10"))
    result = legacy_document(tmp_path, ENUM_TU, receipt)
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}


# --- Property review of ea86db51f: findings 1-6 -------------------------------

def test_admitted_macro_names_need_the_producer_replacement_list(tmp_path):
    attack = ENUM_TU.replace("const int Side__long_ = 1;",
                             "#define PF_PINE_TIME_SESSION_DAY_ARGS(a, b) a ## b\n"
                             "const int Side__long_ = 1;").replace(
        "        side = get_input_int",
        "        int PF_PINE_TIME_SESSION_DAY_ARGS(Side__lo, ng_) = 2;\n        side = get_input_int")
    result = legacy_document(tmp_path, attack, ENUM_RECEIPT)
    assert result["inputs"]["Side"]["resolution"]["reason"] == "unsupported_binding"
    producer = "#define PF_PINE_TIME_SESSION_DAY_ARGS(tz, sess) , tz, sess\n" + ENUM_TU
    result = legacy_document(tmp_path, producer, ENUM_RECEIPT)
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}


@pytest.mark.parametrize("statement", [
    '        for (Probe unused, get_input_int("ghost", 7); once; once = false) {}\n',
    '        if (Probe unused, get_input_int("ghost", 7); once) {}\n',
    '        switch (Probe unused, get_input_int("ghost", 7); 1) {}\n',
])
def test_getter_declarators_in_init_statements_refuse(tmp_path, statement):
    cpp = ENUM_TU.replace("        side = get_input_int", statement + "        side = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"]["reason"] == "unsupported_binding"


@pytest.mark.parametrize("alias", [
    "        using std = AlternateStrings;\n",
    "        namespace pineforge { int x; }\n",
    "        struct checked_settings {};\n",
    "        int std = 1;\n",
])
def test_relied_on_names_must_stay_unaliased(tmp_path, alias):
    cpp = ENUM_TU.replace("        side = get_input_int", alias + "        side = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"]["reason"] == "unsupported_binding"


@pytest.mark.parametrize("change", [
    ("        pineforge::source::PineStrategyConfig cfg{};",
     "#if 0\n        pineforge::source::PineStrategyConfig cfg{};\n#endif"),
    ("    GeneratedStrategy() {", "    GeneratedStrategy(double initial_capital_ = 9.0) {"),
    ("        configure_pine_strategy(cfg);", "        configure_pine_strategy(cfg);\n        helper();"),
    ("        cfg.default_qty_type", "        helper();\n        cfg.default_qty_type"),
])
def test_constructor_flow_outside_the_producer_shape_refuses(tmp_path, change):
    result = legacy_document(tmp_path, ENUM_TU.replace(*change), ENUM_RECEIPT)
    assert result["strategy"]["default_qty_type"] is None
    assert result["strategy_resolution"]["default_qty_type"]["reason"] == "unsupported_binding"


@pytest.mark.parametrize("member", ["", "    double initial_capital_ = 9.0;\n"])
def test_pre_r4c_constructor_member_writes_are_not_certified(tmp_path, member):
    # An unqualified write may reach a derived member (review cont1 finding 2).
    cpp = ("struct GeneratedStrategy {\n" + member + "    GeneratedStrategy() {\n"
           "        initial_capital_ = 5.0;\n    }\n};\n")
    result = legacy_document(tmp_path, cpp, None)
    assert result["strategy"]["initial_capital"] is None
    assert result["strategy_resolution"]["initial_capital"]["reason"] == "unsupported_binding"


def test_strategy_integers_keep_the_native_width(tmp_path):
    cpp = ENUM_TU.replace("        configure_pine_strategy(cfg);",
                          "        cfg.pyramiding = 4294967297;\n        configure_pine_strategy(cfg);")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["strategy"]["pyramiding"] is None
    assert result["strategy_resolution"]["pyramiding"] == refusal("unsupported_default", "4294967297")


def test_legacy_strategy_values_are_compared_with_the_receipt(tmp_path):
    receipt = dict(ENUM_RECEIPT, overrides=[
        {"name": "initial_capital", "type": "float", "default": "42", "effective_value": "42",
         "supported": True}])
    result = legacy_document(tmp_path, ENUM_TU, receipt)
    assert result["strategy"]["initial_capital"] is None
    assert result["strategy_resolution"]["initial_capital"]["reason"] == "ambiguous_binding"
    receipt["overrides"][0].update(default="1000000", effective_value="1000000")
    result = legacy_document(tmp_path, ENUM_TU, receipt)
    assert result["strategy"]["initial_capital"] == 1000000.0


def test_refused_default_keeps_the_override_domain_refusal(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);', 'big = get_input_int64("Big", 1LL);')
    path = tmp_path / "generated.cpp"
    path.write_text(cpp)
    document = run_json.build_provenance(
        {}, path, True, {"Big": "9007199254740992"}, {}, {}, source_feed_sha256="0" * 64)
    with pytest.raises(ValueError):
        run_json.normalize_release_provenance(document, cpp, None, False)


@pytest.mark.parametrize("native", ["9007199254740993", "09007199254740993"])
def test_legacy_override_receipt_scalars_meet_the_domain_first(tmp_path, native):
    receipt = dict(ENUM_RECEIPT, overrides=[
        {"name": "pyramiding", "type": "int", "default": native,
         "effective_value": native, "supported": True}])
    path = tmp_path / "generated.cpp"
    path.write_text(ENUM_TU)
    document = run_json.build_provenance({}, path, True, {}, {}, {}, source_feed_sha256="0" * 64)
    with pytest.raises(ValueError):
        run_json.normalize_release_provenance(document, ENUM_TU, receipt, False)


def test_producer_prelude_names_stay_trusted(tmp_path):
    prelude = ("#include <pineforge/source/pine_strategy_host.hpp>\n"
               "#if __has_include(<pineforge/checked_settings.hpp>)\n"
               "#include <pineforge/checked_settings.hpp>\n#endif\n#include <string>\n"
               "using namespace pineforge;\n")
    result = legacy_document(tmp_path, prelude + ENUM_TU, ENUM_RECEIPT)
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}
    assert result["strategy"]["default_qty_type"] == "cash"



COMMA_MACRO = "#define PF_PINE_TIME_SESSION_DAY_ARGS(tz, sess) , tz, sess\n"


@pytest.mark.parametrize("invocation,trusted", [
    ("t = f(a PF_PINE_TIME_SESSION_DAY_ARGS(syminfo_.timezone, syminfo_.session));", True),
    # Review cont1 finding 1: the expansion declares `get_input_int` locally.
    ('Probe unused PF_PINE_TIME_SESSION_DAY_ARGS(\n        get_input_int("ghost", 7), spare);',
     False),
    ("t = f(a PF_PINE_TIME_SESSION_DAY_ARGS(syminfo_.timezone, other));", False),
    ("t = f(a PF_PINE_TIME_SESSION_DAY_ARGS);", False),
])
def test_comma_macros_are_trusted_only_in_the_producer_invocation(tmp_path, invocation, trusted):
    cpp = COMMA_MACRO + ENUM_TU.replace(
        "        len = get_input_int", "        " + invocation + "\n        len = get_input_int")
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"].get("ghost", {}).get("default") is None
    if trusted:
        assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}
    else:
        assert result["inputs"]["len"]["resolution"] == refusal("unsupported_binding", "10")
        assert result["inputs"]["Side"]["resolution"]["reason"] == "unsupported_binding"


GHOST = '        x = get_input_int("ghost", 7);\n'


@pytest.mark.parametrize("arms", [
    # No #else: the implicit empty branch takes no getter.
    "#ifdef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1\n" + GHOST + "#endif\n",
    # Review cont1 finding 3: an #elif arm with no final #else.
    "#ifdef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1\n" + GHOST + "#elif 0\n" + GHOST + "#endif\n",
    "#ifdef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1\n" + GHOST + "#elif 1\n" + GHOST
    + "#else\n" + GHOST + "#endif\n",
])
def test_security_guard_needs_its_explicit_else_to_cover_every_branch(tmp_path, arms):
    cpp = ENUM_TU.replace("    void init() {\n", "    void init() {\n" + arms)
    result = legacy_document(tmp_path, cpp, None)
    assert result["inputs"].get("ghost", {}).get("default") is None
    if "#elif" not in arms:
        # The getter may not run at all: only the native receipt certifies it.
        assert result["inputs"]["ghost"]["resolution"] == refusal("ambiguous_binding", "7")
        receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("ghost", "int", "7"))
        result = legacy_document(tmp_path, cpp, receipt)
        assert result["inputs"]["ghost"] == {"type": "int", "default": 7, "value": 7}


# TOP ruling 2026-10-07 21:55 (Option A): the same input keeps the fingerprint
# the base resolver (8e49cd58, where this round started) gave it whenever no
# certified value changed. Both resolvers certify the same values on these
# inputs; the source getters, whose certified values this round changes
# (rule 3), are left out.
STABLE_TU = ENUM_TU.replace('        src = get_input_source("Source", _src_close_)[0];\n', "")
STABLE_RECEIPT = legacy_receipt(*(row for row in ENUM_RECEIPT["inputs"] if row["name"] != "Source"))
STABLE_CASES = {
    "receipt": (STABLE_TU, STABLE_RECEIPT, {}, {}),
    "receipt-requests": (STABLE_TU, STABLE_RECEIPT, {"len": "14", "Side": "1"}, {}),
    "receipt-overrides": (STABLE_TU, STABLE_RECEIPT, {}, {"initial_capital": "5000"}),
    "no-receipt": (STABLE_TU, None, {"len": "12"}, {}),
    "release-numbers": (None, None, {"length": "1.4e1", "time": "1001tail", "mult": "1.50e0",
                                     "text": "false", "unknown": "true"}, {}),
    "release-booleans": (None, None, {"armed": "true", "disabled": "unrecognized"}, {}),
}


def stable_case(tmp_path, name):
    cpp, receipt, inputs, overrides = STABLE_CASES[name]
    if cpp is None:
        cpp = release_provenance(tmp_path)[1].replace('        get_input_source("source", close);\n', "")
    return cpp, receipt, dict(inputs), dict(overrides)


# Digests the base resolver (8e49cd58 docker/run_json.py, SHA-256 80dbc642...)
# gave exactly these cases (computed on the third Spot box, 2026-10-07, in the
# verification image, whose installed pineforge-codegen reports 1.3.0).
BASE_FINGERPRINTS = {
    "no-receipt": "sha256:63c498b9d0ee67985b301482239d44995a9db4b8e2af59f944c3e21b669237b2",
    "receipt": "sha256:02ad81404194f9cb591700e46a04ad4fce6369cbaf60be0e7a07c1b1b86c3ab5",
    "receipt-overrides":
        "sha256:9a1d7e2834ea0ac287dee7a415f891c8ea125eda5804b23d39b474c69e184b97",
    "receipt-requests":
        "sha256:782a19690c56e3c7e401321de4e062d83ef7949e410f77afe0f4836cbd301fa3",
    "release-booleans":
        "sha256:65b406ad8451fb0dc8f8b92e2450d54ceec08c4d5cd7a81d5baea437baac999c",
    "release-numbers":
        "sha256:42551c34415bf99163b4504d656ac59c8674c32a96f9fc776cb003c5a074aba4",
}


@pytest.mark.parametrize("name", sorted(STABLE_CASES))
def test_unchanged_certified_values_keep_the_base_fingerprint(tmp_path, monkeypatch, name):
    # The installed codegen's version is part of the fingerprinted input.
    monkeypatch.setattr(run_json, "_codegen_version", lambda: "1.3.0")
    cpp, receipt, inputs, overrides = stable_case(tmp_path, name)
    receipt = json.loads(json.dumps(receipt))
    result = legacy_document(tmp_path, cpp, receipt, inputs, overrides)
    assert run_json.build_fingerprint(result)["digest"] == BASE_FINGERPRINTS[name]


def test_unreadable_conflicting_getters_keep_their_own_refusal(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int64("len", 0xffLL); n = get_input_int64("len", 0x1LL);')
    result = legacy_document(tmp_path, cpp, None)
    assert result["inputs"]["len"]["resolution"] == refusal("unsupported_default", "0xffLL")


# --- Continuation 2: getters in macro arguments, duplicate native titles -----

MACRO_DEFINES = {
    "_PF_ENGINE_INVARIANT": "#define _PF_ENGINE_INVARIANT(english, legacy_type) "
                            "::pineforge::pine_engine_invariant(english)",
    "_PF_INVARIANT_AT": "#define _PF_INVARIANT_AT(container, index) _pf_invariant_at(container, index)",
    "_PF_ARRAY_STOP": "#define _PF_ARRAY_STOP(reason, method, english) "
                      "::pineforge::pine_array_stop(reason, method, std::string(english).c_str())",
    "_PF_NO_DATA_STOP": "#define _PF_NO_DATA_STOP(function, call, line, english) "
                        "::pineforge::pine_no_data_stop(function, call, line, english)",
    "_PF_OTHER_SYMBOL_STOP": "#define _PF_OTHER_SYMBOL_STOP(function, symbol, call, line, english) "
                             "::pineforge::pine_other_symbol_stop(function, symbol, call, line, english)",
}


@pytest.mark.parametrize("macro,invocation", [
    # The cont2 review witness: the release replacement list drops `legacy_type`.
    ("_PF_ENGINE_INVARIANT", '_PF_ENGINE_INVARIANT("unused", GHOST);'),
    # One per producer macro arity (2 to 5), the getter in a kept argument.
    ("_PF_INVARIANT_AT", "_PF_INVARIANT_AT(values, GHOST);"),
    ("_PF_ARRAY_STOP", '_PF_ARRAY_STOP(GHOST, "method", "english");'),
    ("_PF_NO_DATA_STOP", '_PF_NO_DATA_STOP("f", "call", GHOST, "english");'),
    ("_PF_OTHER_SYMBOL_STOP", '_PF_OTHER_SYMBOL_STOP("f", GHOST, "call", 1, "english");'),
    # A standard-library macro.
    ("", "assert(GHOST > 0);"),
])
def test_getters_in_macro_arguments_are_never_certified(tmp_path, macro, invocation):
    prelude = MACRO_DEFINES[macro] + "\n" if macro else ""
    cpp = prelude + ENUM_TU.replace(
        "        len = get_input_int",
        "        " + invocation.replace("GHOST", 'get_input_int("ghost", 7)')
        + "\n        len = get_input_int")
    with_ghost = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("ghost", "int", "7"))
    for receipt in (None, ENUM_RECEIPT, with_ghost):
        result = legacy_document(tmp_path, cpp, receipt)
        assert result["inputs"]["ghost"] == {"type": "int", "default": None, "value": None,
                                             "resolution": refusal("macro_argument", "7")}
        # The rest of the unit is still read.
        assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}


def test_a_title_also_read_in_a_macro_argument_is_not_certified(tmp_path):
    cpp = MACRO_DEFINES["_PF_INVARIANT_AT"] + "\n" + ENUM_TU.replace(
        "        len = get_input_int",
        '        _PF_INVARIANT_AT(values, get_input_int("len", 10));\n        len = get_input_int')
    result = legacy_document(tmp_path, cpp, ENUM_RECEIPT)
    assert result["inputs"]["len"]["resolution"] == refusal("macro_argument", "10")
    assert result["inputs"]["Side"] == {"type": "int", "default": 1, "value": 1}


@pytest.mark.parametrize("defaults,distinct", [
    (("1", "2"), 2), (("1", "2", "3"), 3), (("1", "2", "1"), 2)])
def test_duplicate_native_titles_carry_their_reason_and_distinct_input_count(
        tmp_path, defaults, distinct):
    # TOP ruling 2026-10-07 22:58: a specific reason and the number of
    # distinct native inputs, never the generic ambiguous_binding.
    getters = " ".join('d%d = get_input_int("dup", %s);' % (number, value)
                       for number, value in enumerate(defaults))
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int("len", 10); ' + getters)
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"],
                             *(receipt_row("dup", "int", value) for value in defaults))
    result = legacy_document(tmp_path, cpp, receipt)
    assert result["inputs"]["dup"] == {"type": "int", "default": None, "value": None,
                                       "resolution": duplicate_title(defaults[0], distinct)}
    assert result["inputs"]["len"] == {"type": "int", "default": 10, "value": 10}


def test_a_title_listed_twice_with_one_setting_is_not_a_duplicate_title(tmp_path):
    cpp = ENUM_TU.replace('len = get_input_int("len", 10);',
                          'len = get_input_int("len", 10); a = get_input_int("dup", 4); '
                          'b = get_input_int("dup", 4);')
    receipt = legacy_receipt(*ENUM_RECEIPT["inputs"], receipt_row("dup", "int", "4"),
                             receipt_row("dup", "int", "4"))
    result = legacy_document(tmp_path, cpp, receipt)
    assert result["inputs"]["dup"] == {"type": "int", "default": 4, "value": 4}

