# Trade comparison


Each strategy runs through PineForge, PyneCore and (where a `strategy_vbt.py` port exists) vectorbt on the same 53,929-bar Binance ETH/USDT-USDT 15m feed and is graded against its TradingView tape by the canonical corpus rubric, `scripts/verify_corpus.py::analyze_strategy` (align-then-trim common window, fragment consolidation, range-end mark pairing, exact-count and ≥99% coverage gates for *excellent*, strict/production threshold profiles, `inputs.json` overrides). PineTS has no strategy backtester upstream and is excluded here.

### 001-analyzer-anvil-percent-costs-01  *(corpus, profile: strict)*

- TV closed trades: **325**
- **PineForge** 🟢 **excellent**  (emitted 325; in window engine 325 / TV 325; matched 325; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 450; in window engine 325 / TV 325; matched 325; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 002-analyzer-parity-percent-of-equity-sizing-01  *(corpus, profile: strict)*

- TV closed trades: **57**
- **PineForge** 🟢 **excellent**  (emitted 57; in window engine 57 / TV 57; matched 57; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 81; in window engine 57 / TV 57; matched 57; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.0453%`
    - gates: pnl p90 2.0453%
- **vectorbt** 🟢 **strong**  (emitted 81; in window engine 57 / TV 57; matched 57; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.0450%`
    - gates: pnl p90 2.0450%

### 003-array-atlas-momentum-rotation-01  *(corpus, profile: strict)*

- TV closed trades: **930**
- **PineForge** 🟢 **excellent**  (emitted 930; in window engine 930 / TV 930; matched 930; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1301; in window engine 930 / TV 930; matched 930; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `1.9647%`
    - gates: pnl p90 1.9647%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 004-barstate-isconfirmed-magnifier-off-01b  *(corpus, profile: strict)*

- TV closed trades: **871**
- **PineForge** 🟢 **excellent**  (emitted 871; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1210; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **excellent**  (emitted 1210; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`

### 005-bracket-compass-partial-ladder-01  *(corpus, profile: strict)*

- TV closed trades: **836**
- **PineForge** 🟢 **excellent**  (emitted 836; in window engine 417 / TV 417; matched 417; coverage 99.8%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1166; in window engine 417 / TV 417; matched 417; coverage 99.8%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 006-bracket-exit-tp-sl-fixed-01  *(corpus, profile: strict)*

- TV closed trades: **366**
- **PineForge** 🟢 **excellent**  (emitted 366; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 562; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **strong**  (emitted 562; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.1584%`
    - PnL   p90:   `28.4827%`
    - gates: exit p90 0.1584%; pnl p90 28.4827%

### 007-bracket-tp-sl-oca-reduce-isolate-01  *(corpus, profile: strict)*

- TV closed trades: **2240**
- **PineForge** 🟢 **excellent**  (emitted 2240; in window engine 2240 / TV 2240; matched 2240; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 3120; in window engine 2240 / TV 2240; matched 2240; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: distinct-entry multiplicity Δ 90
- **vectorbt** 🟢 **strong**  (emitted 2989; in window engine 2150 / TV 2240; matched 2150; coverage 96.0%)
    - count delta: `4.0179%` (abs 90)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 4.02%; coverage 96.0%; distinct-entry multiplicity Δ 90

### 008-bracket-trail-points-no-offset-explicit-01  *(corpus, profile: production)*

- TV closed trades: **782**
- **PineForge** 🟢 **excellent**  (emitted 782; in window engine 782 / TV 782; matched 782; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1109; in window engine 782 / TV 782; matched 782; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟠 **weak**  (emitted 53; in window engine 40 / TV 749; matched 40; coverage 5.1%)
    - count delta: `94.6595%` (abs 709)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.5307%`
    - PnL   p90:   `15561.2500%`
    - gates: count Δ 94.66%; exit p90 0.5307%; pnl p90 15561.2500%; coverage 5.1%

### 009-bracket-trail-points-with-offset-only-01  *(corpus, profile: production)*

- TV closed trades: **710**
- **PineForge** 🟢 **excellent**  (emitted 710; in window engine 710 / TV 710; matched 710; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1090; in window engine 710 / TV 710; matched 710; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 010-cap-gatekeeper-intraday-risk-01  *(corpus, profile: strict)*

- TV closed trades: **302**
- **PineForge** 🟢 **excellent**  (emitted 302; in window engine 302 / TV 302; matched 302; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 418; in window engine 302 / TV 302; matched 302; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 011-composite-4emarsi-rsi-pullback-latch-01  *(corpus, profile: strict)*

- TV closed trades: **816**
- **PineForge** 🟢 **excellent**  (emitted 816; in window engine 816 / TV 816; matched 816; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1159; in window engine 815 / TV 815; matched 815; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **excellent**  (emitted 1159; in window engine 815 / TV 815; matched 815; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`

### 012-composite-boscurv-integration-01  *(corpus, profile: strict)*

- TV closed trades: **1026**
- **PineForge** 🟢 **excellent**  (emitted 1026; in window engine 1026 / TV 1026; matched 1026; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1433; in window engine 1026 / TV 1026; matched 1026; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 013-composite-boscurv-pivot-bos-trigger-01  *(corpus, profile: strict)*

- TV closed trades: **906**
- **PineForge** 🟢 **excellent**  (emitted 906; in window engine 906 / TV 906; matched 906; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1246; in window engine 905 / TV 905; matched 905; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 014-composite-ies-adx-regime-classify-01  *(corpus, profile: strict)*

- TV closed trades: **682**
- **PineForge** 🟢 **excellent**  (emitted 682; in window engine 682 / TV 682; matched 682; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 941; in window engine 683 / TV 682; matched 682; coverage 100.0%)
    - count delta: `0.1464%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.15%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 015-composite-ies-cooldown-daily-cap-01  *(corpus, profile: strict)*

- TV closed trades: **727**
- **PineForge** 🟢 **excellent**  (emitted 727; in window engine 727 / TV 727; matched 727; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1013; in window engine 727 / TV 727; matched 727; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 016-composite-kanuck-calc-on-every-tick-01  *(corpus, profile: strict)*

- TV closed trades: **361**
- **PineForge** 🟢 **excellent**  (emitted 361; in window engine 361 / TV 361; matched 361; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 508; in window engine 361 / TV 361; matched 361; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 017-composite-kkb-ema-atr-breakout-band-01  *(corpus, profile: strict)*

- TV closed trades: **641**
- **PineForge** 🟢 **excellent**  (emitted 641; in window engine 641 / TV 641; matched 641; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 939; in window engine 641 / TV 641; matched 641; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 018-composite-kkb-kalman-filter-1d-01  *(corpus, profile: strict)*

- TV closed trades: **5487**
- **PineForge** 🟢 **excellent**  (emitted 5487; in window engine 5487 / TV 5487; matched 5487; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 7639; in window engine 5488 / TV 5487; matched 5487; coverage 100.0%)
    - count delta: `0.0182%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.02%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 019-composite-kkb-margin-100-pct-01  *(corpus, profile: strict)*

- TV closed trades: **2522**
- **PineForge** 🟢 **excellent**  (emitted 2522; in window engine 2522 / TV 2522; matched 2522; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3531; in window engine 2522 / TV 2522; matched 2522; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 020-composite-marketshift-pivot-state-machine-01  *(corpus, profile: strict)*

- TV closed trades: **906**
- **PineForge** 🟢 **excellent**  (emitted 906; in window engine 905 / TV 905; matched 905; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1246; in window engine 905 / TV 905; matched 905; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 021-composite-scalping-integration-01  *(corpus, profile: strict)*

- TV closed trades: **3097**
- **PineForge** 🟢 **excellent**  (emitted 3097; in window engine 3097 / TV 3097; matched 3097; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4352; in window engine 3097 / TV 3097; matched 3097; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **strong**  (emitted 4355; in window engine 3097 / TV 3097; matched 3097; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.4058%`
    - PnL   p90:   `99.1972%`
    - gates: exit p90 0.4058%; pnl p90 99.1972%

### 022-composite-trendmaster-line-new-projection-01  *(corpus, profile: strict)*

- TV closed trades: **1592**
- **PineForge** 🟢 **excellent**  (emitted 1592; in window engine 1592 / TV 1592; matched 1592; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2234; in window engine 1591 / TV 1591; matched 1591; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 023-composite-trendmaster-three-tier-ema-state-01  *(corpus, profile: strict)*

- TV closed trades: **224**
- **PineForge** 🟢 **excellent**  (emitted 224; in window engine 224 / TV 224; matched 224; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 312; in window engine 220 / TV 220; matched 220; coverage 98.2%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: Excellent (224/224 matched, coverage 100.0%). Supersedes the earlier park-at-strong note, which mis-attributed the residual to an unfixable 'TV deep-backtest window-origin artifact'. The residual was a harness-config mismatch of this probe's own making: the previous inputs.json deliberately selected the default full-history 2020-origin 15m feed ('so EMA state warms before the TV comparison window'), while the TV export carries a range-local origin with zero pre-range history (unified range-local seeding, findings 331-333). The engine's three EMAs were therefore converged for five years before the window and its var-based stack-state edge detector had already latched a different regime at the boundary, so the first transitions diverged (TV 4 trades vs engine 2) before resyncing for 220 matched. Origin established by oracle, not assumption: a two-axis sweep of ohlcv_start_ms x chart_ema_na_warmup (2025-03-28..2025-04-02 at 6h steps, then +/-8 bars at 15m granularity), each point scored with scripts/verify_corpus.py, has exactly one setting that reaches 224/224 with zero unmatched TV trades - ohlcv_start_ms=1743379200000 with chart_ema_na_warmup=1, bit-for-bit the config the sibling composite-trendmaster-integration-01 already ships. Coverage rises 98.2% -> 100.0%; no coverage was trimmed and trade_start / trading_is_active semantics are unchanged.
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 024-composite-trendmaster-trend-momentum-structure-gate-01  *(corpus, profile: strict)*

- TV closed trades: **362**
- **PineForge** 🟢 **excellent**  (emitted 362; in window engine 362 / TV 362; matched 362; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 533; in window engine 361 / TV 361; matched 361; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 025-composite-vcp-rsi-smooth-divergence-01  *(corpus, profile: strict)*

- TV closed trades: **142**
- **PineForge** 🟢 **excellent**  (emitted 142; in window engine 142 / TV 142; matched 142; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 214; in window engine 142 / TV 142; matched 142; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 026-composite-vcp-vol-zscore-anomaly-01  *(corpus, profile: strict)*

- TV closed trades: **591**
- **PineForge** 🟢 **excellent**  (emitted 591; in window engine 591 / TV 591; matched 591; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 831; in window engine 591 / TV 591; matched 591; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 027-composite-wunderscalper-integration-01  *(corpus, profile: strict)*

- TV closed trades: **3097**
- **PineForge** 🟢 **excellent**  (emitted 3097; in window engine 3097 / TV 3097; matched 3097; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4352; in window engine 3097 / TV 3097; matched 3097; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 028-drawing-line-level-breakout  *(corpus, profile: strict)*

- TV closed trades: **2265**
- **PineForge** 🟢 **excellent**  (emitted 2265; in window engine 2265 / TV 2265; matched 2265; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3173; in window engine 2265 / TV 2265; matched 2265; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 029-drawing-visual-noise-geometry  *(corpus, profile: strict)*

- TV closed trades: **2969**
- **PineForge** 🟢 **excellent**  (emitted 2969; in window engine 2969 / TV 2969; matched 2969; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4210; in window engine 2969 / TV 2969; matched 2969; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 030-input-source-subscript-hl2-01  *(corpus, profile: strict)*

- TV closed trades: **16347**
- **PineForge** 🟢 **excellent**  (emitted 16347; in window engine 16347 / TV 16347; matched 16347; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 23164; in window engine 16348 / TV 16347; matched 16347; coverage 100.0%)
    - count delta: `0.0061%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.01%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 031-magnifier-tick-dist-volume-weighted-on-01  *(corpus, profile: strict)*

- TV closed trades: **871**
- **PineForge** 🟢 **excellent**  (emitted 871; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1210; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟡 **moderate**  (emitted 1210; in window engine 871 / TV 871; matched 871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `1.8304%`
    - PnL   p90:   `3330.6122%`
    - gates: exit p90 1.8304%; pnl p90 3330.6122%

### 032-map-mosaic-regime-weight-01  *(corpus, profile: strict)*

- TV closed trades: **836**
- **PineForge** 🟢 **excellent**  (emitted 836; in window engine 836 / TV 836; matched 836; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1161; in window engine 836 / TV 836; matched 836; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 033-math-kiln-power-efficiency-01  *(corpus, profile: strict)*

- TV closed trades: **28**
- **PineForge** 🟢 **excellent**  (emitted 28; in window engine 28 / TV 28; matched 28; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 40; in window engine 28 / TV 28; matched 28; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 034-matrix-bool-mask-transpose-roundtrip-01  *(corpus, profile: strict)*

- TV closed trades: **774**
- **PineForge** 🟢 **excellent**  (emitted 774; in window engine 774 / TV 774; matched 774; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1078; in window engine 782 / TV 771; matched 745; coverage 96.3%)
    - count delta: `1.4066%` (abs 11)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 1.41%; coverage 96.3%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 035-matrix-cadence-transpose-pairs-01  *(corpus, profile: strict)*

- TV closed trades: **2358**
- **PineForge** 🟢 **excellent**  (emitted 2358; in window engine 2358 / TV 2358; matched 2358; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3317; in window engine 2358 / TV 2358; matched 2358; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 036-matrix-eigen-covariance-01  *(corpus, profile: strict)*

- TV closed trades: **542**
- **PineForge** 🟢 **excellent**  (emitted 542; in window engine 542 / TV 542; matched 542; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 766; in window engine 542 / TV 542; matched 542; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 037-mtf-daily-array-median-percentrank-01  *(corpus, profile: strict)*

- TV closed trades: **362**
- **PineForge** 🟢 **excellent**  (emitted 362; in window engine 362 / TV 362; matched 362; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 84; in window engine 52 / TV 356; matched 26; coverage 7.2%)
    - count delta: `85.3933%` (abs 304)
    - entry p90:   `0.0000%`
    - exit  p90:   `17.9900%`
    - PnL   p90:   `1941.0589%`
    - gates: count Δ 85.39%; exit p90 17.9900%; pnl p90 1941.0589%; coverage 7.2%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 038-mtf-dual-tf-60-240-rising-01  *(corpus, profile: strict)*

- TV closed trades: **736**
- **PineForge** 🟢 **excellent**  (emitted 736; in window engine 736 / TV 736; matched 736; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1106; in window engine 736 / TV 736; matched 736; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 039-mtf-htf-60-close-change-baseline-01  *(corpus, profile: strict)*

- TV closed trades: **8779**
- **PineForge** 🟢 **excellent**  (emitted 8779; in window engine 8779 / TV 8779; matched 8779; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 13475; in window engine 8781 / TV 8779; matched 8779; coverage 100.0%)
    - count delta: `0.0228%` (abs 2)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.02%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 040-mtf-htf-weekly-sma-cross-01  *(corpus, profile: strict)*

- TV closed trades: **102**
- **PineForge** 🟢 **excellent**  (emitted 102; in window engine 102 / TV 102; matched 102; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 130; in window engine 102 / TV 92; matched 16; coverage 15.7%)
    - count delta: `9.8039%` (abs 10)
    - entry p90:   `0.0000%`
    - exit  p90:   `4.7589%`
    - PnL   p90:   `127.9468%`
    - gates: count Δ 9.80%; exit p90 4.7589%; pnl p90 127.9468%; coverage 15.7%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 041-mtf-orbit-trend-01  *(corpus, profile: strict)*

- TV closed trades: **289**
- **PineForge** 🟢 **excellent**  (emitted 289; in window engine 289 / TV 289; matched 288; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 403; in window engine 289 / TV 289; matched 288; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 042-mtf-triple-tf-macd-hist-confluence-01  *(corpus, profile: strict)*

- TV closed trades: **24**
- **PineForge** 🟢 **excellent**  (emitted 24; in window engine 24 / TV 24; matched 23; coverage 95.8%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🔴 **minimal**  (emitted 3; in window engine 0 / TV 0; matched 0; coverage 0.0%)
    - count delta: `87.5000%` (abs 21)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: no aligned trades
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 043-na-deep-history-int-na-01  *(corpus, profile: strict)*

- TV closed trades: **106**
- **PineForge** 🟢 **excellent**  (emitted 106; in window engine 106 / TV 106; matched 106; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 146; in window engine 106 / TV 106; matched 106; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 044-oca-multi-bracket-isolation-01  *(corpus, profile: strict)*

- TV closed trades: **1244**
- **PineForge** 🟢 **excellent**  (emitted 1244; in window engine 622 / TV 622; matched 622; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1724; in window engine 622 / TV 622; matched 622; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟡 **moderate**  (emitted 1763; in window engine 710 / TV 622; matched 622; coverage 100.0%)
    - count delta: `12.3944%` (abs 88)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.6360%`
    - PnL   p90:   `139.3178%`
    - gates: count Δ 12.39%; exit p90 0.6360%; pnl p90 139.3178%

### 045-oca-raw-strategy-order-reduce-01  *(corpus, profile: strict)*

- TV closed trades: **366**
- **PineForge** 🟢 **excellent**  (emitted 366; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 562; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0005%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **excellent**  (emitted 562; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0003%`
    - PnL   p90:   `0.0000%`

### 046-order-close-immediate-vs-next-bar-01  *(corpus, profile: strict)*

- TV closed trades: **732**
- **PineForge** 🟢 **excellent**  (emitted 732; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1124; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 047-order-cross-exit-close-same-pass-01  *(corpus, profile: strict)*

- TV closed trades: **732**
- **PineForge** 🟢 **excellent**  (emitted 732; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1124; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 048-order-deferred-flip-guaranteed-gap-stops-01  *(corpus, profile: strict)*

- TV closed trades: **792**
- **PineForge** 🟢 **excellent**  (emitted 792; in window engine 792 / TV 792; matched 792; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1124; in window engine 792 / TV 792; matched 792; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **strong**  (emitted 1124; in window engine 792 / TV 792; matched 792; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `92.3077%`
    - gates: pnl p90 92.3077%

### 049-order-dual-four-bar-stop-no-close-01  *(corpus, profile: strict)*

- TV closed trades: **366**
- **PineForge** 🟢 **excellent**  (emitted 366; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 562; in window engine 366 / TV 366; matched 366; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 050-order-dual-stop-open-low-first-path-01  *(corpus, profile: strict)*

- TV closed trades: **189**
- **PineForge** 🟢 **excellent**  (emitted 189; in window engine 189 / TV 189; matched 189; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 296; in window engine 189 / TV 189; matched 189; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 051-order-dual-stop-source-order-short-first-01  *(corpus, profile: strict)*

- TV closed trades: **365**
- **PineForge** 🟢 **excellent**  (emitted 365; in window engine 365 / TV 365; matched 365; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 561; in window engine 365 / TV 365; matched 365; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 052-order-entry-implicit-reversal-exit-01  *(corpus, profile: strict)*

- TV closed trades: **1098**
- **PineForge** 🟢 **excellent**  (emitted 1098; in window engine 1098 / TV 1098; matched 1098; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1686; in window engine 1098 / TV 1098; matched 1098; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 053-order-flip-stop-no-paired-close-01  *(corpus, profile: strict)*

- TV closed trades: **724**
- **PineForge** 🟢 **excellent**  (emitted 724; in window engine 724 / TV 724; matched 724; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1032; in window engine 723 / TV 723; matched 723; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 054-order-keystone-limit-replace-01  *(corpus, profile: strict)*

- TV closed trades: **686**
- **PineForge** 🟢 **excellent**  (emitted 686; in window engine 686 / TV 686; matched 686; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 938; in window engine 686 / TV 686; matched 686; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.4713%`
    - gates: pnl p90 2.4713%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 055-order-one-side-four-bar-far-opposite-01  *(corpus, profile: strict)*

- TV closed trades: **349**
- **PineForge** 🟢 **excellent**  (emitted 349; in window engine 349 / TV 349; matched 349; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 539; in window engine 349 / TV 349; matched 349; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 056-order-process-on-close-true-01  *(corpus, profile: strict)*

- TV closed trades: **857**
- **PineForge** 🟢 **excellent**  (emitted 857; in window engine 857 / TV 857; matched 857; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1194; in window engine 857 / TV 857; matched 857; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 057-order-same-id-market-entry-repeat-01  *(corpus, profile: strict)*

- TV closed trades: **732**
- **PineForge** 🟢 **excellent**  (emitted 732; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1124; in window engine 732 / TV 732; matched 732; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 058-order-same-id-stop-after-flat-01  *(corpus, profile: strict)*

- TV closed trades: **703**
- **PineForge** 🟢 **excellent**  (emitted 703; in window engine 703 / TV 703; matched 703; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1080; in window engine 703 / TV 703; matched 703; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 059-order-stop-entry-cancel-opposite-01  *(corpus, profile: strict)*

- TV closed trades: **1739**
- **PineForge** 🟢 **excellent**  (emitted 1741; in window engine 1738 / TV 1738; matched 1738; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2407; in window engine 1738 / TV 1738; matched 1738; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **strong**  (emitted 2410; in window engine 1738 / TV 1738; matched 1738; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0817%`
    - PnL   p90:   `90.0000%`
    - gates: exit p90 0.0817%; pnl p90 90.0000%

### 060-order-stop-entry-touch-boundary-01  *(corpus, profile: strict)*

- TV closed trades: **548**
- **PineForge** 🟢 **excellent**  (emitted 548; in window engine 548 / TV 548; matched 548; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 840; in window engine 548 / TV 548; matched 548; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 061-pyramid-deferred-flip-close-all-01  *(corpus, profile: strict)*

- TV closed trades: **2356**
- **PineForge** 🟢 **excellent**  (emitted 2356; in window engine 2356 / TV 2356; matched 2356; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3282; in window engine 2356 / TV 2356; matched 2355; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no vectorbt_trades.csv

### 062-pyramid-flip-stop-pyramiding-2-01  *(corpus, profile: strict)*

- TV closed trades: **843**
- **PineForge** 🟢 **excellent**  (emitted 843; in window engine 843 / TV 843; matched 843; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1195; in window engine 840 / TV 840; matched 840; coverage 99.6%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 063-session-borough-new-york-01  *(corpus, profile: strict)*

- TV closed trades: **159**
- **PineForge** 🟢 **excellent**  (emitted 159; in window engine 159 / TV 159; matched 159; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 229; in window engine 159 / TV 159; matched 159; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 064-session-ny-spring-forward-dst-01  *(corpus, profile: strict)*

- TV closed trades: **396**
- **PineForge** 🟢 **excellent**  (emitted 396; in window engine 396 / TV 396; matched 396; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 562; in window engine 396 / TV 396; matched 396; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** 🟢 **excellent**  (emitted 562; in window engine 396 / TV 396; matched 396; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`

### 065-stats-ledger-outcome-throttle-01  *(corpus, profile: strict)*

- TV closed trades: **425**
- **PineForge** 🟢 **excellent**  (emitted 425; in window engine 425 / TV 425; matched 425; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 585; in window engine 425 / TV 425; matched 425; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 066-syntax-glyph-string-regex-01  *(corpus, profile: strict)*

- TV closed trades: **271**
- **PineForge** 🟢 **excellent**  (emitted 271; in window engine 271 / TV 271; matched 271; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 388; in window engine 271 / TV 271; matched 271; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 067-ta-aperture-cog-linreg-01  *(corpus, profile: strict)*

- TV closed trades: **530**
- **PineForge** 🟢 **excellent**  (emitted 530; in window engine 530 / TV 530; matched 530; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 746; in window engine 530 / TV 530; matched 530; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 068-ta-bb-rsi-mean-reversion-01  *(corpus, profile: strict)*

- TV closed trades: **496**
- **PineForge** 🟢 **excellent**  (emitted 496; in window engine 496 / TV 496; matched 496; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 765; in window engine 496 / TV 496; matched 496; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 069-ta-closedtrades-risk-introspection-01  *(corpus, profile: strict)*

- TV closed trades: **751**
- **PineForge** 🟢 **excellent**  (emitted 751; in window engine 751 / TV 751; matched 751; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1143; in window engine 751 / TV 751; matched 751; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 070-ta-cog-10-signal-cross-01  *(corpus, profile: strict)*

- TV closed trades: **5803**
- **PineForge** 🟢 **excellent**  (emitted 5803; in window engine 5803 / TV 5803; matched 5803; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 8287; in window engine 5803 / TV 5803; matched 5803; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 071-ta-dual-thrust-open-anchored-range-01  *(corpus, profile: strict)*

- TV closed trades: **2871**
- **PineForge** 🟢 **excellent**  (emitted 2871; in window engine 2871 / TV 2871; matched 2871; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 4329; in window engine 2872 / TV 2871; matched 2871; coverage 100.0%)
    - count delta: `0.0348%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.03%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 072-ta-highestbars-lowestbars-breakout-01  *(corpus, profile: strict)*

- TV closed trades: **1587**
- **PineForge** 🟢 **excellent**  (emitted 1587; in window engine 1587 / TV 1587; matched 1587; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2448; in window engine 1587 / TV 1587; matched 1587; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 073-ta-inside-bar-engulfing-01  *(corpus, profile: strict)*

- TV closed trades: **3614**
- **PineForge** 🟢 **excellent**  (emitted 3614; in window engine 3614 / TV 3614; matched 3614; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4896; in window engine 3613 / TV 3613; matched 3613; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 074-ta-macd-histogram-reversal-01  *(corpus, profile: strict)*

- TV closed trades: **2816**
- **PineForge** 🟢 **excellent**  (emitted 2816; in window engine 2816 / TV 2816; matched 2816; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4318; in window engine 2816 / TV 2816; matched 2816; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 075-ta-mantle-keltner-regime-01  *(corpus, profile: strict)*

- TV closed trades: **379**
- **PineForge** 🟢 **excellent**  (emitted 379; in window engine 379 / TV 379; matched 379; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 540; in window engine 379 / TV 379; matched 379; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 076-ta-obv-ema-cross-01  *(corpus, profile: strict)*

- TV closed trades: **5296**
- **PineForge** 🟢 **excellent**  (emitted 5296; in window engine 5296 / TV 5296; matched 5296; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 7439; in window engine 5296 / TV 5296; matched 5296; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 077-ta-pivot-array-unshift-pop-01  *(corpus, profile: strict)*

- TV closed trades: **829**
- **PineForge** 🟢 **excellent**  (emitted 829; in window engine 829 / TV 829; matched 829; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1247; in window engine 829 / TV 829; matched 829; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 078-ta-pivot-atr-stop-target-01  *(corpus, profile: strict)*

- TV closed trades: **1620**
- **PineForge** 🟢 **excellent**  (emitted 1620; in window engine 1620 / TV 1620; matched 1620; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2474; in window engine 1620 / TV 1620; matched 1620; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 079-ta-pivot-confirmed-break-01  *(corpus, profile: strict)*

- TV closed trades: **1115**
- **PineForge** 🟢 **excellent**  (emitted 1115; in window engine 1115 / TV 1115; matched 1115; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1544; in window engine 1115 / TV 1115; matched 1115; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 080-ta-plumbline-pvt-vwma-01  *(corpus, profile: strict)*

- TV closed trades: **1371**
- **PineForge** 🟢 **excellent**  (emitted 1371; in window engine 1371 / TV 1371; matched 1370; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1896; in window engine 1371 / TV 1371; matched 1370; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 081-ta-rsi-bb-self-bands-01  *(corpus, profile: strict)*

- TV closed trades: **350**
- **PineForge** 🟢 **excellent**  (emitted 350; in window engine 350 / TV 350; matched 350; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 532; in window engine 350 / TV 350; matched 350; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 082-ta-rsi14-cross-50-01  *(corpus, profile: strict)*

- TV closed trades: **4690**
- **PineForge** 🟢 **excellent**  (emitted 4690; in window engine 4690 / TV 4690; matched 4690; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 6507; in window engine 4690 / TV 4690; matched 4690; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 083-ta-rsi14-gt60-lt45-no-matrix-01  *(corpus, profile: strict)*

- TV closed trades: **785**
- **PineForge** 🟢 **excellent**  (emitted 785; in window engine 785 / TV 785; matched 785; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1086; in window engine 785 / TV 785; matched 785; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 084-ta-sar-flip-entry-01  *(corpus, profile: strict)*

- TV closed trades: **3082**
- **PineForge** 🟢 **excellent**  (emitted 3082; in window engine 3082 / TV 3082; matched 3082; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4425; in window engine 3082 / TV 3082; matched 3082; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 085-ta-stdev-sma-expansion-break-01  *(corpus, profile: strict)*

- TV closed trades: **878**
- **PineForge** 🟢 **excellent**  (emitted 878; in window engine 878 / TV 878; matched 878; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1241; in window engine 877 / TV 877; matched 877; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 086-ta-str-match-regex-filter-01  *(corpus, profile: strict)*

- TV closed trades: **1917**
- **PineForge** 🟢 **excellent**  (emitted 1918; in window engine 1916 / TV 1916; matched 1916; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 2160; in window engine 1514 / TV 1917; matched 982; coverage 51.2%)
    - count delta: `21.0224%` (abs 403)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.5374%`
    - PnL   p90:   `273.2000%`
    - gates: count Δ 21.02%; exit p90 0.5374%; pnl p90 273.2000%; coverage 51.2%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 087-ta-torque-tsi-signal-01  *(corpus, profile: strict)*

- TV closed trades: **402**
- **PineForge** 🟢 **excellent**  (emitted 402; in window engine 402 / TV 402; matched 402; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 576; in window engine 402 / TV 402; matched 402; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 088-ta-vwma-vs-sma-divergence-01  *(corpus, profile: strict)*

- TV closed trades: **2576**
- **PineForge** 🟢 **excellent**  (emitted 2576; in window engine 2576 / TV 2576; matched 2576; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3972; in window engine 2576 / TV 2576; matched 2576; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 089-ta-wpr-14-bands-01  *(corpus, profile: strict)*

- TV closed trades: **1847**
- **PineForge** 🟢 **excellent**  (emitted 1847; in window engine 1847 / TV 1847; matched 1847; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2624; in window engine 1847 / TV 1847; matched 1847; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 090-ta-zenith-rci-wpr-01  *(corpus, profile: strict)*

- TV closed trades: **949**
- **PineForge** 🟢 **excellent**  (emitted 949; in window engine 949 / TV 949; matched 949; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1329; in window engine 949 / TV 949; matched 949; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 091-udt-method-drives-strategy-entry-01  *(corpus, profile: strict)*

- TV closed trades: **1635**
- **PineForge** 🟢 **excellent**  (emitted 1635; in window engine 1635 / TV 1635; matched 1635; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 2271; in window engine 1636 / TV 1635; matched 1635; coverage 100.0%)
    - count delta: `0.0611%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.06%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 092-udt-method-extra-primitive-args-01  *(corpus, profile: strict)*

- TV closed trades: **2504**
- **PineForge** 🟢 **excellent**  (emitted 2504; in window engine 2504 / TV 2504; matched 2504; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3504; in window engine 2504 / TV 2504; matched 2504; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 093-udt-method-in-switch-arms-01  *(corpus, profile: strict)*

- TV closed trades: **402**
- **PineForge** 🟢 **excellent**  (emitted 402; in window engine 402 / TV 402; matched 402; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 583; in window engine 402 / TV 402; matched 402; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 094-udt-method-in-while-loop-01  *(corpus, profile: strict)*

- TV closed trades: **306**
- **PineForge** 🟢 **excellent**  (emitted 306; in window engine 306 / TV 306; matched 306; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 423; in window engine 306 / TV 306; matched 306; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 095-udt-method-reads-strategy-state-01  *(corpus, profile: strict)*

- TV closed trades: **705**
- **PineForge** 🟢 **excellent**  (emitted 705; in window engine 705 / TV 705; matched 705; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 982; in window engine 705 / TV 705; matched 705; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 096-udt-method-tuple-return-destructure-01  *(corpus, profile: strict)*

- TV closed trades: **1095**
- **PineForge** 🟢 **excellent**  (emitted 1095; in window engine 1095 / TV 1095; matched 1095; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1541; in window engine 1094 / TV 1094; matched 1094; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 097-udt-method-udt-return-from-func-01  *(corpus, profile: strict)*

- TV closed trades: **132**
- **PineForge** 🟢 **excellent**  (emitted 132; in window engine 132 / TV 132; matched 132; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 192; in window engine 132 / TV 132; matched 132; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 098-udt-method-var-instance-streak-01  *(corpus, profile: strict)*

- TV closed trades: **1007**
- **PineForge** 🟢 **excellent**  (emitted 1007; in window engine 1007 / TV 1007; matched 1007; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1469; in window engine 1007 / TV 1007; matched 1007; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 099-udt-tessellate-tuple-method-01  *(corpus, profile: strict)*

- TV closed trades: **426**
- **PineForge** 🟢 **excellent**  (emitted 426; in window engine 426 / TV 426; matched 426; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 606; in window engine 426 / TV 426; matched 426; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 100-vwap-bands-mean-reversion-2sigma-01  *(corpus, profile: strict)*

- TV closed trades: **241**
- **PineForge** 🟢 **excellent**  (emitted 241; in window engine 241 / TV 241; matched 241; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 354; in window engine 241 / TV 241; matched 241; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 101-3commas-3commas-bch-overbought-rsi-fade-short-indicator  *(closed, profile: strict)*

- TV closed trades: **288**
- **PineForge** 🟢 **excellent**  (emitted 288; in window engine 288 / TV 288; matched 288; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 400; in window engine 288 / TV 288; matched 288; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 102-3commas-3commas-pol-grid-bot-long-strategy  *(closed, profile: strict)*

- TV closed trades: **354**
- **PineForge** 🟢 **excellent**  (emitted 354; in window engine 198 / TV 198; matched 198; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 555; in window engine 180 / TV 194; matched 180; coverage 91.4%)
    - count delta: `7.2165%` (abs 14)
    - entry p90:   `0.0000%`
    - exit  p90:   `22.1780%`
    - PnL   p90:   `373.1014%`
    - gates: count Δ 7.22%; exit p90 22.1780%; pnl p90 373.1014%; coverage 91.4%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 103-3commas-eth-grid-bot-long-strategy  *(closed, profile: strict)*

- TV closed trades: **332**
- **PineForge** 🟢 **excellent**  (emitted 332; in window engine 179 / TV 179; matched 179; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0850%`
- **PyneCore** 🟡 **moderate**  (emitted 339; in window engine 192 / TV 178; matched 178; coverage 100.0%)
    - count delta: `7.2917%` (abs 14)
    - entry p90:   `0.0000%`
    - exit  p90:   `4.0576%`
    - PnL   p90:   `355.8417%`
    - gates: count Δ 7.29%; exit p90 4.0576%; pnl p90 355.8417%; distinct-entry multiplicity Δ 7
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 104-3commas-gram-rsi-strategy-3commas  *(closed, profile: strict)*

- TV closed trades: **43**
- **PineForge** 🟢 **excellent**  (emitted 43; in window engine 43 / TV 43; matched 43; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 74; in window engine 43 / TV 43; matched 43; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 105-3commas-heikin-ashi-rsi-fade-short-strategy  *(closed, profile: strict)*

- TV closed trades: **392**
- **PineForge** 🟢 **excellent**  (emitted 392; in window engine 392 / TV 392; matched 392; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 553; in window engine 391 / TV 391; matched 391; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.6549%`
    - gates: pnl p90 2.6549%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 106-3commas-sol-rsi-dca-long-strategy  *(closed, profile: strict)*

- TV closed trades: **29**
- **PineForge** 🟢 **excellent**  (emitted 29; in window engine 29 / TV 29; matched 29; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 29; in window engine 29 / TV 29; matched 29; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 107-3commas-xmr-grid-bot-long-strategy  *(closed, profile: strict)*

- TV closed trades: **371**
- **PineForge** 🟢 **excellent**  (emitted 371; in window engine 208 / TV 208; matched 208; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 585; in window engine 188 / TV 204; matched 188; coverage 90.8%)
    - count delta: `7.8431%` (abs 16)
    - entry p90:   `0.0000%`
    - exit  p90:   `20.1305%`
    - PnL   p90:   `401.1577%`
    - gates: count Δ 7.84%; exit p90 20.1305%; pnl p90 401.1577%; coverage 90.8%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 108-a-popal-simple-smart-buy-sell-strategy  *(closed, profile: strict)*

- TV closed trades: **2559**
- **PineForge** 🟢 **excellent**  (emitted 2559; in window engine 2559 / TV 2559; matched 2559; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 2651; in window engine 1918 / TV 2559; matched 1750; coverage 68.4%)
    - count delta: `25.0488%` (abs 641)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 25.05%; coverage 68.4%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 109-acalvillo20-amd1  *(closed, profile: strict)*

- TV closed trades: **172**
- **PineForge** 🟢 **excellent**  (emitted 172; in window engine 172 / TV 172; matched 172; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 237; in window engine 171 / TV 171; matched 171; coverage 99.4%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 110-aiscripts-lvn-rejection-acceptance-strategy  *(closed, profile: strict)*

- TV closed trades: **2949**
- **PineForge** 🟢 **excellent**  (emitted 2949; in window engine 2949 / TV 2949; matched 2949; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 4173; in window engine 2949 / TV 2949; matched 2949; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.0408%`
    - gates: pnl p90 2.0408%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 111-ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1  *(closed, profile: strict)*

- TV closed trades: **2373**
- **PineForge** 🟢 **excellent**  (emitted 2373; in window engine 1182 / TV 1182; matched 1182; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 3428; in window engine 1234 / TV 1182; matched 1101; coverage 93.1%)
    - count delta: `4.2139%` (abs 52)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0291%`
    - PnL   p90:   `17.2567%`
    - gates: count Δ 4.21%; exit p90 0.0291%; pnl p90 17.2567%; coverage 93.1%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 112-alexgrover-g-channel-trend-detection-alerts-non-repainting  *(closed, profile: strict)*

- TV closed trades: **462**
- **PineForge** 🟢 **excellent**  (emitted 462; in window engine 462 / TV 462; matched 462; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 684; in window engine 462 / TV 462; matched 462; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 113-algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash  *(closed, profile: strict)*

- TV closed trades: **7**
- **PineForge** 🟢 **excellent**  (emitted 7; in window engine 7 / TV 7; matched 7; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 8; in window engine 6 / TV 6; matched 6; coverage 85.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `2.1912%`
    - PnL   p90:   `50.0000%`
    - gates: exit p90 2.1912%; pnl p90 50.0000%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 114-amandaborgeson06-bias-status-dashboard  *(closed, profile: strict)*

- TV closed trades: **2183**
- **PineForge** 🟢 **excellent**  (emitted 2183; in window engine 1218 / TV 1218; matched 1218; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 2989; in window engine 1209 / TV 1215; matched 1040; coverage 85.5%)
    - count delta: `0.4938%` (abs 6)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.2936%`
    - PnL   p90:   `177.6368%`
    - gates: count Δ 0.49%; exit p90 0.2936%; pnl p90 177.6368%; coverage 85.5%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 115-anji-ga-9-21ema-anji  *(closed, profile: strict)*

- TV closed trades: **196**
- **PineForge** 🟢 **excellent**  (emitted 196; in window engine 90 / TV 90; matched 90; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 191; in window engine 90 / TV 90; matched 90; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0004%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 116-anonycryptous-ev-edge-anonycryptous  *(closed, profile: strict)*

- TV closed trades: **866**
- **PineForge** 🟢 **excellent**  (emitted 866; in window engine 866 / TV 866; matched 866; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1254; in window engine 866 / TV 866; matched 866; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 117-antoniolinux-rsi-mfi-divergence-momentum  *(closed, profile: strict)*

- TV closed trades: **298**
- **PineForge** 🟢 **excellent**  (emitted 298; in window engine 298 / TV 298; matched 298; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 431; in window engine 297 / TV 297; matched 297; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 118-averagepoe-mnq-anomaly-candle-sma-confluence-v6  *(closed, profile: strict)*

- TV closed trades: **253**
- **PineForge** 🟢 **excellent**  (emitted 253; in window engine 253 / TV 253; matched 253; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 353; in window engine 253 / TV 253; matched 253; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 119-backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris  *(closed, profile: strict)*

- TV closed trades: **466**
- **PineForge** 🟢 **excellent**  (emitted 466; in window engine 464 / TV 464; matched 464; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 668; in window engine 462 / TV 461; matched 461; coverage 99.4%)
    - count delta: `0.2165%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.22%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 120-benblackdiamond-l2gmom-network-momentum  *(closed, profile: strict)*

- TV closed trades: **416**
- **PineForge** 🟢 **excellent**  (emitted 416; in window engine 183 / TV 183; matched 183; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 626; in window engine 181 / TV 181; matched 170; coverage 93.4%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0003%`
    - PnL   p90:   `33.2648%`
    - gates: pnl p90 33.2648%; coverage 93.4%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 121-bipinbiharipatra-5m-sol-scalper-ha-lorentzian  *(closed, profile: strict)*

- TV closed trades: **1119**
- **PineForge** 🟢 **excellent**  (emitted 1119; in window engine 1119 / TV 1119; matched 1119; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1626; in window engine 1118 / TV 1118; matched 1118; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `10.0317%`
    - gates: pnl p90 10.0317%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 122-cb85wj5jmt-moja-strategia-harami-bb  *(closed, profile: strict)*

- TV closed trades: **1976**
- **PineForge** 🟢 **excellent**  (emitted 1976; in window engine 1976 / TV 1976; matched 1976; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2785; in window engine 1976 / TV 1976; matched 1976; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 123-chadow6875-swing-high-low-ict-clean-pro  *(closed, profile: strict)*

- TV closed trades: **14289**
- **PineForge** 🟢 **excellent**  (emitted 14289; in window engine 14289 / TV 14289; matched 14289; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 19950; in window engine 14080 / TV 14289; matched 13666; coverage 95.6%)
    - count delta: `1.4627%` (abs 209)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 1.46%; coverage 95.6%; distinct-entry multiplicity Δ 472
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 124-cihanozdemir-trade-id-signal-engine-buy-only-option  *(closed, profile: strict)*

- TV closed trades: **200**
- **PineForge** 🟢 **excellent**  (emitted 200; in window engine 200 / TV 200; matched 200; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 293; in window engine 200 / TV 200; matched 200; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 125-cleightyp-cleightyp-bos-sma-macd-vwap  *(closed, profile: strict)*

- TV closed trades: **1396**
- **PineForge** 🟢 **excellent**  (emitted 1396; in window engine 1396 / TV 1396; matched 1396; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1957; in window engine 1396 / TV 1396; matched 1396; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 126-cntvxiao-smc-vsa-oi  *(closed, profile: strict)*

- TV closed trades: **978**
- **PineForge** 🟢 **excellent**  (emitted 978; in window engine 557 / TV 557; matched 557; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1345; in window engine 560 / TV 556; matched 556; coverage 100.0%)
    - count delta: `0.7143%` (abs 4)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `96.2341%`
    - gates: count Δ 0.71%; pnl p90 96.2341%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 127-codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge  *(closed, profile: strict)*

- TV closed trades: **1743**
- **PineForge** 🟢 **excellent**  (emitted 1743; in window engine 1743 / TV 1743; matched 1743; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 2419; in window engine 1744 / TV 1743; matched 1743; coverage 100.0%)
    - count delta: `0.0573%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.06%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 128-colasbreugnon-nq-scalp-fix-signals  *(closed, profile: strict)*

- TV closed trades: **1574**
- **PineForge** 🟢 **excellent**  (emitted 1574; in window engine 1574 / TV 1574; matched 1574; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 2204; in window engine 1578 / TV 1574; matched 1573; coverage 99.9%)
    - count delta: `0.2535%` (abs 4)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.25%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 129-daytrader4beginners-box-breakout-strategy-dt4b-trader  *(closed, profile: strict)*

- TV closed trades: **1562**
- **PineForge** 🟢 **excellent**  (emitted 1562; in window engine 1562 / TV 1562; matched 1562; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 2193; in window engine 1562 / TV 1562; matched 1562; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 130-delta-crypto-mu-overnight-gap-capture  *(closed, profile: strict)*

- TV closed trades: **5148**
- **PineForge** 🟢 **excellent**  (emitted 5148; in window engine 5148 / TV 5148; matched 5148; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 7297; in window engine 5148 / TV 5148; matched 5148; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `40.9735%`
    - gates: pnl p90 40.9735%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 131-devildk-option-point  *(closed, profile: strict)*

- TV closed trades: **210**
- **PineForge** 🟢 **excellent**  (emitted 210; in window engine 210 / TV 210; matched 210; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 284; in window engine 210 / TV 210; matched 210; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 132-dinkus3-obsidian  *(closed, profile: strict)*

- TV closed trades: **86**
- **PineForge** 🟢 **excellent**  (emitted 86; in window engine 47 / TV 47; matched 47; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 123; in window engine 46 / TV 46; matched 46; coverage 97.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `27.1446%`
    - gates: pnl p90 27.1446%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 133-drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500  *(closed, profile: strict)*

- TV closed trades: **313**
- **PineForge** 🟢 **excellent**  (emitted 313; in window engine 313 / TV 313; matched 313; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 432; in window engine 313 / TV 313; matched 313; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `18.3779%`
    - gates: pnl p90 18.3779%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 134-elomadablah-atr-trailing-stoploss-multi  *(closed, profile: strict)*

- TV closed trades: **3369**
- **PineForge** 🟢 **excellent**  (emitted 3368; in window engine 3368 / TV 3368; matched 3368; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4765; in window engine 3368 / TV 3368; matched 3368; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 135-finnp17-atm  *(closed, profile: strict)*

- TV closed trades: **1790**
- **PineForge** 🟢 **excellent**  (emitted 1790; in window engine 1790 / TV 1790; matched 1790; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🔴 **minimal**  (emitted 0; in window engine 0 / TV 0; matched 0; coverage 0.0%)
    - count delta: `100.0000%` (abs 1790)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: no aligned trades
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 136-fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy  *(closed, profile: strict)*

- TV closed trades: **928**
- **PineForge** 🟢 **excellent**  (emitted 928; in window engine 928 / TV 928; matched 928; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1286; in window engine 928 / TV 928; matched 928; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 137-fran-pineda-strategy-501-de-franpineda  *(closed, profile: strict)*

- TV closed trades: **224**
- **PineForge** 🟢 **excellent**  (emitted 224; in window engine 224 / TV 224; matched 224; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 322; in window engine 224 / TV 224; matched 224; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 138-fran-pineda-strategy-502-de-franpineda  *(closed, profile: strict)*

- TV closed trades: **151**
- **PineForge** 🟢 **excellent**  (emitted 151; in window engine 151 / TV 151; matched 151; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 218; in window engine 151 / TV 151; matched 151; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 139-francescodimichele-gold-ai-strategy-v2-0  *(closed, profile: production)*

- TV closed trades: **643**
- **PineForge** 🟢 **excellent**  (emitted 643; in window engine 643 / TV 643; matched 643; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 922; in window engine 644 / TV 643; matched 643; coverage 100.0%)
    - count delta: `0.1553%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `3.3148%`
    - gates: count Δ 0.16%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 140-gonzowiththewind-sisyphus-happiness  *(closed, profile: strict)*

- TV closed trades: **96**
- **PineForge** 🟢 **excellent**  (emitted 96; in window engine 96 / TV 96; matched 96; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** ⚪ n/a — PyneCore runtime error: RuntimeError: security context 'sec·93481528·0': the developing batch published
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 141-hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369  *(closed, profile: strict)*

- TV closed trades: **774**
- **PineForge** 🟢 **excellent**  (emitted 774; in window engine 774 / TV 774; matched 774; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1122; in window engine 773 / TV 773; matched 773; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 142-hermescore-momentum-conviction-hermescore  *(closed, profile: strict)*

- TV closed trades: **3194**
- **PineForge** 🟢 **excellent**  (emitted 3194; in window engine 3194 / TV 3194; matched 3194; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4533; in window engine 3194 / TV 3194; matched 3194; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 143-hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway  *(closed, profile: strict)*

- TV closed trades: **4**
- **PineForge** 🟢 **excellent**  (emitted 4; in window engine 4 / TV 4; matched 4; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 6; in window engine 4 / TV 4; matched 3; coverage 75.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.1656%`
    - PnL   p90:   `1232.3814%`
    - gates: exit p90 0.1656%; pnl p90 1232.3814%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 144-igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1  *(closed, profile: strict)*

- TV closed trades: **513**
- **PineForge** 🟢 **excellent**  (emitted 513; in window engine 189 / TV 189; matched 189; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 683; in window engine 200 / TV 188; matched 188; coverage 100.0%)
    - count delta: `6.0000%` (abs 12)
    - entry p90:   `0.0000%`
    - exit  p90:   `1.1695%`
    - PnL   p90:   `80.3572%`
    - gates: count Δ 6.00%; exit p90 1.1695%; pnl p90 80.3572%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 145-imtiyazali73-imtiyaz-signature-liquidity-compass-smc  *(closed, profile: strict)*

- TV closed trades: **906**
- **PineForge** 🟢 **excellent**  (emitted 906; in window engine 906 / TV 906; matched 906; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1246; in window engine 905 / TV 905; matched 905; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 146-inr3d-r3d-jackofxc-rtp-strategy  *(closed, profile: strict)*

- TV closed trades: **48**
- **PineForge** 🟢 **excellent**  (emitted 48; in window engine 48 / TV 48; matched 48; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 66; in window engine 47 / TV 47; matched 47; coverage 97.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 147-jaydeepp095-candle-harry  *(closed, profile: strict)*

- TV closed trades: **746**
- **PineForge** 🟢 **excellent**  (emitted 746; in window engine 746 / TV 746; matched 746; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1022; in window engine 746 / TV 746; matched 746; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 148-jayendranath4-banknifty-15m-clear-tp-sl-strategy  *(closed, profile: strict)*

- TV closed trades: **430**
- **PineForge** 🟢 **excellent**  (emitted 430; in window engine 430 / TV 430; matched 430; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 611; in window engine 430 / TV 430; matched 430; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 149-jayentriken-bbwp-macd-ema-trend-strategy  *(closed, profile: strict)*

- TV closed trades: **593**
- **PineForge** 🟢 **excellent**  (emitted 593; in window engine 593 / TV 593; matched 593; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 822; in window engine 593 / TV 593; matched 593; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `19.5087%`
    - gates: pnl p90 19.5087%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 150-jdceagle-zigzag-de-fractales-williams  *(closed, profile: strict)*

- TV closed trades: **8491**
- **PineForge** 🟢 **excellent**  (emitted 8491; in window engine 8491 / TV 8491; matched 8491; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 12058; in window engine 8492 / TV 8491; matched 8491; coverage 100.0%)
    - count delta: `0.0118%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.01%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 151-jos-protrader-edward-smart-channel-reversal  *(closed, profile: strict)*

- TV closed trades: **16**
- **PineForge** 🟢 **excellent**  (emitted 16; in window engine 16 / TV 16; matched 16; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 20; in window engine 16 / TV 16; matched 16; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 152-jos-protrader-edward-smart-liquidity-sweep  *(closed, profile: strict)*

- TV closed trades: **1040**
- **PineForge** 🟢 **excellent**  (emitted 1040; in window engine 1040 / TV 1040; matched 1040; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1431; in window engine 1035 / TV 1040; matched 1021; coverage 98.2%)
    - count delta: `0.4808%` (abs 5)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.48%; coverage 98.2%; distinct-entry multiplicity Δ 15
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 153-jos-protrader-edward-smart-momentum-pro  *(closed, profile: strict)*

- TV closed trades: **232**
- **PineForge** 🟢 **excellent**  (emitted 232; in window engine 232 / TV 232; matched 232; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 325; in window engine 231 / TV 231; matched 231; coverage 99.6%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 154-khanhtq26-psol-01-donchian-channels  *(closed, profile: strict)*

- TV closed trades: **346**
- **PineForge** 🟢 **excellent**  (emitted 346; in window engine 346 / TV 346; matched 346; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 516; in window engine 346 / TV 346; matched 346; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 155-legalrice2697-nse-elite-strategy-v6-full-system  *(closed, profile: production)*

- TV closed trades: **362**
- **PineForge** 🟢 **excellent**  (emitted 362; in window engine 362 / TV 362; matched 362; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 496; in window engine 362 / TV 362; matched 362; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `2.4454%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 156-m-f-atipey-hybrid-3-strategy-smart-system-v6-1  *(closed, profile: strict)*

- TV closed trades: **132**
- **PineForge** 🟢 **excellent**  (emitted 132; in window engine 123 / TV 123; matched 123; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 185; in window engine 123 / TV 123; matched 123; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `12.0904%`
    - gates: pnl p90 12.0904%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 157-madue2014-twe-2-bar-break-strategy  *(closed, profile: strict)*

- TV closed trades: **5999**
- **PineForge** 🟢 **excellent**  (emitted 5999; in window engine 5999 / TV 5999; matched 5999; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 8501; in window engine 5998 / TV 5999; matched 5998; coverage 100.0%)
    - count delta: `0.0167%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.02%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 158-market-logic-india-low-lag-strength-oscillator  *(closed, profile: strict)*

- TV closed trades: **6826**
- **PineForge** 🟢 **excellent**  (emitted 6826; in window engine 4073 / TV 4073; matched 4073; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟡 **moderate**  (emitted 8516; in window engine 4633 / TV 4070; matched 4056; coverage 99.6%)
    - count delta: `12.1520%` (abs 563)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.3892%`
    - PnL   p90:   `97.6638%`
    - gates: count Δ 12.15%; exit p90 0.3892%; pnl p90 97.6638%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 159-mdfe3757-trade-strategy-v8-4-pine-v6-ready  *(closed, profile: strict)*

- TV closed trades: **642**
- **PineForge** 🟢 **excellent**  (emitted 642; in window engine 301 / TV 301; matched 301; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 943; in window engine 302 / TV 301; matched 301; coverage 100.0%)
    - count delta: `0.3311%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `5.5932%`
    - gates: count Δ 0.33%; pnl p90 5.5932%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 160-mehranazizi219-goldsiggy-murk  *(closed, profile: strict)*

- TV closed trades: **819**
- **PineForge** 🟢 **excellent**  (emitted 819; in window engine 819 / TV 819; matched 819; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1153; in window engine 819 / TV 819; matched 819; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 161-mylivingedge-gold-asian-range-breakout-signals  *(closed, profile: strict)*

- TV closed trades: **4**
- **PineForge** 🟢 **excellent**  (emitted 4; in window engine 4 / TV 4; matched 4; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4; in window engine 4 / TV 4; matched 4; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 162-nicocashfx-prime-strategy-swing  *(closed, profile: strict)*

- TV closed trades: **73**
- **PineForge** 🟢 **excellent**  (emitted 73; in window engine 73 / TV 73; matched 73; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 101; in window engine 71 / TV 71; matched 71; coverage 97.3%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: coverage 97.3%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 163-nightowlxtrader-azt-strategy-v11-first-draft  *(closed, profile: strict)*

- TV closed trades: **13**
- **PineForge** 🟢 **excellent**  (emitted 13; in window engine 13 / TV 13; matched 13; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 17; in window engine 1 / TV 1; matched 1; coverage 8.3%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.9703%`
    - PnL   p90:   `45.3778%`
    - gates: exit p90 0.9703%; pnl p90 45.3778%; coverage 8.3%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 164-officialjackofalltrades-concordance-execution-mandate-joat  *(closed, profile: strict)*

- TV closed trades: **183**
- **PineForge** 🟢 **excellent**  (emitted 183; in window engine 109 / TV 109; matched 109; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 91; in window engine 27 / TV 27; matched 22; coverage 20.2%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0001%`
    - PnL   p90:   `0.0000%`
    - gates: coverage 20.2%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 165-officialjackofalltrades-concordance-regime-synthesis-joat  *(closed, profile: strict)*

- TV closed trades: **7021**
- **PineForge** 🟢 **excellent**  (emitted 7021; in window engine 7021 / TV 7021; matched 7021; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 9891; in window engine 7019 / TV 7019; matched 6988; coverage 99.5%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `27.8351%`
    - gates: pnl p90 27.8351%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 166-officialjackofalltrades-concordance-strategy-joat  *(closed, profile: strict)*

- TV closed trades: **602**
- **PineForge** 🟢 **excellent**  (emitted 602; in window engine 246 / TV 246; matched 246; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** ⚪ n/a — PyneCore runtime error: RuntimeError: security context 'sec·c5dd5335·3': the developing batch published
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 167-officialjackofalltrades-large-lot-reverse-engineer-joat  *(closed, profile: strict)*

- TV closed trades: **795**
- **PineForge** 🟢 **excellent**  (emitted 795; in window engine 795 / TV 795; matched 795; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1083; in window engine 795 / TV 795; matched 795; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 168-officialjackofalltrades-parallax-covenant-strategy-joat  *(closed, profile: strict)*

- TV closed trades: **1002**
- **PineForge** 🟢 **excellent**  (emitted 1002; in window engine 1002 / TV 1002; matched 1002; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1415; in window engine 995 / TV 1001; matched 995; coverage 99.3%)
    - count delta: `0.5994%` (abs 6)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: By-design guard: request.security expression depends on rebound mutable globals / TA constructor args that cannot be safely lifted into the static security evaluator.
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 169-officialjackofalltrades-regime-execution-strategy-joat  *(closed, profile: strict)*

- TV closed trades: **766**
- **PineForge** 🟢 **excellent**  (emitted 766; in window engine 766 / TV 766; matched 766; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 1055; in window engine 751 / TV 765; matched 319; coverage 41.6%)
    - count delta: `1.8301%` (abs 14)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `3.6756%`
    - gates: count Δ 1.83%; pnl p90 3.6756%; coverage 41.6%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 170-ollie-b-ollie-asia-sweep-model  *(closed, profile: strict)*

- TV closed trades: **1**
- **PineForge** 🟢 **excellent**  (emitted 1; in window engine 1 / TV 1; matched 1; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1; in window engine 1 / TV 1; matched 1; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 171-options7700-2min-bullish-confluence  *(closed, profile: strict)*

- TV closed trades: **1106**
- **PineForge** 🟢 **excellent**  (emitted 1106; in window engine 1106 / TV 1106; matched 1106; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1603; in window engine 1106 / TV 1106; matched 1106; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 172-projectsyndicate-strong-breakout-signals-projectsyndicate  *(closed, profile: strict)*

- TV closed trades: **1521**
- **PineForge** 🟢 **excellent**  (emitted 1521; in window engine 507 / TV 507; matched 507; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 2302; in window engine 497 / TV 507; matched 492; coverage 97.0%)
    - count delta: `1.9724%` (abs 10)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0055%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 1.97%; coverage 97.0%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 173-quantitativealpha-strategy-forecast-engine  *(closed, profile: strict)*

- TV closed trades: **1849**
- **PineForge** 🟢 **excellent**  (emitted 1849; in window engine 1015 / TV 1015; matched 1015; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 2557; in window engine 1011 / TV 1011; matched 1011; coverage 99.7%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `81.3551%`
    - gates: pnl p90 81.3551%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 174-quantnomad-ut-bot-v2-atr-trailing-stop  *(closed, profile: strict)*

- TV closed trades: **4330**
- **PineForge** 🟢 **excellent**  (emitted 4330; in window engine 4330 / TV 4330; matched 4330; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 6241; in window engine 4330 / TV 4330; matched 4330; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 175-rakesh-09-edge-confirmation-system  *(closed, profile: strict)*

- TV closed trades: **386**
- **PineForge** 🟢 **excellent**  (emitted 386; in window engine 386 / TV 386; matched 386; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 543; in window engine 386 / TV 386; matched 386; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 176-rakesh-09-edge-confirmation-system-ecs-v2-0  *(closed, profile: strict)*

- TV closed trades: **484**
- **PineForge** 🟢 **excellent**  (emitted 484; in window engine 289 / TV 289; matched 289; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 707; in window engine 298 / TV 289; matched 288; coverage 99.7%)
    - count delta: `3.0201%` (abs 9)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `10.0718%`
    - gates: count Δ 3.02%; pnl p90 10.0718%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 177-rampatel9912-super-rsi-strategy  *(closed, profile: strict)*

- TV closed trades: **2846**
- **PineForge** 🟢 **excellent**  (emitted 2846; in window engine 2846 / TV 2846; matched 2846; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4012; in window engine 2845 / TV 2845; matched 2845; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 178-remarkablefreddy-ultimate-smc-emas-day-trading-strategy  *(closed, profile: strict)*

- TV closed trades: **519**
- **PineForge** 🟢 **excellent**  (emitted 519; in window engine 519 / TV 519; matched 519; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 721; in window engine 517 / TV 517; matched 517; coverage 99.6%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 179-richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3  *(closed, profile: strict)*

- TV closed trades: **220**
- **PineForge** 🟢 **excellent**  (emitted 220; in window engine 220 / TV 220; matched 220; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 319; in window engine 216 / TV 216; matched 216; coverage 98.2%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `3.5901%`
    - gates: pnl p90 3.5901%; coverage 98.2%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 180-robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target  *(closed, profile: strict)*

- TV closed trades: **291**
- **PineForge** 🟢 **excellent**  (emitted 291; in window engine 291 / TV 291; matched 291; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 410; in window engine 291 / TV 291; matched 291; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 181-roi10x-shiva-lt-ls-blend  *(closed, profile: strict)*

- TV closed trades: **2411**
- **PineForge** 🟢 **strong**  (emitted 2412; in window engine 2412 / TV 2411; matched 2411; coverage 100.0%)
    - count delta: `0.0415%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.04%
- **PyneCore** 🟠 **weak**  (emitted 3061; in window engine 2208 / TV 2409; matched 1700; coverage 70.5%)
    - count delta: `8.3437%` (abs 201)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.2761%`
    - PnL   p90:   `77.1709%`
    - gates: count Δ 8.34%; exit p90 0.2761%; pnl p90 77.1709%; coverage 70.5%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 182-sadtrader9-fair-value-gap-strategy  *(closed, profile: strict)*

- TV closed trades: **3031**
- **PineForge** 🟢 **excellent**  (emitted 3031; in window engine 3031 / TV 3031; matched 3031; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 4305; in window engine 3031 / TV 3031; matched 3031; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 183-shiroi-macd-zero-line-candles-alert  *(closed, profile: strict)*

- TV closed trades: **1302**
- **PineForge** 🟢 **excellent**  (emitted 1302; in window engine 1302 / TV 1302; matched 1302; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1832; in window engine 1302 / TV 1302; matched 1302; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 184-shurben5-tradingview-bot-goat  *(closed, profile: production)*

- TV closed trades: **958**
- **PineForge** 🟢 **excellent**  (emitted 958; in window engine 479 / TV 479; matched 479; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 1404; in window engine 479 / TV 479; matched 479; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 185-simon20cent-efi-macd-advanced-pro  *(closed, profile: strict)*

- TV closed trades: **2153**
- **PineForge** 🟢 **excellent**  (emitted 2153; in window engine 2152 / TV 2152; matched 2152; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 3090; in window engine 2152 / TV 2152; matched 2152; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 186-tharris235106-reversal-signals-with-profit-target-and-continuation  *(closed, profile: strict)*

- TV closed trades: **36**
- **PineForge** 🟢 **excellent**  (emitted 36; in window engine 36 / TV 36; matched 36; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 43; in window engine 36 / TV 36; matched 36; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 187-thebitcoin37-9-15-ema-strategy-trade-room  *(closed, profile: strict)*

- TV closed trades: **202**
- **PineForge** 🟢 **excellent**  (emitted 202; in window engine 202 / TV 202; matched 202; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 317; in window engine 202 / TV 202; matched 202; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 188-theforexguy0777-9-ema-20-ema-retest-strategy  *(closed, profile: strict)*

- TV closed trades: **1372**
- **PineForge** 🟢 **excellent**  (emitted 1372; in window engine 1372 / TV 1372; matched 1372; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 1893; in window engine 1356 / TV 1356; matched 1356; coverage 98.8%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: coverage 98.8%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 189-therealbouga-apex-mtf-index-model  *(closed, profile: strict)*

- TV closed trades: **144**
- **PineForge** 🟢 **excellent**  (emitted 144; in window engine 73 / TV 73; matched 73; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 174; in window engine 70 / TV 62; matched 47; coverage 65.3%)
    - count delta: `11.4286%` (abs 8)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0745%`
    - PnL   p90:   `129.6360%`
    - gates: count Δ 11.43%; exit p90 0.0745%; pnl p90 129.6360%; coverage 65.3%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 190-tomukasss-engulfing-mitigation-strategy  *(closed, profile: strict)*

- TV closed trades: **53**
- **PineForge** 🟢 **excellent**  (emitted 38; in window engine 38 / TV 38; matched 38; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟠 **weak**  (emitted 104; in window engine 26 / TV 26; matched 26; coverage 68.4%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `4.3825%`
    - PnL   p90:   `100.0000%`
    - gates: exit p90 4.3825%; pnl p90 100.0000%; coverage 68.4%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 191-tomukasss-trend-pivot-scale-in  *(closed, profile: strict)*

- TV closed trades: **269**
- **PineForge** 🟢 **excellent**  (emitted 229; in window engine 229 / TV 229; matched 229; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 364; in window engine 233 / TV 229; matched 229; coverage 100.0%)
    - count delta: `1.7167%` (abs 4)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.2162%`
    - PnL   p90:   `21.0526%`
    - gates: count Δ 1.72%; exit p90 0.2162%; pnl p90 21.0526%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap  *(closed, profile: strict)*

- TV closed trades: **157**
- **PineForge** 🟢 **excellent**  (emitted 157; in window engine 157 / TV 157; matched 157; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** ⚪ n/a — PyneSys compile error: {"detail":{"status":"error","error":"Empty document.","line":null,"file":"script.pine"}}
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 193-ttagkoin-adaptive-multi-facto-9-years  *(closed, profile: strict)*

- TV closed trades: **3394**
- **PineForge** 🟢 **excellent**  (emitted 3395; in window engine 3393 / TV 3393; matched 3393; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 4811; in window engine 3387 / TV 3388; matched 3386; coverage 99.8%)
    - count delta: `0.0295%` (abs 1)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
    - gates: count Δ 0.03%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 194-usamotorcars-sedat-xi-crypto-ai-bias-engine  *(closed, profile: strict)*

- TV closed trades: **657**
- **PineForge** 🟢 **excellent**  (emitted 657; in window engine 657 / TV 657; matched 657; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 923; in window engine 656 / TV 656; matched 656; coverage 99.8%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 195-van007trader-micro-momentum-oscillator-dyna  *(closed, profile: strict)*

- TV closed trades: **712**
- **PineForge** 🟢 **excellent**  (emitted 712; in window engine 712 / TV 712; matched 712; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 975; in window engine 711 / TV 711; matched 711; coverage 99.9%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 196-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2  *(closed, profile: strict)*

- TV closed trades: **111**
- **PineForge** 🟢 **excellent**  (emitted 111; in window engine 29 / TV 29; matched 29; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 169; in window engine 29 / TV 29; matched 29; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0002%`
    - PnL   p90:   `2.0909%`
    - gates: pnl p90 2.0909%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 197-waranyutrkm-asian-box-breakout-eda-tuned  *(closed, profile: strict)*

- TV closed trades: **110**
- **PineForge** 🟢 **excellent**  (emitted 110; in window engine 110 / TV 110; matched 110; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** ⚪ n/a — PyneCore runtime error: RuntimeError: security context 'sec·6886bfdb·0': the developing batch published
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 198-wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8  *(closed, profile: strict)*

- TV closed trades: **72**
- **PineForge** 🟢 **excellent**  (emitted 72; in window engine 72 / TV 72; matched 72; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **strong**  (emitted 112; in window engine 72 / TV 72; matched 72; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `3.4471%`
    - gates: pnl p90 3.4471%
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 199-yahmis13-nyo-day-type-early-read  *(closed, profile: strict)*

- TV closed trades: **26**
- **PineForge** 🟢 **excellent**  (emitted 26; in window engine 26 / TV 26; matched 26; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 44; in window engine 26 / TV 26; matched 26; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 200-ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils  *(closed, profile: strict)*

- TV closed trades: **533**
- **PineForge** 🟢 **excellent**  (emitted 533; in window engine 533 / TV 533; matched 533; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 713; in window engine 533 / TV 533; matched 533; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port

### 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2  *(closed, profile: strict)*

- TV closed trades: **154**
- **PineForge** 🟢 **excellent**  (emitted 154; in window engine 154 / TV 154; matched 154; coverage 100.0%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **PyneCore** 🟢 **excellent**  (emitted 224; in window engine 153 / TV 153; matched 153; coverage 99.4%)
    - count delta: `0.0000%` (abs 0)
    - entry p90:   `0.0000%`
    - exit  p90:   `0.0000%`
    - PnL   p90:   `0.0000%`
- **vectorbt** ⚪ n/a — no strategy_vbt.py port
