# Pine v6 Coverage — Exhaustive Audit (Master Index)

| Field | Value |
|---|---|
| **Audit date** | 2026-05-21 (initial); enriched same day with F1–F22 clarifications |
| **Audited doc** | `pine_v6_coverage_detail.md` (1039 lines, 941 identifiers) |
| **Method** | 4 parallel C++/Pine v6 expert agents, line-by-line, Playwright MCP for Pine v6 ref + grep on engine + transpiler. Follow-up: 2 clarification agents with live transpile harness. |
| **Status** | Standalone — chunk reports and clarification reports consolidated below and removed. |

## Headline

**38 critical issues, ~62 minor issues** across 941 identifiers.

| Severity | Definition | Count |
|---|---|---|
| Critical | Doc claim provably wrong vs codegen/runtime/spec → wrong code or silent miscompile | **38** |
| Minor | Note stale, ambiguous, off-by-one count, undocumented divergence | **~62** |

## Critical issues — consolidated by class

### A. Silent fallthrough → `return "0"` or `return "false"` (most dangerous)

Transpiler dispatcher misses identifier → emits literal `0` / `false` / namespaced token → either compile failure or silent wrong-result. Pine doc claims a bucket that hides this.

| # | Identifier | Doc claim | Reality | Source |
|---|---|---|---|---|
| 1 | `last_bar_time` | ✅ Runtime | Zero codegen, compile fail | A (CI-A2) |
| 2 | `currency.*` (56) | 🔧 string constants | Emit int `0`; analyzer types as STRING → mismatch | B (CB-2) |
| 3 | `dayofweek.monday..sunday` | ✅ int constants 1..7 | Emit literal token `dayofweek.monday` (no NS) → compile fail | B (CB-1) |
| 4 | `session.regular / .extended` | ✅ string constants | Emit `false` | B (CB-3) |
| 5 | `extend.*, font.*, hline.*, location.*, plot.*, scale.*, shape.*, text.*, xloc.*, yloc.*` | ⏭️ Parse-and-skip | Emit `std::string("<member>")`; analyzer types as INT → mismatch | B (CB-5) |
| 6 | `color.from_gradient()` | ❓ Unknown | Returns hardcoded `0` | C (CI-A5) |
| 7 | `color()` bare cast | ✅ Runtime | No handler → emits `color(x)` C++ call → compile fail | C (CI-A4) |
| 8 | `footprint.*` (9 fns + 1 type) | ❓ Unknown | No namespace → compile fail | C (CI-A8) |
| 9 | `volume_row.*` (8 fns + 1 type) | ❓ Unknown | No codegen support → ❌ hard-reject | D (CD-4) |
| 10 | `timeframe.from_seconds()` | 🔧 Transpiler | Falls through to `return "false"` | D (CD-3) |
| 11 | `syminfo.prefix()` / `syminfo.ticker()` fn forms | ✅ Runtime | Not implemented; `prefix` lowers to `na` | D (CD-2) |
| 12 | `max_bars_back()` | 🔧 Transpiler | ❌ hard-reject (`NOT_YET_FUNC`) | D (CD-1) |
| 36 | `chart.left_visible_bar_time` / `chart.right_visible_bar_time` | ❌ Unsupported | Emit `false` via `ns=="chart"` fallthrough (visit_expr.py:335); NOT rejected by support_checker; silently yields epoch 0 in time arithmetic | F2 |
| 37 | `dividends.*` / `earnings.*` (12 vars) | ❌ Unsupported (Pine: returns na) | No namespace handler → unknown-member branch emits `std::string("<member>")`; assigning to declared `double` causes **downstream C++ compile error**, not "always na" | F3 |
| 38 | `strategy.commission.{percent,cash_per_order,cash_per_contract}` | ✅ Runtime | Routes correctly **only** as `strategy(commission_type=…)` kwarg (via `_extract_literal_value` string match). As free expr, visit_expr.py:466-467 catch-all returns `"0"` for all 3 — values indistinguishable | F4 |

### B. Hardcoded wrong values

Codegen emits a literal constant instead of reading runtime state.

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 13 | `strategy.account_currency` | reads `syminfo_.currency` | hardcoded `"USD"` |
| 14 | `strategy.position_entry_name` | last pyramid entry id | hardcoded `""` |
| 15 | `strategy.closedtrades.first_index` | first index | hardcoded `0` (coincidentally correct) |

### C. Wrong aliasing

Identifier resolves to a different runtime symbol than spec.

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 16 | `last_bar_index` | computed from bar count | aliased to current `bar_index_` (support_checker flags DIVERGENT) |
| 17 | `timenow` | always na | aliased to bar `timestamp` |
| 18 | `time_close` (var) | `pine_time_close` | aliased to bar OPEN `timestamp` |
| 19 | `barstate.islastconfirmedhistory` | always false | emits `barstate_islast_` (true on last bar) |
| 20 | `timeframe.isminutes` | minutes-only | aliased to `tf_is_intraday` (true for seconds TF too) |
| 21 | `ta.tr` | `ta::TR` class | inline expression, not class |
| 22 | `strategy.max_drawdown_percent` | drawdown calc | does NOT call `max_runup_percent()`; uses wrong fn |

### D. Wrong Pine field name → support_checker rejects

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 23 | `syminfo.recommendations_buy_strong` | reads field | codegen uses `recommendations_strong_buy` (wrong name) → reject |
| 24 | `syminfo.recommendations_sell_strong` | reads field | codegen uses `recommendations_strong_sell` → reject |
| 25 | `syminfo.target_price_estimates` | reads field | codegen uses `target_price_analysts_count` → reject |

### E. Codegen uses wrong helper function

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 26 | `input.color()` | `get_input_string()` + color parse | uses `get_input_int` |
| 27 | `input.source()` | series source | uses `get_input_double` → static scalar, NOT a series |
| 28 | `input.time()` | `get_input_double()` | uses `get_input_int` |
| 29 | `color.rgb()` | optional `transp` arg | silently drops 4th arg → alpha always 0 |

### F. Broken bare-property form

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 30 | `ta.vwap` (bare) | works | excluded by `TA_COMPUTE_ARGS["vwap"] = [0]` non-empty → broken |
| 31 | `ta.<name>` bare property reads (all) | works | require sibling `ta.<name>(...)` call to register call site (MI-A8 — see minor) |

### G. Doc note inaccurate / wrong file

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 32 | Date/time fn-forms (`dayofmonth(...)`, etc.) | section "1-arg", uses `pine_time` | spec is 2-arg w/ tz; codegen uses inline `gmtime_r/localtime_r` lambdas |
| 33 | `varip` (sprint claim) | "transpile-time warning emitted" | no warning; silently merged into `is_var` |

### H. New (chunk D)

| # | Identifier | Doc claim | Reality |
|---|---|---|---|
| 34 | `order.ascending/descending` | ✅ Runtime | `array.sort` ignores order arg (always asc); `matrix.sort` emits uncompilable `std::string("descending") != 0.0` |
| 35 | `barmerge.*` / `alert.freq_*` | ✅ Runtime | Only work as `request.security()` kwargs; as free expressions emit `std::string(...)` typed as INT → mismatch |

## Minor issues — themes

(~62 total. F-numbered items verified individually via live transpile harness.)

### Counts / spec drift
- **Off-by-one counts**: `label.style_*` 21 not 22; `plot.style_*` 11 not 12; `text.*` 10 not 8; `box/label/line` counts 29/21/21 not 27/20/20; `table.*` 22 fns + 1 var not 20; `matrix.*` 48 fns (PineForge implements 47 — `median` omitted intentionally per `KNOWN_MATRIX_OMISSIONS`).
- **Missing dividends/earnings vars in doc**: `dividends.gross/.net`, `earnings.actual/.estimate/.standardized` (Pine spec lists; doc omits).
- **`array.* (54 entries)`** matches spec.

### Spec-type divergence
- **`adjustment.*` / `backadjustment.*` / `settlement_as_close.*` (9 entries)** [F5]: Pine spec types `const string`; codegen emits int `0/1/2`; analyzer types as `PineType.INT`. Engine ignores `request.security(adjustment=…)` value, masking divergence. Latent compile-fail if assigned then fed to string slot.
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
- **`varip` warning** claimed in doc — actually NOT emitted (already class G item 33).

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

1. **Triage the 35 critical issues** — split into (a) silent miscompile (must fix), (b) wrong-bucket doc fix only, (c) feature gap requiring engine work.
2. **Fix `dayofweek.*`, `session.regular/extended`, `extend/font/.../yloc` constant namespaces** — these crash or miscompile any script touching them.
3. **Fix transpiler silent fallthrough** — `return "0"` for unrecognized identifier is the common root cause; convert to hard-error so future drift surfaces immediately.
4. **Update doc** to reflect actual buckets + remove "1-arg function form" stale wording on date/time fn-forms section.
5. **Regenerate doc headline counts** after fixes.
