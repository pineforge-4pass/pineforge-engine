"""CR-400C host sizing at the shared-library boundary; execute on the lab.

Only shared-library objects are faked. The reader, ctypes bindings, sizing
mirror, scratch setter path and CLI routes are production code.
"""
import contextlib
import ctypes
import io
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_bin as bars
import test_run_json_selected_window as selected


# Canonical shared fixture bytes, transcribed literally, with no newline.
K0 = b'{"strategy":{"ids":{"freshPerBar":0}},"version":1}'
K3 = b'{"strategy":{"ids":{"freshPerBar":3}},"version":1}'
ABSENT = b'{"strategy":{"ids":{}},"version":1}'
UNBOUNDED = b'{"strategy":{"freshPerBarStatus":"unbounded","ids":{}},"version":1}'
STATE = 1099511627777  # Wider than c_int: the receipt must bind s as void*.
REAL_CDLL = ctypes.CDLL


class IdSitesLibrary:
    """Assignable fake C exports, as in the neighbouring run_json harnesses."""

    def __init__(self, text=K3, *, api_version=1, query_status=4,
                 copy_status=0, query_required=None):
        self.text = text
        self.api_version = api_version
        self.query_status, self.copy_status = query_status, copy_status
        self.query_required = query_required
        self.version_calls, self.receipt_calls, self.manifests = 0, [], []
        self.strategy_id_sites_api_version = lambda: self.version()
        self.strategy_id_sites_receipt = lambda *args: self.receipt(*args)
        self.strategy_set_scratch_tables_v1 = lambda *args: self.install(*args)

    def version(self):
        self.version_calls += 1
        return self.api_version

    def receipt(self, state, out, capacity, required, error, error_capacity):
        self.receipt_calls.append((state, out is None, capacity))
        required._obj.value = len(self.text) + 1
        if out is None:
            if self.query_required is not None:
                required._obj.value = self.query_required
            return self.query_status
        if self.copy_status != 0:
            return self.copy_status
        assert capacity >= len(self.text) + 1
        ctypes.memmove(out, self.text + b"\0", len(self.text) + 1)
        return 0

    def install(self, state, pointer):
        descriptor = run_json.ScratchTablesC.from_buffer_copy(
            ctypes.string_at(pointer, ctypes.sizeof(run_json.ScratchTablesC)))
        self.manifests.append((state, descriptor))
        return 0


def install_from_receipt(library, count, reader=None):
    """Exercise the real sizing mirror and apply path even for the mutation."""
    if reader is None:
        reader = run_json._read_id_sites
    k, unbounded, note = reader(library, STATE)
    manifest = run_json.default_scratch_manifest(
        count, fresh_per_bar=k, fixed_fresh=1048576 if unbounded else 0)
    run_json.apply_scratch_tables(library, STATE, manifest)
    state, installed = library.manifests[-1]
    assert state == STATE
    assert (installed.struct_size, installed.version,
            installed.backend, installed.reserved) == (104, 1, 1, 0)
    assert (installed.K_q, installed.B_q, installed.V, installed.B_V) == (0, 0, 27, 6912)
    return (k, unbounded, note), installed


@pytest.mark.parametrize("text,count,k,unbounded,expected", [
    pytest.param(K0, 200000, 0, False, (65536, 262144, 32768), id="k0-floor"),
    pytest.param(K3, 200000, 3, False, (600000, 2400000, 300000), id="k3-below-cap"),
    pytest.param(K3, 1000000, 3, False, (1048576, 4194304, 524288), id="k3-cap"),
    pytest.param(ABSENT, 200000, 1, False, (200000, 800000, 100000), id="unstated"),
    pytest.param(UNBOUNDED, 200000, 1, True,
                 (1048576, 4194304, 524288), id="unbounded"),
    pytest.param(UNBOUNDED, 0, 1, True,
                 (1048576, 4194304, 524288), id="unbounded-empty"),
    pytest.param(b'{"strategy":{"ids":{"freshPerBar":2}},"version":1}',
                 200000, 2, False, (400000, 1600000, 200000), id="heavy-two-sites"),
])
def test_receipt_sizes_the_installed_scratch_fields(text, count, k, unbounded, expected):
    library = IdSitesLibrary(text)
    result, installed = install_from_receipt(library, count)
    assert result[:2] == (k, unbounded)
    assert (installed.U, installed.K_e, installed.K_c) == expected
    assert installed.R == (0 if count == 0 else 400000 if count == 200000 else 2000000)
    assert library.version_calls == 1
    assert library.receipt_calls == [(STATE, True, 0), (STATE, False, len(text) + 1)]
    assert len(library.manifests) == 1


@pytest.mark.parametrize("text", [
    b'not JSON',
    b'{"strategy":{"ids":{"freshPerBar":-1}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":1.0}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":1.5}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":"3"}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":3}},"version":2}',
    b'{"strategy":{"ids":{"freshPerBar":true}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":null}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":NaN}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":18446744073709551616}},"version":1}',
    b'{"strategy":{"ids":{"freshPerBar":3}},"version":true}',
    b'[]',
    b'{"strategy":null,"version":1}',
    b'{"strategy":{"ids":null},"version":1}',
    b'\xff',
])
def test_invalid_receipts_are_ignored_with_a_note(text):
    result, installed = install_from_receipt(IdSitesLibrary(text), 200000)
    assert result[:2] == (1, False) and result[2]
    assert (installed.U, installed.K_e, installed.K_c) == (200000, 800000, 100000)


@pytest.mark.parametrize("missing", [
    ("strategy_id_sites_api_version", "strategy_id_sites_receipt"),
    ("strategy_id_sites_api_version",),
    ("strategy_id_sites_receipt",),
])
def test_missing_optional_exports_keep_the_default(missing):
    library = IdSitesLibrary()
    for name in missing:
        delattr(library, name)
    result, installed = install_from_receipt(library, 200000)
    assert result[:2] == (1, False) and result[2]
    assert (installed.U, installed.K_e, installed.K_c) == (200000, 800000, 100000)
    assert library.version_calls == 0 and library.receipt_calls == []


@pytest.mark.parametrize("options,expected_calls", [
    ({"api_version": 2}, 0),
    ({"query_status": 1}, 1),
    ({"query_status": 0}, 1),
    ({"copy_status": 3}, 2),
    ({"query_required": 0}, 1),
    ({"query_required": 18446744073709551615}, 1),
])
def test_failed_or_invalid_receipt_calls_keep_the_default(options, expected_calls):
    library = IdSitesLibrary(**options)
    result, installed = install_from_receipt(library, 200000)
    assert result[:2] == (1, False) and result[2]
    assert (installed.U, installed.K_e, installed.K_c) == (200000, 800000, 100000)
    assert len(library.receipt_calls) == expected_calls


@pytest.mark.parametrize("name", [
    "strategy_id_sites_api_version", "strategy_id_sites_receipt",
])
def test_optional_export_call_errors_are_ignored(name):
    library = IdSitesLibrary()

    def fail(*args):
        raise OSError("shared-library call failed")

    setattr(library, name, fail)
    result, installed = install_from_receipt(library, 200000)
    assert result[:2] == (1, False) and result[2]
    assert (installed.U, installed.K_e, installed.K_c) == (200000, 800000, 100000)


def test_receipt_binds_the_generated_export_signatures():
    library = IdSitesLibrary()
    assert run_json._read_id_sites(library, STATE) == (3, False, None)
    assert library.strategy_id_sites_api_version.argtypes == []
    assert library.strategy_id_sites_api_version.restype is ctypes.c_uint32
    assert library.strategy_id_sites_receipt.argtypes == [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t), ctypes.c_char_p, ctypes.c_size_t]
    assert library.strategy_id_sites_receipt.restype is ctypes.c_int


def read_id_sites_ignoring_receipt(lib, state):
    """Mutation copy: delete receipt processing and retain only the fallback."""
    return 1, False, "mutation: receipt ignored"


@pytest.mark.parametrize("count,correct,wrong", [
    (200000, 600000, 200000),
    (1000000, 1048576, 1000000),
])
def test_ignoring_receipt_mutation_gives_the_wrong_rows(count, correct, wrong):
    _, control = install_from_receipt(IdSitesLibrary(K3), count)
    assert control.U == correct
    library = IdSitesLibrary(K3)
    _, mutated = install_from_receipt(library, count, read_id_sites_ignoring_receipt)
    assert mutated.U == wrong
    assert mutated.U != correct
    assert library.version_calls == 0 and library.receipt_calls == []


def test_only_declared_need_above_the_cap_refuses():
    # The same 3 * 1,000,000 demand was admitted by the receipt row above.
    with pytest.raises(run_json.RunFailure) as failure:
        run_json.default_scratch_manifest(1000000, fresh_per_bar=3, declared=True)
    assert failure.value.code == "scratch_capacity_exceeded"
    assert failure.value.code_args == {"resource": "cohort"}


class PlannerLibrary:
    """Only the planner's shared-library boundary; its Python bridge stays real."""

    def __init__(self, plan):
        self.plan = plan
        self.pf_selected_plan_version = lambda: 1
        self.pf_plan_selected_primary_v1 = lambda *args: self.fill(*args)

    def fill(self, request, rows, count, output):
        assert count == 10 and request.contents.preroll_bars == 3
        result = output.contents
        for name, value in self.plan.items():
            setattr(result, name, value.encode() if name == "option" else value)
        result.present_mask = 255  # All eight timestamps of full_plan are present.
        return 0


@pytest.mark.parametrize("route,expected", [
    ("ordinary", (100000, 400000, 50000)),
    ("selected", (180000, 720000, 90000)),
])
@pytest.mark.parametrize("profile", ["jailed", "unjailed"])
@pytest.mark.parametrize("text,unbounded", [
    (b'{"strategy":{"ids":{"freshPerBar":20000}},"version":1}', False),
    (UNBOUNDED, True),
])
def test_both_cli_sites_read_once_only_when_jailed(
        tmp_path, monkeypatch, route, expected, profile, text, unbounded):
    receipts = IdSitesLibrary(text)
    plan = selected.full_plan()
    planner = PlannerLibrary(plan)
    if route == "ordinary":
        library = bars.Library()
        source = bars.csv_file(tmp_path)
        argv = ["--so", "unused.so", "--ohlcv", str(source)]
    else:
        library = selected.FakeLib(plan)
        library.pf_abi_version = lambda: 4
        native_run = library.run_backtest_full
        library.run_backtest_full = lambda *args: native_run(*args)
        source = tmp_path / "chart.csv"
        selected.write_ohlcv(source, selected.csv_rows(10))
        artifact, inventory = selected.write_inventory(tmp_path, [])
        argv = selected.selected_argv(artifact, source, inventory)
    for name in ("strategy_id_sites_api_version", "strategy_id_sites_receipt",
                 "strategy_set_scratch_tables_v1"):
        setattr(library, name, getattr(receipts, name))
    library.strategy_require_report_scratch_arena_v1 = lambda state: 0

    def load(path, *args, **kwargs):
        if path is None:
            return REAL_CDLL(path, *args, **kwargs)
        if str(path) == run_json.trusted_planner_path():
            return planner
        return library

    # ctypes.CDLL is the third-party boundary. Do not replace load_strategy,
    # either _make_state, the receipt reader, sizing, or apply_scratch_tables.
    monkeypatch.setattr(ctypes, "CDLL", load)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = run_json.main([*argv, "--profile", profile])
    assert (status, stderr.getvalue()) == (0, ""), stdout.getvalue()
    assert json.loads(stdout.getvalue())["diagnostics"]["run_profile"] == profile
    if profile == "unjailed":
        assert receipts.version_calls == 0
        assert receipts.receipt_calls == [] and receipts.manifests == []
    else:
        assert receipts.version_calls == 1 and len(receipts.receipt_calls) == 2
        assert len(receipts.manifests) == 1
        state, manifest = receipts.manifests[0]
        assert receipts.receipt_calls == [(state, True, 0), (state, False, len(text) + 1)]
        assert (manifest.U, manifest.K_e, manifest.K_c) == (
            (1048576, 4194304, 524288) if unbounded else expected)
        assert manifest.R == (10 if route == "ordinary" else 18)
