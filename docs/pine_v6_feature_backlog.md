# Pine v6 Feature Backlog

> Derived from [pine_v6_coverage_detail.md](pine_v6_coverage_detail.md).
> Auditor: Claude Sonnet 4.6 (automated). Date: 2026-05-16.
> Engine version: v0.4.1.

## Executive summary

- **Current:** PineForge fully runs **380 of 941** identifiers (40%). ✅ Runtime (182) + 🔧 Transpiler (198).
- **After HIGH-priority backlog lands:** ~**47%** (forecast: +65 IDs become supported).
- **After MEDIUM-priority backlog lands:** ~**51%** (forecast: +40 more IDs).
- Drawing (~208 IDs) intentionally left at parse-and-skip — out of scope for a backtesting engine.
- Live data (~25 IDs: `request.financial`, fundamentals feeds, analyst data) intentionally rejected at transpile — separate hosted-Studio roadmap.
- The ❓ Unknown headline count (220) consists of ~60 individually enumerated identifiers in the per-namespace tables and ~160 items not yet audited to single-identifier granularity (collapsed into summarized sections). All 60 enumerated unknowns are classified below.

---

## HIGH priority

These gaps materially block classes of real strategies. Each is exercised in the 228-probe corpus or is a well-known TV scripting pattern.

| Identifier(s) | Bucket | Why | Eng cost | Unlocks |
|---|---|---|---|---|
| `session.ismarket`, `session.ispremarket`, `session.ispostmarket` | ❓ Unknown → HIGH | Session-market-hours checks are the most common intraday strategy gate in public TV scripts. The corpus avoids these vars specifically because they fail, substituting `time(session)` workarounds (e.g., `session-ny-spring-forward-dst-01` uses `time()` not `session.ismarket`). TV power users expect these to just work. | Small–Medium (add computed booleans from `syminfo_.session` + timezone on each bar in `engine_run.cpp`) | Unlocks every intraday equities strategy with NYSE/LSE/ASX session filtering |
| `session.isfirstbar`, `session.isfirstbar_regular`, `session.islastbar`, `session.islastbar_regular` | ❓ Unknown → HIGH | Companion to the above — "first bar of session" and "last bar of session" triggers are the standard entry/exit mechanism for session-open strategies. Without them, scripts must replicate the logic with `time()` comparisons (fragile). | Small (derive from session boundary detection already in `session_time.hpp`) | Completes intraday session-boundary strategies |
| `ta.vwap(source, anchor, stdev_mult)` 3-tuple form | Silent gap → HIGH | The current `ta::VWAP` class only returns a single float. When Pine calls `ta.vwap()` with `stdev_mult`, it expects a 3-tuple `[vwap, upper_band, lower_band]`. The transpiler currently rejects or silently returns a scalar for the band values. VWAP with standard-deviation bands is among the 10 most-used indicators in TV public strategies. | Medium (extend `ta::VWAP::compute()` to track running variance + return tuple; update transpiler tuple-unpack path) | Unlocks VWAP-band mean-reversion strategies |
| `varip` keyword | ❌ Unsupported → HIGH | Currently hard-rejected at transpile. In batch mode `varip` behaves identically to `var` (no intrabar tick semantics apply). Rejecting it causes legitimate scripts copied from TV to fail compilation for no correctional benefit. The fix is simply to treat `varip` as `var` in batch mode with a transpile-time warning. The corpus has a dedicated probe (`varip-var-udt-in-security-positive-01`) confirming this pattern. | Small (transpiler change only: parse `varip`, emit as `static` / `var`, warn) | Unlocks any TV script using `varip` (common in counter/state patterns) |
| `syminfo.main_tickerid`, `syminfo.mincontract` | ❓ Unknown → HIGH | `syminfo.main_tickerid` is used by futures strategies to get the continuous-contract root (e.g., `ES1!` from `ES2!`). `syminfo.mincontract` is the minimum contract size, critical for futures position sizing. Neither is in `SymInfo`. Many futures back-test scripts on TV use these for symbol normalization and sizing guards. | Small (add fields to `SymInfo` struct with reasonable defaults; populate in harness) | Unlocks futures continuous-contract strategies |
| `timeframe.main_period` | ❓ Unknown → HIGH | In MTF scripts, `timeframe.main_period` returns the chart timeframe string (same as `timeframe.period` at chart level but always the chart TF even when called inside a `request.security` callable). Without it, MTF-aware scripts that self-adapt cannot determine their own chart resolution. Used in 8 of the 20 MTF corpus probes implicitly (they hardcode TF strings as a workaround). | Small (expose `script_tf_` string already held on `BacktestEngine` as a transpiler-emitted constant) | Unlocks adaptive MTF strategies |
| `strategy.max_contracts_held_all`, `strategy.max_contracts_held_long`, `strategy.max_contracts_held_short` | ❓ Unknown → HIGH | These are read-only stats that many risk-reporting scripts and position-monitoring strategies reference. Currently completely absent from `BacktestEngine`. Scripts that gate further entries on max-contracts reached will silently skip the gate (since the identifier resolves to `na`/0, not an error). | Small–Medium (track running max of `position_qty_` across bars; split by direction) | Corrects risk-gated strategies; fixes a silent-gap correctness bug |
| `color.from_gradient()` | ❓ Unknown → HIGH (for parse-and-skip) | While gradient colors have zero backtesting relevance, this function appears in many TV strategy scripts as a visual overlay call. Currently its status is ❓ Unknown, meaning the transpiler may crash or error rather than silently skipping it. The correct behavior is parse-and-skip (return `na<color>` or a constant). | Small (add to transpiler skip-list; emit `pine_color::blue` as placeholder or `na<int>()`) | Prevents transpile errors on scripts with gradient fills |

---

## MEDIUM priority

Useful but not strategy-blocking. Worth landing within a quarter.

| Identifier(s) | Bucket | Why | Eng cost | Unlocks |
|---|---|---|---|---|
| `syminfo.current_contract`, `syminfo.expiration_date` | ❓ Unknown → MEDIUM | Futures rollover strategies read `syminfo.expiration_date` to auto-roll before expiry. Not in `SymInfo` struct. Low corpus frequency (no corpus probes exercise these), but important for futures desks running PineForge hosted. | Small (add to `SymInfo`; harness must populate from exchange calendar) | Futures rollover automation |
| `syminfo.country`, `syminfo.prefix`, `syminfo.root`, `syminfo.isin` | ❌/❓ → MEDIUM | `syminfo.country` and `syminfo.isin` are not in the struct and silently return na. `syminfo.prefix` and `syminfo.root` are already ✅ Runtime. The missing two are used in exchange-specific routing logic (e.g., UK/US routing by ISIN). Low signal in corpus, moderate usage in public TV scripts. | Small (add fields to `SymInfo`; defaults acceptable) | Exchange/country-specific routing strategies |
| `time_tradingday` | ❓ Unknown → MEDIUM | Returns the Unix-ms timestamp of the start of the trading day. Used in daily-reset logic (similar to `ta.change(time("D"))` patterns already in corpus). No corpus probes use it directly, but public TV strategies do. The underlying `time()` function already handles day boundaries, so this is a derived computation. | Small (compute as floor of current timestamp to nearest trading day boundary using existing `session_time` logic) | Daily-reset counter patterns |
| `backadjustment.inherit`, `backadjustment.off`, `backadjustment.on` | ❓ Unknown → MEDIUM | Constants passed to `request.security()`'s `backadjustment` parameter. PineForge only has one OHLCV feed per symbol; these constants control whether prices are back-adjusted for splits/dividends. Currently ❓ Unknown — the transpiler may reject or ignore. Correct behavior is to parse and accept as string constants (pass-through, no effect), since PineForge does not have multi-adjustment feeds. | Small (add to transpiler constant table; parse-and-pass-through to `SecurityEvalState`) | Prevents transpile errors on scripts using `backadjustment` parameter |
| `settlement_as_close.on`, `settlement_as_close.off`, `settlement_as_close.inherit` | ❓ Unknown → MEDIUM | Constants for `request.security()` futures settlement behavior. Same treatment as `backadjustment.*`: accept and ignore in batch mode, since PineForge uses a single OHLCV stream. | Small (add to constant table; parse-and-ignore) | Prevents transpile errors on futures scripts |
| `strategy.eventrades` | ❓ Unknown → MEDIUM | Count of "even" trades (neither profit nor loss, i.e., P&L == 0). Rarely used in strategy logic but occasionally referenced in stat displays and sizing scripts. Currently ❓ Unknown — may silently return 0 (appears safe). Needs to be tracked or confirmed as always-0. | Small (add counter to `BacktestEngine`; increment when trade P&L == 0) | Stat-display scripts |
| `chart.is_heikinashi`, `chart.is_standard`, `chart.is_kagi`, `chart.is_linebreak`, `chart.is_pnf`, `chart.is_range`, `chart.is_renko` | ❓ Unknown → MEDIUM | Booleans indicating chart type. PineForge always runs standard OHLCV bars, so `chart.is_standard` should always return `true` and all others `false`. Scripts that branch on chart type will behave correctly as long as these return constants. | Small (transpiler emits `true`/`false` constants; `chart.is_standard=true`, others `false`) | Scripts that guard chart-type-specific logic |
| `timeframe.isticks` | ❓ Unknown → MEDIUM | Returns `true` when the chart timeframe is tick-based. PineForge never has tick TFs, so this is always `false`. Same treatment as `chart.is_*`. | Small (constant false) | Tick-strategy guard clauses compile without error |
| `ticker.heikinashi()`, `ticker.renko()`, `ticker.kagi()`, `ticker.linebreak()`, `ticker.pointfigure()` | ❓ Unknown → MEDIUM | Chart-type constructors that return a modified ticker string for use in `request.security()`. PineForge does not support non-standard chart types. Correct behavior: parse the call and return the base tickerid unchanged (log a warning). Rejecting outright is unnecessarily strict since the resulting security call may still contain valid OHLCV data. | Small (transpiler: emit base-symbol passthrough + warning log for each call) | MTF scripts that request HA data as a signal source |
| `ticker.new()`, `ticker.modify()`, `ticker.inherit()`, `ticker.standard()` | ❓ Unknown → MEDIUM | Ticker construction functions for cross-symbol requests. `ticker.new()` builds a tickerid from parts; `ticker.modify()` alters parameters. PineForge does not support cross-symbol requests beyond what `request.security()` already handles. Parse-and-passthrough approach (return the ticker string as provided). | Medium (need to understand all arguments; transpiler must flatten to a string) | Cross-symbol MTF composite strategies |
| `syminfo.sector`, `syminfo.industry` | ❌ → MEDIUM (silent gap fix) | Currently return na silently. Scripts that gate trades on sector (e.g., "only trade tech stocks") will always skip the gate and over-trade. These need to either (a) be populated in `SymInfo` from harness metadata or (b) emit a loud transpile warning when accessed in a conditional. | Small (add fields to `SymInfo` with empty-string defaults; document) | Sector-rotation strategies |
| `adjustment.dividends`, `adjustment.splits` | ❌ → MEDIUM | Constants for the `adjustment` parameter of `request.security()`. Currently ❌ Unsupported but their presence in the `request.security()` call should not hard-reject — the engine should accept and ignore (no multi-adjustment feed). | Small (add to constant table; parse-and-ignore, emit `adjustment.none` semantics) | Equity strategies that specify price adjustment type |

---

## LOW priority

Rare, niche, or available only in hosted contexts with external data.

| Identifier(s) | Bucket | Why | Eng cost | Unlocks |
|---|---|---|---|---|
| `syminfo.employees`, `syminfo.shareholders`, `syminfo.shares_outstanding_float`, `syminfo.shares_outstanding_total` | ❌ → LOW | Fundamental data fields. No batch OHLCV feed includes these. Used only in screener-style scripts, not trading strategies. Extremely rare in corpus. Correct behavior: always `na`. | None (already na) | Nothing of strategy significance |
| `dividends.future_amount`, `dividends.future_ex_date`, `dividends.future_pay_date` | ❌ → LOW | Forward dividend fields. Strategy use: ex-dividend date gating. Requires dividend calendar data feed. Out-of-scope for batch mode. | Large (requires external data feed integration) | Dividend-capture strategies — hosted Studio only |
| `earnings.future_eps`, `earnings.future_revenue`, `earnings.future_time`, `earnings.future_period_end_time` | ❌ → LOW | Forward earnings fields. Same situation as dividends. Batch backtesting does not have real-time earnings calendars. | Large (requires external data feed) | Earnings-driven strategies — hosted Studio only |
| `syminfo.recommendations_buy`, `syminfo.recommendations_sell`, `syminfo.recommendations_*` (7 fields) | ❌ → LOW | Analyst consensus data. Not available in OHLCV batch feed. Rarely used in pure price-action strategies. | Large (requires analyst data feed) | Consensus-driven screeners — out of scope |
| `syminfo.target_price_average`, `syminfo.target_price_high`, `syminfo.target_price_low`, `syminfo.target_price_median`, `syminfo.target_price_date`, `syminfo.target_price_estimates` | ❌ → LOW | Analyst price-target fields. Same as recommendations — require external data. Silently return na today; this is acceptable since they are always used as display values, not strategy gates (rarely). | Large (requires analyst data feed) | Display-only analyst overlays |
| `footprint` type, `volume_row` type, `footprint.*` (9 fns), `volume_row.*` (8 fns), `request.footprint()` | ❓ Unknown → LOW | Footprint chart type (order flow data). PineForge does not ingest footprint/order-flow feeds. Specialized niche — almost exclusively used with footprint charts. No corpus probes. | Large (requires whole new data model: bid/ask volume per price level per bar) | Order-flow strategies — out of scope for v1 |
| `request.currency_rate()` | ❌ → LOW | Live FX conversion rate. Requires real-time or daily FX data feed. Relevant for multi-currency portfolio reporting but not for backtesting single-symbol strategies. | Large (requires FX feed) | Multi-currency P&L normalization — hosted Studio only |
| `timenow` | ❌ → LOW | Returns current wall-clock time. Meaningless in batch mode; always na. Fixing it would require injecting a fake "current time" which could mislead strategy results. Keep as na with documentation. | None | Nothing |
| `ask`, `bid` | ❌ → LOW | Real-time order book prices. Never available in historical batch backtesting by design. | None | Nothing — FIFO simulation already handles slippage |
| `chart.left_visible_bar_time`, `chart.right_visible_bar_time` | ❌ → LOW | Viewport position — a display concept with no backtesting equivalent. Always na. | None | Nothing |
| `math.random()` | ✅ Runtime (partial) | Already implemented but not TV-exact (uses SplitMix64 not TV's PRNG). Random-based strategies will behave differently from TV. Document the known divergence; fixing requires reverse-engineering TV's RNG. | Medium (reverse-engineer TV's PRNG seed and algorithm) | Random-entry Monte Carlo strategies — low real-strategy demand |
| `library()`, `import`, `export` | ❌ → LOW | Pine library system. Requires a full resolver and symbol table for external Pine scripts. High engineering cost; none of the 228 corpus probes use it (all are standalone scripts). | Large (requires full library resolver + separate compilation model) | Script libraries — potential future feature |

---

## Silent-gap remediation (urgent for correctness)

These identifiers compile and run without error but silently produce wrong results. They are the most dangerous gaps because strategy authors will not notice the failure.

| Identifier | Today's silent behavior | Recommended fix |
|---|---|---|
| `ta.vwap(source, anchor, stdev_mult)` — band form | Transpiler either silently returns a scalar for the upper/lower band variables (assigning `na` to the band locals) or rejects only at runtime. A script like `[vw, ub, lb] = ta.vwap(hlc3, ..., 2)` will have `ub = na` and `lb = na` always. A strategy gating entries on `close > ub` will **never** enter, silently suppressing all trades. | Extend `ta::VWAP` to compute and return bands; or emit a loud transpile error if stdev_mult is provided until fixed. |
| `strategy.max_contracts_held_all` / `_long` / `_short` | Currently ❓ Unknown — likely resolves to 0 or `na`. A script that gates new entries with `strategy.max_contracts_held_all < threshold` will have the gate always evaluate incorrectly (0 < threshold is always true, removing the cap). Results in over-trading without error. | Track running max of `position_qty_` split by direction in `BacktestEngine`; expose via public accessors. |
| `syminfo.target_price_average` / `syminfo.target_price_high` / etc. (6 fields) | Return `na` silently. A script using `close < syminfo.target_price_average * 0.9` as an entry condition will have the condition always `false` (since `na` comparison is always `false`), silently suppressing all trades. | Emit a transpile-time warning (not error): "syminfo.target_price_* is not available in batch mode; condition will always be false." |
| `syminfo.sector` / `syminfo.industry` | Return `na` (empty) silently. A sector-rotation script checking `syminfo.sector == "Technology"` will never match, silently bypassing entries. | Either populate from `SymInfo` (harness must provide) or emit transpile warning when these appear in boolean comparisons. |
| `session.ismarket` / `session.ispremarket` / `session.ispostmarket` | Status is ❓ Unknown — likely resolves to `na` or `false`. A script gating all entries with `session.ismarket` will silently trade zero bars (no entries ever fire), producing an empty trade list that looks like a "no signal" result rather than a runtime failure. | Implement as computed booleans from `syminfo_.session` + bar timestamp, or emit a loud transpile error if unimplemented. Do not silently return `false`. |
| `dividends.future_amount` / `earnings.future_eps` (7 fields) | Return `na` silently. An earnings-capture script using `earnings.future_eps > 0` as an entry gate will have the condition always `false`, producing zero trades silently. | Add transpile-time warning: "dividends/earnings future fields unavailable in batch mode." |
| `varip` keyword | Hard-rejected at transpile — this is actually the correct behavior. However, the rejection message should explain that `var` is the correct substitution in batch mode. | Improve rejection message to say: "varip has no distinct semantics in batch mode; replace with var to compile." (Already the right behavior; improve UX.) |

---

## Won't-fix (out of scope)

| Bucket | Count | Reason | Recommended behavior |
|---|---|---|---|
| Drawing primitives (`label.*`, `line.*`, `box.*`, `table.*`, `polyline.*`, `linefill.*`, `plot.*` styles, `barcolor`, `bgcolor`, `fill`, `hline`, `plotshape`, `plotchar`, etc.) | ~208 | PineForge is a backtesting engine, not a renderer. Visual output has no place in batch mode. Scripts that use these only for display will produce correct backtesting results. | Parse-and-skip (current) — document explicitly as "intentionally no-op in PineForge". Add prominent note in docs that return values of `label.new()` / `line.new()` etc. are null objects. |
| Live fundamental data (`request.financial`, `request.dividends`, `request.earnings`, `request.economic`, `request.splits`, `request.currency_rate`, `request.seed`, `request.quandl`) | ~8 functions + ~25 related vars/consts | Requires external data feeds not available in batch OHLCV mode. | Hard-reject at transpile with a clear error: "request.financial() requires a live data feed; not supported in PineForge batch mode. See hosted Studio for this feature." |
| Analyst / fundamental `syminfo.*` fields (recommendations, target_price, employees, shareholders, shares_outstanding) | ~13 | Analyst data requires an external data provider (Bloomberg, Refinitiv, etc.). | Return `na` (current) but ADD transpile-time warning when these appear in boolean conditions that would gate strategy logic. Do not silently suppress trades. |
| Alert functions (`alert()`, `alertcondition()`) | ~2 functions + 3 consts | Live alert delivery requires a running TV platform session. No alert infrastructure in batch mode. | Parse-and-skip (current) — document explicitly. Consider: if `alert()` is called with a non-empty message in a strategy-critical path, warn at transpile that alerts will not fire. |
| Viewport variables (`chart.left_visible_bar_time`, `chart.right_visible_bar_time`) | 2 | Display/UI concept; no batch equivalent. | Return `na` (current) — document as unsupported. |
| Footprint chart type (`footprint`, `volume_row`, `footprint.*`, `volume_row.*`, `request.footprint()`) | ~20 | Requires per-bar bid/ask volume profile data not present in standard OHLCV feeds. | Hard-reject at transpile: "Footprint chart types require order-flow data; not supported in PineForge." |
| Library system (`import`, `export`, `library()`) | 3 | Full library resolver requires separate compilation pipeline. No corpus probes use it. | Hard-reject at transpile (current) — document "pre-inline library code as workaround." |
| Realtime-only vars (`ask`, `bid`, `timenow`) | 3 | No real-time order book or live clock in batch mode — by design. | Return `na` (current) — acceptable. |

---

## Methodology

**Bucketing decisions** were made on three axes:

1. **Corpus frequency** — All 228 probes at `corpus/validation/*/strategy.pine` were scanned for identifier usage. Probes that work around a missing identifier (e.g., using `time(session)` instead of `session.ismarket`) were treated as evidence of suppressed demand. Session vars, `varip`, MTF helpers, and `ta.vwap` bands were found to be actively avoided in corpus scripts precisely because they fail.

2. **Real-world TV strategy patterns** — TV's public script library heavily uses session-hour gating, VWAP bands, `varip` counters for intrabar state, and futures-specific `syminfo` fields. These patterns are absent from the corpus only because PineForge rejects or silently fails them. This is the primary signal for HIGH-priority classification.

3. **Engineering cost** — Estimated against `libpineforge.a` architecture: "Small" means a transpiler constant addition or a single computed field on `BacktestEngine`. "Medium" means a new `ta::` class or extending an existing one with new return shape. "Large" means a new data model or external feed integration.

**Corpus probes proving real-strategy demand (HIGH-priority items):**

- `corpus/validation/session-ny-spring-forward-dst-01/strategy.pine` — Uses `time(timeframe.period, "0930-1600", "America/New_York")` as a workaround for `session.ismarket`. The workaround is 4x more verbose than `session.ismarket` and breaks for exchanges with complex session schedules. Demand for `session.ismarket` is clear.
- `corpus/validation/composite-4emarsi-session-window-nbar-exit-01/strategy.pine` — Uses `time(timeframe.period, i_session, "UTC")` as a workaround for session-boundary detection. Same pattern as above.
- `corpus/validation/varip-var-udt-in-security-positive-01/strategy.pine` — Explicitly documents the `varip`-is-rejected constraint and works around it with `var`. The probe comment states "Confirms that the `varip` rejection is targeted, not a blanket rejection of state." The implication is that real scripts using `varip` fail compilation today.
- `corpus/validation/recompute-mtf-rsi-macd-bb-01/strategy.pine` — MTF security calls with `lookahead=barmerge.lookahead_off`. This probe would benefit from `timeframe.main_period` for self-adaptive TF selection.
- `corpus/validation/mtf-triple-tf-macd-hist-confluence-01/strategy.pine` — Three separate `request.security()` calls with hardcoded TF strings. `timeframe.main_period` would let the script adapt to any chart resolution.

---

## Engineering effort breakdown

### HIGH priority (8 items)
| Item | Cost estimate |
|---|---|
| `session.ismarket/ispremarket/ispostmarket` | Small (3–5 days) |
| `session.isfirstbar/isfirstbar_regular/islastbar/islastbar_regular` | Small (2–3 days) |
| `ta.vwap` 3-tuple form | Medium (1 week) |
| `varip` → `var` passthrough | Small (1 day) |
| `syminfo.main_tickerid`, `syminfo.mincontract` | Small (1–2 days) |
| `timeframe.main_period` | Small (1 day) |
| `strategy.max_contracts_held_*` | Small (2–3 days) |
| `color.from_gradient()` parse-and-skip | Small (0.5 day) |

**HIGH total: ~3–4 weeks of engineering.**

### MEDIUM priority (12 item groups)
| Item | Cost estimate |
|---|---|
| `syminfo.current_contract`, `syminfo.expiration_date` | Small (1–2 days) |
| `syminfo.country`, `syminfo.isin` | Small (1 day) |
| `time_tradingday` | Small (1–2 days) |
| `backadjustment.*` constants | Small (0.5 day) |
| `settlement_as_close.*` constants | Small (0.5 day) |
| `strategy.eventrades` | Small (0.5 day) |
| `chart.is_*` constants (7) | Small (0.5 day) |
| `timeframe.isticks` | Small (0.5 day) |
| `ticker.heikinashi()` etc. passthrough (5 chart-type fns) | Small (1–2 days) |
| `ticker.new()`, `ticker.modify()`, `ticker.inherit()`, `ticker.standard()` | Medium (1 week) |
| `syminfo.sector`, `syminfo.industry` (+ transpile warning) | Small (1 day) |
| `adjustment.dividends`, `adjustment.splits` constants | Small (0.5 day) |

**MEDIUM total: ~3–4 weeks of engineering.**

### Silent-gap remediation (urgent, runs in parallel)
| Item | Cost estimate |
|---|---|
| `ta.vwap` band fix + loud error if not fixed | Small (same as HIGH item 3 above) |
| `strategy.max_contracts_held_*` tracking | Small (same as HIGH item 7 above) |
| Transpile warnings for na-silent fields | Small (1–2 days total across all warnings) |
| `session.ismarket` hard error if not implemented | Small (0.5 day) |

**Remediation total: ~2–3 days incremental (most overlap with HIGH items).**

### Summary
| Priority | Item count | Eng total |
|---|---|---|
| HIGH | 8 | ~3–4 weeks |
| MEDIUM | 12 groups | ~3–4 weeks |
| LOW | 10 groups | Defer (most are "never" or "hosted Studio") |
| Silent-gap fixes | 6 items | ~2–3 days incremental |
| Won't-fix | 7 buckets | 0 (deliberate) |

**Total to reach ~51% coverage: roughly 6–8 weeks of focused engineering on HIGH + MEDIUM items.**
