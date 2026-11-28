"""The other harnesses fail a run as docker/run_json.py does: on a text
(strategy_get_last_error), a code (strategy_get_last_error_code) or a run status
of 1 (strategy_last_run_status), so a script stopped by runtime.error("") is not a
result there either, and their output for a run that succeeded is unchanged.

Covered: scripts/run_strategy.py (Strategy.run, the validation harness, and
its docker runner's settings, which run_json must take as given),
scripts/run_stream_corpus.py (its batch run), tutorial/run.py and
tutorial/run_advanced.py, each against a fake strategy library.
tutorial/run_stream.py fails on the status of every stream call, which a failed
run sets, so it never had the text-only test.
"""
import contextlib
import ctypes
import importlib.util
import io
import json
from pathlib import Path
from subprocess import CompletedProcess
import sys
import tarfile

import pytest
import run_json  # docker/ is on sys.path in the engine test env

ROOT = Path(__file__).resolve().parents[1]
ST = 7
TAPE = ("timestamp,open,high,low,close,volume\n"
        "1744032600000,9.06,9.1,9.0,9.05,100\n"
        "1744033500000,9.05,9.2,9.0,9.18,120\n")
FIXED_TEXT = "the run did not complete and the engine reported no error"


def _load(name, path, *search):
    """path imported as module name, with search on sys.path only while it loads."""
    saved = list(sys.path)
    sys.path[:0] = [str(p) for p in search]
    try:
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    finally:
        sys.path[:] = saved
    return module


SCRIPTS = ROOT / "scripts"
run_strategy = _load("run_strategy", SCRIPTS / "run_strategy.py", SCRIPTS)
pf_release_run = _load("pf_release_run", SCRIPTS / "pf_release_run.py", SCRIPTS)
run_stream_corpus = _load("run_stream_corpus", SCRIPTS / "run_stream_corpus.py", SCRIPTS)
tutorial_run = _load("run", ROOT / "tutorial" / "run.py")  # run_advanced imports "run"
run_advanced = _load("run_advanced", ROOT / "tutorial" / "run_advanced.py")


class FakeLib:
    """A strategy library whose run reports text, code (None: no code getter, a
    library built before engine 1.4.0) and status (None: no
    strategy_last_run_status). Exports are plain functions, so a harness can set
    their argtypes and restype."""

    def __init__(self, text=b"", code=None, args=b"{}", status=None):
        self.calls = []
        exports = {
            "pf_abi_version": lambda: run_json.EXPECTED_PF_ABI,
            "strategy_create": lambda params: ST,
            "strategy_set_input": lambda state, key, value: None,
            "strategy_set_override": lambda state, key, value: None,
            "run_backtest_full": lambda state, *rest: None,
            "report_free": lambda report: None,
            "strategy_free": lambda state: None,
            "strategy_get_last_error": lambda state: text,
        }
        if code is not None:
            exports["strategy_get_last_error_code"] = lambda state: code
            exports["strategy_get_last_error_args"] = lambda state: args
        if status is not None:
            exports["strategy_last_run_status"] = lambda state: status
        for name, body in exports.items():
            setattr(self, name, self._record(name, body))

    def _record(self, name, body):
        def call(*args):
            self.calls.append(name)
            return body(*args)
        return call


# What the engine reports after a run -> each harness's verdict. A code with an
# empty text is what runtime.error("") leaves.
CASES = {
    "text": dict(text=b"boom"),
    "text-and-code": dict(text=b"boom", code=b"strategy_runtime_error", status=1),
    "code-only": dict(code=b"strategy_runtime_error", status=0),
    "status-only": dict(code=b"", args=b"", status=1),
    "status-only-old-library": dict(status=1),
    "clean": dict(code=b"", args=b"", status=0),
    "clean-old-library": dict(),
}
FAILED = [name for name in CASES if not name.startswith("clean")]


def _strategy(lib):
    strategy = run_strategy.Strategy.__new__(run_strategy.Strategy)
    strategy.lib = lib
    return strategy


@pytest.fixture
def tape(tmp_path):
    path = tmp_path / "tape.csv"
    path.write_text(TAPE)
    return path


def test_the_fixed_text_is_run_json_s():
    assert run_strategy.RUN_STATUS_FAILED_TEXT == run_json.RUN_STATUS_FAILED_TEXT == FIXED_TEXT


# --- scripts/run_strategy.py --------------------------------------------------

@pytest.mark.parametrize("case,message,code", [
    ("text", "pineforge engine rejected run: boom", None),
    ("text-and-code", "pineforge engine rejected run: boom", "strategy_runtime_error"),
    ("code-only", "pineforge engine rejected run", "strategy_runtime_error"),
    ("status-only", f"pineforge engine rejected run: {FIXED_TEXT}", None),
    ("status-only-old-library", f"pineforge engine rejected run: {FIXED_TEXT}", None),
])
def test_run_strategy_fails_a_run_on_a_text_a_code_or_status_1(tape, case, message, code):
    lib = FakeLib(**CASES[case])
    with pytest.raises(RuntimeError) as failure:
        _strategy(lib).run(tape)
    assert str(failure.value) == message
    assert getattr(failure.value, "run_failure_code", None) == code
    if code:
        assert failure.value.run_failure_args == "{}"


@pytest.mark.parametrize("case", ["clean", "clean-old-library"])
def test_run_strategy_reports_a_run_that_succeeded(tape, case):
    lib = FakeLib(**CASES[case])
    report = _strategy(lib).run(tape)
    assert (report["trades"], report["net_profit"]) == ([], 0.0)
    assert lib.calls.count("run_backtest_full") == 1


def test_run_strategy_declares_the_run_status_getter_without_the_abort_export(monkeypatch):
    lib = FakeLib(code=b"", status=0)  # strategy_last_run_status, no strategy_request_abort
    monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
    run_strategy.Strategy(Path(run_strategy.__file__))  # an existing path: CDLL is faked
    assert lib.strategy_last_run_status.argtypes == [ctypes.c_void_p]
    assert lib.strategy_last_run_status.restype is ctypes.c_int
    for name in ("strategy_get_last_error_code", "strategy_get_last_error_args"):
        assert getattr(lib, name).restype is ctypes.c_char_p, name


def test_run_strategy_docker_runner_sends_overrides_as_strategy_run_does(tmp_path, tape,
                                                                         monkeypatch):
    # The release image's run_json refuses a JSON boolean or null; both
    # runners send strategy property overrides as strings.
    # The image's run_json refuses a JSON boolean or null; the Docker runner
    # sends them as wire text, with lowercase booleans for the generated setter.
    (tmp_path / "generated.cpp").write_text("// generated\n")
    sent = {}

    class Sent(Exception):
        pass

    def run_release(generated_cpp, ohlcv, **kw):
        sent.update(kw)
        raise Sent
    monkeypatch.setattr(pf_release_run, "run_release", run_release)
    overrides = {"process_orders_on_close": True, "calc_on_order_fills": False,
                 "initial_capital": 1000,
                 "commission_value": 0.04, "default_qty_type": "cash", "slippage": None}
    with pytest.raises(Sent):
        run_strategy._run_via_docker(tmp_path, tape, {"Length": 5, "Use Filter": False},
                                     {"strategy_overrides": overrides}, None, None)
    assert sent["overrides"] == {"process_orders_on_close": "true", "calc_on_order_fills": "false",
                                 "initial_capital": "1000",
                                 "commission_value": "0.04", "default_qty_type": "cash",
                                 "slippage": "None"}
    # expectation corrected: "False" -> "false", because Pine boolean inputs require lowercase.
    assert sent["inputs"] == {"Length": "5", "Use Filter": "false"}
    for label in ("inputs", "overrides"):
        assert run_json.parse_kv_json(json.dumps(sent[label]), f"--{label}") == sent[label]


# --- scripts/run_stream_corpus.py ---------------------------------------------

CONFIG = {"strategy_overrides": {}, "params": {}, "runtime": {}, "chart_timezone": None,
          "input_tf": "1", "script_tf": "1"}


@pytest.mark.parametrize("case,message", [
    ("text", "batch run: boom"),
    ("text-and-code", "batch run: boom"),
    ("code-only", "batch run"),
    ("status-only", f"batch run: {FIXED_TEXT}"),
    ("status-only-old-library", f"batch run: {FIXED_TEXT}"),
])
def test_stream_corpus_batch_fails_a_run_on_a_text_a_code_or_status_1(case, message):
    lib = FakeLib(**CASES[case])
    with pytest.raises(RuntimeError) as failure:
        run_stream_corpus.run_batch(_strategy(lib), (run_strategy.BarC * 0)(), 0, CONFIG)
    assert str(failure.value) == message
    assert lib.calls[-2:] == ["report_free", "strategy_free"]


@pytest.mark.parametrize("case", ["clean", "clean-old-library"])
def test_stream_corpus_batch_reports_a_run_that_succeeded(case):
    report = run_stream_corpus.run_batch(_strategy(FakeLib(**CASES[case])),
                                         (run_strategy.BarC * 0)(), 0, CONFIG)
    assert report["trades"] == []


# --- tutorial/run.py and tutorial/run_advanced.py ------------------------------

@pytest.mark.parametrize("case,expected", [
    ("text", "boom"),
    ("text-and-code", "boom (strategy_runtime_error {})"),
    ("code-only", "(strategy_runtime_error {})"),
    ("status-only", FIXED_TEXT),
    ("status-only-old-library", FIXED_TEXT),
    ("clean", None),
    ("clean-old-library", None),
])
def test_tutorial_run_error_follows_run_json(case, expected):
    assert tutorial_run.run_error(FakeLib(**CASES[case]), ST) == expected


@pytest.fixture
def tutorial(tmp_path, monkeypatch, tape):
    so = tmp_path / "strategy.so"
    so.write_bytes(b"")
    monkeypatch.setattr(tutorial_run, "SO", so)
    monkeypatch.setattr(tutorial_run, "OHLCV", tape)

    def main(lib):
        monkeypatch.setattr(ctypes, "CDLL", lambda path: lib)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = tutorial_run.main()
        return status, out.getvalue(), err.getvalue()
    return main


def test_tutorial_main_fails_a_code_only_run(tutorial):
    lib = FakeLib(**CASES["code-only"])
    status, out, err = tutorial(lib)
    assert (status, out, err) == (1, "", "engine error: (strategy_runtime_error {})\n")
    assert lib.calls[-2:] == ["report_free", "strategy_free"]
    assert lib.strategy_last_run_status.restype is ctypes.c_int


def test_tutorial_main_prints_the_summary_of_a_run_that_succeeded(tutorial):
    status, out, err = tutorial(FakeLib(**CASES["clean"]))
    assert (status, err) == (0, "")
    assert out.splitlines()[0] == ("MACD(12,26,9) on BTCUSDT 15m \u2014 2 bars, "
                                   "2025-04-07 13:30 \u2192 2025-04-07 13:45 UTC")
    assert out.splitlines()[1] == "  trades:    0  (0W / 0L, 0.0% win)"


@pytest.mark.parametrize("case", FAILED)
def test_tutorial_sweep_fails_a_run_on_a_text_a_code_or_status_1(case):
    expected = "pineforge engine rejected run: " + tutorial_run.run_error(
        FakeLib(**CASES[case]), ST)
    lib = FakeLib(**CASES[case])
    with pytest.raises(RuntimeError) as failure:
        run_advanced.run_one(lib, (tutorial_run.BarC * 0)(), 0, inputs={}, overrides={})
    assert str(failure.value) == expected
    assert lib.calls[-2:] == ["report_free", "strategy_free"]


def test_tutorial_sweep_reports_a_run_that_succeeded():
    out = run_advanced.run_one(FakeLib(**CASES["clean"]), (tutorial_run.BarC * 0)(), 0,
                               inputs={"Fast Length": 8}, overrides={"default_qty_value": 5})
    assert {k: out[k] for k in ("trades", "wins", "net_pnl")} == {
        "trades": 0, "wins": 0, "net_pnl": 0.0}


# --- scripts/run_strategy.py: the lot keys of runtime_overrides (#716) ----------

class FakeDocker:
    """Replace Docker and record the create args and metadata copied into /in."""

    IMAGE = "sha256:" + "0" * 64
    CONTAINER = "1" * 64

    def __init__(self, stdout="{}\n", stderr=""):
        self.argv, self.documents = None, []
        self.stdout = stdout
        self.stderr = stderr
        self._phase = None

    def run(self, cmd, **options):
        if cmd[:3] == ["docker", "image", "inspect"]:
            config = {"Entrypoint": ["/usr/local/bin/entrypoint.sh"], "Cmd": None,
                      "User": "10001:10001"}
            return CompletedProcess(cmd, 0, json.dumps([{"Id": self.IMAGE, "Config": config}]), "")
        if cmd[:2] == ["docker", "create"]:
            assert self._phase is None
            self.argv = list(cmd)
            self.documents = []
            self._phase = "created"
            return CompletedProcess(cmd, 0, self.CONTAINER + "\n", "")
        if cmd == ["docker", "container", "inspect", "--format", "{{.Image}}", self.CONTAINER]:
            assert self._phase == "created"
            return CompletedProcess(cmd, 0, self.IMAGE + "\n", "")
        if cmd == ["docker", "cp", "-a", "-", f"{self.CONTAINER}:/"]:
            assert self._phase == "created"
            with tarfile.open(fileobj=io.BytesIO(options["input"]), mode="r:") as archive:
                for member in archive:
                    if member.name == "in/syminfo.json":
                        assert member.isfile()
                        with archive.extractfile(member) as document:
                            self.documents.append(document.read().decode("utf-8"))
            self._phase = "copied"
            return CompletedProcess(cmd, 0, b"", b"")
        if cmd == ["docker", "start", "-a", self.CONTAINER]:
            assert self._phase == "copied"
            self._phase = "started"
            return CompletedProcess(cmd, 0, self.stdout, self.stderr)
        if cmd == ["docker", "rm", "-f", "-v", self.CONTAINER]:
            assert self._phase is not None
            self._phase = None
            return CompletedProcess(cmd, 0, "", "")
        raise AssertionError(f"unexpected Docker command: {cmd}")


def test_release_launcher_mounts_the_symbol_document_with_both_lot_keys(tmp_path, tape,
                                                                         monkeypatch):
    # Keep the existing test name for base/head matching. Read the copied JSON.
    generated = tmp_path / "generated.cpp"
    generated.write_text("// generated\n")
    docker = FakeDocker()
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    symbol = {"qty_step": 0.25, "mincontract": 0.01, "timezone": "America/New_York"}
    assert pf_release_run.run_release(generated, tape, syminfo=symbol) == {}
    assert [json.loads(text) for text in docker.documents] == [symbol]
    assert "PINEFORGE_SYMINFO=/in/syminfo.json" in docker.argv


@pytest.mark.parametrize("overrides,lot", [
    pytest.param({"qty_step": 1, "mincontract": 5},
                 {"qty_step": 1.0, "mincontract": 5.0}, id="flat-integers"),
    pytest.param({"syminfo_metadata": {"qty_step": 0.25, "mincontract": 0.01}},
                 {"qty_step": 0.25, "mincontract": 0.01}, id="nested"),
    pytest.param({"qty_step": None, "mincontract": None,
                  "syminfo_metadata": {"qty_step": 0.5, "mincontract": 2}},
                 {"qty_step": 0.5, "mincontract": 2.0}, id="flat-null-nested-used"),
    pytest.param({"qty_step": 0.25, "mincontract": 0.01,
                  "syminfo_metadata": {"qty_step": None, "mincontract": None}},
                 {"qty_step": 0.25, "mincontract": 0.01}, id="nested-null-flat-used"),
    pytest.param({"qty_step": 0.25, "mincontract": 0.01,
                  "syminfo_metadata": {"qty_step": 0.5, "mincontract": 2}},
                 {"qty_step": 0.25, "mincontract": 2.0}, id="both-flat-step-nested-minimum"),
    pytest.param({"qty_step": 0.25}, {"qty_step": 0.25}, id="step-alone"),
    pytest.param({"mincontract": 0.01}, {"mincontract": 0.01}, id="minimum-alone"),
    pytest.param({"qty_step": None, "syminfo_metadata": {"mincontract": None}}, {},
                 id="all-null"),
])
def test_run_strategy_hands_both_runners_the_same_valid_lot(tmp_path, tape, monkeypatch,
                                                            overrides, lot):
    # One inputs.json resolved once (inputs_run_kwargs), then each runner's own
    # hand-over: the metadata the ctypes runner sets on the engine, and the
    # document the Docker launcher copies into the container.
    params = {"runtime_overrides": overrides}
    _, kwargs = run_strategy.inputs_run_kwargs(params, tmp_path, tape, env={})

    engine = {}
    lib = FakeLib(**CASES["clean"])
    lib.strategy_set_syminfo_metadata = (
        lambda state, key, value: engine.update({key.decode(): value}))
    _strategy(lib).run(tape, **kwargs)

    (tmp_path / "generated.cpp").write_text("// generated\n")
    docker = FakeDocker()
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    run_strategy._run_via_docker(tmp_path, tape, params, kwargs, None, None)

    assert engine == lot
    assert [json.loads(text) for text in docker.documents] == ([lot] if lot else [])


def test_runner_projects_each_symbol_key_to_its_ctypes_setter(tmp_path, tape, monkeypatch):
    # Capture both the copied document and the setter arguments it produces.
    # Raw numeric names are open-ended and can even coincide with a catalog field.
    numbers = {
        "account_currency_fx": "1.25", "chart_ema_na_warmup": 0,
        "series_origin_ms": "1744032600000", "security_range_start_na_warmup": False,
        "bar_index_offset": -2, "historical_security_lookahead_projection": 1,
        "margin_long": 25, "margin_short": 30,
        "flat_retained_child_fresh_parent_order": 0, "intraday_cap_skip_noop_market_fills": 0,
        "intraday_cap_defer_pooc_close": 1, "intraday_cap_count_pooc_full_close_fills": 0,
        "symbol_calendar_days": 1, "symbol_calendar_open:0": 1744032600000,
        "symbol_calendar_close:0": 1744036200000,
        "report_terminal_quote_time_ms": 1744036200000, "report_terminal_quote_close": 10,
        "shares_outstanding_total": 47, "target_price_high": "inf", "new_member": -0.75,
        "mintick": 42,
    }
    strings = {"ticker": "TEST", "tickerid": "X:TEST", "currency": "USD",
               "basecurrency": "BTC", "description": "Tést", "volumetype": "base"}
    overrides = {"qty_step": 0.25, "mincontract": 0.01, "mintick": 0.5, "pointvalue": 2,
                 "timezone": "UTC", "session": "24x7", "type": "forex", **strings,
                 "syminfo_metadata": {**numbers, "absent": None, "bad": "no number"}}
    _, kwargs = run_strategy.inputs_run_kwargs({"runtime_overrides": overrides},
                                              tmp_path, tape, env={})

    def observer():
        lib, calls = FakeLib(**CASES["clean"]), []
        for field in ("metadata", "timezone", "session", "type", "string", "mintick", "pointvalue"):
            def record(state, *args, field=field):
                assert state == ST
                calls.append((field, *args))
            setattr(lib, "strategy_set_syminfo_" + field, record)
        return lib, calls

    host, host_calls = observer()
    _strategy(host).run(tape, **kwargs)
    (tmp_path / "generated.cpp").write_text("// generated\n")
    docker = FakeDocker()
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    run_strategy._run_via_docker(tmp_path, tape, {}, kwargs, None, None)
    document, = docker.documents
    symbol = json.loads(document)
    raw = [(key, float(value)) for key, value in numbers.items()]
    wire = [(key, "inf" if key == "target_price_high" else value) for key, value in raw]
    assert list(symbol.get("syminfo_metadata", {}).items()) == wire
    expected_symbol = {
        k: v for k, v in overrides.items()
        if k not in ("syminfo_metadata", "type", *strings)
    }
    expected_symbol["_pineforge_runner_syminfo"] = {
        "type": "forex", "strings": strings}
    assert {k: v for k, v in symbol.items() if k != "syminfo_metadata"} == expected_symbol
    assert "PINEFORGE_SYMINFO=/in/syminfo.json" in docker.argv

    path = tmp_path / "received.json"
    path.write_text(document)
    image, image_calls = observer()
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        applied = run_json.apply_syminfo(image, ST, path)
    # Lot setup historically runs first in the image. The relative order
    # of the other raw settings matters (origin and warm-ups are last-writer wins).
    by_name = [(call[1].decode(), call[2]) for call in image_calls if call[0] == "metadata"]
    assert [(k, v) for k, v in by_name if k not in ("qty_step", "mincontract")] == raw
    assert sorted(image_calls) == sorted(host_calls)
    assert applied["syminfo_metadata"] == [list(pair) for pair in wire]
    assert not any(line.startswith("warning:") for line in stderr.getvalue().splitlines())


def test_missing_optional_symbol_exports_warn_and_keep_results(tmp_path):
    path = tmp_path / "symbol.json"
    path.write_text(json.dumps({"syminfo_metadata": {"account_currency_fx": 1.25},
                                "_pineforge_runner_syminfo": {
                                    "type": "forex", "strings": {"currency": "USD"}}}))
    image = FakeLib()
    image.strategy_set_syminfo_metadata = lambda state, key, value: None
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        applied = run_json.apply_syminfo(image, ST, path)
    assert applied == {"syminfo_metadata": [["account_currency_fx", 1.25]]}
    assert stderr.getvalue().splitlines()[-1:] == [
        "warning: --runner docker does not apply syminfo key(s): "
        "type, currency; the results do not apply them."]


def test_strict_metadata_refuses_missing_optional_symbol_exports(tmp_path, monkeypatch):
    path = tmp_path / "symbol.json"
    path.write_text(json.dumps({
        "_pineforge_runner_syminfo": {
            "type": "forex", "strings": {"currency": "USD"}}}))
    monkeypatch.setenv("PINEFORGE_STRICT_METADATA", "1")
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr), pytest.raises(run_json.SyminfoError) as failure:
        run_json.apply_syminfo(FakeLib(), ST, path)
    assert failure.value.code == "strategy_library_incompatible"
    assert "type, currency" in str(failure.value)
    assert "strategy_set_syminfo_type" in str(failure.value)
    assert "strategy_set_syminfo_string" in str(failure.value)
    assert "--runner ctypes" in str(failure.value)
    assert not any(line.startswith("warning:") for line in stderr.getvalue().splitlines())


def test_old_image_reader_warns_for_forwarded_symbol_keys(tmp_path, tape, monkeypatch):
    generated = tmp_path / "generated.cpp"
    generated.write_text("// generated\n")
    docker = FakeDocker()
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    state = {}
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        result = pf_release_run.run_release(
            generated, tape,
            syminfo={"_pineforge_runner_syminfo": {
                "type": "forex", "strings": {"currency": "USD"}}},
            metadata_warning_state=state)
    assert result == {}
    assert stderr.getvalue() == (
        "warning: --runner docker image reader does not apply syminfo key(s): "
        "type, currency; the results do not apply them "
        "(reader protocol runner-syminfo/v1 unavailable).\n")


def test_release_launcher_forwards_one_current_reader_warning(tmp_path, tape, monkeypatch):
    generated = tmp_path / "generated.cpp"
    generated.write_text("// generated\n")
    warning = (
        "warning: --runner docker does not apply syminfo key(s): "
        "type, currency; the results do not apply them.\n")
    docker = FakeDocker(
        stderr="pineforge: runner syminfo protocol=1\n" + warning + warning)
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    state = {}
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        pf_release_run.run_release(
            generated, tape,
            syminfo={"_pineforge_runner_syminfo": {
                "type": "forex", "strings": {"currency": "USD"}}},
            metadata_warning_state=state)
        pf_release_run.run_release(
            generated, tape,
            syminfo={"_pineforge_runner_syminfo": {
                "type": "forex", "strings": {"currency": "USD"}}},
            metadata_warning_state=state)
    assert stderr.getvalue() == warning
    assert state == {"emitted": True}


def test_strict_metadata_refuses_old_image_reader(tmp_path, tape, monkeypatch):
    generated = tmp_path / "generated.cpp"
    generated.write_text("// generated\n")
    docker = FakeDocker()
    monkeypatch.setattr(pf_release_run, "subprocess", docker)
    with pytest.raises(pf_release_run.ReleaseRunError) as failure:
        pf_release_run.run_release(
            generated, tape,
            syminfo={"_pineforge_runner_syminfo": {
                "type": "forex", "strings": {"currency": "USD"}}},
            strict_metadata=True)
    assert "runner-syminfo/v1 unavailable" in str(failure.value)
    assert "type, currency" in str(failure.value)
    assert "--runner ctypes" in str(failure.value)


MALFORMED_LOTS = {
    "true": True, "false": False, "numeric-string": "0.01", "word": "abc",
    "zero": 0, "zero-float": 0.0, "negative": -1, "negative-float": -0.01,
    "nan": float("nan"), "infinity": float("inf"), "beyond-a-double": 10 ** 400,
    "list": [0.01], "object": {"step": 0.01},
}


@pytest.mark.parametrize("name", ["qty_step", "syminfo_metadata.qty_step",
                                  "mincontract", "syminfo_metadata.mincontract"])
@pytest.mark.parametrize("kind", sorted(MALFORMED_LOTS))
def test_run_strategy_resolver_refuses_a_malformed_lot_key(tmp_path, tape, name, kind):
    # One rule for both lot keys, given directly or under syminfo_metadata: a
    # positive finite JSON number, or null. The refusal comes from the resolver
    # the CLI calls once before it picks a runner, so neither runner starts.
    value = MALFORMED_LOTS[kind]
    if name.startswith("syminfo_metadata."):
        overrides = {"syminfo_metadata": {name.split(".")[1]: value}}
    else:
        overrides = {name: value}
    with pytest.raises(ValueError) as refusal:
        run_strategy.inputs_run_kwargs({"runtime_overrides": overrides}, tmp_path, tape, env={})
    assert str(refusal.value) == f"runtime_overrides.{name} must be a positive finite number"
