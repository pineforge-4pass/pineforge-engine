# Session-flag tapes: several windows, overnight sessions, extended hours

TradingView's own `session.*` flags on every bar of seventeen charts (R5 lane
K-SESSION-WINDOWS). Each directory is one TradingView export (channel
`ws-report-v1`, `rangeProof` covered), byte for byte: `strategy.pine`,
`tv_trades.csv` (times at UTC+8, the exporters' rendering), `metrics.json`,
and for a `lab tv` export `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. The probes are synthetic and public; no scraped or
closed strategy is involved.

Each probe reverses its position at the close of every chart bar
(`process_orders_on_close=true`), so every entry is one chart bar, dated at
its open, and its Signal is the flags TradingView evaluated there, a letter
and a digit each: `M` session.ismarket, `P` session.ispremarket, `Q`
session.ispostmarket, `F` session.isfirstbar, `L` session.islastbar, `f`
session.isfirstbar_regular, `l` session.islastbar_regular. The history probe
(`cgs2-hist-*`) spells the same seven as `C` and seven bits in that order,
followed by history reads the tests here do not use.
`tests/session_flag_tape_fixture.hpp` reads both spellings.

## Where each tape comes from

| tape | chart | window | bars | tv_trades.csv sha256 | exported (UTC) | exporter | origin |
|---|---|---|---|---|---|---|---|
| `cgs2-flags-hkex700-15` | HKEX:700 15 | 2025-03-03 .. 03-15 | 220 | `45dd27e0365a9d4e942a478b457e3eb741fe912b2dba819bae5d8ea2c5d9dd36` | 2026-09-26 09:28:07 | `lab tv` | codegen `cg/session2` b717165 |
| `cgs2-flags-tse7203-15` | TSE:7203 15 | 2025-03-03 .. 03-15 | 230 | `d882fbe5d556d3c3fe5d2884adde2e28113a57c0b90d03072bf31185dca2ab9e` | 2026-09-26 09:27:40 | `lab tv` | codegen `cg/session2` b717165 |
| `cgs2-flags-zc1-15` | CBOT:ZC1! 15 | 2025-03-03 .. 03-15 | 710 | `7a69b0f608ad98aa0055cf16fa201505792dfa1e7dbd2288cffc019a9f528e49` | 2026-09-26 09:28:16 | `lab tv` | codegen `cg/session2` b717165 |
| `ksw-flags-hkex700-60` | HKEX:700 60 | 2025-03-03 .. 03-15 | 60 | `2ec27bec172e1955cb9b3859c3b0c7831d30a8c64718e9c8aa1c3eb6bb0074a2` | 2026-09-27 14:56:15 | `lab tv` | this lane |
| `ksw-flags-hkex700-240` | HKEX:700 240 | 2025-03-03 .. 03-15 | 20 | `749048b62deb91a92aa670f1ad4ad9a33c92b8d86ea0ef9b6b6a4f68be6bdb8a` | 2026-09-27 14:56:25 | `lab tv` | this lane |
| `ksw-flags-tse7203-60` | TSE:7203 60 | 2025-03-03 .. 03-15 | 70 | `8ab368d71a6e8a87e27c6b2c0f312a2e911261de73100761a1ba4e27ce6ec657` | 2026-09-27 14:56:18 | `lab tv` | this lane |
| `ksw-flags-tse7203-240` | TSE:7203 240 | 2025-03-03 .. 03-15 | 20 | `30134cad170cac95eb251511479b34a69bf9733bb26d694805e6c1df9b5eab56` | 2026-09-27 14:56:29 | `lab tv` | this lane |
| `ksw-flags-zc1-60` | CBOT:ZC1! 60 | 2025-03-03 .. 03-15 | 190 | `bb1094fb0ae4d4bb0916387117c1b2f774fc23e1157240b1eccf57809c42fd55` | 2026-09-27 14:56:22 | `lab tv` | this lane |
| `ksw-flags-zc1-240` | CBOT:ZC1! 240 | 2025-03-03 .. 03-15 | 50 | `64b998d56cf824a6f1fcbbc04c5ced77a3fd0c85661956c413457cb5f0310231` | 2026-09-27 14:56:33 | `lab tv` | this lane |
| `cgim-flags-es1-60-dst-mar` | CME_MINI:ES1! 60 | 2025-03-02 .. 03-14 | 210 | `be5bf635932cf5f460509b2f491ba283e904f4e0317a2f3c3c0d9f1c523f4fa1` | 2026-09-25 21:55:48 | `lab tv` | codegen main a425965 |
| `cgim-flags-xauusd-60-dst-mar` | OANDA:XAUUSD 60 | 2025-03-02 .. 03-14 | 210 | `308b250a01e1c2e77a74019d61f89e822305a9e76974bcd2b2e1b939a4cadb7e` | 2026-09-25 21:56:01 | `lab tv` | codegen main a425965 |
| `cgim-flags-eurusd-60-dst-mar` | OANDA:EURUSD 60 | 2025-03-02 .. 03-14 | 220 | `b40fbc81f290140da74127637c705c1d35872c92d10a7aa95e7228e8a791d25b` | 2026-09-25 21:55:58 | `lab tv` | codegen main a425965 |
| `cgim-flags-eth-60-24x7` | BINANCE:ETHUSDT.P 60 | 2025-03-07 .. 03-11 | 97 | `0a077e8b7af95d0a9279b0182d7024c140240ecd47d0d648a82e81cbbc61a5fe` | 2026-09-25 21:56:04 | `lab tv` | codegen main a425965 |
| `cgim-flags-aapl-60-ext` | NASDAQ:AAPL 60, extended hours | 2025-03-03 .. 03-15 | 160 | `c61119c7077dcf01cf89b13c77931a52bff4c1078e63c0e58c3e687f93db0802` | 2026-09-25 23:04:04 | exporter copy | codegen main a425965 |
| `cgim-flags-aapl-60-reg` | NASDAQ:AAPL 60 | 2025-03-03 .. 03-15 | 70 | `163434a171c27026622b65d0777118120f7660a2a97b2e320091aaf1c9bc6a57` | 2026-09-25 23:04:06 | exporter copy | codegen main a425965 |
| `cgs2-hist-aapl-15-ext` | NASDAQ:AAPL 15, extended hours | 2025-03-03 .. 03-15 | 639 | `a6dcfe7eacbc5704c91eac765ab93e7115ceb49d16e5440e8a42e70a2d9952da` | 2026-09-26 09:26:24 | exporter copy | codegen `cg/session2` cecebbd |
| `cgs2-hist-aapl-15-reg` | NASDAQ:AAPL 15 | 2025-03-03 .. 03-15 | 260 | `d4f5ed467ea7ac82fc90e4c0e6cc125c70c47b11762e342d11f946e89cffc737` | 2026-09-26 09:25:49 | `lab tv` | codegen `cg/session2` cecebbd |

- **`lab tv`** tapes ran `lab tv --pine <probe> --slug <tape> --symbol <chart>
  --interval <tf> --from <from> --to <to> --out <dir> --no-note --json`: no
  campaign note was recorded. The `cgim-flags-*`, `cgs2-flags-*` and
  `ksw-flags-*` probes are one source (the every-flag probe; this lane's copy
  differs only in its title and header comment, `strategy.pine` sha256
  `1a78deccefb408d955112da398240e8fc5b146d126c40e514ee24078e4af83d4`).
- **exporter copy**: `lab tv` and the WebSocket exporter
  `pinescript-scrapper/scripts/tv-ws-backtest.mjs` ask TradingView for the
  regular session. The extended-hours tapes, and the two regular AAPL 60
  tapes exported beside them, ran lane CG-ISMARKET's copy of the exporter
  that differs in one line, `session: process.env.CGIM_TV_SESSION ||
  "regular"` (sha256
  `15d8afae63844c35481e94ba14000720a9e8f0f500d28c51bc2f8cb8189f4f06`), with
  `CGIM_TV_SESSION=extended`; they carry no `meta.json`.
- **origin**: the directory each tape was copied from unchanged:
  `pineforge-codegen` `tests/fixtures/session_ismarket/` at the commit named.

## What TradingView shows

- **Pre- and post-market.** Every bar of the TSE:7203, HKEX:700 and CBOT:ZC1!
  charts, at 15, 60 and 240 minutes, is neither pre- nor post-market, and so
  is every bar of the overnight and 24-hour charts (ES1!, XAUUSD, EURUSD,
  ETHUSDT.P). On NASDAQ:AAPL with extended hours the bars opening from 04:00
  to before 09:30 ET are pre-market and from 16:00 to before 20:00
  post-market; without extended hours none is.
- **In market.** TSE:7203's 60-minute bar that opens at 12:00, inside the
  11:30-12:30 lunch break (its first trade is at 12:30), and CBOT:ZC1!'s that
  opens at 08:00, inside the 07:45-08:30 break, are in market, like every
  other bar of those charts. On AAPL with extended hours the 09:00 bar, which
  holds the 09:30 open, is pre-market and the 16:00 bar post-market.
- **First and last bars.** On AAPL with extended hours `isfirstbar` /
  `islastbar` are the extended day's first and last bars (04:00 and 19:00 at
  60 minutes, 04:00 and 19:45 at 15) and `isfirstbar_regular` /
  `islastbar_regular` the first and last bars that open inside regular hours
  (10:00 and 15:00 at 60 minutes, 09:30 and 15:45 at 15). Without extended
  hours each pair is the same bar.
- **TSE:7203's close.** TradingView holds the bar opening at 15:30 in session
  on the 15-minute chart (the closing auction), which the published 15:30 end
  leaves out; a 15:45 end keeps it.
- **HKEX:700 above 30 minutes.** TradingView's 60-minute bars open at 09:30,
  10:00, 11:00, 13:00, 14:00 and 15:00 (a half-hour first bar, then the clock
  hour), and it flags both the 09:30 and the 10:00 bar `isfirstbar`. Its
  240-minute bars open at 09:30 and 13:00, and both are `isfirstbar` and
  `islastbar`. TSE:7203 and CBOT:ZC1! keep one first and one last bar a day
  at every timeframe here.
