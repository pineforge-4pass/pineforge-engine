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
