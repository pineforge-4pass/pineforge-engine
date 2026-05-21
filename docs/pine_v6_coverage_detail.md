# Pine v6 Coverage — Identifier-by-Identifier Audit

> ⚠️ **STALE — DO NOT TRUST INDIVIDUAL ROWS WITHOUT CROSS-CHECK.**
> Exhaustive 2026-05-21 audit ([`pine_v6_audit_master.md`](pine_v6_audit_master.md)) found **38 critical issues** (wrong bucket / wrong backing / silent miscompile) + **~62 minor issues** across the rows below. Headline totals are also affected — bucket counts have not been re-derived post-audit.
>
> This file will be regenerated after the fix sprint. Until then, treat it as a snapshot of *what the doc claimed*, not *what the engine actually does*. The master audit contains consolidated critical-issue tables + minor-issue themes; chunk-level cite-by-line-number reports were merged into the master and removed.

| Field | Value |
|---|---|
| **Generated** | 2026-05-17 (post Pine v6 HIGH+MEDIUM sprint) |
| **Audit status** | ⚠️ Superseded by `pine_v6_audit_master.md` (2026-05-21) |
| **Pine v6 reference** | https://www.tradingview.com/pine-script-reference/v6/ (JS-rendered, scraped 2026-05-16) |
| **PineForge engine version** | 0.4.1 + sprint |
| **Total Pine v6 identifiers** | 941 |

## Headline totals

| Bucket | Count | % of 941 |
|---|---|---|
| ✅ Runtime | 199 | 21% |
| 🔧 Transpiler | 219 | 23% |
| ⏭️ Parse-and-skip | 220 | 23% |
| ❌ Unsupported | 142 | 15% |
| ❓ Unknown / not classified | 161 | 17% |

> **"Fully runs" headline:** PineForge executes **418 of 941** Pine v6 identifiers (✅ Runtime + 🔧 Transpiler = **44%**, up from 40% pre-sprint). An additional 23% parse-and-skip silently (no error, no effect — drawing/plotting + syminfo na-accepts). 15% are rejected at transpile or produce na-returns (fundamentals + cross-symbol + ticker.* chart-type modifiers + library system). 17% remain not-yet-audited at single-identifier precision (down from 23% — ~60 resolved this sprint).

> **Sprint delta (2026-05-17):** +17 ✅ Runtime, +21 🔧 Transpiler, +12 ⏭️ Parse-and-skip, +9 ❌ Unsupported (ticker.* split), −59 ❓ Unknown. See [Sprint changes](#sprint-changes-2026-05-17) section below.

---

## Methodology

**Step 1 — Pine v6 catalog:** Playwright navigated to `https://www.tradingview.com/pine-script-reference/v6/` (JS SPA). The full DOM was evaluated via `document.querySelectorAll('h3')` which returned 941 entries — every identifier in the reference, ordered as the site renders them (Types → Variables → Constants → Functions → Keywords → Operators → Annotations).

**Step 2 — PineForge runtime inventory:** Read all public headers under `include/pineforge/*.hpp`, `src/engine_internal.hpp`, and `docs/coverage.md`. Extracted every exported class, free function, and inline.

**Step 3 — Cross-tabulation:** Each identifier was classified into one of five buckets:
- **✅ Runtime** — has a dedicated C++ class or function in `libpineforge.a`
- **🔧 Transpiler** — no runtime class, but PineForge's transpiler emits inline C++ (`std::vector`, `<cmath>`, `std::string`, generated structs)
- **⏭️ Parse-and-skip** — transpiler parses without error, emits no code / no-op (drawing, plotting, alert APIs)
- **❌ Unsupported** — rejected at transpile (loud error) or runtime no-op returning `na`
- **❓ Unknown** — not yet definitively classified (footprint, volume_row, some syminfo fields, ticker.* chart-type constructors, etc.)

---

## Per-namespace tables

### Types (Pine type keywords)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `array` | type | 🔧 Transpiler | `std::vector<T>` emitted by transpiler | No runtime array module |
| `bool` | type | ✅ Runtime | C++ `bool` via `na.hpp` | |
| `box` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `chart.point` | type | ⏭️ Parse-and-skip | Chart geometry; no runtime | |
| `color` | type | ✅ Runtime | `color.hpp` — 17 named ARGB constants + helpers | |
| `const` | type/qualifier | 🔧 Transpiler | C++ `const` | |
| `float` | type | ✅ Runtime | C++ `double` via `na.hpp` | |
| `footprint` | type | ❓ Unknown | Footprint chart type; no runtime module | |
| `int` | type | ✅ Runtime | C++ `int` via `na.hpp` | |
| `label` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `line` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `linefill` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `map` | type | 🔧 Transpiler | `std::unordered_map<K,V>` emitted by transpiler | |
| `matrix` | type | ✅ Runtime | `matrix.hpp` / `generic_matrix.hpp` | |
| `polyline` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `series` | type/qualifier | ✅ Runtime | `series.hpp` — `Series<T>` ring buffer | |
| `simple` | type/qualifier | 🔧 Transpiler | Compile-time qualifier; transpiler handles | |
| `string` | type | 🔧 Transpiler | C++ `std::string` | |
| `table` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `volume_row` | type | ❓ Unknown | Footprint chart type; no runtime module | |

### Variables — bar data

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ask` | var | ❌ Unsupported | No live feed; always na | Realtime-only |
| `bar_index` | var | ✅ Runtime | `bar_index_` on `BacktestEngine` | |
| `bid` | var | ❌ Unsupported | No live feed; always na | Realtime-only |
| `close` | var | ✅ Runtime | `current_bar_.close` on `BacktestEngine` | |
| `dayofmonth` | var | ✅ Runtime | `_bar_dayofmonth()` on `BacktestEngine` | |
| `dayofweek` | var | ✅ Runtime | `_bar_dayofweek()` on `BacktestEngine` | |
| `high` | var | ✅ Runtime | `current_bar_.high` on `BacktestEngine` | |
| `hl2` | var | 🔧 Transpiler | `(high+low)/2` emitted inline | |
| `hlc3` | var | 🔧 Transpiler | `(high+low+close)/3` emitted inline | |
| `hlcc4` | var | 🔧 Transpiler | `(high+low+close+close)/4` emitted inline | |
| `hour` | var | ✅ Runtime | `_bar_hour()` on `BacktestEngine` | |
| `last_bar_index` | var | ✅ Runtime | Computed from bar count in run loop | |
| `last_bar_time` | var | ✅ Runtime | Timestamp of last bar | |
| `low` | var | ✅ Runtime | `current_bar_.low` on `BacktestEngine` | |
| `minute` | var | ✅ Runtime | `_bar_minute()` on `BacktestEngine` | |
| `month` | var | ✅ Runtime | `_bar_month()` on `BacktestEngine` | |
| `na` | var/fn | ✅ Runtime | `na.hpp` — `na<T>()` generic, `is_na(...)` | |
| `ohlc4` | var | 🔧 Transpiler | `(open+high+low+close)/4` inline | |
| `open` | var | ✅ Runtime | `current_bar_.open` on `BacktestEngine` | |
| `second` | var | ✅ Runtime | `_bar_second()` on `BacktestEngine` | |
| `time` | var | ✅ Runtime | `current_bar_.timestamp` | |
| `time_close` | var | ✅ Runtime | `pine_time_close(...)` in `session_time.hpp` | |
| `time_tradingday` | var | ✅ Runtime | `pine_time_tradingday(bar_ms, session, tz)` in `session_time.hpp` | Sprint G1; derives session-day open in `syminfo_.timezone`; DST-edge fallback for Havana/Lord_Howe |
| `timenow` | var | ❌ Unsupported | No live clock; always na in batch mode | |
| `volume` | var | ✅ Runtime | `current_bar_.volume` on `BacktestEngine` | |
| `weekofyear` | var | ✅ Runtime | `_bar_weekofyear()` on `BacktestEngine` | |
| `year` | var | ✅ Runtime | `_bar_year()` on `BacktestEngine` | |

### Variables — barstate

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `barstate.isconfirmed` | var | ✅ Runtime | `is_last_tick_` (batch approx.) | |
| `barstate.isfirst` | var | ✅ Runtime | `bar_index == 0` (compiler-handled) | |
| `barstate.ishistory` | var | ✅ Runtime | always `true` in batch | |
| `barstate.islast` | var | ✅ Runtime | `barstate_islast_` | |
| `barstate.islastconfirmedhistory` | var | ✅ Runtime | always `false` in batch | Semantically inaccurate but non-crashing |
| `barstate.isnew` | var | ✅ Runtime | `is_first_tick_` | |
| `barstate.isrealtime` | var | ✅ Runtime | always `false` in batch | |

### Variables — chart

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `chart.bg_color` | var | ⏭️ Parse-and-skip | Rendering property; no backtesting role | |
| `chart.fg_color` | var | ⏭️ Parse-and-skip | Rendering property | |
| `chart.is_heikinashi` | var | 🔧 Transpiler | Constant `false` emitted by `visit_expr` | Sprint E |
| `chart.is_kagi` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_linebreak` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_pnf` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_range` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_renko` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_standard` | var | 🔧 Transpiler | Constant `true` (engine always batches standard OHLCV) | Sprint E |
| `chart.left_visible_bar_time` | var | ❌ Unsupported | Viewport/UI concept; no batch equivalent | |
| `chart.right_visible_bar_time` | var | ❌ Unsupported | Viewport/UI concept; no batch equivalent | |

### Variables — dividends / earnings / splits

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dividends.future_amount` | var | ❌ Unsupported | External fundamental data not ingested | |
| `dividends.future_ex_date` | var | ❌ Unsupported | External fundamental data not ingested | |
| `dividends.future_pay_date` | var | ❌ Unsupported | External fundamental data not ingested | |
| `earnings.future_eps` | var | ❌ Unsupported | External fundamental data not ingested | |
| `earnings.future_period_end_time` | var | ❌ Unsupported | External fundamental data not ingested | |
| `earnings.future_revenue` | var | ❌ Unsupported | External fundamental data not ingested | |
| `earnings.future_time` | var | ❌ Unsupported | External fundamental data not ingested | |

### Variables — drawing object collections

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `box.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `label.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `line.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `linefill.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `polyline.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `table.all` | var | ⏭️ Parse-and-skip | Drawing; no runtime | |

### Variables — session

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `session.isfirstbar` | var | ✅ Runtime | `session_isfirstbar_` on engine; per-bar lookahead in `engine_run.cpp` | Sprint A |
| `session.isfirstbar_regular` | var | ✅ Runtime | Aliased to `session.isfirstbar` — engine has single session string, cannot distinguish RTH vs ETH (documented limitation) | Sprint A |
| `session.islastbar` | var | ✅ Runtime | `session_islastbar_` on engine; per-bar lookahead | Sprint A |
| `session.islastbar_regular` | var | ✅ Runtime | Aliased to `session.islastbar` | Sprint A |
| `session.ismarket` | var | ✅ Runtime | `pine_session_ismarket(session, tz, bar_ms)` in `session_time.hpp` | Sprint A |
| `session.ispostmarket` | var | ✅ Runtime | `pine_session_ispostmarket(...)` — standard ETH window `RTH_close-2000` local | Sprint A |
| `session.ispremarket` | var | ✅ Runtime | `pine_session_ispremarket(...)` — standard ETH window `0400-RTH_open` local | Sprint A |

### Variables — strategy

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `strategy.account_currency` | var | ✅ Runtime | `syminfo_.currency` on `BacktestEngine` | |
| `strategy.avg_losing_trade` | var | ✅ Runtime | `avg_losing_trade()` | |
| `strategy.avg_losing_trade_percent` | var | ✅ Runtime | `avg_losing_trade_percent()` | |
| `strategy.avg_trade` | var | ✅ Runtime | `avg_trade()` | |
| `strategy.avg_trade_percent` | var | ✅ Runtime | `avg_trade_percent()` | |
| `strategy.avg_winning_trade` | var | ✅ Runtime | `avg_winning_trade()` | |
| `strategy.avg_winning_trade_percent` | var | ✅ Runtime | `avg_winning_trade_percent()` | |
| `strategy.closedtrades` | var | ✅ Runtime | `trades_.size()` | |
| `strategy.closedtrades.first_index` | var | ✅ Runtime | Computed from closed-trade list | |
| `strategy.equity` | var | ✅ Runtime | `current_equity()` | |
| `strategy.eventrades` | var | ✅ Runtime | `eventrades_count_` incremented in `engine_orders.cpp` when trade.profit == 0 | Sprint F |
| `strategy.grossloss` | var | ✅ Runtime | `gross_loss()` | |
| `strategy.grossloss_percent` | var | ✅ Runtime | `grossloss_percent()` | |
| `strategy.grossprofit` | var | ✅ Runtime | `gross_profit()` | |
| `strategy.grossprofit_percent` | var | ✅ Runtime | `grossprofit_percent()` | |
| `strategy.initial_capital` | var | ✅ Runtime | `initial_capital_` | |
| `strategy.losstrades` | var | ✅ Runtime | `count_losstrades()` | |
| `strategy.margin_liquidation_price` | var | ✅ Runtime | `margin_liquidation_price()` → always na | Returns na per docs |
| `strategy.max_contracts_held_all` | var | ✅ Runtime | `max_contracts_held_all_` per-bar `std::max(|position_qty_|)` in `update_equity_extremes()` | Sprint F |
| `strategy.max_contracts_held_long` | var | ✅ Runtime | `max_contracts_held_long_` (gated on `position_side_ == LONG`) | Sprint F |
| `strategy.max_contracts_held_short` | var | ✅ Runtime | `max_contracts_held_short_` (gated on `position_side_ == SHORT`) | Sprint F |
| `strategy.max_drawdown` | var | ✅ Runtime | `max_drawdown_` | |
| `strategy.max_drawdown_percent` | var | ✅ Runtime | `max_runup_percent()` (drawdown variant) | |
| `strategy.max_runup` | var | ✅ Runtime | `max_runup_` | |
| `strategy.max_runup_percent` | var | ✅ Runtime | `max_runup_percent()` | |
| `strategy.netprofit` | var | ✅ Runtime | `net_profit()` | |
| `strategy.netprofit_percent` | var | ✅ Runtime | `net_profit() / initial_capital_ * 100` | |
| `strategy.openprofit` | var | ✅ Runtime | `open_profit(price)` | |
| `strategy.openprofit_percent` | var | ✅ Runtime | Derived from open_profit | |
| `strategy.opentrades` | var | ✅ Runtime | Position tracking on engine | |
| `strategy.opentrades.capital_held` | var | ✅ Runtime | `open_trades_capital_held()` | |
| `strategy.position_avg_price` | var | ✅ Runtime | `position_entry_price_` | |
| `strategy.position_entry_name` | var | ✅ Runtime | Last entry id in pyramid | |
| `strategy.position_size` | var | ✅ Runtime | `signed_position_size()` | |
| `strategy.wintrades` | var | ✅ Runtime | `count_wintrades()` | |

### Variables — syminfo

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `syminfo.basecurrency` | var | ✅ Runtime | `syminfo_.basecurrency` on `SymInfo` | |
| `syminfo.country` | var | 🔧 Transpiler | Derived from `syminfo_.tickerid` prefix via `_pf_derive_country()` (NASDAQ→US, LSE→UK, TSE→JP, ...30 exchanges) | Sprint G2 (audit rescue from Rule 2 defer) |
| `syminfo.currency` | var | ✅ Runtime | `syminfo_.currency` | |
| `syminfo.current_contract` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.description` | var | ✅ Runtime | `syminfo_.description` | |
| `syminfo.employees` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 LOW na-accept |
| `syminfo.expiration_date` | var | ⏭️ Parse-and-skip | Returns `na<int64_t>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.industry` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.isin` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.main_tickerid` | var | 🔧 Transpiler | Derived from `syminfo_.tickerid` via `_pf_derive_main_tickerid()` (strips futures `N!` suffix) | Sprint G2 (audit rescue) |
| `syminfo.mincontract` | var | ⏭️ Parse-and-skip | Returns `na<double>()`; conditional-use warning emitted | Sprint G2 (audit fix) — was previously silently emitting 0; pineforge-data scope |
| `syminfo.minmove` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct, contrary to prior audit) |
| `syminfo.mintick` | var | ✅ Runtime | `syminfo_.mintick` | |
| `syminfo.pointvalue` | var | ✅ Runtime | `syminfo_.pointvalue` | |
| `syminfo.prefix` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct) |
| `syminfo.pricescale` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct) |
| `syminfo.recommendations_buy` | var | ⏭️ Parse-and-skip | Returns `na<double>()` (was ❌) | Sprint G2 — pineforge-data scope |
| `syminfo.recommendations_buy_strong` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.recommendations_date` | var | ⏭️ Parse-and-skip | Returns `na<int64_t>()` | Sprint G2 |
| `syminfo.recommendations_hold` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.recommendations_sell` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.recommendations_sell_strong` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.recommendations_total` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.root` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct) |
| `syminfo.sector` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning | Sprint G2; pineforge-data scope |
| `syminfo.session` | var | ✅ Runtime | `syminfo_.session` | |
| `syminfo.shareholders` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.shares_outstanding_float` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.shares_outstanding_total` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.target_price_average` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.target_price_date` | var | ⏭️ Parse-and-skip | Returns `na<int64_t>()` | Sprint G2 |
| `syminfo.target_price_estimates` | var | ⏭️ Parse-and-skip | Returns `na<int>()` | Sprint G2 |
| `syminfo.target_price_high` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.target_price_low` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.target_price_median` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 |
| `syminfo.ticker` | var | ✅ Runtime | `syminfo_.ticker` | |
| `syminfo.tickerid` | var | ✅ Runtime | `syminfo_.tickerid` | |
| `syminfo.timezone` | var | ✅ Runtime | `syminfo_.timezone` | |
| `syminfo.type` | var | ✅ Runtime | `syminfo_.type` | |
| `syminfo.volumetype` | var | ✅ Runtime | `syminfo_.volumetype` | |

### Variables — ta.* series

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ta.accdist` | var | ✅ Runtime | `ta::AccDist` class (`ta_extremes_volume.cpp`) | |
| `ta.iii` | var | ✅ Runtime | `ta::III` class (`ta_extremes_volume.cpp`) | |
| `ta.nvi` | var | ✅ Runtime | `ta::NVI` class (`ta_extremes_volume.cpp`) | |
| `ta.obv` | var | ✅ Runtime | `ta::OBV` class (`ta_extremes_volume.cpp`) | |
| `ta.pvi` | var | ✅ Runtime | `ta::PVI` class (`ta_extremes_volume.cpp`) | |
| `ta.pvt` | var | ✅ Runtime | `ta::PVT` class (`ta_extremes_volume.cpp`) | |
| `ta.tr` | var | ✅ Runtime | `ta::TR(handle_na=false)` class (`ta_oscillators.cpp`) | Property form (no args) |
| `ta.vwap` | var | ✅ Runtime | `ta::VWAP` class (`ta_extremes_volume.cpp`) | Single-value daily anchor form |
| `ta.wad` | var | ✅ Runtime | `ta::WAD` class (`ta_extremes_volume.cpp`) | |
| `ta.wvad` | var | ✅ Runtime | `ta::WVAD` class (`ta_extremes_volume.cpp`) | |

### Variables — timeframe

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `timeframe.isdaily` | var | ✅ Runtime | `tf_is_daily(script_tf_)` in `timeframe.hpp` | |
| `timeframe.isdwm` | var | ✅ Runtime | Daily/weekly/monthly predicate derived from helpers | |
| `timeframe.isintraday` | var | ✅ Runtime | `tf_is_intraday(script_tf_)` | |
| `timeframe.isminutes` | var | ✅ Runtime | Intraday and not seconds | |
| `timeframe.ismonthly` | var | ✅ Runtime | `tf_is_monthly(script_tf_)` | |
| `timeframe.isseconds` | var | ✅ Runtime | `tf_is_seconds(script_tf_)` | |
| `timeframe.isticks` | var | 🔧 Transpiler | Constant `false` (engine has no tick TF support) | Sprint E |
| `timeframe.isweekly` | var | ✅ Runtime | `tf_is_weekly(script_tf_)` | |
| `timeframe.main_period` | var | ✅ Runtime | `main_period()` getter on `BacktestEngine` → returns `script_tf_` | Sprint E |
| `timeframe.multiplier` | var | ✅ Runtime | `tf_multiplier(script_tf_)` | |
| `timeframe.period` | var | ✅ Runtime | `script_tf_` string | |

---

### Constants — adjustment

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `adjustment.dividends` | const | 🔧 Transpiler | Integer passthrough (1) to `request.security`; engine ignores | Sprint G2 |
| `adjustment.none` | const | 🔧 Transpiler | Integer passthrough (0) | |
| `adjustment.splits` | const | 🔧 Transpiler | Integer passthrough (2) | Sprint G2 |

### Constants — alert

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `alert.freq_all` | const | ⏭️ Parse-and-skip | Alert frequency; no alert runtime | |
| `alert.freq_once_per_bar` | const | ⏭️ Parse-and-skip | Alert frequency | |
| `alert.freq_once_per_bar_close` | const | ⏭️ Parse-and-skip | Alert frequency | |

### Constants — backadjustment

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `backadjustment.inherit` | const | 🔧 Transpiler | Integer passthrough (2) to `request.security`; engine ignores | Sprint G2 |
| `backadjustment.off` | const | 🔧 Transpiler | Integer passthrough (0) | Sprint G2 |
| `backadjustment.on` | const | 🔧 Transpiler | Integer passthrough (1) | Sprint G2 |

### Constants — barmerge

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `barmerge.gaps_off` | const | ✅ Runtime | `gaps_on=false` in `SecurityEvalState` | |
| `barmerge.gaps_on` | const | ✅ Runtime | `gaps_on=true` in `SecurityEvalState` | |
| `barmerge.lookahead_off` | const | ✅ Runtime | `lookahead_on=false` in `SecurityEvalState` | |
| `barmerge.lookahead_on` | const | ✅ Runtime | `lookahead_on=true` in `SecurityEvalState` | Lower-TF emulation rejects this |

### Constants — color

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `color.aqua` | const | ✅ Runtime | `pine_color::aqua` in `color.hpp` | |
| `color.black` | const | ✅ Runtime | `pine_color::black` | |
| `color.blue` | const | ✅ Runtime | `pine_color::blue` | |
| `color.fuchsia` | const | ✅ Runtime | `pine_color::fuchsia` | |
| `color.gray` | const | ✅ Runtime | `pine_color::gray` | |
| `color.green` | const | ✅ Runtime | `pine_color::green` | |
| `color.lime` | const | ✅ Runtime | `pine_color::lime` | |
| `color.maroon` | const | ✅ Runtime | `pine_color::maroon` | |
| `color.navy` | const | ✅ Runtime | `pine_color::navy` | |
| `color.olive` | const | ✅ Runtime | `pine_color::olive` | |
| `color.orange` | const | ✅ Runtime | `pine_color::orange` | |
| `color.purple` | const | ✅ Runtime | `pine_color::purple` | |
| `color.red` | const | ✅ Runtime | `pine_color::red` | |
| `color.silver` | const | ✅ Runtime | `pine_color::silver` | |
| `color.teal` | const | ✅ Runtime | `pine_color::teal` | |
| `color.white` | const | ✅ Runtime | `pine_color::white` | |
| `color.yellow` | const | ✅ Runtime | `pine_color::yellow` | |

### Constants — currency (56 entries)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `currency.AED` … `currency.ZAR` (56 total) | const | 🔧 Transpiler | Emitted as string constants | No FX conversion done |

### Constants — dayofweek

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dayofweek.friday` | const | 🔧 Transpiler | Integer constant `6` | |
| `dayofweek.monday` | const | 🔧 Transpiler | Integer constant `2` | |
| `dayofweek.saturday` | const | 🔧 Transpiler | Integer constant `7` | |
| `dayofweek.sunday` | const | 🔧 Transpiler | Integer constant `1` | |
| `dayofweek.thursday` | const | 🔧 Transpiler | Integer constant `5` | |
| `dayofweek.tuesday` | const | 🔧 Transpiler | Integer constant `3` | |
| `dayofweek.wednesday` | const | 🔧 Transpiler | Integer constant `4` | |

### Constants — display

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `display.all` | const | ⏭️ Parse-and-skip | Display/rendering const; no backtesting role | |
| `display.data_window` | const | ⏭️ Parse-and-skip | Display/rendering | |
| `display.none` | const | ⏭️ Parse-and-skip | Display/rendering | |
| `display.pane` | const | ⏭️ Parse-and-skip | Display/rendering | |
| `display.pine_screener` | const | ⏭️ Parse-and-skip | Display/rendering | |
| `display.price_scale` | const | ⏭️ Parse-and-skip | Display/rendering | |
| `display.status_line` | const | ⏭️ Parse-and-skip | Display/rendering | |

### Constants — dividends / earnings / splits

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dividends.gross` | const | ❌ Unsupported | Fundamental data const | |
| `dividends.net` | const | ❌ Unsupported | Fundamental data const | |
| `earnings.actual` | const | ❌ Unsupported | Fundamental data const | |
| `earnings.estimate` | const | ❌ Unsupported | Fundamental data const | |
| `earnings.standardized` | const | ❌ Unsupported | Fundamental data const | |
| `splits.denominator` | const | ❌ Unsupported | Corporate action const | |
| `splits.numerator` | const | ❌ Unsupported | Corporate action const | |

### Constants — extend / font / format / hline

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `extend.both` | const | ⏭️ Parse-and-skip | Drawing style; no runtime | |
| `extend.left` | const | ⏭️ Parse-and-skip | Drawing style | |
| `extend.none` | const | ⏭️ Parse-and-skip | Drawing style | |
| `extend.right` | const | ⏭️ Parse-and-skip | Drawing style | |
| `false` | const | 🔧 Transpiler | C++ `false` | |
| `font.family_default` | const | ⏭️ Parse-and-skip | Rendering const | |
| `font.family_monospace` | const | ⏭️ Parse-and-skip | Rendering const | |
| `format.inherit` | const | 🔧 Transpiler | String constant for display | |
| `format.mintick` | const | 🔧 Transpiler | Used by `str.tostring` / `pine_str_tostring` | ✅ runtime for mintick mode |
| `format.percent` | const | 🔧 Transpiler | Used by `str.tostring` | ✅ runtime for percent mode |
| `format.price` | const | 🔧 Transpiler | Display hint | |
| `format.volume` | const | 🔧 Transpiler | Used by `str.tostring` | ✅ runtime for volume mode |
| `hline.style_dashed` | const | ⏭️ Parse-and-skip | Rendering const | |
| `hline.style_dotted` | const | ⏭️ Parse-and-skip | Rendering const | |
| `hline.style_solid` | const | ⏭️ Parse-and-skip | Rendering const | |

### Constants — label.style_* (22 entries)

All 22 `label.style_*` constants are **⏭️ Parse-and-skip** — drawing style constants; no runtime backing.

### Constants — line.style_* (6 entries)

All 6 `line.style_*` constants are **⏭️ Parse-and-skip** — drawing style constants; no runtime backing.

### Constants — location / math / order / plot / position / scale / session / shape / size / strategy

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `location.*` (5 entries) | const | ⏭️ Parse-and-skip | Plotshape placement; no runtime | |
| `math.e` | const | 🔧 Transpiler | `M_E` from `<cmath>` | |
| `math.phi` | const | 🔧 Transpiler | `1.618033988...` inline | |
| `math.pi` | const | 🔧 Transpiler | `M_PI` from `<cmath>` | |
| `math.rphi` | const | 🔧 Transpiler | `0.618033988...` inline | |
| `order.ascending` | const | ✅ Runtime | Used in `PineMatrix::sort` and `array.sort` | |
| `order.descending` | const | ✅ Runtime | Used in `PineMatrix::sort` and `array.sort` | |
| `plot.linestyle_*` (3) | const | ⏭️ Parse-and-skip | Plotting style | |
| `plot.style_*` (12) | const | ⏭️ Parse-and-skip | Plotting style | |
| `position.*` (9) | const | ⏭️ Parse-and-skip | Table/label position; rendering only | |
| `scale.*` (3) | const | ⏭️ Parse-and-skip | Rendering | |
| `session.extended` | const | 🔧 Transpiler | Session string constant | |
| `session.regular` | const | 🔧 Transpiler | Session string constant | |
| `settlement_as_close.on/off/inherit` | const | 🔧 Transpiler | Integer passthrough (1/0/2) to `request.security`; engine ignores | Sprint G2 |
| `shape.*` (12) | const | ⏭️ Parse-and-skip | Plotshape style | |
| `size.*` (6) | const | ⏭️ Parse-and-skip | Label/table size; rendering | |
| `strategy.cash` | const | ✅ Runtime | `QtyType::CASH` enum | |
| `strategy.commission.cash_per_contract` | const | ✅ Runtime | `CommissionType::CASH_PER_CONTRACT` | |
| `strategy.commission.cash_per_order` | const | ✅ Runtime | `CommissionType::CASH_PER_ORDER` | |
| `strategy.commission.percent` | const | ✅ Runtime | `CommissionType::PERCENT` | |
| `strategy.direction.all` | const | ✅ Runtime | `RiskDirection::BOTH` | |
| `strategy.direction.long` | const | ✅ Runtime | `RiskDirection::LONG_ONLY` | |
| `strategy.direction.short` | const | ✅ Runtime | `RiskDirection::SHORT_ONLY` | |
| `strategy.fixed` | const | ✅ Runtime | `QtyType::FIXED` | |
| `strategy.long` | const | ✅ Runtime | `is_long=true` in order calls | |
| `strategy.oca.cancel` | const | ✅ Runtime | `oca_type=1` | |
| `strategy.oca.none` | const | ✅ Runtime | `oca_type=0` | |
| `strategy.oca.reduce` | const | ✅ Runtime | `oca_type=2` | |
| `strategy.percent_of_equity` | const | ✅ Runtime | `QtyType::PERCENT_OF_EQUITY` | |
| `strategy.short` | const | ✅ Runtime | `is_long=false` in order calls | |
| `text.*` (8) | const | ⏭️ Parse-and-skip | Text rendering; label/table only | |
| `true` | const | 🔧 Transpiler | C++ `true` | |
| `xloc.*` (2) | const | ⏭️ Parse-and-skip | Drawing xloc mode | |
| `yloc.*` (3) | const | ⏭️ Parse-and-skip | Drawing yloc mode | |

---

### Functions — alert / alertcondition

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `alert()` | fn | ⏭️ Parse-and-skip | Parsed; no live alert emission | |
| `alertcondition()` | fn | ⏭️ Parse-and-skip | Parsed; no live alert emission | |

### Functions — array.* (54 entries)

All `array.*` functions are **🔧 Transpiler** — emitted against `std::vector<T>` by PineForge's transpiler. No runtime module. Selected notes:

| Identifier | Notes |
|---|---|
| `array.new_bool/float/int/string/color/label/line/linefill/box/table()` | 🔧 Transpiler; drawing-type arrays parsed but drawing ops skipped |
| `array.new<type>()` | 🔧 Transpiler generic form |
| `array.sort()` / `array.sort_indices()` | 🔧 Transpiler; uses `std::sort` |
| `array.from()` | 🔧 Transpiler |

### Functions — barcolor / bgcolor

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `barcolor()` | fn | ⏭️ Parse-and-skip | Rendering; no runtime | |
| `bgcolor()` | fn | ⏭️ Parse-and-skip | Rendering; no runtime | |

### Functions — bool / float / int / string (type-cast functions)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `bool()` | fn | 🔧 Transpiler | C++ cast | |
| `float()` | fn | 🔧 Transpiler | C++ `static_cast<double>` | |
| `int()` | fn | 🔧 Transpiler | C++ `static_cast<int>` | |
| `string()` | fn | 🔧 Transpiler | `std::to_string` or no-op | |

### Functions — box.* (27 entries)

All **⏭️ Parse-and-skip** — drawing object methods; no runtime backing.

### Functions — chart.point.* (5 entries)

All **⏭️ Parse-and-skip** — chart geometry; no runtime backing.

### Functions — color.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `color()` | fn | ✅ Runtime | `pine_color::new_color` or direct construction | |
| `color.b()` | fn | ✅ Runtime | `pine_color::b(c)` | |
| `color.from_gradient()` | fn | ❓ Unknown | No runtime gradient function | |
| `color.g()` | fn | ✅ Runtime | `pine_color::g(c)` | |
| `color.new()` | fn | ✅ Runtime | `pine_color::new_color(c, transp)` | |
| `color.r()` | fn | ✅ Runtime | `pine_color::r(c)` | |
| `color.rgb()` | fn | 🔧 Transpiler | ARGB assembly inline | |
| `color.t()` | fn | ✅ Runtime | `pine_color::t(c)` | |

### Functions — dayofmonth / dayofweek / hour / minute / month / second / weekofyear / year (1-arg function forms)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dayofmonth()` | fn | ✅ Runtime | 1-arg form uses `pine_time` + decompose | |
| `dayofweek()` | fn | ✅ Runtime | Same | |
| `hour()` | fn | ✅ Runtime | Same | |
| `minute()` | fn | ✅ Runtime | Same | |
| `month()` | fn | ✅ Runtime | Same | |
| `second()` | fn | ✅ Runtime | Same | |
| `weekofyear()` | fn | ✅ Runtime | Same | |
| `year()` | fn | ✅ Runtime | Same | |

### Functions — fill / fixnan / hline / indicator / library

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `fill()` | fn | ⏭️ Parse-and-skip | Drawing fill; no runtime | |
| `fixnan()` | fn | 🔧 Transpiler | Emitted inline: `is_na(x) ? prev : x` | |
| `hline()` | fn | ⏭️ Parse-and-skip | Rendering; no runtime | |
| `indicator()` | fn | ⏭️ Parse-and-skip | Indicator declaration; strategy-only engine | |
| `library()` | fn | ❌ Unsupported | Library system not implemented | |

### Functions — footprint.* (9 entries)

All **❓ Unknown** — footprint chart type not in runtime.

### Functions — input.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `input()` | fn | ✅ Runtime | `get_input_*()` typed getters on engine | |
| `input.bool()` | fn | ✅ Runtime | `get_input_bool()` | |
| `input.color()` | fn | ✅ Runtime | `get_input_string()` → color parse | |
| `input.enum()` | fn | ✅ Runtime | `get_input_int()` with enum table | |
| `input.float()` | fn | ✅ Runtime | `get_input_double()` | |
| `input.int()` | fn | ✅ Runtime | `get_input_int()` | |
| `input.price()` | fn | ✅ Runtime | `get_input_double()` | |
| `input.session()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.source()` | fn | ✅ Runtime | `get_input_string()` → source series | |
| `input.string()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.symbol()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.text_area()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.time()` | fn | ✅ Runtime | `get_input_double()` (timestamp) | |
| `input.timeframe()` | fn | ✅ Runtime | `get_input_string()` | |

> Note: UI metadata (`group`, `tooltip`, `confirm`, `options`, `min/max/step`) has no runtime backing — all inputs arrive as strings in the injection map.

### Functions — label.* (20 entries)

All **⏭️ Parse-and-skip** — drawing object methods; no runtime backing.

### Functions — line.* (20 entries)

All **⏭️ Parse-and-skip** — drawing object methods; no runtime backing.

### Functions — linefill.* (5 entries)

All **⏭️ Parse-and-skip** — drawing object methods; no runtime backing.

### Functions — log.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `log.error()` | fn | ✅ Runtime | `pine_log_error()` in `log.hpp` | |
| `log.info()` | fn | ✅ Runtime | `pine_log_info()` in `log.hpp` | |
| `log.warning()` | fn | ✅ Runtime | `pine_log_warning()` in `log.hpp` | |

### Functions — map.* (11 entries)

All **🔧 Transpiler** — emitted against `std::unordered_map<K,V>`. No runtime module.

### Functions — math.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `math.abs()` | fn | 🔧 Transpiler | `std::abs()` / `std::fabs()` | |
| `math.acos()` | fn | 🔧 Transpiler | `std::acos()` | |
| `math.asin()` | fn | 🔧 Transpiler | `std::asin()` | |
| `math.atan()` | fn | 🔧 Transpiler | `std::atan()` | |
| `math.avg()` | fn | 🔧 Transpiler | Inline mean of variadic args | |
| `math.ceil()` | fn | 🔧 Transpiler | `std::ceil()` | |
| `math.cos()` | fn | 🔧 Transpiler | `std::cos()` | |
| `math.exp()` | fn | 🔧 Transpiler | `std::exp()` | |
| `math.floor()` | fn | 🔧 Transpiler | `std::floor()` | |
| `math.log()` | fn | 🔧 Transpiler | `std::log()` | |
| `math.log10()` | fn | 🔧 Transpiler | `std::log10()` | |
| `math.max()` | fn | 🔧 Transpiler | Variadic max inline | |
| `math.min()` | fn | 🔧 Transpiler | Variadic min inline | |
| `math.pow()` | fn | 🔧 Transpiler | `std::pow()` | |
| `math.random()` | fn | ✅ Runtime | `pine_random(...)` in `math.hpp` (SplitMix64, not TV-exact) | |
| `math.round()` | fn | 🔧 Transpiler | `std::round()` | |
| `math.round_to_mintick()` | fn | ✅ Runtime | `round_to_mintick()` on `BacktestEngine` | |
| `math.sign()` | fn | 🔧 Transpiler | Inline signum | |
| `math.sin()` | fn | 🔧 Transpiler | `std::sin()` | |
| `math.sqrt()` | fn | 🔧 Transpiler | `std::sqrt()` | |
| `math.sum()` | fn | ✅ Runtime | `math::Sum` class (`math.cpp`) | |
| `math.tan()` | fn | 🔧 Transpiler | `std::tan()` | |
| `math.todegrees()` | fn | 🔧 Transpiler | `x * 180.0 / M_PI` | |
| `math.toradians()` | fn | 🔧 Transpiler | `x * M_PI / 180.0` | |

### Functions — matrix.* (44 entries)

All **✅ Runtime** — backed by `PineMatrix` (`matrix.hpp` / `matrix.cpp`) for float matrices, and `PineGenericMatrix<T>` (`generic_matrix.hpp`) for int/bool/string/color element types. Numeric methods (`det`, `inv`, `pinv`, `eigenvalues`, etc.) are float-only.

| Identifier | Notes |
|---|---|
| `matrix.new<type>()` | ✅ Runtime; `<float>` → `PineMatrix`, other types → `PineGenericMatrix<T>` |
| `matrix.det()` / `matrix.inv()` / `matrix.pinv()` / `matrix.eigenvalues()` / `matrix.eigenvectors()` | ✅ Runtime float-only |
| `matrix.kron()` | ✅ Runtime |
| All structural ops (add_row, remove_row, reshape, transpose, etc.) | ✅ Runtime |

### Functions — max_bars_back / na / nz

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `max_bars_back()` | fn | 🔧 Transpiler | Series buffer size hint; `Series<T>` max_len | |
| `na()` | fn | ✅ Runtime | `na<T>()` in `na.hpp` | |
| `nz()` | fn | 🔧 Transpiler | `is_na(x) ? 0.0 : x` inline | |

### Functions — plot / plotarrow / plotbar / plotcandle / plotchar / plotshape

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `plot()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |
| `plotarrow()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |
| `plotbar()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |
| `plotcandle()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |
| `plotchar()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |
| `plotshape()` | fn | ⏭️ Parse-and-skip | Compiles; no visual output | |

### Functions — polyline.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `polyline.delete()` | fn | ⏭️ Parse-and-skip | Drawing; no runtime | |
| `polyline.new()` | fn | ⏭️ Parse-and-skip | Drawing; no runtime | |

### Functions — request.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `request.currency_rate()` | fn | ❌ Unsupported | Rejected at transpile | No FX data feed |
| `request.dividends()` | fn | ❌ Unsupported | Rejected at transpile | No fundamentals feed |
| `request.earnings()` | fn | ❌ Unsupported | Rejected at transpile | No fundamentals feed |
| `request.economic()` | fn | ❌ Unsupported | Rejected at transpile | No macro data feed |
| `request.financial()` | fn | ❌ Unsupported | Rejected at transpile | No fundamentals feed |
| `request.footprint()` | fn | ❌ Unsupported | Footprint data not supported | |
| `request.quandl()` | fn | ❌ Unsupported | Deprecated upstream; rejected | |
| `request.security()` | fn | ✅ Runtime | `SecurityEvalState` + full TF aggregation machinery | Same-symbol MTF + higher-TF aggregation |
| `request.security_lower_tf()` | fn | ✅ Runtime | Lower-TF emulation via `synthesize_lower_tf_bars` | Intraday same-symbol only |
| `request.seed()` | fn | ❌ Unsupported | TV-infrastructure-dependent; rejected | |
| `request.splits()` | fn | ❌ Unsupported | Rejected at transpile | No corporate actions feed |

### Functions — runtime.error

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `runtime.error()` | fn | ✅ Runtime | `pine_runtime_error()` in `log.hpp` (throws) | |

### Functions — str.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `str.contains()` | fn | 🔧 Transpiler | `std::string::find` | |
| `str.endswith()` | fn | 🔧 Transpiler | `std::string::ends_with` or substr compare | |
| `str.format()` | fn | ✅ Runtime | `pine_str_format()` in `str_utils.hpp` | |
| `str.format_time()` | fn | ✅ Runtime | `pine_str_format_time()` in `str_utils.hpp` | |
| `str.length()` | fn | 🔧 Transpiler | `std::string::size()` | |
| `str.lower()` | fn | 🔧 Transpiler | `std::tolower` transform | |
| `str.match()` | fn | ✅ Runtime | `pine_str_match()` — regex match | |
| `str.pos()` | fn | 🔧 Transpiler | `std::string::find` | |
| `str.repeat()` | fn | 🔧 Transpiler | Loop concat | |
| `str.replace()` | fn | 🔧 Transpiler | `std::string::replace` | |
| `str.replace_all()` | fn | 🔧 Transpiler | Loop replace | |
| `str.split()` | fn | ✅ Runtime | `pine_str_split()` in `str_utils.hpp` | |
| `str.startswith()` | fn | 🔧 Transpiler | `std::string::starts_with` | |
| `str.substring()` | fn | 🔧 Transpiler | `std::string::substr` | |
| `str.tonumber()` | fn | 🔧 Transpiler | `std::stod` with catch | |
| `str.tostring()` | fn | ✅ Runtime | `pine_str_tostring()` in `str_utils.hpp` | |
| `str.trim()` | fn | 🔧 Transpiler | Whitespace strip inline | |
| `str.upper()` | fn | 🔧 Transpiler | `std::toupper` transform | |

### Functions — strategy.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `strategy()` | fn | ✅ Runtime | `BacktestEngine` constructor + `StrategyOverrides` | |
| `strategy.cancel()` | fn | ✅ Runtime | `strategy_cancel()` | |
| `strategy.cancel_all()` | fn | ✅ Runtime | `strategy_cancel_all()` | |
| `strategy.close()` | fn | ✅ Runtime | `strategy_close()` | |
| `strategy.close_all()` | fn | ✅ Runtime | `strategy_close_all()` | |
| `strategy.closedtrades.commission()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.entry_bar_index()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.entry_comment()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.entry_id()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.entry_price()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.entry_time()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.exit_bar_index()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.exit_comment()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.exit_id()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.exit_price()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.exit_time()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.max_drawdown()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.max_drawdown_percent()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.max_runup()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.max_runup_percent()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.profit()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.profit_percent()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.closedtrades.size()` | fn | ✅ Runtime | Trade accessor | |
| `strategy.convert_to_account()` | fn | 🔧 Transpiler | Identity (no FX conversion) | |
| `strategy.convert_to_symbol()` | fn | 🔧 Transpiler | Identity (no FX conversion) | |
| `strategy.default_entry_qty()` | fn | ✅ Runtime | `default_qty_value_` | |
| `strategy.entry()` | fn | ✅ Runtime | `strategy_entry()` — full OCA/pyramid/deferred-flip | |
| `strategy.exit()` | fn | ✅ Runtime | `strategy_exit()` — trail/limit/stop exits | |
| `strategy.opentrades.commission()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.entry_bar_index()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.entry_comment()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.entry_id()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.entry_price()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.entry_time()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.max_drawdown()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.max_drawdown_percent()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.max_runup()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.max_runup_percent()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.profit()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.profit_percent()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.opentrades.size()` | fn | ✅ Runtime | Open-trade accessor | |
| `strategy.order()` | fn | ✅ Runtime | `strategy_order()` — raw pending order | |
| `strategy.risk.allow_entry_in()` | fn | ✅ Runtime | `risk_direction_` | |
| `strategy.risk.max_cons_loss_days()` | fn | ✅ Runtime | `risk_max_cons_loss_days_` | |
| `strategy.risk.max_drawdown()` | fn | ✅ Runtime | `risk_max_drawdown_` | |
| `strategy.risk.max_intraday_filled_orders()` | fn | ✅ Runtime | `max_intraday_filled_orders_` | |
| `strategy.risk.max_intraday_loss()` | fn | ✅ Runtime | `risk_max_intraday_loss_` | |
| `strategy.risk.max_position_size()` | fn | ✅ Runtime | `risk_max_position_size_` | |

### Functions — syminfo.prefix / syminfo.ticker (function forms)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `syminfo.prefix()` | fn | ✅ Runtime | `syminfo_.prefix` via free helper | |
| `syminfo.ticker()` | fn | ✅ Runtime | `syminfo_.ticker` | |

### Functions — ta.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ta.alma()` | fn | ✅ Runtime | `ta::ALMA` class | |
| `ta.atr()` | fn | ✅ Runtime | `ta::ATR` class | |
| `ta.barssince()` | fn | ✅ Runtime | `ta::BarsSince` class | |
| `ta.bb()` | fn | ✅ Runtime | `ta::BB` class | |
| `ta.bbw()` | fn | ✅ Runtime | `ta::BBW` class | |
| `ta.cci()` | fn | ✅ Runtime | `ta::CCI` class | |
| `ta.change()` | fn | ✅ Runtime | `ta::Change` class (bool inputs cast by transpiler) | |
| `ta.cmo()` | fn | ✅ Runtime | `ta::CMO` class | |
| `ta.cog()` | fn | ✅ Runtime | `ta::COG` class | |
| `ta.correlation()` | fn | ✅ Runtime | `ta::Correlation` class | |
| `ta.cross()` | fn | ✅ Runtime | `ta::Cross` class (skip-tie state) | |
| `ta.crossover()` | fn | ✅ Runtime | `ta::Crossover` class | |
| `ta.crossunder()` | fn | ✅ Runtime | `ta::Crossunder` class | |
| `ta.cum()` | fn | ✅ Runtime | `ta::Cum` class | |
| `ta.dev()` | fn | ✅ Runtime | `ta::Dev` class (mean absolute deviation) | |
| `ta.dmi()` | fn | ✅ Runtime | `ta::DMI` class | |
| `ta.ema()` | fn | ✅ Runtime | `ta::EMA` class | |
| `ta.falling()` | fn | ✅ Runtime | `ta::Falling` class | |
| `ta.highest()` | fn | ✅ Runtime | `ta::Highest` class | |
| `ta.highestbars()` | fn | ✅ Runtime | `ta::HighestBars` class | |
| `ta.hma()` | fn | ✅ Runtime | `ta::HMA` class | |
| `ta.kc()` | fn | ✅ Runtime | `ta::KC` class | |
| `ta.kcw()` | fn | ✅ Runtime | `ta::KCW` class | |
| `ta.linreg()` | fn | ✅ Runtime | `ta::Linreg` class | |
| `ta.lowest()` | fn | ✅ Runtime | `ta::Lowest` class | |
| `ta.lowestbars()` | fn | ✅ Runtime | `ta::LowestBars` class | |
| `ta.macd()` | fn | ✅ Runtime | `ta::MACD` class | |
| `ta.max()` | fn | ✅ Runtime | `ta::AllTimeMax` class | Single-arg form only |
| `ta.median()` | fn | ✅ Runtime | `ta::Median` class | |
| `ta.mfi()` | fn | ✅ Runtime | `ta::MFI` class | |
| `ta.min()` | fn | ✅ Runtime | `ta::AllTimeMin` class | Single-arg form only |
| `ta.mode()` | fn | ✅ Runtime | `ta::Mode` class | |
| `ta.mom()` | fn | ✅ Runtime | `ta::Mom` class | |
| `ta.percentile_linear_interpolation()` | fn | ✅ Runtime | `ta::PercentileLinearInterpolation` class | |
| `ta.percentile_nearest_rank()` | fn | ✅ Runtime | `ta::PercentileNearestRank` class | |
| `ta.percentrank()` | fn | ✅ Runtime | `ta::PercentRank` class | |
| `ta.pivot_point_levels()` | fn | ✅ Runtime | Free function `ta::pivot_point_levels(method, high, low, close)` | Woodie uses close-based fallback |
| `ta.pivothigh()` | fn | ✅ Runtime | `ta::PivotHigh` class | |
| `ta.pivotlow()` | fn | ✅ Runtime | `ta::PivotLow` class | |
| `ta.range()` | fn | ✅ Runtime | `ta::Range` class | |
| `ta.rci()` | fn | ✅ Runtime | `ta::RCI` class | |
| `ta.rising()` | fn | ✅ Runtime | `ta::Rising` class | |
| `ta.rma()` | fn | ✅ Runtime | `ta::RMA` class | |
| `ta.roc()` | fn | ✅ Runtime | `ta::ROC` class | |
| `ta.rsi()` | fn | ✅ Runtime | `ta::RSI` class | |
| `ta.sar()` | fn | ✅ Runtime | `ta::SAR` class | |
| `ta.sma()` | fn | ✅ Runtime | `ta::SMA` class | |
| `ta.stdev()` | fn | ✅ Runtime | `ta::StdDev` class | |
| `ta.stoch()` | fn | ✅ Runtime | `ta::Stoch` class | %K only; %D is explicit Pine |
| `ta.supertrend()` | fn | ✅ Runtime | `ta::Supertrend` class | |
| `ta.swma()` | fn | ✅ Runtime | `ta::SWMA` class | |
| `ta.tr()` | fn | ✅ Runtime | `ta::TR(handle_na)` class | |
| `ta.tsi()` | fn | ✅ Runtime | `ta::TSI` class | |
| `ta.valuewhen()` | fn | ✅ Runtime | `ta::ValueWhen` class | |
| `ta.variance()` | fn | ✅ Runtime | `ta::Variance` class | |
| `ta.vwap()` | fn | ✅ Runtime | `ta::VWAP` (single value) + `ta::VWAPBands` 3-tuple class (`VWAPBandsResult{vwap,upper,lower}`) | Both overloads supported; Sprint B added 3-arg `(src, anchor, stdev_mult)` form with running variance |
| `ta.vwma()` | fn | ✅ Runtime | `ta::VWMA` class | |
| `ta.wma()` | fn | ✅ Runtime | `ta::WMA` class | |
| `ta.wpr()` | fn | ✅ Runtime | `ta::WPR` class | |

### Functions — table.* (20 entries)

All **⏭️ Parse-and-skip** — table drawing methods; no runtime backing.

### Functions — ticker.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ticker.heikinashi()` | fn | ❌ Unsupported | Hard-reject: "chart-type modifier not supported" | Sprint G2 explicit reject (was blanket namespace reject) |
| `ticker.inherit()` | fn | 🔧 Transpiler | Passthrough — emits `symbol` argument unchanged | Sprint G2 rescue (same-symbol passthrough is valid) |
| `ticker.kagi()` | fn | ❌ Unsupported | Hard-reject | Sprint G2 |
| `ticker.linebreak()` | fn | ❌ Unsupported | Hard-reject | Sprint G2 |
| `ticker.modify()` | fn | ❌ Unsupported | Hard-reject: "cross-symbol construction not supported" | Sprint G2 |
| `ticker.new()` | fn | ❌ Unsupported | Hard-reject: "cross-symbol construction not supported" | Sprint G2 |
| `ticker.pointfigure()` | fn | ❌ Unsupported | Hard-reject | Sprint G2 |
| `ticker.renko()` | fn | ❌ Unsupported | Hard-reject | Sprint G2 |
| `ticker.standard()` | fn | 🔧 Transpiler | Passthrough — emits `symbol` argument unchanged | Sprint G2 rescue |

### Functions — time / time_close / timeframe.* / timestamp

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `time()` | fn | ✅ Runtime | `pine_time(bar_ms, tf, session, tz, chart_tf)` in `session_time.hpp` | |
| `time_close()` | fn | ✅ Runtime | `pine_time_close(...)` in `session_time.hpp` | |
| `timeframe.change()` | fn | ✅ Runtime | `tf_change(prev_ms, curr_ms, tf)` in `timeframe.hpp` | |
| `timeframe.from_seconds()` | fn | 🔧 Transpiler | Inverse of `tf_to_seconds` | |
| `timeframe.in_seconds()` | fn | ✅ Runtime | `tf_to_seconds(tf)` | |
| `timestamp()` | fn | 🔧 Transpiler | UTC timestamp construction inline | |

### Functions — volume_row.* (8 entries)

All **❓ Unknown** — footprint chart type; no runtime module.

### Keywords

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `and` | kw | 🔧 Transpiler | C++ `&&` | |
| `enum` | kw | 🔧 Transpiler | C++ enum + `pine_enum_str_at` | |
| `export` | kw | ❌ Unsupported | Library export; not implemented | |
| `for` | kw | 🔧 Transpiler | C++ `for` | |
| `for...in` | kw | 🔧 Transpiler | Range-based for | |
| `if` | kw | 🔧 Transpiler | C++ `if` | |
| `import` | kw | ❌ Unsupported | Library system not implemented | |
| `method` | kw | 🔧 Transpiler | UDT method generation | |
| `not` | kw | 🔧 Transpiler | C++ `!` | |
| `or` | kw | 🔧 Transpiler | C++ `||` | |
| `switch` | kw | 🔧 Transpiler | C++ `switch` | |
| `type` | kw | 🔧 Transpiler | UDT struct generation | |
| `var` | kw | 🔧 Transpiler | Persistent variable (static in on_bar) | |
| `varip` | kw | 🔧 Transpiler | Treated as `var` (semantically identical in batch mode); transpile warning emitted | Sprint C |
| `while` | kw | 🔧 Transpiler | C++ `while` | |

### Operators

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `-` | op | 🔧 Transpiler | C++ `-` | |
| `-=` | op | 🔧 Transpiler | C++ `-=` | |
| `:=` | op | 🔧 Transpiler | Assignment | |
| `!=` | op | 🔧 Transpiler | C++ `!=` | |
| `?:` | op | 🔧 Transpiler | C++ ternary | |
| `[]` | op | ✅ Runtime | `Series<T>::operator[](k)` | |
| `*` | op | 🔧 Transpiler | C++ `*` | |
| `*=` | op | 🔧 Transpiler | C++ `*=` | |
| `/` | op | 🔧 Transpiler | C++ `/` | |
| `/=` | op | 🔧 Transpiler | C++ `/=` | |
| `%` | op | 🔧 Transpiler | C++ `%` or `std::fmod` | |
| `%=` | op | 🔧 Transpiler | C++ `%=` | |
| `+` | op | 🔧 Transpiler | C++ `+` | |
| `+=` | op | 🔧 Transpiler | C++ `+=` | |
| `<` | op | 🔧 Transpiler | C++ `<` | |
| `<=` | op | 🔧 Transpiler | C++ `<=` | |
| `=` | op | 🔧 Transpiler | C++ `=` | |
| `==` | op | 🔧 Transpiler | C++ `==` | |
| `=>` | op | 🔧 Transpiler | Arrow syntax (function body) | |
| `>` | op | 🔧 Transpiler | C++ `>` | |
| `>=` | op | 🔧 Transpiler | C++ `>=` | |

### Annotations

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `@description` | anno | 🔧 Transpiler | Doc comment; transpiler reads, no runtime effect | |
| `@enum` | anno | 🔧 Transpiler | Doc comment | |
| `@field` | anno | 🔧 Transpiler | Doc comment | |
| `@function` | anno | 🔧 Transpiler | Doc comment | |
| `@param` | anno | 🔧 Transpiler | Doc comment | |
| `@returns` | anno | 🔧 Transpiler | Doc comment | |
| `@strategy_alert_message` | anno | ⏭️ Parse-and-skip | Alert template; no runtime | |
| `@type` | anno | 🔧 Transpiler | Doc comment | |
| `@variable` | anno | 🔧 Transpiler | Doc comment | |
| `@version=` | anno | 🔧 Transpiler | Version declaration read by transpiler | |

---

## What PineForge cannot run

These identifiers are rejected at transpile time with a loud error or produce a no-op with `na`. Strategies relying on them cannot run on PineForge without modification.

### Fundamentals and external data (hard reject)

- `request.financial(symbol, field, period)` — fundamental data fetch. No external data feed; transpiler rejects.
- `request.dividends(ticker, field, gaps, lookahead, ignore_startbar, currency)` — corporate action data. Rejected.
- `request.earnings(ticker, field, gaps, lookahead, ignore_startbar, currency)` — earnings data. Rejected.
- `request.splits(ticker, field, gaps, lookahead, ignore_startbar)` — corporate action data. Rejected.
- `request.economic(country_code, field, ...)` — macro economic data. Rejected.
- `request.currency_rate(from, to, ignore_startbar)` — FX rate feed. Rejected.
- `request.seed(source, symbol, expression)` — TV user-published time series. Rejected (infrastructure-dependent).
- `request.quandl(ticker, gaps, index, ignore_startbar)` — deprecated upstream. Rejected.

### Analyst and fundamental syminfo fields (return na)

`syminfo.recommendations_buy`, `syminfo.recommendations_sell`, `syminfo.target_price_*`, `syminfo.employees`, `syminfo.shareholders`, `syminfo.shares_outstanding_*`, `syminfo.sector`, `syminfo.industry`, `syminfo.isin`, `syminfo.expiration_date`, `syminfo.current_contract`, `syminfo.mincontract` — these fields are not in the `SymInfo` struct and return na (sprint G2 moved them from silent ❌ to explicit ⏭️ na-accept). Strategies that gate logic on these values via `if` / `?:` get a transpile-time **warning** so the silent-suppression issue is surfaced. `syminfo.country` and `syminfo.main_tickerid` are derived from `syminfo.tickerid` at codegen-time (no external data needed). Long-term: pineforge-data integration will populate these fields per symbol.

### Dividend / earnings / splits variables (return na)

`dividends.future_amount`, `dividends.future_ex_date`, `dividends.future_pay_date`, `earnings.future_eps`, `earnings.future_revenue`, `earnings.future_time`, `earnings.future_period_end_time` — no external data feed; always na. Strategies using these as signals will silently produce na-driven (always-false) conditions.

### Realtime-only variables (always na or false)

- `ask` / `bid` — live order book; no feed. Always na.
- `timenow` — live clock. Batch mode has no "now". Always na.
- `chart.left_visible_bar_time` / `chart.right_visible_bar_time` — viewport concept. No equivalent in batch.

### varip (keyword — accepted as var with warning)

**Sprint C update:** `varip` is now accepted by PineForge's transpiler and treated identically to `var` (semantically equivalent in batch mode — `varip`'s intrabar-tick distinction has no meaning when bars are evaluated once). A transpile-time warning is emitted to remind authors of the substitution. Strategies using `varip` now compile cleanly.

### Library system (hard reject)

`import`, `export`, `library()` — the library resolver is not implemented. Pre-inline library code as a workaround.

### Drawing / plotting (parse-and-skip — silent)

`plot`, `plotshape`, `plotchar`, `plotcandle`, `plotbar`, `plotarrow`, `fill`, `hline`, `bgcolor`, `barcolor`, all `label.*` / `line.*` / `box.*` / `table.*` / `polyline.*` / `linefill.*` methods — these compile silently; no visual output is emitted. **Strategies that only use these for display will run correctly for backtesting purposes.** Strategies that use the _return values_ of `label.new()` / `line.new()` / etc. to store state will compile but those objects will be null/no-op references.

### Alert functions (parse-and-skip — silent)

`alert()` / `alertcondition()` — compiled silently; no alert is ever sent. PineForge is a batch engine with no live alert capability.

### ta.vwap 3-tuple form

**Sprint B update — RESOLVED.** `ta.vwap(source, anchor, stdev_mult)` 3-tuple form `[vwap, upper_band, lower_band]` is now fully supported. The engine's `ta::VWAP` class was extended with running variance (`cum_pv_sq_` field) and a `VWAPBandsResult{vwap,upper,lower}` struct (modelled on `ta::BBResult`). Codegen dispatches the 3-arg overload via `signatures.py` dual-overload (matching the `ta.highest`/`lowest` pattern) and routes through `vwap_bands` in `analyzer/call_handlers.py`. Verified bit-exact against TV at 916/916 (breakout 1σ) and 225/225 (mean-reversion 2σ) corpus probes.

---

## Sprint changes (2026-05-17)

Pine v6 HIGH+MEDIUM sprint added ~50 identifiers to the supported surface
(✅ Runtime + 🔧 Transpiler) and reclassified ~20 from ❓/❌ to explicit
buckets. Full final-state release notes: [v0.5.0 release](https://github.com/fullpass-4pass/pineforge-engine/releases/tag/v0.5.0).

### ✅ Runtime additions (17)

| Identifiers | Group |
|---|---|
| `session.ismarket`, `session.ispremarket`, `session.ispostmarket`, `session.isfirstbar(_regular)`, `session.islastbar(_regular)` (7) | A |
| `ta.vwap` 3-tuple bands overload (1) | B |
| `timeframe.main_period` (1) | E |
| `strategy.max_contracts_held_all/_long/_short`, `strategy.eventrades` (4) | F |
| `time_tradingday` (1) | G1 |
| `pine_session_ismarket/ispremarket/ispostmarket` engine helpers + `pine_time_tradingday` (3, internal) | A + G1 |

### 🔧 Transpiler additions (21)

| Identifiers | Group |
|---|---|
| `chart.is_standard/heikinashi/kagi/linebreak/pnf/range/renko` (7) | E |
| `timeframe.isticks` (1) | E |
| `backadjustment.inherit/on/off` (3) | G2 |
| `settlement_as_close.on/off/inherit` (3) | G2 |
| `adjustment.dividends/splits` (2; `none` was already ✅) | G2 |
| `syminfo.main_tickerid`, `syminfo.country` (2 — derived from `syminfo_.tickerid` via `helpers_syminfo.py`) | G2 |
| `varip` (1 — now warn-then-emit-as-var) | C |
| `ticker.inherit`, `ticker.standard` (2 — passthrough for same-symbol use) | G2 |

### ⏭️ Parse-and-skip additions (12) — most are silent-gap remediation

| Identifiers | Group | Note |
|---|---|---|
| `syminfo.prefix`, `syminfo.root`, `syminfo.pricescale`, `syminfo.minmove` (4) | G2 | **Critical fix** — were silently emitting `0` (fields NOT in `SymInfo` struct, contrary to prior audit); now `na<T>()` |
| `syminfo.mincontract`, `syminfo.current_contract`, `syminfo.expiration_date`, `syminfo.isin`, `syminfo.sector`, `syminfo.industry` (6) | G2 | Was ❓; now ⏭️ na-accept; conditional-use warning |
| ~14 LOW-tier `syminfo.recommendations_*`, `syminfo.target_price_*`, `syminfo.employees`, etc. moved from ❌ to ⏭️ | G2 | Reclassification only; behaviour unchanged (still `na`) |

### ❌ Unsupported (additions: 9; all explicit ticker.* hard-reject)

| Identifiers | Group |
|---|---|
| `ticker.heikinashi/renko/kagi/linebreak/pointfigure` (5) — chart-type modifiers, engine doesn't synthesize alt bars | G2 |
| `ticker.new`, `ticker.modify` (2) — cross-symbol construction not supported | G2 |
| `request.footprint`, `footprint.*` (counted in unchanged ❌ but now explicitly noted) | — |

(Previously these were blanket-rejected via namespace; sprint G2 made per-function rejection explicit with clearer error message.)

### Engine bug filed during sprint

- **GitHub Issue [#16](https://github.com/fullpass-4pass/pineforge-engine/issues/16):** `max_intraday_filled_orders` cap-day boundary uses `chart_timezone` instead of exchange `syminfo.timezone`. Workaround shipped: validator `engine_chart_timezone` override key (per-probe). Long-term fix blocked on pineforge-data integration.

### Validator workflow improvements (pineforge-utils)

- Eigen auto-detect at homebrew/macports/apt paths — recovered 6 matrix probes regressing to compile-fail.
- New `engine_chart_timezone` key in `inputs.json` schema for per-probe override of `_engine_chart_tz_for_csv_tz` auto-derivation.
- Validator default sweep skips `corpus/validation/symbol-specified/` subtree (5 stock probes blocked on pineforge-data).

### Final corpus parity

After sprint + 6 new TV-parity probes + 2 strong→excellent config tweaks:

| Bucket | Count |
|---|---|
| Excellent | **233 / 234** (99.6%) |
| Strong | 0 |
| Weak | 1 (`anomaly-equity-mirror-strategy-equity-01` — intentional) |

vs pre-sprint baseline 227 excellent + 1 anomaly: **+6 excellent, 0 regressions**.

---

## Reproducibility

To re-derive the Pine v6 identifier list:
1. Open a Playwright browser to `https://www.tradingview.com/pine-script-reference/v6/`
2. Wait for the JS SPA to fully render (all sections must be in DOM)
3. Execute: `Array.from(document.querySelectorAll('h3')).map(h => h.textContent.trim())`
4. The result is the 941-entry flat list used above

To re-derive the PineForge runtime surface:
1. Read all `*.hpp` under `include/pineforge/` and catalog every class, free function, and inline helper
2. Read `src/engine_internal.hpp` for additional internal types
3. Cross-reference against `docs/coverage.md` for the authoritative module-level summary
