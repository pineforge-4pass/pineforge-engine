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
        "trail": 'strategy.entry("S", strategy.short)\nstrategy.exit("X", "S", trail_points=2, trail_offset=1)',
        "order": 'strategy.order("L", strategy.long)',
        "cancel": 'strategy.entry("L", strategy.long, stop=high)\nstrategy.cancel("L")',
        "immediate": 'strategy.entry("L", strategy.long)\nstrategy.close("L", immediately=true)',
    }
    for name, body in rejected_orders.items():
        result["refused_pooc_" + name] = '//@version=6\nstrategy("unproven closing", process_orders_on_close=true)\n' + body + '\n'
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
