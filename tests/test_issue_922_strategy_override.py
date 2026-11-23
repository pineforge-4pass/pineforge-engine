"""Check the bytes sent by the issue's JSON override to the native setter."""

import json
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
ISSUE_JSON = '{"strategy_overrides": {"process_orders_on_close": true}}'
ISSUE_PARAMS = {"strategy_overrides": {"process_orders_on_close": True}}
CHART = ROOT / "tutorial/data/btcusdt_15m_7d.csv"
CHART_SPAN = (672, 1777486500000, 1778090400000)
HANDLE = 7
KEY = b"process_orders_on_close"
EXPECTED = b"true"


class AtBacktest(Exception):
    """Stop at the backtest entry point after taking a snapshot of the setter calls."""


def outcome(status, reason, actual, expected):
    print(f"{status} #922: {reason}: got {actual!r}; expected {expected!r}")
    return 0 if status == "PASS" else 1


def check_issue_922(raw=ISSUE_JSON):
    if not raw.strip():
        return outcome("NOT COMPARED", "empty inputs.json", raw, ISSUE_JSON)
    try:
        params = json.loads(raw)
    except (ValueError, RecursionError) as error:
        return outcome("NOT COMPARED", f"invalid inputs.json ({error})", raw, ISSUE_JSON)
    if (params != ISSUE_PARAMS
            or params["strategy_overrides"]["process_orders_on_close"] is not True):
        return outcome("NOT COMPARED", "wrong inputs.json", params, ISSUE_PARAMS)

    sys.path.insert(0, str(ROOT / "scripts"))
    try:
        import run_strategy as runner
    except ImportError as error:
        return outcome("NOT COMPARED", "runner import failed", str(error),
                       str(ROOT / "scripts/run_strategy.py"))

    calls = []

    def start_backtest(state, bars, count, *_):
        span = (count, bars[0].timestamp if count else None,
                bars[count - 1].timestamp if count else None)
        raise AtBacktest((state, span), tuple(calls))

    strategy = runner.Strategy.__new__(runner.Strategy)
    strategy.lib = SimpleNamespace(
        strategy_create=lambda _: HANDLE,
        strategy_set_override=lambda *args: calls.append(args),
        run_backtest_full=start_backtest,
        report_free=lambda _: None,
        strategy_free=lambda _: None,
    )
    expected_run = (HANDLE, CHART_SPAN)
    try:
        chart, kwargs = runner.inputs_run_kwargs(params, ROOT, CHART, env={})
        # Bind the strategy_overrides metadata key as a non-input for this stub.
        with patch.object(runner, "declared_pine_input_names", return_value=frozenset()), \
                patch.dict(runner.os.environ, {runner.REPORT_SINK_ENV: "0"}):
            strategy.run(chart, params=params, **kwargs)
    except AtBacktest as reached:
        actual_run, writes = reached.args
    except (OSError, ValueError, TypeError, LookupError, RuntimeError) as error:
        return outcome("NOT COMPARED", "backtest setup failed", str(error), expected_run)
    else:
        return outcome("NOT COMPARED", "backtest entry not reached", None, expected_run)

    if actual_run != expected_run:
        return outcome("NOT COMPARED", "wrong backtest handle or chart span",
                       actual_run, expected_run)
    if (len(writes) != 1 or len(writes[0]) != 3
            or writes[0][:2] != (HANDLE, KEY) or not writes[0][2]):
        return outcome("NOT COMPARED", "missing or wrong setter call",
                       writes, ((HANDLE, KEY, EXPECTED),))
    actual = writes[0][2]
    return outcome("PASS" if actual == EXPECTED else "FAIL",
                   "process_orders_on_close wire value", actual, EXPECTED)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        sys.exit(outcome("NOT COMPARED", "override argument count",
                         len(sys.argv) - 1, "0 or 1"))
    sys.exit(check_issue_922(*sys.argv[1:]))
