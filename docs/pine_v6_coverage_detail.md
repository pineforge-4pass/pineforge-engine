# Pine v6 Coverage — Identifier-by-Identifier Audit

> ✅ **RECONCILED 2026-07-18** against pineforge-codegen-oss `cefeec8` and pineforge-engine `06bb724`. The 2026-07-18 delta records end-to-end `PineMap<K,V>` integration and its fail-closed boundaries on top of the 2026-06-10 audit-fix sweep. This remains a **surgical refresh**, not a full per-identifier re-derivation: untouched rows still carry the 2026-05-17 sprint snapshot, and the ❓ bucket has not been re-audited. Headline totals are delta-reconciled (see the footnote under the totals table). Rows corrected since against engine `35db01c8` / codegen-oss `70c2b4af` (drawings, `varip`, `max_bars_back()`, the cosmetic `color()` / `chart.*` reads, `timenow`, `ticker.heikinashi()`, library imports, the recorded `request.*` forms) move identifiers between buckets that the totals below do not recount.

| Field | Value |
|---|---|
| **Generated** | 2026-05-17 (post Pine v6 HIGH+MEDIUM sprint) |
| **Reconciled** | 2026-07-18 — vs codegen-oss `cefeec8` / engine `06bb724`; map type + 11 map functions moved from 🔧 to ✅ after bounded runtime integration |
| **Audit trail** | `pine_v6_audit_master.md` (2026-05-21 audit + per-fix [RESOLVED] tags), pineforge-engine#122, pineforge-codegen-oss#78 |
| **Pine v6 reference** | https://www.tradingview.com/pine-script-reference/v6/ (JS-rendered, scraped 2026-05-16) |
| **PineForge engine version** | v0.12.0-24-g06bb724 (commit `06bb724`) |
| **Total Pine v6 identifiers** | 941 |

## Headline totals

| Bucket | Count | % of 941 |
|---|---|---|
| ✅ Runtime | 193 | 21% |
| 🔧 Transpiler | 216 | 23% |
| ⏭️ Parse-and-skip | 216 | 23% |
| ❌ Unsupported | 175 | 19% |
| ❓ Unknown / not classified | 141 | 15% |

> **"Fully runs" headline:** PineForge executes **409 of 941** Pine v6 identifiers (✅ Runtime + 🔧 Transpiler = **43%**). The 2026-07-18 map integration reclassifies the `map` type and 11 `map.*` functions from 🔧 to ✅ without changing that combined total. An additional 23% parse-and-skip silently (no error, no effect — drawing/plotting + syminfo na-accepts). 19% are rejected loudly at transpile or produce na-returns — this bucket *grew* in the 2026-06-10 sweep because ~35 identifiers that previously miscompiled silently (constant-namespace free reads, `varip`, `footprint.*`/`volume_row.*`, bare `color()`, `color.from_gradient`, drawing-typed `array.new_*`, `max_bars_back`, `timeframe.from_seconds`, `indicator()`, `chart.bg/fg_color`) hard-rejected with a precise error after that sweep; several are accepted since (see their rows). 15% remain not-yet-audited at single-identifier precision.

> **Sprint delta (2026-05-17):** +17 ✅ Runtime, +21 🔧 Transpiler, +12 ⏭️ Parse-and-skip, +9 ❌ Unsupported (ticker.* split), −59 ❓ Unknown. See [Sprint changes](#sprint-changes-2026-05-17) section below.

> **Counting basis (2026-07-18 totals):**[^totals] the 2026-06-10 delta produced 181/228/216/175/141. The bounded PineMap integration moves the `map` type and 11 `map.*` functions from 🔧 to ✅: ✅ 181+12=193, 🔧 228−12=216, while ⏭️ 216, ❌ 175, and ❓ 141 remain unchanged; sum 941 ✓.

[^totals]: A full re-derivation (re-bucketing all 941 identifiers from code) has not been done; rows untouched by the master audit, the 2026-06-10 sweep, and the 2026-07-18 PineMap delta are trusted as of 2026-05-17.

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
| `box` | type | ✅ Runtime | `Box` handle, geometry as data in `drawing.hpp` | Visual setters are accepted no-ops |
| `chart.point` | type | ✅ Runtime | `ChartPoint` aggregate in `drawing.hpp` | |
| `color` | type | ✅ Runtime | `color.hpp` — 17 named ARGB constants + helpers | |
| `const` | type/qualifier | 🔧 Transpiler | C++ `const` | |
| `float` | type | ✅ Runtime | C++ `double` via `na.hpp` | |
| `footprint` | type | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACES` (`support_checker.py`) | Requires tick-level data the engine does not consume |
| `int` | type | ✅ Runtime | C++ `int` via `na.hpp` | |
| `label` | type | ✅ Runtime | `Label` handle, geometry and text as data in `drawing.hpp` | Visual setters are accepted no-ops |
| `line` | type | ✅ Runtime | `Line` handle, geometry as data in `drawing.hpp` | Visual setters are accepted no-ops |
| `linefill` | type | ✅ Runtime | `Linefill` handle in `drawing.hpp` | |
| `map` | type | ✅ Runtime | `PineMap<K,V>` in `map.hpp`, emitted by the transpiler | End-to-end for supported string-key/primitive-value maps; unsupported history, nested map-bearing matrices, and ambiguous inferred specializations fail closed |
| `matrix` | type | ✅ Runtime | `matrix.hpp` / `generic_matrix.hpp` | |
| `polyline` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `series` | type/qualifier | ✅ Runtime | `series.hpp` — `Series<T>` ring buffer | |
| `simple` | type/qualifier | 🔧 Transpiler | Compile-time qualifier; transpiler handles | |
| `string` | type | 🔧 Transpiler | C++ `std::string` | |
| `table` | type | ⏭️ Parse-and-skip | Drawing object; no runtime | |
| `volume_row` | type | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACES` (`support_checker.py`) | Same reason as `footprint` |

### Variables — bar data

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ask` | var | ❌ Unsupported | Rejected at transpile: a realtime-only builtin with no value in a backtest | Realtime-only |
| `bar_index` | var | ✅ Runtime | `bar_index_` on `BacktestEngine` | |
| `bid` | var | ❌ Unsupported | Rejected at transpile: a realtime-only builtin with no value in a backtest | Realtime-only |
| `close` | var | ✅ Runtime | `current_bar_.close` on `BacktestEngine` | |
| `dayofmonth` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware per Pine spec (was UTC `_bar_dayofmonth()`); value-identical on UTC data |
| `dayofweek` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `high` | var | ✅ Runtime | `current_bar_.high` on `BacktestEngine` | |
| `hl2` | var | 🔧 Transpiler | `(high+low)/2` emitted inline | |
| `hlc3` | var | 🔧 Transpiler | `(high+low+close)/3` emitted inline | |
| `hlcc4` | var | 🔧 Transpiler | `(high+low+close+close)/4` emitted inline | |
| `hour` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `last_bar_index` | var | ✅ Runtime | Computed from bar count in run loop | |
| `last_bar_time` | var | ✅ Runtime | Timestamp of last bar | |
| `low` | var | ✅ Runtime | `current_bar_.low` on `BacktestEngine` | |
| `minute` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `month` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `na` | var/fn | ✅ Runtime | `na.hpp` — `na<T>()` generic, `is_na(...)` | |
| `ohlc4` | var | 🔧 Transpiler | `(open+high+low+close)/4` inline | |
| `open` | var | ✅ Runtime | `current_bar_.open` on `BacktestEngine` | |
| `second` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `time` | var | ✅ Runtime | `current_bar_.timestamp` | |
| `time_close` | var | ✅ Runtime | `pine_time_close(...)` in `session_time.hpp` | |
| `time_tradingday` | var | ✅ Runtime | `pine_time_tradingday(bar_ms, session, tz)` in `session_time.hpp` | Sprint G1; derives session-day open in `syminfo_.timezone`; DST-edge fallback for Havana/Lord_Howe |
| `timenow` | var | 🔧 Transpiler | The current bar's `time` (`current_bar_.timestamp`); codegen warns it is not wall-clock time | |
| `volume` | var | ✅ Runtime | `current_bar_.volume` on `BacktestEngine` | |
| `weekofyear` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |
| `year` | var | 🔧 Transpiler | `pine_<field>(current_bar_.timestamp, syminfo_.timezone)` in `session_time.hpp` (`BAR_BUILTINS`) | 2026-06-10: exchange-TZ aware (was UTC) |

### Variables — barstate

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `barstate.isconfirmed` | var | ✅ Runtime | `is_last_tick_` (batch approx.) | |
| `barstate.isfirst` | var | ✅ Runtime | `bar_index == 0` (compiler-handled) | |
| `barstate.ishistory` | var | ✅ Runtime | always `true` in batch | |
| `barstate.islast` | var | ✅ Runtime | `barstate_islast_` | |
| `barstate.islastconfirmedhistory` | var | ✅ Runtime | `barstate_islast_` (true on the run's final bar) | Approximation; codegen warns |
| `barstate.isnew` | var | ✅ Runtime | `is_first_tick_` | |
| `barstate.isrealtime` | var | ✅ Runtime | always `false` in batch | |

### Variables — chart

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `chart.bg_color` | var | ⏭️ Parse-and-skip | Cosmetic read (`COSMETIC_MEMBERS` in `support_checker.py`): a warning and a default colour | |
| `chart.fg_color` | var | ⏭️ Parse-and-skip | Cosmetic read (`COSMETIC_MEMBERS` in `support_checker.py`): a warning and a default colour | |
| `chart.is_heikinashi` | var | 🔧 Transpiler | Constant `false` emitted by `visit_expr` | Sprint E |
| `chart.is_kagi` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_linebreak` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_pnf` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_range` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_renko` | var | 🔧 Transpiler | Constant `false` | Sprint E |
| `chart.is_standard` | var | 🔧 Transpiler | Constant `true` (engine always batches standard OHLCV) | Sprint E |
| `chart.left_visible_bar_time` | var | ⏭️ Parse-and-skip | Cosmetic read (`COSMETIC_MEMBERS` in `support_checker.py`): a warning and `0` | Viewport/UI concept; no batch equivalent |
| `chart.right_visible_bar_time` | var | ⏭️ Parse-and-skip | Cosmetic read (`COSMETIC_MEMBERS` in `support_checker.py`): a warning and `0` | Viewport/UI concept; no batch equivalent |

### Variables — dividends / earnings / splits

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dividends.future_amount` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` (`support_checker.py`) | Transpile-time reject, NOT a runtime na-return |
| `dividends.future_ex_date` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |
| `dividends.future_pay_date` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |
| `earnings.future_eps` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |
| `earnings.future_period_end_time` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |
| `earnings.future_revenue` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |
| `earnings.future_time` | var | ❌ Unsupported | Hard-reject via `UNSUPPORTED_NAMESPACE_VARS` | Transpile-time reject |

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
| `session.isfirstbar` | var | ✅ Runtime | `session_isfirstbar_` on the Pine host, selected by `scheduler_update_session_state` (`src/source/pine_strategy_host.cpp`) from the kernel's `NativeDecisionContext::opens_session_day` (R5 lane F5), widened on an extended-hours chart to the first pre-market bar of the day (`update_extended_session_day`, `tests/test_session_extended_day_tape.cpp`); the first bar of a session DAY — its predecessor was out of session or belonged to another session day; on a 1D+ chart every bar (the bar is its whole session) | Sprint A |
| `session.isfirstbar_regular` | var | ✅ Runtime | The regular session day's first bar, `PineStrategyHost::session_isfirstbar_regular_` (the kernel's `opens_session_day`); on an extended-hours chart `session.isfirstbar` is the pre-market's first bar instead. The transpiler lowers it to that member | Sprint A |
| `session.islastbar` | var | ✅ Runtime | `session_islastbar_` on the Pine host, same selection, from `NativeDecisionContext::closes_session_day` (`closes_session_day_open_ended` on a live probe's forming bar), widened on an extended-hours chart to the last post-market bar of the day; the last bar of a session DAY — a one-bar lookahead whose successor is out of session or belongs to another session day, as TradingView's tapes flag it (`tests/fixtures/session_islastbar`); on a 1D+ chart every bar (the bar is its whole session). Where the run holds TradingView's calendar for the symbol (`PineStrategyHost::set_symbol_calendar`, the harness's `PINEFORGE_RUN_SESSION_CALENDAR`), a regular intraday bar is last when the calendar's next slot opens another day: OANDA:XAUUSD's calendar goes on past the feed's early closes (`tests/fixtures/symbol_calendar`) | Sprint A |
| `session.islastbar_regular` | var | ✅ Runtime | The regular session day's last bar, `PineStrategyHost::session_islastbar_regular_`; on an extended-hours chart `session.islastbar` is the post-market's last bar instead. The transpiler lowers it to that member | Sprint A |
| `session.ismarket` | var | ✅ Runtime | On the chart, the session calendar asked at the bar's open (the transpiler's own helper over `native_calendar::session_day_at`), which reads a masked overnight session's Sunday open and a `0000-2400` day as TradingView's tapes do; always true on a 1D+ chart, as TradingView documents. Inside a `request.security` payload it is `pine_session_ismarket(session, tz, bar_ms, chart_tf)` in `session_time.hpp` (via `source::PineStrategyHost::pine_session_ismarket`), which reads a weekday-masked overnight session's Sunday open (`1700-1600:23456`) and every bar of `0000-2400` in market, as TradingView does (`tests/test_session_ismarket_tape.cpp`). A reading at the bar's open puts a bar that opens inside a break out of market, where TradingView and the kernel's `in_session` hold it in market (`tests/test_session_break_open_bar_tape.cpp`) | Sprint A |
| `session.ispostmarket` | var | ✅ Runtime | `pine_session_ispostmarket(...)` — from the session day's last close over every window to 20:00 local, on a bar not in session; none for a session that spans 24 hours or wraps midnight; always false on a 1D+ chart | Sprint A |
| `session.ispremarket` | var | ✅ Runtime | `pine_session_ispremarket(...)` — from 04:00 local to the session day's first open over every window, on a bar not in session; none for a session that spans 24 hours or wraps midnight; always false on a 1D+ chart | Sprint A |

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
| `strategy.closedtrades.first_index` | var | 🔧 Transpiler | Hardcoded `0` (`visit_expr.py`, with explanatory comment) | Correct until the engine implements the 9000-trade-list cap (Pine only advances `first_index` when capping drops old trades) |
| `strategy.equity` | var | ✅ Runtime | `current_equity() + open_profit(current_bar_.close)` | |
| `strategy.eventrades` | var | ✅ Runtime | `eventrades_count_` incremented in `engine_orders.cpp` when trade.profit == 0 | Sprint F |
| `strategy.grossloss` | var | ✅ Runtime | `gross_loss()` | |
| `strategy.grossloss_percent` | var | ✅ Runtime | `grossloss_percent()` | |
| `strategy.grossprofit` | var | ✅ Runtime | `gross_profit()` | |
| `strategy.grossprofit_percent` | var | ✅ Runtime | `grossprofit_percent()` | |
| `strategy.initial_capital` | var | ✅ Runtime | `initial_capital_` | |
| `strategy.losstrades` | var | ✅ Runtime | `count_losstrades()` | |
| `strategy.margin_liquidation_price` | var | ✅ Source host | `PineStrategyHost::margin_liquidation_price()` | Source-level projection over current native position/account facts |
| `strategy.max_contracts_held_all` | var | ✅ Runtime | `max_contracts_held_all_` per-bar `std::max(|position_qty_|)` in `update_equity_extremes()` | Sprint F |
| `strategy.max_contracts_held_long` | var | ✅ Runtime | `max_contracts_held_long_` (gated on `position_side_ == LONG`) | Sprint F |
| `strategy.max_contracts_held_short` | var | ✅ Runtime | `max_contracts_held_short_` (gated on `position_side_ == SHORT`) | Sprint F |
| `strategy.max_drawdown` | var | ✅ Runtime | `max_drawdown_` | |
| `strategy.max_drawdown_percent` | var | ✅ Runtime | `max_drawdown_percent()` | |
| `strategy.max_runup` | var | ✅ Runtime | `max_runup_` | |
| `strategy.max_runup_percent` | var | ✅ Runtime | `max_runup_percent()` | |
| `strategy.netprofit` | var | ✅ Runtime | `net_profit()` | |
| `strategy.netprofit_percent` | var | ✅ Runtime | `net_profit() / initial_capital_ * 100` | |
| `strategy.openprofit` | var | ✅ Runtime | `open_profit(price)` | |
| `strategy.openprofit_percent` | var | ✅ Runtime | `open_profit(close) / current_equity() * 100` with zero-guard | 2026-06-10: denominator is realized equity per Pine spec (was `initial_capital`) |
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
| `syminfo.country` | var | 🔧 Transpiler | Derived from `syminfo_.tickerid` prefix via `_pf_derive_country()` (NASDAQ→US, LSE→GB, TSE→JP, ...35 exchanges) | 2026-06-10: ISO 3166-1 alpha-2 (LSE/AQUIS→GB); EURONEXT and crypto-venue "GLOBAL" pseudo-codes removed — unknown prefix → na |
| `syminfo.currency` | var | ✅ Runtime | `syminfo_.currency` | |
| `syminfo.current_contract` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.description` | var | ✅ Runtime | `syminfo_.description` | |
| `syminfo.employees` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("employees")` — na until a feed injects | Runtime metadata map (`strategy_set_syminfo_metadata`); conditional-use warning |
| `syminfo.expiration_date` | var | ⏭️ Parse-and-skip | Returns `na<int64_t>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.industry` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.isin` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning emitted | Sprint G2; pineforge-data scope |
| `syminfo.main_tickerid` | var | 🔧 Transpiler | Derived from `syminfo_.tickerid` via `_pf_derive_main_tickerid()` (strips futures `N!` suffix) | Sprint G2 (audit rescue) |
| `syminfo.mincontract` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("mincontract")` — na until a feed injects; conditional-use warning emitted | Sprint G2 (audit fix) — was previously silently emitting 0; pineforge-data scope |
| `syminfo.minmove` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct, contrary to prior audit) |
| `syminfo.mintick` | var | ✅ Runtime | `syminfo_.mintick` | |
| `syminfo.pointvalue` | var | ✅ Runtime | `syminfo_.pointvalue` | |
| `syminfo.prefix` | var | 🔧 Transpiler | Derived: `_pf_derive_prefix(syminfo_.tickerid)` (text before `:`) | 2026-06-10: was `na<std::string>()`; now derived like `country`/`main_tickerid` |
| `syminfo.pricescale` | var | ⏭️ Parse-and-skip | Returns `na<double>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct) |
| `syminfo.recommendations_buy` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_buy")` — na until a feed injects | Runtime metadata map; conditional-use warning |
| `syminfo.recommendations_buy_strong` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_buy_strong")` | Runtime metadata map |
| `syminfo.recommendations_date` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_date")` | Runtime metadata map; returns `double` (Pine types `series int` — known drift, see master audit F17) |
| `syminfo.recommendations_hold` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_hold")` | Runtime metadata map |
| `syminfo.recommendations_sell` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_sell")` | Runtime metadata map |
| `syminfo.recommendations_sell_strong` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_sell_strong")` | Runtime metadata map |
| `syminfo.recommendations_total` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("recommendations_total")` | Runtime metadata map |
| `syminfo.root` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()` | Sprint G2 critical fix — was silently emitting 0 (field NOT in `SymInfo` struct) |
| `syminfo.sector` | var | ⏭️ Parse-and-skip | Returns `na<std::string>()`; conditional-use warning | Sprint G2; pineforge-data scope |
| `syminfo.session` | var | ✅ Runtime | `syminfo_.session` | |
| `syminfo.shareholders` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("shareholders")` | Runtime metadata map |
| `syminfo.shares_outstanding_float` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("shares_outstanding_float")` | Runtime metadata map |
| `syminfo.shares_outstanding_total` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("shares_outstanding_total")` | Runtime metadata map |
| `syminfo.target_price_average` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_average")` | Runtime metadata map |
| `syminfo.target_price_date` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_date")` | Runtime metadata map; returns `double` (Pine types `series int` — F17) |
| `syminfo.target_price_estimates` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_estimates")` | Runtime metadata map |
| `syminfo.target_price_high` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_high")` | Runtime metadata map |
| `syminfo.target_price_low` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_low")` | Runtime metadata map |
| `syminfo.target_price_median` | var | ⏭️ Parse-and-skip | `get_syminfo_metadata("target_price_median")` | Runtime metadata map |
| `syminfo.ticker` | var | ✅ Runtime | `syminfo_.ticker` | |
| `syminfo.tickerid` | var | ✅ Runtime | `syminfo_.tickerid` | |
| `syminfo.timezone` | var | ✅ Runtime | `syminfo_.timezone` | The engine's "UTC" (and an empty zone) reads "Etc/UTC", TradingView's spelling (codegen) |
| `syminfo.type` | var | ✅ Runtime | `syminfo_.type` | |
| `syminfo.volumetype` | var | ✅ Runtime | `syminfo_.volumetype` | |

### Variables — ta.* series

> 2026-06-10: a bare `ta.<name>` property read that cannot be bound to a registered call site (and any non-property `ta.<member>` read like `x = ta.rsi` without parentheses) now **rejects loudly** at codegen instead of silently emitting `std::string("<name>")`.

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ta.accdist` | var | ✅ Runtime | `ta::AccDist` class (`ta_extremes_volume.cpp`) | |
| `ta.iii` | var | ✅ Runtime | `ta::III` class (`ta_extremes_volume.cpp`) | |
| `ta.nvi` | var | ✅ Runtime | `ta::NVI` class (`ta_extremes_volume.cpp`) | |
| `ta.obv` | var | ✅ Runtime | `ta::OBV` class (`ta_extremes_volume.cpp`) | |
| `ta.pvi` | var | ✅ Runtime | `ta::PVI` class (`ta_extremes_volume.cpp`) | |
| `ta.pvt` | var | ✅ Runtime | `ta::PVT` class (`ta_extremes_volume.cpp`) | |
| `ta.tr` | var | ✅ Runtime | `ta::TR(handle_na=false)` class (`ta_volatility_trend.cpp`) | Property form (no args) |
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
| `alert.freq_all` | const | ⏭️ Parse-and-skip | Alert frequency; no alert runtime | 2026-06-10: free-expression reads outside `alert(...)` args reject loudly (`UNSUPPORTED_CONST_NAMESPACES`, context-aware) |
| `alert.freq_once_per_bar` | const | ⏭️ Parse-and-skip | Alert frequency | Same context-aware rejection |
| `alert.freq_once_per_bar_close` | const | ⏭️ Parse-and-skip | Alert frequency | Same context-aware rejection |

### Constants — backadjustment

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `backadjustment.inherit` | const | 🔧 Transpiler | Integer passthrough (2) to `request.security`; engine ignores | Sprint G2 |
| `backadjustment.off` | const | 🔧 Transpiler | Integer passthrough (0) | Sprint G2 |
| `backadjustment.on` | const | 🔧 Transpiler | Integer passthrough (1) | Sprint G2 |

### Constants — barmerge

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `barmerge.gaps_off` | const | ✅ Runtime | `gaps_on=false` in `SecurityEvalState` | 2026-06-10: only valid as `request.security` gaps/lookahead kwargs; free-expression reads reject loudly (`UNSUPPORTED_CONST_NAMESPACES`, context-aware) |
| `barmerge.gaps_on` | const | ✅ Runtime | `gaps_on=true` in `SecurityEvalState` | Same context-aware rejection |
| `barmerge.lookahead_off` | const | ✅ Runtime | `lookahead_on=false` in `SecurityEvalState` | Same context-aware rejection |
| `barmerge.lookahead_on` | const | ✅ Runtime | `lookahead_on=true` in `SecurityEvalState` | Lower-TF emulation rejects this; same context-aware rejection |

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
| `display.pine_screener` | const | ⏭️ Parse-and-skip | Display/rendering | 2026-06-10: distinct code `"6"` in `DISPLAY_MAP` (was falling through to `"0"` = `display.all`) |
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

> 2026-06-10: `extend.*`, `font.*`, `hline.*` (and `location/plot/scale/shape/text/xloc/yloc` below) are **context-aware rejected** — accepted as arguments to parse-and-skip visual calls (`plot`, `plotshape`, `hline`, `label.new`, `table.cell`, ...), the `strategy()` declaration, and `request.security` kwargs, but a free-expression read rejects loudly via `UNSUPPORTED_CONST_NAMESPACES` (previously: silent `std::string("<member>")` typed INT).

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `extend.both` | const | ⏭️ Parse-and-skip | Drawing style; no runtime | Context-aware reject outside visual calls |
| `extend.left` | const | ⏭️ Parse-and-skip | Drawing style | Same |
| `extend.none` | const | ⏭️ Parse-and-skip | Drawing style | Same |
| `extend.right` | const | ⏭️ Parse-and-skip | Drawing style | Same |
| `false` | const | 🔧 Transpiler | C++ `false` | |
| `font.family_default` | const | ⏭️ Parse-and-skip | Rendering const | Context-aware reject outside visual calls |
| `font.family_monospace` | const | ⏭️ Parse-and-skip | Rendering const | Same |
| `format.inherit` | const | 🔧 Transpiler | String constant for display | 2026-06-10: analyzer types all `format.*` reads STRING (matches `std::string` emission) |
| `format.mintick` | const | 🔧 Transpiler | Used by `str.tostring` / `pine_str_tostring` | ✅ runtime for mintick mode; typed STRING |
| `format.percent` | const | 🔧 Transpiler | Used by `str.tostring` | ✅ runtime for percent mode; typed STRING |
| `format.price` | const | 🔧 Transpiler | Display hint | Typed STRING |
| `format.volume` | const | 🔧 Transpiler | Used by `str.tostring` | ✅ runtime for volume mode; typed STRING |
| `hline.style_dashed` | const | ⏭️ Parse-and-skip | Rendering const | Context-aware reject outside visual calls |
| `hline.style_dotted` | const | ⏭️ Parse-and-skip | Rendering const | Same |
| `hline.style_solid` | const | ⏭️ Parse-and-skip | Rendering const | Same |

### Constants — label.style_* (22 entries)

All 22 `label.style_*` constants are **⏭️ Parse-and-skip** — drawing style constants; no runtime backing.

### Constants — line.style_* (6 entries)

All 6 `line.style_*` constants are **⏭️ Parse-and-skip** — drawing style constants; no runtime backing.

### Constants — location / math / order / plot / position / scale / session / shape / size / strategy

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `location.*` (5 entries) | const | ⏭️ Parse-and-skip | Plotshape placement; no runtime | Context-aware reject outside visual calls (2026-06-10) |
| `math.e` | const | 🔧 Transpiler | `M_E` from `<cmath>` | |
| `math.phi` | const | 🔧 Transpiler | `1.618033988...` inline | |
| `math.pi` | const | 🔧 Transpiler | `M_PI` from `<cmath>` | |
| `math.rphi` | const | 🔧 Transpiler | `0.618033988...` inline | |
| `order.ascending` | const | ✅ Runtime | Used in `PineMatrix::sort` and `array.sort` | |
| `order.descending` | const | ✅ Runtime | Used in `PineMatrix::sort` and `array.sort` | |
| `plot.linestyle_*` (3) | const | ⏭️ Parse-and-skip | Plotting style | Context-aware reject outside visual calls (2026-06-10) |
| `plot.style_*` (12) | const | ⏭️ Parse-and-skip | Plotting style | Same |
| `position.*` (9) | const | ⏭️ Parse-and-skip | Table/label position; rendering only | |
| `scale.*` (3) | const | ⏭️ Parse-and-skip | Rendering | Context-aware reject outside visual calls / `strategy()` decl (2026-06-10) |
| `session.extended` | const | 🔧 Transpiler | Session string constant | |
| `session.regular` | const | 🔧 Transpiler | Session string constant | |
| `settlement_as_close.on/off/inherit` | const | 🔧 Transpiler | Integer passthrough (1/0/2) to `request.security`; engine ignores | Sprint G2 |
| `shape.*` (12) | const | ⏭️ Parse-and-skip | Plotshape style | Context-aware reject outside visual calls (2026-06-10) |
| `size.*` (6) | const | ⏭️ Parse-and-skip | Label/table size; rendering | |
| `strategy.cash` | const | ✅ Runtime | `QtyType::CASH` enum | |
| `strategy.commission.cash_per_contract` | const | ✅ Runtime | `CommissionType::CASH_PER_CONTRACT` | |
| `strategy.commission.cash_per_order` | const | ✅ Runtime | `CommissionType::CASH_PER_ORDER` | |
| `strategy.commission.percent` | const | ✅ Runtime | `CommissionType::PERCENT` | |
| `strategy.direction.all` | const | ✅ Runtime | Integer `0` to `set_pine_risk_direction` | |
| `strategy.direction.long` | const | ✅ Runtime | Integer `1` to `set_pine_risk_direction` | |
| `strategy.direction.short` | const | ✅ Runtime | Integer `-1` to `set_pine_risk_direction` | |
| `strategy.fixed` | const | ✅ Runtime | `QtyType::FIXED` | |
| `strategy.long` | const | ✅ Runtime | `is_long=true` in order calls | |
| `strategy.oca.cancel` | const | ✅ Runtime | `oca_type=1` | |
| `strategy.oca.none` | const | ✅ Runtime | `oca_type=0` | |
| `strategy.oca.reduce` | const | ✅ Runtime | `oca_type=2` | |
| `strategy.percent_of_equity` | const | ✅ Runtime | `QtyType::PERCENT_OF_EQUITY` | |
| `strategy.short` | const | ✅ Runtime | `is_long=false` in order calls | |
| `text.*` (8) | const | ⏭️ Parse-and-skip | Text rendering; label/table only | Context-aware reject outside visual calls (2026-06-10) |
| `true` | const | 🔧 Transpiler | C++ `true` | |
| `xloc.*` (2) | const | ⏭️ Parse-and-skip | Drawing xloc mode | Context-aware reject outside visual calls (2026-06-10) |
| `yloc.*` (3) | const | ⏭️ Parse-and-skip | Drawing yloc mode | Same |

---

### Functions — alert / alertcondition

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `alert()` | fn | ⏭️ Parse-and-skip | Parsed; no live alert emission | |
| `alertcondition()` | fn | ⏭️ Parse-and-skip | Parsed; no live alert emission | |

### Functions — array.* (54 entries)

`array.*` functions are **🔧 Transpiler** — emitted against `std::vector<T>` by PineForge's transpiler. No runtime module. **Exceptions:** `array.new_line/_box/_label/_linefill` hold `drawing.hpp` handles; `array.new_color` and `array.new_table` are **❌ Unsupported** — the `SUPPORTED_ARRAY` whitelist in `support_checker.py` rejects them. Selected notes:

| Identifier | Notes |
|---|---|
| `array.new_bool/float/int/string()` | 🔧 Transpiler |
| `array.new_line/box/label/linefill()` | 🔧 Transpiler; `std::vector` of `drawing.hpp` handles |
| `array.new_color/table()` | ❌ Hard-reject (`SUPPORTED_ARRAY` whitelist) |
| `array.new<type>()` | 🔧 Transpiler generic form |
| `array.sort()` / `array.sort_indices()` | 🔧 Transpiler; `std::sort`; `sort` honors the `order` arg (2026-06-10) |
| `array.stdev()` / `array.variance()` | 🔧 Transpiler; honor the optional `biased` arg (population vs n−1 sample) (2026-06-10) |
| `array.join()` | 🔧 Transpiler; handles string elements as well as numeric (2026-06-10) |
| `array.copy()` / `array.slice()` | 🔧 Transpiler; functional form preserves the receiver's element type (was hardcoded `double`) (2026-06-10) |
| `array.size()` | 🔧 Transpiler; emits `(double)v.size()` — Pine types `series int` (known drift, deliberate) |
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
| `int()` | fn | 🔧 Transpiler | na-propagating lambda: `is_na(x) ? na<int>() : (int)x` | 2026-06-10: Pine `int(na)` → na (was NaN→0 collapse) |
| `string()` | fn | 🔧 Transpiler | `str.tostring` emission path | 2026-06-10: handles bool ("true"/"false"), string passthrough, and numeric (was `std::to_string` numerics-only) |

### Functions — box.* (27 entries)

`new`, the geometry getters and setters, `copy` and `delete` are **✅ Runtime** (the per-type arena in `drawing.hpp`); the visual setters (colour, style, width, text styling) are **⏭️ Parse-and-skip** with a warning.

### Functions — chart.point.* (5 entries)

`new`, `now`, `from_index`, `from_time` and `copy` are **✅ Runtime**: `ChartPoint` aggregates (`drawing.hpp`).

### Functions — color.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `color()` | fn | ⏭️ Parse-and-skip | Cosmetic cast (`COSMETIC_BARE_FUNCS` in `support_checker.py`): a warning and a default colour | |
| `color.b()` | fn | ✅ Runtime | `pine_color::b(c)` | |
| `color.from_gradient()` | fn | ⏭️ Parse-and-skip | Cosmetic (`COSMETIC_COLOR_FUNC` in `support_checker.py`): arguments evaluated, a warning and a default colour | Charting helper |
| `color.g()` | fn | ✅ Runtime | `pine_color::g(c)` | |
| `color.new()` | fn | ✅ Runtime | `pine_color::new_color(c, transp)` | |
| `color.r()` | fn | ✅ Runtime | `pine_color::r(c)` | |
| `color.rgb()` | fn | 🔧 Transpiler | ARGB assembly inline | |
| `color.t()` | fn | ✅ Runtime | `pine_color::t(c)` | |

### Functions — dayofmonth / dayofweek / hour / minute / month / second / weekofyear / year (function forms, `fn(time[, timezone])`)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `dayofmonth()` | fn | 🔧 Transpiler | `pine_<field>(ts, tz)` in `session_time.hpp` — the same engine helper as the bare-variable form, so the two cannot drift | 1-arg form defaults tz to `syminfo_.timezone` per Pine spec; optional 2-arg tz honored |
| `dayofweek()` | fn | 🔧 Transpiler | Same | |
| `hour()` | fn | 🔧 Transpiler | Same | |
| `minute()` | fn | 🔧 Transpiler | Same | |
| `month()` | fn | 🔧 Transpiler | Same | |
| `second()` | fn | 🔧 Transpiler | Same | |
| `weekofyear()` | fn | 🔧 Transpiler | Same | |
| `year()` | fn | 🔧 Transpiler | Same | |

### Functions — fill / fixnan / hline / indicator / library

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `fill()` | fn | ⏭️ Parse-and-skip | Drawing fill; no runtime | |
| `fixnan()` | fn | 🔧 Transpiler | Emitted inline: `is_na(x) ? prev : x` | |
| `hline()` | fn | ⏭️ Parse-and-skip | Rendering; no runtime | |
| `indicator()` | fn | ❌ Unsupported | Hard-reject in `support_checker` (`_visit_StrategyDecl`: only `strategy(...)` accepted) | Strategy-only engine |
| `library()` | fn | ❌ Unsupported | Library system not implemented | |

### Functions — footprint.* (9 entries)

All **❌ Unsupported** — hard-reject via `UNSUPPORTED_NAMESPACES` (`support_checker.py`): footprint requires tick-level data the engine does not consume. On codegen-oss main one member is read: `delta()` (`footprint.delta(fp)`) of a footprint that another symbol's `request.security` returns, from that symbol's pinned feed column; every other member is refused by name.

### Functions — input.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `input()` | fn | ✅ Runtime | `get_input_*()` typed getters on engine | |
| `input.bool()` | fn | ✅ Runtime | `get_input_bool()` | |
| `input.color()` | fn | ✅ Runtime | `get_input_int64()` with packed-ARGB defval (`0xAARRGGBB` overflows int32) | Defval must be `color.<const>` / `color.new(...)` / `color.rgb(...)` — others reject |
| `input.enum()` | fn | ✅ Runtime | `get_input_int()` with enum table | |
| `input.float()` | fn | ✅ Runtime | `get_input_double()` | |
| `input.int()` | fn | ✅ Runtime | `get_input_int()` | |
| `input.price()` | fn | ✅ Runtime | `get_input_double()` | |
| `input.session()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.source()` | fn | ✅ Runtime | `get_input_source("title", _src_<field>_)[0]` — engine returns the (optionally overridden) native source series | Defval must be a native chart series (open/high/low/close/volume/hl2/hlc3/ohlc4/hlcc4); others reject loudly |
| `input.string()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.symbol()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.text_area()` | fn | ✅ Runtime | `get_input_string()` | |
| `input.time()` | fn | ✅ Runtime | `get_input_int64()` | Pine v6 returns `series int` Unix-ms; int32 getter overflowed (fixed 2026-05-29, engine#22 + codegen#15) |
| `input.timeframe()` | fn | ✅ Runtime | `get_input_string()` | |

> Note: UI metadata (`group`, `tooltip`, `confirm`, `options`, `min/max/step`) has no runtime backing — all inputs arrive as strings in the injection map.

### Functions — label.* (20 entries)

`new`, the geometry getters and setters, `copy` and `delete` are **✅ Runtime** (the per-type arena in `drawing.hpp`); the visual setters (colour, style, width, text styling) are **⏭️ Parse-and-skip** with a warning.

### Functions — line.* (20 entries)

`new`, the geometry getters and setters, `copy` and `delete` are **✅ Runtime** (the per-type arena in `drawing.hpp`); the visual setters (colour, style, width, text styling) are **⏭️ Parse-and-skip** with a warning.

### Functions — linefill.* (5 entries)

`new`, the geometry getters and setters, `copy` and `delete` are **✅ Runtime** (the per-type arena in `drawing.hpp`); the visual setters (colour, style, width, text styling) are **⏭️ Parse-and-skip** with a warning.

### Functions — log.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `log.error()` | fn | ✅ Runtime | `pine_log_error()` in `log.hpp` | |
| `log.info()` | fn | ✅ Runtime | `pine_log_info()` in `log.hpp` | |
| `log.warning()` | fn | ✅ Runtime | `pine_log_warning()` in `log.hpp` | |

### Functions — map.* (11 entries)

End-to-end status is **✅ Runtime** within the transpiler's explicit supported
boundary. Generated strategies call `PineMap<K,V>` in `map.hpp`, including null
map IDs, alias-preserving assignment, explicit container copy, insertion
ordering, typed missing results, `put_all`, the 50,000-pair limit, pair loops,
and primitive-value snapshot/restore. Codegen propagates supported map types
through variables, UDT fields, parameters, returns, wrappers, and `na`, while
preserving once-only receiver/key/value evaluation. It fails closed for
map-bearing history, nested map-bearing matrices, incompatible inferred
specializations, and other paths whose rollback or type semantics are not yet
safe; ✅ here is not a claim that those rejected boundaries are supported.

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
| `math.round_to_mintick()` | fn | ✅ Runtime | `round_to_mintick()` on `BacktestEngine` | 2026-06-10: codegen now actually calls the guarded engine method (NaN / `mintick<=0` safe); previously inlined an unguarded `std::round` |
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
| `max_bars_back()` | fn | 🔧 Transpiler | Sizes the `Series<T>` ring buffers to the requested depth (`Series<T>(int max_len)`) | |
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
| `request.dividends()` | fn | ❌ Unsupported (codegen v0.10.4) | v0.10.4 rejects it at transpile; codegen-oss main reads TradingView's recorded answers by request key (`strategy_set_recorded_series`) and `na` when the value reaches only display sinks | No fundamentals feed |
| `request.earnings()` | fn | ❌ Unsupported (codegen v0.10.4) | v0.10.4 rejects it at transpile; codegen-oss main reads TradingView's recorded answers by request key (`strategy_set_recorded_series`) and `na` when the value reaches only display sinks | No fundamentals feed |
| `request.economic()` | fn | ❌ Unsupported | Rejected at transpile | No macro data feed |
| `request.financial()` | fn | ❌ Unsupported (codegen v0.10.4) | v0.10.4 rejects it at transpile; codegen-oss main reads TradingView's recorded answers by request key (`strategy_set_recorded_series`) and `na` when the value reaches only display sinks | No fundamentals feed |
| `request.footprint()` | fn | ❌ Unsupported (codegen v0.10.4) | On codegen-oss main: `na` when the value reaches only display sinks; `delta()` of a footprint that is another symbol's whole `request.security` expression reads that symbol's pinned feed column; otherwise the run stops where it is read | TradingView's delta depends on the chart range |
| `request.quandl()` | fn | ❌ Unsupported | Deprecated upstream; rejected | |
| `request.security()` | fn | ✅ Runtime | `SecurityEvalState` + full TF aggregation machinery | Same-symbol MTF + higher-TF aggregation; another symbol's installed bars (`strategy_set_symbol_feed`, XSYM-D), which codegen-oss main lowers onto (not in a release yet) |
| `request.security_lower_tf()` | fn | ✅ Runtime | Lower-TF emulation via `synthesize_lower_tf_bars` | Intraday same-symbol only |
| `request.seed()` | fn | ❌ Unsupported | TV-infrastructure-dependent; rejected | |
| `request.splits()` | fn | ❌ Unsupported (codegen v0.10.4) | v0.10.4 rejects it at transpile; codegen-oss main reads TradingView's recorded answers by request key (`strategy_set_recorded_series`) and `na` when the value reaches only display sinks | No corporate actions feed |

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
| `str.replace()` | fn | 🔧 Transpiler | `std::string::replace`; 4-arg `occurrence` form via inline lambda | 2026-06-10: honors the 4th `occurrence` arg (0-based per Pine; out-of-range/negative → original string) |
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
| `strategy.close()` | fn | ✅ Runtime | `strategy_close()` | `immediately = true` fills at the bar's close, and no `calc_on_order_fills` recalculation follows it (`tests/test_coof_immediate_close_final_tapes.cpp`) |
| `strategy.close_all()` | fn | ✅ Runtime | `strategy_close_all()` | As `strategy.close()` |
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
| `strategy.default_entry_qty()` | fn | ❓ Unknown | Codegen emits `calc_qty(fill_price)`, which engine `35db01c8` does not define | |
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
| `strategy.risk.allow_entry_in()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_direction` | |
| `strategy.risk.max_cons_loss_days()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_max_cons_loss_days` | |
| `strategy.risk.max_drawdown()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_max_drawdown` | |
| `strategy.risk.max_intraday_filled_orders()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_max_intraday_filled_orders` | |
| `strategy.risk.max_intraday_loss()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_max_intraday_loss` | |
| `strategy.risk.max_position_size()` | fn | ✅ Runtime | `PineStrategyHost::set_pine_risk_max_position_size` | |

### Functions — syminfo.prefix / syminfo.ticker (function forms)

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `syminfo.prefix()` | fn | ✅ Runtime | `syminfo_.prefix` via free helper | |
| `syminfo.ticker()` | fn | ✅ Runtime | `syminfo_.ticker` | |

### Functions — ta.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ta.alma()` | fn | ✅ Runtime | `ta::ALMA` class | `floor` is the fourth constructor argument, `ALMA(length, offset, sigma, floor)` (R5 lane TA1) |
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
| `ta.highest()` | fn | ✅ Runtime | `ta::Highest` class | bar-addressed ring (K = length + 1 slots by `bar_index`, per context — chart and each `request.security`): a call inside an `if` that does not run every bar keeps stale slots, skips never-written ones, na iff `bar_index < length - 1` (NYSE:F 1D pins 39/39, 30/30), read through a cached extremum with an implied bar — a call within `length` bars of the cached bar compares against the cache only and does not rescan the aliased slots (8 tapes, 696/696); every-bar callers unchanged |
| `ta.highestbars()` | fn | ✅ Runtime | `ta::HighestBars` class | same ring as `ta.highest`; offset = slot distance of the extremum |
| `ta.hma()` | fn | ✅ Runtime | `ta::HMA` class | |
| `ta.kc()` | fn | ✅ Runtime | `ta::KC` class | middle band = `ta::EMA` of the source on every bar (first bar included); bands na until the range EMA has a value; `useTrueRange` is the third constructor argument, `KC(length, mult, use_true_range)` (R5 lane TA1) |
| `ta.kcw()` | fn | ✅ Runtime | `ta::KCW` class | `useTrueRange` is the third constructor argument, `KCW(length, mult, use_true_range)` (R5 lane TA1) |
| `ta.linreg()` | fn | ✅ Runtime | `ta::Linreg` class | |
| `ta.lowest()` | fn | ✅ Runtime | `ta::Lowest` class | same bar-addressed ring and cached extremum as `ta.highest` (mirror; the 696/696 alias tapes are `ta.lowest(low, 10)`) |
| `ta.lowestbars()` | fn | ✅ Runtime | `ta::LowestBars` class | same ring as `ta.lowest`; offset = slot distance of the extremum |
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
| `ta.pivot_point_levels()` | fn | ✅ Runtime | Free function `ta::pivot_point_levels(method, high, low, close)`; with `anchor` / `developing`: `ta::PivotPointLevels` class | Free function: Traditional / Fibonacci / Classic / Camarilla equal `PivotPointLevels`; Woodie and DM keep a historical approximation, and `ta::pivot_point_levels(method, open, high, low, close, next_open)` computes every type as `PivotPointLevels` does (R5 lane B-ENGINE). `PivotPointLevels::compute(type, anchor, developing, open, high, low, close)` accumulates the anchored period, held or developing; Woodie + developing throws (R5 lane TA1) |
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
| `ta.vwap()` | fn | ✅ Runtime | `ta::VWAP` (single value) + `ta::VWAPBands` 3-tuple class (`VWAPBandsResult{vwap,upper,lower}`); any anchor other than the session day: `ta::AnchoredVWAP` + `ta::AnchoredVWAPBands` | Both overloads supported; Sprint B added 3-arg `(src, anchor, stdev_mult)` form with running variance. `AnchoredVWAP::compute(src, volume, anchor)` restarts on each bar the anchor is true and is na until the first (R5 lane TA1) |
| `ta.vwma()` | fn | ✅ Runtime | `ta::VWMA` class | |
| `ta.wma()` | fn | ✅ Runtime | `ta::WMA` class | |
| `ta.wpr()` | fn | ✅ Runtime | `ta::WPR` class | |

### Functions — table.* (20 entries)

All **⏭️ Parse-and-skip** — table drawing methods; no runtime backing.

### Functions — ticker.*

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `ticker.heikinashi()` | fn | ✅ Runtime | The chart's own symbol only: the Pine host applies the Heikin-Ashi candle inside the same-symbol `request.security` eval; another symbol's is refused | |
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
| `time()` | fn | ✅ Runtime | `pine_time(bar_ms, tf, session, tz, chart_tf)` in `session_time.hpp`; with `bars_back` / `timeframe_bars_back`, `PineStrategyHost::pine_time_offset` | Another bar's time reads the bars the chart's history holds, the session calendar past them (`tests/test_time_bars_back_tapes.cpp`); where the run holds TradingView's calendar for the symbol (`PineStrategyHost::set_symbol_calendar`), the chart bars ahead on an intraday or 1D chart, a timeframe's bars stepped either way and a day's open and close are the calendar's (`tests/test_symbol_calendar_tapes.cpp`) |
| `time_close()` | fn | ✅ Runtime | `pine_time_close(...)` in `session_time.hpp`; with `bars_back` / `timeframe_bars_back`, `PineStrategyHost::pine_time_offset` | As `time()` |
| `timeframe.change()` | fn | ✅ Runtime | `tf_change(prev_ms, curr_ms, tf)` in `timeframe.hpp` | |
| `timeframe.from_seconds()` | fn | ❌ Unsupported | Hard-reject via `NOT_YET_FUNC` (`support_checker.py`) | Codegen would have emitted `false` → wrong TF strings |
| `timeframe.in_seconds()` | fn | ✅ Runtime | `tf_to_seconds(tf)` | |
| `timestamp()` | fn | 🔧 Transpiler | Numeric form inline; `dateString` literals parsed to Unix-ms at transpile time | 2026-06-10: bad arities reject loudly (year/month/day required); `dateString` supports ISO-8601 + "DD MMM YYYY" forms, GMT+0 default; non-literal `dateString` rejected; tz-first overload handled |

### Functions — volume_row.* (8 entries)

All **❌ Unsupported** — hard-reject via `UNSUPPORTED_NAMESPACES` (`support_checker.py`), same reason as `footprint.*`.

### Keywords

| Identifier | Kind | Status | Backing | Notes |
|---|---|---|---|---|
| `and` | kw | 🔧 Transpiler | C++ `&&` | |
| `enum` | kw | 🔧 Transpiler | C++ enum + `pine_enum_str_at` | |
| `export` | kw | ❌ Unsupported | Library export; not implemented | 2026-06-10: a stray `export` identifier in a strategy script rejects loudly (`support_checker._visit_Identifier`) instead of dying downstream with a cryptic C++ error |
| `for` | kw | 🔧 Transpiler | C++ `for` | |
| `for...in` | kw | 🔧 Transpiler | Range-based for | |
| `if` | kw | 🔧 Transpiler | C++ `if` | |
| `import` | kw | 🔧 Transpiler | On codegen-oss main (not in a release yet): the library source handed to `transpile(..., libraries=...)` is inlined; a built-in namespace import naming only built-ins is a no-op | |
| `method` | kw | 🔧 Transpiler | UDT method generation | |
| `not` | kw | 🔧 Transpiler | C++ `!` | |
| `or` | kw | 🔧 Transpiler | C++ `||` | |
| `switch` | kw | 🔧 Transpiler | C++ `switch` | |
| `type` | kw | 🔧 Transpiler | UDT struct generation | |
| `var` | kw | 🔧 Transpiler | Persistent variable (static in on_bar) | |
| `varip` | kw | 🔧 Transpiler | On codegen-oss main (not in a release yet; v0.10.4 rejects it): kept like `var` on historical bars and left out of the `calc_on_order_fills` rollback | 2026-05-29 (codegen#13) made it a loud reject; 2026-09-28 (29bfd21) accepts it |
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
| `/` | op | 🔧 Transpiler | Always-float division `(double)a / (double)b` (Pine v6 breaking change) | |
| `/=` | op | 🔧 Transpiler | Lowers to always-float division, matching `/` | 2026-06-10: was raw C++ `/=` (integer division on int operands) |
| `%` | op | 🔧 Transpiler | `std::fmod((double)a, (double)b)` | |
| `%=` | op | 🔧 Transpiler | Lowers to `std::fmod`, matching `%` | 2026-06-10: was raw C++ `%=` (integer modulo; doesn't compile for doubles) |
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

The released codegen (v0.10.4) rejects every call below. On pineforge-codegen main (not in a release yet) `request.financial`, `dividends`, `earnings` and `splits` read TradingView's recorded answers by request key instead (`strategy_set_recorded_series`), and read `na` when their value reaches only plots and alerts.

- `request.financial(symbol, field, period)` — fundamental data fetch. No external data feed; transpiler rejects.
- `request.dividends(ticker, field, gaps, lookahead, ignore_startbar, currency)` — corporate action data. Rejected.
- `request.earnings(ticker, field, gaps, lookahead, ignore_startbar, currency)` — earnings data. Rejected.
- `request.splits(ticker, field, gaps, lookahead, ignore_startbar)` — corporate action data. Rejected.
- `request.economic(country_code, field, ...)` — macro economic data. Rejected.
- `request.currency_rate(from, to, ignore_startbar)` — FX rate feed. Rejected.
- `request.seed(source, symbol, expression)` — TV user-published time series. Rejected (infrastructure-dependent).
- `request.quandl(ticker, gaps, index, ignore_startbar)` — deprecated upstream. Rejected.

### Analyst and fundamental syminfo fields (na until a feed injects)

`syminfo.recommendations_*`, `syminfo.target_price_*`, `syminfo.employees`, `syminfo.shareholders`, `syminfo.shares_outstanding_*` route through the engine's runtime metadata map (`get_syminfo_metadata(key)` — na until a value is injected via `strategy_set_syminfo_metadata`); `syminfo.sector`, `syminfo.industry`, `syminfo.isin`, `syminfo.expiration_date`, `syminfo.current_contract`, `syminfo.mincontract`, `syminfo.root`, `syminfo.pricescale`, `syminfo.minmove` emit `na<T>()` literals. Strategies that gate logic on **any** of these via `if` / `?:` get a transpile-time **warning** — since 2026-06-10 the warning set (`_SYMINFO_SILENT_GAP_FIELDS`, 26 fields) is derived from the emission table, so it can no longer drift. `syminfo.country`, `syminfo.main_tickerid`, and `syminfo.prefix` are derived from `syminfo.tickerid` at codegen-time (no external data needed). Long-term: pineforge-data integration will populate the metadata map per symbol.

### Dividend / earnings / splits variables (hard reject)

`dividends.*`, `earnings.*`, `splits.*` variable reads **reject loudly at transpile** via `UNSUPPORTED_NAMESPACE_VARS` (2026-05-29) — no external fundamentals feed. They are NOT runtime na-returns; a strategy using them does not compile.

### Realtime-only variables (always na or false)

- `ask` / `bid` — live order book; no feed. Rejected at transpile.
- `timenow` — no live clock: it reads the current bar's `time`, with a warning.
- `chart.left_visible_bar_time` / `chart.right_visible_bar_time` — viewport concept; a warning and `0` (`COSMETIC_MEMBERS`); `chart.bg_color` / `chart.fg_color` read a default colour.

### varip (keyword)

pineforge-codegen main accepts `varip` (not in a release yet; v0.10.4 still rejects it at transpile, the 2026-05-29 codegen#13 behaviour): a historical bar executes once, so a `varip` keeps its value like `var`, and it is left out of the `calc_on_order_fills` rollback.

### Library system

`library()` scripts and a stray `export` in a strategy are refused. The released codegen (v0.10.4) refuses every `import`; pineforge-codegen main inlines an imported library whose source the caller hands `transpile(..., libraries=...)`.

### Drawing / plotting (parse-and-skip — silent)

`plot`, `plotshape`, `plotchar`, `plotcandle`, `plotbar`, `plotarrow`, `fill`, `hline`, `bgcolor`, `barcolor`, all `table.*` / `polyline.*` methods and the visual setters of `label.*` / `line.*` / `box.*` / `linefill.*` — these compile silently; no visual output is emitted. **Strategies that only use these for display will run correctly for backtesting purposes.** The geometry of `label.new()` / `line.new()` / `box.new()` / `linefill.new()` objects is real state (`drawing.hpp`): a strategy that stores and reads it back reads what it wrote; only their visual setters are no-ops.

### Alert functions (parse-and-skip — silent)

`alert()` / `alertcondition()` — compiled silently; no alert is ever sent. PineForge is a batch engine with no live alert capability.

### ta.vwap 3-tuple form

**Sprint B update — RESOLVED.** `ta.vwap(source, anchor, stdev_mult)` 3-tuple form `[vwap, upper_band, lower_band]` is now fully supported. The engine's `ta::VWAP` class was extended with running variance (`cum_pv_sq_` field) and a `VWAPBandsResult{vwap,upper,lower}` struct (modelled on `ta::BBResult`). Codegen dispatches the 3-arg overload via `signatures.py` dual-overload (matching the `ta.highest`/`lowest` pattern) and routes through `vwap_bands` in `analyzer/call_handlers.py`. Verified bit-exact against TV at 916/916 (breakout 1σ) and 225/225 (mean-reversion 2σ) corpus probes.

---

## Sprint changes (2026-05-17)

Pine v6 HIGH+MEDIUM sprint added ~50 identifiers to the supported surface
(✅ Runtime + 🔧 Transpiler) and reclassified ~20 from ❓/❌ to explicit
buckets. Full final-state release notes: [v0.5.0 release](https://github.com/pineforge-4pass/pineforge-engine/releases/tag/v0.5.0).

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
| ~~`varip` (1 — now warn-then-emit-as-var)~~ **superseded 2026-05-29: hard-reject** (see Keywords table) | C |
| `ticker.inherit`, `ticker.standard` (2 — passthrough for same-symbol use) | G2 |

### ⏭️ Parse-and-skip additions (12) — most are silent-gap remediation

| Identifiers | Group | Note |
|---|---|---|
| `syminfo.prefix`, `syminfo.root`, `syminfo.pricescale`, `syminfo.minmove` (4) | G2 | **Critical fix** — were silently emitting `0` (fields NOT in `SymInfo` struct, contrary to prior audit); now `na<T>()`. *Superseded for `syminfo.prefix` (2026-06-10): now derived via `_pf_derive_prefix(syminfo_.tickerid)` — see syminfo table.* |
| `syminfo.mincontract`, `syminfo.current_contract`, `syminfo.expiration_date`, `syminfo.isin`, `syminfo.sector`, `syminfo.industry` (6) | G2 | Was ❓; now ⏭️ na-accept; conditional-use warning |
| ~14 LOW-tier `syminfo.recommendations_*`, `syminfo.target_price_*`, `syminfo.employees`, etc. moved from ❌ to ⏭️ | G2 | *Updated since:* these now route through the runtime metadata map (`get_syminfo_metadata`, na until injected) rather than emitting bare `na<double>()` literals — see syminfo table |

### ❌ Unsupported (additions: 9; all explicit ticker.* hard-reject)

| Identifiers | Group |
|---|---|
| `ticker.heikinashi/renko/kagi/linebreak/pointfigure` (5) — chart-type modifiers, engine doesn't synthesize alt bars | G2 |
| `ticker.new`, `ticker.modify` (2) — cross-symbol construction not supported | G2 |
| `request.footprint`, `footprint.*` (counted in unchanged ❌ but now explicitly noted) | — |

(Previously these were blanket-rejected via namespace; sprint G2 made per-function rejection explicit with clearer error message.)

### Engine bug filed during sprint

- **GitHub Issue [#16](https://github.com/pineforge-4pass/pineforge-engine/issues/16):** `max_intraday_filled_orders` cap-day boundary uses `chart_timezone` instead of exchange `syminfo.timezone`. Workaround shipped: validator `engine_chart_timezone` override key (per-probe). The issue was closed on 2026-05-24.

### Validator workflow improvements (the maintainers' private validator)

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
