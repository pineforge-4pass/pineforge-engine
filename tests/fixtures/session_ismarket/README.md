# session.ismarket tapes (R5 wave H, lane H-MEASURE, row G2-36)

TradingView's own `session.ismarket` on every chart bar. The probe
(`strategy.pine`, the same in every directory) reverses its position at the
close of every bar (`process_orders_on_close=true`), so every entry is one
chart bar, dated at its open, and its Signal is the flag TradingView
evaluated there: `M1` in market, `M0` not. Each directory is one `lab tv`
export, byte-identical (`strategy.pine`, `tv_trades.csv` with times UTC+8,
`meta.json`, `metrics.json`); `metrics.json` `tvTradesCsvHash` is the sha256.

| tape | chart | window | bars (entries) | flags | tv_trades.csv sha256 | campaign note |
|---|---|---|---|---|---|---|
| `hm-g236-es1-60-dst-mar` | CME_MINI:ES1! 60 | 2025-03-02 .. 03-14 (US DST 03-09) | 210 | all `M1` | `c1a6b73b032a51b6e8007d6fdbe4b59dc6aaa09ff7eafdceab7914ff46c9443d` | `tv-tape-hm-g236-es1-60-dst-mar-c1a6b73b` |
| `hm-g236-es1-60-dst-nov` | CME_MINI:ES1! 60 | 2025-10-26 .. 11-07 (US DST 11-02) | 209 | all `M1` | `a7fbd62928dc49caf7bcdc6c9d8569139eab036860aa375aa0fc2c7db9b408b9` | `tv-tape-hm-g236-es1-60-dst-nov-a7fbd629` |
| `hm-g236-es1-60-thanksgiving` | CME_MINI:ES1! 60 | 2025-11-23 .. 12-05 | 192 | all `M1` | `52512b7df99b764b8f13f8101b7e37481bdad5dfbf86620868f11fe175efaf8f` | `tv-tape-hm-g236-es1-60-thanksgiving-52512b7d` |
| `hm-g236-eurusd-60-dst-mar` | OANDA:EURUSD 60 | 2025-03-02 .. 03-14 | 220 | all `M1` | `60713d2c22442ffe14070fddb1c5cd7fbf6fc6054c0b532b4c4275de305decf1` | `tv-tape-hm-g236-eurusd-60-dst-mar-60713d2c` |
| `hm-g236-xauusd-60-dst-mar` | OANDA:XAUUSD 60 | 2025-03-02 .. 03-14 | 210 | all `M1` | `ebc226aea010125fb66b7cda3a86563846a243463723816ee529f8f044c54cf6` | `tv-tape-hm-g236-xauusd-60-dst-mar-ebc226ae` |
| `hm-g236-eth-60-24x7` | BINANCE:ETHUSDT.P 60 | 2025-03-07 .. 03-11 | 97 | all `M1` | `96b7e8890b41a9e8aa60698a43cdffd5f42c55bb8d2d0223f7985c13b5040b74` | `tv-tape-hm-g236-eth-60-24x7-96b7e889` |

Every tape is rangeProof `covered`. The flag is a function of the bar's time
and the symbol's session and timezone only, so
`tests/test_session_ismarket_tape.cpp` replays each tape on flat bars stamped
at the tape's own entry times. The session strings it replays are the
campaign's lane facts (pineforge-lab `config/symbol-lanes-v1.json`: ES1!
`1700-1600` America/Chicago, EURUSD `1700-1700` and XAUUSD `1800-1700`
America/New_York, crypto `24x7`), the same sessions with TradingView's
weekday mask `:23456`, and a 24-hour day spelled `0000-2400` and `0000-0000`.
