# margin_call_rules

TradingView strategy tapes of synthetic probes that pin margin-call, admission
and sizing rules: 266 tapes in `cases.tsv` -- 199 asserted (399 paired trade
rows) and 67 recorded only (`short-cutoff-gate`, 203 rows) -- plus 10 order
controls (21 rows). Each probe opens one market position (optionally behind a
stop level, behind a ledger of earlier closed trades, or reversing an explicit
prior long), or a same-bar batch of entries `A`, `B`, `C` for the two
counter controls, at a fixed 15-minute bar and closes it with
`strategy.close_all()` at another (or leaves it open when the chart range
ends first), under a chosen initial capital, quantity
(explicit, 100 % of equity, or the script's `floor(equity / close / step) *
step`), slippage, commission (percent or cash per contract), margin,
`process_orders_on_close` and `calc_on_order_fills`. The tapes were exported
with `lab tv --no-note` over the ws-report-v1 channel.

## Layout

- `<group>/<name>/` — one tape: `strategy.pine` (the probe's source),
  `parameters.json` (the probe's settings), `tv_trades.csv` (TradingView's
  trade list, byte-identical to the export), `metrics.json` and `meta.json`
  (the export's summary and provenance). Groups: `admit`, `boundary`,
  `close-point`, `coof`, `counter-controls`, `factorial`, `fee-sizing`,
  `inherited`, `initial`, `ledger`, `literals`, `order-controls`, `origin`,
  `quantum`, `repeat`, `residual-cash`, `schedule`, `short-cutoff-gate`,
  `sizing`, `stop`, `stopband`. The `literals` and `ledger` `literal-<n>`
  tapes replay population strategies' decisive trades literally.
- `fee-sizing` (13 tapes): process_orders_on_close default
  percent-of-equity sizing on the slipped execution price, fee-grossed when the
  commission is positive; `earned-reversal` reverses an explicit prior long.
  The `factorial/c1r3-fee0-*` tapes are the same sizing at commission 0.
- `short-cutoff-gate` (67 tapes, `cutoff`, `gateedge`, `holdout`, `mincall`,
  `slipgate`): a short's margin-call cut-off and slippage gate. The rule is not
  pinned, so these are recorded, not asserted (`asserted` = 0).
- `order-controls` (10 order controls): timed stop entries,
  `strategy.cancel`, an SMA-readiness gate, a dynamic `strategy.exit` stop and
  a re-entry cooldown. The test replays each with a handwritten host that makes
  its script's `strategy.*` calls; they are not in `cases.tsv`
  (`provenance.json` lists them under `order_controls`).
  The `inherited` tapes' files were materialized from an earlier evidence
  bundle; their `parameters.json` is derived from the Pine source, with the
  chart window (`from`, `to`) taken from the export's provenance.
- `bars/` — 15-minute OHLCV windows (`timestamp,open,high,low,close,volume`,
  timestamp in UTC milliseconds), one file per distinct (feed, from, to):
  every bar from `from` 00:00 UTC through `to` 00:00 UTC inclusive. The two
  counter controls use their own exported window (`eth-counter-<name>.csv`);
  the tapes whose provenance `feed_set` is `own-feed` and the order controls
  read the feeds exported with them (`<feed>-own-<from>-<to>.csv`). A warmup
  order control
  sees exactly its chart range, as TradingView did.
- `provenance.json` — per tape: fixture path, original export path, sha256 of
  `tv_trades.csv` and `strategy.pine`, export time, tool and channel; the
  sha256 and row count of every `bars/` file. The tapes with a
  `repeat_tv_trades_sha256` were exported twice; both exports were
  byte-identical.
  The 10 `factorial` tapes (fill-price and money admission of explicit and
  default entries, slipped, behind a stop, or at margin 50) were exported four
  times, all byte-identical (`repeat_exports`); their `origin` is relative to
  the export's scratch root.
- `cases.tsv` — the generated manifest the test reads, one row per tape.
- `build_cases.py` — regenerates `cases.tsv`.

## cases.tsv

`python3 build_cases.py` verifies every `tv_trades.csv`, `strategy.pine` and
`bars/` file against `provenance.json` and rewrites `cases.tsv` from the
per-tape `parameters.json`; `python3 build_cases.py --check` fails when the
committed manifest is stale. Numbers are written with Python `repr()` so the
test's `strtod` reads the identical double. Columns (tab-separated):

| column | meaning |
|---|---|
| `path` | tape directory |
| `bars` | file under `bars/` |
| `tick`, `step` | price tick and quantity step |
| `capital` | initial capital |
| `direction` | `long` or `short` |
| `qty` | explicit quantity, `percent` (100 % of equity) or `source_raw` (`floor(equity / close / step) * step` at the signal bar) |
| `slippage` | ticks |
| `fee`, `fee_type` | commission value; `percent` or `cash_per_contract` |
| `margin` | margin percent, long and short |
| `pooc`, `coof` | `process_orders_on_close`, `calc_on_order_fills` (0/1) |
| `entry_ms`, `close_ms` | UTC ms of the entry signal bar and the `close_all` bar |
| `to_ms` | the chart's `to` date at 00:00 UTC |
| `stop` | entry stop level, or empty |
| `prior` | ledger trades before the probe, `entry_ms:close_ms:side:qty` (side `long` or `short`) joined by `;`, or empty |
| `lots` | same-bar batch quantities joined by `;`, or empty |
| `pyramiding` | 1, or 3 for a batch |
| `reversal` | the explicit prior long the main order reverses, `entry_ms:qty`, or empty |
| `asserted` | 1, or 0 for a recorded-only tape |

## What the test asserts

`tests/test_margin_call_rules_tapes.cpp` replays every row through the Pine
adapter on every bar of its `bars/` file and compares TradingView's rows —
each Exit row paired with the Entry row of its trade number, in trade-number
order, including a position still open at the range end (reported as a trade
closed at the last bar's close with an empty Signal) — with the engine's
report rows at the same index: side, quantity in lots, entry and exit price in
ticks, entry and exit time, and the row count. The Signal column is not
compared. It prints `MATCH <path>` or `DIFF <path> <first difference>` per tape
and a summary line. Tapes in the test's known-divergence table must still
differ; every other asserted tape and order control must match. A recorded
tape prints `GAP-MATCH` or `GAP-DIFF` and is replayed with the
`pooc_fee_sizing` switch off and on (`GAP-TOGGLE-MOVES` when its rows move);
nothing about it is checked.
