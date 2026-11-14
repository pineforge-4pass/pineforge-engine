"""Numeric symbol metadata admission through the real JSON entry point.

The library stand-in from test_run_json_bin records setter arguments and run
calls. Parsing, validation, failure serialization and report assembly stay real.
The separate --q716-live-caller mode drives compiled source-host modules through
the live executable for the parent-owned caller metadata witness.
"""

import json

import pytest

import test_run_json_bin as harness


class SymbolLibrary(harness.Library):
    def __init__(self):
        super().__init__()
        self.applied = []
        for key in ("mintick", "pointvalue"):
            name = "strategy_set_syminfo_" + key

            def record(state, value, key=key):
                self.applied.append((key, value))

            setattr(self, name, self.export(name, record))


INVALID_VALUES = (
    pytest.param("mintick", 0, "symbol_feeds_refused",
                 {"reason": "syminfo_mintick_invalid"}, id="mintick-zero"),
    pytest.param("mintick", -0.01, "symbol_feeds_refused",
                 {"reason": "syminfo_mintick_invalid"}, id="mintick-negative"),
    pytest.param("mintick", float("nan"), "symbol_feeds_refused",
                 {"reason": "syminfo_mintick_invalid"}, id="mintick-nan"),
    pytest.param("pointvalue", 0, "symbol_metadata_rejected",
                 {"field": "point_value"}, id="pointvalue-zero"),
    pytest.param("pointvalue", -50, "symbol_metadata_rejected",
                 {"field": "point_value"}, id="pointvalue-negative"),
)


def _observed_run(status, document, stderr, library):
    return (f"observed status={status!r}, document={document!r}, stderr={stderr!r}, "
            f"applied={library.applied!r}, calls={library.calls!r}")


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("key,value,code,args", INVALID_VALUES)
def test_syminfo_invalid_numeric_value_refuses_before_run(
        tmp_path, monkeypatch, wrapped, key, value, code, args):
    # Before the fix float() succeeds, the void setter is called, and main()
    # publishes a successful report with the library's unchanged defaults.
    path = tmp_path / "syminfo.json"
    metadata = {"mintick": 0.25, "pointvalue": 50, key: value}
    path.write_text(json.dumps({"syminfo": metadata} if wrapped else metadata),
                    encoding="utf-8")
    library = SymbolLibrary()
    status, document, stderr, _ = harness.invoke(
        monkeypatch, "--ohlcv", harness.csv_file(tmp_path), "--syminfo", path,
        library=library)
    observed = _observed_run(status, document, stderr, library)

    assert status == 1, (
        f"expected exit status 1 for syminfo.{key}={value!r}, wrapped={wrapped}; {observed}")
    assert set(document) == {"engine", "error", "code", "args"}, (
        f"expected only the failure keys engine/error/code/args; {observed}")
    assert document["engine"] == "pineforge", (
        f"expected engine='pineforge'; {observed}")
    assert (document["code"], document["args"]) == (code, args), (
        f"expected code/args={(code, args)!r}; {observed}")
    assert key in document["error"], (
        f"expected error text containing key {key!r}; {observed}")
    assert json.dumps(value) in document["error"], (
        f"expected error text containing value {json.dumps(value)!r}; {observed}")
    # All values are checked before any setter.
    assert library.applied == [], (
        f"expected no applied setters for the invalid document; {observed}")
    assert "run_backtest_full" not in library.calls, (
        f"expected no strategy execution after metadata refusal; {observed}")
    assert library.calls[-1:] == ["strategy_free"], (
        f"expected the final library call to be strategy_free; {observed}")
    assert stderr == "", f"expected empty stderr for a typed refusal; {observed}"


@pytest.mark.parametrize("value", [0, -0.01, float("nan")],
                         ids=["zero", "negative", "nan"])
def test_symbol_feeds_invalid_mintick_keeps_its_typed_refusal(
        tmp_path, monkeypatch, value):
    path = tmp_path / "symbols.json"
    path.write_text(json.dumps({"symbols": {
        "EXCHANGE:OTHER": {"syminfo": {"mintick": value}, "feeds": {}}}}),
        encoding="utf-8")
    library = SymbolLibrary()
    status, document, stderr, _ = harness.invoke(
        monkeypatch, "--ohlcv", harness.csv_file(tmp_path), "--symbol-feeds", path,
        library=library)
    observed = _observed_run(status, document, stderr, library)

    assert status == 1, (
        f"expected exit status 1 for symbol-feed mintick={value!r}; {observed}")
    assert set(document) == {"engine", "error", "code", "args"}, (
        f"expected only the failure keys engine/error/code/args; {observed}")
    assert document["code"] == "symbol_feeds_refused", (
        f"expected code='symbol_feeds_refused'; {observed}")
    assert document["args"] == {"reason": "syminfo_mintick_invalid"}, (
        f"expected args={{'reason': 'syminfo_mintick_invalid'}}; {observed}")
    assert "mintick" in document["error"], (
        f"expected error text containing key 'mintick'; {observed}")
    assert json.dumps(value) in document["error"], (
        f"expected error text containing value {json.dumps(value)!r}; {observed}")
    assert "run_backtest_full" not in library.calls, (
        f"expected no strategy execution after symbol-feed refusal; {observed}")
    assert stderr == "", f"expected empty stderr for a typed refusal; {observed}"


@pytest.mark.parametrize("metadata,expected", [
    ({}, []),
    ({"mintick": 0.25}, [("mintick", 0.25)]),
    ({"pointvalue": 50}, [("pointvalue", 50.0)]),
    ({"mintick": 0.00001, "pointvalue": 1},
     [("mintick", 0.00001), ("pointvalue", 1.0)]),
    ({"mintick": "0.25", "pointvalue": "50"},
     [("mintick", 0.25), ("pointvalue", 50.0)]),
], ids=["absent", "tick", "point-value", "both", "numeric-text"])
def test_syminfo_positive_and_absent_values_keep_their_application(
        tmp_path, monkeypatch, metadata, expected):
    path = tmp_path / "syminfo.json"
    path.write_text(json.dumps(metadata), encoding="utf-8")
    library = SymbolLibrary()
    status, document, stderr, _ = harness.invoke(
        monkeypatch, "--ohlcv", harness.csv_file(tmp_path), "--syminfo", path,
        library=library)
    observed = _observed_run(status, document, stderr, library)

    assert status == 0, (
        f"expected exit status 0 for valid metadata={metadata!r}; {observed}")
    assert "error" not in document, (
        f"expected a successful report without an error field; {observed}")
    assert "summary" in document, (
        f"expected a successful report containing summary; {observed}")
    assert library.applied == expected, (
        f"expected applied setters={expected!r}; {observed}")
    assert library.calls.count("run_backtest_full") == 1, (
        f"expected exactly one run_backtest_full call; {observed}")
    assert stderr == "", f"expected empty stderr for valid metadata; {observed}"


class QuantityLibrary(SymbolLibrary):
    """Records caller dispatch, not native sizing or TradingView parity."""

    def __init__(self, failure=None):
        super().__init__()

        def metadata(state, key, value):
            self.applied.append((key.decode("utf-8"), value))
            if failure is not None:
                raise failure

        name = "strategy_set_syminfo_metadata"
        setattr(self, name, self.export(name, metadata))


QTY_GRID_VALID_CASES = (
    pytest.param({"qty_step": 0.00001}, [("qty_step", 0.00001)], id="step-only-fine"),
    pytest.param({"qty_step": 0.25}, [("qty_step", 0.25)], id="step-only-coarse"),
    pytest.param({"mincontract": 0.01},
                 [("qty_step", 0.01), ("mincontract", 0.01)], id="minimum-only-control"),
    pytest.param({"qty_step": 0.25, "mincontract": 0.01},
                 [("qty_step", 0.25), ("mincontract", 0.01)], id="explicit-first"),
    pytest.param({"mincontract": 0.01, "qty_step": 0.25},
                 [("qty_step", 0.25), ("mincontract", 0.01)], id="minimum-first"),
    pytest.param({"mincontract": 1, "qty_step": 0.001},
                 [("qty_step", 0.001), ("mincontract", 1.0)], id="conflict-finer-step"),
    pytest.param({"qty_step": 0.01, "mincontract": 0.01},
                 [("qty_step", 0.01), ("mincontract", 0.01)], id="same-value-control"),
    pytest.param({}, [], id="absent"),
    pytest.param({"qty_step": None}, [], id="null-step"),
    pytest.param({"mincontract": None}, [], id="null-minimum"),
    pytest.param({"qty_step": None, "mincontract": None}, [], id="both-null"),
    pytest.param({"qty_step": None, "mincontract": 0.01},
                 [("qty_step", 0.01), ("mincontract", 0.01)], id="null-step-fallback"),
    pytest.param({"mincontract": None, "qty_step": 0.25},
                 [("qty_step", 0.25)], id="null-minimum-explicit"),
    pytest.param({"mintick": 0.5, "qty_step": 0.125, "pointvalue": 2},
                 [("qty_step", 0.125), ("mintick", 0.5), ("pointvalue", 2.0)],
                 id="step-with-other-metadata"),
)

QTY_GRID_INVALID_CASES = (
    pytest.param("0", id="zero"),
    pytest.param("-0.01", id="negative"),
    pytest.param("true", id="true"),
    pytest.param("false", id="false"),
    pytest.param("NaN", id="nan"),
    pytest.param("Infinity", id="infinity"),
    pytest.param("-Infinity", id="negative-infinity"),
    pytest.param("1e9999", id="float-overflow"),
    pytest.param("1" + "0" * 400, id="integer-overflow"),
    pytest.param("1e-9999", id="underflow"),
    pytest.param('"0.25"', id="numeric-text"),
    pytest.param('"invalid"', id="text"),
    pytest.param("[]", id="array"),
    pytest.param("{}", id="object"),
)

# Exact focused collection: 28 + 28 + 28 + 6 + 4 + 2 = 96 caller rows.
QTY_GRID_CASE_COUNTS = {
    "test_syminfo_qty_grid_valid_dispatch": 28,
    "test_syminfo_qty_grid_invalid_step_is_atomic": 28,
    "test_syminfo_qty_grid_invalid_minimum_is_atomic": 28,
    "test_syminfo_qty_grid_later_invalid_field_is_atomic": 6,
    "test_syminfo_qty_grid_missing_setter_is_typed": 4,
    "test_syminfo_qty_grid_setter_failure_is_preserved": 2,
}


def _qty_grid_invoke(tmp_path, monkeypatch, record_property, metadata, wrapped, library=None):
    text = metadata if isinstance(metadata, str) else json.dumps(metadata)
    if wrapped:
        text = '{"syminfo":' + text + "}"
    path = tmp_path / "quantity-syminfo.json"
    path.write_text(text, encoding="utf-8")
    library = QuantityLibrary() if library is None else library
    status, document, stderr, _ = harness.invoke(
        monkeypatch, "--ohlcv", harness.csv_file(tmp_path), "--syminfo", path, library=library)
    # Retain actual main() status, setter arguments and serialized output even
    # when the next assertion fails. This is a Python caller library stand-in.
    record_property("q716_json_call", json.dumps({
        "scope": "JSON caller dispatch/admission",
        "input": text, "main_status": status, "document": document,
        "stderr": stderr, "setter_calls": library.applied, "calls": library.calls,
    }, sort_keys=True))
    return status, document, stderr, library


def _qty_grid_refusal(result, code, args):
    status, document, stderr, library = result
    observed = _observed_run(status, document, stderr, library)
    assert status == 1, observed
    assert set(document) == {"engine", "error", "code", "args"}, observed
    assert document["engine"] == "pineforge", observed
    assert (document["code"], document["args"]) == (code, args), observed
    assert "run_backtest_full" not in library.calls, observed
    assert library.calls[-1:] == ["strategy_free"], observed
    assert stderr == "", observed


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("metadata,expected", QTY_GRID_VALID_CASES)
def test_syminfo_qty_grid_valid_dispatch(
        tmp_path, monkeypatch, record_property, wrapped, metadata, expected):
    result = _qty_grid_invoke(tmp_path, monkeypatch, record_property, metadata, wrapped)
    status, document, stderr, library = result
    observed = _observed_run(*result)
    assert status == 0 and "summary" in document and "error" not in document, observed
    assert library.applied == expected, observed
    assert all(type(value) is float for _, value in library.applied), observed
    grid = {key: value for key, value in expected if key in ("qty_step", "mincontract")}
    assert document["applied_runtime"].get("syminfo", {}) == grid, observed
    assert library.calls.count("run_backtest_full") == 1, observed
    setter_indices = [i for i, name in enumerate(library.calls)
                      if name.startswith("strategy_set_syminfo_")]
    assert all(i < library.calls.index("run_backtest_full") for i in setter_indices), observed
    assert stderr == "", observed


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("raw", QTY_GRID_INVALID_CASES)
def test_syminfo_qty_grid_invalid_step_is_atomic(
        tmp_path, monkeypatch, record_property, wrapped, raw):
    # A valid minimum and other valid metadata must not be installed before an
    # invalid explicit step is discovered. Base main ignores that step instead.
    text = json.dumps({"mintick": 0.5, "pointvalue": 2, "mincontract": 0.01})[:-1]
    text += ',"qty_step":' + raw + "}"
    result = _qty_grid_invoke(tmp_path, monkeypatch, record_property, text, wrapped)
    _qty_grid_refusal(result, "lot_grid_rejected", {})
    _, document, _, library = result
    assert library.applied == [], _observed_run(*result)
    assert document["error"] == (
        "syminfo.qty_step must be a positive finite number, got "
        + json.dumps(json.loads(raw))[:80]), _observed_run(*result)


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("raw", QTY_GRID_INVALID_CASES)
def test_syminfo_qty_grid_invalid_minimum_is_atomic(
        tmp_path, monkeypatch, record_property, wrapped, raw):
    text = json.dumps({"qty_step": 0.25, "mintick": 0.5, "pointvalue": 2})[:-1]
    text += ',"mincontract":' + raw + "}"
    result = _qty_grid_invoke(tmp_path, monkeypatch, record_property, text, wrapped)
    _qty_grid_refusal(result, "lot_grid_rejected", {})
    _, document, _, library = result
    assert library.applied == [], _observed_run(*result)
    assert document["error"] == (
        "syminfo.mincontract must be a positive finite number, got "
        + json.dumps(json.loads(raw))[:80]), _observed_run(*result)


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("key,value,code,args", [
    pytest.param("mintick", -0.5, "symbol_feeds_refused",
                 {"reason": "syminfo_mintick_invalid"}, id="tick"),
    pytest.param("pointvalue", -2, "symbol_metadata_rejected",
                 {"field": "point_value"}, id="point-value"),
    pytest.param("series_origin_ms", False, "syminfo_unreadable",
                 {"reason": "value_type"}, id="origin"),
])
def test_syminfo_qty_grid_later_invalid_field_is_atomic(
        tmp_path, monkeypatch, record_property, wrapped, key, value, code, args):
    metadata = {"qty_step": 0.25, "mincontract": 0.01, key: value}
    result = _qty_grid_invoke(tmp_path, monkeypatch, record_property, metadata, wrapped)
    _qty_grid_refusal(result, code, args)
    assert result[3].applied == [], _observed_run(*result)


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
@pytest.mark.parametrize("metadata", [
    pytest.param({"qty_step": 0.25}, id="step-only"),
    pytest.param({"qty_step": 0.25, "mincontract": 0.01}, id="both"),
])
def test_syminfo_qty_grid_missing_setter_is_typed(
        tmp_path, monkeypatch, record_property, wrapped, metadata):
    library = QuantityLibrary()
    del library.strategy_set_syminfo_metadata
    result = _qty_grid_invoke(
        tmp_path, monkeypatch, record_property, metadata, wrapped, library=library)
    _qty_grid_refusal(result, "strategy_library_incompatible",
                     {"reason": "setter_missing", "missing": "strategy_set_syminfo_metadata"})
    assert library.applied == [], _observed_run(*result)


@pytest.mark.parametrize("wrapped", [False, True], ids=["flat", "wrapped"])
def test_syminfo_qty_grid_setter_failure_is_preserved(
        tmp_path, monkeypatch, record_property, wrapped):
    failure = harness.run_json.RunFailure("metadata setter refused qty_step", "lot_grid_rejected")
    library = QuantityLibrary(failure=failure)
    result = _qty_grid_invoke(tmp_path, monkeypatch, record_property,
                              {"qty_step": 0.125}, wrapped, library=library)
    _qty_grid_refusal(result, "lot_grid_rejected", {})
    assert result[1]["error"] == str(failure), _observed_run(*result)
    assert library.applied == [("qty_step", 0.125)], _observed_run(*result)


def _metadata_witness_head(expected_head, source):
    from pathlib import Path
    import subprocess

    tree = Path(__file__).resolve().parents[1]
    head = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    if head != expected_head:
        raise ValueError(f"engine HEAD {head} differs from requested {expected_head}")
    committed = subprocess.run(["git", "-C", str(tree), "show", head + ":" + source],
                               capture_output=True, check=True).stdout
    if committed != (tree / source).read_bytes():
        raise ValueError(f"{source} must match the named HEAD; overlay only witness files on main")
    return tree, head


def json_metadata_main(argv):
    """Collect and run the exact JSON caller population, retaining raw reports."""
    import argparse
    from collections import Counter
    import hashlib
    from pathlib import Path

    parser = argparse.ArgumentParser(description=json_metadata_main.__doc__)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    tree, head = _metadata_witness_head(args.expected_head, "docker/run_json.py")
    args.out.mkdir(parents=True, exist_ok=False)
    root = args.out.resolve()
    expected = sum(QTY_GRID_CASE_COUNTS.values())
    if expected != 96 or expected <= 0:
        raise ValueError("JSON caller witness must contain exactly 96 cases")
    (root / "inputs.json").write_text(json.dumps({
        "engine_head": head, "scope": "JSON caller dispatch/admission",
        "sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in (Path(__file__).resolve(), tree / "docker/run_json.py")},
    }, sort_keys=True, indent=2) + "\n", encoding="utf-8")

    class Reports:
        def __init__(self):
            self.collected = []
            self.calls = []
            self.reports = []

        def pytest_collection_finish(self, session):
            self.collected = [item.nodeid for item in session.items]
            counts = Counter(item.originalname for item in session.items)
            (root / "collection.json").write_text(json.dumps({
                "expected": expected, "collected": len(self.collected),
                "counts": counts, "nodeids": self.collected,
            }, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            if len(self.collected) <= 0 or counts != QTY_GRID_CASE_COUNTS:
                raise pytest.UsageError("JSON caller population differs from its exact 96-case contract")

        def pytest_runtest_logreport(self, report):
            row = {"nodeid": report.nodeid, "phase": report.when, "outcome": report.outcome,
                   "properties": report.user_properties,
                   "diagnostic": str(report.longrepr) if report.longrepr else None}
            self.reports.append(row)
            with (root / "pytest-reports.jsonl").open("a", encoding="utf-8") as output:
                output.write(json.dumps(row, sort_keys=True) + "\n")
            if report.when == "call":
                self.calls.append(row)

    reports = Reports()
    targets = [str(Path(__file__).resolve()) + "::" + name for name in QTY_GRID_CASE_COUNTS]
    pytest_exit = int(pytest.main([
        "-q", "-rA", "--tb=short", "--maxfail=0", "-p", "no:cacheprovider",
        "-o", "addopts=", "-o", "junit_family=xunit1",
        "--junitxml", str(root / "junit.xml"), *targets,
    ], plugins=[reports]))
    raw_calls = [dict(row["properties"]).get("q716_json_call") for row in reports.calls]
    complete = (len(reports.collected) == expected and len(reports.calls) == expected
                and len({row["nodeid"] for row in reports.calls}) == expected
                and all(raw_calls)
                and all(row["outcome"] in ("passed", "failed") for row in reports.calls)
                and all(row["outcome"] == "passed" for row in reports.reports
                        if row["phase"] != "call"))
    # Preserve pytest's actual status. Missing collection, setup or raw call
    # evidence is a usage failure (4), never an intended main-RED result (1).
    driver_exit = pytest_exit if complete else int(pytest.ExitCode.USAGE_ERROR)
    summary = {
        "engine_head": head, "scope": "JSON caller dispatch/admission",
        "expected_cases": expected, "collected": len(reports.collected),
        "call_rows": len(reports.calls), "population_complete": complete,
        "passed": sum(row["outcome"] == "passed" for row in reports.calls),
        "failed": [row["nodeid"] for row in reports.calls if row["outcome"] == "failed"],
        "pytest_exit": pytest_exit, "driver_exit": driver_exit,
    }
    (root / "summary.json").write_text(
        json.dumps(summary, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True), flush=True)
    return driver_exit


def caller_metadata_main(argv):
    """Focused source-host witness through pineforge-live, run only by its owner.

    Both modules use the original items' entry/close patterns. The synthetic
    flat feeds isolate caller metadata; they are not the measured TV packet.
    Every subprocess exit and its untouched streams are retained before checks.
    """
    import argparse
    import hashlib
    import math
    from decimal import Decimal, ROUND_FLOOR, localcontext
    from pathlib import Path
    import subprocess

    parser = argparse.ArgumentParser(description=caller_metadata_main.__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--item1", type=Path, required=True)
    parser.add_argument("--item2", type=Path, required=True)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    tree, head = _metadata_witness_head(args.expected_head, "runner/main.cpp")
    runner = args.runner.resolve(strict=True)
    modules = {1: args.item1.resolve(strict=True), 2: args.item2.resolve(strict=True)}
    for path in (runner, *modules.values()):
        path.relative_to(tree)
    # A fresh directory prevents stale raw output from satisfying any check.
    args.out.mkdir(parents=True, exist_ok=False)
    root = args.out.resolve()
    pins = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (runner, *modules.values(), Path(__file__).resolve(),
                         tree / "runner/main.cpp",
                         tree / "tests/test_syminfo_mincontract_qty_step.cpp")}
    (root / "inputs.json").write_text(
        json.dumps({"engine_head": head, "tree": str(tree), "sha256": pins},
                   sort_keys=True, indent=2) + "\n", encoding="utf-8")

    cases = (
        ("absent", (), None, None),
        ("explicit-step-control", ("qty_step=0.00001",), "0.00001", None),
        ("mincontract", ("mincontract=0.00001",), "0.00001", None),
        ("mincontract-coarser", ("mincontract=0.0001",), "0.0001", None),
        ("explicit-first", ("qty_step=0.00001", "mincontract=0.0001"), "0.00001", None),
        ("mincontract-first", ("mincontract=0.0001", "qty_step=0.00001"), "0.00001", None),
        ("mincontract-zero", ("mincontract=0",), None, "nonpositive"),
        ("mincontract-negative", ("mincontract=-1",), None, "nonpositive"),
        ("explicit-zero-control", ("qty_step=0",), None, "nonpositive"),
        ("mincontract-null", ("mincontract=null",), None, "number"),
    )
    expected_cases = 2 * 2 * len(cases)
    if expected_cases != 40 or expected_cases <= 0:
        raise ValueError("caller witness must contain exactly 40 cases")
    expected_main_red = {
        f"item{item}-price{price}-{name}"
        for item in modules for price in (3, 7)
        for name, metadata, step, refusal in cases
        if refusal is None and any(value.startswith("mincontract=") for value in metadata)
    }
    rows = []
    epoch = 1704067200000
    stride = 60000
    feed_count = 30
    allowed_warnings = {
        "pineforge-live: warning: compiled strategy lacks execution capabilities; "
        "close-only eligibility cannot be proved (legacy behavior retained)",
        "pineforge-live: warning: compiled strategy lacks checked settings; "
        "legacy settings may be ignored or defaulted",
    }

    def capture(command, directory, name):
        argv = [str(part) for part in command]
        try:
            result = subprocess.run(argv, capture_output=True)
        except (OSError, subprocess.SubprocessError) as error:
            (directory / (name + ".stdout")).write_bytes(getattr(error, "stdout", None) or b"")
            (directory / (name + ".stderr")).write_bytes(getattr(error, "stderr", None) or b"")
            (directory / (name + ".json")).write_text(json.dumps({
                "command": argv, "exit": None, "error": str(error)}) + "\n", encoding="utf-8")
            raise
        (directory / (name + ".stdout")).write_bytes(result.stdout)
        (directory / (name + ".stderr")).write_bytes(result.stderr)
        raw = {"command": argv, "exit": result.returncode}
        (directory / (name + ".json")).write_text(
            json.dumps(raw, sort_keys=True) + "\n", encoding="utf-8")
        return result, raw

    def require(condition, message):
        if not condition:
            raise AssertionError(message)

    for item, module in modules.items():
        for price in (3, 7):
            for name, metadata, step, refusal in cases:
                case_id = f"item{item}-price{price}-{name}"
                directory = root / case_id
                directory.mkdir()
                warmup = directory / "warmup.csv"
                warmup.write_text(
                    "timestamp,open,high,low,close,volume\n"
                    f"{epoch},{price},{price},{price},{price},1\n", encoding="utf-8")
                feed = directory / "feed.jsonl"
                feed.write_text("".join(json.dumps({
                    "type": "bar", "bar": {"ts_open": epoch + index * stride,
                    "o": price, "h": price, "l": price, "c": price, "v": 1}})
                    + "\n" for index in range(1, feed_count + 1)), encoding="utf-8")
                ledger = directory / "orders.sqlite3"
                command = [runner, "run", "--strategy", module, "--warmup", warmup,
                           "--feed", feed, "--ledger", ledger, "--script-tf", "1",
                           "--mode", "bars", "--symbol", "TEST:CALLER"]
                for value in metadata:
                    command.extend(("--syminfo", value))
                row = {"case": case_id, "item": item, "price": price,
                       "metadata": metadata, "step": step, "expected_inputs": feed_count,
                       "expected_rows": 1 if refusal else 6, "rows": 0, "row_exit": 1,
                       "intended_main_refusal": False}
                try:
                    result, row["run"] = capture(command, directory, "run")
                    if refusal:
                        require(result.returncode == 1, "invalid metadata must exit 1")
                        require(not result.stdout.strip(), "refusal must have no success output")
                        require(not ledger.exists(), "refusal must precede ledger creation")
                        messages = ("expected number",) if refusal == "number" else (
                            "symbol tick/point/quantity steps must be positive",
                            "unsupported syminfo key: mincontract")
                        diagnostics = result.stderr.decode("utf-8").splitlines()
                        require(bool(diagnostics) and diagnostics[-1] in {
                            "pineforge-live: " + message for message in messages},
                            "wrong refusal diagnostic")
                        require(set(diagnostics[:-1]) <= allowed_warnings,
                                "unexpected error before refusal")
                        # One admission outcome, not a claim of a filled trade.
                        row["rows"] = 1
                    else:
                        diagnostics = result.stderr.decode("utf-8").splitlines()
                        row["intended_main_refusal"] = (
                            case_id in expected_main_red and result.returncode == 1
                            and not result.stdout.strip() and not ledger.exists()
                            and bool(diagnostics)
                            and diagnostics[-1] == (
                                "pineforge-live: unsupported syminfo key: mincontract")
                            and set(diagnostics[:-1]) <= allowed_warnings)
                        require(result.returncode == 0, "caller run must succeed")
                        require(set(result.stderr.decode("utf-8").splitlines()) <= allowed_warnings,
                                "caller reported an unexpected diagnostic")
                        summary = json.loads(result.stdout)
                        require(summary["inputs_committed"] == feed_count
                                and summary["inputs_processed"] == feed_count,
                                "missing or extra committed inputs")
                        require(summary["webhooks_delivered"] == 0
                                and summary["webhook_failures"] == 0
                                and summary["webhooks_pending"] == 0,
                                "witness must remain journal-only")
                        observed, row["actions"] = capture(
                            [runner, "actions", "--ledger", ledger, "--after", "0"],
                            directory, "actions")
                        require(observed.returncode == 0, "action read must succeed")
                        require(not observed.stderr.strip(), "action read reported an error")
                        events = [json.loads(line) for line in observed.stdout.splitlines()]
                        row["rows"] = len(events)
                        row["action_rows"] = len(events)
                        require(row["rows"] > 0 and row["rows"] == row["expected_rows"],
                                "empty, missing or extra raw actions")
                        with localcontext() as context:
                            context.prec = 50
                            request = (Decimal(10000) / Decimal(price) if item == 1
                                       else Decimal("0.123456789"))
                            quantity = request if step is None else (
                                (request / Decimal(step)).to_integral_value(rounding=ROUND_FLOOR)
                                * Decimal(step))
                        row["expected_quantity"] = str(quantity)
                        for index, event in enumerate(events):
                            order = event["order"]
                            require(event["sequence"] == index + 1, "action sequence differs")
                            require(event["symbol"] == "TEST:CALLER", "symbol binding differs")
                            require(order["leg"] == ("entry" if index % 2 == 0 else "exit"),
                                    "missing or reordered entry/close pair")
                            require(index % 2 == 1 or order["id"] == "L", "wrong entry id")
                            require(order["action"] == ("buy" if index % 2 == 0 else "sell"),
                                    "wrong order direction")
                            require(order["price"] == price, "feed price was not used")
                            require(math.isfinite(order["contracts"]) and math.isclose(
                                order["contracts"], float(quantity), rel_tol=0, abs_tol=1e-10),
                                "declared metadata did not reach quantity sizing")
                    require(row["rows"] > 0 and row["rows"] == row["expected_rows"],
                            "nonpositive or incorrect case count")
                    row["row_exit"] = 0
                except (AssertionError, ValueError, KeyError, TypeError,
                        OSError, subprocess.SubprocessError) as error:
                    row["error"] = str(error)
                rows.append(row)
                # Keep partial evidence even if a later case cannot be run.
                (directory / "case.json").write_text(
                    json.dumps(row, sort_keys=True, indent=2) + "\n", encoding="utf-8")
                print(json.dumps(row, sort_keys=True), flush=True)
    matched = sum(row["row_exit"] == 0 for row in rows)
    raw_actions = sum(row.get("action_rows", 0) for row in rows)
    observed_main_red = {row["case"] for row in rows if row["intended_main_refusal"]}
    failed_cases = {row["case"] for row in rows if row["row_exit"] != 0}
    controls_matched = sum(row["row_exit"] == 0 and row["case"] not in expected_main_red
                           for row in rows)
    main_red_complete = (
        len(rows) == expected_cases and len(expected_main_red) == 16
        and failed_cases == observed_main_red == expected_main_red and controls_matched == 24)
    success = (len(rows) == expected_cases and len(rows) > 0 and matched == expected_cases
               and raw_actions == 144 and raw_actions > 0)
    summary = {"engine_head": head, "cases": len(rows),
               "expected_cases": expected_cases, "matched": matched,
               "raw_actions": raw_actions, "expected_raw_actions": 144,
               "expected_main_red": sorted(expected_main_red),
               "observed_main_red": sorted(observed_main_red),
               "main_controls_matched": controls_matched,
               "main_red_population_complete": main_red_complete,
               "row_exit": 0 if success else 1, "scope": "synthetic caller quantity propagation"}
    (root / "summary.json").write_text(
        json.dumps(summary, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True), flush=True)
    return summary["row_exit"]


if __name__ == "__main__":
    import sys

    if sys.argv[1:2] == ["--q716-json"]:
        raise SystemExit(json_metadata_main(sys.argv[2:]))
    if sys.argv[1:2] == ["--q716-live-caller"]:
        raise SystemExit(caller_metadata_main(sys.argv[2:]))
    raise SystemExit("use pytest, --q716-json, or --q716-live-caller")
