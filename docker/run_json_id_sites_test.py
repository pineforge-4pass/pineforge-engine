"""Host sizing at the shared-library boundary.

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


# Design rule: occupancy is fresh per trial, while admission depends
# only on the fed bar count and the module's receipt.
SCRATCH_MAXIMA = (
    "K_q", "B_q", "K_e", "B_e", "K_c", "B_c", "U", "B_U", "V", "B_V", "R")
K1 = b'{"strategy":{"ids":{"freshPerBar":1}},"version":1}'
DESIGN_ROUTES = ("ordinary", "ordinary-bench", "selected")


def scratch_maxima(manifest):
    return {name: int(getattr(manifest, name)) for name in SCRATCH_MAXIMA}


def assert_scratch_invariant(*groups):
    """Compare every setter snapshot, including every repeat/body installation."""
    assert len(groups) >= 2 and all(groups), "missing independent CLI executions"
    snapshots = [manifest for group in groups for _, manifest in group]
    expected = scratch_maxima(snapshots[0])
    for manifest in snapshots[1:]:
        assert scratch_maxima(manifest) == expected, (
            "scratch manifest changed with trial count or trade history")
    return expected


def expected_scratch_maxima(count, k, *, unbounded=False):
    """P0 formula in run_json.py, not values obtained from an executed run.

    An erased key has four rows per cohort, a consumed key one per two
    cohorts; their byte factors are 96 and 129. A cohort id uses 64 bytes.
    q is retired, V is fixed, and the replacement reserve is two per bar.
    """
    rows = max(65536, min(1048576, k * count + (1048576 if unbounded else 0)))
    erased, consumed = 4 * rows, rows // 2
    return dict(zip(SCRATCH_MAXIMA, (
        0, 0, erased, 96 * erased, consumed, 129 * consumed,
        rows, 64 * rows, 27, 6912, 2 * count)))


def drive_design_rule_cli(tmp_path, monkeypatch, *, route, text, count,
                          trials=1, trade_count=0):
    """Use main -> real reader -> real sizing/apply -> copied C descriptor.

    The only substituted boundary is CDLL (strategy and trusted planner).
    Selected mode has no multi-run request; its trials are separate CLI
    requests. Ordinary bench additionally covers the in-request repeat loop,
    which always executes one extra body run after the timed trials.
    """
    receipts = IdSitesLibrary(text)
    created, executed, freed = [], [], []
    if route == "selected":
        assert count == 10  # The neighbouring planner fixture supplies nine.
        plan = selected.full_plan()
        planner = PlannerLibrary(plan)
        library = selected.FakeLib(plan)
        library.pf_abi_version = lambda: run_json.EXPECTED_PF_ABI
        source = tmp_path / "design-rule-selected.csv"
        selected.write_ohlcv(source, selected.csv_rows(count))
        artifact, inventory = selected.write_inventory(tmp_path, [])
        argv = selected.selected_argv(artifact, source, inventory)
        fed_count = plan["fed_input_bars"]
    else:
        assert route in ("ordinary", "ordinary-bench")
        planner = None
        library = bars.Library()
        source = bars.csv_file(tmp_path, name="design-rule.csv", count=count)
        argv = ["--so", "unused.so", "--ohlcv", str(source),
                "--input-tf", "1", "--script-tf", "1"]
        fed_count = count
    native_run = library.run_backtest_full
    # Keep native report storage alive through serialization/report_free.
    trade_storage = (run_json.TradeC * trade_count)()

    def create(params):
        state = STATE + len(created)
        created.append(state)
        return state

    def run(state, rows, n, *args):
        assert n == fed_count
        assert [handle for handle, _ in receipts.manifests] == created
        assert state == created[len(executed)]
        native_run(state, rows, n, *args)
        report = args[-1]._obj
        report.input_bars_processed = report.script_bars_processed = n
        report.input_tf_seconds = report.script_tf_seconds = 60
        report.script_tf_ratio = 1
        first_trade_bar = plan["preroll_input_bars"] if route == "selected" else 0
        for index, trade in enumerate(trade_storage):
            bar_index = first_trade_bar + index * (n - first_trade_bar) // trade_count
            trade.entry_time = trade.exit_time = rows[bar_index].timestamp
            trade.entry_bar_index = trade.exit_bar_index = bar_index
            trade.entry_price = trade.exit_price = rows[bar_index].close
            trade.qty, trade.is_long = 1, 1
        if trade_count:
            report.trades = ctypes.cast(trade_storage, ctypes.POINTER(run_json.TradeC))
        report.total_trades = report.trades_len = trade_count
        report.metrics.all.num_trades = report.metrics.longs.num_trades = trade_count
        report.metrics.all.num_even = report.metrics.longs.num_even = trade_count
        executed.append(state)

    library.strategy_create = create
    library.strategy_free = lambda state: freed.append(state)
    library.run_backtest_full = run
    for name in ("strategy_id_sites_api_version", "strategy_id_sites_receipt",
                 "strategy_set_scratch_tables_v1"):
        setattr(library, name, getattr(receipts, name))
    library.strategy_require_report_scratch_arena_v1 = lambda state: 0

    def load(path, *args, **kwargs):
        if path is None:
            return REAL_CDLL(path, *args, **kwargs)
        if str(path) == run_json.trusted_planner_path():
            assert planner is not None
            return planner
        return library

    monkeypatch.setattr(ctypes, "CDLL", load)
    if route == "ordinary-bench":
        argv += ["--bench", "--warmup", "0", "--repeats", str(trials)]
    requests = 1 if route == "ordinary-bench" else trials
    for _ in range(requests):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = run_json.main([*argv, "--profile", "jailed"])
        assert (status, stderr.getvalue()) == (0, ""), stdout.getvalue()
        document = json.loads(stdout.getvalue())
        assert document["diagnostics"]["run_profile"] == "jailed"
        assert document["input"]["bars"] == fed_count
        assert document["summary"]["total_trades"] == trade_count
        assert len(document["trades"]) == trade_count
        if route == "ordinary-bench":
            timing = document["diagnostics"]["timing"]
            assert (timing["warmup"], timing["repeats"]) == (0, trials)
            assert len(timing["samples_ns"]) == trials

    runs = trials + 1 if route == "ordinary-bench" else trials
    assert len(created) == runs
    assert created == executed == freed
    assert [state for state, _ in receipts.manifests] == created
    assert receipts.version_calls == runs
    assert receipts.receipt_calls == [
        call for state in created
        for call in ((state, True, 0), (state, False, len(text) + 1))]
    for _, manifest in receipts.manifests:
        assert (manifest.struct_size, manifest.version,
                manifest.backend, manifest.reserved) == (104, 1, 1, 0)
    return receipts.manifests


@pytest.mark.parametrize("route", DESIGN_ROUTES)
@pytest.mark.parametrize("text,unbounded", [(K3, False), (UNBOUNDED, True)])
def test_design_rule_trial_count_does_not_size_scratch(
        tmp_path, monkeypatch, route, text, unbounded):
    count = 10 if route == "selected" else 24000
    groups = [
        drive_design_rule_cli(tmp_path, monkeypatch, route=route, text=text,
                              count=count, trials=trials)
        for trials in (1, 2, 5)]
    maxima = assert_scratch_invariant(*groups)
    # Independent arithmetic: k3 at 24,000 bars needs 72,000 cohort rows.
    fed_count = count - 1 if route == "selected" else count
    assert maxima == expected_scratch_maxima(
        fed_count, 1 if unbounded else 3, unbounded=unbounded)


@pytest.mark.parametrize("route", DESIGN_ROUTES)
@pytest.mark.parametrize("text", [K3, UNBOUNDED])
def test_design_rule_trade_history_does_not_size_scratch(
        tmp_path, monkeypatch, route, text):
    # The fake reports thousands of actual TradeC rows, not just a counter.
    # A second/third fresh state also observes the host after a busy run.
    count = 10 if route == "selected" else 24000
    groups = [
        drive_design_rule_cli(tmp_path, monkeypatch, route=route, text=text,
                              count=count, trials=3, trade_count=trade_count)
        for trade_count in (0, 4096)]
    assert_scratch_invariant(*groups)


def test_design_rule_scratch_grows_only_with_bars(tmp_path, monkeypatch):
    # N, 2N, 4N cross the floor and grow strictly below the cap; the final
    # two feeds both saturate. R intentionally continues to reserve 2*N.
    small = 16384
    cap_bars = (1048576 + 3 - 1) // 3
    counts = (small, 2 * small, 4 * small, cap_bars, 2 * cap_bars)
    manifests = []
    for count in counts:
        installed = drive_design_rule_cli(
            tmp_path, monkeypatch, route="ordinary", text=K3, count=count)
        maxima = scratch_maxima(installed[0][1])
        assert maxima == expected_scratch_maxima(count, 3)
        manifests.append(maxima)
    for name in SCRATCH_MAXIMA:
        values = [manifest[name] for manifest in manifests]
        assert values == sorted(values), name
    for name in ("K_e", "B_e", "K_c", "B_c", "U", "B_U"):
        values = [manifest[name] for manifest in manifests]
        assert values[0] < values[1] < values[2] < values[3]
        assert values[3] == values[4]
    assert manifests[0]["U"] == 65536
    assert manifests[-1]["U"] == 1048576
    assert all(manifest["U"] >= 65536 for manifest in manifests)


def test_design_rule_one_key_per_bar_is_admitted(tmp_path, monkeypatch):
    count = 131072  # Above the floor and below the cap: k*N must fit.
    installed = drive_design_rule_cli(
        tmp_path, monkeypatch, route="ordinary", text=K1, count=count)
    maxima = scratch_maxima(installed[0][1])
    assert maxima == expected_scratch_maxima(count, 1)
    assert maxima["U"] >= count


@pytest.mark.parametrize("route", DESIGN_ROUTES)
@pytest.mark.parametrize("dependence", ["trials", "trades"])
def test_design_rule_invariance_comparison_rejects_sizing_mutation(
        tmp_path, monkeypatch, route, dependence):
    real_sizing = run_json.default_scratch_manifest
    context = {"extra": 0}

    def dependent_sizing(*args, **kwargs):
        manifest = real_sizing(*args, **kwargs)
        manifest.U += context["extra"]
        return manifest

    monkeypatch.setattr(run_json, "default_scratch_manifest", dependent_sizing)
    groups = []
    for trials, trade_count in (
            ((1, 0), (2, 0)) if dependence == "trials"
            else ((3, 0), (3, 4096))):
        context["extra"] = trials if dependence == "trials" else trade_count
        groups.append(drive_design_rule_cli(
            tmp_path, monkeypatch, route=route, text=K3, count=10,
            trials=trials, trade_count=trade_count))
    # Prove the mutant travelled through main and reached the C setter.
    delta = 1 if dependence == "trials" else 4096
    assert groups[1][0][1].U - groups[0][0][1].U == delta
    with pytest.raises(AssertionError, match="scratch manifest changed"):
        assert_scratch_invariant(*groups)
