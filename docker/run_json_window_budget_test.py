"""Account for artifact windows without changing the report or sink bytes."""
from contextlib import ExitStack
import inspect
import json
from pathlib import Path
import sys
import textwrap

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

import pytest
import run_json
import run_json_fused_test_support as support
import test_run_json_sink as ordinary
from run_json_fused_test import _assert_decoded_rows, _drain_report_artifact, _sink_case
from run_json_trade_scan_test import _event, _point, _trade


def test_artifact_window_budget_literal_contract():
    assert getattr(run_json, "_ARTIFACT_WINDOW_BUDGET_BYTES", None) == 6 * 1024 * 1024, (
        "ARTIFACT_WINDOW_BUDGET_CONSTANT: one stated per-run budget")
    assert getattr(run_json, "_ARTIFACT_LARGE_WINDOW", None) == 1024 * 1024, (
        "ARTIFACT_LARGE_WINDOW: budgeted artifacts use 1 MiB")
    assert run_json._BoundedJSONFile.window == 64 * 1024
    assert run_json._UncachedSinkFile.window == 64 * 1024
    assert run_json._UncachedSinkFile.window == run_json._BULK_E_WINDOW
    assert getattr(run_json, "_JSON_COLUMN_LOOKUP_READ_SIZE", None) == 64 * 1024
    assert run_json._ARTIFACT_LARGE_WINDOW % 4096 == 0
    assert run_json._ARTIFACT_WINDOW_BUDGET_BYTES % run_json._ARTIFACT_LARGE_WINDOW == 0


def test_output_series_ceiling_literal_contract():
    assert getattr(run_json, "_OUTPUT_SERIES_LIMIT", None) == 64, (
        "OUTPUT_SERIES_LIMIT: the production ceiling matches TradingView's plot-count limit")


def _production_sizes():
    small = run_json._BoundedJSONFile.window
    # The fallback lets the behavioral rows reach their named assertions on
    # an implementation with only the final artifact's larger window.
    large = getattr(run_json, "_ARTIFACT_LARGE_WINDOW", 16 * small)
    budget = getattr(run_json, "_ARTIFACT_WINDOW_BUDGET_BYTES", 6 * large)
    return budget, large, small


def _small_windows(monkeypatch, budget_units, *, short_by=0):
    small = run_json.mmap.PAGESIZE
    large = 2 * small
    budget = budget_units * large - short_by
    monkeypatch.setattr(run_json._BoundedJSONFile, "window", small)
    monkeypatch.setattr(run_json, "_ARTIFACT_LARGE_WINDOW", large, raising=False)
    monkeypatch.setattr(run_json, "_ARTIFACT_WINDOW_BUDGET_BYTES", budget, raising=False)
    return budget, large, small


def _capture_report(tmp_path, monkeypatch, series, *, outputs=True, legacy=False):
    """Observe real constructors, their column roles, and the complete artifact."""
    header = ordinary.header
    manifest = json.loads(ordinary.outputs_reader().manifest())
    manifest["series"] = [{"slot": slot, "output": "p"} for slot in range(series)]
    raw_manifest = ordinary.encoded(manifest).rstrip(b"\n")
    rows = [_point(0), _trade(1, pnl=-0.0), _point(1), _trade(2)]
    if outputs:
        rows += [
            ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1,
             *(-0.0 for _ in range(series))],
            ["O", 1, None, None, *(None for _ in range(series))],
            _event('quote"\\\n漢'), ["K", 50.],
        ]
    constructed, captured = [], {}
    constructor = run_json._BoundedJSONFile.__init__

    def observed(self, file, window=None):
        constructor(self, file, window)
        if type(self) is run_json._BoundedJSONFile:
            constructed.append(self)

    def series_header(outputs=False):
        result = header(outputs)
        if outputs:
            descriptor = next(entry for entry in result["records"] if entry["type"] == "O")
            descriptor["fields"] = ["row", "open_ms", "close_ms"] + [
                f"v{slot}" for slot in range(series)]
            descriptor["types"] = ["i64", "i64?", "i64?"] + ["f64?"] * series
            descriptor["units"] = ["", "unix_ms", "unix_ms"] + [""] * series
        return result

    def consume(artifact, document):
        columns = ([] if legacy else [
            document["equity_curve"], document["trades"]])
        assert ("outputs" in document) == outputs
        if outputs:
            block = document["outputs"]
            columns += [
                block["bars"]["open_ms"], block["bars"]["close_ms"], block["events"]]
            columns += [entry["values"] for entry in block["series"]]
        expected_files = [column.file for column in columns] + [artifact]
        assert constructed == expected_files, (
            "ARTIFACT_WINDOW_PRIORITY: equity, trades, open, close, events, series, report")
        captured["windows"] = [file.window for file in constructed]
        captured["report"] = _drain_report_artifact(artifact)
        assert all(len(file.buffer) == file.window for file in constructed)

    with monkeypatch.context() as patch:
        patch.setattr(ordinary, "header", series_header)
        patch.setattr(run_json, "_outputs_manifest", lambda lib, state: raw_manifest)
        patch.setattr(run_json._BoundedJSONFile, "__init__", observed)
        with _sink_case(tmp_path, rows, outputs=outputs) as (sink, library, report):
            sink._fused_pass = not legacy
            outcome = support.assemble(
                run_json, sink, library, report, outputs=outputs,
                consume=consume, consume_before_close=True)
            assert outcome.failure is None, outcome.failure
            captured["digest"] = sink.sha256
    return captured


def _assert_allocations(windows, budget, large, small):
    # Reservation priority differs from construction order: report first.
    priority = windows[-1:] + windows[:-1]
    large_count = min(len(priority), budget // large)
    expected = [large] * large_count + [small] * (len(priority) - large_count)
    assert priority == expected, (
        f"ARTIFACT_WINDOW_BUDGET: actual={priority!r}, expected={expected!r}")
    assert sum(window for window in priority if window == large) <= budget, (
        "ARTIFACT_WINDOW_LARGE_TOTAL: large windows exceed the budget")
    overflow = len(priority) - large_count
    assert sum(priority) <= budget + overflow * small, (
        "ARTIFACT_WINDOW_TOTAL: budget plus fallback windows exceeded")


@pytest.mark.parametrize("series", (0, 1, 10))
def test_fixed_files_are_large_and_recorded_series_are_small(tmp_path, monkeypatch, series):
    budget, large, small = _production_sizes()
    captured = _capture_report(tmp_path, monkeypatch, series)
    assert len(captured["windows"]) == 6 + series
    _assert_allocations(captured["windows"], budget, large, small)
    assert captured["windows"] == [large] * 5 + [small] * series + [large], (
        "ARTIFACT_WINDOW_FIXED_FILES_AND_SERIES")


def test_without_outputs_only_three_large_files_use_the_budget(tmp_path, monkeypatch):
    budget, large, small = _production_sizes()
    captured = _capture_report(tmp_path, monkeypatch, 0, outputs=False)
    assert len(captured["windows"]) == 3
    _assert_allocations(captured["windows"], budget, large, small)
    assert captured["windows"] == [large] * 3, "ARTIFACT_WINDOW_WITHOUT_OUTPUTS"
    assert budget - sum(captured["windows"]) == 3 * large, "ARTIFACT_WINDOW_UNUSED_CREDIT"


def test_many_series_cross_the_production_budget(tmp_path, monkeypatch):
    budget, large, small = _production_sizes()
    captured = _capture_report(tmp_path, monkeypatch, run_json._OUTPUT_SERIES_LIMIT)
    assert len(captured["windows"]) == 70
    _assert_allocations(captured["windows"], budget, large, small)
    assert captured["windows"][-1] == large, "ARTIFACT_WINDOW_FINAL_RESERVATION"


def test_series_ceiling_has_ten_mib_of_mapped_artifact_windows(tmp_path, monkeypatch):
    budget, large, small = _production_sizes()
    captured = _capture_report(tmp_path, monkeypatch, run_json._OUTPUT_SERIES_LIMIT)
    assert len(captured["windows"]) == 6 + run_json._OUTPUT_SERIES_LIMIT
    _assert_allocations(captured["windows"], budget, large, small)
    assert sum(captured["windows"]) == (
        6 * 1024 * 1024 + 64 * 1024 * run_json._OUTPUT_SERIES_LIMIT
    ) == 10_485_760, "ARTIFACT_WINDOW_CEILING_CAPACITY"


@pytest.mark.parametrize("legacy", (False, True), ids=("fused", "legacy"))
@pytest.mark.parametrize("series", (0, 1, 7))
@pytest.mark.parametrize("short_by", (0, 1), ids=("exact-budget", "one-byte-short"))
def test_injected_budget_edge_keeps_the_crossing_file_small(
        tmp_path, monkeypatch, legacy, series, short_by):
    file_count = (4 if legacy else 6) + series
    budget, large, small = _small_windows(
        monkeypatch, file_count, short_by=short_by)
    captured = _capture_report(tmp_path, monkeypatch, series, legacy=legacy)
    assert len(captured["windows"]) == file_count
    _assert_allocations(captured["windows"], budget, large, small)
    assert captured["windows"][-1] == large, "ARTIFACT_WINDOW_FINAL_RESERVATION"
    if short_by:
        assert captured["windows"][-2] == small, "ARTIFACT_WINDOW_CROSSING_FILE"


@pytest.mark.parametrize("legacy", (False, True), ids=("fused", "legacy"))
def test_partial_budget_keeps_every_file_after_the_edge_small(
        tmp_path, monkeypatch, legacy):
    budget, large, small = _small_windows(monkeypatch, 3)
    captured = _capture_report(tmp_path, monkeypatch, 7, legacy=legacy)
    _assert_allocations(captured["windows"], budget, large, small)
    assert captured["windows"][2:-1] == [small] * (len(captured["windows"]) - 3)


def test_exhausted_partial_and_unlimited_budgets_preserve_stdout_and_sink(
        tmp_path, monkeypatch):
    _, large, _ = _production_sizes()
    results, allocations = [], []
    constructor = run_json._BoundedJSONFile.__init__
    trade_count = 2 * large // 128 + 1
    for budget in (0, 4 * large, 100 * large):
        windows = []

        def observed(self, file, window=None):
            constructor(self, file, window)
            if type(self) is run_json._BoundedJSONFile:
                windows.append(self.window)

        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_ARTIFACT_WINDOW_BUDGET_BYTES", budget, raising=False)
            patch.setattr(run_json._BoundedJSONFile, "__init__", observed)
            status, text, stderr, library = ordinary.invoke(
                tmp_path, patch, library=ordinary.SinkLibrary(
                    outputs=True, pnls=tuple(float(index % 3 - 1) for index in range(trade_count))),
                extra=["--outputs", "--spill-dir", str(tmp_path)])
            assert status == 0 and stderr == "", (status, text, stderr)
            assert len(text) > 2 * large, "ARTIFACT_WINDOW_MULTICHUNK_BYTES"
            _assert_allocations(windows, budget, large, run_json._BoundedJSONFile.window)
            allocations.append(windows)
            results.append((text.encode("ascii"), library.sha))
    assert results[0] == results[1] == results[2], "ARTIFACT_WINDOW_BYTE_IDENTITY"
    assert allocations[0] != allocations[1] != allocations[2], "ARTIFACT_WINDOW_DISTINCT_CASES"


@pytest.mark.parametrize("legacy", (False, True), ids=("fused", "legacy"))
def test_prepared_reports_do_not_share_their_budget(tmp_path, monkeypatch, legacy):
    budget, large, small = _small_windows(monkeypatch, 3)
    first = _capture_report(tmp_path, monkeypatch, 7, legacy=legacy)
    second = _capture_report(tmp_path, monkeypatch, 7, legacy=legacy)
    _assert_allocations(first["windows"], budget, large, small)
    _assert_allocations(second["windows"], budget, large, small)
    assert first == second, "ARTIFACT_WINDOW_PER_RUN"


def test_independent_resource_scopes_reserve_their_own_final_artifact(tmp_path, monkeypatch):
    budget, large, small = _small_windows(monkeypatch, 2)
    with ExitStack() as first, ExitStack() as second:
        columns = [run_json._JSONColumn(resources, str(tmp_path))
                   for resources in (first, second)]
        for column in columns:
            column.append({"value": -0.0})
        artifacts = [
            run_json._prepare_stream_report({"rows": column}, resources, str(tmp_path))
            for resources, column in zip((first, second), columns)]
        for column, artifact in zip(columns, artifacts):
            _assert_allocations([column.file.window, artifact.window], budget, large, small)
        assert _drain_report_artifact(artifacts[0]) == _drain_report_artifact(artifacts[1])


def _mutate(function, old, new, **bindings):
    source = textwrap.dedent(inspect.getsource(function))
    assert source.count(old) == 1, "ARTIFACT_WINDOW_MUTATION_SITE"
    namespace = dict(function.__globals__, **bindings)
    exec(compile(source.replace(old, new), "<artifact-window-mutation>", "exec"), namespace)
    return namespace[function.__name__]


@pytest.mark.parametrize("kind", ("unbudgeted", "crossing-file", "shared-between-runs"))
def test_budget_mutations_are_rejected(tmp_path, monkeypatch, kind):
    budget, large, small = _small_windows(
        monkeypatch, 2, short_by=0 if kind == "shared-between-runs" else 1)
    accountant = getattr(run_json, "_ArtifactWindowBudget", None)
    assert accountant is not None, "ARTIFACT_WINDOW_ACCOUNTANT: per-run allocation is required"
    with monkeypatch.context() as patch:
        if kind == "shared-between-runs":
            patch.setattr(run_json, "_artifact_windows", _mutate(
                run_json._artifact_windows, "_ArtifactWindowBudget()", "_shared_budget",
                _shared_budget=accountant()))
            _capture_report(tmp_path, patch, 1)
        else:
            replacement = "if True:" if kind == "unbudgeted" else "if self.remaining > 0:"
            patch.setattr(accountant, "take", _mutate(
                accountant.take, "if self.remaining >= _ARTIFACT_LARGE_WINDOW:", replacement))
        captured = _capture_report(tmp_path, patch, 1)
        with pytest.raises(AssertionError, match="ARTIFACT_WINDOW_BUDGET"):
            _assert_allocations(captured["windows"], budget, large, small)


def _assert_lookup_requests(requests):
    assert requests and all(size <= run_json._UncachedSinkFile.window for size in requests), (
        f"COLUMN_LOOKUP_READ: requested sizes {requests!r}")


def _lookup_case(tmp_path, monkeypatch, *, mutation=False):
    _, large, _ = _production_sizes()
    trade_width = len(json.dumps(
        dict(zip(ordinary.TRADE_FIELDS[:16], _trade(1)[1:])),
        separators=(",", ":"), allow_nan=False)) + 1
    count = 3 * large // trade_width + 2
    rows = [_point(0)] + [_trade(index + 1) for index in range(count)]

    def before_prepare(document):
        column = document["trades"]
        assert column.file.window > run_json._UncachedSinkFile.window, (
            "COLUMN_LOOKUP_LARGE_WINDOW: the lookup bound must be independent")
        assert column.file.size > 3 * column.file.window
        requests = {"iteration": [], "lookup": []}
        phase = "iteration"
        read_at = column.file.read_at

        def counted(offset, size):
            requests[phase].append(size)
            return read_at(offset, size)

        monkeypatch.setattr(column.file, "read_at", counted)
        expected = list(column)
        assert requests["iteration"] and all(
            size == column.file.window for size in requests["iteration"]), (
                f"COLUMN_ITERATION_READ: requested sizes {requests['iteration']!r}")
        if mutation:
            mutant = _mutate(
                run_json._JSONColumn._item_at,
                "_JSON_COLUMN_LOOKUP_READ_SIZE", "self.file.window")
            monkeypatch.setattr(column, "item", mutant.__get__(column, type(column)))
        phase = "lookup"
        indexed = [column[index] for index in range(len(expected))]
        _assert_lookup_requests(requests["lookup"])
        _assert_decoded_rows(indexed, expected)

    with _sink_case(tmp_path, rows) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report, before_prepare=before_prepare)
        assert outcome.failure is None, outcome.failure


def test_fused_column_lookup_read_size_is_independent_of_artifact_window(
        tmp_path, monkeypatch):
    _lookup_case(tmp_path, monkeypatch)


def test_unbounded_lookup_mutation_is_rejected(tmp_path, monkeypatch):
    with pytest.raises(AssertionError, match="COLUMN_LOOKUP_READ"):
        _lookup_case(tmp_path, monkeypatch, mutation=True)


def test_fused_column_index_decodes_row_larger_than_lookup_size(tmp_path):
    def before_prepare(document):
        column = document["trades"]
        column.append({"payload": "x" * (3 * run_json._UncachedSinkFile.window + 17)})
        column.append({"tail": "after-large-row"})
        expected = list(column)
        assert column.file.size > run_json._UncachedSinkFile.window
        for order in (range(len(expected)), range(len(expected) - 1, -1, -1)):
            indexed = [column[index] for index in order]
            _assert_decoded_rows(indexed, [expected[index] for index in order])

    with _sink_case(tmp_path, [_point(0), _trade(1), _trade(2)]) as (sink, library, report):
        outcome = support.assemble(
            run_json, sink, library, report, before_prepare=before_prepare)
        assert outcome.failure is None, outcome.failure


@pytest.mark.parametrize("legacy", (False, True), ids=("fused", "legacy"))
@pytest.mark.parametrize("creation", ("first", "fourth", "first-series", "last-column"))
def test_artifact_creation_failure_is_position_independent(
        tmp_path, monkeypatch, legacy, creation):
    import errno
    import os

    series = 3
    temporary = run_json._json_temporary
    calls, created, fired = [], [], []
    fail_at = None

    def no_space(*args, **kwargs):
        error = OSError(errno.ENOSPC, "injected artifact creation failure")
        fired.append((len(calls), error))
        raise error

    def observed(resources, directory, window=None):
        index = len(calls) + 1
        calls.append(window)
        if index == fail_at:
            # The real factory catches this disk error and chooses its own
            # failure text and typed arguments.
            with monkeypatch.context() as fault:
                fault.setattr(run_json.tempfile, "TemporaryFile", no_space)
                file = temporary(resources, directory, window)
        else:
            file = temporary(resources, directory, window)
        assert file.buffer is not None
        created.append((index, file, file.fileno(), file.buffer))
        return file

    def assert_closed():
        for _, file, fd, buffer in created:
            assert file.closed and file.buffer is None and buffer.closed
            with pytest.raises(OSError) as caught:
                os.fstat(fd)
            assert caught.value.errno == errno.EBADF
        assert list(tmp_path.iterdir()) == []

    with monkeypatch.context() as patch:
        patch.setattr(run_json, "_json_temporary", observed)
        captured = _capture_report(tmp_path, patch, series, legacy=legacy)
        assert [file.window for _, file, _, _ in created] == captured["windows"] == calls
        assert_closed()
        recorded_series = json.loads(captured["report"])["outputs"]["series"]
        assert len(recorded_series) == series
        # _capture_report proves the actual column roles and construction order,
        # including the final report. Derive both series positions from that run.
        creation_count = len(created)
        fail_at = {
            "first": 1,
            "fourth": 4,
            "first-series": creation_count - len(recorded_series),
            "last-column": creation_count - 1,
        }[creation]
        assert 1 <= fail_at < creation_count
        baseline_windows = calls[:]
        calls, created = [], []

        header = ordinary.header
        manifest = json.loads(ordinary.outputs_reader().manifest())
        manifest["series"] = [{"slot": slot, "output": "p"} for slot in range(series)]
        raw_manifest = ordinary.encoded(manifest).rstrip(b"\n")

        def series_header(outputs=False):
            result = header(outputs)
            if outputs:
                descriptor = next(entry for entry in result["records"] if entry["type"] == "O")
                descriptor["fields"] = ["row", "open_ms", "close_ms"] + [
                    f"v{slot}" for slot in range(series)]
                descriptor["types"] = ["i64", "i64?", "i64?"] + ["f64?"] * series
                descriptor["units"] = ["", "unix_ms", "unix_ms"] + [""] * series
            return result

        rows = [
            _point(0), _trade(1, pnl=-0.0), _point(1), _trade(2),
            ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1,
             *(-0.0 for _ in range(series))],
            ["O", 1, None, None, *(None for _ in range(series))],
            _event('quote"\\\n漢'), ["K", 50.],
        ]
        patch.setattr(ordinary, "header", series_header)
        patch.setattr(run_json, "_outputs_manifest", lambda lib, state: raw_manifest)
        with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
            sink._fused_pass = not legacy
            outcome = support.assemble(run_json, sink, library, report, outputs=True)
            assert list(tmp_path.iterdir()) == [Path(sink.path)]

    assert sink.closed
    assert_closed()
    assert len(fired) == 1 and fired[0][0] == fail_at
    assert len(calls) == len(created) + 1
    assert calls[:fail_at] == baseline_windows[:fail_at]
    assert [index for index, _, _, _ in created if index < fail_at] == list(range(1, fail_at))
    assert all(index != fail_at for index, _, _, _ in created)
    assert type(outcome.error) is run_json.RunFailure
    assert outcome.error.__cause__ is fired[0][1]
    assert outcome.error.__cause__.errno == errno.ENOSPC
    assert outcome.report == b""
    # One literal envelope for every route and position, independent of the
    # fault-free run used only to locate the constructor calls.
    assert outcome.failure == (
        "run_json", "RunFailure", "report sink open failed", "report_sink_failed",
        {"reason": "scratch_io"}, ("report sink open failed",), 1,
        b'{"engine":"pineforge","error":"report sink open failed",'
        b'"code":"report_sink_failed","args":{"reason":"scratch_io"},'
        b'"reason":"scratch_io"}\n',
    )
