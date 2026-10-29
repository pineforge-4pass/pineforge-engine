"""Replay S-record tests for the proposed report-replay extension.

The feature is deliberately red-first: this file calls the proposed
``build_report_dict(..., replay=...)`` pass-through and the proposed
``write_replay_sink`` test seam, so the current tree must fail because those
feature surfaces do not exist. No assertion accepts an absent key or a
constant value.

These are supplied-value transport tests, not engine P&L evidence.
tests/test_replay_abi.cpp and tests/test_run_json_replay_engine.py
exercise the real engine and independently derive the realized value.

The self-consistency checks here are necessary local checks. The full MARKET
C4 check remains a follow-up for CI: run two independent builds (the
pre-change baseline and the implementation build), run the same scenario with
the extension OFF, and compare the default JSON bytes and the primary
``pf-report-sink/v1`` bytes byte-for-byte.
"""
from __future__ import annotations

import io
import json
import math
from pathlib import Path
from types import SimpleNamespace

import pytest

import run_json


_MISSING = object()
_REPLAY = {
    "initial_capital": 12_345.0,
    "pointvalue": 1.0,
    "net_profit_sum": 17.0,
}


class _ReplayAccessor:
    def __init__(self):
        self.calls = []
        self.values = {41: 17.0, 73: -6.25}

    def strategy_net_profit_sum(self, state):
        self.calls.append(state)
        return self.values[state]


def _trade(*, entry, exit, qty, pnl, commission, is_long=True,
           open_at_end=False, entry_bar=1, exit_bar=2):
    value = run_json.TradeC()
    value.entry_time = entry_bar * 60_000
    value.exit_time = exit_bar * 60_000
    value.entry_price = entry
    value.exit_price = exit
    value.qty = qty
    value.pnl = pnl
    value.pnl_pct = 0.0
    value.max_runup = 0.0
    value.max_drawdown = 0.0
    value.commission = commission
    value.is_long = int(is_long)
    value.entry_bar_index = entry_bar
    value.exit_bar_index = exit_bar
    value.open_at_end = int(open_at_end)
    return value


def _report(rows=(), *, report_net_profit=9_999.0):
    report = run_json.ReportC()
    owned_rows = (run_json.TradeC * len(rows))()
    for index, row in enumerate(rows):
        owned_rows[index] = row
    report._owned_rows = owned_rows
    report.trades = owned_rows if rows else None
    report.trades_len = report.total_trades = len(rows)
    report.net_profit = report_net_profit
    report.input_bars_processed = 4
    report.script_bars_processed = 4
    report.metrics = run_json.MetricsC()
    report.equity_curve = None
    report.equity_curve_len = 0
    return report


def _array_report(report, replay=_MISSING):
    arguments = dict(
        ohlcv_path=Path("replay-s.csv"),
        n_bars=4,
        first_ts=60_000,
        last_ts=240_000,
        elapsed=0.0,
        applied_inputs={},
        applied_overrides={},
        applied_runtime={},
    )
    if replay is not _MISSING:
        arguments["replay"] = replay
    return run_json.build_report_dict(report, **arguments)


def _summary_only_sink():
    diagnostics = SimpleNamespace(
        count_e=0,
        equity_failure=None,
        trade_at_start=0,
    )
    return SimpleNamespace(
        fd_mode=False,
        named=False,
        summary_failure=None,
        summary=None,
        diagnostics=diagnostics,
        counts={"E": 0, "T": 0, "O": 0, "V": 0, "K": 0, "X": 0, "H": 0},
        records=lambda _tag: iter(()),
    )


def _sink_report(report, sink, replay):
    return run_json._build_sink_report(
        report,
        Path("replay-s.csv"),
        4,
        60_000,
        240_000,
        0.0,
        {},
        {},
        {},
        sink,
        replay=replay,
    )


def test_replay_reader_calls_supplied_accessor_for_each_handle():
    library = _ReplayAccessor()

    first = run_json.read_replay_values(
        library, 41, initial_capital=5_000.0, pointvalue=2.5
    )
    second = run_json.read_replay_values(
        library, 73, initial_capital=12_345.0, pointvalue=0.25
    )

    assert first == {
        "initial_capital": 5_000.0,
        "pointvalue": 2.5,
        "net_profit_sum": 17.0,
    }
    assert second == {
        "initial_capital": 12_345.0,
        "pointvalue": 0.25,
        "net_profit_sum": -6.25,
    }
    assert library.calls == [41, 73]


def test_report_replay_switch_reaches_library_admission_instead_of_argparse(
        capsys, tmp_path):
    bars = tmp_path / "bars.csv"
    bars.write_text(
        "open,high,low,close,volume,timestamp\n"
        "1,2,0.5,1.5,10,1000\n",
        encoding="utf-8",
    )
    missing_library = tmp_path / "missing.so"

    status = run_json.main([
        "--report-replay",
        "--so", str(missing_library),
        "--ohlcv", str(bars),
    ])

    assert status == 1
    document = json.loads(capsys.readouterr().out)
    assert document["code"] == "strategy_library_incompatible"
    assert document["args"]["reason"] == "load_failed"


def test_replay_namespace_is_additive_and_has_all_three_finite_scalars():
    report = _report(())
    document = _array_report(report, _REPLAY)

    assert list(document["replay"]) == [
        "initial_capital", "pointvalue", "net_profit_sum"
    ]
    assert document["replay"] == _REPLAY
    assert all(type(value) is float and math.isfinite(value)
               for value in document["replay"].values())


@pytest.mark.parametrize("supplied", [17.0, -6.25])
def test_replay_builder_preserves_supplied_value_with_terminal_rows(supplied):
    rows = [
        _trade(entry=100, exit=107, qty=1, pnl=6, commission=1),
        _trade(entry=100, exit=109, qty=1, pnl=8, commission=1),
        _trade(entry=120, exit=116, qty=1, pnl=3, commission=1, is_long=False),
        _trade(entry=130, exit=999, qty=1, pnl=900, commission=0, open_at_end=True),
    ]
    report = _report(rows, report_net_profit=12_345.0)
    replay = dict(_REPLAY, net_profit_sum=supplied)

    document = _array_report(report, replay)

    assert document["replay"]["net_profit_sum"] == supplied
    assert document["summary"]["net_pnl"] == 12_345.0
    assert document["trades"][-1]["open_at_end"] is True
    assert document["trades"][-1]["pnl"] == 900.0


def test_replay_builder_preserves_supplied_zero_with_empty_trade_rows():
    document = _array_report(
        _report((), report_net_profit=0.0),
        {"initial_capital": 5_000.0, "pointvalue": 2.5, "net_profit_sum": 0.0},
    )

    assert "replay" in document
    assert document["replay"]["initial_capital"] == 5_000.0
    assert document["replay"]["pointvalue"] == 2.5
    assert type(document["replay"]["net_profit_sum"]) is float
    assert math.isfinite(document["replay"]["net_profit_sum"])
    assert document["replay"]["net_profit_sum"] == 0.0


def test_extension_off_has_no_replay_key_and_preserves_report_values():
    """Current-behavior OFF guard; needs no proposed replay keyword."""
    report = _report([
        _trade(entry=100, exit=107, qty=1, pnl=6, commission=1),
        _trade(entry=130, exit=999, qty=1, pnl=900, commission=0, open_at_end=True),
    ], report_net_profit=906.0)

    document = _array_report(report)

    assert "replay" not in document
    assert document["summary"]["total_trades"] == 2
    assert document["summary"]["net_pnl"] == 906.0
    assert [row["pnl"] for row in document["trades"]] == [6.0, 900.0]
    assert document["trades"][-1]["open_at_end"] is True


def _report_text(document):
    # The array route holds bounded record arrays that plain json cannot dump:
    # serialise with the report writer's own compact encoder.
    encoded = io.StringIO()
    run_json._stream_json(document, encoded)
    return encoded.getvalue()


def test_extension_off_has_no_replay_key_and_matches_extension_on_after_removal():
    report = _report(())
    off = _array_report(report)
    on = _array_report(report, _REPLAY)

    assert "replay" not in off
    assert "replay" in on
    del on["replay"]
    assert _report_text(on) == _report_text(off)


@pytest.mark.parametrize("field", ["initial_capital", "pointvalue", "net_profit_sum"])
@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf],
                         ids=["nan", "positive-infinity", "negative-infinity"])
def test_replay_scalars_use_the_existing_nonfinite_result_contract(field, bad):
    replay = dict(_REPLAY, **{field: bad})
    with pytest.raises(run_json.RunFailure) as caught:
        run_json._validate_report_result(_array_report(_report(()), replay))

    assert caught.value.code == "report_result_invalid"
    assert caught.value.code_args == {
        "field": "replay." + field,
        "reason": "nonfinite",
    }


def test_sink_builder_passes_the_same_scalars_without_changing_primary_summary(
        monkeypatch):
    report = _report(())
    sink = _summary_only_sink()
    monkeypatch.setattr(
        run_json,
        "_sink_summary",
        lambda _report_value, _sink_value: {
            "total_trades": 0,
            "wins": 0,
            "losses": 0,
            "win_rate_pct": 0.0,
            "net_pnl": 0.0,
            "avg_trade": 0.0,
            "best_trade": 0.0,
            "worst_trade": 0.0,
            "max_drawdown": 0.0,
            "bars_processed": 4,
        },
    )
    monkeypatch.setattr(run_json, "_validate_report_result", lambda _out: None)
    monkeypatch.setattr(run_json, "_annualization_block", lambda *args, **kwargs: {})

    document = _sink_report(report, sink, _REPLAY)

    assert document["replay"] == _REPLAY
    assert document["summary"]["net_pnl"] == 0.0


def test_primary_sink_bytes_are_unchanged_by_replay_negotiation(tmp_path):
    import pf_sink

    off_primary = tmp_path / "off-primary.jsonl"
    on_primary = tmp_path / "on-primary.jsonl"
    companion = tmp_path / "on-replay.jsonl"
    values = {
        "initial_capital": 5_000.0,
        "pointvalue": 2.5,
        "net_profit_sum": 17.0,
    }

    run_json.write_report_sinks(
        primary_path=off_primary,
        replay_path=None,
        run_id="replay",
        **values,
    )
    run_json.write_report_sinks(
        primary_path=on_primary,
        replay_path=companion,
        run_id="replay",
        **values,
    )

    assert off_primary.read_bytes() == on_primary.read_bytes()
    with pf_sink.ReplaySinkReader(companion) as reader:
        assert [record.tag for record in reader] == ["S"]
        assert reader.complete


@pytest.mark.parametrize(
    "initial_capital,pointvalue,net_profit_sum",
    [(5_000.0, 2.5, 17.0), (12_345.0, 0.25, -6.0)],
    ids=["positive-profit", "negative-profit"],
)
def test_replay_companion_writer_round_trips_each_input_value(
        tmp_path, initial_capital, pointvalue, net_profit_sum):
    import pf_sink

    path = tmp_path / "replay.jsonl"
    run_json.write_replay_sink(
        path,
        run_id="replay",
        initial_capital=initial_capital,
        pointvalue=pointvalue,
        net_profit_sum=net_profit_sum,
    )

    with pf_sink.ReplaySinkReader(path) as reader:
        records = list(reader)
        assert reader.complete
        assert reader.counts == {"S": 1}
        assert [record.tag for record in records] == ["S"]
        assert records[0].values == (
            initial_capital, pointvalue, net_profit_sum
        )
