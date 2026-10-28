"""Singleton regressions. Execute only on the Linux test host."""
import ctypes
import json
from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
from run_json_codes_test import assert_run_json_codes_catalogued
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
