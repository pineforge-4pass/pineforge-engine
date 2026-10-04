# margin_ledger_rules

TradingView strategy tapes of synthetic event-ledger probes that pin margin,
sizing and admission rules: 381 tapes of 165 probes, 43088 paired trade rows.
Each probe's script issues a fixed ledger of `strategy.entry`,
`strategy.close` and `strategy.close_all` calls, each on the bar opening at a
fixed time: explicit quantities or the default 100 % of equity
(`default_qty_type = strategy.percent_of_equity`, `default_qty_value = 100`),
no commission, no slippage, margin 100 (or 0 for five probes), on NYSE:F
(15 minutes and 1 day), NASDAQ:AAPL and BINANCE:ETHUSDT.P (15 minutes). The
last entry before the final `strategy.close_all()` is the boundary order
`Turn` — a reversal against the held `Seed` (or ledger) position, or an entry
from flat; an `after_close` probe also calls `strategy.close("Seed")` right
after it. Ledgers run from no prior trade to 1794 closed trades (the longest
scripts read their ledger from arrays). Some probes were exported up to four
times (`r1` … `r4`). The tapes were exported with `lab tv --no-note` over the
ws-report-v1 channel.

## Layout

- `<name>/` — one probe: `strategy.pine` (its source), `plan.json` (its
  settings and event ledger) and one `<rK>/tv_trades.csv` per export
  (TradingView's trade list, byte-identical to the export).
- `bars/` — OHLCV windows (`timestamp,open,high,low,close,volume`, timestamp
  in UTC milliseconds), one file per distinct (feed, from, to): every bar from
  `from` 00:00 UTC through `to` 00:00 UTC inclusive, rows copied verbatim from
  the feed. Feeds: `f-roi` (NYSE:F, 1 day), `f-antoni` (NYSE:F, 15 minutes),
  `aapl` (NASDAQ:AAPL, 15 minutes), `eth-proozac` (BINANCE:ETHUSDT.P, 15
  minutes).
- `provenance.json` — per probe: its origin plan file and Pine source and the
  sha256 of `strategy.pine`; per tape: fixture path, original export path,
  sha256 and row count of `tv_trades.csv`, export time, channel and chart;
  the sha256, row count and origin of every `bars/` file.
- `cases.tsv` — the generated manifest the test reads, one row per tape.
- `build_cases.py` — regenerates `cases.tsv`.

## cases.tsv

`python3 build_cases.py` verifies every `tv_trades.csv`, `strategy.pine` and
`bars/` file against `provenance.json`, checks that every event time is a bar
of the probe's window, and rewrites `cases.tsv` from the per-probe
`plan.json`; `python3 build_cases.py --check` fails when the committed
manifest is stale. Numbers are written with Python `repr()` so the test's
`strtod` reads the identical double. Columns (tab-separated):

| column | meaning |
|---|---|
| `name` | probe directory |
| `tape` | `<name>/<rK>/tv_trades.csv` |
| `bars` | file under `bars/` |
| `symbol`, `timeframe` | chart symbol; `15` or `1D` |
| `tick`, `step` | price tick and quantity step |
| `capital` | initial capital |
| `margin_long`, `margin_short` | margin percent |
| `after_close` | 1 when the script calls `strategy.close("Seed")` right after the boundary entry |
| `events` | the script's calls in statement order, `time:op:id:side:qty:boundary` joined by `;` — `op` is `entry`, `close` or `close_all`; `side` 1 long, -1 short, 0 for a close; `qty` empty for the default quantity; `boundary` 0/1 |

## What the test asserts

`tests/test_margin_ledger_rules_tapes.cpp` replays every row through the Pine
adapter on every bar of its `bars/` file — a handwritten host issues the
row's calls on the bar opening at their time, in order — with every margin
rule switch on, and compares TradingView's rows — each Exit row paired with
the Entry row of its trade number, in trade-number order, including a
position still open at the range end (reported as a trade closed at the last
bar's close) — with the engine's report rows at the same index: side,
quantity in lots, entry and exit price in ticks, entry and exit time, and the
row count. PnL and the Signal column are not compared. It prints
`MATCH <name>/<rK>` or `DIFF <name>/<rK> <first difference>` per tape and a
summary line. Tapes in the test's known-divergence table must still differ;
every other tape must match.
