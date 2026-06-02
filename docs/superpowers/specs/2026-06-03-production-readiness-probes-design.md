---
name: production-readiness-probes
description: New probes + targeted fixes that make PineForge's production-readiness claim defensible to a skeptical reader, within the compute-only scope
metadata:
  type: project
---

# Design Spec — Production-Readiness Probes

**Date:** 2026-06-03
**Author:** gap-analysis workflow (42 agents) + source re-verification

## 0. Goal & scope

Make the "production-ready" claim defensible to a *skeptical reader* by closing
the loudest credibility gaps in the validation corpus — **without** widening
PineForge's mandate. PineForge **computes**; charts / `import`/`export` / cross-symbol
`request.security` stay **out of scope** (documented known limitations).

The corpus today is deep on engine *mechanics* (235 probes: order/composite/ta/
udt/mtf/bracket/…) but **all on one Binance ETH/USDT 15m feed**, and several
flagship README claims (bit-reproducible, reuse-one-`.so`-for-sweeps, thread-safe
multi-TZ harness, trade-for-trade parity) are pinned by **no test**.

Writing the *most convincing* probes splits three ways: **green-now** (pin
already-correct behavior), **red-on-HEAD** (writing them honestly exposes a real
bug or a doc that lies), and **TV-parity spec** (needs a TradingView export we
cannot generate here).

## 1. Verified findings (source-checked, not agent-claimed)

Every claim below was re-read in source by the author (line refs are HEAD as of
this spec).

| # | Finding | Source | Corpus green because… | Disposition |
|---|---|---|---|---|
| 1 | **Handle-reuse leaks state.** `run()` only `trades_.reserve(256)` — never clears `trades_`, zeroes `net_profit_sum_`/gross/counters, or resets position/pending/`risk_halted_`/`intraday_pnl_`. ABI advertises reuse-then-rerun. | `engine_run.cpp:59,254` | corpus uses a fresh handle per probe | **FIX now** |
| 2 | **`pointvalue` is a dead field.** Declared, read nowhere; PnL = `(fill-entry)*qty`, no point-value factor. | `engine.hpp:203` decl; `engine_orders.cpp:277` | every probe is point-value=1 crypto | **Disclose + spec** |
| 3 | **Silent wrong-qty fallback.** `calc_qty_for_type` returns the raw percent/cash number as a contract count when `fill_price<=0`/NaN. | `engine_orders.cpp:29,32` | no degenerate/NaN bars in feed | **FIX now** |
| 4 | **Two equity bases for %-equity sizing.** Default (NaN) path → realized-only `current_equity()`; explicit-PERCENT path → mark-to-market `+open_profit`. | `engine.hpp:621,601` vs `engine_orders.cpp:27` vs `engine.hpp:882` | no %-equity pyramid-add while in open profit | **Disclose + spec** |
| 5 | **`pnl_pct` contradicts its own doc.** Formula = price-ratio, ignores commission+qty; header says "percentage of entry capital". | `engine_orders.cpp:280` vs `pineforge.h:113` | field never parity-checked | **FIX now (doc) + field extend** |
| 6 | **Documented thread-safety is false.** `ScopedTimezone` unlocks in the ctor; dtor is a no-op; `g_active_tz` non-atomic — caller's `localtime_r`/`mktime` run unlocked. Docs promise "concurrent backtests on different timezones are safe". | `timezone.cpp:41,44` vs `pineforge.h:336`, `timeframes.md:70` | engine core single-threaded; never raced | **Disclose + spec** |
| 7 | **No sanitizer CI lane.** `PINEFORGE_ENABLE_SANITIZERS` exists; CI never sets it → leak/NaN/double-free probes run un-instrumented. | `CMakeLists.txt:172` vs `.github/workflows/` (absent) | — | **FIX now** |
| 8 | **`margin_liquidation_price()` hardwired `na`; no in-position liquidation.** | `engine.hpp:667` | — | **Known limitation (spec only)** |

## 2. Approach (approved)

- **Hybrid** on the reds: land all green probes; **fix low-risk** #1, #3, #5, #7
  with green regression guards; **disclose + spec** high-risk #2, #4, #6 (findings
  doc + a deliberately-failing-probe *demonstration* kept out of the default
  `ctest` gate).
- **Extend the validator's compared fields** (qty / pnl_pct / MFE / MAE — only 5
  of 10 `TradeC` fields checked today), **staged**: report-only first → measure
  divergence → correct deeply where real → promote to gating.
- **Invariant:** the default `ctest` suite and `scripts/run_corpus.sh` stay green
  at every commit (CLAUDE.md). No red tests merged; red findings are demos behind
  a non-default target or `WILL_FAIL`.

## 3. Workstreams

### WS1 — Green-now confidence probes (no engine change)

All are engine-only, verified provable by the workflow (several were compiled/
traced against `build/lib/libpineforge.a`). Each is a new `tests/test_*.cpp`
registered in `tests/CMakeLists.txt` `TEST_SOURCES`, unless noted.

1. **`test_determinism_reproducibility.cpp`** — run the same synthetic feed twice
   on **two fresh handles**; assert byte-identical `TradeC` (all 10 fields via
   `memcmp`/field-eq) **and** `ReportC` scalars. Pins README "bit-identical".
   *(The reuse-handle variant lives in WS2/#1 — fresh-handle determinism is green
   today; reused-handle is the red guard.)*
2. **`test_exit_path_segment_tiebreak.cpp`** — analytic OHLC oracle for same-bar
   **TP+SL both touched** and **gap-through** brackets. Cases A–D (verifier traced
   all four closed-form): A `mk(100,102.4,97.0)`→high-first→TP@102; B low-first→
   SL@98; C `mk(100,103,97)` tie→`O→L→H→C`→SL@98; D gap open=96≤98→open-gap
   shortcut→96.0. `slippage_=0`, mintick 0.01 → exact. Drop the redundant
   OCA case E (covered by `test_strategy_oca.cpp`) unless adding the `==`
   equal-position non-defer case.
3. **`test_dst_fall_back.cpp`** — 25-hour-day / ambiguous-hour session oracle, 11
   CHECKs (verifier built+linked green). Cases A/B = TV-aligned wall-clock
   session membership + end-exclusive boundary; F/G = UTC bucket no-aliasing;
   C/D/E = regression-lock current trading-day behavior on the fall-back day (add
   the `tm_isdst` comment explaining the 05:00-UTC midnight is intentional).
4. **`test_calendar_aggregation_wm.cpp`** — Weekly/Monthly `TimeframeAggregator`
   oracle. **Layer A (primary):** capture `AggregatedBar` (not `Bar`) and assert
   `r.sub_bar_count` == 31 (Jan), 28 (Feb), 29 (leap Feb-2024); Dec-2023→Jan-2024
   two-completion; ISO-week year boundary. **Layer B:** drive `M`,`D` through the
   security path; assert `bar.close` == last-calendar-day close (month-length
   proof) + `security_diag[0].feed_count` totals. Do **not** assert `sub_bar_count`
   off the bare `Bar` security callback (unsatisfiable).
5. **`test_ta_volume_state_oracle.cpp`** — analytic-invariant oracles for the
   TV-oracle-less TA family: MFI∈[0,100], CMO∈[-100,100], WPR∈[-100,0] with
   strict monotone endpoints; OBV/PVT/AccDist forward-sum identity. **Drop the
   RCI Pearson-on-ranks oracle** (wrong on ties — needs TV; route to WS5).
6. **`test_accounting_reconciliation.cpp`** — *slim, independent* identities only:
   `gross_profit_sum_ + gross_loss_sum_ == net_profit_sum_`;
   `win+loss+even == trades_len`; and an **independent** PnL recompute per trade
   (`dir*(exit-entry)*qty − 2·commission` from the trade's own fields, **not**
   reading `trade.pnl`) summing to `net_profit_sum_` under zero commission. Use
   member `free_report(&rep)` (no `pf_free_report`, no `reinterpret_cast`). Drop
   the circular blotter-vs-cached identities and the tautological equity one-liner.
7. **`test_market_structure_fills.cpp`** — the genuinely-new tick-size branch:
   **short-stop sub-tick FLOOR** (e.g. 99.994@0.01→99.99; FX 1.234566→1.23456 —
   no existing test hits the `is_long=false` floor with a sub-tick price) +
   cross-config linear-scaling invariant (snap delta == 1 tick for a fixed
   sub-grid fraction across mintick 0.25 vs 0.1). Drop the long-stop-ceil /
   slippage-at-0.1 cases (already covered by `test_integration.cpp:347,2612`).
8. **`test_c_abi_lifecycle.c`** (or `.cpp`) — **dlopen** the tutorial strategy
   `.so`, `dlsym` the 6 codegen + key runtime symbols, do
   create→run→read `pf_report_t`→`report_free`; assert idempotent free + NULL +
   empty-report free; assert accounting identity `Σpnl == net_profit`; in-process
   determinism. CMake-guard on `if(TARGET strategy_tutorial_macd)`, pass
   `$<TARGET_FILE:strategy_tutorial_macd>` (option `PINEFORGE_BUILD_TUTORIAL`,
   default ON). **Fix the fabricated input key** — use the real title from
   `tutorial/macd/strategy.pine` and assert the override *changes* trade count, or
   drop the override step. Reframe: this proves dlsym symbol-resolution + lifecycle
   + idempotency; leak-freedom is meaningful only under the WS2/#7 ASan lane.
9. **`scripts/verify_self_test.py`** (validator-of-the-validator) — inject a
   deliberately wrong / extra / missing trade into a copy of an `engine_trades.csv`
   and assert `verify_corpus.py` **flips to a failing tier**. Proves the scorer
   actually catches errors instead of silently dropping them. Pure post-processing,
   no TV. Wire as a ctest or a `scripts/` self-check.

### WS2 — Low-risk red fixes + green guards

1. **#1 Handle-reuse reset.** Add a single `reset_run_state()` called at the top
   of all three `run()` overloads, resetting the **full** surface (verifier-found,
   broader than P&L): `trades_.clear()`, `net_profit_sum_/gross_profit_sum_/
   gross_loss_sum_=0`, `win_trades_count_/loss_trades_count_/eventrades_count_=0`,
   `position_side_=FLAT`, `position_qty_=0`, `pyramid_entries_.clear()`,
   `pending_orders_.clear()`, `max_drawdown_/max_runup_` reset,
   `risk_halted_=false`, `intraday_pnl_`/`cons_loss_day_count_`/`last_loss_day_`
   reset, `prev_bar_timestamp_`, `security_eval_states_`. Guard:
   `test_handle_reuse_reset.cpp` — run twice on the SAME handle, assert run-2 ==
   fresh-handle run-1 (byte-identical). **Verify** `run_corpus.sh` still green
   (corpus uses fresh handles, so unaffected).
2. **#3 NaN/degenerate-bar guard.** In `calc_qty_for_type`/`calc_qty`: when
   `fill_price` is non-finite or `<=0`, **reject the fill** (return 0 / skip)
   rather than returning the percent/cash number as a contract count; document the
   chosen policy. In `run()`'s OHLC accumulation (`engine_run.cpp:178`), guard
   `running_high/low/close` against non-finite samples. Guard:
   `test_adversarial_ohlcv.cpp` — feed NaN/Inf/zero-volume/zero-price/dup-timestamp
   bars; assert all emitted `entry/exit/qty/pnl/equity` are finite and no silent
   qty fallback fires. (Oracles 1/2/4 from the workflow; drop the tautological
   equity identity, reuse WS1/#6's independent recompute.)
3. **#5 `pnl_pct` doc.** Cheapest: correct the header doc at `pineforge.h:113` to
   state the actual definition (per-unit price-return %, pre-capital), **or** —
   preferred given the field extension (WS3) — recompute `pnl_pct` as
   `pnl / (entry_price*qty) * 100` to match the documented "percentage of entry
   capital" and TV's "Net P&L %". Decide in WS3 once the TV column is compared;
   land whichever keeps corpus green. Guard: assertion in
   `test_accounting_reconciliation.cpp`.
4. **#7 Sanitizer CI lane.** Add an ASan+UBSan job to `.github/workflows/ci.yml`
   building `-DPINEFORGE_ENABLE_SANITIZERS=ON` and running `ctest` (+ optionally a
   small corpus subset / the dlopen lifecycle test). Makes the WS1/#8 + WS2/#2
   leak/NaN proofs actually enforced. No engine change.

### WS3 — Validator field extension (staged)

`scripts/verify_corpus.py` compares only entry_time/exit_time/entry_price/
exit_price/pnl. TV exports also carry **qty** (Position size), **Net PnL %**, and
**MFE/MAE** (Favorable/Adverse excursion USD); engine emits qty/pnl_pct/max_runup/
max_drawdown.

- **Stage 1 (report-only):** parse + delta qty, pnl_pct, MFE, MAE; add to the
  per-probe report + a suite-wide **p100/worst-case** + **unmatched/dropped-trade
  count** roll-up. Non-gating. This *discloses* the real coverage honestly and
  surfaces where engine fields diverge from TV.
- **Stage 2 (correct deeply):** for each field that diverges materially across the
  corpus, investigate at source and correct the engine (e.g. `pnl_pct` #5; verify
  MFE/MAE definition vs TV's per-unit excursion). Use the per-bar trace tooling in
  `pineforge-utils/per-bar-trace/` where needed. Keep corpus green after each fix.
- **Stage 3 (gate):** once a field matches across the corpus, promote it into the
  gating thresholds (extend strict/production profiles) so regressions fail.

Honest-headline disclosure (Q2, user note "extend the validation fields"):
regenerate `corpus/validation_report.md` with the new fields + p100 + drop counts;
README headline updated to disclose the gating basis (deferred to user for
marketing wording).

### WS4 — Findings doc + failing-probe demos (high-risk reds, no engine change yet)

Write `docs/production-readiness-findings.md` documenting #2 (pointvalue dead),
#4 (equity-base inconsistency), #6 (TZ thread-safety) with exact `file:line`,
repro, and the probe that would catch each. Ship the demos **out of the default
gate**:
- #2/#4: `tests/extra/` (non-default target) ctests asserting the *current* wrong
  behavior with a `// EXPECTED-RED until fixed` banner, or `WILL_FAIL` ctest props.
- #6: a **ThreadSanitizer** test reusing the existing `PINEFORGE_ENABLE_SANITIZERS`
  plumbing (not a parallel flag) + a functional concurrent two-TZ run; gated to
  the TSan lane only, since the timing oracle is flaky.

Each finding gets a recommended fix sketch so WS-future (or the user's "deep
correction") can land it as a green guard.

### WS5 — TV-parity probe spec (handoff, needs TradingView export)

Write `docs/tv-parity-probe-spec.md`: for each, a clean-room `strategy.pine`, the
exact TV capture recipe (symbol, TF, chart-TZ, `inputs.json` keys — mirror
`corpus/special-validation/README.md`), and expected outcome.

- **Instrument / asset-class realism:** equity RTH + overnight gaps + holidays
  (extend `symbol-specified/{AAPL,QQQ,SPY}` + `special-validation/SPX`); futures
  point-value + 0.25 tick; FX 5-dp; extreme price magnitudes (sub-dollar →
  six-figure); commission-per-contract; exchange-TZ vs chart-TZ daily boundaries.
- **Leverage / margin-call / liquidation** (#8): a leveraged perp probe; capture
  TV's Margin Call rows; then implement the latch to match.
- **TA volume/state family TV parity** (RCI, TSI, COG, OBV, AccDist, NVI/PVI/PVT,
  MFI, CMO, WPR, `pivot_point_levels`) — confirmed shipped suspects: TSI ×100,
  III mul-vs-div.
- **Weekly/Monthly HTF parity** (promotes WS1/#4 from synthetic to TV-backed).
- **Per-trade field parity** (qty/MFE/MAE/pnl_pct) once WS3 lands.

## 4. Verification

Per CLAUDE.md, after every change:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j
ctest --test-dir build --output-on-failure        # all green incl. new probes
./scripts/run_corpus.sh                             # no parity drift
```

New CI ASan lane: `-DPINEFORGE_ENABLE_SANITIZERS=ON` + `ctest`.

## 5. Sequencing & risk

1. WS1 (all green, zero risk) — fastest confidence win.
2. WS2 #7 (CI lane), #5 (doc) — trivial.
3. WS2 #1 (reset), #3 (NaN guard) — low risk; corpus unaffected (fresh handles,
   clean feed); guarded by new ctests.
4. WS3 Stage 1 (report-only) — zero gate risk; surfaces divergence data.
5. WS4 (findings + demos behind non-default/TSan) — no gate risk.
6. WS3 Stage 2/3 + WS5 — **deeper correction**; each engine edit re-runs the full
   corpus; equity-base/pointvalue/pnl_pct changes can move trade economics, so each
   is its own reviewed change with a cold corpus sweep (HANDOFF.md: always
   `rm -rf build/_validator_cache` before final parity).

## 6. Out of scope (unchanged known limitations)

Charts/plot/label/alerts; `import`/`export`/`library`; cross-symbol / lookahead
lower-TF `request.security`; TV-exact PRNG; live/realtime tick semantics (`varip`,
`calc_on_every_tick`). PineForge computes; these stay external.
