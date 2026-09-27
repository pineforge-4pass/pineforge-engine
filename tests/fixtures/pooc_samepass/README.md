# process_orders_on_close decides the calculation's exits in its close pass (lane W4-ENG-POOC-SAMEPASS, F08)

TradingView's own trades for exits a bar's close calculation places under
`process_orders_on_close`, written for this lane (no closed or scraped strategy
is involved). Each directory is one `lab tv --no-note` export (channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`. The ETH probes run
on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with fixed
`timestamp("UTC", ...)` cells on 2025-04-08 .. 2025-04-10 and a
`strategy.cancel_all()` plus `strategy.close_all()` cleanup two hours after each
cell; `w4-f08t-pooc` runs on NYSE:F 15, 2025-07-01 .. 2025-07-08, with cells on
2025-07-02 flattened on the next bar. `tests/test_pooc_samepass_tapes.cpp`
replays every tape.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `w4-f08-pooc` | POOC, fixed 1 | 15 | `02002952d943a123520938a3e2f013ae2ef49e912aa779020529b06d499ec5de` |
| `w4-f08-pooc-coof` | POOC + calc_on_order_fills, fixed 1 | 15 | the same file |
| `w4-f08-plain` | fixed 1 | 15 | `819ef43656390131fcbc96eb53de6bd2fd28afdfda139b8742fe1aa4a6e76824` |
| `w4-f08-plain-coof` | calc_on_order_fills, fixed 1 | 15 | the same file |
| `w4-f08b-pooc` | POOC, fixed 1 | 11 | `45ad8a1a2de52e3a3317f2190c819bb1fac510c24d386ea423c9b602072b253c` |
| `w4-f08b-pooc-coof` | POOC + calc_on_order_fills, fixed 1 | 11 | the same file |
| `w4-f08b-plain` | fixed 1 | 11 | `60253261df37b1b5ca82156e315710aa3c86eb6c06a5f731c047a4c97e6f46ea` |
| `w4-f08b-plain-coof` | calc_on_order_fills, fixed 1 | 11 | the same file |
| `w4-f08s-pooc-slip` | POOC, fixed 1, slippage 5 | 7 | `168c32a9b1bf5a3b2bfc795de14b9962e59fdbfb4d0dae04d71a701eaa09b5b1` |
| `w4-f08s-plain-slip` | fixed 1, slippage 5 | 7 | `659c3827964ee03901b3b62f6254ea1bbd392b80def51e61f713b9c144f18b49` |
| `w4-f08s-pooc-pct50` | POOC, percent_of_equity 50 | 7 | `0638a86847ce715aed151529caf84251f6d02d8c599870ad3e97149b74c7c678` |
| `w4-f08s-pooc-v6` | POOC, v6 default (percent_of_equity 100) | 8 | `412b176b1d437eb0ce00279db92afeab07b982b23390fd9d4167bc362f1f86b9` |
| `w4-f08t-pooc` | POOC, fixed 1 (NYSE:F) | 8 | `5e640888d5007b74d2d9be33f7facfcb1b91abd51fa4bb89fcfd126fa53ffa49` |
| `w4-f08r-pooc` | POOC, fixed 1 | 3 | `337d49c911641118071f121613575b9a49b052ff80588b4e9b544dcbb37f40fa` |
| `w4-f08r-plain` | fixed 1 | 3 | `f5ec2d1eeb793cc4d82257db93f841a77cb270867f3a57b9e5b4af9d197b37c2` |

What they show:

- `w4-f08-*` (cells A..N): every exit a calculation places whose level the
  close's tick reaches on its closing side fills at that close under
  `process_orders_on_close` -- with the entry that opens the position (A-D, K, N:
  stop at the close), a held position's (F, G), a close + reversal's and a
  reversal's new side (H, M), a `from_entry ""` exit (J) -- and a level the close
  does not reach (E) rests. Without it the same exits fill at the next open.
  `calc_on_order_fills` changes nothing: the paired tapes are one file each.
  Cell I, an exit placed before the entry it names, is never applied.
- `w4-f08b-*` (cells P..W): the calculation's market orders fill first. A
  reversal or a `strategy.close` takes the held long, whose own exit (through
  the close) never fills (P, Q, S, U); the reversal's new short is decided
  against the same close (R); a re-issued exit through only on its third bar
  fills at that bar's close (V); the second full-quantity exit of one entry is
  never applied (W, TradingView's first-exit reservation).
- `w4-f08s-*`: the same cells under slippage (a stop exit at the close is
  slipped, a limit exit is not) and percent sizing.
- `w4-f08t-pooc`: exit levels 0.002 from half-cent closes decide the close:
  TradingView compares the close's tick (11.465 -> 11.47 fills a sell limit at
  11.467; 11.445 -> 11.45 leaves a sell stop at 11.447 resting), on either side,
  for a fresh and a held position.
- `w4-f08r-*`: an exit the script re-issues on every bar with an unchanged level,
  naming an entry that holds no lot yet, is that entry's exit once the entry is
  placed: it fills at the close that fills the entry (X after a
  `strategy.close` of the held long, Y from flat).

## Bars

`bars.inc` holds the replayed ETH bars, 2025-04-07 00:00 .. 2025-04-10 12:00 UTC,
copied as text from the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`)
from the corpus 1m feed (sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`);
`exec/W4-ENG-POOC-SAMEPASS-scratch/tools/gen_bars_inc.py` wrote it. The NYSE:F
tape replays `tests/fixtures/session_islastbar/bars.inc`'s 15m bars.
