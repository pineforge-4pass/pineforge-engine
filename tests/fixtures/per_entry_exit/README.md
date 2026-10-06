# A global exit over pyramided entries, entry by entry

A `strategy.exit` with no `from_entry` and a relative leg (`profit`, `loss`), called while a position is
held, binds to that position (tests/fixtures/cross_side_exit, tests/fixtures/same_side_exit). Once the
position holds a second entry -- here an add of the held side that fills after the exit was called --
TradingView does not re-price one exit at the average price. It covers the position entry by entry:

1. Each entry has its own exit orders, at levels from its own fill price (profit: fill + / - P ticks,
   loss: fill - / + L ticks). The two legs of one entry are one bracket.
2. An entry that fills after the exit was called gets its orders at its fill; they act for the rest of
   that bar.
3. Each entry's orders fill at most once: a re-issued exit does not give a consumed entry new orders.
4. Every exit fill closes the oldest open trade (FIFO), whichever entry's orders filled. A long that
   adds below its entry and then reaches the add's profit level closes its FIRST trade there, and the
   add's trade later at the first entry's level.
5. A book that goes flat ends the exit's remaining orders: an entry that fills later in that bar waits
   for the next call.
6. At one point of the path, stop and market orders fill before limit orders, and among the stop and
   market orders buys before sells; two orders on one tick meet at one point. A long's limit add that a
   gapped open passes together with the exit's stop fills after the stop closed the position, and waits
   for the next call for its own orders.
7. The broker path is O-H-L-C when |H - O| < |O - L|, else O-L-H-C; resting orders are tested against
   the tick-quantized bar, and a level the open gaps through fills at the quantized open.

The engine booked one exit for the whole position, re-priced at the average price at every call: all 60
tapes without `calc_on_order_fills` departed, at trades 1 to 8.

## The engine

`ExitBindingRuleSwitches::global_exit_per_entry_levels` turns the rule off for tests.

- A position of one entry keeps its one exit at the average price, which is that entry's own bracket.
  When a second entry fills while the exit stands, the exit's legs become the first entry's bracket,
  closing that entry's units, and the new entry's bracket is placed at its fill, so it acts for the
  rest of the bar (`PineExecutionAdapter::per_entry_exits_at_entry_fill`). A call made over two
  entries places one bracket per entry (`per_entry_exit_call`).
- Each bracket is a cancel group of its own; its legs are host-sized closes of the book that close the
  entry's units, and the kernel closes the oldest lots first (rule 4). A filled bracket is never
  placed again; a flat book ends the brackets left (`per_entry_exits_after_fill`).
- Rule 6: the kernel breaks a tie at one point by queue order, so after the script the add and the
  exit's legs are re-priced in place into TradingView's order (`order_per_entry_point_ties`, the
  technique of `order_open_marketable_limit_entries`). Rule 7 is the existing path and tick-quantized
  trigger thresholds; no change was needed there.

The point-tie re-price moves the selected requests behind every other working request at that
point, not only behind each other. It also moves the broker state hash of runs with a held
global relative exit beside a same-side add. Entry incarnation is an ABI provenance field,
run-scoped rather than a stable cross-run identifier; its renumbering in unreached-add controls
does not change their fill, price, quantity, money or time fields.

The tapes pin the rule without `calc_on_order_fills` and without `process_orders_on_close`, on the
chart's own bars, under the FIFO close rule, for lots opened by `strategy.entry` and an exit with only
relative profit and loss legs (no absolute level, trail, quantity, percent below 100 or OCA name).
Every other configuration keeps the one exit at the average price, and an add sized by the core (an
unpriced default quantity) keeps the queue order.

Inside these gates the rule also decides shapes no tape reaches, as the predictor's model reads them: a
first call made over two entries already held (each takes its bracket at the call, and a leg the exit
held over the whole position gives way), a third entry (pyramiding above 2), and entries of different
quantities (each bracket closes its own entry's units, the transaction size of
tests/fixtures/global_exit_children). Without a model either: a call that changes the tick distances
re-prices the brackets still live and leaves a consumed one consumed, and a `strategy.cancel` of the
exit's id or `strategy.cancel_all` ends the record with the orders it cancels.

## The tapes

Synthetic scripts of our own on NYSE:F 15m (mintick 0.01, quantity 1, capital 1,000,000, pyramiding 2),
each exported once with `lab tv --no-note` (`provenance.json`: the pin's script name, its parameters,
the range, returned range, raw-frame hash, and the codegen that froze `generated.cpp`). Each script
holds one lot (entry `L` long or `H` short, market, when flat), places once, on its first held bar, the
add `A` of the held side, and calls `strategy.exit("X", ...)` with no `from_entry` on every held bar;
when flat it cancels `A`. A tape is named `<set>-<side>-<legs>-<add>[-cf]`:

| part | values |
|---|---|
| set | `a`: 2025-04-01 .. 2026-05-01, where the rules were read off; `b`: the same period, fresh parameters; `c`: 2024-04-01 .. 2025-04-01; `d`: 2023-04-03 .. 2024-03-29 |
| side | `l`: long; `s`: short |
| legs | `p<P>`: profit = P ticks; `l<L>`: loss = L ticks; `p<P>l<L>`: both |
| add | `lim`: a limit some ticks through the entry price; `stp`: a stop some ticks beyond it; `mkt`: a market add (the offsets are in `provenance.json`) |
| `-cf` | `calc_on_order_fills`, 2025-04-01 .. 2025-05-01 |

Sets `b`, `c` and `d` were each predicted before their export by a forward predictor of these rules,
frozen with its sha256 (`provenance.json` `predictor_sha256` is the final model's): `b` 15 of 16 (the
miss gave rule 6 its buy-before-sell), `c` 13 of 16 (the misses gave the one-tick point), `d` 16 of 16
with the final model, which reproduces all 60 tapes, 158,960 rows. `bars/` holds the lane's chart-feed
rows of each window (`nyse-f-15-2025-04-01-2026-04-30.csv` and `nyse-f-15-2025-04-01-2025-04-30.csv` are
byte-identical to tests/fixtures/same_side_exit's). Set `d` is the re-export over 2023-04-03 ..
2024-03-29: the first export over 2023-04-01 .. 2024-04-01 returned a narrower range proof (the bounds
fall on a weekend and Good Friday) over the same 6,450 bars, and was byte-identical.

## Known divergences

The six `-cf` tapes are outside the rule: the pin exported them but did not model a fill
recalculation. The engine books them exactly as before the rule, with the switch on or off; four depart
from TradingView (`known_divergences.json`, with the rows), two equal it.

## The test

`test_per_entry_exit_tapes` (tests/test_per_entry_exit_tapes.py) runs every tape's frozen generated
strategy through the C ABI. With the rule on, every tape not listed in `known_divergences.json` must
equal TradingView row for row (count, side, quantity, entry and exit time and price), and every listed
one must depart exactly at its recorded row. With the switch off exactly the 60 tapes without
`calc_on_order_fills` move, each departing from TradingView as `rule_off_departures.json` records. The
`-cf` tapes must be refused by a forward stream; the other 60 are replayed as a stream over each run of
the feed between two nights (1 and 30 bars of history), and must book the trades of a backtest over the
same bars with the rule on, and over the first run with it off too.
