"""Exercise the strategy runner's symbol document without starting a container."""

import csv
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest import mock

import pytest

TESTS = Path(__file__).resolve().parent
FIXTURES = TESTS / "fixtures"
sys.path[:0] = [str(TESTS.parent / "scripts"), str(TESTS.parent / "docker")]
import pf_release_run
import run_json
import run_strategy


def runner_symbols(work, source, chart, overrides, env):
    """Get the ctypes metadata and the document handed to the image launcher."""
    shutil.copyfile(source, work / "generated.cpp")
    params = {"runtime_overrides": overrides}
    feed, kwargs = run_strategy.inputs_run_kwargs(params, work, chart, env=env)
    with mock.patch.object(pf_release_run, "run_release", return_value={}) as launch:
        run_strategy._run_via_docker(work, feed, params, kwargs, None, None)
    launch.assert_called_once()
    return kwargs["syminfo_metadata"] or {}, launch.call_args.kwargs["syminfo"] or {}


def test_runner_lot_declarations(tmp_path):
    source = TESTS / "entry_quantity_strategy.cpp"
    chart = FIXTURES / "qty_step_mincontract/btc60/feed/bars.csv"
    template = {run_strategy.LANE_QTY_STEP_ENV: "1"}
    for overrides, env, expected in (
        # A price tick and a symbol name alone supply no quantity increment.
        ({"tickerid": "EXCHANGE:ABC", "mintick": 0.25}, {}, {}),
        # Keep the two facts distinct, including when the step is coarser.
        ({"qty_step": 0.25, "mincontract": 0.01}, {},
         {"qty_step": 0.25, "mincontract": 0.01}),
        # An already declared minimum keeps its meaning.
        ({"syminfo_metadata": {"mincontract": 0.5}, "mincontract": 0.01},
         template, {"mincontract": 0.5}),
        ({"mincontract": 0.01}, template, {"mincontract": 0.01}),
        ({"syminfo_metadata": {"mincontract": None}}, template, {"mincontract": 1.0}),
        ({"syminfo_metadata": {"mincontract": None}}, {}, {}),
        ({}, {run_strategy.LANE_QTY_STEP_ENV: "1e-05"}, {"mincontract": 0.00001}),
    ):
        for symbol in runner_symbols(tmp_path, source, chart, overrides, env):
            actual = {k: symbol[k] for k in ("qty_step", "mincontract") if symbol.get(k) is not None}
            assert actual == expected, (overrides, actual, expected)


def test_flat_mincontract_must_be_positive_finite_number(tmp_path):
    for value in (False, True, "0.01", 0, -1, math.nan, math.inf, 10**400):
        with pytest.raises(ValueError, match=r"runtime_overrides\.mincontract") as refusal:
            run_strategy.inputs_run_kwargs({"runtime_overrides": {"mincontract": value}},
                                           tmp_path, tmp_path / "bars.csv", env={})
        assert refusal.value.run_failure_code == "lot_grid_rejected"
        assert json.loads(refusal.value.run_failure_args) == {}


@pytest.mark.parametrize("runner", ["ctypes", "docker"])
@pytest.mark.parametrize("key", ["mincontract", "qty_step"])
@pytest.mark.parametrize("nested", [False, True], ids=["flat", "nested"])
def test_runner_cli_prints_the_lot_refusal(tmp_path, runner, key, nested):
    chart = tmp_path / "chart.csv"
    chart.write_text("timestamp,open,high,low,close,volume\n1000,1,2,1,2,10\n")
    overrides = {"syminfo_metadata": {key: False}} if nested else {key: False}
    (tmp_path / "inputs.json").write_text(json.dumps({"runtime_overrides": overrides}))
    result = subprocess.run(
        [sys.executable, "-B", run_strategy.__file__, str(tmp_path), "--runner", runner,
         "--ohlcv", str(chart), "--no-trim-output"],
        env={k: v for k, v in os.environ.items() if not k.startswith("PINEFORGE_")},
        text=True, capture_output=True, check=False)
    assert result.returncode == 1, (result.stdout, result.stderr)
    assert result.stderr.splitlines().count("run failure code: lot_grid_rejected") == 1
    name = ("syminfo_metadata." if nested else "") + key
    assert f"runtime_overrides.{name} must be a positive finite number" in result.stderr
    assert not (tmp_path / "engine_trades.csv").exists()


def test_invalid_lane_step_leaves_mincontract_undeclared(tmp_path):
    source = TESTS / "entry_quantity_strategy.cpp"
    chart = FIXTURES / "qty_step_mincontract/btc60/feed/bars.csv"
    # A malformed lane hint has the same result as no hint. In both cases an
    # explicit step still reaches both runners, with no invented minimum.
    overrides = {"qty_step": 0.25}
    absent = runner_symbols(tmp_path, source, chart, overrides, {})
    assert absent == (overrides, overrides)
    for value in ("1_0", " 0.01 ", "\uff11"):
        actual = runner_symbols(tmp_path, source, chart, overrides,
                                {run_strategy.LANE_QTY_STEP_ENV: value})
        assert actual == absent, (value, actual, absent)


def replay(program, option, tape):
    if option == "--explicit-entry":
        # Independent symbol readout, not the fill used as the oracle.
        with (FIXTURES / "syminfo_mincontract/eth-15/tv_trades.csv").open(
                newline="", encoding="utf-8-sig") as src:
            entry = next(r for r in csv.DictReader(src) if r["Type"].startswith("Entry"))
        facts = dict(item.split("=", 1) for item in entry["Signal"].split("|"))
        lot = float(facts["mincontract"])
        overrides = {"syminfo_metadata": {"mincontract": lot}}
        chart = FIXTURES / "exit_queue/bars.inc"
        source = TESTS / "test_grid_close_tapes.cpp"
    elif option == "--equity-entry":
        with (FIXTURES / "margin_ledger_rules/cases.tsv").open(
                newline="", encoding="utf-8") as src:
            case, = (r for r in csv.DictReader(src, delimiter="\t") if r["tape"] == tape)
        lot = float(case["step"])
        overrides = {"qty_step": lot}
        chart = FIXTURES / "margin_ledger_rules/bars" / case["bars"]
        source = TESTS / "test_margin_ledger_rules_tapes.cpp"
    else:
        raise ValueError(f"unknown replay: {option}")
    if not (math.isfinite(lot) and lot > 0 and chart.is_file()):
        raise ValueError("invalid symbol metadata or missing replay feed")

    # Only the image launch is replaced. Forward the container parser's actual
    # setter calls to the executable that owns the recorded strategy and bars.
    # A missing declaration must reach that executable as no setter calls.
    command = [program, option, tape]

    def metadata(_state, key, value):
        command.extend((key.decode(), repr(value)))

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        _, document = runner_symbols(work, source, chart, overrides, {})
        if document:
            path = work / "syminfo.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            run_json.apply_syminfo(SimpleNamespace(strategy_set_syminfo_metadata=metadata),
                                   None, path)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(replay(*sys.argv[1:]))
