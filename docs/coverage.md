# PineScript v6 Runtime Coverage

> **What this page is.** A complete map of the Pine v6 surface area that
> `libpineforge.a` actually implements: which features have a dedicated
> runtime class or function, which features are deliberately left to the
> consuming compiler, and which features are not supported anywhere in
> PineForge today.
>
> **Audience.** Anyone using PineForge as a backend — building a custom
> Pine-to-C++ transpiler against this runtime, integrating PineForge into
> a strategy harness, or auditing what is *actually* covered before
> trusting the parity claim. Source-of-truth files: the headers under
> `[include/pineforge/](../include/pineforge/)` and the implementations
> under `[src/](../src/)`.
>
> **Two layers of "supported".** PineForge as a whole = (a) this runtime
>
> - (b) PineForge's closed-source PineScript-to-C++transpiler. Some Pine
> surface (arrays, maps, UDTs, most scalar `math.`* calls) has no
> dedicated runtime class because the transpiler emits the implementation
> inline using the C++ standard library or generated structs. Those are
> still fully supported in PineForge end-to-end; they just don't appear
> as a runtime module here. Where this distinction matters, the
> "no runtime module — Pine surface still supported" bucket below calls
> it out.
>
> **Out of scope.** Visual / charting / alert APIs are not implemented
> by this runtime regardless of consumer (PineForge is an offline
> backtesting engine, not a renderer).

## Coverage summary


| Category                    | Runtime status                                                             | What `libpineforge.a` owns                                                                                                                                                                                                                                                                                                                                          |
| --------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Engine / strategy lifecycle | Supported                                                                  | `BacktestEngine`, three `run(...)` overloads, bar loop, `on_bar(...)` hook, `ReportC` / `SecurityDiagC` reporting.                                                                                                                                                                                                                                                  |
| Strategy orders             | Supported                                                                  | `strategy_entry / order / exit / close / close_all / cancel / cancel_all` with OHLC-path fill resolution, OCA, pyramiding, slippage, commissions, margin gates, partial / FIFO-vs-ANY closes, trailing stops, and TV deferred-flip carry handling.                                                                                                                  |
| Strategy state / accessors  | Supported                                                                  | Position state, equity / drawdown / runup tracking, win / loss counts, full closed- and open-trade accessor methods, intraday fill counter.                                                                                                                                                                                                                         |
| Strategy risk               | Partial                                                                    | Runtime fields cover position-size cap, drawdown cap (abs / %), intraday-loss cap (abs / %), consecutive losing days, and direction allow-list.                                                                                                                                                                                                                     |
| Inputs                      | Value support only                                                         | `unordered_map<string,string>` injection plus typed getters (`get_input_`*). UI metadata is the consumer's problem.                                                                                                                                                                                                                                                 |
| `ta.`*                      | Broad runtime support                                                      | 59 official Pine v6 `ta.`* functions plus 8 official `ta.`* series variables backed by stateful runtime classes, and a free `pivot_point_levels(...)`. Stateful classes expose both `compute(...)` (advance state) and `recompute(...)` (re-run on the same bar without permanently advancing history).                                                             |
| `math.`*                    | Narrow runtime backing                                                     | Runtime owns only deterministic `pine_random(...)` and rolling `math::Sum`; everything else is left to consumer-emitted code.                                                                                                                                                                                                                                       |
| `str.`*                     | Narrow runtime backing                                                     | Runtime owns `pine_str_format`, `pine_str_format_time`, `pine_str_match`, `pine_str_split`, `pine_str_tostring`.                                                                                                                                                                                                                                                    |
| `request.security()`        | Partial                                                                    | Runtime owns the security state machine, ratio / calendar aggregation, lookahead / gaps semantics, lower-TF emulation, and per-security diagnostics.                                                                                                                                                                                                                |
| Bar magnifier               | Supported                                                                  | OHLC-path sampling with 6 distribution modes plus optional volume-weighted sample density.                                                                                                                                                                                                                                                                          |
| Time / session / timezone   | Supported                                                                  | `pine_time` / `pine_time_close` with session filtering and a mutex-guarded `pine_tz::ScopedTimezone`.                                                                                                                                                                                                                                                               |
| Timeframe parsing           | Supported                                                                  | `tf_to_seconds`, `tf_ratio`, `tf_change`, `detect_timeframe`, calendar boundary detection, `TimeframeAggregator` (passthrough / ratio / calendar).                                                                                                                                                                                                                  |
| Numeric matrices            | Supported                                                                  | `PineMatrix` over `Eigen::MatrixXd` — construction, access, transforms, linear algebra, predicates.                                                                                                                                                                                                                                                                 |
| Series history              | Supported                                                                  | `Series<T>` ring buffer with Pine `[k]` semantics.                                                                                                                                                                                                                                                                                                                  |
| Color                       | Supported                                                                  | `pine_color` constants plus `new_color`, `r`, `g`, `b`, `t` helpers.                                                                                                                                                                                                                                                                                                |
| `na` / `is_na`              | Supported                                                                  | Generic `na<T>()` and `is_na(...)` for double / integer / bool.                                                                                                                                                                                                                                                                                                     |
| Logging / runtime errors    | Supported                                                                  | `pine_log_info / warning / error`, `pine_runtime_error` (throws).                                                                                                                                                                                                                                                                                                   |
| Arrays / maps / UDTs        | **No runtime module** (Pine surface still supported via consumer compiler) | The runtime ships no `array.hpp` / `map.hpp` / UDT module — its only generic value containers are `Series<T>` (history) and `PineMatrix` (numeric matrices). Pine arrays / maps / UDTs themselves work in PineForge: PineForge's transpiler emits them as `std::vector<T>`, `std::unordered_map<K,V>`, and generated C++ structs against this runtime's primitives. |
| Drawing / plotting / alerts | **No runtime module**                                                      | No charting / drawing / alert types exist in the runtime. PineForge's transpiler parses-and-skips these so the strategy still compiles and runs, but no visual side-effects are emitted.                                                                                                                                                                            |


## Public C ABI

`<pineforge/pineforge.h>` is the **single canonical consumer header**.
Every compiled PineForge strategy `.so` exports exactly the 10 symbols
declared there:


| Symbol                                   | Role                                         |
| ---------------------------------------- | -------------------------------------------- |
| `strategy_create`                        | Allocate a strategy instance                 |
| `strategy_free`                          | Release the instance                         |
| `run_backtest`                           | Run with auto-detected timeframe             |
| `run_backtest_full`                      | Run with timeframe + magnifier configuration |
| `report_free`                            | Free arrays inside a filled `pf_report_t`    |
| `strategy_set_input`                     | Override a Pine `input.*()` value            |
| `strategy_set_override`                  | Override a `strategy(...)` declaration param |
| `strategy_set_magnifier_volume_weighted` | Toggle volume-weighted magnifier             |
| `strategy_set_trace_enabled`             | Toggle per-bar trace recording               |
| `pf_version_get`                         | Runtime version                              |


POD types (`pf_bar_t`, `pf_trade_t`, `pf_report_t`, `pf_security_diag_t`,
`pf_trace_entry_t`, `pf_version_t`) and the `pf_magnifier_distribution_t`
enum complete the surface. **Stability:** within the same
`PINEFORGE_VERSION_MAJOR`, struct layouts and `extern "C"` signatures are
append-only. New fields may be appended; existing fields are never
reordered, removed, or retyped. New functions may be added; existing
functions are never removed or signature-changed. Compile-time
`static_assert`s in `src/c_abi.cpp` pin the layouts against drift.

The C++ headers (`<pineforge/engine.hpp>`, `<pineforge/ta.hpp>`, …) are
*internal* implementation surface — used by PineForge's transpiler, not
part of the stability guarantee, and not recommended for external
consumption.

## Runtime modules — file layout

Headers live under `[include/pineforge/](../include/pineforge/)` and
implementations under `[src/](../src/)`. Several large concerns are
split across multiple `.cpp` files (declarations stay in the matching
single `.hpp`):


| Module             | Header                   | Source                                                                                                                                                                                                                                   | Pine-facing role                                                                                                                                              |
| ------------------ | ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Public C ABI       | `pineforge.h`            | `c_abi.cpp` (+ layout `static_assert`s)                                                                                                                                                                                                  | The 10 documented C symbols every compiled strategy `.so` exports.                                                                                            |
| Engine             | `engine.hpp`             | `engine_run.cpp`, `engine_orders.cpp`, `engine_fills.cpp`, `engine_path_resolve.cpp`, `engine_strategy_commands.cpp`, `engine_trade_accessors.cpp`, `engine_security.cpp`, `engine_lower_tf.cpp`, `engine_risk.cpp`, `engine_report.cpp` | Strategy lifecycle, orders, fills, risk gating, reports, inputs / syminfo, magnifier loop, TF aggregation, `request.security` plumbing.                       |
| Engine internals   | `engine_internal.hpp`    | (private cross-TU header)                                                                                                                                                                                                                | `pineforge::internal::`* types and helpers shared between engine `.cpp` partitions; not part of the public ABI.                                               |
| Technical analysis | `ta.hpp`                 | `ta_moving_averages.cpp`, `ta_oscillators.cpp`, `ta_volatility_trend.cpp`, `ta_extremes_volume.cpp`, `ta_misc.cpp`                                                                                                                       | Official `ta.`* functions and series variables backed by stateful runtime classes with `compute` / `recompute`, plus `pivot_point_levels(...)` free function. |
| Math               | `math.hpp`               | `math.cpp`                                                                                                                                                                                                                               | Inline `pine_random(...)` PRNG and rolling `math::Sum` class.                                                                                                 |
| Strings            | `str_utils.hpp`          | `str_utils.cpp`                                                                                                                                                                                                                          | Format, format-time, regex match, split, and numeric-to-string helpers.                                                                                       |
| Timeframe          | `timeframe.hpp`          | `timeframe.cpp`                                                                                                                                                                                                                          | TF string parsing, ratio computation, calendar detection, `TimeframeAggregator`.                                                                              |
| Session / time     | `session_time.hpp`       | `session_time.cpp`                                                                                                                                                                                                                       | `pine_time(...)`, `pine_time_close(...)` with session and timezone gating.                                                                                    |
| Timezone           | (private) `timezone.hpp` | `timezone.cpp`                                                                                                                                                                                                                           | `pine_tz::ScopedTimezone` — mutex-guarded `TZ` env-var swap for thread-safe formatting. Internal; not in public include path.                                 |
| Bar magnifier      | `magnifier.hpp`          | `magnifier.cpp`                                                                                                                                                                                                                          | OHLC price-path sampling with six distribution modes; optional volume-weighted sample density.                                                                |
| Matrices           | `matrix.hpp`             | `matrix.cpp`                                                                                                                                                                                                                             | Eigen-backed `PineMatrix`.                                                                                                                                    |
| Series history     | `series.hpp`             | header-only                                                                                                                                                                                                                              | Generic `Series<T>` deque with `push` / `update` / `[k]` indexing.                                                                                            |
| `na`               | `na.hpp`                 | header-only                                                                                                                                                                                                                              | `na<T>()` generators and `is_na(...)` checks.                                                                                                                 |
| Bar struct         | `bar.hpp`                | header-only                                                                                                                                                                                                                              | `struct Bar { double open, high, low, close, volume; int64_t timestamp; };` (Unix milliseconds).                                                              |
| Color              | `color.hpp`              | header-only                                                                                                                                                                                                                              | 17 named ARGB constants plus `new_color`, `r`, `g`, `b`, `t`.                                                                                                 |
| Logging            | `log.hpp`                | header-only                                                                                                                                                                                                                              | `pine_log_info / warning / error` (stderr) and `pine_runtime_error` (throws `std::runtime_error`).                                                            |


## Engine lifecycle

`BacktestEngine` is an abstract base; the consumer compiler emits a
strategy class that derives from it and implements `on_bar(const Bar&)`.
Three `run(...)` overloads are exposed:

```cpp
void run(const Bar* bars, int n);

void run(const Bar* input_bars, int n_input,
         const std::string& input_tf,
         const std::string& script_tf,
         bool bar_magnifier = false,
         int magnifier_samples = 4,
         MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

void run(const Bar* input_bars, int n_input,
         const std::string& input_tf,
         const std::string& script_tf,
         const std::unordered_map<std::string, std::string>& inputs,
         const SymInfo& syminfo,
         const StrategyOverrides* overrides = nullptr,
         bool bar_magnifier = false,
         int magnifier_samples = 4,
         MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);
```

The TF-aware overload auto-detects `input_tf` from bar timestamps when
empty (via `detect_timeframe`) and defaults `script_tf` to `input_tf`.
The full overload additionally injects `SymInfo`, the input map, and a
`StrategyOverrides` struct (NaN / `-1` mean "leave default").

`StrategyOverrides` only carries a fixed set of override fields:
`initial_capital`, `commission_value`, `default_qty_value`, `pyramiding`,
`slippage`, `commission_type`, `default_qty_type`, `process_orders_on_close`,
`close_entries_rule`. Anything else (currency, margin, risk thresholds,
etc.) must be set by the generated subclass — there is no runtime entry
point for it.

Per-input runtime overrides are written via `set_input(key, value)` /
`clear_inputs()` on `BacktestEngine` before `run(...)`. Magnifier sample
density can be flipped to volume-weighted via
`set_magnifier_volume_weighted(bool)`.

## Strategy orders and state

### Order entry points


| Method                                                                                                      | Notes                                                                                                                                                                                                                 |
| ----------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `strategy_entry(id, is_long, limit, stop, qty, comment, oca_name, oca_type, qty_type)`                      | Replaces an existing pending order with the same `id`. Plain market entry under `process_orders_on_close=true` fills immediately at bar close so `position_avg_price` is correct for follow-up `strategy_exit` calls. |
| `strategy_order(id, is_long, qty, limit, stop, oca_name, oca_type)`                                         | "Raw" pending order. When direction opposes the open position the order is treated as exit-style for fill resolution.                                                                                                 |
| `strategy_exit(id, from_entry, limit, stop, trail_points, trail_offset, trail_price, qty_percent, comment)` | Reserves a slice of the open position; partial exits with the same `id` are one-shot per live position.                                                                                                               |
| `strategy_close(id, comment, qty, qty_percent, immediately)`                                                | FIFO close by entry id (or all when `id` is empty). Honours `close_entries_rule_any_` for ANY-mode partial close. `immediately` bypasses pending-order resolution.                                                    |
| `strategy_close_all()`                                                                                      | Convenience wrapper for `strategy_close("")`.                                                                                                                                                                         |
| `strategy_cancel(id)` / `strategy_cancel_all()`                                                             | Drops pending orders by id or globally.                                                                                                                                                                               |


Pending orders are resolved on every `process_pending_orders(bar)` call,
which walks a 4-waypoint OHLC path (`O → H → L → C` or `O → L → H → C`
depending on open proximity to high vs low). The runtime resolves stop
/ limit priority, gap fills, opposing-stop arbitration, OCA siblings,
and trail levels along that path. `slippage_` (in ticks) and
`syminfo_mintick_` round all fill prices; stop entries use directional
mintick snapping (long stops up, short stops down) to match TradingView.

Priced `strategy.entry` orders also track TradingView's deferred-flip
carry rule. When an opposite priced entry is placed while a position is
open, then fires later from flat after a `strategy.close` /
`strategy.close_all`, the runtime opens `qty + carried_position_qty`.
Source order inside a single `on_bar(...)` matters: close calls that
appear before the entry reduce the captured carry for that entry.

`strategy_exit` accepts price params (`profit`, `loss`, `limit`, `stop`,
`trail_*`); the runtime's exit method itself does not enforce that at
least one is set — that policy lives outside the runtime.

### Position-sizing and commission

Quantity sizing is governed by `default_qty_type_`
(enum `QtyType { FIXED, PERCENT_OF_EQUITY, CASH }`) and
`default_qty_value_`. Commission is `commission_type_`
(enum `CommissionType { PERCENT, CASH_PER_ORDER, CASH_PER_CONTRACT }`)
and `commission_value_`. Both are per-trade; there is no separate
runtime entry point for `strategy.default_entry_qty` — the value is
read directly from `default_qty_value_`.

Margin checks use `margin_long_` / `margin_short_` percentages from the
generated subclass (`100` = no leverage). If the implied required
capital for a flat entry or pyramid add exceeds current equity, the fill
is silently rejected, matching TradingView's strategy engine behaviour.

### Risk

`BacktestEngine` tracks six risk fields and gates entries through
`check_risk_allow_entry(is_long)` and `update_risk_state()`:


| Field                                                 | Effect                                                                                                        |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `risk_direction_` (`BOTH`, `LONG_ONLY`, `SHORT_ONLY`) | Block entries against the allowed direction.                                                                  |
| `risk_max_position_size_`                             | Block new entries when current `position_qty_` ≥ cap.                                                         |
| `risk_max_drawdown_` (+ `_is_pct_`)                   | Halt strategy when peak-to-trough drawdown crosses the cap (absolute $ or % of peak equity).                  |
| `risk_max_intraday_loss_` (+ `_is_pct_`)              | Halt strategy when running intraday P&L crosses the cap. Day boundary uses month / day-of-month, not session. |
| `risk_max_cons_loss_days_`                            | Halt strategy after N consecutive losing days.                                                                |
| `max_intraday_filled_orders_`                         | Skip pending fills past the per-day cap; counter resets on a new day-of-year.                                 |


Risk halt is one-way: once `risk_halted_` is set, no new entries are
accepted for the remainder of the run. None of these fields are exposed
via `StrategyOverrides`; they must be set by the generated subclass.

### Trade accessors

`strategy.closedtrades.*` accessors are wired (defined inline on `BacktestEngine`):

```
profit, profit_percent, commission, direction,
entry_bar_index, exit_bar_index,
entry_comment, exit_comment, entry_id, exit_id,
entry_price, exit_price, entry_time, exit_time,
size, max_runup, max_runup_percent, max_drawdown, max_drawdown_percent
```

`strategy.opentrades.*` accessors mirror the closed set minus the four
`exit_*` fields (exits do not exist for an open trade):

```
profit, profit_percent, commission, direction,
entry_bar_index, entry_comment, entry_id, entry_price, entry_time,
size, max_runup, max_runup_percent, max_drawdown, max_drawdown_percent
```

Aggregate strategy state methods are also defined on the engine:
`net_profit / gross_profit / gross_loss` (and `_percent` variants),
`avg_trade / avg_winning_trade / avg_losing_trade` (and `_percent`),
`count_wintrades / count_losstrades`, `current_equity`,
`open_profit(price)`, `open_trades_capital_held`, and
`signed_position_size`. `margin_liquidation_price()` always returns
`na<double>()`.

### Bar metadata helpers

`BacktestEngine::_decompose_bar_time()` decomposes
`current_bar_.timestamp` (UTC) into
`{ year, month, dayofmonth, hour, minute, second, dayofweek, weekofyear }`
and individual scalar accessors (`_bar_year()`, `_bar_hour()`, …) are
exposed for the consumer to read. The runtime stores a single `int64_t`
timestamp per bar — there is no separate close timestamp at runtime, so
any semantic distinction between `time` and `time_close` as bar
variables must be reconstructed from `tf_to_seconds(...)` (which
`pine_time_close` does for explicit calls).

`barstate` flags tracked on the engine:

- `is_first_tick_` — first sample within the script bar (true under non-magnifier mode).
- `is_last_tick_` — last sample within the script bar.
- `barstate_islast_` — last script bar in the run.

`barstate.isrealtime`, `barstate.ishistory`, and live-tick semantics
have no runtime backing.

## Technical Analysis (`namespace ta`)

Every TA class exposes both `compute(...)` (advance state, push history)
and `recompute(...)` (re-run on the same bar — used by the magnifier
and security intrabar paths so a TA's permanent state is not disturbed).
State is owned per instance; the consumer compiler allocates one
instance per call site.

### Tuple-returning TA classes


| Class        | Result struct      | Fields                                  |
| ------------ | ------------------ | --------------------------------------- |
| `MACD`       | `MACDResult`       | `macd_line`, `signal_line`, `histogram` |
| `BB`         | `BBResult`         | `middle`, `upper`, `lower`              |
| `KC`         | `KCResult`         | `middle`, `upper`, `lower`              |
| `Supertrend` | `SupertrendResult` | `value`, `direction`                    |
| `DMI`        | `DMIResult`        | `diplus`, `diminus`, `adx`              |


`Stoch::compute(src, high, low)` returns Pine v6's official single
stochastic value. `%K / %D` smoothing is explicit Pine code, e.g. assign
the result to `k` and compute `d = ta.sma(k, length)`.

### Single-value TA classes

Moving averages and smoothing (`src/ta_moving_averages.cpp`): `SMA`,
`EMA`, `RMA`, `WMA`, `HMA`, `VWMA`, `ALMA(length, offset=0.85, sigma=6.0)`,
`SWMA` (period-4 symmetric weights).

Oscillators / momentum (`src/ta_oscillators.cpp`): `RSI`, `Stoch`,
`CCI`, `MFI`, `Mom`, `ROC`, `CMO`, `TSI(short_length, long_length)`,
`WPR`, `COG`, `TR`, `ATR`, `RCI`.

Bands / channels / widths (`src/ta_volatility_trend.cpp`): `BB`, `KC`,
`BBW`, `KCW`.

Trend / pivots (`src/ta_volatility_trend.cpp`): `Supertrend(factor, atr_period)`,
`DMI(di_length, adx_smoothing)`, `SAR(start, increment, maximum)`,
`PivotHigh(left, right)`, `PivotLow(left, right)`.

Cross / state machines (`src/ta_misc.cpp`): `Crossover`, `Crossunder`,
`Cross`, `Rising(length)`, `Falling(length)`, `BarsSince`,
`ValueWhen(max_occurrence=1)`, `Change(max_length=1)`.

Statistical / windowed (`src/ta_misc.cpp`): `StdDev`, `Variance`,
`Median`, `Mode`, `Range`, `Dev` (mean absolute deviation), `Highest`,
`Lowest`, `HighestBars`, `LowestBars`, `PercentRank`,
`PercentileNearestRank`, `PercentileLinearInterpolation`, `Correlation`.

Volume indicators (`src/ta_extremes_volume.cpp`): official `ta.vwap(...)`
is a function backed by `VWAP`; official `ta.obv`, `ta.accdist`,
`ta.nvi`, `ta.pvi`, `ta.pvt`, `ta.wad`, `ta.wvad`, and `ta.iii` are
series variables backed by `OBV`, `AccDist`, `NVI`, `PVI`, `PVT`,
`WAD`, `WVAD`, and `III`. Parenthesized call forms such as
`ta.obv()` are rejected by PineForge's support checker because they are
not Pine v6 functions.

Cumulative / chart-extreme (`src/ta_extremes_volume.cpp`): `Cum`,
`AllTimeMax`, `AllTimeMin`.

Linear regression (`src/ta_misc.cpp`): `Linreg(length).compute(src, offset)`.

### `ta::TR` first-bar behaviour

`TR(bool handle_na=false).compute(high, low, close)` matches Pine v6's
`ta.tr(handle_na)` split. With the default `handle_na=false`, the first
bar returns `na`; with `handle_na=true`, the first bar falls back to
`high - low`. The property form `ta.tr` maps to the default form.

### `pivot_point_levels(method, high, low, close)`

Free function in `namespace ta`. The runtime returns Pine v6's
documented 11-slot order:
`P, R1, S1, R2, S2, R3, S3, R4, S4, R5, S5`. Levels absent from the
selected method are `na<double>()`. Current runtime inputs are
`method, high, low, close`; the official Pine `anchor` / `developing`
parameters are handled by the consumer compiler layer when present.
Woodie pivots use this runtime's close-based fallback because the free
function does not receive the period open required by TradingView's full
Woodie formula.

## Math (`<pineforge/math.hpp>`)

The runtime exposes only two pieces under math:


| Symbol                                            | Signature                                    | Notes                                                                                                          |
| ------------------------------------------------- | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `pine_random(lo, call_site, hi, seed, bar_index)` | inline free function                         | Deterministic SplitMix64-style mixer. Stable across platforms / runs; **not** TradingView's PRNG.              |
| `math::Sum(length)`                               | class with `compute(src)` / `recompute(src)` | Rolling-window sum used to back PineScript `math.sum(source, length)`. NaN inputs short-circuit to NaN output. |


Order-fill rounding to mintick lives on `BacktestEngine::round_to_mintick(price)`.
Every other Pine math function is the consumer compiler's responsibility
— the runtime intentionally provides no `abs`, `sqrt`, trig, `min` /
`max`, etc. PineForge's transpiler emits those inline against `<cmath>`.

## Strings (`<pineforge/str_utils.hpp>`)


| Helper                 | Signature                                | Behaviour                                                                                                                                                                                                                                     |
| ---------------------- | ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pine_str_format`      | `(fmt, vector<string> args)`             | `{N}` placeholder substitution against `args[N]`. Multiple occurrences of the same placeholder are all replaced.                                                                                                                              |
| `pine_str_format_time` | `(timestamp_ms, format, timezone)`       | Maps Pine tokens (`yyyy / MM / dd / HH / mm / ss`) to `strftime` and formats. Empty / `"UTC"` / `"Etc/UTC"` use `gmtime_r`; everything else swaps `TZ` under `pine_tz::ScopedTimezone` and uses `localtime_r`.                                |
| `pine_str_match`       | `(source, regex_pattern)`                | Returns the first capture group if any, else the full match. Empty string on no match or regex error.                                                                                                                                         |
| `pine_str_split`       | `(source, separator)`                    | Returns `vector<string>`. Empty separator yields `{source}`.                                                                                                                                                                                  |
| `pine_str_tostring`    | `(value, format_mode = "", mintick = 0)` | NaN renders as `"NaN"`. Modes: `"mintick"` (rounds to mintick, decimal places implied by mintick), `"percent"` (×100 with `%` suffix, 2 decimals), `"volume"` (B / M / K abbreviations). Default mode falls back to `std::to_string(double)`. |


Enum-string lookup for `str.tostring(<enum_member>)` is implemented by
`pine_enum_str_at(table, n, idx)` (defined in `engine.hpp`), which
clamps the index to the table size to avoid out-of-bounds reads.

Other string operations (`length`, `contains`, `replace`, etc.) are not
part of the runtime API — the consumer compiler emits those inline.

## Inputs

Inputs are stored as `std::unordered_map<std::string, std::string>` on
the engine. Generated strategy code reads them through typed getters
that fall back to the Pine default on missing key or parse failure:

```cpp
double  get_input_double(const std::string& key, double default_val) const;
int     get_input_int   (const std::string& key, int    default_val) const;
bool    get_input_bool  (const std::string& key, bool   default_val) const;
std::string get_input_string(const std::string& key, const std::string& default_val) const;
```

`get_input_bool` accepts `"true" / "1"` and `"false" / "0"` (anything
else returns the default). `get_input_double` / `_int` route through
`std::stod` / `std::stoi` with a try / catch around parse errors.

The runtime is intentionally agnostic about the *kind* of input
(`input.float / .int / .bool / .string / .source / .color / .timeframe / …`);
all inputs are presented as strings and the typed getter at the call
site decides the parse. UI metadata (`group`, `inline`, `tooltip`,
`display`, `confirm`, `options`, `min / max / step`) has no runtime
backing.

## `request.security()`

The runtime owns same-symbol security computation. Per-call state lives
in `SecurityEvalState`:

```cpp
struct SecurityEvalState {
    int sec_id;
    std::string tf;
    TimeframeAggregator aggregator;
    Bar current_bar;
    bool gaps_on, lookahead_on;
    bool lower_tf_requested, lower_tf_emulation;
    int lower_tf_ratio, lower_tf_seconds;
    int current_sub_bar_count;
    int64_t feed_count, eval_complete_count, eval_partial_count;
    bool lower_tf_array_requested;
    int lower_tf_sub_bar_index;
};
```

Lifecycle hooks the generated subclass implements:

- `configure_security_evaluators()` — called once at the start of `run(...)`; the subclass calls `register_security_eval(sec_id, requested_tf, input_tf, lookahead_on, gaps_on)` for each `request.security()` call site.
- `evaluate_security(sec_id, bar, is_complete)` — invoked by the runtime each time a security bar is ready (complete or partial under `lookahead_on`).
- `clear_security(sec_id)` — invoked when `gaps_on` produces an empty bar.

For `request.security_lower_tf(...)`, the generated subclass registers
with `register_security_lower_tf_eval(sec_id, requested_tf, input_tf)`
and reads `security_lower_tf_sub_bar_index(sec_id)` during synthesis so
it can clear and append to the returned array in earliest-to-latest
order.

Per-bar feed semantics (`feed_security_eval_state`):

- **Higher-TF requests** route input bars through `TimeframeAggregator`.
  - On a complete aggregated bar: `eval_complete_count++`, then `evaluate_security(...)` with `is_complete=true`.
  - On a partial bar with `lookahead_on`: `eval_partial_count++`, then `evaluate_security(...)` with `is_complete=false`.
  - On a partial bar with `gaps_on`: `clear_security(sec_id)`.
  - Otherwise the partial bar is silently held until completion.
- **Lower-TF emulation** (`lower_tf_emulation=true`) synthesizes intrabar bars from the input bar via `synthesize_lower_tf_bars`, which samples the OHLC path in `ratio + 1` `ENDPOINTS`-distribution points, time-stamps each slice on a fixed `requested_seconds` grid, and divides volume evenly (with the remainder on the last slice). Each synthetic bar is fed as a complete update.

`supports_lower_tf_emulation` only accepts emulation when **both** input
and requested timeframes are fixed intraday minute strings (no `D / W / M / S` suffix), `requested < input`, and
`input_seconds % requested_seconds == 0`.

`ensure_supported_lower_tf_emulation_flags` rejects lower-TF emulation
when `lookahead_on` or `gaps_on` is set — emulation is
`lookahead_off / gaps_off` only.

`request.security_lower_tf(...)` is supported for same-symbol lower
timeframes that satisfy the same emulation constraints. It returns an
array whose elements are ordered earliest-to-latest within the chart bar,
matching Pine v6's documented return shape. PineForge currently supports
numeric and bool element arrays; tuple, UDT, color, and string element
arrays are rejected by the transpiler.

`validate_security_timeframes(input_tf)` runs at the start of `run(...)`
and throws when:

- a request exists but `input_tf` is empty, or
- a requested TF is finer than the input but does not satisfy the lower-TF emulation constraints above.

Beyond that, the runtime does not police the symbol argument or reject
other `request.`* variants — those rejections live in the surrounding
compiler layers.

## Bar magnifier

`MagnifierDistribution` has six modes (in `magnifier.hpp`):


| Mode                  | Sample placement                                              |
| --------------------- | ------------------------------------------------------------- |
| `UNIFORM`             | Equal arc-length spacing along the OHLC path.                 |
| `COSINE`              | Chebyshev-like density at segment endpoints.                  |
| `TRIANGLE`            | Density at midpoints of each segment.                         |
| `ENDPOINTS` (default) | Always include exact O, H, L, C with uniform fill in between. |
| `FRONT_LOADED`        | Density near `O`.                                             |
| `BACK_LOADED`         | Density near `C`.                                             |


`sample_price_path(bar, n, dist)` samples at least 2 points and always
emits exactly `O` first and `C` last. The middle leg sequence is `O → H → L → C` when the open is closer to high, otherwise `O → L → H → C`
(ties go low-first).

`sample_price_path_volume_weighted(bar, base, mean_volume, min=2, max=64, dist)`
scales sample count by `bar.volume / mean_volume`, clamped to
`[min, max]`. `BacktestEngine::run_magnified_bar(sub_bars)` precomputes
the per-bar mean volume so each sub-bar's tick density is relative to
its own script bar's average. The toggle is
`set_magnifier_volume_weighted(bool)`.

Inside `run_magnified_bar` the engine threads through every sub-bar,
calling `feed_security_eval_state` once per sub-bar (so security
state-machines see the same fine bars), then iterates the sampled price
path. On the last sample of the last sub-bar `is_first_tick_` is forced
to `true` so generated `on_bar(...)` advances series history exactly
once per script bar.

## Series, Bar, and `na`

`Series<T>` (header-only template in `series.hpp`):

```cpp
template<typename T> class Series {
    void push(T value);     // new bar — newest at the front
    void update(T value);   // overwrite current bar (magnifier intrabar)
    T operator[](int k) const;  // 0 = current, k >= 1 = k bars ago, out-of-range -> na<T>()
    T current() const; int size() const; void clear();
};
```

`max_len` defaults to 500; out-of-range or negative offsets return `na<T>()`.

`Bar`:

```cpp
struct Bar { double open, high, low, close, volume; int64_t timestamp; };
```

`timestamp` is Unix milliseconds. There is no separate close timestamp
at the storage layer.

`na`:

```cpp
template<typename T> T na();         // double -> NaN, int/int64_t -> INT_MIN, bool -> false
inline bool is_na(double v);          // std::isnan
template<typename T, ...> bool is_na(T v);  // integer overload (== INT_MIN)
```

## Color

`pine_color::*` holds 17 named ARGB constants. Helpers:

- `new_color(c, transp)` — clear alpha and pack `(100 - transp) * 2.55` into the high byte.
- `r(c)`, `g(c)`, `b(c)` — channel bytes.
- `t(c)` — recover `transp` (0–100) from the alpha byte.

There are no charting / drawing types in the runtime.

## Timeframes

`tf_to_seconds(tf)` covers minute strings (`"1"`, `"5"`, `"60"`, `"240"`, …),
day strings (`"D"`, `"1D"` → 86400), and week strings (`"W"`, `"1W"` →
604800). Month (`"M"`, `"1M"`) returns `-1` to flag calendar mode.
`tf_multiplier`, `tf_is_intraday / _daily / _weekly / _monthly / _seconds`
are inline string predicates.

`tf_change(prev_ms, curr_ms, tf)` and
`crosses_boundary(prev_ms, curr_ms, period)` provide TF / calendar
boundary detection.

`tf_ratio(input_tf, target_tf)` returns:

- `> 1` for ratio aggregation,
- `1` for same TF,
- `-1` for calendar aggregation (month),
- `-2` when the target TF is finer than the input.

`detect_timeframe(bars, n, max_samples=100)` infers a TV-style TF string
from median timestamp deltas, with fallback `"1"` on insufficient or
irregular data.

`TimeframeAggregator` runs in three modes:

- `PASSTHROUGH` (default constructor),
- `RATIO` (constructor `(int ratio)` — every `ratio` input bars produce one output bar),
- `CALENDAR` (constructor `(target_tf, input_tf)` — aggregate to day / week / month boundaries).

`feed(bar)` returns an `AggregatedBar { Bar bar; bool is_complete; int sub_bar_count; }`.

## Time / Session / Timezone

`pine_time(bar_ms, tf, session, tz, chart_tf)` and
`pine_time_close(...)` return Unix milliseconds, or `na<int64_t>()` when
the bar is outside the requested session (TradingView semantics for
filtered sessions). They handle session string parsing and timezone
conversion internally.

`pine_tz::ScopedTimezone(tz)` is RAII — it grabs a process-wide mutex,
swaps `TZ` (saving the prior value), restores `TZ` on destruction, and
releases the mutex. This is the only reason `pine_str_format_time` and
the session helpers are safe to call from a multi-strategy harness.

## Matrices

`PineMatrix` wraps `Eigen::MatrixXd`. Member surface:


| Group            | Methods                                                                                                                                                             |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Construction     | `new_(rows, cols, init_val=0)` (static)                                                                                                                             |
| Access           | `get`, `set`, `fill`, `row`, `col`, `rows`, `columns`                                                                                                               |
| Row / column ops | `add_row(idx, values)`, `add_col(idx, values)`, `remove_row`, `remove_col`, `swap_rows`, `swap_columns`                                                             |
| Transform        | `copy`, `submatrix(from_row, to_row, from_col, to_col)`, `reshape(rows, cols)`, `reverse`, `transpose`, `sort(column, ascending=true)`, `concat(other, horizontal)` |
| Aggregation      | `avg`, `min`, `max`, `mode`, `sum`                                                                                                                                  |
| Arithmetic       | `diff`, `mult`, `pow(n)`                                                                                                                                            |
| Linear algebra   | `det`, `inv`, `pinv`, `rank`, `trace`, `eigenvalues`, `eigenvectors`                                                                                                |
| Kronecker        | `kron(other)`                                                                                                                                                       |
| Count            | `elements_count`                                                                                                                                                    |
| Predicates       | `is_square`, `is_identity`, `is_diagonal`, `is_antidiagonal`, `is_symmetric`, `is_antisymmetric`, `is_triangular`, `is_stochastic`, `is_binary`, `is_zero`          |


The element type is fixed to `double`. UDT-typed matrices are not
runtime-supported (see "Not implemented anywhere" below).

## Runtime diagnostics

`fill_report(ReportC*)` populates:


| Field                                                                                                    | Meaning                                                                                               |
| -------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `total_trades`, `trades`, `trades_len`, `net_profit`                                                     | Closed trade summary; `trades` is heap-allocated `TradeC[]`, freed via `free_report`.                 |
| `input_bars_processed`, `script_bars_processed`                                                          | Bar counters from the `run(...)` loop.                                                                |
| `magnifier_sub_bars_total`, `magnifier_sample_ticks_total`                                               | Magnifier work counters (sub-bars consumed × ticks sampled).                                          |
| `input_tf_seconds`, `script_tf_seconds`, `script_tf_ratio`, `needs_aggregation`, `bar_magnifier_enabled` | TF / aggregation diagnostics for the run.                                                             |
| `security_feeds_total`, `security_eval_complete_total`, `security_eval_partial_total`                    | Aggregate `request.security` counters.                                                                |
| `security_diag`, `security_diag_len`                                                                     | Per-security `SecurityDiagC { sec_id, feed_count, eval_complete_count, eval_partial_count }`.         |
| `trace`, `trace_len`, `trace_names`, `trace_names_len`                                                   | Optional per-bar trace records and interned trace-name table, populated only when tracing is enabled. |


Each `TradeC` carries
`entry_time / exit_time / entry_price / exit_price / pnl / pnl_pct / is_long / max_runup / max_drawdown / qty`
(where `max_runup` / `max_drawdown` are peak favorable / adverse
excursions in $ / contract).

## Logging and runtime errors

`log.hpp` exposes four inline functions:

```cpp
inline void pine_log_info(const std::string& msg);     // stderr "[INFO] "
inline void pine_log_warning(const std::string& msg);  // stderr "[WARN] "
inline void pine_log_error(const std::string& msg);    // stderr "[ERROR] "
inline void pine_runtime_error(const std::string& msg); // throws std::runtime_error
```

The runtime itself raises `std::runtime_error` from
`validate_security_timeframes`, `feed_security_eval_state` (lower-TF
synthesis failure), and `ensure_supported_lower_tf_emulation_flags`.

## What the runtime does not implement

The two lists below distinguish between *PineForge does not support
this at all* and *the runtime has no module for this, but PineForge
supports it via the consumer compiler's emitted code*.

### No runtime module — Pine surface still supported via consumer compiler

These features work in PineForge code today; the runtime simply does
not own a dedicated class or function for them. PineForge's transpiler
emits inline C++ against `<cmath>`, `<vector>`, `<unordered_map>`, and
generated structs.

- Pine `array<T>` — emitted as `std::vector<T>` by PineForge's transpiler.
- Pine `map<K,V>` — emitted as `std::unordered_map<K,V>`.
- User-defined types (UDTs) — emitted as plain C++ structs; nested fields and `array<UDT>` are also handled there.
- Currency conversion (`strategy.convert_to_`*) — no runtime feed; PineForge's transpiler treats this as identity (no FX adjustment).
- Most scalar `math.`* functions (`abs`, `sqrt`, `min`, `max`, trig, `round`, etc.) — PineForge emits these against `<cmath>` / inline expressions.
- Most `str.`* operations (`length`, `contains`, `replace`, `lower`, `upper`, `tonumber`, etc.) — PineForge emits these against `std::string`.

### Not implemented anywhere — gaps with a future story

These are **not** supported by PineForge as a whole today. Each item
carries a forward-looking assessment using these buckets:


| Tag                           | Meaning                                                                                                                  |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| **Easy**                      | Mechanically straightforward — small, localized runtime / consumer-compiler change, no architectural shift.              |
| **Feasible**                  | Doable with non-trivial work but no fundamental conflict with PineForge's offline-batch model.                           |
| **Feasible — needs aux data** | Mechanically feasible, but requires the user to provide an external dataset PineForge does not currently ingest.         |
| **Out of scope by design**    | Conflicts with PineForge's offline-batch / paid-parity-validity model. Not a roadmap item even if mechanically possible. |
| **Out of scope structurally** | Cannot be done without a feature PineForge does not have (e.g. live data feed).                                          |


#### Drawing / charting / alerts

`plot`, `plotshape`, `plotchar`, `plotcandle`, `plotbar`, `plotarrow`,
`fill`, `hline`, `bgcolor`, `barcolor`, `label.`*, `line.`*, `box.*`,
`table.*`, `polyline.*`, `linefill.*`, `alert(...)`, `alertcondition(...)`.

- **Feasibility:** *Feasible* for plotting primitives (capture series + style metadata into the `ReportC` extension or a side-channel CSV / JSON for an external renderer); *Out of scope structurally* for live `alert(...)` because PineForge produces no realtime stream.
- **Future story:** A "report-as-data" path is the obvious target — `plot(...)` and friends would write tagged time-series rows into a new diagnostics array on `ReportC`, and a Python harness would render them with Plotly / matplotlib. `alertcondition(...)` results could be returned as a list of `(bar_time, message)` triples. The graphics primitives (`label.new`, `line.new`, `box.new`) would need a small annotation runtime — straightforward but high surface area. Webhook / push-notification alerts stay out of scope.
- **Why not done yet:** Backtests already produce `TradeC[]` and per-bar diagnostics; visual plotting has not been the unblocker for any user-facing strategy validation. It is a UX feature, not a correctness feature.

#### `varip` and realtime tick semantics

`varip` (persists across realtime ticks; resets on bar close in TV);
`barstate.isrealtime`, `barstate.isnew` in realtime, `calc_on_every_tick`,
`calc_on_order_fills`, live tick streams.

- **Feasibility:**
  - `varip` itself: *Feasible*. With the bar magnifier already simulating intrabar samples, `varip` could map onto sub-bar persistence (do not reset between magnifier ticks; reset only on bar close).
  - Realtime barstate flags + `calc_on_every_tick` + `calc_on_order_fills`: *Out of scope structurally* — they require a live data feed PineForge does not have.
- **Future story:** `varip` mapping to magnifier sub-bar state is the right design; currently the support checker rejects `varip` outright but that is conservative rather than fundamental. Realtime semantics would require a streaming runtime that PineForge is not built for and does not aim to be.
- **Why not done yet:** `varip` use cases overlap heavily with what `var` already covers in batch mode. The realtime distinction TV makes is meaningful only when the data feed is live.

#### Import / export / library system

`import <user>/<lib>/<version>`, `export` keyword, `library(...)`
declaration.

- **Feasibility:** *Feasible*. Pure compiler concern — not a runtime issue at all.
- **Future story:** Resolve the import (local file or fetched bundle), parse it into an AST, inline its `export`-ed names into the importing strategy's symbol table, and proceed. The runtime needs zero changes; this is an analyzer / compiler-pipeline feature. The hard parts are the package-resolution UX (registry, versioning, caching) and namespacing rules, not code generation.
- **Why not done yet:** PineForge's primary user surface is single-file strategies. Library-driven workflows are common on TradingView but the cost / value has not justified building the resolver. Pre-expansion (paste the library inline) is the current workaround.

#### External `request.`* variants

`request.financial(symbol, field, period)`, `request.dividends`,
`request.earnings`, `request.splits`, `request.currency_rate`,
`request.seed(source, symbol, expression)`, `request.quandl`.

- **Feasibility:**
  - `request.financial / dividends / earnings / splits / currency_rate`: *Feasible — needs aux data*. Would need a parallel data-ingestion path so the user can supply a CSV / Parquet of fundamentals or corporate actions; the runtime would then look up the right slice by `bar.timestamp`.
  - `request.seed`: *Out of scope structurally*. TradingView seeds are user-published time series hosted on TV's infrastructure; PineForge has no equivalent registry.
  - `request.quandl`: *Out of scope by design*. Deprecated upstream; not worth implementing.
- **Future story:** Fundamentals would land as a generic "auxiliary timeline" feature: pass `aux_data={"earnings": df}` to the runner, exposed to Pine via `request.financial`-shaped accessors that read from the aux table.
- **Why not done yet:** Most strategy logic in our test corpus relies on the chart symbol's bars + indicators. Fundamentals-driven strategies are a meaningful but smaller user segment; we have not built the aux-data ingestion contract.

#### `barmerge.lookahead_on` for lower-TF emulation

Currently `ensure_supported_lower_tf_emulation_flags(...)` throws when
`lookahead_on=true` for a lower-TF request.

- **Feasibility:** *Out of scope by design*. Mechanically possible — just remove the guard — but `lookahead_on` combined with synthesized intrabar bars exposes information from a not-yet-complete sub-bar. That is a backtest-validity footgun.
- **Future story:** No plan to enable it. Higher-TF aggregation already honours `lookahead_on` for partial updates (which is the legitimate use case); the lower-TF synthesis path is fundamentally incompatible with backtest-honest lookahead.
- **Why not done yet:** Intentional rejection, not an oversight.

#### TradingView-exact PRNG parity

`math.random(...)` byte-for-byte matching TradingView's stream.

- **Feasibility:** *Out of scope by design*. PRNG is deterministic and reproducible across runs / platforms; that is the contract PineForge advertises for paid-parity. TV's PRNG is undocumented and would need black-box reverse engineering to match exactly.
- **Future story:** None. Determinism is preferred over TV-byte parity. If TradingView publishes their generator we can revisit.
- **Why not done yet:** Trade-off was made explicitly; see the `pine_random` SplitMix64 comment in `math.hpp`.

#### Typed (UDT) matrices

`matrix<MyUDT>`. `PineMatrix` is `double`-only because `Eigen::MatrixXd`
is the backing store.

- **Feasibility:** *Feasible*. Two layouts compete: keep `PineMatrix` as the numeric path (Eigen-backed), and add a generic `PineUDTMatrix<T>` backed by `std::vector<std::vector<T>>` for UDTs and non-numeric primitives. Methods that only make sense for numeric matrices (`det`, `inv`, `eigenvalues`, …) would not be exposed on the generic version.
- **Future story:** Worth doing if a strategy needs UDT matrices. The consumer compiler routing is already prepared to dispatch on matrix element type — only the runtime side is missing.
- **Why not done yet:** No real strategy in our corpus uses UDT matrices; numeric matrices satisfy every observed use case so far.

## Verifying the surface yourself

The 162-strategy validation corpus under `[corpus/](../corpus/)` is the
canonical proof that this runtime delivers the surface listed above.
Run `bash scripts/run_corpus.sh` to compile every `generated.cpp`
against `libpineforge.a` and diff the per-strategy `engine_trades.csv`
against the TradingView export shipped alongside it — the current
canonical report is **excellent=158, strong=4** across 162 strategies.
The strict profile checks count + entry-price + exit-price + P&L within
`1.0% / 0.01% / 0.01% / 1.0%`; the production profile is reserved for
path-dependent trailing-stop strategies.

The three-way benchmark under `[benchmarks/](../benchmarks/)` extends
this comparison to include [PyneCore](https://github.com/PyneSys/pynecore)
and [PineTS](https://github.com/LuxAlgo/PineTS), exercising the same
surface across three independent engines (PineForge hits canonical
*excellent* tier on 48 / 50 strategies vs PyneCore's 45 / 50; the 3
PyneCore-only outliers all involve bracket / trail / partial exits, see
`[benchmarks/results/summary.md](../benchmarks/results/summary.md)`).