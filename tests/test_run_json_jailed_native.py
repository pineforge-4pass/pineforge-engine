"""Real scratch-bound admission through the production JSON runner.

The ordinary CMake row supplies the current engine MODULE on Linux source
builds without sanitizers or coverage. A missing module fails fixture setup;
no engine export is replaced by a transport stand-in.
"""
from __future__ import annotations

import contextlib
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

from test_run_json_nonfinite_report import strict_line

REAL_CDLL = ctypes.CDLL


@pytest.fixture(autouse=True)
def clear_native_profile(monkeypatch):
    # Preserve the original module's profile isolation after moving the cases.
    monkeypatch.delenv("PINEFORGE_RUN_JSON_PROFILE", raising=False)


@pytest.fixture
def native_manifest_module():
    # CMake builds this module from test_scratch_manifest_options.cpp and
    # links it against the current libpineforge. A missing module fails.
    name = os.environ.get("PF_SCRATCH_MANIFEST_MODULE")
    assert name, "build test_scratch_manifest_options.cpp with PINEFORGE_SCRATCH_MANIFEST_MODULE"
    path = Path(name).resolve()
    assert path.is_file(), path
    return path


def native_chart(tmp_path, count):
    path = tmp_path / "native-chart.csv"
    with path.open("w", encoding="utf-8") as output:
        output.write("timestamp,open,high,low,close,volume\n")
        for i in range(count):
            output.write(f"{1736121600000 + i * 60000},100,101,99,100,1\n")
    return path


def native_cli(module, chart, *options, jailed=False, arena_bytes=256 * 1024 * 1024):
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    for name in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES", "PF_SCRATCH_WINDOW_BYTES",
                 "PF_SCRATCH_ARENA_JAILED", "PINEFORGE_RUN_JSON_PROFILE"):
        environment.pop(name, None)
    arguments = ["--so", str(module), "--ohlcv", str(chart),
                 "--input-tf", "1", "--script-tf", "1", *options]
    command = [sys.executable, "-B", str(ROOT / "docker/run_json.py"), *arguments]
    with contextlib.ExitStack() as resources:
        pass_fds = ()
        if jailed:
            import fcntl
            # Real sealed physical backing, with no finite logical manifest.
            libc = REAL_CDLL(None, use_errno=True)
            libc.memfd_create.argtypes, libc.memfd_create.restype = [ctypes.c_char_p, ctypes.c_uint], ctypes.c_int
            arena = libc.memfd_create(b"scratch-manifest-options", 3)
            assert arena >= 0, ctypes.get_errno()
            resources.callback(os.close, arena)
            os.ftruncate(arena, arena_bytes)
            fcntl.fcntl(arena, 1033, 7)
            environment.update(PF_SCRATCH_ARENA_FD="5", PF_SCRATCH_ARENA_BYTES=str(arena_bytes),
                               PF_SCRATCH_WINDOW_BYTES=str(min(arena_bytes, 1024 * 1024)))
            # Reuse the existing exec-only fd/rlimit launcher. It runs the
            # actual JSON CLI and does not inspect or replace strategy exports.
            command = [sys.executable, "-B", str(ROOT / "tests/test_run_json_metrics_only_real.py"),
                       "--exec-jailed", str(arena), "--profile", "jailed", *arguments]
            pass_fds = (arena,)
        result = subprocess.run(command, env=environment, cwd=chart.parent, pass_fds=pass_fds,
                                capture_output=True, text=True)
    assert result.stdout.endswith("\n") and len(result.stdout.splitlines()) == 1, result
    return result.returncode, strict_line(result.stdout), result.stderr


@pytest.mark.parametrize("jailed", [False, True])
@pytest.mark.parametrize("request_other_field", [False, True])
def test_real_omitted_cohort_bound_passes_former_cap(
        native_manifest_module, tmp_path, jailed, request_other_field):
    count = 65537  # Former U=65536, crossed by real distinct entry admissions.
    source = native_chart(tmp_path, count)
    options = ["--scratch-k-e", str(2 * count)] if request_other_field else []
    status, document, error = native_cli(native_manifest_module, source, *options, jailed=jailed)
    assert (status, error) == (0, ""), document
    assert document["diagnostics"]["script_bars_processed"] == count
    assert document["diagnostics"]["run_profile"] == ("jailed" if jailed else "unjailed")


@pytest.mark.parametrize("count", [12, 17])
@pytest.mark.parametrize("option,finite,workload,resource", [
    ("k-e", 2, "cohorts", "erased_index"),
    ("b-e", 14, "cohorts", "erased_data"),
    ("k-c", 1, "cycles", "consumed_index"),
    ("b-c", 14, "cycles", "consumed_data"),
    ("u", 2, "cohorts", "cohort_ids_total"),
    ("b-u", 14, "cohorts", "cohort_ids_total"),
    ("v", 1, "fills", "exit_rank"),
    ("b-v", 7, "fills", "exit_rank"),
])
def test_real_finite_flags_refuse_at_native_counters(
        native_manifest_module, tmp_path, count, option, finite, workload, resource):
    source = native_chart(tmp_path, count)
    inputs = ["--inputs", json.dumps({"workload": workload})]
    status, document, error = native_cli(native_manifest_module, source, *inputs)
    assert (status, error) == (0, ""), document
    assert document["diagnostics"]["script_bars_processed"] == count
    for limit in (0, finite):
        status, document, error = native_cli(
            native_manifest_module, source, *inputs, "--scratch-" + option, str(limit))
        assert (status, error) == (1, ""), document
        assert (document["code"], document["args"]) == (
            "scratch_capacity_exceeded", {"resource": resource})
        assert "summary" not in document


@pytest.mark.parametrize("option,value", [("v", 28)])
def test_real_unsupported_representation_requests_are_typed(
        native_manifest_module, tmp_path, option, value):
    source = native_chart(tmp_path, 3)
    status, document, error = native_cli(
        native_manifest_module, source, "--scratch-" + option, str(value))
    assert (status, error) == (1, ""), document
    assert (document["code"], document["args"]) == ("setting_unsupported", {})
    assert "summary" not in document


@pytest.mark.parametrize("jailed", [False, True])
@pytest.mark.parametrize("count", [2, 8, 65, 66, 130])
@pytest.mark.parametrize("with_other_fields", [False, True])
def test_real_root_limit_equality_overage_and_rootless_controls(
        native_manifest_module, tmp_path, jailed, count, with_other_fields):
    source = native_chart(tmp_path, count)
    inputs = ["--inputs", '{"workload":"roots"}']
    independent = (["--scratch-u", "1", "--scratch-b-u", "7"]
                   if with_other_fields else [])
    for options in (independent, [*independent, "--scratch-r", str(count - 1)]):
        status, document, error = native_cli(
            native_manifest_module, source, *inputs, *options, jailed=jailed)
        assert (status, error) == (0, ""), document
        assert document["diagnostics"]["script_bars_processed"] == count
    for maximum in sorted({0, count - 2}):
        status, document, error = native_cli(
            native_manifest_module, source, *inputs, *independent,
            "--scratch-r", str(maximum), jailed=jailed)
        assert (status, error) == (1, ""), document
        assert (document["code"], document["args"]) == (
            "scratch_capacity_exceeded", {"resource": "successor_roots"})
        assert f"limit {maximum}" in document["error"] and f"need {maximum + 1}" in document["error"]
    # R=0 still admits distinct rootless requests and their cancellations,
    # including the native codec prewarming before the first callback.
    status, document, error = native_cli(
        native_manifest_module, source, "--scratch-r", "0", jailed=jailed)
    assert (status, error) == (0, ""), document
    assert document["diagnostics"]["script_bars_processed"] == count


@pytest.mark.parametrize("optional_roots", [False, True])
def test_real_physical_arena_exhaustion_remains_typed(native_manifest_module, tmp_path, optional_roots):
    source = native_chart(tmp_path, 3000)
    options = ["--scratch-r", "0"] if optional_roots else []
    status, document, error = native_cli(
        native_manifest_module, source, *options, jailed=True, arena_bytes=2 * 1024 * 1024)
    assert (status, error) == (1, ""), document
    assert document["code"] == "report_sink_failed"
    assert document["args"]["reason"] == "arena_exhausted"
    assert "summary" not in document
