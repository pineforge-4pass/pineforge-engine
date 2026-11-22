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
import sys
from pathlib import Path

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
    """The docker executable and nothing else: put in place of pf_release_run's
    subprocess module, it answers `docker image inspect` and `docker run`. For
    a run it keeps the argv and the text of every file mounted as
    /in/syminfo.json, read while the launcher still holds the file."""

    MOUNTED = ":/in/syminfo.json:ro"

    class Exit0:
        returncode, stderr = 0, ""

        def __init__(self, stdout):
            self.stdout = stdout

    def __init__(self):
        self.argv, self.documents = None, []

    def run(self, cmd, **options):
        if cmd[:3] == ["docker", "image", "inspect"]:
            config = {"Entrypoint": ["/usr/local/bin/entrypoint.sh"], "Cmd": None}
            return self.Exit0(json.dumps([{"Id": "sha256:" + "0" * 64, "Config": config}]))
        self.argv = list(cmd)
        mounts = [cmd[i + 1] for i, word in enumerate(cmd[:-1]) if word == "-v"]
        self.documents = [Path(mount[:-len(self.MOUNTED)]).read_text(encoding="utf-8")
                          for mount in mounts if mount.endswith(self.MOUNTED)]
        return self.Exit0("{}\n")


def test_release_launcher_mounts_the_symbol_document_with_both_lot_keys(tmp_path, tape,
                                                                         monkeypatch):
    # pf_release_run.run_release writes the document itself: only the docker
    # executable is replaced, and it reads the file the launcher mounts.
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
    # document the docker runner's launcher mounts for the image.
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


MALFORMED_LOTS = {
    "true": True, "false": False, "numeric-string": "0.01", "word": "abc",
    "zero": 0, "zero-float": 0.0, "negative": -1, "negative-float": -0.01,
    "nan": float("nan"), "infinity": float("inf"), "beyond-a-double": 10 ** 400,
    "list": [0.01], "object": {"step": 0.01},
}


@pytest.mark.parametrize("name", ["qty_step", "syminfo_metadata.qty_step",
                                  "mincontract", "syminfo_metadata.mincontract"])
@pytest.mark.parametrize("kind", sorted(MALFORMED_LOTS))
def test_run_strategy_refuses_a_malformed_lot_key_for_both_runners(tmp_path, tape, name, kind):
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
