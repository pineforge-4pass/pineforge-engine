# Pine v6 Coverage — Exhaustive Audit (Master Index)

| Field | Value |
|---|---|
| **Audit date** | 2026-05-21 (initial); enriched same day with F1–F22 clarifications |
| **Last update** | 2026-05-29 — Phase B/C/D merges (codegen rejections + analyzer matrix arm) |
| **Audited doc** | `pine_v6_coverage_detail.md` (1039 lines, 941 identifiers) |
| **Method** | 4 parallel C++/Pine v6 expert agents, line-by-line, Playwright MCP for Pine v6 ref + grep on engine + transpiler. Follow-up: 2 clarification agents with live transpile harness. |
| **Status** | Standalone — chunk reports and clarification reports consolidated below and removed. |

## Headline

**~9 critical issues remain (down from 18 after Phase B/C/D merges 2026-05-29), ~55 minor issues** across 941 identifiers.

| Severity | Definition | Count |
|---|---|---|
| Critical | Doc claim provably wrong vs codegen/runtime/spec → wrong code or silent miscompile | **~9** (was 18; 7 closed by Phase B rejections, 1 by Phase C, 1 verified false-positive) |
| Minor | Note stale, ambiguous, off-by-one count, undocumented divergence | **~55** |

### Phase B/C/D resolutions (2026-05-29)

Codegen now rejects loudly via `support_checker.py` tables instead of silently emitting `"false"` / undeclared C++ symbols. Cross-repo PRs:
- pineforge-codegen [#12](https://github.com/fullpass-4pass/pineforge-codegen/pull/12) — Phase B (6 hard-rejects)
- pineforge-codegen [#13](https://github.com/fullpass-4pass/pineforge-codegen/pull/13) — Phase C (varip + input.color + security adjustment)
- pineforge-codegen [#14](https://github.com/fullpass-4pass/pineforge-codegen/pull/14) — Phase D (analyzer matrix arm + plan audit)

Items resolved: #7, #8, #9, #10, #33, #36, #37, F5, F22; #29 verified false-positive; #26 partially resolved (analyzer guard added; codegen helper deferred).

## Critical issues — consolidated by class

### A. Silent fallthrough → `return "0"` or `return "false"` (most dangerous)

Transpiler dispatcher misses identifier → emits literal `0` / `false` / namespaced token → either compile failure or silent wrong-result. Pine doc claims a bucket that hides this.

| # | Identifier | Doc claim | Reality | Source |
|---|---|---|---|---|
| 1 | `last_bar_time` | ✅ Runtime | **[RESOLVED]** Tracked via `last_bar_time_` in C++ engine and codegen. | A (CI-A2) |
| 2 | `currency.*` (56) | 🔧 string constants | **[RESOLVED]** Maps currency constants as string literals (`std::string("<member>")`). | B (CB-2) |
| 3 | `dayofweek.monday..sunday` | ✅ int constants 1..7 | **[RESOLVED]** Maps Sunday-Saturday to integers "1"-"7". | B (CB-1) |
| 4 | `session.regular / .extended` | ✅ string constants | **[RESOLVED]** Correctly maps regular/extended session string constants. | B (CB-3) |
| 5 | `extend.*, font.*, hline.*, location.*, plot.*, scale.*, shape.*, text.*, xloc.*, yloc.*` | ⏭️ Parse-and-skip | Emit `std::string("<member>")`; analyzer types as INT → mismatch | B (CB-5) |
| 6 | `color.from_gradient()` | ❓ Unknown | Returns hardcoded `0` | C (CI-A5) |
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
| 15 | `strategy.closedtrades.first_index` | first index | hardcoded `0` (coincidentally correct) |

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
| 26 | `input.color()` | `get_input_string()` + color parse | **[PARTIAL RESOLVED 2026-05-29, PR codegen#13]** Analyzer now requires defval to be `color.<const>` / `color.new(...)` / `color.rgb(...)`; rejects arbitrary expressions. Codegen still emits `get_input_int` (engine has no `get_input_color` helper — full fix is cross-repo follow-up). |
| 27 | `input.source()` | series source | **[BLOCKED]** Codegen still emits `get_input_double` → frozen scalar (verified Phase C investigation: `current_bar_.close` captured once at call site). Fix requires new `get_input_source(name, default_series) → const Series<double>&` helper in pineforge-engine; tracked as cross-repo follow-up on codegen issue #9. |
| 28 | `input.time()` | `get_input_double()` | uses `get_input_int` |
| 29 | `color.rgb()` | optional `transp` arg | **[RESOLVED — false-positive in original audit]** Verified `visit_call.py:1095-1098` correctly passes `args[3]` as transparency in the 4-arg form; 3-arg fallback defaults to 0. Original audit claim was wrong. |

### F. Broken bare-property form

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 30 | `ta.vwap` (bare) | works | **[RESOLVED]** Supported by injecting default parameters `(close, volume, timestamp)` during transpilation. |
| 31 | `ta.<name>` bare property reads (all) | works | require sibling `ta.<name>(...)` call to register call site (MI-A8 — see minor) |

### G. Doc note inaccurate / wrong file

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 32 | Date/time fn-forms (`dayofmonth(...)`, etc.) | section "1-arg", uses `pine_time` | spec is 2-arg w/ tz; codegen uses inline `gmtime_r/localtime_r` lambdas |
| 33 | `varip` (sprint claim) | "transpile-time warning emitted" | **[RESOLVED 2026-05-29, PR codegen#13]** `support_checker.py:412-425` now raises `_err` (was `_warn`) — varip declarations reject loudly. Both corpus probes using varip removed (`varip-counter-state-positive-01`, `varip-var-udt-in-security-positive-01`). |

### H. New (chunk D)

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 34 | `order.ascending/descending` | ✅ Runtime | **[RESOLVED]** Collection sorting is fully implemented using runtime lambdas dynamically checking sorting orders. |
| 35 | `barmerge.*` / `alert.freq_*` | ✅ Runtime | Only work as `request.security()` kwargs; as free expressions emit `std::string(...)` typed as INT → mismatch |

## Minor issues — themes

(~62 total. F-numbered items verified individually via live transpile harness.)

### Counts / spec drift
- **Off-by-one counts**: `label.style_*` 21 not 22; `plot.style_*` 11 not 12; `text.*` 10 not 8; `box/label/line` counts 29/21/21 not 27/20/20; `table.*` 22 fns + 1 var not 20; `matrix.*` 48 fns (PineForge implements 47 — `median` omitted intentionally per `KNOWN_MATRIX_OMISSIONS`).
- **Missing dividends/earnings vars in doc**: `dividends.gross/.net`, `earnings.actual/.estimate/.standardized` (Pine spec lists; doc omits).
- **`array.* (54 entries)`** matches spec.

### Spec-type divergence
- **`adjustment.*` / `backadjustment.*` / `settlement_as_close.*` (9 entries)** [F5]: **[PARTIALLY RESOLVED 2026-05-29, PR codegen#13]** `request.security(adjustment=…)` non-no-op values (e.g. `adjustment.splits`, `backadjustment.on`, `settlement_as_close.on`) now reject loudly via `SECURITY_ADJUSTMENT_ALLOWED_VALUES` in `support_checker.py`. The underlying constant-type drift (string vs int) is unchanged but no longer masks engine silent-drop because the kwarg path itself is gated.
- **`currency.*`** typed STRING by analyzer but emit int 0 (already class A).
- **`syminfo.recommendations_date` / `target_price_date`** [F17]: emit `na<double>()` even though Pine types `series int`; inconsistent with sibling `expiration_date` (`na<int64_t>()`). Risk of double→int coercion surprise.

### Codegen silent fallthrough fallout (beyond class A)
- **`chart.bg_color` / `chart.fg_color`** [F1]: emit `false` via `ns=="chart"` fallthrough; analyzer types COLOR; latent type mismatch in non-trivial use.
- **`display.pine_screener`** [F19]: falls through `_disp.get(…, "0")` — numerically equal to `display.all`. Cosmetic, but constant-equality probes will see spurious match.
- **`format.mintick / .percent / .volume`** [F20]: emit raw `std::string("mintick"|"percent"|"volume")` via unknown-member fallback. Runtime `pine_str_tostring` string-matches; doc's "✅ runtime" is true in effect but constant carries no type info.
- **`export` keyword** [F6]: no lexer KEYWORD entry. Library form blocked at `library()` hard-reject (clean). Non-library form (`x = export + 1`) silently passes support_checker; codegen verbatim-emits → downstream C++ "not declared" compile error.

### Backing / note inaccuracies (behaviour OK, doc misleading)
- **`math.round_to_mintick()`** [F7]: doc says backed by `BacktestEngine::round_to_mintick(price)`. Method does exist (engine.hpp:451-454, used by engine_fills.cpp) — but codegen does NOT call it; inlines `(std::round(x/syminfo_mintick_) * syminfo_mintick_)` without the engine method's NaN / `mintick<=0` guards.
- **`na(x)` function form** [F8]: lowers to `is_na(x)`, NOT `na<T>()`. Only the bare `na` identifier maps to `na<double>()`.
- **`str.startswith` / `str.endswith`** [F9]: doc says `std::string::starts_with/ends_with`. Actually uses `substr(0,n)==pre` and length-check + `compare(off,len,suf)` (pre-C++20 portability). Functionally equivalent.
- **`var` keyword** [F10]: doc says "static in on_bar". Actually class member + `_var_initialized` gate inside `on_bar` (per-instance, NOT C++ `static`). Pattern is safer — allows multiple `GeneratedStrategy` instances in one process.
- **Annotations** [F11]: lexer drops every `//@*` line; only `//@version=N` is recovered via regex on raw source. `//@variable / //@strategy / //@description` etc. silently discarded. Doc overstates "transpiler reads".
- **`strategy()`** [F12]: call site lowers to `/* strategy declaration */` comment placeholder. State lives on `StrategyDecl` AST → materialised by `emit_top` into ctor / `set_param` / `extern "C"` bridge. `StrategyOverrides` is host-side override layer, NOT the kwarg sink.
- **`strategy.default_entry_qty(fill_price)`** [F13]: lowers to `calc_qty(fill_price)` engine method (reads `default_qty_value_` + `default_qty_type_` + equity). Doc's `default_qty_value_` only names underlying setting.
- **`request.security()`** [F14]: 3 hard restrictions (support_checker.py:683-760): (1) same-chart symbol only (`syminfo.tickerid/.ticker`); (2) `lookahead_on` rejected outright; (3) `currency` / `ignore_invalid_symbol` rejected. Allowed params: `symbol, timeframe, expression, gaps, lookahead`.
- **`ta.tr` (property) vs `ta.tr(handle_na)` (fn)** [F15]: property hardcodes `handle_na=true` via inline lambda; fn form dispatches `ta::TR` ctor with `handle_na=false` default. Matches Pine v6 spec divergence between forms.
- **`strategy.equity`** [F16]: lowers to `(current_equity() + open_profit(current_bar_.close))` — includes mark-to-market open P&L. Doc's bare `current_equity()` understates.
- **`strategy.openprofit_percent`** denominator: `(open_profit / initial_capital * 100)`; Pine: `openPL / realizedEquity * 100`. Diverges once `netprofit ≠ 0`.
- **`syminfo.country` LSE → `"UK"`** but Pine spec ISO `"GB"`. Several other PREFIX_TO_COUNTRY entries warrant spot-check.
- **TZ divergence**: bare `hour/minute/dayofweek/...` use UTC (`gmtime_r`), not exchange TZ per Pine spec.

### Behavioural gaps
- **`str.tonumber`** lacks try/catch → throws `std::invalid_argument` on parse failure instead of returning na (real behaviour bug).
- **`str.replace`** 4-arg form (with `occurrence`) unhandled.
- **`timestamp()`** [F21]: only 1 of 6 Pine overloads. Silent bugs: returns `"0"` if <3 args; silently drops `second` arg; `(timezone, year, ...)` overload feeds string into year slot → UB; always UTC via `timegm`.
- **`/=` and `%=`** NOT cast/fmod'd like `/` and `%` — int/float operand semantics mismatch (Pine v6 always-float `/`; PineForge integer-modulo `%=`).
- **`array.size()` / `matrix.size()`** [F18]: emit `(double)v.size()`; Pine returns `series int`. Comparisons promote OK; explicit int-overload dispatch would surprise.
- **Array silent gaps**: `array.new_color/_label/_line/_linefill/_box/_table` silently emit `0` (only 5 of 11 `new_*` handled); `array.copy/slice` hardcode element type to `double`; `array.join` numeric-only; `array.sort/stdev/variance` drop 2nd arg.
- **`string(x)`** numerics only; `int(x)` no NaN propagate.
- **Drawing-var method calls** produce dangling C++ identifiers (label/line/box/linefill — `var label l = label.new(...)` dropped, then `l.set_text(...)` references undeclared identifier).

### Diagnostic / warning coverage gaps
- **Syminfo na-fields "conditional-use" warning** [F22]: warning DOES exist (`support_checker._SYMINFO_SILENT_GAP_FIELDS`) but covers only 6 fields (`sector, industry, isin, expiration_date, current_contract, mincontract`). Other na-accept fields (`employees, shareholders, shares_outstanding_*, recommendations_*, target_price_*`) silently return `na<double>()` even in `if/?:` context — NO warning.
- **`ta.<name>` bare property reads** require sibling `ta.<name>(...)` call to register call site, else falls to `std::string("<name>")`.
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

**Remaining open work:**

1. **`input.source` series emission** (#27, codegen issue #9): cross-repo blocker — needs `get_input_source(name, default_series) → const Series<double>&` helper in pineforge-engine. Currently produces frozen scalar.
2. **Remaining class A items** — #5 (`extend.*, font.*, hline.*, ...` namespaces) and #6 (`color.from_gradient`) still emit `std::string("<member>")` or hardcoded `0`. Same rejection pattern applies.
3. **`input.time`** (#28) — still uses `get_input_int` where doc claims `get_input_double`. Verify which is correct against Pine v6 spec.
4. **Class H #35** — `barmerge.*` / `alert.freq_*` as free expressions still emit `std::string(...)` typed as INT. Same fix pattern as #36/#37.
5. **Doc** — refresh `pine_v6_coverage_detail.md` bucket assignments now that rejections moved several identifiers from "✅ Runtime" or "❓ Unknown" to "❌ Unsupported (loud reject)".
6. **Regenerate headline counts** after the remaining fixes.
