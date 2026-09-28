# A slipped all-in short: its admission and its one-unit margin calls (lane INT28-FIX)

TradingView's own trades for default-quantity percent_of_equity MARKET entries
at Pine v6's defaults (100 % of equity, margin 100 both ways) from a flat book,
written for this lane. Each directory is one `lab tv --no-note` export (channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`, on NYSE:F 15,
2025-04-01 .. 2025-07-01. `tests/test_slipped_short_tapes.cpp` replays every
tape through the Pine adapter over the bars in `bars.inc`, with TradingView's
one-share lot as the `qty_step`, and requires each trade the tape closes inside
the replayed window -- entry and exit time, side, price in ticks of 0.01,
quantity in shares -- to be the engine's.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `int28fix-adm-s-s` | 10000, slippage 1, short | 14 | `2b64805194a04422ec495aa2560e0af7d8b38f29ce3426ac787cbbd8e6df0319` |
| `int28fix-ou-s3` | 4000, commission 0.04 %, slippage 3, short | 17 | `29c741bad36cecb308a90bef065ddb2b1e22e5a12de0e751fe97070d918f4ecc` |
| `int28fix-ou-s4` | 4000, commission 0.04 %, slippage 4, short | 0 | `86e88f3e8067b4b2595036db9feaa39a14df66b43e6b306397c0ed2445acd76c` |
| `int28fix-adm-s-cs` | 10000, commission 0.04 %, slippage 1, short | 147 | `be02018d982d590dee2f808ecec99e944825f951c49ac262746c740ee507b85a` |
| `int28fix-ou-s1` | 4000, commission 0.04 %, slippage 1, short | 600 | `21713e2a157b20dc828b256bf450003c4285792729de20f945167595c97279fd` |
| `int28fix-adm-s-0` | 10000, short (control) | 218 | `fc2743e5ee431da6d0eae7c78c9a37cd4328b61a94855b571b085db0903724f9` |
| `int28fix-adm-s-c` | 10000, commission 0.04 %, short (control) | 231 | `89b71cf21cf8aa63b14b98035b6c7767a6100c9a0dadbec2df175eb8d5aa5ed1` |
| `int28fix-adm-l-cs` | 10000, commission 0.04 %, slippage 1, long (control) | 121 | `bb43cab56586784a40e14de6effdf263916375ccc68292185af1bc6664eb42b7` |
| `int28fix-ou-s2b` | 2000, commission 0.04 %, slippage 2, short, to 2025-10-01 | 896 | `64eff775fa865595507d4b142ce52e2128f6188763e2027e0a6c9bdab7458e05` |

`int28fix-ou-s2b` is replayed by lane W13-ENG-MARGIN-OPP's
`tests/test_margin_opposite_tapes.cpp` (tests/fixtures/margin_opposite), which
also replays `-adm-s-cs` over its whole window.

The `int28fix-adm-*` probes take a default entry from flat at the 10:00 and
13:00 New York bars and flatten it with `strategy.close_all()` at the bar 45
minutes later; the `int28fix-ou-*` probes do the same on every hour from 10:00
to 14:00. An entry fills at the next open; the close_all at the open after it.

What they show:

- SS, the admission: the short is sized at the signal close less the slippage
  ticks, and the commission, and opens only while those units at the signal
  close, on its tick, fit the equity: 6 of 124 cells open at slippage 1
  without commission (`-adm-s-s`), 58 of 123 with it (`-adm-s-cs`), 8 of 310
  at slippage 3 (`-ou-s3`) and none at 4 (`-ou-s4`). Without slippage the
  short is sized at the close itself and only its fill-time admission applies
  (`-adm-s-0`, 107 of 124; `-adm-s-c`, 110 of 124), as for the long, which is
  sized above the close (`-adm-l-cs`, 116 of 124).
- OU, the one-unit call: a restore that floors below one share takes that
  share only where it restores the book at the call's own fill, the whole
  position's requirement at the slipped print less the equity at the mark
  being under one share's margin: a slipped short qualifies only while its
  whole position's slippage stays under that margin, so `-adm-s-s`'s and
  `-ou-s3`'s sub-share deficits take no call, while on the entry bars of
  `-ou-s1`'s window 55 such calls are taken and 11 deficits take none, and in
  `-adm-s-cs`'s 3 and 34. A long's sell always qualifies (`-adm-l-cs`'s five
  calls), as does any call without slippage (`-adm-s-c`'s 22).
- OF, the follow-up: a one-unit call that leaves the book short at its own
  fill -- by more than one share's margin and two slippage steps, the book
  re-marked there -- gives up one more share at the bar's next path point
  where that point's own check calls nothing and its print is inside the
  call's fill; a share so taken is followed alike. `-ou-s1`'s window holds 29
  follow-ups, `-adm-s-cs`'s 3.

## Windows

`tests/test_slipped_short_tapes.cpp` replays `-adm-s-cs` through 2025-06-25
and `-ou-s1` through 2025-04-30: each window ends before the first of the
shapes below that its tape holds, which lane W13-ENG-MARGIN-OPP models
(tests/fixtures/margin_opposite, rule CU):

- A sub-share deficit at a bar's close takes its share at the next open, and
  the one-unit test reads that fill: `-adm-s-cs` 2025-06-26, a short of 810
  shares 4.95 short at the 10.62 close mark, gives one up at 10.62 off the
  10.61 open.
- A follow-up that falls on a bar's close is booked after the script's
  calculation there, so a `strategy.close_all()` the script places on that bar,
  sized at placement, overshoots it into a one-share long at the next open
  (`-ou-s1` from 2025-05-01, `-adm-s-cs` 2025-06-27).

## Open

Measured on these tapes and on the lane's slippage 2 and 3 probes, not
modelled here:

- At two slippage ticks and more a short's check also runs at the bar's low,
  where the book can be short once its fill lies two ticks under the print
  (the lane's `int28fix-ou-s2`, `-s2b` and `-s3b` probes): `-ou-s2b`
  2025-04-25 18:15 UTC (a unit at 10.06 on the bar the cleanup is placed on,
  which the engine does not take) and 2025-05-30 15:15 UTC (a unit on the
  entry bar that the engine takes a bar later).

## Bars

`bars.inc` is the NYSE:F 15m chart feed of the lab lane f-15 (evidence sha256
`80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`), rows
2025-04-01 13:30 .. 2025-06-30 19:45 UTC, copied as text.
