# Reversal rules under a 100%-of-equity default (lane TVDEF-DROPS)

Pine v6's default order size is 100% of equity (lane TV-DEFAULTS,
`tests/fixtures/tv_strategy_defaults`). Many scripts that used to trade one
contract now size every entry from the account, and three Pine adapter rules
that R4 slice C lost while lowering ab9714be's source broker onto the native
kernel surfaced as population drops. Each rule is pinned by synthetic probes
written for this lane; no closed or scraped strategy is involved.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-05-01, with fixed `timestamp("UTC", ...)` cells on
2025-04-08 .. 2025-04-10 and a `strategy.cancel_all()` plus
`strategy.close_all()` cleanup two hours after each cell. None declares
`initial_capital`, so each runs on v6's 100000.

`tests/test_tvdef_drops_tapes.cpp` replays every tape through the Pine adapter
under the configuration the generated constructor declares for that probe,
trading from the bar before TradingView's first entry (2025-04-08 00:00 UTC)
as `run_strategy.py` does for a corpus tape, with TradingView's 0.0001 lot as
the `qty_step`, and requires each trade the tape closes inside the replayed
bars -- entry and exit time, side, price in ticks of 0.01, quantity in lots of
0.0001 -- to be the engine's. It also reads each rule off TradingView's own
rows.

## R1: a default percent stop that reverses keeps its closing leg

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `tdd-r1-stop-reversal-close-only` | 2 | nothing (v6: percent_of_equity, 100) | each long seed is closed at its sell stop (1548, 1640) by the reversal, and no short opens | `19548c0a494a39ece13982acbc96b73323ba53c7eb1d21d70ab2b8ed0f8c9f6b` |
| `tdd-r1-stop-reversal-half` | 4 | `default_qty_type = strategy.percent_of_equity, default_qty_value = 50` | the same stops close the long and open the short | `f0ea2dbacd3c03b52ff7a269b50559945cc1b6224f9d4e3cecd875575f4f5925` |

A default percent_of_equity (at most 100) pure STOP entry is sized at its
directionally snapped level and priced at the signal close. A sell stop sits
below the close, so at 100% its short needs more margin than the equity:
TradingView keeps only the closing leg (ab9714be
`pine_strategy_commands.cpp:408-419`). The lowering dropped the whole order,
so the long was held to the end of the range. At 50% the short funds.

## R2: an explicit quantity is never a declined reversal

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `tdd-r2-explicit-reversal-bracket` | 4 | nothing (v6: percent_of_equity, 100) | each qty=1 reversal opens and its from_entry bracket scratches it at the reversal's open | `1e6e1c5de83a75a62781dcf1778e4dfbf356d44352e4c097e920dd9e69b336d2` |
| `tdd-r2-explicit-reversal-bracket-fixed1` | 4 | `default_qty_type = strategy.fixed, default_qty_value = 1` | the same rows: the two tapes are one file | `1e6e1c5de83a75a62781dcf1778e4dfbf356d44352e4c097e920dd9e69b336d2` |

Every entry names `qty=1`. The bracket's stop is already through at the
reversal's open fill (1556 above the 1553.57 open for the long, 1470 below the
1473.01 open for the short), so TradingView exits the new side there. Only a
default quantity can be declined at the open (ab9714be
`pine_fills.cpp:5227-5235` scopes the decline to `frozen_default_qty`, which
`pine_strategy_commands.cpp:596-601` sets only when qty is omitted); the
lowering priced the one-unit reversal at the default value (100 units),
declined it and cancelled the bracket.

## R3: an entry reversal voids the reversed side's from_entry="" exit

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `tdd-r3-reversal-voids-global-exit` | 6 | nothing (v6: percent_of_equity, 100) | each reversed side exits later by its own exit, never by the prior side's; margin calls trim the 100% shorts on their entry bars | `38b5fe54c5bc6865f516dcc2394d961fbc69d75693424fcb14f875f5dbad3374` |
| `tdd-r3-reversal-voids-global-exit-half` | 4 | `default_qty_type = strategy.percent_of_equity, default_qty_value = 50` | the same, without margin calls | `99291b37185dedb19d5675e294428f2db7b05ffd8143580f61dc19033018f94b` |
| `tdd-r3-reversal-voids-global-exit-fixed1` | 4 | `default_qty_type = strategy.fixed, default_qty_value = 1` | the same timing at one contract | `c4856aabf8d16a69f33a3699de44a16407b82fab24520fd0ecae26788347966f` |
| `tdd-r3b-reversal-voids-right-side-exit` | 7 | nothing (v6: percent_of_equity, 100) | the reversal-bar exit, placed once with levels on the new side's correct side (A: stop 1540 / limit 1570 for the long; B: stop 1490 / limit 1440 for the short), is void too: both ride to the cleanup; C's exit, placed with an entry from flat, closes it at 1640 | `bd15a8cb437bbd2d923923e7a6e5bf18d7947af16120b4ce9d4a5f1b0230d151` |

While a side is held the script re-issues a `from_entry=""` exit for it every
bar (`strategy.position_avg_price` -15 / +15 for a short, +20 / -20 for a
long). On the bar a MARKET entry for the other side is placed, that exit is
issued again after the entry, still for the held side; at the reversal's open
fill its levels are marketable against the new side. TradingView never applies
it there or later, and `tdd-r3b-reversal-voids-right-side-exit` shows the same
for an exit whose levels suit the new side: the exit belongs to the position it
was placed under. Under a fixed default the lowering batched the reversal
behind the bar's commands, so the kernel bound the exit to the held book and
cancelled it with that book; under a percent default the reversal reached the
kernel first and the exit bound to the new position and closed it at its
entry open.

## Bars

`bars.inc` holds the replayed bars, 2025-04-07 00:00 .. 2025-04-10 12:00 UTC,
copied as text from the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`) from the
corpus 1m feed (sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`);
`exec/TVDEF-DROPS-scratch/tools/gen_bars_inc.py` wrote it.
