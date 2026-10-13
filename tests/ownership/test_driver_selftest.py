"""Driver checks only: no codegen, compiler, engine or private fixtures."""

import argparse
import hashlib
import importlib.util
import json
import shlex
import subprocess
import sys
from pathlib import Path

import pytest


DRIVER_PATH = Path(__file__).with_name("driver.py")
SPEC = importlib.util.spec_from_file_location("ownership_probe_driver", DRIVER_PATH)
driver = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = driver
SPEC.loader.exec_module(driver)


def expectation(**changes):
    doc = {
        "outcome": "failed",
        "code": "strategy_value_invalid",
        "args": {"field": "entry.limit_price", "reason": "nonfinite"},
        "min_trades": 0,
        "label": "S",
        "pinned_by": "revision 3 Appendix B: entry.limit_price; Appendix D",
        "native_boundary_control": None,
    }
    doc.update(changes)
    return doc


def failure(**changes):
    observed = driver.observation(
        "failed", "strategy_value_invalid",
        {"field": "entry.limit_price", "reason": "nonfinite"},
        trade_count=0, exit_status=1, stage="runtime",
    )
    observed.update(changes)
    return observed


def success(trades=1):
    return driver.observation(
        "completed", trade_count=trades, exit_status=0, stage="runtime",
    )


def compare(expected, observed, repo_root=None):
    return driver.compare("seed/example", expected, observed, repo_root=repo_root)


def test_schema_loader_accepts_contract_expectation(tmp_path):
    path = tmp_path / "expect.json"
    path.write_text(json.dumps(expectation()), encoding="utf-8")
    assert driver.load_expectation(path) == expectation()


@pytest.mark.parametrize("changes", [
    {"outcome": "success"},
    {"label": "I"},
    {"min_trades": True},
    {"min_trades": -1},
    {"min_trades": 1, "outcome": "failed"},
    {"args": []},
    {"code": None},
    {"pinned_by": ""},
    {"native_boundary_control": False},
    {"typo": 1},
])
def test_schema_loader_rejects_bad_fields(tmp_path, changes):
    path = tmp_path / "expect.json"
    path.write_text(json.dumps(expectation(**changes)), encoding="utf-8")
    with pytest.raises(driver.ProbeError):
        driver.load_expectation(path)


def test_schema_loader_rejects_missing_field(tmp_path):
    doc = expectation()
    del doc["pinned_by"]
    path = tmp_path / "expect.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    with pytest.raises(driver.ProbeError, match="pinned_by"):
        driver.load_expectation(path)


@pytest.mark.parametrize("raw", [
    '{"args":{"field":"entry.limit_price","field":"exit.limit_price"}}',
    '{"args":{"value":NaN}}',
    '{"args":{"value":Infinity}}',
    '{"args":{"value":1e999}}',
])
def test_json_loader_rejects_duplicate_and_nonfinite_values(raw):
    with pytest.raises(driver.ProbeError):
        driver.load_json(raw)


def test_compare_matches_failure_and_args_subset():
    actual = failure(args={
        "field": "entry.limit_price", "reason": "nonfinite", "extra": 17,
    })
    row = compare(expectation(), actual)
    assert row["pass"] is True
    assert row["coverage"] == "RUNTIME COVERAGE"


def test_compare_wrong_args_must_fail():
    # Named killer for the compare-ignores-args mutant.
    row = compare(expectation(), failure(args={
        "field": "exit.limit_price", "reason": "nonfinite",
    }))
    assert row["pass"] is False
    assert "args.field" in row["reason"]


def test_compare_missing_argument_must_fail():
    row = compare(expectation(), failure(args={"field": "entry.limit_price"}))
    assert row["pass"] is False
    assert "args.reason" in row["reason"]


@pytest.mark.parametrize(("wanted", "actual"), [(True, 1), (1, 1.0), ("1", 1)])
def test_args_match_requires_exact_json_types(wanted, actual):
    row = compare(expectation(args={"value": wanted}), failure(args={"value": actual}))
    assert row["pass"] is False


@pytest.mark.parametrize(("observed", "reason"), [
    (success(), "outcome"),
    (failure(code="engine_command_value_invalid"), "code"),
])
def test_compare_outcome_and_code_mismatch(observed, reason):
    row = compare(expectation(), observed)
    assert row["pass"] is False
    assert reason in row["reason"]


def test_completed_comparison_checks_minimum_trade_count():
    wanted = expectation(outcome="completed", code=None, args={}, min_trades=1, label="control")
    assert compare(wanted, success(1))["pass"] is True
    row = compare(wanted, success(0))
    assert row["pass"] is False
    assert "min_trades" in row["reason"]


def compile_refusal():
    return driver.observation(
        "compile_refused", "PF-E0036", {}, exit_status=5, stage="transpile",
    )


def test_compile_refused_without_control_is_fail_and_never_runtime_coverage(tmp_path):
    # Named killer for the compile-refusal-counted-as-covered mutant.
    wanted = expectation(outcome="compile_refused", code="PF-E0036", args={})
    row = compare(wanted, compile_refusal(), repo_root=tmp_path)
    assert row["pass"] is False
    assert row["coverage"] == "COMPILE COVERAGE"
    assert row["runtime_covered"] is False
    assert "native_boundary_control" in row["reason"]


@pytest.mark.parametrize("control", ["missing.cpp", "."])
def test_compile_refused_requires_control_to_be_a_file(tmp_path, control):
    wanted = expectation(
        outcome="compile_refused", code="PF-E0036", args={}, native_boundary_control=control,
    )
    assert compare(wanted, compile_refusal(), repo_root=tmp_path)["pass"] is False


def test_compile_refused_with_existing_control_stays_compile_coverage(tmp_path):
    control = tmp_path / "native_control.cpp"
    control.write_text("// Native row execution belongs to the supervisor.\n", encoding="utf-8")
    wanted = expectation(
        outcome="compile_refused", code="PF-E0036", args={},
        native_boundary_control="native_control.cpp",
    )
    row = compare(wanted, compile_refusal(), repo_root=tmp_path)
    assert row["pass"] is True
    assert row["coverage"] == "COMPILE COVERAGE"
    assert row["runtime_covered"] is False
    assert row["native_boundary_control_exists"] is True


def runtime_line(doc, exit_status):
    return driver.observe_runtime((json.dumps(doc) + "\n").encode(), exit_status)


def test_runtime_reader_accepts_one_typed_failure():
    actual = runtime_line({
        "engine": "pineforge", "error": "invalid source value",
        "code": "strategy_value_invalid",
        "args": {"field": "entry.limit_price", "reason": "nonfinite"},
    }, 1)
    assert compare(expectation(), actual)["pass"] is True


def test_runtime_reader_accepts_completed_report():
    actual = runtime_line({
        "engine": "pineforge", "summary": {"total_trades": 1}, "trades": [{"qty": 1.0}],
    }, 0)
    wanted = expectation(outcome="completed", code=None, args={}, min_trades=1, label="control")
    assert compare(wanted, actual)["pass"] is True


@pytest.mark.parametrize(("doc", "exit_status"), [
    ({"engine": "pineforge", "error": "bad", "code": "strategy_value_invalid", "args": {}}, 0),
    ({"engine": "pineforge", "error": "bad", "code": "strategy_value_invalid", "args": {},
      "trades": []}, 1),
    ({"engine": "pineforge", "error": "uncoded"}, 1),
    ({"engine": "pineforge", "summary": {"total_trades": 1}, "trades": [{}]}, 1),
    ({"engine": "pineforge", "summary": {"total_trades": 1}, "trades": []}, 0),
    ({"engine": "pineforge", "summary": {"total_trades": True}, "trades": [{}]}, 0),
    ({"engine": "other", "summary": {"total_trades": 0}, "trades": []}, 0),
])
def test_runtime_reader_rejects_contradictory_or_untyped_result(doc, exit_status):
    assert runtime_line(doc, exit_status)["outcome"] == "driver_error"


@pytest.mark.parametrize("raw", [b"", b"noise\n{}\n", b"{}\n{}\n", b"\n{}\n", b"not JSON\n"])
def test_runtime_reader_rejects_missing_or_multiple_lines(raw):
    assert driver.observe_runtime(raw, 0)["outcome"] == "driver_error"


def test_result_row_format_and_deliberately_wrong_expectation(tmp_path):
    row = compare(expectation(code="report_result_invalid"), failure())
    assert row["pass"] is False
    assert {"probe", "outcome", "code", "args", "trade_count", "pass", "reason"} <= row.keys()
    assert row["probe"] == "seed/example"
    assert row["outcome"] == "failed"
    assert row["code"] == "strategy_value_invalid"
    assert row["args"] == failure()["args"]
    assert row["trade_count"] == 0
    assert row["reason"]
    report = tmp_path / "report.json"
    driver.write_report(report, [row])
    doc = json.loads(report.read_text(encoding="utf-8"))
    assert doc["results"] == [row]
    assert doc["summary"]["failed"] == 1
    assert doc["summary"]["runtime_covered"] == 0


def test_main_returns_nonzero_for_a_fail_row(tmp_path, monkeypatch):
    # Named killer for the exit-status-always-zero mutant; exercises real main.
    row = compare(expectation(), success())
    monkeypatch.setattr(driver, "run_suite", lambda args: [row])
    report = tmp_path / "report.json"
    status = driver.main([
        "--probes", str(tmp_path), "--prefix", str(tmp_path),
        "--codegen", str(tmp_path), "--work", str(tmp_path / "work"),
        "--report", str(report),
    ])
    assert status != 0
    assert json.loads(report.read_text(encoding="utf-8"))["results"][0]["pass"] is False


def test_main_returns_zero_only_for_pass_rows(tmp_path, monkeypatch):
    row = compare(expectation(), failure())
    monkeypatch.setattr(driver, "run_suite", lambda args: [row])
    assert driver.main([
        "--probes", str(tmp_path), "--prefix", str(tmp_path),
        "--codegen", str(tmp_path), "--work", str(tmp_path / "work"),
        "--report", str(tmp_path / "report.json"),
    ]) == 0


def test_cli_returns_nonzero_for_invalid_expectation(tmp_path):
    # Runs only the driver's schema-failure route, never a toolchain stage.
    probe = tmp_path / "probes" / "bad"
    probe.mkdir(parents=True)
    (probe / "probe.pine").write_text("//@version=6\nstrategy(\"schema check\")\n", encoding="utf-8")
    (probe / "expect.json").write_text(json.dumps(expectation(min_trades=True)), encoding="utf-8")
    report = tmp_path / "report.json"
    result = subprocess.run([
        sys.executable, "-I", str(DRIVER_PATH), "--probes", str(tmp_path / "probes"),
        "--prefix", str(tmp_path / "no-engine"), "--codegen", str(tmp_path / "no-codegen"),
        "--work", str(tmp_path / "work"), "--report", str(report),
    ], capture_output=True, check=False, timeout=30)
    assert result.returncode != 0
    row = json.loads(report.read_text(encoding="utf-8"))["results"][0]
    assert row["pass"] is False
    assert "min_trades" in row["reason"]


def test_compile_command_matches_entrypoint_flags_verbatim(tmp_path):
    prefix, cpp, so = tmp_path / "prefix", tmp_path / "strategy.cpp", tmp_path / "strategy.so"
    entrypoint = (driver.ROOT / "docker" / "entrypoint.sh").read_text(encoding="utf-8")
    start = entrypoint.index("\ng++ ") + 1
    block = entrypoint[start:entrypoint.index("||", start)].replace("\\\n", " ")
    expected = [token.replace("${PREFIX}", str(prefix)).replace("${SRC}", str(cpp))
                .replace("${SO}", str(so)) for token in shlex.split(block)]
    assert driver.compile_command(prefix, cpp, so) == expected


def test_probe_selection_rejects_unknown_and_ambiguous_names(tmp_path):
    for family in ("seed", "doors"):
        path = tmp_path / family / "same" / "probe.pine"
        path.parent.mkdir(parents=True)
        path.write_text("//@version=6\n", encoding="utf-8")
    with pytest.raises(driver.ProbeError, match="unknown"):
        driver.discover_probes(tmp_path, ["typo"])
    with pytest.raises(driver.ProbeError, match="ambiguous"):
        driver.discover_probes(tmp_path, ["same"])
    assert driver.discover_probes(tmp_path, ["seed/same"])[0][0] == "seed/same"


def test_run_options_cannot_replace_driver_owned_artifacts(tmp_path):
    path = tmp_path / "run.json"
    path.write_text(json.dumps({"options": ["--so=elsewhere.so"]}), encoding="utf-8")
    with pytest.raises(driver.ProbeError, match="--so"):
        driver.load_run_options(path)


def test_run_command_has_ordinary_provenance_and_declared_options(tmp_path):
    options = driver.load_run_options(tmp_path / "absent-run.json")
    options.update(input_tf="1", script_tf="1", options=["--chart-tz", "UTC"])
    command = driver.runtime_command(tmp_path / "run_json.py", tmp_path, options)
    assert command == [
        sys.executable, str(tmp_path / "run_json.py"),
        "--so", str(tmp_path / "strategy.so"), "--ohlcv", str(tmp_path / "bars.csv"),
        "--inputs", "", "--overrides", "", "--input-tf", "1", "--script-tf", "1",
        "--bar-magnifier", "", "--magnifier-samples", "4", "--magnifier-dist", "endpoints",
        "--generated-cpp", str(tmp_path / "strategy.cpp"), "--transpiled", "true",
        "--chart-tz", "UTC",
    ]


def test_file_options_resolve_relative_to_probe(tmp_path):
    path = tmp_path / "run.json"
    path.write_text(json.dumps({"options": [
        "--syminfo", "syminfo.json", "--symbol-feeds=feeds.json",
    ]}), encoding="utf-8")
    assert driver.load_run_options(path)["options"] == [
        "--syminfo", str(tmp_path / "syminfo.json"), f"--symbol-feeds={tmp_path}/feeds.json",
    ]


def test_run_suite_snapshots_all_expectations_before_first_execution(tmp_path, monkeypatch):
    probes = tmp_path / "probes"
    pinned = json.dumps(expectation()).encode()
    for name in ("a", "b"):
        probe = probes / name
        probe.mkdir(parents=True)
        (probe / "expect.json").write_bytes(pinned)
        (probe / "probe.pine").write_text("// selftest input, never transpiled\n", encoding="utf-8")
        (probe / "bars.csv").write_text("// selftest input, never loaded\n", encoding="utf-8")
        (probe / "NOTES.md").write_text("Selftest of snapshot ordering.\n", encoding="utf-8")
    monkeypatch.setattr(driver, "resolve_toolchain", lambda *_: (tmp_path, tmp_path, tmp_path))
    calls = []

    def observe_snapshot(work, *_):
        calls.append(work)
        # Simulate an external expectation edit after execution starts. This
        # must not alter either already pinned comparison or its SHA receipt.
        (probes / "b" / "expect.json").write_text(
            json.dumps(expectation(code="report_result_invalid")), encoding="utf-8",
        )
        assert (work / "expect.json").read_bytes() == pinned
        return failure()

    monkeypatch.setattr(driver, "execute_probe", observe_snapshot)
    rows = driver.run_suite(argparse.Namespace(
        probes=probes, only=[], prefix=tmp_path, codegen=tmp_path, work=tmp_path / "work",
    ))
    assert len(calls) == 2
    assert all(row["pass"] for row in rows)
    assert {row["expect_sha256"] for row in rows} == {hashlib.sha256(pinned).hexdigest()}


def test_build_tree_uses_its_checkout_and_scratch_prefix_view(tmp_path):
    # File-layout test only. None of these marker files is executed or linked.
    source, build, codegen, session = [tmp_path / name for name in ("src", "build", "cg", "work")]
    files = {
        source / "include/pineforge/pineforge.h": "header marker\n",
        source / "docker/run_json.py": "harness marker\n",
        build / "lib/libpineforge.a": "archive marker\n",
        codegen / "pineforge_codegen/__init__.py": "package marker\n",
        build / "CMakeCache.txt": f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n",
    }
    for path, text in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    session.mkdir()
    prefix, harness, package_parent = driver.resolve_toolchain(build, codegen, session)
    assert prefix == session / "build-prefix"
    assert (prefix / "include").resolve() == source / "include"
    assert (prefix / "lib/libpineforge.a").resolve() == build / "lib/libpineforge.a"
    assert harness == source / "docker/run_json.py"
    assert package_parent == codegen
    assert not (build / "include").exists()


def test_transpile_signal_cannot_be_accepted_as_compile_refusal(tmp_path, monkeypatch):
    (tmp_path / "transpile.failure.json").write_text(json.dumps(compile_refusal()), encoding="utf-8")
    monkeypatch.setattr(driver, "run_stage", lambda command, *_: subprocess.CompletedProcess(
        command, -9, stdout=b"", stderr=b"",
    ))
    actual = driver.execute_probe(tmp_path, {}, tmp_path, tmp_path, tmp_path)
    assert actual["outcome"] == "driver_error"
    assert actual["stage"] == "transpile"
    assert actual["exit_status"] == -9


def selected_options(tmp_path, *, tokens=None, **changes):
    # Same option layout as S1a's Selected, retained and earlier probes. These
    # values are declared inputs, independent of anything a child prints.
    doc = {
        "input_tf": "1", "script_tf": "1",
        "options": [
            "--report-policy", "selected-window/v1",
            "--window-start-ms", "1704067320000",
            "--window-end-ms", "1704069600000",
            "--preroll-bars", "2", "--fed-start-ms", "1704067200000",
        ] if tokens is None else tokens,
    }
    doc.update(changes)
    path = tmp_path / "run.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    return driver.load_run_options(path)


def test_selected_run_command_has_window_inventory_and_phase_arguments(tmp_path):
    options = selected_options(tmp_path)
    command = driver.runtime_command(tmp_path / "run_json.py", tmp_path, options, phase_fd=17)
    assert command == [
        sys.executable, str(tmp_path / "run_json.py"),
        "--so", str(tmp_path / "strategy.so"), "--ohlcv", str(tmp_path / "bars.csv"),
        "--inputs", "", "--overrides", "", "--input-tf", "1", "--script-tf", "1",
        "--bar-magnifier", "", "--magnifier-samples", "4", "--magnifier-dist", "endpoints",
        "--generated-cpp", str(tmp_path / "strategy.cpp"), "--transpiled", "true",
        *options["options"],
        "--request-feed-inventory", str(tmp_path / "request_feed_inventory.json"),
        "--run-phase-fd", "17",
    ]


def test_selected_run_command_preserves_equals_form_and_zero_preroll(tmp_path):
    tokens = [
        "--report-policy=selected-window/v1", "--window-start-ms=1704067320000",
        "--window-end-ms=1704069600000", "--preroll-bars=0",
        "--fed-start-ms=1704067200000",
    ]
    options = selected_options(tmp_path, tokens=tokens)
    command = driver.runtime_command(tmp_path / "run_json.py", tmp_path, options, phase_fd=19)
    assert command[-9:] == [
        *tokens, "--request-feed-inventory", str(tmp_path / "request_feed_inventory.json"),
        "--run-phase-fd", "19",
    ]


@pytest.mark.parametrize("phase_fd", [None, True, 2, "17"])
def test_selected_run_command_requires_real_phase_descriptor(tmp_path, phase_fd):
    options = selected_options(tmp_path)
    with pytest.raises(driver.ProbeError, match="phase"):
        driver.runtime_command(tmp_path / "run_json.py", tmp_path, options, phase_fd=phase_fd)


@pytest.mark.parametrize("changes", [{"input_tf": ""}, {"script_tf": ""}])
def test_selected_route_requires_explicit_timeframes(tmp_path, changes):
    with pytest.raises(driver.ProbeError, match="timeframe"):
        selected_options(tmp_path, **changes)


@pytest.mark.parametrize("tokens", [
    ["--report-policy", "selected-window/v1"],
    ["--report-policy", "ordinary/v1"],
    ["--report-policy"],
    ["--report-policy=selected-window/v1", "--report-policy=selected-window/v1"],
    ["--window-start-ms", "1704067320000"],
])
def test_selected_route_rejects_incomplete_or_ambiguous_request(tmp_path, tokens):
    with pytest.raises(driver.ProbeError):
        selected_options(tmp_path, tokens=tokens)


@pytest.mark.parametrize("token", [
    "--request-feed-inventory=caller.json", "--run-phase-fd=17",
    "--report-p=selected-window/v1", "--window-start=1704067320000",
])
def test_selected_options_cannot_replace_transport_or_abbreviate_flags(tmp_path, token):
    with pytest.raises(driver.ProbeError):
        selected_options(tmp_path, tokens=[token])


def test_schema_loader_accepts_optional_phase_final(tmp_path):
    wanted = expectation(phase_final="execution")
    path = tmp_path / "expect.json"
    path.write_text(json.dumps(wanted), encoding="utf-8")
    assert driver.load_expectation(path) == wanted


@pytest.mark.parametrize("phase_final", [None, "", True, "run_phase", "unknown"])
def test_schema_loader_rejects_bad_phase_final(tmp_path, phase_final):
    path = tmp_path / "expect.json"
    path.write_text(json.dumps(expectation(phase_final=phase_final)), encoding="utf-8")
    with pytest.raises(driver.ProbeError, match="phase_final"):
        driver.load_expectation(path)


def stub_phase_records(phases):
    records = [
        {"record": "run_phase", "version": 1, "sequence": index + 1,
         "scope": "run", "trial_id": None, "phase": phase, "elapsed_ms": index}
        for index, phase in enumerate(phases)
    ]
    if phases and phases[-1] == "completed":
        records[-1].update(execution_ms=1, result_assembly_ms=1,
                           results_digest_ms=None, serialization_ms=1)
    return records


def test_compare_wrong_phase_final_must_fail():
    # Named killer for the phase-final-not-compared mutant. Outcome/code/args
    # all match, so a lost phase comparison cannot hide behind another failure.
    actual = failure(phase_records=stub_phase_records(["preflight", "execution"]))
    row = compare(expectation(phase_final="result_assembly"), actual)
    assert row["pass"] is False
    assert "phase_final" in row["reason"]
    assert row["phase_records"] == actual["phase_records"]
    assert row["runtime_covered"] is False


def test_compare_matches_phase_final_and_keeps_records(tmp_path):
    records = stub_phase_records(["preflight", "execution"])
    row = compare(expectation(phase_final="execution"), failure(phase_records=records))
    assert row["pass"] is True
    assert row["phase_records"] == records
    report = tmp_path / "report.json"
    driver.write_report(report, [row])
    assert json.loads(report.read_text(encoding="utf-8"))["results"][0]["phase_records"] == records


@pytest.mark.parametrize("records", [None, []])
def test_compare_required_phase_cannot_match_missing_transport(records):
    actual = failure() if records is None else failure(phase_records=records)
    row = compare(expectation(phase_final="execution"), actual)
    assert row["pass"] is False
    assert "phase_final" in row["reason"]


PHASE_CHILD = '''import argparse
import json
import socket
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--run-phase-fd", type=int, required=True)
parser.add_argument("--report-policy", required=True)
parser.add_argument("--request-feed-inventory", required=True)
parser.add_argument("--stub-outcome", choices=("completed", "failed"), default="failed")
parser.add_argument("--stub-wire", default="valid")
args, remaining = parser.parse_known_args()
assert args.report_policy == "selected-window/v1"
assert args.request_feed_inventory.endswith("/request_feed_inventory.json")
phases = ["preflight", "execution"]
if args.stub_outcome == "completed":
    phases += ["result_assembly", "serialization", "completed"]
records = [{"record": "run_phase", "version": 1, "sequence": index + 1,
            "scope": "run", "trial_id": None, "phase": phase, "elapsed_ms": index}
           for index, phase in enumerate(phases)]
if args.stub_outcome == "completed":
    records[-1].update(execution_ms=1, result_assembly_ms=1,
                       results_digest_ms=None, serialization_ms=1)
wire = b"".join((json.dumps(record) + "\\n").encode() for record in records)
if args.stub_wire == "truncated":
    wire = wire[:-1]
elif args.stub_wire == "malformed":
    wire = b"not JSON\\n"
elif args.stub_wire == "empty":
    wire = b""
with socket.socket(fileno=args.run_phase_fd) as transport:
    # Exercise a split record and multiple records in one socket write.
    transport.sendall(wire[:13])
    transport.sendall(wire[13:])
if args.stub_outcome == "failed":
    print(json.dumps({"engine": "pineforge", "error": "stub source failure",
                      "code": "strategy_value_invalid",
                      "args": {"field": "entry.limit_price", "reason": "nonfinite"}}))
    sys.exit(1)
print(json.dumps({"engine": "pineforge", "summary": {"total_trades": 1},
                  "trades": [{"qty": 1.0}],
                  "report_window": {"policy": "selected-window/v1", "applied": True}}))
'''


def phase_child(tmp_path):
    child = tmp_path / "run_json.py"
    child.write_text(PHASE_CHILD, encoding="utf-8")
    return child


@pytest.mark.parametrize("outcome", ["failed", "completed"])
def test_selected_runtime_collects_stub_child_phase_records(tmp_path, outcome):
    child = phase_child(tmp_path)
    options = selected_options(tmp_path)
    options["options"] += ["--stub-outcome", outcome]
    actual = driver.run_selected_runtime(child, tmp_path, options)
    phases = ["preflight", "execution"]
    wanted = expectation(phase_final="execution")
    if outcome == "completed":
        phases += ["result_assembly", "serialization", "completed"]
        wanted = expectation(outcome="completed", code=None, args={}, min_trades=1,
                             label="control", phase_final="completed")
    row = compare(wanted, actual)
    assert row["pass"] is True
    assert row["phase_records"] == stub_phase_records(phases)
    assert row["exit_status"] == (1 if outcome == "failed" else 0)
    assert json.loads((tmp_path / "runtime.phase_records.json").read_text()) == row["phase_records"]
    stdout = (tmp_path / "runtime.stdout").read_bytes()
    assert len(stdout.splitlines()) == 1
    assert json.loads(stdout)["engine"] == "pineforge"
    command = json.loads((tmp_path / "runtime.command.json").read_text())
    assert int(command[command.index("--run-phase-fd") + 1]) >= 3


@pytest.mark.parametrize("wire", ["truncated", "malformed"])
def test_selected_runtime_invalid_phase_stream_cannot_pass(tmp_path, wire):
    child = phase_child(tmp_path)
    options = selected_options(tmp_path)
    options["options"] += ["--stub-wire", wire]
    actual = driver.run_selected_runtime(child, tmp_path, options)
    assert actual["outcome"] == "driver_error"
    assert compare(expectation(), actual)["pass"] is False


def test_selected_runtime_missing_phase_pin_is_fail_row(tmp_path):
    child = phase_child(tmp_path)
    options = selected_options(tmp_path)
    options["options"] += ["--stub-wire", "empty"]
    actual = driver.run_selected_runtime(child, tmp_path, options)
    assert actual["outcome"] == "failed"
    row = compare(expectation(phase_final="execution"), actual)
    assert row["pass"] is False
    assert row["phase_records"] == []


def test_execute_selected_uses_inventory_route_without_ordinary_fallback(tmp_path, monkeypatch):
    # Named killer for selected-falls-back-to-ordinary. Only the transpile,
    # compiler and binder stages are stand-ins; runtime uses the real driver
    # subprocess/descriptor path and the explicit protocol stub above.
    child = phase_child(tmp_path)
    helper = tmp_path / "bind_compiled_inventory.py"
    helper.write_text("# layout marker, never executed\n", encoding="utf-8")
    options = selected_options(tmp_path)
    stages = []
    real_run_stage = driver.run_stage

    def fake_toolchain(command, stage, work, **kwargs):
        stages.append((stage, command))
        if stage == "runtime":
            return real_run_stage(command, stage, work, **kwargs)
        return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")

    monkeypatch.setattr(driver, "run_stage", fake_toolchain)
    actual = driver.execute_probe(tmp_path, options, tmp_path, child, tmp_path / "codegen")
    assert [stage for stage, _ in stages] == ["transpile", "compile", "bind", "runtime"]
    transpile = stages[0][1]
    assert transpile[3] == "--_transpile-selected"
    assert transpile[-1] == str(tmp_path / "request_feed_inventory.source.json")
    bind = stages[2][1]
    assert bind == [
        sys.executable, "-I", str(DRIVER_PATH.resolve()), "--_bind-inventory",
        str(tmp_path / "codegen"), str(helper),
        "--inventory", str(tmp_path / "request_feed_inventory.source.json"),
        "--cpp", str(tmp_path / "strategy.cpp"), "--so", str(tmp_path / "strategy.so"),
        "--out", str(tmp_path / "request_feed_inventory.json"),
    ]
    assert compare(expectation(phase_final="execution"), actual)["pass"] is True


def test_selected_inventory_binding_refusal_never_runs_or_falls_back(tmp_path, monkeypatch):
    options = selected_options(tmp_path)
    child = tmp_path / "run_json.py"
    (tmp_path / "bind_compiled_inventory.py").write_text("# layout marker\n", encoding="utf-8")
    stages = []

    def refuse_binding(command, stage, work, **kwargs):
        stages.append(stage)
        assert stage != "runtime"
        return subprocess.CompletedProcess(command, 14 if stage == "bind" else 0,
                                           stdout=b"", stderr=b"binder refused\n")

    monkeypatch.setattr(driver, "run_stage", refuse_binding)
    actual = driver.execute_probe(tmp_path, options, tmp_path, child, tmp_path)
    assert stages == ["transpile", "compile", "bind"]
    assert actual["outcome"] == "driver_error"
    assert actual["stage"] == "bind"
    assert actual["exit_status"] == 14
    assert compare(expectation(), actual)["pass"] is False


def test_selected_missing_binder_never_uses_another_checkout(tmp_path):
    with pytest.raises(driver.ProbeError, match="bind_compiled_inventory"):
        driver.bind_inventory_command(tmp_path / "run_json.py", tmp_path, tmp_path / "codegen")


def codegen_standin(monkeypatch):
    import types

    package = types.ModuleType("pineforge_codegen")
    errors = types.ModuleType("pineforge_codegen.errors")
    errors.CompileError = type("CompileError", (Exception,), {})
    ordinary_calls = []

    def ordinary_poison(*args, **kwargs):
        ordinary_calls.append((args, kwargs))
        raise AssertionError("selected worker must never call ordinary transpile")

    package.transpile = ordinary_poison
    monkeypatch.setitem(sys.modules, "pineforge_codegen", package)
    monkeypatch.setitem(sys.modules, "pineforge_codegen.errors", errors)
    monkeypatch.setattr(sys, "path", sys.path.copy())
    return package, ordinary_calls


def test_selected_transpile_preserves_codegen_inventory_and_cpp_bytes(tmp_path, monkeypatch):
    package, ordinary_calls = codegen_standin(monkeypatch)
    calls = []
    source = "//@version=6\n// é\n"
    cpp_text = "// generated é\n// exact UTF-8 bytes\n"
    # A stand-in's marker, deliberately not an admission inventory schema. The
    # driver must serialize what the API returns without constructing a schema.
    inventory = {"producer_marker": "only the codegen stand-in returned this"}

    def with_inventory(pine, **kwargs):
        calls.append((pine, kwargs))
        return {"cpp": cpp_text, "request_feed_inventory": inventory}

    package.transpile_with_request_inventory = with_inventory
    pine, cpp, diagnostic, sidecar = [tmp_path / name for name in (
        "strategy.pine", "strategy.cpp", "transpile.failure.json", "inventory.source.json",
    )]
    pine.write_text(source, encoding="utf-8")
    assert driver.transpile_worker(tmp_path, pine, cpp, diagnostic, sidecar) == 0
    assert calls == [(source, {"filename": "strategy.pine"})]
    assert ordinary_calls == []
    assert cpp.read_bytes() == cpp_text.encode("utf-8")
    assert json.loads(sidecar.read_bytes()) == inventory
    assert not diagnostic.exists()


def test_selected_missing_inventory_api_is_error_without_ordinary_fallback(tmp_path, monkeypatch):
    _, ordinary_calls = codegen_standin(monkeypatch)
    pine, cpp, diagnostic, sidecar = [tmp_path / name for name in (
        "strategy.pine", "strategy.cpp", "transpile.failure.json", "inventory.source.json",
    )]
    pine.write_text("//@version=6\n", encoding="utf-8")
    assert driver.transpile_worker(tmp_path, pine, cpp, diagnostic, sidecar) != 0
    actual = json.loads(diagnostic.read_bytes())
    assert actual["outcome"] == "driver_error"
    assert actual["code"] == "driver_toolchain_invalid"
    assert ordinary_calls == []
    assert not sidecar.exists()
    assert not cpp.exists()


def test_selected_binder_worker_delegates_helper_and_propagates_refusal(tmp_path, monkeypatch):
    helper = tmp_path / "bind_compiled_inventory.py"
    receipt = tmp_path / "bind.receipt.json"
    # Execute a Python helper stand-in only, with no inventory or library.
    helper.write_text(
        "import json, sys\nfrom pathlib import Path\n"
        "Path(sys.argv[1]).write_text(json.dumps({'argv': sys.argv[2:], 'parent': sys.path[0]}))\n"
        "raise SystemExit(14)\n", encoding="utf-8",
    )
    monkeypatch.setattr(sys, "path", sys.path.copy())
    monkeypatch.setattr(sys, "argv", sys.argv.copy())
    arguments = [str(receipt), "--inventory", "source.json", "--cpp", "strategy.cpp",
                 "--so", "strategy.so", "--out", "bound.json"]
    with pytest.raises(SystemExit) as ended:
        driver.bind_inventory_worker(tmp_path / "codegen", helper, *arguments)
    assert ended.value.code == 14
    assert json.loads(receipt.read_bytes()) == {
        "argv": arguments[1:], "parent": str(tmp_path / "codegen"),
    }


def test_selected_suite_snapshots_phase_pin_before_first_execution(tmp_path, monkeypatch):
    probes = tmp_path / "probes"
    pinned = json.dumps(expectation(phase_final="execution")).encode()
    records = stub_phase_records(["preflight", "execution"])
    for name in ("a", "b"):
        probe = probes / name
        probe.mkdir(parents=True)
        (probe / "expect.json").write_bytes(pinned)
        (probe / "probe.pine").write_text("// never transpiled\n", encoding="utf-8")
        (probe / "bars.csv").write_text("// never loaded\n", encoding="utf-8")
        (probe / "NOTES.md").write_text("Selected snapshot check.\n", encoding="utf-8")
        selected_options(probe)
    monkeypatch.setattr(driver, "resolve_toolchain", lambda *_: (tmp_path, tmp_path, tmp_path))
    calls = []

    def observed(work, *_):
        calls.append(work)
        (probes / "b" / "expect.json").write_text(
            json.dumps(expectation(phase_final="completed")), encoding="utf-8",
        )
        assert (work / "expect.json").read_bytes() == pinned
        return failure(phase_records=records)

    monkeypatch.setattr(driver, "execute_probe", observed)
    rows = driver.run_suite(argparse.Namespace(
        probes=probes, only=[], prefix=tmp_path, codegen=tmp_path, work=tmp_path / "work",
    ))
    assert len(calls) == 2
    assert all(row["pass"] for row in rows)
    assert all(row["phase_records"] == records for row in rows)
    assert {row["expect_sha256"] for row in rows} == {hashlib.sha256(pinned).hexdigest()}


def test_selected_stage_error_closes_phase_reader_and_keeps_records(tmp_path, monkeypatch):
    import os
    import socket

    options = selected_options(tmp_path)
    records = stub_phase_records(["preflight"])

    def stage_error(command, stage, work, *, pass_fds):
        assert stage == "runtime"
        with socket.socket(fileno=os.dup(pass_fds[0])) as transport:
            transport.sendall((json.dumps(records[0]) + "\n").encode())
        raise driver.ProbeError("stub stage timeout")

    monkeypatch.setattr(driver, "run_stage", stage_error)
    actual = driver.run_selected_runtime(tmp_path / "run_json.py", tmp_path, options)
    assert actual["outcome"] == "driver_error"
    assert actual["detail"] == "stub stage timeout"
    assert actual["phase_records"] == records
    assert compare(expectation(), actual)["pass"] is False
