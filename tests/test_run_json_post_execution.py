"""Force failures through main(), with the real phase descriptor.

The engine/planner are existing stand-ins, with real ctypes reports
and the native-style observer callback. Faults come from the library exports,
report-data reads, the standard JSON encoder, or buffer/descriptor IO. The
builder, validator, phase/boundary classifiers and failure writer stay real.

Revision 3 section 2 / Appendix D and amendment 4 (4) pin the expectations
below, before execution. This is entrypoint/ownership evidence, not evidence
of native C++ extraction, app billing, or an external deadline watchdog.
There is no results-digest producer in this tree: its real preflight refusal
is tested without manufacturing a digest phase. Final stdout IO failures are
already covered by the selected-window partial-write row (stderr, no second
document). Ordinary stdout is not filtered here: stdout-spoof controls pin
the extra line explicitly, and do not claim a single-line guarantee.
"""

import json
import sys
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json  # noqa: E402
import run_phase_transport as rpt  # noqa: E402
import test_run_json_nonfinite_report as reports  # noqa: E402

# These are contract expectations, not constants read from the producer.
POST_CODE = "report_post_execution_failed"
POST_TEXT = ("The engine could not finish producing the result after strategy "
             "execution completed.")
ASSEMBLY = ("preflight", "execution", "result_assembly")
SERIALIZATION = ASSEMBLY + ("serialization",)
EXECUTION = ("preflight", "execution")
MODES = ("ordinary", "selected")
FAULTS = (
    pytest.param(lambda: MemoryError("injected resource fault"), "resource", id="resource"),
    pytest.param(lambda: OSError(5, "injected IO fault"), "io", id="io"),
    pytest.param(lambda: RuntimeError("injected generic fault"), "exception", id="exception"),
)
ENGINE_TEXT = "engine injected failure"
TYPED_ENGINE_LINES = (
    pytest.param("strategy_runtime_error", {}, id="strategy-runtime"),
    pytest.param("setting_rejected", {"entrypoint": "strategy_set_input", "input": "length",
                                      "reason": "expected_integer"}, id="setting"),
    pytest.param("strategy_settings_rejected", {"field": "slippage_ticks"}, id="declaration"),
    pytest.param("pine_invalid_argument", {"function": "log", "argument": "value",
                                           "rule": "must_be_positive"}, id="argument"),
    pytest.param("engine_invariant", {}, id="invariant"),
    # The library code getter accepts this literal lexically. It is NOT a
    # cataloged deadline producer or an actual process timeout in this tree.
    pytest.param("deadline", {}, id="deadline-literal"),
)
SPOOFS = (
    pytest.param(
        '{"engine":"pineforge","error":"spoofed","code":"report_post_execution_failed",'
        '"args":{"phase":"serialization","reason":"resource"}}\n', id="failure-line"),
    pytest.param(
        '{"engine":"pineforge","summary":{"total_trades":0,"wins":0,"losses":0,'
        '"win_rate_pct":0,"net_pnl":0,"avg_trade":0,"best_trade":0,"worst_trade":0,'
        '"max_drawdown":0,"bars_processed":0},"trades":[],"equity_curve":[],'
        '"metrics":{"all":{},"longs":{},"shorts":{},"equity":{}}}\n', id="report-line"),
)


class Fault:
    """One forced boundary failure, with an independent reachability witness."""

    def __init__(self, factory, site):
        self.factory = factory
        self.site = site
        self.events = None
        self.hits = 0
        self.prefix = ""

    def bind(self, library):
        self.events = library.events

    def trip(self):
        self.hits += 1
        self.events.append("fault:" + self.site)
        raise self.factory()


@pytest.fixture
def harness(tmp_path):
    return reports.Harness(tmp_path)


def run_with_library(harness, mode, *, configure=None, after_native=None, **options):
    """Extend only the fake library; run_json.main and the harness are real."""
    base = reports.ReportLib

    class Library(base):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            if configure is not None:
                configure(self)

        def _run(self, *args):
            super()._run(*args)
            if after_native is not None:
                after_native(self, args[-1]._obj)
            # Deliberately later than the observer's callback and all fake
            # extraction work: a native fault must occur before this marker.
            self.events.append("library.return")

    with mock.patch.object(reports, "ReportLib", Library):
        return harness.run(mode, **options)


def assert_records(result, phases):
    # The fixed clock makes each received record exact, including the
    # last record; these assertions do not inspect the writer to infer phase.
    expected = []
    for sequence, phase in enumerate(phases, 1):
        record = {"record": "run_phase", "version": 1, "sequence": sequence,
                  "scope": "run", "trial_id": None, "phase": phase, "elapsed_ms": 0}
        if phase == "completed":
            record.update(execution_ms=0, result_assembly_ms=0,
                          results_digest_ms=None, serialization_ms=0)
        expected.append(record)
    assert result.records == expected
    if expected:
        assert result.records[-1] == expected[-1]


def assert_cleanup(result, mode):
    assert [call[0] for call in result.library.calls][-2:] == ["report_free", "strategy_free"]
    assert result.library.observer_descriptor is None
    if mode == "selected":
        assert result.library.window_config is None


def assert_failure(result, mode, code, args, phases, *, stderr="", message=None,
                   stdout_prefix=""):
    # A stdout spoof is the documented exception: retain its exact bytes,
    # then assert that the harness itself wrote exactly one complete line.
    assert result.stdout.startswith(stdout_prefix)
    owned_line = result.stdout[len(stdout_prefix):]
    stdout_error = (
        "stdout was not one whole JSON document: stdout was not exactly one JSON "
        "object line (empty stdout, more than one line, not a JSON object, or a "
        "report prefix before the failure document); "
        f"first 80 characters={owned_line[:80]!r}; length={len(owned_line)}"
    )
    try:
        assert len(owned_line.splitlines()) == 1 and isinstance(
            json.loads(owned_line), dict), stdout_error
    except json.JSONDecodeError as error:
        raise AssertionError(stdout_error) from error
    document = reports.strict_line(owned_line)
    assert result.status == 1
    assert set(document) == {"engine", "error", "code", "args"}
    assert document["engine"] == "pineforge"
    assert (document["code"], document["args"]) == (code, args)
    if message is not None:
        assert document["error"] == message
    if stderr is not None:  # execution exceptions retain a traceback with variable locations
        assert result.stderr == stderr
    assert len(result.stdout.splitlines()) == (2 if stdout_prefix else 1)
    assert result.events.count("stdout.write") == (2 if stdout_prefix else 1)
    assert result.events.count("stdout.flush") == 1
    assert_records(result, phases)
    assert "advance:completed" not in result.events
    if phases:
        assert_cleanup(result, mode)
    return document


def assert_post_failure(result, mode, reason, *, phase="result_assembly"):
    return assert_failure(result, mode, POST_CODE, {"phase": phase, "reason": reason},
                          SERIALIZATION if phase == "serialization" else ASSEMBLY,
                          message=POST_TEXT)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_native_extraction_before_library_return(harness, mode, factory, reason):
    fault = Fault(factory, "native.report_extraction")

    class ExtractionReport(run_json.ReportC):
        def __setattr__(self, name, value):
            if name == "net_profit":
                fault.trip()
            return super().__setattr__(name, value)

    # ReportLib publishes its summary after FakeLib has called the real
    # observer. Fail at that output assignment, still inside run_backtest_full.
    with mock.patch.object(run_json, "ReportC", ExtractionReport):
        result = run_with_library(harness, mode, configure=fault.bind)
    assert fault.hits == 1
    assert result.library.observer_rc == 0
    assert result.library.receipt[2] == 1
    assert result.events.index("advance:result_assembly") < result.events.index(
        "fault:native.report_extraction")
    assert "library.return" not in result.events
    assert result.built == []
    assert_post_failure(result, mode, reason)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_native_trade_accessor_after_library_return(harness, mode, factory, reason):
    fault = Fault(factory, "native.trade_accessor")

    def configure(library):
        fault.bind(library)

        def entry_incarnation(state, index):
            library.calls.append(("strategy_closed_trade_entry_incarnation", state, index))
            fault.trip()

        library.strategy_closed_trade_entry_incarnation = entry_incarnation

    result = run_with_library(harness, mode, configure=configure)
    assert fault.hits == 1
    assert result.events.index("library.return") < result.events.index("fault:native.trade_accessor")
    assert result.built == []
    assert_post_failure(result, mode, reason)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_real_report_builder_summary_read(harness, mode, factory, reason):
    fault = Fault(factory, "python.summary_read")

    class AssemblyReport(run_json.ReportC):
        armed = False

        def __getattribute__(self, name):
            if name == "net_profit" and super().__getattribute__("armed"):
                fault.trip()
            return super().__getattribute__(name)

    def arm(library, report):
        report.armed = True

    # Supply a ctypes report with a failing field read, not a failing builder.
    with mock.patch.object(run_json, "ReportC", AssemblyReport):
        result = run_with_library(harness, mode, configure=fault.bind, after_native=arm)
    assert fault.hits == 1
    assert result.events.index("library.return") < result.events.index("fault:python.summary_read")
    assert result.built == []  # the real builder failed before it returned
    assert_post_failure(result, mode, reason)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_real_report_validation_summary_read(harness, mode, factory, reason):
    fault = Fault(factory, "python.validation_read")

    class Summary(dict):
        def __getitem__(self, key):
            if key == "worst_trade":
                fault.trip()
            return super().__getitem__(key)

    def inject(document):
        document["summary"] = Summary(document["summary"])

    result = run_with_library(harness, mode, configure=fault.bind, inject=inject)
    assert fault.hits == 1
    assert len(result.built) == 1  # real builder completed before real validation
    assert_post_failure(result, mode, reason)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_encoder_failure_after_report_chunks(harness, mode, factory, reason):
    fault = Fault(factory, "serialization.encoder")
    real_encode = json.JSONEncoder.encode
    real_write = run_json._BoundedJSONFile.write

    def encode(encoder, document):
        if document == "summary":
            fault.trip()
        return real_encode(encoder, document)

    def write(buffer, text):
        fault.prefix += text
        return real_write(buffer, text)

    # The streaming serializer still uses the standard encoder for each
    # field. Fail on the actual summary key after real artifact writes.
    with mock.patch.object(json.JSONEncoder, "encode", encode), \
            mock.patch.object(run_json._BoundedJSONFile, "write", write):
        result = run_with_library(harness, mode, configure=fault.bind)
    assert fault.hits == 1
    assert len(fault.prefix) > 32  # actual chunks existed before the failure
    with pytest.raises(json.JSONDecodeError):
        json.loads(fault.prefix)
    # Every report chunk must stay buffered; stdout gets only the failure write.
    assert (not result.stdout.startswith(fault.prefix)
            and fault.prefix not in result.stdout), (
        "stdout was not one whole JSON document: a half-written report prefix "
        "was found before the failure document; "
        f"report prefix first 80 characters={fault.prefix[:80]!r}; "
        f"report prefix length={len(fault.prefix)}; "
        f"stdout first 80 characters={result.stdout[:80]!r}; length={len(result.stdout)}"
    )
    assert result.events.count("stdout.write") == 1, (
        f"observed {result.events.count('stdout.write')} stdout writes; "
        "expected exactly one stdout write (the failure document)"
    )
    assert len(result.built) == 1
    assert_post_failure(result, mode, reason, phase="serialization")


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_report_buffer_write_failure(harness, mode, factory, reason):
    fault = Fault(factory, "serialization.buffer_write")
    real_write = run_json._BoundedJSONFile.write

    def write(buffer, text):
        if text == '"summary"':
            fault.trip()
        fault.prefix += text
        return real_write(buffer, text)

    with mock.patch.object(run_json._BoundedJSONFile, "write", write):
        result = run_with_library(harness, mode, configure=fault.bind)
    assert fault.hits == 1
    assert len(fault.prefix) > 32
    with pytest.raises(json.JSONDecodeError):
        json.loads(fault.prefix)
    assert len(result.built) == 1
    assert_post_failure(result, mode, reason, phase="serialization")


def engine_line(library, code, args, *, observed=True):
    library.call_observer = observed
    library.strategy_get_last_error = lambda state: ENGINE_TEXT.encode()
    library.strategy_get_last_error_code = lambda state: code.encode()
    library.strategy_get_last_error_args = lambda state: json.dumps(args).encode()
    library.strategy_last_run_status = lambda state: 1


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("observed", (True, False), ids=("after-boundary", "execution"))
@pytest.mark.parametrize("code,reason", (("out_of_memory", "resource"),
                                         ("engine_unclassified_error", "exception")))
def test_engine_resource_and_unclassified_lines(harness, mode, observed, code, reason):
    result = run_with_library(harness, mode, configure=lambda library: engine_line(
        library, code, {}, observed=observed))
    assert result.built == []
    if observed:
        assert_post_failure(result, mode, reason)
    else:
        assert_failure(result, mode, code, {}, EXECUTION, message=ENGINE_TEXT)


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("observed", (True, False), ids=("after-boundary", "execution"))
@pytest.mark.parametrize("code,args", TYPED_ENGINE_LINES)
def test_other_engine_codes_keep_code_and_args(harness, mode, observed, code, args):
    result = run_with_library(harness, mode, configure=lambda library: engine_line(
        library, code, args, observed=observed))
    assert result.built == []
    assert_failure(result, mode, code, args, ASSEMBLY if observed else EXECUTION,
                   message=ENGINE_TEXT)


@pytest.mark.parametrize("mode", MODES)
def test_missing_observer_acknowledgement_stops_before_report_access(harness, mode):
    result = run_with_library(harness, mode, configure=lambda library: setattr(
        library, "call_observer", False))
    assert "library.return" in result.events
    assert result.library.receipt is None
    assert result.built == []  # catches removal even if a later phase-order fault has same code
    assert "advance:serialization" not in result.events
    document = assert_failure(result, mode, "harness_internal_error", {}, EXECUTION)
    assert "without the engine announcing the end of execution" in document["error"]


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("factory,reason", FAULTS)
def test_execution_exception_keeps_harness_classification(harness, mode, factory, reason):
    error = factory()
    result = run_with_library(harness, mode, configure=lambda library: setattr(
        library, "raise_before_observer", error))
    assert result.built == []
    assert result.library.observer_rc is None
    assert "library.return" not in result.events
    document = assert_failure(result, mode, "harness_internal_error", {}, EXECUTION,
                              stderr=None,
                              message=f"harness internal error: {type(error).__name__}: {error}")
    assert "Traceback" in result.stderr
    assert "injected" in document["error"]
    assert f"{type(error).__name__}: {error}" in result.stderr


@pytest.mark.parametrize("mode", MODES)
def test_typed_runfailure_after_boundary_keeps_code_and_args(harness, mode):
    args = {"field": "summary.net_pnl", "reason": "nonfinite"}
    fault = Fault(lambda: run_json.RunFailure("injected typed report fault",
                                             "report_result_invalid", args), "native.typed")
    result = run_with_library(harness, mode, configure=fault.bind,
                              after_native=lambda library, report: fault.trip())
    assert fault.hits == 1
    assert "library.return" not in result.events
    assert result.built == []
    assert_failure(result, mode, "report_result_invalid", args, ASSEMBLY,
                   message="injected typed report fault")


@pytest.mark.parametrize("mode", MODES)
def test_raw_typed_phase_error_after_boundary_keeps_its_code(harness, mode):
    # A raw typed exception also travels through the outer classifier; the
    # real descriptor IO row below covers the transport's production wrapping.
    fault = Fault(lambda: rpt.PhaseTransportError("injected typed transport fault",
                                                reason="write_failed",
                                                phase="result_assembly"), "native.typed_phase")
    result = run_with_library(harness, mode, configure=fault.bind,
                              after_native=lambda library, report: fault.trip())
    assert fault.hits == 1
    assert "library.return" not in result.events
    assert result.built == []
    assert_failure(result, mode, "harness_internal_error", {}, ASSEMBLY,
                   message="injected typed transport fault")


@pytest.mark.parametrize("mode", MODES)
def test_phase_transport_write_error_after_boundary_keeps_its_code(harness, mode):
    real_write = rpt.os.write
    refused = []

    def write(fd, frame):
        if b'"record":"run_phase"' in frame:
            record = json.loads(frame)
            if record["phase"] == "serialization":
                refused.append(record)
                raise OSError(5, "injected phase transport IO")
        return real_write(fd, frame)

    # The actual transport turns this descriptor write into PhaseTransportError;
    # no transport helper, phase writer, or typed-failure helper is replaced.
    with mock.patch.object(rpt.os, "write", write):
        result = run_with_library(harness, mode)
    assert len(refused) == 1
    assert refused[0]["phase"] == "serialization"
    assert len(result.built) == 1
    document = assert_failure(result, mode, "harness_internal_error", {}, ASSEMBLY)
    assert "injected phase transport IO" in document["error"]


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("observed", (True, False), ids=("after-boundary", "execution"))
@pytest.mark.parametrize("spoof", SPOOFS)
@pytest.mark.parametrize("channel", ("stderr", "stdout"))
def test_spoofed_lines_do_not_decide_failure_or_phase(harness, mode, observed, spoof, channel):
    def emit(library, report):
        getattr(sys, channel).write(spoof)

    result = run_with_library(harness, mode, configure=lambda library: engine_line(
        library, "out_of_memory", {}, observed=observed), after_native=emit)
    assert result.built == []
    if observed:
        code, args = POST_CODE, {"phase": "result_assembly", "reason": "resource"}
        message, phases = POST_TEXT, ASSEMBLY
    else:
        code, args, message, phases = "out_of_memory", {}, ENGINE_TEXT, EXECUTION
    assert_failure(result, mode, code, args, phases, message=message,
                   stderr=spoof if channel == "stderr" else "",
                   stdout_prefix=spoof if channel == "stdout" else "")


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("spoof", SPOOFS)
@pytest.mark.parametrize("channel", ("stderr", "stdout"))
def test_spoofed_lines_do_not_turn_success_into_failure(harness, mode, spoof, channel):
    result = run_with_library(harness, mode, after_native=lambda library, report: getattr(
        sys, channel).write(spoof))
    assert result.status == 0
    prefix = spoof if channel == "stdout" else ""
    assert result.stdout.startswith(prefix)
    document = reports.strict_line(result.stdout[len(prefix):])
    assert "error" not in document and "code" not in document and "args" not in document
    assert document["engine"] == "pineforge"
    assert document["summary"]["total_trades"] == 2
    assert document["summary"]["net_pnl"] == 3.0
    assert len(document["trades"]) == 2
    assert result.stderr == (spoof if channel == "stderr" else "")
    assert len(result.stdout.splitlines()) == (2 if prefix else 1)
    assert result.events.count("stdout.write") == (2 if prefix else 1)
    assert result.events.count("stdout.flush") == 1
    assert_records(result, SERIALIZATION + ("completed",))
    assert_cleanup(result, mode)


@pytest.mark.parametrize("mode", MODES)
def test_results_digest_request_is_refused_before_execution(harness, mode):
    result = run_with_library(harness, mode, extra=("--results-digest", "sha256/v1"))
    assert_failure(result, mode, "results_digest_unsupported", {"requested": "sha256/v1"}, ())
    assert result.library.calls == []
    assert result.built == []
    assert not any(event.startswith("advance:") for event in result.events)
