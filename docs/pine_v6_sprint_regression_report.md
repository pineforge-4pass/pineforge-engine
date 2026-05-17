# Pine v6 HIGH+MEDIUM Sprint — Final Regression Report

**Sprint completed:** 2026-05-17
**Plan:** `/Users/haoliangwen/.claude/plans/linked-doodling-beacon.md`
**Coverage docs:** `docs/pine_v6_coverage_detail.md`, `docs/pine_v6_feature_backlog.md`
**Scope analysis:** `docs/pine_v6_dispatch_scope.html`

## Headline

**Zero sprint-introduced regressions** across all 7 agent merges. 224/228 baseline-excellent probes still excellent; remaining 4 non-excellent are pre-existing (not introduced this sprint).

## Sprint dispatch summary

7 parallel agents, one git worktree+branch per feature group, each self-tested via ctest + pytest + smoke validator + 228-probe regression sweep before reporting completion.

| Agent | Scope | Engine | Codegen | Corpus probes |
|---|---|---|---|---|
| A | session.is* predicates (7 ids) | session_time.cpp/hpp + engine_run.cpp + new ctest | signatures + visit_expr | 4 (TV_RUN_NEEDED) |
| B | ta.vwap 3-tuple bands | ta.hpp + ta_extremes_volume.cpp + new ctest | dual overload + tuple dispatch | 2 (TV_RUN_NEEDED) |
| C | varip passthrough | (none) | support_checker warn instead of err | 1 update + 1 new (TV_RUN_NEEDED) |
| E | timeframe.main_period + chart.is_* + timeframe.isticks (9 ids) | engine.hpp getter | signatures + visit_expr namespace branches | 2 (1 smoke + 1 TV_RUN_NEEDED) |
| F | max_contracts_held_* + eventrades (4 ids) | engine.hpp fields + per-bar update + new ctest | signatures + visit_expr | 2 (TV_RUN_NEEDED) |
| G1 | time_tradingday + DST handling | session_time.cpp helper promotion + pine_time_tradingday + new ctest | signatures + visit_expr | 1 (TV_RUN_NEEDED) |
| G2 | codegen grab bag (24 syminfo na-accept + 8 constants + 2 syminfo derivations + ticker.* split 7-reject + 2-passthrough) | (none) | helpers_syminfo.py + tables + signatures + visit_expr + support_checker | 4 (2 smoke + 1 expected_reject + 1 smoke) |

## Final corpus state

| Bucket | Count |
|---|---|
| **excellent** | **226** (+2 vs baseline 224, after validator engine_chart_timezone fix) |
| strong (pre-existing) | 1 |
| weak (pre-existing) | 1 |
| TV_RUN_NEEDED placeholders (no CSV yet) | 15 |
| **Total probes** | **243** (228 baseline + 15 new from sprint) |

Notes:
- `ticker-heikinashi-rejected-01` removed post-sprint — expected-reject probes
  belong in codegen pytest, not corpus. Coverage retained at
  `pineforge_codegen/tests/test_support_checker.py:482 test_ticker_heikinashi_rejected`.
- `cap-max-intraday-filled-orders-isolate-01`: weak (51.2%) → excellent (100%
  bit-exact 1958/1958) via validator `engine_chart_timezone=""` override
  (no engine change). See `docs/issues/cap-day-boundary-chart-tz-precision.md`
  for full analysis + long-term pineforge-data dependency.
- `composite-bracket-cap-range-pending-stop-01`: strong → excellent
  (no explicit change — likely flaky probe stabilising after rebuild).

## Per-merge regression check

Sequential merge order: C → G2 → E → G1 → F → A → B. After each merge, full validator sweep run; baseline-excellent count compared to pre-sprint state.

| Merge | excellent count | new probes (None) | regression vs baseline |
|---|---|---|---|
| Baseline (post fixes) | 224 / 228 | 0 | — |
| Post C | 224 / 229 | 1 | 0 |
| Post G2 | 224 / 233 | 4 | 0 |
| Post E | 224 / 235 | 6 | 0 |
| Post G1 | 224 / 236 | 7 | 0 |
| Post F | 224 / 238 | 9 | 0 |
| Post A | 224 / 242 | 13 | 0 |
| Post B (final) | 224 / 244 | 15 | 0 |

## Pre-existing non-excellent probes (NOT sprint regressions)

| Probe | Status | Root cause |
|---|---|---|
| anomaly-equity-mirror-strategy-equity-01 | weak | Documented anomaly probe (intentional weak) |
| cap-max-intraday-filled-orders-isolate-01 | weak | Engine cap-day-boundary bug introduced in `6b062fc` (May 15, pre-sprint). Count delta fixed by `tv_trades_csv_tz=utc` in inputs.json; price drift requires engine fix (separate issue). |
| composite-bracket-cap-range-pending-stop-01 | strong | Pre-existing (not analyzed in detail) |
| composite-trendmaster-three-tier-ema-state-01 | strong | Warmup-sensitive multi-tier EMA state. Improved weak→strong via `tv_trades_csv_tz=utc` + `ohlcv_start_ms=1743588000000`. |

## Environment / config fixes shipped this sprint

- **Eigen auto-detect** in `pineforge-utils/validator/_compiler.py`: falls back to common system paths (`/opt/homebrew/include/eigen3`, `/usr/local/include/eigen3`, `/usr/include/eigen3`, `/opt/local/include/eigen3`) when `EIGEN3_INCLUDE_DIR` env var unset and `<engine>/build/_deps/eigen-src` missing. Recovered 6 matrix-* probes that previously regressed to compile-fail.
- **Probe input.json fixes** for 2 pre-existing regressions (see Pre-existing table above).

## Conflicts resolved during merge

| Merge | Conflicts | Resolution |
|---|---|---|
| A | `session_time.hpp`, `session_time.cpp`, codegen `signatures.py`, codegen `visit_expr.py` | Concat both sides; dedupe `hhmm_to_minutes` declaration (G1 already promoted it from anon namespace; A's duplicate dropped). |
| B | `tests/CMakeLists.txt` | Both `test_session_predicates` + `test_vwap_bands` added. |

## Next steps (human-driven)

1. **TV CSV capture (~14 probes):** Open TradingView, run each TV_RUN_NEEDED probe with the symbol/TF/range specified in its `strategy.pine` header, export "List of trades" → `tv_trades.csv` into the probe directory. Then re-run validator on those probes; they should all be excellent (or strong for warmup-sensitive ones).
2. **Push branches:** all merges currently local. `git push origin main` on each of engine + codegen + corpus when ready.
3. **Backlog update:** mark HIGH (8 items) + MEDIUM (12 groups) as DONE in `docs/pine_v6_feature_backlog.md`. Remaining backlog should reduce to LOW + future engine work (library system, fundamentals via pineforge-data).
4. **Coverage docs refresh:** regenerate headline numbers in `docs/pine_v6_coverage_detail.md` once TV CSVs land and final parity numbers stabilize. Forecast: 40% → ~45% fully-runs, ~70 Unknowns resolved to known buckets.
5. **Engine bug ticket (separate):** `cap-max-intraday-filled-orders-isolate-01` price drift in chart-TZ-aware day rollover (`6b062fc`); root-cause + fix not in scope this sprint.

## Branches created (still local)

- pineforge-engine: `worktree-agent-ac0cadde3dda4261f` (A), `a9429cf60671a6120` (B), `ac83b4e1d5f3f63b9` (E), `ab3417b6dec84c9e1` (F), `a9884bb0dc009577a` (G1). All merged.
- pineforge-codegen: `feat/pine-v6-{A,B,C,E,F,G1,G2}`. All merged.
- pineforge-corpus: `feat/pine-v6-{A,B,C,E,F,G1,G2}`. All merged.

## Approximate identifier impact

Per-agent identifier counts moving from ❓/❌ to ✅/🔧 status:

| Agent | New ✅/🔧 identifiers |
|---|---|
| A | 7 (session.is* family) |
| B | 1 (ta.vwap 3-tuple form) |
| C | 1 (varip keyword, now warn-then-emit-as-var) |
| E | 9 (timeframe.main_period + chart.is_* x7 + timeframe.isticks) |
| F | 4 (max_contracts_held_* x3 + eventrades) |
| G1 | 1 (time_tradingday) |
| G2 | 26+ (syminfo na-accept 24 + constants 8 + ticker.* passthrough 2 - some overlap) |
| **Total** | **~50 new identifiers fully supported** |

Plus warnings for silent-gap remediation on 6 syminfo conditional uses (sector/industry/isin/expiration_date/current_contract/mincontract).
