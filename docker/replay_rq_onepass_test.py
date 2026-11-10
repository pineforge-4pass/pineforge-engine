"""Native replay report read budget and lab-only byte comparison."""
from contextlib import ExitStack
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import replay_rq_support as rq
import run_json
import test_run_json_sink as ordinary
from run_json_fused_test_support import SinkIO


# Copied from run_json_fused_test._assert_decoded_rows to avoid importing its tests.
def _assert_decoded_rows(actual, expected):
    """Include insertion order, scalar types and signed zero in the comparison."""
    assert type(actual) is type(expected)
    if isinstance(expected, dict):
        assert tuple(actual) == tuple(expected)
        for key in expected:
            _assert_decoded_rows(actual[key], expected[key])
    elif isinstance(expected, list):
        assert len(actual) == len(expected)
        for left, right in zip(actual, expected):
            _assert_decoded_rows(left, right)
    elif isinstance(expected, float):
        assert actual.hex() == expected.hex()
    else:
        assert actual == expected


@pytest.mark.parametrize("shape", ("fifo", "any", "ordinary", "zero"))
def test_fused_sink_read_once_replay_entrypoint(tmp_path, monkeypatch, shape):
    """Native main and the production join read the primary exactly once.

    Bound replay verification remains inside the audit, before report joining.
    """
    captured = []
    original_verify = run_json._ReportSink.verify
    with ExitStack() as scopes, monkeypatch.context() as patch:
        def verify(sink, *args, **kwargs):
            assert not captured, "the replay CLI row must verify exactly one primary"
            assert not sink.named, "this row must exercise the unnamed sink entry"
            # Observe the completed native input before counting product I/O.
            raw = Path(sink.path).read_bytes()
            audit = SinkIO(sink.path)
            scopes.enter_context(audit.install(monkeypatch))
            captured.append((audit, raw))
            return original_verify(sink, *args, **kwargs)

        patch.setattr(run_json._ReportSink, "verify", verify)
        result = rq.invoke(tmp_path, monkeypatch, shape, route="sink")

    # Independent folds consume the real companion, without replacing capture.join.
    rq.successful(result)
    rq.assert_exact(result.document, shape)
    rq.assert_q_schedule(result.document, shape)
    assert len(captured) == 1
    audit, raw = captured[0]
    with pf_sink.SinkReader(stream=io.BytesIO(raw)) as primary:
        primary_records = list(primary)
        assert primary.complete
    companion_records, companion = rq.replay_records(result.companion)
    trades = rq.rows_with_tag(primary_records, "T")
    points = rq.rows_with_tag(primary_records, "E")
    annotations = rq.rows_with_tag(companion_records, "R")
    history = rq.rows_with_tag(companion_records, "Q")
    document = result.document
    assert len(annotations) == companion.counts["R"] == len(trades)
    assert len(trades) == primary.counts["T"] == len(document["trades"])
    assert len(history) == companion.counts["Q"] == len(points)
    assert len(points) == primary.counts["E"] == len(document["equity_curve"])
    assert companion.trailer["base_sha256"] == primary.sha256
    # _sink_trade deliberately keeps these four fields sink-only; the base
    # stdout contract omits them. Compare every emitted field and each omission.
    sink_only = ("close_cause", "entry_id", "exit_id", "exit_comment")
    for trade, source, row in zip(document["trades"], trades, annotations):
        assert set(source) == set(ordinary.TRADE_FIELDS)
        for field in ordinary.TRADE_FIELDS:
            if field in sink_only:
                assert field not in trade
            else:
                _assert_decoded_rows(trade[field], source[field])
        assert trade["n"] == row["n"] == source["n"]
        assert trade["settlement_index"] == row["settlement_index"]
        if row["parent_id"] is None:
            assert trade["recut"] is None
        else:
            _assert_decoded_rows(
                trade["recut"], {field: row[field] for field in rq.MARKER_FIELDS})
    for index, (point, source, row) in enumerate(
            zip(document["equity_curve"], points, history)):
        assert row["point_index"] == index
        assert point["time_ms"] == row["time_ms"] == source["time_ms"]
        for field in source:
            _assert_decoded_rows(point[field], source[field])
        for field in rq.POINT_FIELDS:
            _assert_decoded_rows(point[field], row[field])
    audit.assert_one_pass(raw)


REPLAY_BASE = "33503b885cb8aa261f5a8156b2fb1c8846516a48"
REPLAY_BASE_FILES = (
    ("docker/run_json.py",
     "8cec2b3fa052c131c89b1bd18c424afa578c6796cf6b9801ae2786418e1dcf3e",
     "run_json_frozen_replay_base.py"),
    ("docker/pf_sink.py",
     "957ac56ab5b4b934a38a469fd5884ea1ddee99982b144ed57cc1d26b6c01c456",
     "pf_sink_replay_base.py"),
)
# The pinned replay base implements every reported member. The only stdout fields
# that cannot be byte-equal are the two wall-clock measurements of the native
# writer's phase timing (diagnostics.phase_timing): they differ between ANY two
# real runs, including two runs of the base itself, so they are the whole
# exception list. Their values alone are masked, symmetrically for base and
# candidate; the masked count must be equal, every other byte of stdout and every
# sink byte is compared as is, and no numeric tolerance is applied.
REPLAY_BYTE_FIELD_EXCEPTIONS = {
    "diagnostics.phase_timing.execution_ms":
        "wall-clock milliseconds of the native execution phase, measured per run",
    "diagnostics.phase_timing.result_assembly_ms":
        "wall-clock milliseconds of the native result assembly phase, measured per run",
}
_WALL_CLOCK_VALUE = re.compile(rb'("(?:execution_ms|result_assembly_ms)":)[-+0-9.eE]+')


def _mask_wall_clock(stdout):
    return _WALL_CLOCK_VALUE.subn(rb"\1<wall-clock>", stdout)


REPLAY_SHAPES = (
    "fifo", "any", "ordinary", "zero", "fifo_no_fees", "two_groups",
    "sealed", "sort", "nondyadic", "quote", "cancellation",
)
# The two_groups fixture shape begins its native stream only on the legacy array
# route, which the existing array-route rows select inside a process and a
# subprocess run cannot. Both runners must refuse it, with the same exit code and
# the same bytes (an equal refusal is also a byte comparison).
REPLAY_ARRAY_ONLY_SHAPES = ("two_groups",)
REPLAY_ARRAY_ONLY_REFUSAL = b'"error":"replay R/Q fixture: stream begin failed"'


def _replay_corpus_manifest(directory, module, planner):
    """All supported schedules, temporary/named sinks, and selected anchors."""
    cases = []
    for shape in REPLAY_SHAPES:
        for selected in (False, True):
            for transport in ("temporary", "temporary-companion", "named"):
                folder = directory / f"{shape}-{int(selected)}-{transport}"
                folder.mkdir()
                chart = folder / "chart.csv"
                count = rq.csv_tape(chart, shape)
                argv = [
                    "--profile", "unjailed", "--ohlcv", str(chart), "--input-tf", "1",
                    "--script-tf", "15" if shape == "quote" else "1",
                    "--inputs", json.dumps({"shape": shape}),
                    "--run-id", "replay-byte-oracle", "--report-replay",
                ]
                if shape == "quote":
                    argv += ["--bar-magnifier", "true"]
                sink_paths = []
                if transport != "temporary-companion":
                    replay_path = folder / "replay.ndjson"
                    argv += ["--report-replay-sink", str(replay_path)]
                    sink_paths.append(("replay", str(replay_path)))
                if transport == "named":
                    primary_path = folder / "primary.ndjson"
                    argv += ["--report-sink", str(primary_path)]
                    sink_paths.append(("primary", str(primary_path)))
                if selected:
                    inventory = folder / "inventory.json"
                    inventory.write_bytes(rq.wire({
                        "schema": "pineforge-request-feed-inventory/v1",
                        "source_sha256": "0" * 64,
                        "artifact_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                        "primary_chart_timeframe": "15" if shape == "quote" else "1",
                        "entries": [],
                    }))
                    argv += [
                        "--report-policy", "selected-window/v1",
                        "--window-start-ms", str(rq.START),
                        # The window end must sit on the script calendar: the quote
                        # shape runs a 15-minute script on a 1-minute tape.
                        "--window-end-ms", str(rq.START + (
                            (count + 2 + 14) // 15 * 15 if shape == "quote" else count + 2) * rq.MINUTE),
                        "--preroll-bars", "0", "--fed-start-ms", str(rq.START),
                        "--request-feed-inventory", str(inventory),
                        "--overrides", '{"initial_capital":"16384"}',
                    ]
                cases.append({
                    "so": str(module), "argv": argv, "shape": shape,
                    "selected": selected, "transport": transport,
                    "sink_paths": sink_paths, "planner": str(planner) if selected else "",
                })
    return cases


def test_replay_report_bytes_corpus(tmp_path):
    """Lab-only, outside CI: compare stdout and EVERY sink with the replay base.

    Uses the same _corpus_run helper as the non-replay 40-run proof. That proof
    keeps its older frozen reference. Missing native modules, planner, or base
    git objects are prerequisite failures here, never environment-based skips.
    The generated manifest retains each distinct (so, argv) and input path.
    """
    from run_json_fused_test import _corpus_run

    module = Path(os.environ["PINEFORGE_REPLAY_RQ_LIBRARY"]).resolve()
    planner = Path(os.environ["PINEFORGE_REPLAY_RQ_PLANNER"]).resolve()
    assert module.is_file(), module
    assert planner.is_file(), planner
    assert set(REPLAY_BYTE_FIELD_EXCEPTIONS) == {
        "diagnostics.phase_timing.execution_ms",
        "diagnostics.phase_timing.result_assembly_ms"}
    base_paths = []
    for source, expected_sha, name in REPLAY_BASE_FILES:
        result = subprocess.run(
            ["git", "show", f"{REPLAY_BASE}:{source}"], cwd=ROOT, capture_output=True)
        assert result.returncode == 0, (source, result.stderr)
        assert hashlib.sha256(result.stdout).hexdigest() == expected_sha, source
        path = tmp_path / name
        path.write_bytes(result.stdout)
        base_paths.append(path)
    base_runner, base_sink = base_paths
    cases = _replay_corpus_manifest(tmp_path, module, planner)
    assert {case["shape"] for case in cases} == set(REPLAY_SHAPES)
    identities = {json.dumps((case["so"], case["argv"])) for case in cases}
    assert len(identities) == len(cases) == len(REPLAY_SHAPES) * 2 * 3
    (tmp_path / "replay-manifest.json").write_text(
        json.dumps(cases, indent=2) + "\n", encoding="utf-8")
    for index, case in enumerate(cases):
        captures = []
        results = []
        for label, runner in (("base", base_runner),
                              ("candidate", ROOT / "docker" / "run_json.py")):
            capture = tmp_path / f"capture-{index}-{label}"
            result = _corpus_run(runner, base_sink, dict(case, sink_capture=str(capture)))
            if case["shape"] in REPLAY_ARRAY_ONLY_SHAPES:
                assert result.returncode == 1 and REPLAY_ARRAY_ONLY_REFUSAL in result.stdout, (
                    case, label, result.stdout, result.stderr)
                results.append(result)
                continue
            assert result.returncode == 0, (case, label, result.stdout, result.stderr)
            assert result.stderr == b"", (case, label, result.stderr)
            assert {path.name for path in capture.iterdir()} == {"primary", "replay"}
            for role, path in case["sink_paths"]:
                assert Path(path).read_bytes() == (capture / role).read_bytes()
            captures.append(capture)
            results.append(result)
        if case["shape"] in REPLAY_ARRAY_ONLY_SHAPES:
            assert [(r.returncode, r.stdout, r.stderr) for r in results[:1]] == [
                (r.returncode, r.stdout, r.stderr) for r in results[1:]], case
            continue
        (base_bytes, base_count), (candidate_bytes, candidate_count) = (
            _mask_wall_clock(result.stdout) for result in results)
        # The same wall-clock values are masked in both, and nothing else differs.
        assert base_count == candidate_count, (case, base_count, candidate_count)
        assert base_bytes == candidate_bytes, case
        for role in ("primary", "replay"):
            assert (captures[0] / role).read_bytes() == (captures[1] / role).read_bytes(), (
                case, role)
