"""Singleton regressions. Execute only on the Linux test host."""
import ctypes
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
from run_json_codes_test import assert_run_json_codes_catalogued, catalogued, fake_lib
import test_run_json_sink as ordinary
from test_run_json_sink import frozen  # noqa: F401


@pytest.mark.parametrize("kind", [0x01021994, 0x858458F6], ids=["tmpfs", "ramfs"])
@pytest.mark.parametrize("sink_api", [False, True], ids=["legacy", "sink"])
def test_default_route_on_tmpfs_tmpdir(tmp_path, monkeypatch, frozen, kind, sink_api):
    # The libc statfs result is injected; no mount or /dev/shm is required.
    # All report creation, serialization, sink validation and readback stay real.
    reference = ordinary.invoke(
        tmp_path, monkeypatch, runner=frozen,
        library=ordinary.without_report_sink(ordinary.SinkLibrary()))
    assert reference[0] == 0 and reference[2] == ""
    expected = json.loads(reference[1])
    # Revision 3.1 names finite cagr/calmar on this four-minute curve.
    expected["diagnostics"]["annualization"] = {
        "cagr": "simple_proportional_subday",
        "calmar": "simple_proportional_subday",
    }
    expected["diagnostics"]["run_profile"] = "unjailed"
    expected["diagnostics"]["scratch_arena_jailed"] = False
    expected_bytes = json.dumps(expected, separators=(",", ":"), allow_nan=False) + "\n"
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    monkeypatch.setattr(run_json.sys, "platform", "linux")
    original_cdll = ctypes.CDLL
    checked = []

    class TmpfsLibc:
        def __init__(self, *args, **kwargs):
            self.library = original_cdll(*args, **kwargs)

        def __getattr__(self, name):
            return getattr(self.library, name)

        def fstatfs(self, fd, output):
            checked.append(fd)
            ctypes.cast(output, ctypes.POINTER(ctypes.c_long))[0] = kind
            return 0

    monkeypatch.setattr(run_json.ctypes, "CDLL", TmpfsLibc)
    library = ordinary.SinkLibrary()
    if not sink_api:
        library = ordinary.without_report_sink(library)
    status, stdout, stderr, library = ordinary.invoke(
        tmp_path, monkeypatch, library=library)
    assert checked, "the report must exercise the injected filesystem check"
    if sink_api:
        # This probe marks EVERY candidate (including /var/tmp) as memory
        # backed. The engine sink contract requires refusal before installation.
        assert (status, stderr) == (1, ""), stdout
        failure = json.loads(stdout)
        ordinary.assert_no_run_profile(failure)
        assert failure["code"] == "run_request_invalid"
        assert "TMPDIR" in failure["error"] and "--spill-dir" in failure["error"]
        assert "disk-backed" in failure["error"]
        assert ("tmpfs" if kind == 0x01021994 else "ramfs") in failure["error"]
        assert library.installed == []
        return
    assert (status, stderr) == (0, ""), stdout
    ordinary.assert_run_profile(json.loads(stdout))
    assert stdout == expected_bytes
    assert library.installed == []
    # This legacy producer has no engine sink. Its unjailed JSON artifact can
    # use tmpfs; jailed artifact absence is checked by the misc2 arena row.


def test_every_run_json_code_is_catalogued():
    assert_run_json_codes_catalogued()


_SERIES_EXPORTS = ("pf_output_series_limit_set_checked", "pf_output_series_limit_failure")
_MAP_EXPORT = "pf_map_pairs_limit_set_checked"
_REPLAY_EXPORTS = ("strategy_net_profit_sum", "strategy_report_pointvalue",
                   "strategy_initial_capital", "strategy_set_report_replay_v1")


@pytest.mark.parametrize("missing", [*_SERIES_EXPORTS, _MAP_EXPORT, *_REPLAY_EXPORTS])
def test_missing_optional_export_names_are_in_the_published_catalog(missing):
    exports = (*_SERIES_EXPORTS, _MAP_EXPORT, *_REPLAY_EXPORTS)
    lib = fake_lib(*(name for name in exports if name != missing),
                   returns={_MAP_EXPORT: run_json.PF_SETTINGS_OK})
    with pytest.raises(run_json.StrategyLibraryError) as refusal:
        if missing in _REPLAY_EXPORTS:
            run_json._ReplayCapture(lib, 7, SimpleNamespace(), None)
        else:
            run_json.create_strategy(lib, False, max_output_series=3, max_map_pairs=2)
    assert refusal.value.code == "strategy_library_incompatible"
    assert refusal.value.code_args == {"reason": "symbol_missing", "missing": missing}
    assert catalogued(refusal.value.code, refusal.value.code_args)


@pytest.mark.parametrize("has_setter", [False, True], ids=["legacy", "current"])
@pytest.mark.parametrize("maximum", [None, 0, 17], ids=["omitted", "zero", "positive"])
def test_map_limit_at_strategy_construction(has_setter, maximum):
    applied = []

    def set_limit(pointer, error, capacity):
        applied.append(ctypes.cast(pointer, ctypes.POINTER(ctypes.c_int64))[0]
                       if pointer is not None else None)
        return run_json.PF_SETTINGS_OK

    lib = fake_lib(impl={_MAP_EXPORT: set_limit} if has_setter else {})
    if maximum and not has_setter:
        with pytest.raises(run_json.StrategyLibraryError) as refusal:
            run_json.create_strategy(lib, False, max_map_pairs=maximum)
        assert refusal.value.code_args == {"reason": "symbol_missing", "missing": _MAP_EXPORT}
        assert lib.names() == []
    else:
        assert run_json.create_strategy(lib, False, max_map_pairs=maximum) == 7
        assert lib.names() == ([_MAP_EXPORT, "strategy_create", _MAP_EXPORT]
                               if has_setter else ["strategy_create"])
        assert applied == ([maximum or None, None] if has_setter else [])


@pytest.mark.parametrize("option,label", [("limit-output-series", "series count"),
                                           ("limit-map-pairs", "map-pair count")])
@pytest.mark.parametrize("value", ["1.25", str(1 << 63), "9" * 5000],
                         ids=["fraction", "overflow", "long-decimal"])
def test_bad_decimal_limit_names_the_option_and_exits_two(capsys, option, label, value):
    with pytest.raises(SystemExit) as stop:
        run_json.main(["--so", "unused.so", "--ohlcv", "unused.csv", "--" + option, value])
    out, err = capsys.readouterr()
    assert stop.value.code == 2
    document = json.loads(out)
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": option.replace("-", "_")}
    assert catalogued(document["code"], document["args"])
    assert f"argument --{option}: expected a nonnegative int64 {label}" in document["error"]
    assert err.endswith(document["error"] + "\n")


@pytest.mark.parametrize("convert", [run_json._output_series_count, run_json._map_pairs_count])
def test_decimal_limits_use_the_value_after_leading_zeroes(convert):
    for text, expected in (("0", 0), ("37", 37), ("0" * 5000 + str(2**63 - 1), 2**63 - 1)):
        assert convert(text) == expected


@pytest.mark.parametrize("mode", ["SUMMARY_ONLY", "STREAMED"])
def test_unsupported_report_mode_matches_catalogue(tmp_path, monkeypatch, mode):
    status, stdout, stderr, library = ordinary.invoke(
        tmp_path, monkeypatch, extra=["--report-mode", mode])
    assert status == 1 and stderr == ""
    failure = json.loads(stdout)
    assert failure["code"] == "report_mode_unsupported"
    assert failure["args"] == {"mode": mode, "contract": "pineforge-report/v1"}
    catalogue = json.loads((ROOT / "docker/run_failure_codes.json").read_text())
    entry = catalogue["codes"][failure["code"]]
    assert entry["class"] == "unsupported" and entry["retryable"] is False
    assert set(entry["args"]) == set(failure["args"])
    for name, value in failure["args"].items():
        assert entry["args"][name]["kind"] == "string" and isinstance(value, str)
    assert not library.installed
