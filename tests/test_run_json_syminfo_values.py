"""Numeric symbol metadata admission through the real JSON entry point.

The library stand-in from test_run_json_bin records setter arguments and run
calls. Parsing, validation, failure serialization and report assembly stay real.
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
