# Percent-of-equity sizing under a cash commission (R5 lane PAR-CASHFEE)

TradingView's own trades for one question: what quantity does a
`default_qty_type = strategy.percent_of_equity` order get when the strategy
pays a cash commission (`strategy.commission.cash_per_order` or
`cash_per_contract`)? Every script opens from flat on a UTC day of its own,
pyramids, closes part, reverses, or places a priced entry or a
`strategy.order`, and ends the day with `strategy.close_all`. Their headers
name each scenario.

TradingView takes the percentage of `strategy.equity` -- every open entry's
fee charged, the cash fees included -- and leaves out of that money the fee the
order itself pays, then floors onto the quantity grid:

- cash per order: `floor((pct * E - fee) / (price * pointvalue))`;
- cash per contract: `floor(pct * E / (price * pointvalue + fee))`;

`price` being the signal close for a market `strategy.entry` and for
`strategy.order`, and the level for a limit or stop entry. A reversal sizes its
new position the same way from the equity that still carries the reversed
position, and its one order pays one fee. Reading `E` off each tape (initial
capital, net profit closed before the signal, open pieces marked at the signal
close less their pro-rata entry fee), the rule fits all 60 sized entries of the
seven per-order tapes and all 54 of the six per-contract tapes.

`tests/test_cash_fee_sizing_tapes.cpp` replays every script through the Pine
adapter on `bars.inc` and compares every trade with its tape: the eleven tapes
without an entry-bar margin call trade for trade (111 trades), the two
`*-p100-m100` tapes up to the margin call TradingView books on the second
scenario's entry bar and the adapter does not (a recorded divergence; the
adapter's rows are pinned). `docs/design/native-feature-parity.md` §3.10 has
the measurement.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`, `--no-note`), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.
`hthin-x15b-fifo-fee-basis` is lane H-THIN's tape (its X15 b measurement: a
FIFO `strategy.exit(from_entry)` closes an older lot of another id before the
next default entry sizes), copied from that lane's scratch.

| tape | chart | window | trades | tv_trades.csv sha256 |
|---|---|---|---|---|
| `pcf-order-p50-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 15 | `5b858a622a4d8afdb8e3297fbe74e83fdeb6ba2701be8fd8a4bc13c8dac55849` |
| `pcf-order-p100-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 15 | `93cd6031e84fb1b303e9d22cc8a8cc3f5af7dfe48b640c9b31cc819656519602` |
| `pcf-order-p200-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 6 | `8e105f593fe1da7900b19eaba74a28b9ce61c20d0178f18d88e09bb673aa9b85` |
| `pcf-order-priced-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 7 | `3210a82cc426fb6cd5d7a1a09027a7aeab8eccc2b512cd88b893c42efee6da57` |
| `pcf-order-p100-m100` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 11 | `5fe7fb71f114a7ecc08a295cd373d4cfd93d1f6f8be9545875d32e876620bab4` |
| `pcf-es-order-p50-m25` | CME_MINI:ES1! 15 | 2025-06-01 .. 2025-07-01 | 7 | `05882d7a78e3a8ca6a3e8a49249ff66169b30cbe23982c547f977e2fae387f44` |
| `pcf-contract-p50-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 15 | `4c357f02aa06ceb6577a8ba6ba601f73dddb771536401989765cd5a208256852` |
| `pcf-contract-p100-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 15 | `e713dda381f35e43a88506821004aa82d2afccd2e39a69dd2746100083accba3` |
| `pcf-contract-p200-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 6 | `fcf3e66efb18a028b94333fadcb89290888216c56b507b403b12423f7c2f561e` |
| `pcf-contract-priced-m25` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 7 | `c9b9c711279b8e20a3abfa34b6a9ef968e8c8c6ff776eb03db3e30b99f91d9ec` |
| `pcf-contract-p100-m100` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 9 | `38cdc2cee4d123b0f540e2d4514249bae4a5f46be2247d45714652ba8dfe2077` |
| `pcf-es-contract-p50-m25` | CME_MINI:ES1! 15 | 2025-06-01 .. 2025-07-01 | 7 | `953754c5e1e3ed239f9375b3834f3d20c89026b53d69edb9cbc911ff13a31a5e` |
| `hthin-x15b-fifo-fee-basis` | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2025-05-01 | 11 | `f170f906d10112fbb6524fc51747c3b7e64e063651114449c7b8364b4ce9a075` |

`bars.inc`: the windows of the two feeds the scripts trade on -- the corpus
feed's BINANCE:ETHUSDT.P 15m bars (`scripts/derive_corpus_feeds.py` at corpus
gitlink f989e36) from each scenario day's 10:00 UTC signal bar through its
closing fill, and the registry feed `lab bars CME_MINI:ES1! 15` (evidence
sha256 in the file) for the ES days. An account flat between windows books
nothing on the bars left out: on the ETH tapes the adapter's trades over the
whole of 2025-04 and over the windows are the same, row for row.

TradingView refuses a per-call `qty_type` on `strategy.entry` (pine-facade:
`The "{signature}" function does not have an argument with the name "{name}"`,
at the `qty_type` argument), so the typed quantity the adapter also accepts has
no tape here.
