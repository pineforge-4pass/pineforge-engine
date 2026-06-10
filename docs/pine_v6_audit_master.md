# Pine v6 Coverage — Exhaustive Audit (Master Index)

| Field | Value |
|---|---|
| **Audit date** | 2026-05-21 (initial); enriched same day with F1–F22 clarifications |
| **Last update** | 2026-06-10 — audit-fix sweep (codegen `974cda7`, engine `6aa1d13`): remaining class-A/H rejections + semantic emission fixes; corpus regenerated (`f143504`), parity unchanged 245 excellent + 1 anomaly |
| **Audited doc** | `pine_v6_coverage_detail.md` (1039 lines, 941 identifiers) |
| **Method** | 4 parallel C++/Pine v6 expert agents, line-by-line, Playwright MCP for Pine v6 ref + grep on engine + transpiler. Follow-up: 2 clarification agents with live transpile harness. |
| **Status** | Standalone — chunk reports and clarification reports consolidated below and removed. |

## Headline

**1 critical issue remains (down from ~8 after the 2026-06-10 audit-fix sweep), ~40 minor issues** across 941 identifiers.

| Severity | Definition | Count |
|---|---|---|
| Critical | Doc claim provably wrong vs codegen/runtime/spec → wrong code or silent miscompile | **1** (#15 `first_index`; was ~8 — #5/#6/#31/#32/#35 closed 2026-06-10) |
| Minor | Note stale, ambiguous, off-by-one count, undocumented divergence | **~40** (was ~54; ~14 closed 2026-06-10 — see [RESOLVED] tags below) |

### Phase B/C/D resolutions (2026-05-29)

Codegen now rejects loudly via `support_checker.py` tables instead of silently emitting `"false"` / undeclared C++ symbols. Cross-repo PRs:
- pineforge-codegen [#12](https://github.com/pineforge-4pass/pineforge-codegen/pull/12) — Phase B (6 hard-rejects)
- pineforge-codegen [#13](https://github.com/pineforge-4pass/pineforge-codegen/pull/13) — Phase C (varip + input.color + security adjustment)
- pineforge-codegen [#14](https://github.com/pineforge-4pass/pineforge-codegen/pull/14) — Phase D (analyzer matrix arm + plan audit)

Items resolved: #7, #8, #9, #10, #33, #36, #37, F5, F22; #29 verified false-positive; #26 partially resolved (analyzer guard added; codegen helper deferred).

### Follow-up fixes (2026-05-29, post-Phase-D)

- **#28 `input.time`** — RESOLVED. Engine [pineforge-engine#22](https://github.com/pineforge-4pass/pineforge-engine/pull/22) added `get_input_int64`; codegen [#15](https://github.com/pineforge-4pass/pineforge-codegen/pull/15) routes `input.time` to it (Pine v6 returns `series int` Unix-ms; old `get_input_int` int32 overflowed).
- **Drawing-var dangling-identifier minor** — RESOLVED. codegen [#16](https://github.com/pineforge-4pass/pineforge-codegen/pull/16) tracks omitted drawing-typed UDT fields (`_udt_omitted_fields`) and rewrites reads to `/* drawing field omitted */ 0` / strips writes (closes codegen issue #10).
- **#26 / #27 `input.color` / `input.source`** — RESOLVED (verified against pineforge-codegen source 2026-06-10, commit `64fc886` "input.source series binding, packed-color defval"). Engine ships `get_input_source(title, default_series) → const Series<double>&` and codegen emits `get_input_source("title", _src_<field>_)[0]` for `input.source`; `input.color` routes to `get_input_int64` with a packed-ARGB defval, so the `get_input_color` engine helper contemplated by [pineforge-engine#23](https://github.com/pineforge-4pass/pineforge-engine/issues/23) was deemed unnecessary.

### Audit-fix sweep (2026-06-10, codegen `974cda7`)

One codegen commit closed the remaining class-A/H criticals and most behavioural minors:

- **New loud rejections:** constant-namespace free reads (`extend/font/hline/location/plot/scale/shape/text/xloc/yloc` + `barmerge.*` / `alert.freq_*`) via new `UNSUPPORTED_CONST_NAMESPACES` table — context-aware (`_const_arg_ctx_depth`): still allowed inside parse-and-skip visual calls, the `strategy()` declaration, and `request.security` kwargs. `chart.bg_color`/`fg_color` added to `UNSUPPORTED_MEMBERS`. Bare `ta.<name>` property reads without a registered call site reject in `visit_expr.py`. `export` outside library scripts rejects in `_visit_Identifier`. `_SYMINFO_SILENT_GAP_FIELDS` now derived from the `SYMINFO_MEMBER_MAP` emission table (26 fields — no more 6-field drift).
- **Semantic fixes:** `/=`/`%=` lower to always-float division / `std::fmod`; `str.replace` honors the 4th `occurrence` arg; `timestamp()` rejects bad arities loudly and parses `dateString` literals at transpile time (no more silent 1970 epoch); `string(x)` routes through the `str.tostring` path (bool/string/numeric); `int(x)` propagates na; `strategy.openprofit_percent` divides by realized equity (`current_equity()`) with zero-guard; `syminfo.country` uses ISO 3166-1 alpha-2 (LSE/AQUIS→GB; EURONEXT / crypto-venue "GLOBAL" pseudo-codes removed → na); `math.round_to_mintick` calls the guarded engine method; `array.stdev/variance` honor the `biased` arg; `array.sort` honors the order arg; `array.join` handles string elements; `array.copy/slice` functional-form element typing fixed; bare `hour/minute/second/dayofmonth/dayofweek/month/year/weekofyear` are exchange-timezone aware via `syminfo_.timezone` (value-identical on UTC data); `format.*` members typed STRING by the analyzer; `display.pine_screener` got a distinct `DISPLAY_MAP` code ("6").
- **Deliberately NOT changed (still open):** `strategy.closedtrades.first_index` hardcoded `0` (correct until trade-list capping; explanatory comment added); `array.size`/`map.size` emit `(double)`; `timestamp` non-literal `dateString` rejected; `input.source` non-native defvals rejected.

## Critical issues — consolidated by class

### A. Silent fallthrough → `return "0"` or `return "false"` (most dangerous)

Transpiler dispatcher misses identifier → emits literal `0` / `false` / namespaced token → either compile failure or silent wrong-result. Pine doc claims a bucket that hides this.

| # | Identifier | Doc claim | Reality | Source |
|---|---|---|---|---|
| 1 | `last_bar_time` | ✅ Runtime | **[RESOLVED]** Tracked via `last_bar_time_` in C++ engine and codegen. | A (CI-A2) |
| 2 | `currency.*` (56) | 🔧 string constants | **[RESOLVED]** Maps currency constants as string literals (`std::string("<member>")`). | B (CB-2) |
| 3 | `dayofweek.monday..sunday` | ✅ int constants 1..7 | **[RESOLVED]** Maps Sunday-Saturday to integers "1"-"7". | B (CB-1) |
| 4 | `session.regular / .extended` | ✅ string constants | **[RESOLVED]** Correctly maps regular/extended session string constants. | B (CB-3) |
| 5 | `extend.*, font.*, hline.*, location.*, plot.*, scale.*, shape.*, text.*, xloc.*, yloc.*` | ⏭️ Parse-and-skip | **[RESOLVED 2026-06-10]** Free reads reject via `UNSUPPORTED_CONST_NAMESPACES` (`support_checker.py`); context-aware — still allowed as args to visual calls / `strategy()` / `request.security`. | B (CB-5) |
| 6 | `color.from_gradient()` | ❓ Unknown | **[RESOLVED — verified 2026-06-10]** Rejected via `HARD_REJECT_FUNC` (`support_checker.py`). | C (CI-A5) |
| 7 | `color()` bare cast | ✅ Runtime | **[RESOLVED 2026-05-29, PR codegen#12]** Rejected via `UNSUPPORTED_BARE_FUNCS` in `support_checker.py`. | C (CI-A4) |
| 8 | `footprint.*` (9 fns + 1 type) | ❓ Unknown | **[RESOLVED 2026-05-29, PR codegen#12]** Rejected via `UNSUPPORTED_NAMESPACES`. | C (CI-A8) |
| 9 | `volume_row.*` (8 fns + 1 type) | ❓ Unknown | **[RESOLVED 2026-05-29, PR codegen#12]** Rejected via `UNSUPPORTED_NAMESPACES`. | D (CD-4) |
| 10 | `timeframe.from_seconds()` | 🔧 Transpiler | **[RESOLVED 2026-05-29, PR codegen#12]** Added to `NOT_YET_FUNC`; `visit_call.py:809` fallthrough now raises `ValueError` defensively. | D (CD-3) |
| 11 | `syminfo.prefix()` / `syminfo.ticker()` fn forms | ✅ Runtime | **[RESOLVED]** Plumbed in signatures, analyzer, and visit_call to use `_pf_derive_prefix` / `syminfo_.ticker`. | D (CD-2) |
| 12 | `max_bars_back()` | 🔧 Transpiler | ❌ hard-reject (`NOT_YET_FUNC`) | D (CD-1) |
| 36 | `chart.left_visible_bar_time` / `chart.right_visible_bar_time` | ❌ Unsupported | **[RESOLVED 2026-05-29, PR codegen#12]** Rejected via `UNSUPPORTED_MEMBERS`. `visit_expr.py:340` fallthrough now raises `ValueError`. | F2 |
| 37 | `dividends.*` / `earnings.*` (12 vars) | ❌ Unsupported (Pine: returns na) | **[RESOLVED 2026-05-29, PR codegen#12]** Rejected via `UNSUPPORTED_NAMESPACE_VARS` (`dividends`, `earnings`). | F3 |
| 38 | `strategy.commission.{percent,cash_per_order,cash_per_contract}` | ✅ Runtime | **[RESOLVED]** Maps correctly to distinct integer codes ("0", "1", "2") for both strategy kwargs and free expressions. | F4 |

### B. Hardcoded wrong values

Codegen emits a literal constant instead of reading runtime state.

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 13 | `strategy.account_currency` | reads `syminfo_.currency` | **[RESOLVED]** Reads `syminfo_.currency` successfully at runtime. |
| 14 | `strategy.position_entry_name` | last pyramid entry id | **[RESOLVED]** Correctly binds to C++ engine `position_entry_name()` accessor. |
| 15 | `strategy.closedtrades.first_index` | first index | hardcoded `0` — correct until the engine implements the 9000-trade-list cap; explanatory comment added 2026-06-10 (`visit_expr.py`). **Only remaining critical.** |

### C. Wrong aliasing

Identifier resolves to a different runtime symbol than spec.

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 16 | `last_bar_index` | computed from bar count | **[RESOLVED]** Binds to the actual `last_bar_index_` engine field. |
| 17 | `timenow` | always na | **[RESOLVED]** Binds to the actual `timenow_` engine property. |
| 18 | `time_close` (var) | `pine_time_close` | **[RESOLVED]** Correctly binds to C++ engine `time_close()` accessor. |
| 19 | `barstate.islastconfirmedhistory` | always false | **[RESOLVED]** Properly checks and runs against C++ engine state. |
| 20 | `timeframe.isminutes` | minutes-only | **[RESOLVED]** Restricts seconds TF correctly via `(tf_is_intraday(...) && !tf_is_seconds(...))`. |
| 21 | `ta.tr` | `ta::TR` class | **[RESOLVED]** Verified inlined expression matches exact TV v6 mathematical definition. |
| 22 | `strategy.max_drawdown_percent` | drawdown calc | **[RESOLVED]** Binds to actual C++ engine `max_drawdown_percent()`. |

### D. Wrong Pine field name → support_checker rejects

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 23 | `syminfo.recommendations_buy_strong` | reads field | **[RESOLVED]** Maps correctly to `recommendations_buy_strong` (or `recommendations_buy_strong`). |
| 24 | `syminfo.recommendations_sell_strong` | reads field | **[RESOLVED]** Maps correctly to `recommendations_sell_strong`. |
| 25 | `syminfo.target_price_estimates` | reads field | **[RESOLVED]** Maps correctly to `target_price_estimates`. |

### E. Codegen uses wrong helper function

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 26 | `input.color()` | `get_input_string()` + color parse | **[RESOLVED — verified vs codegen source 2026-06-10]** Analyzer requires defval to be `color.<const>` / `color.new(...)` / `color.rgb(...)` (PR codegen#13); codegen now emits `get_input_int64` with a packed-ARGB defval (`0xAARRGGBB` overflows int32, hence int64). A dedicated engine `get_input_color` helper (engine issue #23) was deemed unnecessary. |
| 27 | `input.source()` | series source | **[RESOLVED — verified vs codegen source 2026-06-10]** Engine ships `get_input_source(name, default_series) → const Series<double>&`; codegen emits `get_input_source("title", _src_<field>_)[0]` against the engine's always-materialized native source series (no more frozen scalar). Remaining engine issue #23 scope (`get_input_color`) deemed unnecessary — see #26. |
| 28 | `input.time()` | `get_input_double()` | **[RESOLVED 2026-05-29, PR engine#22 + codegen#15]** Routes to `get_input_int64` (Pine v6 `input.time` returns `series int` Unix-ms; int32 `get_input_int` overflowed). |
| 29 | `color.rgb()` | optional `transp` arg | **[RESOLVED — false-positive in original audit]** Verified `visit_call.py:1095-1098` correctly passes `args[3]` as transparency in the 4-arg form; 3-arg fallback defaults to 0. Original audit claim was wrong. |

### F. Broken bare-property form

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 30 | `ta.vwap` (bare) | works | **[RESOLVED]** Supported by injecting default parameters `(close, volume, timestamp)` during transpilation. |
| 31 | `ta.<name>` bare property reads (all) | works | **[RESOLVED 2026-06-10]** Unbound property reads reject loudly (`visit_expr.py` codegen error) instead of falling to `std::string("<name>")`; sibling-call binding still works. |

### G. Doc note inaccurate / wrong file

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 32 | Date/time fn-forms (`dayofmonth(...)`, etc.) | section "1-arg", uses `pine_time` | **[RESOLVED 2026-06-10]** Variable and fn forms share `tz_time_field_lambda` (`codegen/tables.py`): exchange TZ (`syminfo_.timezone`) by default, optional tz arg on fn form. |
| 33 | `varip` (sprint claim) | "transpile-time warning emitted" | **[RESOLVED 2026-05-29, PR codegen#13]** `support_checker.py:412-425` now raises `_err` (was `_warn`) — varip declarations reject loudly. Both corpus probes using varip removed (`varip-counter-state-positive-01`, `varip-var-udt-in-security-positive-01`). |

### H. New (chunk D)

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 34 | `order.ascending/descending` | ✅ Runtime | **[RESOLVED]** Collection sorting is fully implemented using runtime lambdas dynamically checking sorting orders. |
| 35 | `barmerge.*` / `alert.freq_*` | ✅ Runtime | **[RESOLVED 2026-06-10]** Free-expression reads reject via `UNSUPPORTED_CONST_NAMESPACES`; `request.security` / `alert()` kwarg use unchanged. |

## Minor issues — themes

(~54 total. F-numbered items verified individually via live transpile harness.)

### Counts / spec drift
- **Off-by-one counts**: `label.style_*` 21 not 22; `plot.style_*` 11 not 12; `text.*` 10 not 8; `box/label/line` counts 29/21/21 not 27/20/20; `table.*` 22 fns + 1 var not 20; `matrix.*` 48 fns (PineForge implements 47 — `median` omitted intentionally per `KNOWN_MATRIX_OMISSIONS`).
- **Missing dividends/earnings vars in doc**: `dividends.gross/.net`, `earnings.actual/.estimate/.standardized` (Pine spec lists; doc omits).
- **`array.* (54 entries)`** matches spec.

### Spec-type divergence
- **`adjustment.*` / `backadjustment.*` / `settlement_as_close.*` (9 entries)** [F5]: **[PARTIALLY RESOLVED 2026-05-29, PR codegen#13]** `request.security(adjustment=…)` non-no-op values (e.g. `adjustment.splits`, `backadjustment.on`, `settlement_as_close.on`) now reject loudly via `SECURITY_ADJUSTMENT_ALLOWED_VALUES` in `support_checker.py`. The underlying constant-type drift (string vs int) is unchanged but no longer masks engine silent-drop because the kwarg path itself is gated.
- **`currency.*`** typed STRING by analyzer but emit int 0 (already class A).
- **`syminfo.recommendations_date` / `target_price_date`** [F17]: now route through `get_syminfo_metadata(...)` (returns `double`, na until a feed injects) even though Pine types `series int`; still inconsistent with sibling `expiration_date` (`na<int64_t>()`). Risk of double→int coercion surprise. *(backing updated 2026-06-10; type drift still open)*

### Codegen silent fallthrough fallout (beyond class A)
- **`chart.bg_color` / `chart.fg_color`** [F1]: **[RESOLVED 2026-06-10]** Reject via `UNSUPPORTED_MEMBERS` (`support_checker.py`).
- **`display.pine_screener`** [F19]: **[RESOLVED 2026-06-10]** Distinct code `"6"` in `DISPLAY_MAP` (`codegen/tables.py`) — no longer collides with `display.all`.
- **`format.mintick / .percent / .volume`** [F20]: **[RESOLVED 2026-06-10]** Analyzer types `format.*` reads STRING (`analyzer/base.py`), matching the `std::string` emission.
- **`export` keyword** [F6]: **[RESOLVED 2026-06-10]** Stray `export` identifier rejects loudly in `support_checker._visit_Identifier` with an inline-the-library hint; library form still blocked at `library()`.

### Backing / note inaccuracies (behaviour OK, doc misleading)
- **`math.round_to_mintick()`** [F7]: **[RESOLVED 2026-06-10]** Codegen now calls the guarded engine method `round_to_mintick(x)` (`visit_call.py`) instead of the unguarded inline round.
- **`na(x)` function form** [F8]: lowers to `is_na(x)`, NOT `na<T>()`. Only the bare `na` identifier maps to `na<double>()`.
- **`str.startswith` / `str.endswith`** [F9]: doc says `std::string::starts_with/ends_with`. Actually uses `substr(0,n)==pre` and length-check + `compare(off,len,suf)` (pre-C++20 portability). Functionally equivalent.
- **`var` keyword** [F10]: doc says "static in on_bar". Actually class member + `_var_initialized` gate inside `on_bar` (per-instance, NOT C++ `static`). Pattern is safer — allows multiple `GeneratedStrategy` instances in one process.
- **Annotations** [F11]: lexer drops every `//@*` line; only `//@version=N` is recovered via regex on raw source. `//@variable / //@strategy / //@description` etc. silently discarded. Doc overstates "transpiler reads".
- **`strategy()`** [F12]: call site lowers to `/* strategy declaration */` comment placeholder. State lives on `StrategyDecl` AST → materialised by `emit_top` into ctor / `set_param` / `extern "C"` bridge. `StrategyOverrides` is host-side override layer, NOT the kwarg sink.
- **`strategy.default_entry_qty(fill_price)`** [F13]: lowers to `calc_qty(fill_price)` engine method (reads `default_qty_value_` + `default_qty_type_` + equity). Doc's `default_qty_value_` only names underlying setting.
- **`request.security()`** [F14]: 3 hard restrictions (support_checker.py:683-760): (1) same-chart symbol only (`syminfo.tickerid/.ticker`); (2) `lookahead_on` rejected outright; (3) `currency` / `ignore_invalid_symbol` rejected. Allowed params: `symbol, timeframe, expression, gaps, lookahead`.
- **`ta.tr` (property) vs `ta.tr(handle_na)` (fn)** [F15]: property hardcodes `handle_na=true` via inline lambda; fn form dispatches `ta::TR` ctor with `handle_na=false` default. Matches Pine v6 spec divergence between forms.
- **`strategy.equity`** [F16]: lowers to `(current_equity() + open_profit(current_bar_.close))` — includes mark-to-market open P&L. Doc's bare `current_equity()` understates.
- **`strategy.openprofit_percent`** denominator: **[RESOLVED 2026-06-10]** Now `open_profit / current_equity() * 100` (realized equity) with zero-guard (`visit_expr.py`).
- **`syminfo.country`**: **[RESOLVED 2026-06-10]** `PREFIX_TO_COUNTRY` (`helpers_syminfo.py`) is ISO 3166-1 alpha-2 (LSE/AQUIS→GB); EURONEXT and crypto-venue "GLOBAL" pseudo-codes removed — unknown prefix → na.
- **TZ divergence**: **[RESOLVED 2026-06-10]** Bare `hour/minute/dayofweek/...` route through `tz_time_field_lambda(..., syminfo_.timezone)` (`BAR_BUILTINS` in `codegen/tables.py`) — exchange TZ per Pine spec; value-identical on UTC data.

### Behavioural gaps
- **`str.tonumber`** ~~lacks try/catch~~ **[RESOLVED]** Emits try/catch lambda returning `na<double>()` on parse failure (`tables.py` STR_FUNC_MAP "tonumber").
- **`str.replace`** 4-arg form: **[RESOLVED 2026-06-10]** `occurrence` arg honored (0-based per Pine spec; out-of-range/negative → original string) in `visit_call.py`.
- **`timestamp()`** [F21]: **[RESOLVED 2026-06-10]** Bad arities reject loudly (year/month/day required); `dateString` literals parsed at transpile time (ISO-8601 + "DD MMM YYYY" forms, GMT+0 default); non-literal `dateString` rejected; tz-first overload handled (`visit_call.py`).
- **`/=` and `%=`**: **[RESOLVED 2026-06-10]** Lower to always-float division / `std::fmod` matching the binary `/` and `%` semantics (`visit_stmt._compound_assign_rhs`).
- **`array.size()` / `matrix.size()`** [F18]: emit `(double)v.size()`; Pine returns `series int`. Comparisons promote OK; explicit int-overload dispatch would surprise. *(still open — deliberate)*
- **Array silent gaps**: **[RESOLVED 2026-06-10]** `array.new_color/_label/_line/_linefill/_box/_table` reject via the `SUPPORTED_ARRAY` whitelist; `array.copy/slice` functional form preserves element type (`codegen/types.py`); `array.join` handles string elements; `array.sort` honors order arg; `array.stdev/variance` honor `biased` arg (`codegen/tables.py`).
- **`string(x)` / `int(x)`**: **[RESOLVED 2026-06-10]** `string(x)` routes through the `str.tostring` path (bool/string/numeric); `int(x)` propagates na via the engine int sentinel (`visit_call.py`).
- **Drawing-var method calls** ~~produce dangling C++ identifiers~~ **[RESOLVED 2026-05-29, PR codegen#16]** — omitted drawing-typed UDT fields tracked in `_udt_omitted_fields`; reads rewrite to `/* drawing field omitted */ 0`, writes stripped (closes codegen issue #10).

### Diagnostic / warning coverage gaps
- **Syminfo na-fields "conditional-use" warning** [F22]: **[RESOLVED 2026-06-10]** `_SYMINFO_SILENT_GAP_FIELDS` is now derived from the `SYMINFO_MEMBER_MAP` emission table (every `na<T>()` / `get_syminfo_metadata(...)` entry — 26 fields), so new na-accept fields cannot drift out of the warning.
- **`ta.<name>` bare property reads**: **[RESOLVED 2026-06-10]** Unbound reads reject loudly instead of falling to `std::string("<name>")` — see item #31.
- **`varip` warning** claimed in doc — actually NOT emitted (originally class G item 33). **[RESOLVED 2026-05-29, PR codegen#13]** Now raises a hard `CompileError` instead. See item 33 above.

## Methodology

Phase 1 — 4 parallel chunk agents:
1. Loaded official Pine v6 reference via Playwright MCP (`https://www.tradingview.com/pine-script-reference/v6/` — JS SPA, `<h3>` enumeration).
2. Read transpiler emitters under `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/`.
3. Read engine headers under `/Users/haoliangwen/code/pineforge-engine/include/pineforge/`.
4. Verified bucket + backing + notes + emitted-value for EVERY row (no skipping, no spot-check exemption).
5. Cross-checked grouped sections (`array.*`, `currency.*`, `matrix.*`, etc.) member-by-member against Pine spec.

Phase 2 — 2 clarification agents (F1–F22):
1. Re-verified 22 findings from chunk reports that weren't in the initial consolidation.
2. Used live transpile harness (`Lexer → Parser → Analyzer → CodeGen.generate()`) + live `support_checker.check_or_raise()` for every finding.
3. Corrected 2 chunk-D errors: F7 (engine `round_to_mintick` method DOES exist — codegen just doesn't call it); F22 (na-syminfo warning DOES exist — but covers only 6 of ~14 na-accept fields).

## What's NOT in this audit

- Headline bucket totals (199/219/220/142/161) not re-derived; doc-stated counts trusted.
- Did not run the transpiler end-to-end on a representative Pine corpus to surface runtime crashes; static codegen path inspection only.
- Sprint history sections (962–1039) audited for accuracy of recent additions but not for completeness of what *should* have been added.

## Recommended next steps

**Phase B/C/D status (2026-05-29):** Steps 2 and 3 substantially addressed by the codegen rejection sweep — `support_checker.py` now carries 4 new rejection tables (`UNSUPPORTED_BARE_FUNCS`, `UNSUPPORTED_NAMESPACES`, `UNSUPPORTED_MEMBERS`, `UNSUPPORTED_NAMESPACE_VARS`) plus `varip` and `request.security` adjustment guards. Two defensive `raise ValueError` guards added at `visit_call.py:809` and `visit_expr.py:340` so any future drift surfaces loudly.

**Audit-fix sweep status (2026-06-10):** #5, #6, #31, #32, #35 and ~14 minors closed by codegen `974cda7` (see the sweep section above). `pine_v6_coverage_detail.md` rows and bucket totals reconciled the same day (surgical refresh against codegen `974cda7` / engine `6aa1d13`).

**Remaining open work:**

1. **#15 `strategy.closedtrades.first_index`** — hardcoded `0`; becomes wrong only when the engine implements the 9000-trade-list cap. Fix together with that feature.
2. **`array.size` / `map.size` / `matrix.size` int typing** [F18] — emit `(double)`; Pine types `series int`. Low risk; needs an int-emission pass.
3. **Engine economics findings O4 / O5** — percent-of-equity sizing base, `pnl_pct` net-of-commission — tracked in `production-readiness-findings.md`; each needs a TV-parity export to pin the convention before changing. (O7 — MFE/MAE convention — fixed 2026-06-10: TV-aligned export sign/names, exit-fill + trail-peak + pre-fill-path excursion folding, partial-slice scaling, net-of-entry-commission basis; MAE p90 now gated at 5% in `verify_corpus.py`.)
4. **Doc** — `pine_v6_coverage_detail.md` totals are delta-reconciled, not re-derived from scratch; a full per-identifier regeneration is still the long-term fix for the ❓ bucket (141 left).
5. **Corpus probe gaps** — no probe exercises `slippage≠0`, `close_entries_rule`, `risk.max_intraday_loss`/`max_cons_loss_days`, magnifier non-ENDPOINTS modes, `lookahead_on`, `input.color/time/symbol`, `str.format` family, `timestamp()`, `math.random`, `for-in`.
