"""Jailed/guard controls; execute on the Linux test host, never skip a row."""
import inspect
import json
import os
from pathlib import Path
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import run_json_codes_test as codes
import test_run_json_jailed_profile as jailed
import test_run_json_sink as ordinary


def test_jailed_route_opens_no_report_artifact(tmp_path, monkeypatch):
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)
    opened = []

    def forbidden(*args, **kwargs):
        opened.append((args, kwargs))
        raise AssertionError("jailed route opened a report artifact")

    monkeypatch.setattr(run_json._BoundedJSONFile, "__init__", forbidden)
    files = jailed.forbid_spools(monkeypatch)
    library = jailed.ProfileLibrary()  # existing admitted-arena fixture
    status, stdout, stderr = jailed.invoke(
        tmp_path, monkeypatch, library, "--profile", "jailed")
    assert (status, stderr) == (0, ""), stdout
    assert json.loads(stdout)["summary"]["total_trades"] == 3
    jailed.assert_admission(library, 1, 10, require_arena=True)
    assert opened == [] and files == [] and library.installed == []


def test_bounded_json_file_has_no_unused_strict_default():
    assert "require_disk" not in inspect.signature(run_json._BoundedJSONFile).parameters


@pytest.mark.parametrize("call", [
    'RunFailure("bad", code)',
    'RunFailure("bad", code=code)',
    'RunFailure(*failure)',
    'write_failure("bad", code)',
    'failure_line("bad", code)',
    'OutputsError("bad", "uncatalogued_s4e_failure")',
    'SyminfoError("bad", code)',
    'OutputsError("bad", code=code)',
    'IndirectOutputsError("bad", code)',
    'IndirectSyminfoError("bad", "uncatalogued_s4e_failure")',
])
def test_guard_rejects_uncatalogued_variable(monkeypatch, call):
    source = (
        'class OutputsError(RunFailure, ValueError):\n'
        '    pass\n'
        'class SyminfoError(RunFailure, ValueError):\n'
        '    pass\n'
        'class IndirectOutputsError(OutputsError):\n'
        '    pass\n'
        'class IndirectSyminfoError(SyminfoError):\n'
        '    pass\n'
        'class FixedOutputsError(OutputsError):\n'
        '    def __init__(self, text, reason):\n'
        '        super().__init__(text, "outputs_rejected", {"reason": reason})\n'
        'class IndirectFixedOutputsError(FixedOutputsError):\n'
        '    pass\n'
        'def synthetic():\n'
        '    RunFailure("known", "run_request_invalid")\n'
        '    OutputsError("known", "outputs_rejected")\n'
        '    SyminfoError("known", "syminfo_unreadable")\n'
        '    IndirectOutputsError("known", "outputs_rejected")\n'
        '    IndirectSyminfoError("known", "syminfo_unreadable")\n'
        '    FixedOutputsError("known", "not_declared")\n'
        '    IndirectFixedOutputsError("known", "not_declared")\n'
        '    code = "uncatalogued_s4e_failure"\n'
        '    failure = ("bad", code, {})\n'
    )
    real_read = Path.read_text
    target = ROOT / "docker/run_json.py"

    def read(path, *args, **kwargs):
        return source if path == target else real_read(path, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", read)
    # This source has no forwarding calls: an empty reviewed list prevents a
    # missing production pass-through from masquerading as emitter rejection.
    monkeypatch.setattr(codes, "_CODE_PASSTHROUGHS", [])
    codes.assert_run_json_codes_catalogued()  # the literal-only control passes
    source += f'    {call}\n'
    # At 61ab57dac1 the subclass calls are ignored, so no rejection occurs.
    with pytest.raises(AssertionError, match="non-literal|uncatalogued"):
        codes.assert_run_json_codes_catalogued()


@pytest.mark.parametrize("backing", ["usable", "missing", "unwritable", "tmpfs", "ramfs"])
@pytest.mark.parametrize("spelling", ["absolute", "relative-pathlike"])
def test_explicit_spill_dir_is_not_replaced(tmp_path, monkeypatch, backing, spelling):
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)
    monkeypatch.chdir(tmp_path)
    explicit = tmp_path / "caller-spill"
    supplied = str(explicit) if spelling == "absolute" else Path("caller-spill")
    if backing != "missing":
        explicit.mkdir()
    fallback = tmp_path / "usable-fallback"
    fallback.mkdir()
    identity = explicit.stat() if backing != "missing" else None
    opened, probes, accessed, artifacts = [], [], [], []
    real_open, real_access = os.open, os.access

    def open_directory(path, flags, *args, **kwargs):
        opened.append(os.fspath(path))
        # Make /var/tmp usable regardless of the box's mounts/permissions,
        # while keeping every actual open in the fixture directory.
        return real_open(fallback if os.fspath(path) == "/var/tmp" else path,
                         flags, *args, **kwargs)

    def filesystem_type(fd, libc=None):
        status = os.fstat(fd)
        probes.append((status.st_dev, status.st_ino))
        if identity is not None and probes[-1] == (identity.st_dev, identity.st_ino):
            if backing in {"tmpfs", "ramfs"}:
                return 0x01021994 if backing == "tmpfs" else 0x858458F6
        return 0xEF53  # usable disk filesystem for the fallback and other probes

    def access(path, mode, *args, **kwargs):
        accessed.append(os.fspath(path))
        if os.fspath(path) == str(explicit) and backing == "unwritable":
            return False
        if os.fspath(path) == "/var/tmp":
            return True
        return real_access(path, mode, *args, **kwargs)

    def artifact(*args, **kwargs):
        artifacts.append((args, kwargs))
        raise AssertionError("a named sink must reach the engine before any artifact")

    class EngineLibrary(ordinary.SinkLibrary):
        def __init__(self):
            self.descriptors, self.run_called = [], False
            super().__init__()
            self.strategy_get_last_error = self.export("last_error", lambda state: (
                b"engine spill refusal" if self.run_called else b""))
            self.strategy_get_last_error_code = self.export("last_code", lambda state: (
                b"report_sink_failed" if self.run_called else b""))
            self.strategy_get_last_error_args = self.export("last_args", lambda state: (
                b'{"reason":"scratch_io"}'))

        def install(self, state, pointer):
            descriptor = pointer._obj
            path = os.fsdecode(descriptor.path)
            self.descriptors.append((path, os.fsdecode(descriptor.scratch_dir)))
            self.installed.append(path)
            return 0

        def run(self, *args):
            # The fake engine owns this refusal; the harness must preserve it.
            self.run_called = True

    monkeypatch.setattr(run_json.os, "open", open_directory)
    monkeypatch.setattr(run_json.os, "access", access)
    monkeypatch.setattr(run_json, "_filesystem_type", filesystem_type)
    monkeypatch.setattr(tempfile, "mkstemp", artifact)
    monkeypatch.setattr(tempfile, "TemporaryFile", artifact)
    assert run_json._disk_report_directory(supplied) == str(explicit)
    library = EngineLibrary()
    sink_path = tmp_path / "caller.sink"
    status, stdout, stderr, _ = ordinary.invoke(
        tmp_path, monkeypatch, library=library,
        extra=["--spill-dir", os.fspath(supplied), "--report-sink", str(sink_path)])
    assert library.descriptors == [(str(sink_path), str(explicit))]
    assert library.installed == [str(sink_path)] and library.run_called
    assert opened == [] and probes == [] and accessed == []
    assert (status, stderr) == (1, ""), stdout
    assert len(stdout.splitlines()) == 1
    failure = json.loads(stdout)
    assert failure["code"] == "report_sink_failed"
    assert failure["args"] == {"reason": "scratch_io"}
    assert failure["error"] == "engine spill refusal" and "summary" not in failure
    assert artifacts == [] and not sink_path.exists()


@pytest.mark.parametrize("module,assignment", [
    ("request_feed_inventory", 'CODE = "window_mode_unsupported"'),
    ("selected_window_plan", 'CODE_OUT_OF_MEMORY = "out_of_memory"'),
    ("run_phase_transport", 'code = "harness_internal_error"'),
])
def test_guard_rejects_uncatalogued_imported_constant(monkeypatch, module, assignment):
    target = ROOT / "docker" / (module + ".py")
    real_read = Path.read_text

    def read(path, *args, **kwargs):
        text = real_read(path, *args, **kwargs)
        if path == target:
            assert assignment in text
            target_name = assignment.split(" = ", 1)[0]
            return text.replace(assignment, f'{target_name} = "uncatalogued_s4e_failure"', 1)
        return text

    monkeypatch.setattr(Path, "read_text", read)
    with pytest.raises(AssertionError, match="uncatalogued"):
        codes.test_every_own_code_is_catalogued()
