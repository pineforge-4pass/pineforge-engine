# TradingView margins the position a same-open reversal would close (lane W10-DIAG-UNKNOWN, rule SAMEOPEN-REV)

A script can place, on one bar and while flat, a `strategy.entry` long and then
a `strategy.entry` short (with or without a `strategy.close` of the long
between them). Both are market orders for the next open, sized with the
default percent of equity against the same equity. At that open the long
fills first; the short then reverses it. TradingView judges the short against
the equity it was sized from with the long it would close still margined: its
own side plus the held side must fit. Otherwise it refuses the short and the
long stays. The adapter admitted such a reversal whenever its own side
fitted, so at 100 % the reversal went through and a margin call trimmed it.
The same holds when a position is held and the bar places the reversing
entry and then an entry on the held side (at the pyramiding cap when placed):
the first reverses the position at the next open, and the second is a
reversal of a position its own bar's entry opened at that open.

Each directory is one `lab tv --no-note` export of a synthetic probe written
for this lane (`p1-*` by agent gjv, 2026-09-27T21:37-21:39Z;
`w10-sameopen-held-*` 2026-09-28T05:38Z; channel `ws-report-v1`,
`range proof: covered`), byte for byte: `strategy.pine`, `tv_trades.csv`
(times at UTC+8), `metrics.json`, `meta.json`. All run on NASDAQ:AAPL 1D,
2025-04-01 .. 2026-05-01, with 10,000,000 initial capital, a 0.01 %
commission and pyramiding 1. From 2025-04-14 the `p1-*` probes place a cell
every two daily bars in a 12-bar cycle: L1, `strategy.close("LONG")`, S1; the
cleanup X1; L2, S2; X2; S3, `strategy.close("SHORT")`, L3; X3. `p1-c50` runs
a 16-bar cycle that adds L4, S4 with an explicit quantity of half the equity,
and X4. The `w10-sameopen-held-*` probes run a 10-bar cycle: S0; L1, S1 on
one bar; X1; then L0; S2, L2 on one bar; X2.

| tape | default size | tv_trades.csv sha256 |
|---|---|---|
| `p1-a100` | 100 % | `bac7b5cb2ffd8fcfafbe022aa011ddae0022272a9dc48b66f05b902a69ca8cda` |
| `p1-b10` | 10 % | `b4d118a92f4b1dd108c7b624cffa27d0d31dd5ac23725615d7b6630c65aa253f` |
| `p1-c50` | 50 % | `7613999f24d20394691fd57b445ae17971f545dbdefeac2d4ff922b65f0eade8` |
| `w10-sameopen-held-a100` | 100 % | `cb27d51bf2bf3806415870523bbda72b4e59aadae53f0b78a531e427cb984fa2` |
| `w10-sameopen-held-b10` | 10 % | `b145ef0c2d66fd714a43b6f5a90f5e1493ef3e88777ff01465c3a1305e15e2cf` |

What TradingView books:

- 100 %: every L1 fills and runs to X1; no S1 ever fills (it needs 200 %).
  Every S3 fills (margin calls trim them); no L3 ever fills.
- 10 %: every S1 and S2 reverses its long at the same open (20 % fits).
- 50 %: the first L1-CL1-S1 reverses (24686 x 201.86 twice is 9,966,231.92
  against 10,000,000), the first L2-S2 does not (26175 x 196.12 twice is
  10,266,882.00 against 10,113,055.02): the long runs to X2. The first S3-CS3-L3
  keeps its short the same way.
- Held, 100 %: no S1 and no L2 ever fills; every L1 that reverses S0 runs to
  X1, every S2 that reverses L0 runs to X2 (margin calls trim them).
- Held, 10 %: every S1 reverses L1 back at the same open, and every L2
  reverses S2 back (TradingView books each such S2 as a long L2 closed by S2,
  the label swap of `p1-b10`'s S3-CS3-L3).

The base engine booked the 100 % reversals (L1 closed at the open, then a
margin-called short) and the 50 % L2-S2 reversal. It now refuses them. The
population probe that showed it is `htanrisevdir-trail-stop-al-sat-stratejisi`
(100 % of equity, no `process_orders_on_close`) on NASDAQ:AAPL, NSE:NIFTY,
NYSE:F and OANDA:XAUUSD 1D, all four strong on the base engine and excellent
with the rule; the held-side form is `job-2614-andrewwieiw-frosty-alerts`, whose
first divergence on BINANCE:BTCUSDT, OANDA:EURUSD and BINANCE:ETHUSDT.P 15 is a
Short placed while short reversing the same bar's Long back at the open. AAPL and XAUUSD become byte-identical to their tapes; NIFTY and
F keep a margin call split differently on three bars and one (NIFTY 2025-09-03:
TradingView calls 8 of 372 and keeps 364 in one row, the engine calls 4 and
keeps 4 + 360; F 2025-10-23: TradingView keeps 713503 in one row, the engine
83152 + 630351), which is margin-call sizing, not this rule.

Two cells are not this rule, and the test does not compare them:

- `p1-b10` S3-CS3-L3: TradingView reports each as a long L3 closed by S3 at
  the open plus the long L3, where the engine books the short S3 closed by L3
  plus the long L3: the same fills, prices, sizes and money under swapped
  labels. The base engine shares it.
- `p1-c50` L4-S4 (from 2025-05-02): TradingView reverses the explicit-quantity
  L4 into S4; the engine closes L4 and opens no short, as the base engine
  does. The money diverges from there.
- `w10-sameopen-held-b10` S2-L2 (from 2025-04-25): TradingView reverses S2
  back into L2 at 10 %; the engine keeps S2 (its L2 never fills), as the base
  engine does. The rule only refuses, so it cannot be the cause.

`tests/test_same_open_reversal_tapes.cpp` replays the five tapes through the
Pine adapter under the configuration the generated constructor declares,
trading from 2025-04-14 13:30 UTC with TradingView's 0.01 tick and whole-share
lot, and requires the trades the tape closes inside the replayed bars (entry
and exit time, side, price in ticks, quantity) to be the engine's: every
trade of `p1-a100`, every trade of the L1 and L2 cells of `p1-b10`, every
trade of `p1-c50` closed before 2025-05-02, every trade of
`w10-sameopen-held-a100`, and every trade of `w10-sameopen-held-b10` closed
before 2025-04-25. It also reads the rule off TradingView's own rows.
Fail-before on 4091273d (GAPSTOP): 1754 passed, 2 failed (the `p1-a100` and
`p1-c50` replays); for the held-side form, on e8170ffa (the rule for entries
placed flat only): 3024 passed, 1 failed (the `w10-sameopen-held-a100`
replay).

`bars.inc` holds the lane's daily chart feed rows 2025-04-01 .. 2026-04-30
13:30 UTC (sha256 `cac03bf7a41e2e0e2ea3107d56d1c332db5f3329135537b3f07c8c40575e3aab`).
