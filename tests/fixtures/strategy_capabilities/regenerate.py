"""Generate join fixtures with the paired codegen checkout on a build host."""
import argparse
from pathlib import Path

from pineforge_codegen import transpile


MARKET = '''if bar_index % 7 == 0
    strategy.entry("long", strategy.long)
if bar_index % 7 == 3
    strategy.entry("short", strategy.short)
'''
SOURCES = {
    'close': ('', MARKET),
    'clock_flags': ('timeframe_gaps=false, dynamic_requests=false', MARKET),
    'tick': ('calc_on_every_tick=true', MARKET),
    'fills': ('calc_on_order_fills=true', MARKET),
    'pooc': ('process_orders_on_close=true', MARKET),
    'magnifier': ('use_bar_magnifier=true', MARKET),
    'standard': ('fill_orders_on_standard_ohlc=true', MARKET),
    'limits': ('backtest_fill_limits_assumption=3', MARKET),
    'currency': ('currency=currency.EUR', MARKET),
    'timeframe': ('timeframe="15"', MARKET),
    'history_tick': ('calc_on_every_history_tick=true', MARKET),
    'unresolved': ('pyramiding=COUNT', 'const int COUNT = 1\n' + MARKET),
    'unresolved_tick': ('calc_on_every_tick=input.bool(false)', MARKET),
    'varip': ('', 'varip int count = 0\ncount += 1\n' + MARKET),
    'realtime': ('', 'if barstate.isrealtime\n    strategy.entry("entry", strategy.long)\n'),
    'timenow': ('', 'if timenow > time\n    strategy.entry("entry", strategy.long)\n'),
    'islast': ('', 'if barstate.islast\n    strategy.entry("entry", strategy.long)\n'),
    'lastconfirmedhistory': ('', 'if barstate.islastconfirmedhistory\n    strategy.entry("entry", strategy.long)\n'),
    'last_bar_index': ('', 'if bar_index == last_bar_index\n    strategy.entry("entry", strategy.long)\n'),
    'last_bar_time': ('', 'if time == last_bar_time\n    strategy.entry("entry", strategy.long)\n'),
    'security': ('', 'value = request.security(syminfo.tickerid, "5", close)\nif value > open\n    strategy.entry("entry", strategy.long)\n'),
    'lower_tf': ('', 'values = request.security_lower_tf("NASDAQ:MSFT", "1", close)\nif array.get(values, 0) > close\n    strategy.entry("entry", strategy.long)\n'),
    'recorded': ('', 'value = request.earnings("NASDAQ:MSFT")\nif value > close\n    strategy.entry("entry", strategy.long)\n'),
    'unpinned': ('', 'float value = na\nif true\n    symbol = "NASDAQ:MSFT"\n    value := request.security(symbol, "D", close)\nif not na(value)\n    strategy.entry("entry", strategy.long)\n'),
    'footprint': ('', 'value = request.footprint(syminfo.tickerid, 100)\nif not na(value)\n    strategy.entry("entry", strategy.long)\n'),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    output = parser.parse_args().output
    output.mkdir(parents=True, exist_ok=True)
    for name, (declaration, body) in SOURCES.items():
        source = f'//@version=6\nstrategy("join", {declaration})\n{body}' if declaration else f'//@version=6\nstrategy("join")\n{body}'
        (output / f'{name}.pine').write_text(source)
        (output / f'{name}.cpp').write_text(transpile(source))
    positional = '//@version=6\nstrategy("t","s",true,format.price,2,scale.right,1,true,true)\n' + MARKET
    (output / 'positional.pine').write_text(positional)
    (output / 'positional.cpp').write_text(transpile(positional))
    print(f'generated {len(SOURCES) + 1} capability join fixtures')


if __name__ == '__main__':
    main()
