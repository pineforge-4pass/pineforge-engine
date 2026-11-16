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
