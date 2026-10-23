"""S4E jailed/guard controls; execute on the lab box, never skip a row."""
import inspect
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import run_json_codes_test as codes
import test_run_json_jailed_profile as jailed


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
])
def test_guard_rejects_uncatalogued_variable(monkeypatch, call):
    source = (
        'def synthetic():\n'
        '    RunFailure("known", "run_request_invalid")\n'
        '    code = "uncatalogued_s4e_failure"\n'
        '    failure = ("bad", code, {})\n'
        f'    {call}\n'
    )
    real_read = Path.read_text
    target = ROOT / "docker/run_json.py"

    def read(path, *args, **kwargs):
        return source if path == target else real_read(path, *args, **kwargs)

    monkeypatch.setattr(Path, "read_text", read)
    # On b08701f6ef the existing guard silently ignores the variable/starred
    # code and returns successfully, so this assertion is a behavioral red.
    with pytest.raises(AssertionError, match="non-literal|uncatalogued"):
        codes.test_every_own_code_is_catalogued()


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
