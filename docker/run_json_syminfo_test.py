import base64
import contextlib
import ctypes
import io
import json
import sys
import types
from pathlib import Path

import pytest
import run_json  # docker/ is on sys.path in the engine test env


def test_apply_syminfo_calls_setters(tmp_path):
    calls = {}
    lib = types.SimpleNamespace(
        strategy_set_syminfo_mintick=lambda s, v: calls.setdefault("mintick", v),
        strategy_set_syminfo_pointvalue=lambda s, v: calls.setdefault("pointvalue", v),
        strategy_set_syminfo_timezone=lambda s, v: calls.setdefault("tz", v),
        strategy_set_syminfo_session=lambda s, v: calls.setdefault("session", v),
    )
    p = tmp_path / "syminfo.json"
    p.write_text(json.dumps({"syminfo": {"mintick": 0.5, "pointvalue": 2.0,
                                          "timezone": "UTC", "session": "24x7"}}))
    run_json.apply_syminfo(lib, object(), p)
    assert calls == {"mintick": 0.5, "pointvalue": 2.0, "tz": b"UTC", "session": b"24x7"}


# syminfo.mincontract (the instrument's lot size) is the engine's lot grid: set as the
# metadata keys qty_step and mincontract before the other setters; absent or null changes
# nothing; anything but a positive finite JSON number is a structured failure.

SETTERS = ("strategy_set_syminfo_metadata", "strategy_set_syminfo_mintick",
           "strategy_set_syminfo_pointvalue", "strategy_set_syminfo_timezone",
           "strategy_set_syminfo_session")
CORE = ("pf_abi_version", "strategy_create", "strategy_set_input", "strategy_set_override",
        "run_backtest_full", "strategy_free", "report_free")
FOUR = {"mintick": 0.5, "pointvalue": 2, "timezone": "UTC", "session": "24x7"}
ST = 7
FOUR_CALLS = [("strategy_set_syminfo_mintick", ST, 0.5),
              ("strategy_set_syminfo_pointvalue", ST, 2.0),
              ("strategy_set_syminfo_timezone", ST, b"UTC"),
              ("strategy_set_syminfo_session", ST, b"24x7")]
BAD_LOT = "syminfo.mincontract must be a positive finite number, got "


class FakeLib:
    """A strategy library stand-in: each named symbol records (name, *args); a name
    left out fails hasattr like a missing export."""

    def __init__(self, names, returns=None):
        self.calls = []
        for name in names:
            setattr(self, name, self._fn(name, (returns or {}).get(name)))

    def _fn(self, name, result):
        def call(*args):
            self.calls.append((name,) + args)
            return result
        return call


def write(tmp_path, doc, name="syminfo.json"):
    p = tmp_path / name
    p.write_text(json.dumps(doc))
    return p


def fake_lib(setters=SETTERS):
    # strategy_create returns a handle: run_json refuses a NULL one before any setter.
    return FakeLib(CORE + tuple(setters), {"pf_abi_version": run_json.EXPECTED_PF_ABI,
                                           "strategy_create": ST})


@pytest.mark.parametrize("doc,lot", [
    (dict(FOUR, mincontract=0.25), 0.25),
    ({"syminfo": dict(FOUR, mincontract=1e-05)}, 1e-05),
    (dict(FOUR, mincontract=1), 1.0),
], ids=["flat", "wrapped", "integer"])
def test_mincontract_sets_the_lot_grid_before_the_other_setters(tmp_path, doc, lot):
    lib = FakeLib(SETTERS)
    got = run_json.apply_syminfo(lib, ST, write(tmp_path, doc))
    assert lib.calls == [("strategy_set_syminfo_metadata", ST, b"qty_step", lot),
                         ("strategy_set_syminfo_metadata", ST, b"mincontract", lot)] + FOUR_CALLS
    assert all(type(c[3]) is float for c in lib.calls[:2])
    assert got == {"qty_step": lot, "mincontract": lot}
    assert all(type(v) is float for v in got.values())  # an integer mincontract is recorded as a float


@pytest.mark.parametrize("setters", [SETTERS, SETTERS[1:]], ids=["setter", "no-setter"])
@pytest.mark.parametrize("doc", [
    dict(FOUR),
    dict(FOUR, mincontract=None),
    {"syminfo": dict(FOUR, mincontract=None)},
], ids=["absent", "null", "wrapped-null"])
def test_mincontract_absent_or_null_applies_no_grid(tmp_path, doc, setters):
    lib = FakeLib(setters)
    assert run_json.apply_syminfo(lib, ST, write(tmp_path, doc)) == {}
    assert lib.calls == FOUR_CALLS


def test_syminfo_error_is_a_value_error():
    assert issubclass(run_json.SyminfoError, ValueError)


HUGE_INT = "1" + "0" * 400
LONG_STR = '"' + "x" * 1000 + '"'


@pytest.mark.parametrize("raw,shown", [
    ("0", "0"), ("-1", "-1"), ("-0.0", "-0.0"), ("1e-400", "0.0"),
    ('"0.001"', '"0.001"'), ("true", "true"), ("false", "false"),
    ("NaN", "NaN"), ("Infinity", "Infinity"), ("-Infinity", "-Infinity"),
    ("[]", "[]"), ("{}", "{}"),
    (HUGE_INT, HUGE_INT[:80]), (LONG_STR, LONG_STR[:80]),
], ids=["zero", "negative", "negative-zero", "underflow", "string", "true", "false",
        "nan", "infinity", "negative-infinity", "array", "object", "huge-int", "long-string"])
def test_mincontract_not_a_positive_finite_number_is_rejected(tmp_path, raw, shown):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(FOUR)[:-1] + ', "mincontract": ' + raw + "}")
    lib = FakeLib(SETTERS)
    with pytest.raises(run_json.SyminfoError) as e:
        run_json.apply_syminfo(lib, ST, bad)
    assert str(e.value) == BAD_LOT + shown
    assert lib.calls == []


def test_mincontract_without_the_metadata_setter_is_rejected(tmp_path):
    lib = FakeLib(SETTERS[1:])
    with pytest.raises(run_json.SyminfoError, match="strategy_set_syminfo_metadata"):
        run_json.apply_syminfo(lib, ST, write(tmp_path, dict(FOUR, mincontract=0.25)))
    assert lib.calls == []


def test_load_strategy_declares_the_metadata_setter_signature(monkeypatch):
    monkeypatch.setattr(ctypes, "CDLL", lambda path: fake_lib())
    meta = run_json.load_strategy(Path("fake.so")).strategy_set_syminfo_metadata
    assert meta.argtypes == [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
    assert meta.restype is None


# main() against the fake library.

@pytest.fixture
def harness(tmp_path, monkeypatch):
    tape = tmp_path / "tape.csv"
    tape.write_text("open,high,low,close,volume,timestamp\n"
                    "1,2,0.5,1.5,10,1000\n1.5,3,1,2.5,20,2000\n")

    def run(lib, *extra, syminfo=None):
        argv = ["run_json.py", "--so", "fake.so", "--ohlcv", str(tape), *extra]
        if syminfo is not None:
            argv += ["--syminfo", str(write(tmp_path, syminfo, "s.json"))]
        monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
        monkeypatch.setattr(sys, "argv", argv)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = run_json.main()
        return status, out.getvalue()

    return run


def report_of(text):
    rep = json.loads(text)
    rep["elapsed_seconds"] = 0
    return rep


def count(lib, name):
    return [c[0] for c in lib.calls].count(name)


@pytest.mark.parametrize("doc", [dict(FOUR), dict(FOUR, mincontract=None)],
                         ids=["absent", "null"])
def test_main_without_a_grid_reports_as_without_syminfo(harness, doc):
    _, plain = harness(fake_lib())
    status, out = harness(fake_lib(), syminfo=doc)
    assert status == 0
    assert report_of(out) == report_of(plain)
    assert "syminfo" not in report_of(out)["applied_runtime"]


def test_main_records_the_grid_in_the_report_and_the_fingerprint(harness):
    _, plain = harness(fake_lib())
    status, out = harness(fake_lib(), syminfo=dict(FOUR, mincontract=0.25))
    rep = report_of(out)
    token = base64.b64decode(rep["fingerprint"]["token"])
    prov = json.loads(token)
    grid = {"qty_step": 0.25, "mincontract": 0.25}
    assert status == 0
    assert rep["applied_runtime"]["syminfo"] == grid
    assert prov["runtime"]["syminfo"] == grid
    assert rep["fingerprint"]["digest"] != report_of(plain)["fingerprint"]["digest"]
    # The digested bytes carry the grid in canonical form, and only when one was applied.
    assert b'"syminfo":{"mincontract":0.25,"qty_step":0.25}' in token
    assert b"syminfo" not in base64.b64decode(report_of(plain)["fingerprint"]["token"])


def test_main_bench_applies_the_grid_to_every_state(harness):
    lib = fake_lib()
    status, out = harness(lib, "--bench", "--warmup", "2", "--repeats", "3",
                          syminfo=dict(FOUR, mincontract=0.25))
    states = 2 + 3 + 1
    assert status == 0
    assert report_of(out)["applied_runtime"]["syminfo"] == {"qty_step": 0.25, "mincontract": 0.25}
    assert count(lib, "strategy_create") == count(lib, "strategy_free") == states
    assert [c[2] for c in lib.calls if c[0] == "strategy_set_syminfo_metadata"] \
        == [b"qty_step", b"mincontract"] * states


@pytest.mark.parametrize("extra", [
    [],
    ["--bench", "--warmup", "1", "--repeats", "1"],
    ["--bench", "--warmup", "0", "--repeats", "1"],
], ids=["body", "bench-warmup", "bench-repeats"])
def test_main_rejects_a_bad_mincontract_with_one_structured_line(harness, extra):
    lib = fake_lib()
    status, out = harness(lib, *extra, syminfo=dict(FOUR, mincontract=-1))
    assert status == 1
    assert out == ('{"engine":"pineforge","error":"' + BAD_LOT + '-1",'
                   '"code":"lot_grid_rejected","args":{}}\n')
    assert count(lib, "run_backtest_full") == 0
    assert count(lib, "strategy_create") == count(lib, "strategy_free") == 1


def test_main_with_a_grid_and_no_metadata_setter_fails(harness):
    status, out = harness(fake_lib(SETTERS[1:]), syminfo=dict(FOUR, mincontract=0.25))
    assert status == 1
    assert json.loads(out)["error"].startswith(
        "the strategy library has no strategy_set_syminfo_metadata")
    assert json.loads(out)["code"] == "strategy_library_incompatible"
    assert json.loads(out)["args"] == {"reason": "setter_missing",
                                       "missing": "strategy_set_syminfo_metadata"}


@pytest.mark.parametrize("value", [None, True], ids=["None", "True"])
def test_main_ignores_a_non_dict_from_a_replacement_apply_syminfo(harness, monkeypatch, value):
    _, plain = harness(fake_lib())
    monkeypatch.setattr(run_json, "apply_syminfo", lambda lib, strat, path: value)
    status, out = harness(fake_lib(), syminfo=dict(FOUR, mincontract=0.25))
    assert status == 0
    assert report_of(out) == report_of(plain)
