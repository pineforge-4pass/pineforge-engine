"""S4E singleton regressions. Execute only on the lab box."""
import ast
import ctypes
import json
from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
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
    assert (status, stderr) == (0, ""), stdout
    assert stdout == expected_bytes
    assert bool(library.installed) is sink_api

    # The strict artifact policy remains available to jailed callers.
    with tempfile.TemporaryFile(mode="w+b", buffering=0, dir=tmp_path) as file:
        with pytest.raises(OSError, match="not tmpfs or ramfs"):
            run_json._BoundedJSONFile(file)
    jailed = ordinary.invoke(
        tmp_path, monkeypatch, library=ordinary.SinkLibrary(),
        extra=["--profile", "jailed", "--spill-dir", str(tmp_path)])
    assert jailed[0] == 1
    assert json.loads(jailed[1])["code"] == "run_request_invalid"


def test_every_run_json_code_is_catalogued():
    tree = ast.parse((ROOT / "docker/run_json.py").read_text(encoding="utf-8"))
    catalogue = json.loads((ROOT / "docker/run_failure_codes.json").read_text())
    emitted = set()
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Name)
             and node.func.id in {"RunFailure", "write_failure", "failure_line"}]
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and any(
                isinstance(base, ast.Name) and base.id == "RunFailure" for base in node.bases):
            calls.extend(child for child in ast.walk(node) if isinstance(child, ast.Call)
                         and isinstance(child.func, ast.Attribute)
                         and child.func.attr == "__init__"
                         and isinstance(child.func.value, ast.Call)
                         and isinstance(child.func.value.func, ast.Name)
                         and child.func.value.func.id == "super")
    for call in calls:
        code = call.args[1] if len(call.args) > 1 else None
        code = next((item.value for item in call.keywords if item.arg == "code"), code)
        if isinstance(code, ast.Constant) and isinstance(code.value, str):
            emitted.add(code.value)
    assert emitted, "the source scan must reach the failure emitters"
    assert not emitted - catalogue["codes"].keys(), sorted(emitted - catalogue["codes"].keys())


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
