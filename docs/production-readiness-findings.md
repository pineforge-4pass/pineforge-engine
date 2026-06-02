# Production-Readiness Findings

> Source-verified findings from the 2026-06-03 probe gap-analysis. Each entry
> cites `file:line` at the time of writing, a reproduction, and a recommended
> fix. The **fixed** findings already have green regression guards in `tests/`;
> the **open** findings are disclosed here rather than silently patched because
> the correct fix either changes trade economics (and must be validated against
> a TradingView export, which the public corpus cannot regenerate) or is a
> documentation/contract decision for the maintainers.
>
> Scope reminder: PineForge **computes**. Charts, `import`/`export`, cross-symbol
> `request.security`, live-tick semantics are out of scope and are not listed as
> findings.

## Fixed (with regression guards)

### F1 — Reused handle leaked per-run state  ✅ fixed
- **Was:** `run()` only `trades_.reserve(256)` — never cleared `trades_`,
  `net_profit_sum_`, gross sums, win/loss/even counts, open position, pending
  orders, `risk_halted_`, intraday/day counters, or source-series history. A
  reused handle (an explicitly advertised path: README "Parameter sweeps load
  one `.so` and re-run"; ABI: inputs "take effect on subsequent runs") silently
  accumulated — run 2 could start already in a position or risk-halted.
- **Fix:** `BacktestEngine::reset_run_state()` resets the full per-run surface
  (config preserved) at the top of every run loop (`src/engine_run.cpp`).
- **Guard:** `tests/test_handle_reuse_reset.cpp` (reused handle == fresh handle,
  bit-identical). Corpus unaffected (fresh handle per probe) — re-verified 233
  excellent + 1 anomaly, 0 regressions.

### F3 — Silent wrong-qty fallback on degenerate fill price  ✅ fixed
- **Was:** `calc_qty` / `calc_qty_for_type` returned the raw percent/cash
  *number* as a contract count when `fill_price <= 0` or NaN — a `$0`/NaN print
  sized "10% of equity" as **10 contracts** (`engine_orders.cpp:29,32`,
  `engine.hpp:603,605`). A non-finite fill price could also open a position at a
  NaN/Inf entry price.
- **Fix:** reject (qty `0`) on non-finite/`<=0` fill price; `execute_market_entry`
  drops a fill at a non-finite price (`src/engine_orders.cpp`).
- **Guard:** `tests/test_adversarial_ohlcv.cpp` (no silent fallback; degenerate
  NaN/Inf/zero/dup-timestamp/empty/single-bar feeds produce only finite trades).

### F5 — `pnl_pct` documentation contradicted the formula  ✅ doc fixed
- **Was:** `pineforge.h` documented `pnl_pct` as "Net realized PnL as a
  percentage of entry capital," but the formula is a **gross per-unit price
  return** `(exit/entry-1)*100` (long), ignoring commission and qty
  (`engine_orders.cpp:280`).
- **Fix (now):** the header doc now states the actual semantics and flags the
  TV-alignment as a tracked correction (below).
- **Open follow-up:** see O5.

## Open (disclosed; fix is economics- or contract-affecting)

### O2 — `syminfo.pointvalue` is a dead field
- **What:** `double pointvalue = 1.0;` is declared at `engine.hpp:203` and read
  **nowhere**. Per-trade PnL is `(fill_price - entry_price) * qty`
  (`engine_orders.cpp:277`) with no point-value multiplier.
- **Impact:** correct for crypto/equity (point value = 1); **PnL is wrong by the
  point-value factor for futures** (ES = $50/pt, etc.).
- **Why not auto-fixed:** wiring point value into PnL changes trade economics on
  any non-unit-point-value instrument and must be validated against a TV futures
  export (the public corpus is all point-value-1 crypto, so it cannot prove it).
- **Recommended fix:** multiply realized PnL (and MFE/MAE) by `syminfo_.pointvalue`
  in `emit_close_trade`; add a futures TV-parity probe (see the TV-parity spec).

### O4 — Two equity bases for percent-of-equity sizing
- **What:** the default-sizing path (`calc_qty`, `engine.hpp:601`) uses
  realized-only `current_equity() = initial_capital_ + net_profit_sum_`; the
  explicit-PERCENT path (`calc_qty_for_type`, `engine_orders.cpp:27`) uses
  mark-to-market `current_equity() + open_profit(close)`. A third spot
  (`engine.hpp:882`) also adds open profit.
- **Impact:** a percent-of-equity pyramid add taken while holding **open profit**
  is sized differently depending on the codegen path; TV sizes off equity
  including open profit, so the realized-only default path can diverge from TV.
  No corpus probe exercises this combination, so it is currently invisible.
- **Why not auto-fixed:** unifying the base changes position size (the biggest
  lever on returns) on a path real strategies use; needs a TV pyramid-in-profit
  export to pin the correct convention before changing it.
- **Recommended fix:** make both paths use `current_equity() + open_profit(...)`
  for `PERCENT_OF_EQUITY`; add a TV-parity probe sizing a percent pyramid add
  while in open profit; then convert to a green regression guard.

### O5 — `pnl_pct` does not match TV's "Net P&L %"
- **What:** even with the doc corrected (F5), the value is a gross price-return %,
  not TV's net-of-commission return-on-cost.
- **Why not auto-fixed:** recomputing it (`pnl / (entry_price*qty) * 100`, net)
  changes a published output column for every trade and should be validated
  against TV's column — which requires the field-extended validator (below) plus
  a TV export.
- **Recommended fix:** land the validator field extension (qty / pnl_pct / MFE /
  MAE, report-only) → measure the divergence vs TV → correct `pnl_pct` → promote
  to a gated field.

### O7 — MFE/MAE (excursion) fields diverge from TV  ⚠ surfaced by the field extension
- **What:** the now-extended `verify_corpus.py` (report-only) compares the
  previously-unchecked qty / pnl_pct / MFE / MAE columns. On
  `ta-macd-12-26-9-line-signal-cross-01` qty matches exactly (0%) and entry/exit
  are tight, but **MAE p90 ≈ 200%** and pnl_pct shows a p100 tail of ~1.28
  percentage points (the O5 convention gap).
- **Impact:** the engine's Adverse-excursion column does not track TV's, and was
  never compared before — a value emitted across the ABI and consumed by no
  assertion until now.
- **Why not auto-fixed:** the correct excursion convention (per-unit vs total,
  intrabar sampling) must be pinned against TV's column; needs a Family-E
  per-trade-field TV-parity probe (see the TV-parity spec) before changing the
  computation.
- **Recommended fix:** add the per-trade-field TV-parity probe; reconcile MFE/MAE
  (and pnl_pct, O5) to TV; then promote those fields from report-only to gated.

### O6 — Documented multi-thread timezone safety is not provided
- **What:** the docs promise concurrent multi-timezone safety
  (`pineforge.h:336` "process-global mutex so multi-threaded harnesses don't
  corrupt each other's wall time"; `docs/pages/timeframes.md:70` "concurrent
  backtests on different timezones are safe"; `examples-multi.md` ships a
  `ThreadPoolExecutor(max_workers=8)` pattern). But `ScopedTimezone` **releases
  the mutex in its constructor** (`timezone.cpp:41`) and the **destructor is a
  no-op** (`timezone.cpp:44`), so the caller's `localtime_r`/`mktime`
  decomposition runs unlocked, and `g_active_tz` (`timezone.cpp:14`) is a
  non-atomic global. Two threads on different chart timezones can race.
- **Impact:** wrong wall-clock decomposition (and thus session/day-boundary
  trades) under the advertised concurrent-harness pattern. The engine core
  itself is single-threaded; this is a harness-contract defect.
- **Why not auto-fixed:** the correct fix is a design choice between (a) holding
  the lock for the decomposition's lifetime (kills the lazy-TZ caching the
  current code optimizes for) and (b) a thread-local TZ cache / `localtime_r`
  with an explicit zone. Either is more than a one-line patch and changes the
  threading contract.
- **Recommended proof + fix:** a ThreadSanitizer CI lane running two concurrent
  two-timezone backtests is the deterministic oracle (the timing-based detector
  is flaky); then adopt thread-local timezone state so each handle decomposes in
  its own zone without a process-global `TZ`.

## Validation-methodology disclosures (not bugs; honest framing)

### M-p90 — the headline is a p90 claim
`scripts/verify_corpus.py` gates entry/exit/PnL at the **90th percentile** with
edge-trim and a near-zero-PnL exclusion, and compares **5 of 10** `TradeC`
fields (entry/exit time, entry/exit price, pnl). qty, pnl_pct, MFE, MAE are not
compared; direction is a match key, not a checked output. So the worst single
fill and dropped/mis-keyed trades are not bounded by the headline.

### M-mask — the matcher can mask trades
`align_by_time` pairs only same-direction trades within a 1-hour / `$3` window;
non-matches fall out of the comparison and `trim_to_common_match_window` drops
anything outside the matched span, leaving only `count_delta < 1%` to bound
unmatched volume. `scripts/verify_self_test.py` demonstrates this directly: gross
corruptions (price shift, interior drop, direction flip) **are** caught, but a
single-trade drop (sub-1%) and a **trailing-block drop** (trimmed away) are
**not**. Mitigation: the field-extended validator adds a p100/worst-case + an
unmatched-trade roll-up so masked drops surface.
