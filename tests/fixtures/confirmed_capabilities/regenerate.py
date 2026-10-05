"""Snapshot real generated strategies for confirmed-bar admission proofs."""
import argparse
from pathlib import Path

from pineforge_codegen import transpile


PROVEN = ("htf5_close", "htf15_sma_pooc", "htf60_close", "daily_close", "heikinashi5",
          "htf60_ema_gaps", "period_previous", "pooc_dual_stop", "pooc_limit_reversal",
          "pooc_slipped_short", "pooc_short_exit")
MARKET = '''if bar_index % 7 == 0
    strategy.entry("long", strategy.long)
if bar_index % 7 == 3
    strategy.entry("short", strategy.short)
'''


def sources():
    fixtures = Path(__file__).resolve().parent.parent
    result = {name: (fixtures / "stream_security" / name / "strategy.pine").read_text()
              for name in PROVEN}
    result["htf5_close_active"] = result["htf5_close"].replace(
        "ta.crossover(close, h)", "ta.crossover(h, ta.sma(h, 3))").replace(
        "ta.crossunder(close, h)", "ta.crossunder(h, ta.sma(h, 3))")
    result["constant_daily"] = result["daily_close"].replace(
        'h = request.security(syminfo.tickerid, "D", close)',
        'htf = "D"\nh = request.security(syminfo.tickerid, htf, close)')
    result["helper5"] = result["htf5_close"].replace(
        'h = request.security(syminfo.tickerid, "5", close)',
        'f(tf) => request.security(syminfo.tickerid, tf, close)\nh = f("5")')
    result["heikinashi_alias5"] = result["heikinashi5"].replace(
        'h = request.security(ticker.heikinashi(syminfo.tickerid), "5", close)',
        'symbol = ticker.heikinashi(syminfo.tickerid)\nh = request.security(symbol, "5", close)')
    result["varip"] = ('//@version=6\nstrategy("confirmed varip")\nvarip int count = 0\n'
                       'count += 1\nif count % 7 == 1\n    strategy.entry("long", strategy.long)\n'
                       'if count % 7 == 4\n    strategy.entry("short", strategy.short)\n')
    result["pooc_market"] = '//@version=6\nstrategy("closing market", process_orders_on_close=true)\n' + MARKET
    result["pooc_close"] = ('//@version=6\nstrategy("closing close", process_orders_on_close=true)\n'
                            'if bar_index % 7 == 0\n    strategy.entry("long", strategy.long)\n'
                            'if bar_index % 7 == 3\n    strategy.close_all()\n')
    result["pooc_close_entry"] = result["pooc_close"].replace(
        'strategy.close_all()', 'strategy.close("long", immediately=false)')
    result["pooc_close_short"] = result["pooc_close"].replace('strategy.long', 'strategy.short')
    result["pooc_close_short_entry"] = result["pooc_close_short"].replace(
        'strategy.close_all()', 'strategy.close("long", immediately=false)')
    for clock in ("W", "5D", "30S", "M", "7"):
        result["refused_clock_" + clock] = result["htf5_close"].replace('"5", close', f'"{clock}", close')
    result["refused_foreign"] = result["htf5_close"].replace('syminfo.tickerid', '"NASDAQ:MSFT"')
    result["refused_empty_symbol"] = result["htf5_close"].replace('syminfo.tickerid', '""')
    result["refused_lookahead"] = result["htf5_close"].replace('"5", close)', '"5", close, lookahead=barmerge.lookahead_on)')
    result["refused_expression"] = result["htf5_close"].replace('"5", close)', '"5", open)')
    result["refused_gaps"] = result["htf5_close"].replace('"5", close)', '"5", close, gaps=barmerge.gaps_on)')
    result["refused_dynamic"] = result["htf5_close"].replace('h = request.security', 'tf = input.timeframe("5")\nh = request.security').replace('"5", close)', 'tf, close)')
    rejected_orders = {
        "stop_limit": 'strategy.entry("L", strategy.long, stop=high, limit=low)',
        "oca": 'strategy.entry("L", strategy.long, stop=high, oca_name="group", oca_type=strategy.oca.cancel)',
        "long_bracket": 'strategy.entry("L", strategy.long)\nstrategy.exit("X", "L", stop=high, limit=low)',
        "mixed_bracket": 'strategy.entry("S", strategy.short)\nstrategy.entry(id="S", direction=strategy.long)\nstrategy.exit("X", "S", stop=high, limit=low)',
        "mixed_market_bracket": 'strategy.entry("S", strategy.short)\nstrategy.entry("L", strategy.long)\nstrategy.exit("X", "S", stop=high, limit=low)',
        "trail": 'strategy.entry("S", strategy.short)\nstrategy.exit("X", "S", trail_points=2, trail_offset=1)',
        "order": 'strategy.order("L", strategy.long)',
        "cancel": 'strategy.entry("L", strategy.long, stop=high)\nstrategy.cancel("L")',
        "immediate": 'strategy.entry("L", strategy.long)\nstrategy.close("L", immediately=true)',
        "risk_filled": 'strategy.risk.max_intraday_filled_orders(2)\nstrategy.entry("L", strategy.long)',
        "risk_drawdown": 'strategy.risk.max_drawdown(5, strategy.percent_of_equity)\nstrategy.entry("L", strategy.long)',
        "risk_direction": 'strategy.risk.allow_entry_in(strategy.direction.long)\nstrategy.entry("L", strategy.long)\nstrategy.entry("S", strategy.short)',
        "risk_loss": 'strategy.risk.max_intraday_loss(5, strategy.percent_of_equity)\nstrategy.entry("L", strategy.long)',
        "risk_size": 'strategy.risk.max_position_size(2)\nstrategy.entry("L", strategy.long)',
        "risk_days": 'strategy.risk.max_cons_loss_days(2)\nstrategy.entry("L", strategy.long)',
        "exit_oca": 'strategy.entry("S", strategy.short)\nstrategy.exit("X", "S", stop=high, limit=low, oca_name="g")',
        "entry_qty": 'strategy.entry("S", strategy.short, qty=1)',
        "dynamic_entry_id": 'order_id = input.string("S")\nstrategy.entry(order_id, strategy.short)',
        "dynamic_close_id": 'order_id = input.string("S")\nstrategy.entry("S", strategy.short)\nstrategy.close(order_id)',
        "dynamic_exit_id": 'order_id = input.string("X")\nstrategy.entry("S", strategy.short)\nstrategy.exit(order_id, "S", stop=high, limit=low)',
        "entry_comment": 'strategy.entry("S", strategy.short, comment="unmodeled")',
        "entry_alert": 'strategy.entry("S", strategy.short, alert_message="unmodeled")',
        "entry_disable_alert": 'strategy.entry("S", strategy.short, disable_alert=true)',
        "exit_comment": 'strategy.entry("S", strategy.short)\nstrategy.exit("X", "S", stop=high, limit=low, comment="unmodeled")',
        "exit_alert": 'strategy.entry("S", strategy.short)\nstrategy.exit("X", "S", stop=high, limit=low, alert_profit="unmodeled")',
        "close_comment": 'strategy.entry("S", strategy.short)\nstrategy.close("S", comment="unmodeled")',
        "close_alert": 'strategy.entry("S", strategy.short)\nstrategy.close_all(alert_message="unmodeled")',
        "cancel_all": 'strategy.entry("L", strategy.long)\nstrategy.cancel_all()',
        "pending_bracket": 'strategy.entry("S", strategy.short, stop=low)\nstrategy.exit("X", "S", stop=high, limit=low)',
        "mixed_entries": 'strategy.entry("L", strategy.long, stop=high)\nstrategy.entry("S", strategy.short, limit=high)',
        "priced_close": 'strategy.entry("L", strategy.long, stop=high)\nstrategy.close("L")',
        "all_categories": 'varip int count = 0\ncount += 1\nh = request.security(syminfo.tickerid, "D", close)\nstrategy.entry("S", strategy.short, stop=h)\nstrategy.exit("X", "S", stop=high, limit=low)',
        "daily_market": 'h = request.security(syminfo.tickerid, "D", close)\nstrategy.entry("L", strategy.long)',
        "varip": 'varip int count = 0\ncount += 1\nstrategy.entry("L", strategy.long)',
        "request_priced": 'h = request.security(syminfo.tickerid, "15", ta.sma(close, 4))\nstrategy.entry("L", strategy.long, stop=h)',
    }
    for name, body in rejected_orders.items():
        result["refused_pooc_" + name] = '//@version=6\nstrategy("unproven closing", process_orders_on_close=true)\n' + body + '\n'
    result["refused_two_requests"] = result["htf5_close"].replace(
        'if ta.crossover', 'daily = request.security(syminfo.tickerid, "D", close)\nif ta.crossover')
    for name, settings in {
        "priced_sizing": 'default_qty_type=strategy.percent_of_equity, default_qty_value=100',
        "priced_slippage": 'slippage=15', "pyramiding": 'pyramiding=2', "margin": 'margin_short=50',
        "commission": 'commission_value=1', "capital": 'initial_capital=5000', "close_rule": 'close_entries_rule="ANY"',
    }.items():
        result["refused_pooc_" + name] = result["pooc_dual_stop"].replace(
            'process_orders_on_close=true)', f'process_orders_on_close=true, {settings})')
    result["refused_pooc_slipped_request"] = result["htf15_sma_pooc"].replace(
        'process_orders_on_close=true)', 'process_orders_on_close=true, slippage=15, '
        'default_qty_type=strategy.percent_of_equity, default_qty_value=100)')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    output = parser.parse_args().output
    output.mkdir(parents=True, exist_ok=True)
    for name, source in sources().items():
        (output / f"{name}.pine").write_text(source)
        (output / f"{name}.cpp").write_text(transpile(source))
    print(f"generated {len(sources())} confirmed-bar fixtures")


if __name__ == "__main__":
    main()
