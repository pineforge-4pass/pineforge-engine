# Native feature parity: using pineforge-engine well without Pine

- **Status:** design + roadmap. Synthesis of three independent gap audits under the supervisor's R5 rulings. No engine code is changed by this document.
- **Tree:** `pineforge-engine` main @ `73817c1`, branch `design/native-feature-parity`.
- **Goal:** a user writes a strategy in pure C++ against `NativeStrategyHost` (no PineScript, no codegen, no `src/source`, no `src/compat/pine`) and gets every execution feature Pine strategies rely on. The Pine adapter becomes an optional, thin TradingView-parity layer.
- **Citations:** `file:line` at `73817c1`, each copied from an input or read fresh for this merge (marked *fresh*). Unique basenames are used; a path appears only where the basename is ambiguous. ADR-0001 lives on PR #255 (head `ec77693`), not in this tree, so its lines are written `ADR L<n>` and are carried from inputs F and O.

## 0. Inputs, rulings, merged tally

### 0.1 Inputs (same brief, three models; citations script-checked, 4 claims per input spot-checked by the supervisor)

| Key | Input | Rows | native | partial | missing | TV-only rows | Citations |
|---|---|---|---|---|---|---|---|
| F | `D1-fable.md` | 59 generic + 5 quirk | 30 | 14 | 15 | 5 | 245 |
| O | `D1-opus.md` | 75 | 33 | 12 | 27 | 3 | 385 |
| S | `D1-sol.md` | 45 | 13 | 12 | 20 | 9, counted inside "missing" | 131 |

Most status differences are row granularity (one input folds two features into one row). Real disagreements are named in the row's Notes with the ruling that settles them; §3.4 lists the lane-level ones. Test-count claims agree once the measure is named: 22 `tests/*.cpp` name `NativeStrategyHost`, 20 of them include no source/compat header (F), 14 files subclass it directly against 430 for `source::PineStrategyHost` (O) — *fresh* recount matches all three numbers.

### 0.2 Supervisor rulings applied (not re-litigated here)

| # | Ruling |
|---|---|
| R5-1 | **Classification.** Generic execution feature → kernel native API. TradingView quirk → adapter. Mixed feature → kernel gets the generic mechanism with a policy knob, the adapter selects the TV policy, the bare-host default is the plain generic behaviour. |
| R5-2 | **Brackets are not a new kernel object.** `WaitForApplied{RequestHandle parent}` (native_order.hpp:105-107) and `waiting_children(parent)` (native_order.hpp:1115, *fresh*) exist; L7 delivers a builder/toolkit over them plus relative anchors and the missing offsets. Size M. |
| R5-3 | **Price grid is its own kernel lane (L8).** Quantity grid is native already; the price grid (booked fill on the tick, optional quantized trigger test) is generic and missing. TV's exact half-tick rule stays in the adapter on top. |
| R5-4 | **Close reservations split.** Fractional reduce + reservation against sibling exits → kernel, inside the sizing lane. Callsite batching, two-call provenance, the POOC population predicate → adapter. |
| R5-5 | **Calc-timing is v1.** Sub-bar hook + callback chronology contract; the adapter keeps its current path (zero drift). |
| R5-6 | **Native HTF is v1.** A native subscription/registration API over the existing kernel feed machinery (`prepare_native_security_feeds` made native-callable); `engine_security.cpp` and the Pine scheduler path untouched; `TimeframeAggregator` self-aggregation is the documented interim. The lookahead default is a user question (§4 U1). |
| R5-7 | **C order/callback API is post-v1**, last lane, design retained (§2.iii). |
| R5-8 | **Coupling extraction splits** into NEUTRAL (small PRs after L1) and HASH-VISIBLE (post-v1, explicit campaign exception). F §2.ii a-h is the reference list; O §2.ii and S lane I are merged into it (§2.ii). |
| R5-9 | **Risk limits are in scope** as their own lane after margin; not a v1.0.0 blocker. |
| R5-10 | **Parity discipline per lane:** (a) neutrality, (b) twin test diffed by identity, (c) epoch budget. Stated once in §3.1. |
| R5-11 | **Lane list and order** L0-L13 (§3.2). |

### 0.3 Merged tally

89 merged rows = **77 generic + 12 TV-quirk**. Generic rows, as a bare host sees the generic core today: **native 34 · partial 14 · missing 29**. Of the 43 partial / missing rows, 41 are assigned to a lane, OT1 needs nothing for v1, and FP6 is unassigned pending a user decision (§4 U4).

---

## 1. Inventory

Columns: **Feature** (adapter mechanism, cited) · **Native today** (`yes` / `partial` / `no`, cited; `n/a` = pure TV-quirk row) · **Class** (K kernel, A adapter, K+A generic core + TV variant) · **Lane** (§3.2) · **Sources** (input row ids) · **Notes** (disagreement + ruling; `(single-source)` = only one input found it).

### 1.1 Order lifecycle and brackets

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| OL1 | Market / limit / stop / stop-limit / trail triggers (`trigger_for` pine_adapter.cpp:1624-1635; `entry()` pine_adapter.cpp:3944, `order()` pine_adapter.cpp:9085) | **yes** — `Trigger` native_order.hpp:77-96; geometric matching native_execution_consumer.cpp:3397-3445; `submit` native_host.hpp:487 | K | — | F:A1 O:A1 S:O2 | — |
| OL2 | Intents: flatten, reduce by units, reduce owner-opened units, transact, reverse-to (`close_all()` pine_adapter.cpp:6387 → `Flatten{}` pine_adapter.cpp:6415-6416; `Reduce{ExplicitUnits}` pine_adapter.cpp:6125-6134; `ReverseTo` pine_adapter.cpp:4378-4379) | **yes** — `OrderIntent` native_order.hpp:75, reduce sizes native_order.hpp:67-74, `ReverseTo` native_order.hpp:56-58, `OpeningShape` native_order.hpp:436-440, `Flatten` native_order.hpp:36 | K | — | F:A4 F:A8 O:A2 O:A3 S:O1 | — |
| OL3 | Amend / cancel one working order (`submit_or_replace` pine_adapter.cpp:1855-2380 → `host.replace` pine_adapter.cpp:2161-2227) | **yes** — `replace` native_host.hpp:488-489, `cancel` native_host.hpp:493; `ReplacedEvent` native_order.hpp:537-547; result and event types native_order.hpp:395-416, native_order.hpp:521-583 | K | — | F:A2 O:A4 S:O3 | id → handle map is host bookkeeping (F) → OL4 |
| OL4 | Cancel all / cancel by label; label → handle book (`cancel(id)` pine_adapter.cpp:9020-9066, `cancel_all` pine_adapter.cpp:9068-9083; `live_by_source_key_` pine_adapter.hpp:1217; predecessor state hand-carried pine_adapter.cpp:2239-2303) | **partial** — single-handle `cancel` native_host.hpp:493; no enumeration (`WorkingRequestCore::live()` is consumer-internal native_order.hpp:1007); labels exist but are unindexed (`Request::label` native_order.hpp:141) | K+A | L7 | F:A3 F:A2 O:A5 O:A14 S:O3 | **Disagree:** F partial / O no (A5) + partial (A14) / S yes (folded into O3). R5-1: cancel-all / by-label is generic → kernel. TV state inheritance across a same-id re-issue stays A. |
| OL5 | Per-point fill capacity, partial fills, residual working quantity (Pine uses only `ImmediateRemaining` pine_adapter.cpp:2373; residual closes pine_adapter.cpp:5221-5572) | **yes** — `Capacity` / `PointBudget` native_order.hpp:98-102, `Remaining` native_order.hpp:198-220, allowances native_order.hpp:290-303 | K | — | O:A16 S:O4 | — |
| OL6 | Ownership: legs bound to a parent / opening(s) / cohort; close the lots of a given entry, FIFO or ANY (`owner_for_close` pine_adapter.cpp:3564-3599, `cohort_for` pine_adapter.cpp:1539; FIFO prefix computed adapter-side pine_adapter.cpp:5519-5536) | **yes** — `Owner` native_order.hpp:104-119, `Authority` native_order.hpp:263-264, `CohortClose` native_order.hpp:260-262, `SelectedExposure` native_order.hpp:268-272, cohorts native_host.hpp:494-496, `CancelReason::OwnerGone` native_order.hpp:478 | K | — | F:A6 O:A8 O:A9 S:O5 | FIFO *ordering* is host-computed (O) |
| OL7 | OCA cancel / reduce groups (`group_for` pine_adapter.cpp:1637-1643) | **yes** — `Group` / `GroupEffect` native_order.hpp:121-128, `CancelReason::Group` native_order.hpp:475-480, `ReservationReducedEvent` native_order.hpp:681-691 (deferred form native_order.hpp:693-703) | K | — | F:A12 O:A6 O:A7 S:O7 | — |
| OL8 | Immediate execution at the current point (`immediately=true`) | **yes** — `inspect_current_execution` / `execute_current` native_host.hpp:480-481 | K | — | F:A9 | (single-source row; O:F2 cites the same call) |
| OL9 | Birth eligibility: an order accepted at bar N cannot fill on N's already-delivered points | **yes** — `point_eligible` native_order.hpp:357-361, include/pineforge/order_birth.hpp:59-104, contract native-engine.md:263-269 | K+A | — | F:A13 O:A10 S:R4 | **Disagree:** F yes / O partial / S "no, TV quirk". R5-1: the generic core (birth cursor) is native; TV's bar-granular first-bar rule is the A half → OL14. |
| OL10 | TP/SL bracket per entry, armed before the entry fills, OCA between legs (`exit()` pine_adapter.cpp:6509; lowering pine_adapter.cpp:7061-7084, pine_adapter.cpp:7595-7601, pine_adapter.cpp:7619-7653; lifecycle exit_lifecycle.cpp:9-65) | **partial** — every primitive is native (`WaitForApplied{parent}` native_order.hpp:105-107, `Reduce{OwnerOpenedUnits}` native_order.hpp:70-74, `Member{…, Cancel}` native_order.hpp:121-128, `waiting_children` native_order.hpp:1115); no builder | K (toolkit) | L7 | F:A10 O:A8 S:O6 | **Disagree:** F "yes (primitives)" + toolkit / S partial + new kernel bracket object, lane size L and prerequisite for everything. R5-2: builder over the existing primitives, size M. |
| OL11 | Bracket levels relative to the parent's fill (profit / loss ticks; derived pine_adapter.cpp:6689-6692; `materialize_relative_exits` pine_adapter.cpp:8950-8991, pine_adapter.hpp:969-981) | **partial** — composable: the host computes levels in `on_native_applied` and submits; suffix eligibility makes that same-bar correct native_execution_consumer.cpp:3248-3257. `Trigger` itself is absolute-only native_order.hpp:96 | K | L7 | F:A11 O:A11 S:O6 | **Disagree:** F partial (toolkit is enough) / O no (kernel `TriggerAnchor`). R5-2: L7 delivers relative anchors; kernel anchor primary, toolkit materialization is the zero-kernel fallback. |
| OL12 | Working-order view for the host and outside observers (`strategy_pending_order_*`; `PendingIntentView` pine_adapter.hpp:676-692; the POD is filled only by the adapter pine_adapter.cpp:16317-16346) | **no** — base returns 0 / -1 engine_consumer.cpp:124-133; the POD is TV-shaped (`tv_carry_qty` pending_order_mirror.hpp:73, `pine_birth_reach` pending_order_mirror.hpp:135) | K+A | L7 (C++), L13 (C) | F:J5 O:J6 S:X4 | **Disagree:** O "partial + TV-shaped" / F, S no. A bare host reads zeros → no. R5-1: neutral view → kernel; POD shape and Pine journal retention stay A (POD frozen, §2.ii j). |
| OL13 | *(quirk)* Same-bar command batching and the seven deferred queues (pine_adapter.hpp:1219-1232); KI-62 open-order priority (order_priority.cpp:13-60, selected pine_adapter.cpp:821-880); per-bar priced-entry throttle, BUY-before-SELL batch, short-seed plan / collision (pine_adapter.cpp:1686-1853, pine_adapter.cpp:8923-8947), reversal-gap bracket policy (pine_adapter.hpp:589-614, pine_adapter.hpp:935-958, pine_adapter.hpp:1002-1017; pine_adapter.cpp:12940, pine_adapter.cpp:13085) | n/a — native priority is acceptance / incarnation order native-engine.md:264-266 | A | — | F:A15 O:A15 S:O9 S:R3 | R5-1 names KI-62. S: the kernel exposes deterministic insertion order only. |
| OL14 | *(quirk)* Exit activation / suspension / revival barriers, historical birth reach (`select_exit_activation` exit_activation.cpp:42-88, enforced pine_adapter.cpp:11053-11060; exit_activation.cpp:18-88, order_birth.cpp:5-14, pine_adapter.cpp:549-581) | n/a | A | — | S:R4 O:A10 | TV half of OL9 |

### 1.2 Trailing

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| TR1 | Trail arm → track → trigger, with a state query | **yes** — `Trail{offset, arm_price}` native_order.hpp:92-95; states native_order.hpp:280-288; arm / track / fire native_execution_consumer.cpp:3409-3432; `NativeTrailState` native_host.hpp:334-339, `trail_state` native_host.hpp:478 | K | — | F:B1 O:B1 S:T1 | — |
| TR2 | Trail activation / offset in ticks or points, relative to the entry fill (`trail_points_to_ticks` + grid snap exit_lifecycle.cpp:22-27; helpers engine_internal.hpp:165-173; `trail_activation_level` pine_adapter.hpp:250-252) | **partial** — absolute prices and a raw price distance only | K | L7 | F:B2 O:B2 S:T2 | **Disagree:** S classes all trail operand conversion as TV quirk. R5-1: tick / relative spelling is generic. |
| TR3 | Zero-distance trail, "ride the best, exit on any adverse tick" (adapter substitutes `tick * 0.5` pine_adapter.cpp:6700-6712) | **no** — the kernel fails the run when `offset <= 0` (`checked_trail_stop` → native_execution_consumer.cpp:3424-3429) | K+A | L7 | O:B3 F:B3 S:T2 | **Disagree:** O kernel / F, S quirk. R5-1: accepting a zero offset is generic; TV zero-offset *pricing* (pine_adapter.cpp:9317-9440) stays A. |
| TR4 | Re-price a trail without losing the running extreme (adapter skips `replace` and reads `trail_state()` pine_adapter.cpp:2191-2226) | **partial** — the read exists native_host.hpp:478; `replace` always rebuilds trigger state | K+A | L7 | O:B4 F:B3 | Status is single-source (O). R5-1: retain-trail replace is generic; TV's "retained best across re-issue" convention (pine_adapter.hpp:253-255) is A. |
| TR5 | *(quirk)* TV trail conventions: half-tick arm threshold against tick-quantized extremes pine_adapter.cpp:7655-7660, zero-offset pricing pine_adapter.cpp:9317-9440, retained best pine_adapter.hpp:253-255, operand / restart rules pine_adapter.cpp:6569-6770, pine_adapter.cpp:7654-7787 | n/a | A | — | F:B3 S:T2 | R5-1 |

### 1.3 Sizing bases and reservations

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| SZ1 | Explicit units (`Transact{signed}` after a grid floor pine_adapter.cpp:3950-3954, pine_adapter.cpp:4381) | **yes** — `Transact` in `OrderIntent` native_order.hpp:75 | K | — | F:C1 O:C1 S:S1 | — |
| SZ2 | Host override of the resolved price / units (`resolve_terms` pine_adapter.cpp:9310-10487) | **yes** — `HostSized` native_order.hpp:60-65 + `resolve_execution_terms` native_host.hpp:455-459, invoked native_execution_consumer.cpp:2721-2739, guarded native_execution_consumer.cpp:2851-2885 | K | — | F:C2 O:H9 S:S2 | **Disagree:** S "partial" (a hook is not a basis). The hook is native; bases are SZ3-SZ5. It stays the custom-policy seam after L3. |
| SZ3 | Percent-of-equity sizing (`default_sizing_units` pine_adapter.cpp:1576-1590) | **no** — `HostSized` without host units is `TermsUnresolved` native_execution_consumer.cpp:2753-2755 | K+A | L3 | F:C3 O:C2 S:S3 | TV parts stay A: rounded equity pine_adapter.cpp:1580, money floor-to-lot pine_adapter.cpp:1589 (→ SZ10) |
| SZ4 | Cash-value sizing (pine_adapter.cpp:1570-1575) | **no** — same | K | L3 | F:C2 O:C3 S:S4 | — |
| SZ5 | Fee-net sizing (commission reserve `/(1+c)` pine_adapter.cpp:1582-1585; `percent_commission_live_equity` pine_adapter.cpp:2441-2473) | **no** — only a dead primitive, `reserve_percent_commission` engine.hpp:1522, with no caller in `src/` | K | L3 | F:C3 O:C6 S:S3 | **Disagree:** F lists the reserve under quirk yet offers a knob / O kernel, "partial" / S knob. R5-1: fee-net option is generic. |
| SZ6 | Placement-time vs fill-time sizing (`sizing_snapshot` pine_adapter.cpp:1551-1566, `PineSizingSnapshot` pine_adapter.hpp:124-131; fill-time exceptions pine_adapter.cpp:4709-4717) | **no** | K+A | L3 | F:C3 O:C7 S:S3 | **Disagree:** O adapter-only / F, S generic timing knob. R5-11 (L3 scope) + R5-1: the knob is kernel; TV's frozen tuple stays A through `HostSized`. |
| SZ7 | Quantity step | **yes** — `quantity_grid` admission native_run_spec.hpp:167, native_order.hpp:366-383; host-sized terms snap (`ExecutionGridPolicy` native_order.hpp:442-456); exact grid validation native_execution_consumer.cpp:2768-2771 | K | — | F:C4 O:C5 S:S1 | **Disagree:** F, O partial / S yes. R5-3: the quantity grid is native already. L3's `Sized` intent reuses the snap policy. |
| SZ8 | Fractional reduce of the position / a cohort (`quantize_close_units` pine_adapter.cpp:2528-2548, `compute_exit_reservation` pine_adapter.cpp:2567-2687) | **no** — sizes are `ExplicitUnits` / `OwnerOpenedUnits` / `HostSized` only native_order.hpp:60-74 | K | L3 | F:A7 O:A12 O:C4 S:S5 | F's "partial" counts the units-reduce of OL2. R5-4 → L3. |
| SZ9 | Exit-quantity reservation against sibling exits (pine_adapter.cpp:2567-2890, pine_adapter.cpp:6898-7224) | **partial** — `PointBudget` native_order.hpp:99-102 and group `Reduce` native_order.hpp:121 give a generic reservation; no net-of-siblings claim for fraction sizes | K+A | L3 | F:A14 S:S5 S:O10 O:A12 | F proposed nothing. R5-4: generic half → L3; the Pine ledger → SZ11, SZ12. |
| SZ10 | *(quirk)* Ten-significant-digit money rule (`source_money_round` pine_adapter.cpp:396-403, `source_money_floor_lot` pine_adapter.cpp:404; twin pine_policy_support.hpp:9-15); exact percent-exit rounding (`quantize_percent_exit_units` pine_adapter.cpp:2550-2565) | n/a — kernel money is plain binary64 engine_execution.cpp:1011-1026 | A | — | O:C8 F:C3 S:M4 S:S5 | R5-1 |
| SZ11 | *(quirk)* Same-bar `strategy.close` callsite batching, two-call provenance, entry-id ledger (pine_adapter.cpp:5221-5572; ledger pine_adapter.hpp:1247-1258; kernel residue is comment-only engine.hpp:520-543) | n/a | A | — | S:O8 F:A15 O:A15 | **Disagree:** S "generic → kernel" (its P2 `ReplaceByKey` / `Fifo`). R5-4: adapter. |
| SZ12 | *(quirk)* POOC reservation-growth market-entry population predicate (`select_reservation_growth_sources` src/compat/pine/reservation_expansion.cpp:9-34, called from pine_adapter.cpp:727-819) | n/a | A | — | S:O10 | (single-source) R5-4 confirms S's own split: concept → SZ9, predicate A. |

### 1.4 Margin, admission, liquidation, risk

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| MG1 | Allowed opening directions (pine_adapter.cpp:1471) | **yes** — `allowed_open_directions` native_run_spec.hpp:172 → `MatchRejectReason::OpeningDirection` native_execution_consumer.cpp:1989-1997 | K | — | F:D4 O:D7 S:M1 | the one risk rule the adapter delegates (O) |
| MG2 | Max absolute position size | **yes** — `max_abs_units` native_run_spec.hpp:170 tests the resulting book native_execution_consumer.cpp:1998-2001 | K+A | — | F:D5 O:D5 S:M1 S:M5 | **Disagree:** O, S:M5 partial. R5-1: the generic cap is native; the adapter's live-position `>= held` variant (pine_adapter.cpp:10808-10811) is TV. |
| MG3 | Open-lot cap (pyramiding) | **yes** — `max_open_lots` native_run_spec.hpp:171 → `MaxOpenLots` native_execution_consumer.cpp:2002-2005 | K+A | — | F:A5 O:A13 O:D6 S:M1 | **Disagree:** F, O:A13 partial / O:D6, S yes. R5-1: Pine's per-cycle entry count (pine_adapter.cpp:4235-4281, cap left unset pine_adapter.cpp:1468-1470) is TV. |
| MG4 | Initial-margin opening gate (adapter answers `AdmitWithHostMargin` from frozen signal-time equity pine_adapter.cpp:10977-11042 and leaves the kernel gate unset pine_adapter.cpp:1472-1474) | **yes** — `initial_margin_fraction` native_run_spec.hpp:173; `required = resulting_abs_notional * f` against marked equity native_execution_consumer.cpp:2006-2013 | K | — | F:D1 O:D1 S:M2 | **Disagree:** F, O partial (they fold per-side in) / S yes. Row split → MG5. |
| MG5 | Per-side initial margin (pine_adapter.cpp:11441-11442, pine_adapter.cpp:10925) | **no** — one `std::optional<double>` native_run_spec.hpp:173 | K | L4 | F:D1 O:D2 | R5-1 |
| MG6 | Maintenance margin + forced liquidation (`submit_margin_call_slice` pine_adapter.cpp:11432-11607, `submit_margin_call_units` pine_adapter.cpp:11609-11655, `schedule_margin_call_path` pine_adapter.cpp:12157-12234) | **no** — "no maintenance liquidation" native_run_spec.hpp:173-174; margin is checked only when `inspect.would_open` native_execution_consumer.cpp:1988 | K+A | L4 | F:D2 O:D3 S:M3 | TV parts → MG15 |
| MG7 | Liquidation slice sizing (grid floor, ×4 restore, one-contract fallback pine_adapter.cpp:11506-11537) | **no** | K+A | L4 | O:D4 F:D2 S:M4 | **Disagree:** F generic knob `ShortfallMultiple` / O "×4 is A" / S quirk, yet a 4.0 default inside its generic model. R5-1: policy knob in the kernel, plain default (restore-minimum), the adapter selects TV's. |
| MG8 | Liquidation price query (pine_strategy_host.cpp:173-194) | **no** | K+A | L4 | F:D3 S:R5 | **Disagree:** F generic / S adapter-only. R5-1: liquidation price is generic; the Pine spelling stays A. |
| MG9 | FX revaluation of margin (`apply_fx_open_margin_slice` pine_adapter.cpp:2980-2996, `apply_fx_opening_margin_slice` pine_adapter.cpp:2998-3026) | **no** — staged FX curve native_host.hpp:484 but no margin re-check. **Closed by audit lane N6:** `NativeMarginCheckKind::FxRoll` — a step of the declared curve is a check point of its own, measured at the unchanged price before the first point the new rate converts (`tests/test_native_margin_fx_roll.cpp`). The adapter refuses the point by kind and keeps its broker-open slice; its two FX limits are the N6 follow-ups below (§4) | K | L4 → N6 | O:D8 | (single-source) |
| MG10 | Max drawdown halt (`update_risk_state` pine_adapter.cpp:11387-11414) | **no** — observation field only include/pineforge/market_admission.hpp:44; composable today from `native_marked_equity` native_host.hpp:499 | K | L9 | F:D6 O:E1 S:M6 | R5-9 |
| MG11 | Max consecutive losing days (`SourceDayLedger` pine_adapter.hpp:633-641; gate pine_adapter.cpp:10802-10805) | **no** — observation only include/pineforge/market_admission.hpp:43 | K | L9 | F:D6 O:E2 S:M6 | R5-9 |
| MG12 | Max intraday loss → flatten + block (pine_adapter.cpp:11416-11430, forced flatten pine_adapter.cpp:12381-12411) | **no** — observation only include/pineforge/market_admission.hpp:45 | K | L9 | F:D7 O:E3 S:M6 | R5-9 |
| MG13 | Max filled orders per day (intraday_cap.hpp:83-241, intraday_order_budget.hpp:27-92; pine_adapter.cpp:12424-12658) | **no** | K+A | L9 | F:D8 O:E4 S:M7 | Count cap K; quota transfer, POOC deferral, TV close price A |
| MG14 | Risk-day boundary (`chart_day_key` pine_adapter.cpp:11281-11305) | **no** — the calendar exists native_calendar.hpp:363-366, there is no risk-day ledger | K+A | L9 | O:E5 S:M7 | The chart-day shortcut stays A; the kernel keys on session / calendar day |
| MG15 | *(quirk)* TV margin call: money rounding pine_adapter.cpp:11492-11499, 4× shortfall pine_adapter.cpp:11520, POOC chronology exceptions pine_adapter.cpp:12161-12201, 1×-long money call pine_adapter.cpp:11657-11769, fill pricing pine_adapter.cpp:11588-11597 | n/a | A | — | F:D2 F:G4 O:D4 S:M4 | R5-1 |
| MG16 | *(quirk)* Historical market-admission review / retention journal (src/compat/pine/market_admission.cpp:33-257; pine_adapter.cpp:2024-2091, pine_adapter.cpp:12662-12720) | n/a | A | — | S:R2 | (single-source) |

### 1.5 Calculation timing

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| CT1 | Calculate at script-bar close | **yes** — `on_native_bar` native_host.hpp:450, dispatched native_execution_consumer.cpp:4318-4340 (call site native_execution_consumer.cpp:4449) | K | — | F:E1 O:F1 | — |
| CT2 | Process orders on close (pine_adapter.cpp:1463-1464) | **yes** — `NativeCloseExecution::AfterCalculation` native_run_spec.hpp:25-28; native_execution_consumer.cpp:4451-4454 | K | — | F:E2 O:F6 S:C1 | — |
| CT3 | Pre-match hook at bar open (`on_bar_open` pine_adapter.cpp:13551) | **yes** — `on_native_bar_open` native_host.hpp:446 runs before the open match native_execution_consumer.cpp:4394-4398; `execute_current` legal native_host.hpp:481 | K | — | F:E3 O:F2 | hazard → CT4 |
| CT4 | Partial-bar view / open-bar lookahead guard | **no** — the open hook receives the *complete* script bar: `invoke_bar_open_callback(engine, bar, point)` native_execution_consumer.cpp:4395 with `engine.current_bar_ = bar` native_execution_consumer.cpp:4187 | K | L5 | F:E3 | (single-source) The adapter needs the full bar, so the guard is opt-in. |
| CT5 | Raw input observation before aggregation | **yes** — `on_native_input` native_host.hpp:440, `NativeInputContext` native_host.hpp:402-407 | K | — | O:F3 | (single-source row) |
| CT6 | Calculate on order fills (scheduler pine_scheduler_native.cpp:588-640, pine_scheduler_native.cpp:643-669; `begin_coof_recalc` pine_adapter.cpp:3661, `flush_coof_tail` pine_adapter.cpp:3925, `end_coof_recalc` pine_adapter.cpp:3682) | **partial** — `on_native_applied` native_host.hpp:452 fires mid-path after each fill native_execution_consumer.cpp:3710, native_execution_consumer.cpp:4101-4122; commands are legal there native_execution_consumer.cpp:956-964; a request born there is eligible on the unconsumed rest of the bar native_execution_consumer.cpp:3248-3257, native_order.cpp:1397-1405. Missing: calculation re-entry, a documented chronology, a bar-so-far view | K+A | L5 | F:E4 O:F5 S:C2 | **Disagree:** F partial / O, S no; F, S v1 / O deferrable. R5-5: v1, sub-bar hook + chronology contract, adapter path unchanged. TV parts → CT11. |
| CT7 | Calculate on every tick | **partial** — `on_native_tick` native_host.hpp:443 fires per accepted realtime print native_execution_consumer.cpp:5383-5387; no per-tick calculation in batch; the guide pins "close-only" native-engine.md:374 | K | L5 | F:E5 O:F4 S:C3 | **Disagree:** F yes / O partial / S no. R5-5: v1. Not an adapter feature either: absent from `PineStrategyConfig` pine_adapter.hpp:39-53, the runner rejects it main.cpp:213, the Pine stream route refuses it pine_strategy_host.cpp:266-268. |
| CT8 | Historical intrabar calculation (per lower-TF sub-bar / magnifier sample) | **no** — with an `IntrabarPath` the callback still fires once per script bar native_execution_consumer.cpp:4515-4615 | K | L5 | F:E6 O:F8 S:C4 | — |
| CT9 | Intrabar (magnifier) matching path + path-order policy (pine_adapter.cpp:1475-1526) | **yes** — `IntrabarPath` native_run_spec.hpp:86-126, delivery native_execution_consumer.cpp:4457-4628; `NativePathOrder` native_run_spec.hpp:57-61 | K+A | — | F:E7 F:G5 O:F7 S:C4 | **Disagree:** S partial (it wants callback cadence → CT8). TV sampler choice is A. |
| CT10 | Batch → warmup → realtime forward execution | **yes** — `stream_*` engine.hpp:3086-3096, phases native_host.hpp:28-32, C ABI c_abi.cpp:522-641 | K+A | — | F:E8 O:J3 S:C5 | **Disagree:** S partial. R5-1: the lifecycle is native; Pine trade-start suppression and realtime-tail / probe rules (pine_strategy_commands.cpp:15-21, pine_strategy_host.cpp:227-320) are A. S's "neutral tail + trade-window policy" is single-source and unassigned (§4 Q8). |
| CT11 | *(quirk)* COOF TV specifics: waypoint-only refill (`coof_next_waypoint` pine_adapter.cpp:3803), two-fills-at-open rule, script-state rollback (pine_scheduler_native.cpp:53-104; hooks engine.hpp:2356-2358), cascade guard `kCoofLoopGuard` pine_scheduler_native.cpp:682, deferral queue of `PendingCoofRequest` pine_adapter.hpp:983-990 | n/a | A | — | F:E4 O:F5 S:C2 | R5-5: the adapter keeps its path |

### 1.6 Higher timeframes and auxiliary feeds

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| HT1 | HTF series of the same symbol (`request.security`): evaluators registered pine_strategy_host.cpp:1304-1306, pumped from the scheduler pine_strategy_host.cpp:1316-1400; state lives in `BacktestEngine` engine.hpp:1974-2178, engine.hpp:2284-2330, engine_security.cpp:22 | **no** sanctioned path — `configure_security_evaluators` is an empty virtual engine.hpp:2346 whose only caller is pine_strategy_host.cpp:1305 | K | L6 | F:F1 O:G-1 S:H1 | **Disagree:** S partial (it counts kernel-side storage → HT2); F, S v1 / O deferrable. R5-6: v1. |
| HT2 | Authoritative (exchange) HTF bars substituted into a series (`set_native_security_feed` engine.hpp:3061, engine_aux_security.cpp:89-140, C ABI c_abi.cpp:751) | **partial (inert)** — pre-run staging is accepted native_execution_consumer.cpp:991-1001 but never consumed: `prepare_native_security_feeds` is protected engine.hpp:2897, sole caller pine_strategy_host.cpp:1325; in-run it latches `UnsupportedSource` native_execution_consumer.cpp:1002-1007 | K | L6 | F:F2 O:G-2 S:H1 | F says "no" on the same facts. "native" in these names means TradingView's native-timeframe bars, not the native host engine_aux_security.cpp:48-57. |
| HT3 | Lower-timeframe arrays (`request.security_lower_tf`; `register_security_lower_tf_eval` engine.hpp:2294) | **no** — `IntrabarPath::lower_tf` drives matching only | K | L5 (sub-bar hook); series form optional in L6 | F:F4 O:G-3 S:H2 | **Disagree on lane:** F via `on_native_sub_bar` (the host already owns the lower bars it put in the path) / O, S via the series API. Not ruled; v1 = the L5 hook (R5-5). |
| HT4 | Auxiliary finer feed driving `request.security` (pine_aux_security.cpp:20-197, pine_aux_security.cpp:218-390; the kernel base returns false engine_consumer.cpp:143-146) | **no** | K+A | L6 | F:F3 S:H2 | **Disagree:** F quirk (TV split-feed harness construct) / S generic. R5-1 + R5-6: aux feed registration is generic; chart-slice mapping and deferred first-bucket publication stay A. |
| HT5 | Publication mode: lookahead, gaps, session / early-close boundaries (pine_scheduler.cpp:41-130, pine_aux_security.cpp:232-315) | **no** for a bare host | K+A | L6 | S:H3 F:F1 O:G-1 | **Disagree:** F "no `lookahead_on`, by construction" / O `lookahead = false` default, available / S `lookahead_on` field, status partial. R5-6: user question (§4 U1); the exact TV projection stays A. |
| HT6 | Self-aggregation recipe (interim) | **yes** — `TimeframeAggregator` is public and engine-free timeframe.hpp:288-486, fed from `on_native_input` native_host.hpp:440; Pine uses the same class pine_scheduler_native.cpp:117-128 | K | L0 (document) | O:G-4 F:F1 | R5-6 |

### 1.7 Fill price, slippage, commission, FX

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| FP1 | Trigger geometry on the modeled path; fill at the level or at the gap price; market-if-touched | **yes** — native_execution_consumer.cpp:3397-3445, native_matching.hpp:59-83; kind native_execution_consumer.cpp:3689-3697; `Limit::fill_through` native_order.hpp:79-84 enforced native_execution_consumer.cpp:3676-3688, re-checked after host terms native_execution_consumer.cpp:2866-2885 | K | — | F:G3 O:H1 O:H2 O:H3 | — |
| FP2 | Slippage in ticks, limit-clamped | **yes** — `slippage_ticks` native_run_spec.hpp:163 applied once native_execution_consumer.cpp:3671-3672, clamp native_execution_consumer.cpp:3687 | K+A | — | F:G1 O:H7 S:F2 | TV's round-then-slip order (pine_adapter.cpp:9553-9562) is A |
| FP3 | Commission: percent / cash per unit / cash per execution (`fee_kind_for` pine_adapter.cpp:415-422) | **yes** — `NativeFeeKind` native_run_spec.hpp:19-23; `quote_execution_commissions` engine_execution.cpp:960-996 | K | — | F:G2 O:H8 S:F1 | — |
| FP4 | Booked fill quantized to the instrument tick, nearest or directional (`nearest_tick` pine_adapter.cpp:252-255, `source_bar_fill` pine_adapter.cpp:9553-9562, `directional_tick` pine_adapter.cpp:297-301) | **no** — `price_tick` is only a slippage multiplier native_execution_consumer.cpp:3671; `bar_fill_price` engine.hpp:1198 is reachable only through `NativeCurrentPriceRule::NearestTick` native_execution_consumer.cpp:3851-3852; reference helpers are dead engine.hpp:1338, engine.hpp:1348, engine.hpp:1388 | K | L8 | O:H4 O:H6 F:G4 S:F3 | **Disagree:** O kernel / F, S "stays adapter, no promotion". R5-3: own kernel lane. |
| FP5 | Trigger tested against the tick-quantized bar | **no** — the kernel tests raw prices against raw levels | K+A | L8 | O:H5 F:G4 S:F3 | R5-3: the optional quantized trigger test is K; TV's exact half-tick rule (`source_trigger_threshold` pine_adapter.cpp:328-376) is A on top. |
| PG | The Pine adapter on the kernel price grid: raw levels submitted, `project()` declaring `QuantizeFillsAndTriggers` + `HalfUp`, `source_trigger_threshold` deleted (lane R7; re-tested after L8b by gap lane N13) | **measured-infeasible for the Pine adapter** — B1 (the sub-tick cursor-print abort) is closed by L8b's `ActivationGrid`: 0 aborted runs under the trial (R7: 9 zero-offset-trail tapes + 6 POOC panels; now `test_zero_offset_trail_rides_l4c` 449/450, `test_pooc_short_close_tick_l4d` 183/183). B2 (TradingView quantizes per order kind, the grid is one run-wide rule) still moves 30 pinned checks in 4 units with no adapter-side remedy: `test_coof_market_limit_recross_l4c` 24, `test_stop_tick_rounding_l4d` 3, `test_adapter_grid_relower` 2, `test_zero_offset_trail_rides_l4c` 1; 12 more checks in 3 units have an adapter-side cause (`test_trail_fill_snap_l4c` 2, `test_native_margin_hooks` 2, `test_adapter_brackets_relower` 8 book digests). Corpus: 5/312 probes differ, in the engine-only entry-incarnation column only (every probe runs a 0.01 tick on an on-grid feed, so it cannot arbitrate) | A | waived (R7, N13) | — | **Waiver.** No class of triggers is byte-identical on its own: the grid is a run-wide switch and a per-kind mask would spell TradingView's inconsistency into the kernel (not generic, R5-3). The adapter stays on `NativePriceGrid::None` and keeps `source_trigger_threshold` and its call sites; native-engine.md "The Pine adapter's grid re-lowering is waived". **Native-only by ruling (audit lane P6): §3.6.2**, with `examples/native/native_price_grid_strategy.cpp` and its C twin as the feature's hosts. |
| FP6 | Account-currency FX | **partial** — scalar `account_fx` + immutable `NativeFxCurve` native_host.hpp:484 in batch; the stream refuses a non-empty curve native_execution_consumer.cpp:1251-1261, native-engine.md:415. **Closed by audit lane N6:** the declared curve is the run's immutable FX epoch and a confirmed-bar stream runs under it exactly as a batch does (stream twin in `tests/test_native_margin_fx_roll.cpp`); no realtime rate ingestion and no tick-driven input under a curve are stated as permanent limits with their reasons (native-engine.md, "Stream FX") | K | unassigned → N6 | F:D9 S:F4 | **Disagree:** F out of scope ("O7 clock" native-engine.md:739-744) / S its P10, a stream-safe FX epoch. No R5 lane → §4 U4 (answered by N6: S's "same curve in batch and stream", without a new clock or callback). |
| FP7 | *(quirk)* TV tick conventions: half-tick threshold pine_adapter.cpp:328-376 (used pine_adapter.cpp:7053-7058, pine_adapter.cpp:7627-7651), `source_bar_fill_tick` pine_adapter.cpp:273-286, raw-vs-booked spelling pine_adapter.cpp:9519-9625, margin-call fill pricing pine_adapter.cpp:11588-11597 | n/a — already isolated behind the terms seam (adapter `resolve_terms` pine_adapter.cpp:9310) | A | — | F:G4 O:H5 S:F3 | R5-1, R5-3 |

### 1.8 Excursion, metrics, report, hash

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| RP1 | Per-lot MFE / MAE, kernel-sampled or host-owned | **yes** — sampler native_execution_consumer.cpp:1769-1782, closing-row fold engine_orders.cpp:318-355; `owns_lot_excursions` native_host.hpp:471-475 | K+A | — | F:H1 O:I1 O:I2 S:X1 | **Disagree:** S partial + its P8 excursion-mode enum (single-source, not adopted, Appendix A.9). TV entry-bar masks are A (pine_adapter.hpp:662-669). |
| RP2 | Closed-trade list, trade statistics, report struct | **yes** — `fill_report` engine_report.cpp:39-66, `get_trade` engine.hpp:3125-3162, `compute_trade_stats` engine_metrics.cpp:72 | K | — | F:H2 O:I3 S:X2 | — |
| RP3 | Equity curve | **no** — `update_equity_extremes` / `record_equity_point` are protected engine.hpp:2386-2412 and called only at pine_strategy_host.cpp:1582-1583, pine_strategy_host.cpp:1600-1601 | K | L2 | F:H3 O:I4 S:X2 | S's "partial" folds RP2 in |
| RP4 | Equity metrics (drawdown, run-up, Sharpe / Sortino, CAGR, time in market) | **no** — `compute_equity_stats` exits early on an empty curve engine_metrics.cpp:168; engine_report.cpp:119-139 | K | L2 | F:H3 O:I5 | silent degenerate metrics, no error (O) |
| RP5 | Range-end row for a position still open at the end (`scheduler_record_range_end` pine_strategy_host.cpp:1410; the only push is pine_strategy_host.cpp:1435) | **no** — the kernel clears the vector engine_run.cpp:191 and never fills it; `report_trade_count()` includes it engine_report.cpp:75; Completed leaves lots open native-engine.md:40 | K+A | L2 | F:H4 O:I6 S:X2 | **Disagree:** S says kernel range-end rows exist (storage only). TV report shape and short-seed swaps (pine_strategy_host.cpp:1252-1284) are A. |
| RP6 | Per-trade accessors | **partial** — `get_trade` is public; the `closed_trade_*` family is protected engine.hpp:2415 | K | L2 | F:H6 | (single-source) |
| RP7 | Live order-action stream (runner ledger / webhook) | **yes** — engine_execution.cpp:691, enabled native_execution_consumer.cpp:5111, C ABI c_abi.cpp:561-578 | K | — | F:H5 | (single-source) |
| RP8 | Continuation hash | **yes** — `native_continuation_hash` native_host.hpp:505, native_execution_consumer.cpp:1037; the spec is folded by `hash_spec` native_execution_consumer.cpp:60-81 | K | — | F:I1 O:J2 S:X3 | — |
| RP9 | Broker-state hash: scalar + per-bar recording | **partial** — scalar engine_state_hash.cpp:12-114, C ABI c_abi.cpp:436; per-bar rows are appended only by the source scheduler pine_strategy_host.cpp:1605 | K | L2 | F:I2 O:J1 S:X3 | O's "yes" covers the scalar only |
| RP10 | Fold the host's own durable state into the hash | **partial** — the protected virtual `hash_source_extension` engine.hpp:467 is overridable but source-named; the kernel default folds `"source:none"` engine_state_hash.cpp:8-10 | K | L2 (the virtual rides the shared bump) | F:I3 O:J1 S:X3 | — |

### 1.9 Other runtime surface

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| OT1 | Key / value run parameters (`set_input` engine.hpp:3173, engine_run.cpp:397-435) | **partial** — staging allowed pre-run, getters protected | K (low) | — | F:J1 | (single-source) Nothing for v1: native hosts take constructor parameters. |
| OT2 | Symbol info, sessions, calendars, timezones (pine_adapter.cpp:1399-1400, pine_adapter.cpp:1440-1442) | **yes** — spec fields native_run_spec.hpp:147-157; native_calendar.hpp:174-409; intervals on every decision context market_driver.hpp:87-88 | K | — | F:J2 O:G-5 | — |
| OT3 | Indicators, series, math, matrix, map, string utils | **yes** — `ta.hpp` includes only `na/series/window_sum` ta.hpp:2-8 | K | L11 (naming only) | F:J3 | (single-source row) TV-calibrated numerics → §2.ii b |
| OT4 | Cooperative abort | **yes** — `request_abort` engine.hpp:3591, `NativeAbortReporting` native_run_spec.hpp:33-36 | K | — | F:J4 | (single-source) |
| OT5 | C-level run configuration, stream, FX curve, contract probe | **yes** — `strategy_configure_native_v1` c_abi.cpp:794, FX curve c_abi.cpp:848, probe c_abi.cpp:785, stream c_abi.cpp:522-641 | K | — | O:J4 | The C spec omits `intrabar`, `path_order`, `abort_reporting` (pineforge.h:450-461 vs native_run_spec.hpp:134-176) (F §2.iii) |
| OT6 | C-level order submission + strategy callbacks | **no** — none of the 57 `PF_API` symbols; listed as a refusal native-engine.md:420 | K | L13 | F:J6 O:J5 | R5-7: post-v1. S covers it in its §2.3 without a row. |
| OT7 | *(quirk)* Pine language state: series, barstate / session flags, position-view freezing (pine_language_state.hpp:12-68; pine_scheduler_native.cpp:195-230, pine_scheduler_native.cpp:484-586) | n/a | A | — | S:R1 | (single-source) |
| OT8 | *(quirk)* Live-tail / probe-suppress harness flags (pine_strategy_host.cpp:270-272) | n/a — **contained since N14** (the flags were `BacktestEngine` members until then; now `source::PineStrategyHost` state behind two kernel virtual seams, §2.ii q) | A | N14 | F:J7 S:C5 | parity-campaign tooling |

---

## 2. Structural work

### 2.i Kernel-only build target (L1)

- **Today:** one static library always compiles the source layer (list CMakeLists.txt:81-94, `add_library(pineforge STATIC` CMakeLists.txt:96, splice CMakeLists.txt:133) and installs every header, `include/pineforge/source/**` and `include/pineforge/compat/pine/**` included (CMakeLists.txt:307-310). `src/compat/pine/market_admission.cpp` sits in the *main* list (CMakeLists.txt:111) for historical reasons (added by `3b13183`, before the source-layer list existed — F).
- **Link feasibility:** no kernel TU and no public kernel header includes `source/` or `compat/pine/` (F, O). The only code coupling is the forward-declared opaque `source::StrategyOverrides*` (§2.ii a), which needs no definition.
- **Risk:** the archive member order of `libpineforge.a` changes. Harmless for static linking, but check `PINEFORGE_REQUIRE_ABI_RECEIPTS` (CMakeLists.txt:58); what the receipts pin is unverified (F).

| Aspect | F | O | S | Merged (L1) |
|---|---|---|---|---|
| Option | `PINEFORGE_BUILD_SOURCE_LAYER`, default ON | same | `PINEFORGE_BUILD_KERNEL_ONLY` | `PINEFORGE_BUILD_SOURCE_LAYER` (default ON; OFF = kernel only): 2 of 3, and it names the optional part |
| Targets | object libs `pineforge_kernel_objs` + `pineforge_source_objs`; `pineforge_kernel` STATIC, alias `PineForge::kernel`; `pineforge` STATIC = both | `pineforge_kernel` OBJECT; `pineforge` STATIC = kernel objects + conditional source list | `pineforge_kernel` + `pineforge` as the compatibility aggregate | F's shape. `pineforge` keeps its name, alias and install rules, so codegen-built strategies and `find_package(PineForge)` consumers do not move |
| What moves | `compat/pine/market_admission.cpp` → source list | same + delete dead `admission_retention` | same ("a concrete boundary bug") | Move it (unanimous). Its callers are the adapter only (pine_adapter.cpp:1816, pine_adapter.cpp:12674-12676, pine_adapter.cpp:12869-12870, pine_adapter.cpp:13496-13497) and tests, so the move is link-neutral. Delete `admission_retention` (src/compat/pine/market_admission.cpp:153, declared include/pineforge/compat/pine/market_admission.hpp:20; no caller anywhere — O, single-source) |
| Installed headers | OFF skips `include/pineforge/source`, `include/pineforge/compat` | install conditional on the option | installed closure with source + compat headers physically removed | Conditional install; S's closure test is the acceptance |
| CI / gate | link the two native examples + the 20 source-free native tests against `pineforge_kernel` only; `nm` assertion in the independence checker (it already forbids `pineforge::source` / `compat::pine` symbols check_native_include_independence.py:40-46) | new CI lane: configure with the source layer OFF, build, run the checker against that prefix | clean kernel-only compile; `nm` shows no source / compat symbol; keep the textual ban (test_native_source_guard.py:18-60) as a fast preflight | All three, one CI lane |
| Size | M | M | L | M (R5-11) |

### 2.ii Coupling extraction (L11 neutral, L12 hash-visible) — R5-8

| # | Item | Where / what it is used for | Class | Plan | Src |
|---|---|---|---|---|---|
| a | `source::StrategyOverrides` opaque pointer | Forward decls execution_consumer.hpp:15, engine.hpp:407-409; parameter of `run_rich` (execution_consumer.hpp:47-56, native_execution_consumer.hpp:33-42) and of the rich `BacktestEngine::run` overload (engine.hpp:3115-3120, engine_consumer.cpp:81-93). The kernel only forwards it as `NativeBeginArgs::overrides_opaque` native_host.hpp:387; the source host casts it back pine_strategy_host.cpp:274-277 | NEUTRAL in behaviour; a consumer-vtable signature change, so it rides the shared `engine_script_run` bump | `const void*` in the kernel signatures (F `host_begin_extras`, O `overrides_opaque`), typed convenience overload in `source/pine_strategy_host.hpp`; then drop the checker's single whitelist entry check_native_include_independence.py:42-46. S's alternative (`NativeRunOverrides`, or removing the rich overload) not adopted. Unverified: generated code calls the typed overload (codegen repo is not in this tree — F) | F O S |
| b | `pine_float_compare.hpp` | 1e-10 absolute equality band, used by kernel TA only: ta_misc.cpp:28 (percentrank), ta_oscillators.cpp:462-463 (MFI), ta_volatility_trend.cpp:356-359 (DMI); no execution TU includes it (O) | NEUTRAL (rename only) | Rename to a neutral header (F `float_band_compare.hpp`, O `ta_compare_band.hpp`), keep `pine_float_*` inline aliases in a source-layer header for generated code. Numerics must not move. S's behavioural `ComparePolicy` split is not neutral → not adopted (§4 U6). **Status (L11 + N14): done** — `ta_compare_band.hpp` is the kernel header; the alias shim is `include/pineforge/source/pine_float_compare.hpp` (N14 moved it off the kernel include root; it has no in-tree includer, generated code emits its own comparator) | F O S |
| c | `NativeSlotLabelPolicy::LegacyTolerant`, `NativeLegacyTolerance` | native_run_spec.hpp:49-74; read by market_driver.cpp:41-54, native_execution_consumer.cpp:1190, native_execution_consumer.cpp:4777, native_execution_consumer.cpp:5098-5100; set only by the adapter pine_adapter.cpp:1453-1460. A feed-tolerance policy (raw strictly-increasing labels, zero / NaN-volume legacy bars), not TV semantics (O) | NEUTRAL | Rename with deprecated aliases (F `RawIncreasing` + `NativeInputTolerance::{StructuralBars, WarmupNonNegativeOHLC}`; O `NativeLabelPolicy::ProviderLabels`). Enumerator values stay 0/1 and 1/2: they are hashed native_execution_consumer.cpp:64-65. The C transport does not carry them pineforge.h:450-461 | F O S |
| d | Orphaned comments | KI-62 block engine_orders.cpp:198-208 (*fresh*: comment-only, the bodies are deleted; S read it as live code) and engine.hpp:2654-2658; wider deleted-declaration blocks engine.hpp:498-560, engine.hpp:2654-2700; the money-rule essay engine.hpp:73-180 (the code is pine_adapter.cpp:396-403) | NEUTRAL | Delete, or move to `docs/` next to the adapter code that implements them | F O |
| e | Dead helpers in the public kernel header | `round_to_mintick_directional` engine.hpp:1338, `apply_slippage` engine.hpp:1348, `apply_limit_fill` engine.hpp:1388, `reserve_percent_commission` engine.hpp:1522 have no callers in `src/` (`round_to_mintick` engine.hpp:1141 and `bar_fill_price` engine.hpp:1198 are live via native_execution_consumer.cpp:3852) | NEUTRAL | Reuse as the reference arithmetic of L8 / L3, delete the rest | O (single-source) |
| f | Dead legacy stream path | `stream_feed_input_bar` engine_stream.cpp:16-74 has no caller; it is the only caller of `dispatch_source_stream_script_bar` (engine.hpp:2987; the kernel default throws engine_consumer.cpp:138-140) | NEUTRAL (removing the virtual changes the vtable → shared bump) | Delete both. Keep `stream_state_hash` engine_stream.cpp:144 (pinned by check_broker_state_hash_coverage.py:204); decl / def pairing is enforced by test_native_source_guard.py:258-267 | O; F asked (its Q6) |
| g | Legacy path resolver | `src/engine_path_resolve.cpp` (1051 lines): `resolve_exit_path_fill`, `compute_exit_trail_state`, `tick_quantized_price`, `collect_cross_events` … have no production caller outside the file except the adapter's `try_exit_open_gap_fill` / `entry_stop_first_touch` and `first_touch_position`; 13 tests call `resolve_exit_path_fill`. The kernel needs only `bar_path_uses_high_first` and `set_path_order_override` | NEUTRAL (pure move; verify with the L1 link gate) | Keep the two generic functions in a small kernel TU, move the rest to `src/source/` (tests link the full library) | F (single-source) |
| h | `pine_*`-named generic API | session_time.hpp:34-195 (live, e.g. engine_run.cpp:372), `str_utils.hpp`, math.hpp:9, timezone.hpp:8-30; the guard bans `pine_*` only inside `IDENTIFIER_ROOTS` (test_native_source_guard.py:52, test_native_source_guard.py:35-43) | NEUTRAL, M | Neutral aliases; `pine_*` kept as deprecated inlines for codegen. Large blast radius in generated code. **Status (L11 + N14): done** — session_time / str_utils / math / timezone in L11; N14 applied the same treatment to the two public matrix types: `PineMatrix` → `NumericMatrix`, `PineGenericMatrix<T>` → `GenericMatrix<T>`, old spellings kept as exact aliases (no version script pins either name) | O (single-source) |
| i | `pf_pending_order_v1_t` Pine-named fields | pending_order_mirror.hpp:73, pending_order_mirror.hpp:135, pending_order_mirror.hpp:147-156, pending_order_mirror.hpp:164-169; enumerated by pending_order_mirror.cpp:140-174; written only by pine_adapter.cpp:16317-16346 | no change | Freeze (ABI). The neutral view arrives beside it (L7 / L13) | O (single-source) |
| j | Pine-shaped lot flags | `PyramidEntry::market_pyramid_add` engine.hpp:247 (written only pine_strategy_host.cpp:500, read pine_adapter.cpp:15533, hashed engine_state_hash.cpp:57); `ordinary_market_open`, `pooc_terminal_market_entry`, `ordinary_stop_open`, `bracket_slot_shadowed` engine.hpp:242-283 have no writer in `src/` or `include/` yet are hashed (F); `entry_path_position` engine.hpp:253 is never written, hashed engine_state_hash.cpp:58 (O) | HASH-VISIBLE | Move the live flag to an adapter side table keyed by `entry_incarnation` (the adapter already keeps `PlacementTable` pine_adapter.hpp:297-476) and fold it through `hash_source_extension`; delete the dead ones; update the hash-coverage waivers. Whether a kernel TU beyond the hash reads them is unverified (O) | F O S |
| k | `margin_call_enabled_` | engine.hpp:606, setter engine.hpp:3294-3298; no kernel reader; forwarded to the adapter pine_strategy_host.cpp:306 | HASH-VISIBLE (R5-8) | After L4 the kernel toggle is the presence of `spec.margin`. Move the flag and its C setter semantics into the source host; the C symbol stays (pinned runtime list) | F O |
| l | Kernel decodes adapter strings | `closed_trade_close_cause` engine_trade_accessors.cpp:155-167 branches on `"__margin_call__"` and two `"Close Position (…)"` prefixes that only `src/source/**` writes; the stale comment engine_trade_accessors.cpp:145-149 cites the deleted `engine_risk.cpp`; it implements a public C contract pineforge.h:1051, pineforge.h:1077-1065 | HASH-VISIBLE (adds durable state) | Kernel `enum class CloseCause` recorded on the `Trade` at settlement (the kernel sets `MarginCall` when L4 liquidates; a source virtual covers adapter causes). C numbers unchanged. After L4 | O (single-source) |
| m | `request.security` machinery | engine_security.cpp:22, engine_aux_security.cpp:89-140, `SecurityEvalState` engine.hpp:1974-2178 carry Pine semantics (lookahead, gaps, Heikin-Ashi, KI-55 range start) and are driven only by the source layer | HASH-VISIBLE / measured path | F planned to move both TUs and the state into the source host after native HTF. **R5-6 narrows this:** L6 reuses the feed store and routing, so those stay kernel; only the Pine-only evaluator semantics remain candidates. Do last. **Status (lane L12b): done** — the evaluator semantics are `source::PineSecurityEvalState` + `src/source/pine_security_eval.cpp`; the kernel keeps the registry, aggregator, feed store and one generic step | F S |
| n | S-only proposals, not adopted | A generic `FillModel` interface with a TV model installed by the adapter (for the TV money / tick / path comments and helpers engine.hpp:1125-1348, engine.hpp:1514-1621); "emit generic FIFO facts and let an adapter policy produce the KI-62 scratch" | — | Superseded by R5-3 (price-grid lane) and R5-1 (TV fill specifics stay behind `resolve_execution_terms`); the dead-helper part is item e; there is no scratch code left to re-lower (item d) | S |
| o | `pineforge::admission` (market-admission journal) — **N14 addition** | `include/pineforge/market_admission.hpp` (275 lines, installed by a kernel-only build) + `src/market_admission.cpp` (kernel TU): the observation journal of TradingView admission reviews; every `Configuration` field is a `strategy()` declaration parameter. No kernel TU consumed it (engine.hpp included it and used nothing; the hash helper in `src/broker_state_hash_internal.hpp` had one caller, `src/source/pine_state_hash.cpp`) | NEUTRAL (pure move; the fold is unchanged, so no hash domain moves) | **Status (N14): done** — moved whole to `include/pineforge/source/market_admission.hpp` + `src/source/market_admission.cpp` (`PINEFORGE_SOURCE_LAYER_SOURCES`); `hash_admission_field` moved verbatim into the source hash TU; `check_market_admission_schema.py` re-pointed; `MarketAdmissionDraft` / `MarketAdmissionJournal` spellings and the `market_admission_v2` inline namespace unchanged. ADR-0001 no longer calls it a generic stem | audit (Opus N14) |
| p | TradingView trail tick arithmetic — **N14 addition** | `src/engine_internal.hpp`: `kTrailPointsCeilEps` (5e-5 tolerant ceil of `trail_points`), `trail_points_to_ticks`, `trail_offset_to_ticks`, `snap_trail_level_to_tick_grid`; every value a `lab tv` calibration; readers only in `src/source/` and `src/compat/pine/` (the kernel's trail is `TrailTicks`, L7) | NEUTRAL (bodies byte-identical) | **Status (N14): done** — moved verbatim to `include/pineforge/compat/pine/trail_ticks.hpp` (`pineforge::compat::pine`); nine call sites re-qualified; `test_trail_fill_snap_l4c` includes the compat header (CHECK texts unchanged, twin parity OK) and is therefore a source-layer row | audit (Opus N14) |
| q | OT8 live-tail / probe-suppress overrides — **N14 addition** | `BacktestEngine::realtime_tail_`, `realtime_tail_horizon_bars_`, `probe_suppress_tail_logic_` with public kernel setters and the horizon walk `apply_realtime_tail_horizon` (engine_run.cpp); every reader in `src/source/`; two frozen `PF_API` setters in `c_abi.cpp` | HASH-WAIVED (configuration before and after; no domain moves) | **Status (N14): done** — as 2.ii k: state, setters, readers and the walk move verbatim to `source::PineStrategyHost`; the C exports stay in `c_abi.cpp` and reach the host through two kernel virtual seams (`virtual bool set_realtime_tail`, `virtual bool set_probe_suppress_tail_logic`) whose kernel defaults are accepted-and-inert (answer `false`, no error: the L1 pre-begin ingress contract `test_native_example_batch` pins on a native module); v16→v18 relocation manifest rows added, epoch v18 still open, no bump; kernel-only pin `test_native_tail_override_seam` | audit (Opus N14) |
| r | Lower-timeframe merge-flag rule — **N14 addition** | `internal::ensure_supported_lower_tf_emulation_flags` (engine_lower_tf.cpp) threw "request.security lower TF emulation only supports lookahead=barmerge.lookahead_off and gaps=barmerge.gaps_off" from a kernel TU — the one executable Pine/`barmerge` string in `libpineforge_kernel.a` | NEUTRAL (message and refusal unchanged) | **Status (N14): done** — the predicate is a file-local helper in `src/source/pine_security_eval.cpp`; the kernel TU keeps the generic primitives (fixed intraday TF parser, input : requested ratio, evenly sampled sub-bars), which have kernel-only tests (`test_lower_tf_parse_extra`, `test_lower_tf_seconds_suffix`) and source-only production callers — retained as a generic capability | audit (Opus N14, Codex N4) |
| s | Authoritative-feed period partition (engine_aux_security.cpp) — **N14 ruling** | "W"/"M" from the installed daily feed; feed stamps as the period partition; trade date = session-day of the period's last chart bar (§2.iv item 6, §4 E8). A bare host that installs `authoritative_bars` inherits these | — (documented contract, no gate) | **Ruled (N14): retained as the generic contract.** The three rules follow from "a feed is the venue's own bars of one timeframe" and from nothing platform-specific; the policy knob is the feed itself (install none → plain aggregation of the input; the doc says so, `native-engine.md` "Authoritative bars"). A separate partition-policy field would be hash-visible spec surface for a choice the host already makes by installing or not installing the feed. The TradingView pins stay as the calibration evidence. Listed in ADR-0001's residual table | audit (Opus R-7, Codex "TV-shaped auxiliary-feed policy") |
| t | Early-close completion keyed by instrument class (engine_security.cpp `session_template_knows_early_close`) — **N14 ruling** | Branches on `SymInfo::type` ∈ {forex, cfd, crypto} (continuous-session OTC classes never complete a calendar period at an early close; the OANDA pins in timeframe.hpp) | — (documented contract) | **Ruled (N14): retained.** `SymInfo::type`'s vocabulary is fixed by the frozen C ABI (`strategy_set_syminfo_type`), so classifying by it is the kernel's own market-structure vocabulary, not a source-language one; the adapter's routed sites (R3b) and the bare host's subscriptions share the rule, which keeps the corpus byte-identical by construction. Stated on the function and in ADR-0001's residual table | audit (Opus R-8, design §4 E8) |

### 2.iii C-level order submission and callbacks (L13, post-v1 — R5-7)

Today a C host can configure (`strategy_configure_native_v1` c_abi.cpp:794-846), stream (`strategy_stream_*` c_abi.cpp:522-631) and read results, but strategy logic is C++-only and the batch entry points (`strategy_create`, `run_backtest*`) are per-strategy symbols a native author hand-writes (native_market_strategy.cpp:152-218). Design retained from F (most complete), with O's event polling and S's hardening rules:

```c
typedef struct pf_native_decision_v1 { uint64_t ordinal; int32_t interval_index; int64_t effective_time_ms,
    script_bar_open_ms, sub_bar_open_ms; uint8_t provenance, path_phase; double price; } pf_native_decision_v1;
typedef struct pf_native_request_v1 { uint32_t struct_size;
    uint32_t intent;  double intent_value; uint32_t side;          /* Flatten/Reduce/Transact/ReverseTo/Sized */
    uint32_t trigger; double p1, p2; uint8_t fill_through;         /* Market/Limit/Stop/StopLimit/Trail */
    uint32_t capacity; double capacity_units;
    uint32_t owner; const uint64_t* owner_incarnations; uint32_t owner_n; int64_t owner_cycle; uint64_t cohort;
    uint64_t group; int64_t group_cohort; uint32_t group_effect;
    const char* label; const char* comment; } pf_native_request_v1;
typedef struct pf_native_applied_v1 { uint64_t ordinal, incarnation; double raw_price, resolved_price,
    closed_units, opened_units, ticket; int64_t cycle_before, cycle_after; uint8_t terminal; } pf_native_applied_v1;
typedef struct pf_native_callbacks_v1 { uint32_t struct_size; void* user;
    int (*on_run_begin)(void*);
    int (*on_input)(void*, const pf_bar_t*);                                          /* S */
    int (*on_bar_open)(void*, const pf_bar_t*, const pf_native_decision_v1*);
    int (*on_bar)(void*, const pf_bar_t*, const pf_native_decision_v1*);
    int (*on_tick)(void*, const pf_bar_t*, const pf_native_decision_v1*);
    int (*on_applied)(void*, const pf_native_applied_v1*, const pf_native_decision_v1*); } pf_native_callbacks_v1;

PF_API pf_strategy_t pf_native_host_create_v1(const pf_native_callbacks_v1*);
PF_API void          pf_native_host_free(pf_strategy_t);
PF_API int pf_native_run_v1(pf_strategy_t, const pf_bar_t*, int n, pf_report_t* out);      /* batch */
PF_API int pf_native_submit_v1(pf_strategy_t, const pf_native_request_v1*, uint64_t* incarnation, uint32_t* reject);
PF_API int pf_native_replace_v1(pf_strategy_t, uint64_t incarnation, const pf_native_request_v1*, uint64_t* successor);
PF_API int pf_native_cancel_v1(pf_strategy_t, uint64_t incarnation);
PF_API int pf_native_execute_current_v1(pf_strategy_t, uint64_t incarnation, uint32_t price_rule);
PF_API int pf_native_position_v1(pf_strategy_t, double* signed_units, double* average_price, uint64_t* lots);
PF_API int pf_native_working_len_v1(pf_strategy_t);  /* + _get_v1: copy-out, mirrors L7's working view */
PF_API int pf_native_events_v1(pf_strategy_t, uint64_t after_ordinal, pf_native_event_v1* out, int cap);   /* O, S */
```

| Point | Decision | Src |
|---|---|---|
| Implementation | One new kernel TU (F `src/native_c_host.cpp`, O `src/c_native_host.cpp`, S `native_c_api.cpp`): `class CCallbackHost final : public NativeStrategyHost` forwarding each virtual to the table. Streaming reuses the existing `strategy_stream_*` symbols unchanged (they take any `BacktestEngine*`) | F O S |
| Errors | A non-zero callback return maps to `NativeFailureCode::CallbackException`; C callbacks must not unwind. The C++ path already latches exceptions native_execution_consumer.cpp:2729-2739 | F O |
| Legality | The existing rule (`commands_allowed` native_execution_consumer.cpp:956-963): inside a callback, or between realtime inputs | F |
| Hardening | Tagged, size-prefixed PODs; `struct_size` + version; unknown-tag refusal; never cast the C request to a C++ variant; a C fixture compiles against the frozen previous header. S puts the API in a separate header `include/pineforge/native_c_api.h` | S |
| SOP | Every symbol goes into `src/c_abi.cpp`, `pineforge.h`, `EXPECTED_RUNTIME` + both counts (check_c_abi_runtime.py:19-79), the README table and the Python harnesses, or all CI matrix jobs fail at the "C ABI runtime source check" step (`CLAUDE.md`) | F O S |
| Not in v1 of the C API | `resolve_execution_terms` / `validate_execution_precommit` are the adapter's seam; a C host that needs sizing uses `Sized` (F). O offers an optional `resolve_terms` callback: deferred | F O |
| Open | Naming: F and O use `pf_native_*`; S uses `strategy_native_*`, which matches the existing runtime symbols. The C spec also omits `intrabar`, `path_order`, `abort_reporting` today (pineforge.h:450-461 vs native_run_spec.hpp:134-176) | F S |

Depends on L7 (working view), L3 (so `Sized` is in v1 of the C request) and the epoch batch.

### 2.iv Native HTF for bare hosts (L6) — chosen design per R5-6

| | F (its §2.iv A) | O (its G10) | S (its P7) | Chosen |
|---|---|---|---|---|
| API shape | spec `subscriptions` + push callback `on_native_timeframe_bar` | `register_native_series` + pull `native_series_bar(id)` / `native_series_slot_is_new(id)` | spec `security_requests` + `configure_native_security_feed` + `native_security(id)` view | F's subscription in the spec + push callback, plus O's pull accessor |
| Engine reuse | none: generalize the consumer's one script bucket (`ScriptBucket` native_execution_consumer.hpp:153-167, `contribute_input` native_execution_consumer.cpp:4697, `seal_script` native_execution_consumer.cpp:4674) to N; rejects exposing the `request.security` path as Pine-shaped end to end | promote the existing evaluator + feed machinery; edits `engine_security.cpp`, `engine_aux_security.cpp`, `engine_lower_tf.cpp` | same TUs + `native_calendar.cpp` | The existing feed machinery, **called, not edited**; `engine_security.cpp` untouched |
| Pump | consumer seal logic | accepted-input path (`invoke_input_callback` native_execution_consumer.cpp:4891), only for natively registered series | copy at Ready, evaluators created before the first input | O's registration-gated pump + S's copy-at-Ready |
| Authoritative bars | `authoritative_bars` on the subscription | `Source::ExchangeFeed` through `set_native_security_feed` | `configure_native_security_feed(id, bars, tf)` | F's field, installed through the existing store |
| Lookahead | none, by construction | `lookahead = false` default, available | `lookahead_on = false`, `gaps_on = true` | open → §4 U1 |

1. **Spec:** `std::vector<NativeTimeframeSubscription> subscriptions` (Appendix A.5), hashed only when non-empty (digest like `native_intrabar_path_digest`).
2. **Wiring**, all inside the native consumer at Ready → begin and only when `subscriptions` is non-empty (zero work for the adapter): register one evaluator state per subscription through the existing `register_security_eval` (engine.hpp:2284-2286, *fresh*); install `authoritative_bars` into the existing store (`set_native_security_feed` engine.hpp:3061, engine_aux_security.cpp:89-140); call `prepare_native_security_feeds` (engine.hpp:2897, defined engine_aux_security.cpp:141, *fresh*) — the "made native-callable" step: reachable from the native consumer, still refused from host code in-run; pump `feed_security_eval_state` (engine.hpp:2332-2334, *fresh*) from the accepted-input path; deliver each completed bucket through `on_native_timeframe_bar`.
3. **Untouched:** `engine_security.cpp`, the Pine scheduler sequence pine_strategy_host.cpp:1316-1400 and its call order. The per-run evaluator init is today a *source-layer* member (`PineStrategyHost::init_security_eval_states_for_run` pine_scheduler.cpp:9-39, declared pine_strategy_host.hpp:324, *fresh*), so L6 adds a kernel-side twin instead of moving it.
4. **Delivery rule (F):** a subscription bar is delivered when its last contributing input bar is accepted, immediately before that input's script calculation, never earlier — and never before an earlier script interval's lazily sealed calculation has run: the pump is ordered against the script interval, `seal(k) → deliver(i+1) → calc(i+1)` (L6d).
5. **Interim, documented in L0:** `on_native_input` + `TimeframeAggregator` (HT6). Doc notes (O): `set_native_security_feed` is public but inert without evaluators and throws in-run (engine_aux_security.cpp:91 → engine_consumer.cpp:42-52 → native_execution_consumer.cpp:991-1007); `set_aux_security_feed` returns false in the kernel engine_consumer.cpp:143-145.
6. **Entanglement to put in the lane brief** (*fresh* read of engine_aux_security.cpp:48-87): the feed machinery is TV-calibrated. "W" / "M" buckets are built from the installed native daily bars, and the native stamps define the period partition (a holiday session folds into the next trade date's bar). A bare host that supplies `authoritative_bars` inherits those rules. Document them as the contract or gate them behind a partition policy — decide in the lane, not silently (§4 E8).
7. **HT3 / HT4:** lower-TF arrays come from the L5 sub-bar hook in v1. An auxiliary feed is a subscription whose bars come from the host; the Pine chart-slice mapping is not exposed. Whether v1 accepts a feed *finer* than the input is left to the lane brief (S wants it, F calls it a TV harness construct).
   **Status (audit lane N7): HT4 done for a bare host; the adapter's auxiliary path retained, measured.** `NativeRunSpec::auxiliary_feed` (`NativeAuxiliaryFeed{tf, bars}`, strictly finer than `input_tf`, opt-in and hashed only when present) plus `NativeTimeframeSubscription::source = NativeSeriesSource::AuxiliaryFeed` build a series from the host's finer bars through the existing evaluator step (`register_security_eval` against the feed's timeframe, `feed_security_eval_state` per feed bar — called, not edited); routing is by time alone (a feed bar rides on the first accepted input whose period it opened before), with `declare_auxiliary_feed` at begin, `append_auxiliary_bars` on a realtime stream and a C spelling (`PF_NATIVE_SPEC_EXT_AUXILIARY_FEED` tail, `strategy_native_append_auxiliary_bars_v1`). No epoch moved: `native_run_spec_v3` and `engine_script_run_v18` are open (no release tag contains them, no frozen provider fixture of either exists), exactly as L6c's `gaps` and begin-time hook were added. `BacktestEngine::set_aux_security_feed` stays the source host's door and still answers a silent false on a bare host (the pre-begin setter pins, `tests/test_native_example_batch.cpp` among them, hold it to that). **Retained, why:** the adapter does not re-lower its auxiliary sites, on three measurements — 0 of 312 corpus probes install an auxiliary feed, so corpus byte-identity cannot witness the move; TradingView's chart slice leaves pre-range feed coverage inert where the kernel folds it by time (two hourly buckets against three over one feed, `tests/test_native_auxiliary_feed_twin.cpp` row B); and the adapter evaluates a chart bar's slice at its calculation, after that bar's matching pass, where the kernel delivers before it (position 1 against 0 at the same bucket, row C), which no predicate over an opaque generated `evaluate_security` can prove neutral. The congruent shape is the same series on both (row A) and can move once a population exercises it. Full contract: `docs/pages/native-engine.md`, "The auxiliary finer feed".

### 2.v Promote the examples (L10)

- Move `runner/examples/native_market_strategy.cpp` and `native_selected_strategy.cpp` to a top-level `examples/native/`, built by `PINEFORGE_BUILD_EXAMPLES` (today "none yet" and guarding nothing CMakeLists.txt:37; the examples build only under `PINEFORGE_BUILD_LIVE_RUNNER` CMakeLists.txt:341-342, runner/CMakeLists.txt:37-45). Link the kernel target (S), or `PineForge::pineforge` during migration (O) — no SQLite / curl / OpenSSL (runner/CMakeLists.txt:2-5).
- Build (1) standalone executables with a `main()` that runs batch + stream on embedded bars (the guide already contains that `main` native-engine.md:522-589) and (2) the same MODULE targets the runner tests load; keep the C-ABI shims so the live runner can still `dlopen` them (F, O, S).
- Update the three places that pin the paths: runner/CMakeLists.txt:37-45, check_native_include_independence.py:36-39, the tests at runner/CMakeLists.txt:98-128.
- Add a minimal "hello, kernel" host (~60 lines, no C ABI — O) and one example per v1 feature lane as it lands; each doubles as that lane's twin host (F).
- Add `include/pineforge/native_module.hpp` with `PINEFORGE_EXPORT_NATIVE_STRATEGY(Class)` to replace the ~70 hand-written `extern "C"` lines per example (native_market_strategy.cpp:152-218) (F, single-source).
- Ship the native API reference and the standard dual-run harness of §3.1 b (S lane J).

### 2.vi ADR-0001 correction list (input for rewriting PR #255) and `native-engine.md` fixes (L0)

Union of F §2.vi (9), O §2.vi (5 + 1) and S §2.6 (7), deduplicated. S had no ADR file in its checkout and wrote "what an ADR in this tree must say"; rows 11-12 come from that.

| # | ADR claim | Correct statement | Evidence | Src |
|---|---|---|---|---|
| 1 | Orders are "Market / Limit / Stop" (ADR L72-75) | Also StopLimit and Trail; five intents, five owner kinds, groups, `PointBudget` | native_order.hpp:88-96, native_order.hpp:75, native_order.hpp:104-119, native_order.hpp:121-128, native_order.hpp:99-102 | F |
| 2 | "The kernel matches and fills … with sizing, slippage, fees, margin and settlement" (ADR L74-75) | "with slippage, fees, opening admission and settlement; sizing is resolved by the host". `NativeRunSpec` owns fees, FX, close timing, direction / caps and intrabar paths; it does not own percent / cash sizing, maintenance margin, calc-on-fill, every-tick calculation or bare-host security registration | native_execution_consumer.cpp:2753-2755, native_run_spec.hpp:19-176, native_run_spec.hpp:173-174 | O S |
| 3 | "Read results via `metrics.hpp`" (ADR L76-80) | Trade stats yes. Equity curve and equity metrics are empty for a bare host, silently, and an open final position is missing from the report | engine_metrics.cpp:168, engine_report.cpp:119-139, engine.hpp:2386-2412, pine_strategy_host.cpp:1582-1583, engine_run.cpp:191 | F O |
| 4 | HTF "comes from the kernel feed `set_native_security_feed` / `prepare_native_security_feeds`" (ADR L84-89) | Not usable by a bare host: a partial boundary, not an absent kernel. `prepare_native_security_feeds` is protected with one caller; `configure_security_evaluators` has one caller; the public setter is inert pre-run and refused in-run | engine.hpp:2897, pine_strategy_host.cpp:1325, engine.hpp:2346, pine_strategy_host.cpp:1305, native_execution_consumer.cpp:991-1007 | F O S |
| 5 | "`engine_aux_security.cpp` includes `source/pine_strategy_host.hpp`" (ADR L129-132) | False at `73817c1`: its includes are `engine_internal.hpp`, `ta.hpp` and std headers (*fresh* re-read). No kernel TU includes a source header | engine_aux_security.cpp:5-12 | F |
| 6 | "Kernel TUs reference adapter types (`source::StrategyOverrides` …)" (ADR L129-131) | True only as a forward-declared opaque pointer the kernel never dereferences; the independence checker whitelists exactly that symbol | engine.hpp:407-409, execution_consumer.hpp:15, native_host.hpp:387, check_native_include_independence.py:42-46 | F O |
| 7 | `engine.hpp` hosts "the ten-significant-digit money rule, KI-62, the `strategy.close` batching + entry-id ledger, a TradingView margin-call toggle" (ADR L131-134) | Two of four survive as code. Money rule: a comment block; the arithmetic is in the adapter. KI-62: orphaned comments + one live, hashed lot flag. Close batching + id ledger: gone from the kernel (the ledger is in the adapter; the old names survive only in comments). Live: `margin_call_enabled_`, the KI-62 flag and — unmentioned by the ADR — string-sentinel decoding of adapter comments | engine.hpp:73-180, pine_adapter.cpp:396-403, pine_policy_support.hpp:9-15; engine_orders.cpp:198-208, engine.hpp:2654-2658, engine.hpp:247, engine_state_hash.cpp:57, pine_strategy_host.cpp:500; pine_adapter.hpp:1247-1258, engine.hpp:3486, engine.hpp:2803; engine.hpp:606, engine.hpp:3294-3298; engine_trade_accessors.cpp:155-167 | F O |
| 8 | "No kernel-level C API. `c_abi.cpp` exports only version/descriptor symbols" (ADR L136-138) | 57 runtime `PF_API` symbols: native configure, the 12-symbol stream family, pending-order views, broker-state hash, security feeds, FX curve, reports. Accurate statement: no C symbol submits / replaces / cancels an order and no C strategy callback exists, so strategy logic is C++-only | c_abi.cpp:221-848, check_c_abi_runtime.py:19-79, c_abi.cpp:794, c_abi.cpp:522-641, c_abi.cpp:447-499, c_abi.cpp:426-443, c_abi.cpp:734-765, c_abi.cpp:848 | F O S |
| 9 | "No examples. `PINEFORGE_BUILD_EXAMPLES` is 'none yet'" (ADR L139) | The option is inert, but two Pine-free `NativeStrategyHost` examples exist, are compiled by the independence checker and tested through the live runner, only under `PINEFORGE_BUILD_LIVE_RUNNER`. Wording: "no top-level examples target" | CMakeLists.txt:37, check_native_include_independence.py:36-39, runner/CMakeLists.txt:37-45, CMakeLists.txt:341-342 | F O S |
| 10 | Follow-ups (1)-(4) (ADR L168-170) | Add native reporting, sizing, margin, HTF — and, per R5, calc-timing, price grid and order ergonomics. Without them "usable without Pine" is a build-system claim, not a feature claim | §3.5 | F |
| 11 | Build layout | One `pineforge` library always appends the source list; a kernel-only target does not exist yet; `compat/pine/market_admission.cpp` sits in the main list, a boundary bug | CMakeLists.txt:81-134, CMakeLists.txt:111 | S (F, O agree in §2.i) |
| 12 | Host constructors | `NativeStrategyHost` is a zero-argument C++ host with the callbacks and the request API; `PineStrategyHost` is the class that takes `CapAttachment` — do not attribute it to the native constructor | native_host.hpp:425-505, pine_strategy_host.hpp:21-25 | S (single-source) |

`docs/pages/native-engine.md` fixes for L0 (F unless noted):

| Where | Says | Tree says |
|---|---|---|
| native-engine.md:5-9, native-engine.md:374, native-engine.md:413 | callbacks are close-only | The kernel has bar-open, tick and post-fill hooks (native_host.hpp:446, native_host.hpp:443, native_host.hpp:452). Document `on_native_applied` as the calculate-on-fill hook, with suffix eligibility; the FIFO notification rule is already at native-engine.md:240-247 |
| native-engine.md:153 | `native_order_v4` | `native_order_v5` native_order.hpp:25 |
| native-engine.md:169, native-engine.md:602 | host epoch v16 | v17 native_host.hpp:18 |
| native-engine.md:737-754 | `BacktestEngine` still holds `CapAttachment` / `OrderPriority` / `IntradayCap` | `engine.hpp` has zero references to them (F grep) |
| native-engine.md:129-132 | the rich `run` overload fails a native host | `run_rich` native_execution_consumer.cpp:4990-5028 shows no such refusal (unverified by test) |
| add | — | interim HTF recipe (HT6); open-bar lookahead warning (CT4); `set_native_security_feed` is inert pre-run and throws in-run (§2.iv 5, O) |

---

## 3. Roadmap

### 3.1 Parity discipline for every lane (R5-10) — stated once, referenced per lane

- **(a) NEUTRALITY.** Every new behaviour is opt-in by a spec field or a new request kind. The adapter's `project()` (pine_adapter.cpp:1391-1533) never sets the field and never emits the kind, so adapter runs are byte-identical by construction. New spec blocks are folded into `hash_spec` **only when set**: it folds every field unconditionally today (native_execution_consumer.cpp:60-81), so even a defaulted new field would move every continuation hash; the precedent for a conditional digest is `precommit_digest_` (native_execution_consumer.hpp:414-418). Each lane adds a test that a default spec hashes to the pre-change constant. Every new durable field is hashed (`check_broker_state_hash_coverage.py`, fail-closed, waivers in `broker_state_hash_waivers.txt`). Proof per PR: `scripts/run_corpus.sh` + `scripts/verify_corpus.py` locally, then ONE Cloud Run campaign sweep on the final tree (fixed population, 4190 probes, zero tolerance on the hard band — native-refactor-progress.md:41-47; a fresh exact-HEAD verdict is required, AGENTS.md:124-143). A lane that cannot be shown neutral by construction does not merge.
- **(b) TWIN.** One chosen probe per lane runs through the adapter AND a hand-written `NativeStrategyHost` on the same bars and spec. Trade lists are diffed **by identity** (entry / exit time, price, qty, pnl, commission), never by trade number; then event, ownership and hash detail (S's D → E order). A difference is allowed only where a named TV-quirk row of §1 is active, and each one is itemized. Because 430 test files drive the adapter and 14 the native host (O), every lane also adds native-only tests: a lane is not accepted on the corpus alone, nor on a unit test alone (S).
- **(c) EPOCH BUDGET.** Three inline-namespace epochs move: `native_order_v5` (native_order.hpp:25), `engine_script_run_v17` (native_host.hpp:18) and — *fresh*, not in the inputs — `native_run_spec_v2` (native_run_spec.hpp:15), which every new spec field moves. One bump each per release batch: `native_order_v6` = L3 + L7 + L4's origin / event; `engine_script_run_v18` = L4's host hooks + L5 + L6 virtuals + L2's `hash_host_extension` + §2.ii a, f; `native_run_spec_v3` = the spec blocks of L2, L4, L5, L6, L8 (L9 if it ships in the batch). A lane that lands before its batch closes stages behind the open epoch, so codegen-built strategies rebuild once per batch. Epoch lanes are not neutral-refactor lanes: the brief lists the header extension explicitly; the frozen C++ ABI fixtures (`tests/fixtures/native_cpp_abi/host-*`) and the ABI checkers (`check_native_cpp_abi.py`, `check_settlement_cpp_abi.py`) move with them; `variant_size` pins change (`OrderIntent` native_order.hpp:1342, `CommandEvent` native_order.hpp:1346). Every new test unit compiles first against the frozen previous-epoch header closure (fail-before, `CLAUDE.md`).
- **Worker verification** (`CLAUDE.md`): `ci_preflight.py`, then `ci_verify.py release`; plus `check_c_abi_runtime.py` and `check_native_include_independence.py`. No campaign sweep from a worker.

### 3.2 Lanes: scope, size, dependencies, epochs

| Lane | Concern | Size | v1 | Depends on | Epoch impact | Input lanes F / O / S |
|---|---|---|---|---|---|---|
| **L0** | Docs truth pass: `native-engine.md` fixes + the ADR-0001 correction list (§2.vi); interim HTF recipe; open-bar lookahead warning | S | yes, first | — | none | L0 / — / — |
| **L1** | Kernel-only build target; move `compat/pine/market_admission.cpp`; installed-header closure; CI lane (§2.i) | M | yes | — | none | L1 / L1 / A |
| **L2** | Report truth for native hosts: equity curve, equity metrics, range-end row, per-bar hash, host hash extension, per-trade accessors | M | yes | — (∥ L3) | `native_run_spec`; the hash virtual waits for the shared `engine_script_run` bump | L2 / L2 / G (part) |
| **L3** | Sizing bases (percent-of-equity, cash, fee-net), placement-vs-fill timing, fractional reduce, sibling reservation | M/L | yes | — (∥ L2) | opens `native_order_v6` | L3 / L3 / C + B (reservations) |
| **L4** | Margin model: per-side initial + maintenance, liquidation policy / events / price, host sizing hook, kernel-originated request | L | yes | L2 (equity marking — O); state-hash coverage | all three epochs | L4 / L4 / D (margin part) |
| **L5** | Calc-timing contract: partial-bar view, open-bar guard, sub-bar hook, calc-on-fill re-entry, every-tick | M/L | yes | runs after L7 in the R5-11 order; ∥ L6 | `engine_script_run_v18`, `native_run_spec` | L5 / L7 / E |
| **L6** | Native HTF / aux feed registration for bare hosts (§2.iv) | L | yes | L1; user decision U1; ∥ L5 | `engine_script_run_v18`, `native_run_spec` | L6 / L8 / F |
| **L7** | Order ergonomics + bracket / trailing completeness: builder, relative anchors, zero / tick offsets, retain-trail replace, cancel-all / by-label, working view | M | yes | L3 (shared `native_order` epoch) | `native_order_v6` | L7 / L9 / B |
| **L8** | Price grid: quantized fills, optional quantized triggers | M | yes | L3 (sizing uses the grid price — O) | `native_run_spec` | — (quirk) / L6 / G (as `FillModel`) |
| **L9** | Risk limits: `strategy.risk.*` equivalents, halt semantics, day-boundary basis | M | in scope, post-v1 | L4 (kernel-originated flatten), L2 (drawdown needs equity) | `native_run_spec`, one event | L9 / L10 / D (risk part) |
| **L10** | Examples under `PINEFORGE_BUILD_EXAMPLES`, `native_module.hpp`, native API reference, dual-run harness (§2.v) | S | yes | L1; one example per landed lane | none | L8 / L12 / J |
| **L11** | Neutral coupling extraction, small PRs (§2.ii a-i) | M | post-L1, any time (a, d wanted for v1 — F) | L1 (link gate) | a, f ride `engine_script_run_v18`; the rest none | L11 / L11(a) / I (part) |
| **L12** | Hash-visible TV extraction (§2.ii j-m); explicit campaign exception | L | post-v1 | L4 (k, l), L6 (m), a planned hash-domain bump (j) | broker-state hash domain | L12 / L11(b) / I |
| **L13** | C order / callback API (§2.iii) | L | post-v1 | L3, L7, the epoch batch | new `PF_API` symbols only | L10 / L5 / H |

**Order:** L0 → L1 → (L2 ∥ L3) → L7 → L8 → L4 → (L5 ∥ L6, one shared epoch bump) → L10 → L11 → L9 → L12 → L13.

### 3.3 Lanes: API, neutrality proof, twin test

| Lane | API proposal (signature level → Appendix) | Neutrality proof (§3.1 a) | Twin test (§3.1 b) |
|---|---|---|---|
| L0 | — | docs only | — |
| L1 | option `PINEFORGE_BUILD_SOURCE_LAYER`, targets `pineforge_kernel` + unchanged `pineforge` (§2.i) | same TUs, same flags; ctest + corpus | kernel-only link of the two examples + the 20 source-free native tests; installed-header closure |
| L2 | `NativeReportPolicy { HostRecorded, KernelRecorded }`, `report_open_position_at_end`, `hash_host_extension`, `native_metrics()` → A.1 | the adapter stays `HostRecorded`: no double recording (pine_strategy_host.cpp:1582-1601), `fill_report` bytes unchanged on the corpus | native example: curve length == script bars, drawdown equals a hand walk (F); 50-bar host asserts a non-empty curve, a finite max drawdown and a range-end row (O); same generic strategy through both hosts → equal rows and curve (S) |
| L3 | `Sized{side, basis, time, grid_policy, reserve_percent_fee}` as the 6th `OrderIntent`; `ScopeFraction{fraction, claim}` in `ReductionSize` → A.2 | the adapter never emits `Sized` / `ScopeFraction`; it keeps `HostSized` (pine_adapter.cpp:4372-4377) | percent-of-equity probe, zero fee, no grid: exact; with grid / fee: differences explained by SZ10 only (F). Differential: `Sized` vs a host-resolved `HostSized` on the same facts → identical `ExecutionAppliedEvent` (O). Gap, reversal, fee-bearing first lot; two 50 % siblings, partial close, pending parent, replacement (S) |
| L4 | `NativeMarginModel`, `native_liquidation_price()`, `resolve_margin_call_units`, `on_native_margin_call`, `RequestDefinition::origin`, `MarginCallEvent` → A.3 | the adapter leaves `margin` unset and keeps answering `AdmitWithHostMargin` (native_host.hpp:316-321, effect native_execution_consumer.cpp:3015-3039); assert in `project()` that the projected spec has no margin model; an absent model means the liquidation path is never entered | first a plain maintenance breach in both hosts (S); then a margin-call probe reproduced with `ShortfallMultiple 4`: same bar, same side; quantity deltas itemized against MG15 (F, O) |
| L5 | `NativeCalculationTrigger`, `on_native_recalculate(…, reason, cause)`, `on_native_sub_bar`, `current_partial_bar()`, `NativeOpenBarView` → A.4 | the adapter keeps `BarClose` + its own cascade; `born_on_remaining_path` (native_execution_consumer.cpp:3253-3257) and `drain_applied_notifications` (native_execution_consumer.cpp:4146-4159) are frozen; accessors are inert | COOF probe re-expressed natively: same fills where the TV waypoint rule is inactive (F); a host that flips on its own fill produces the adapter's cascade in shape (O); callback reason, cursor, newborn eligibility and event ordinals compared (S) |
| L6 | `NativeTimeframeSubscription`, `on_native_timeframe_bar`, `native_series_bar` → A.5, §2.iv | `subscriptions` empty for the adapter; registration-gated pump; `engine_security.cpp` and pine_strategy_host.cpp:1316-1400 untouched; corpus diff before / after (O: highest parity risk of any lane) | HTF-filter probe on a 24x7 symbol: exact (F); `"D"` over a 15m feed equals a `TimeframeAggregator` baseline bar for bar (O); installed-feed and no-feed cases (S) |
| L7 | `native_working_requests()`, `cancel_all()`, `cancel_where(label)`, `TriggerAnchor`, `TrailTicks`, zero offset, `ReplaceOptions`; toolkit `submit_bracket`, `OrderBook` → A.6 | new kinds unused by the adapter; anchor defaults to `Absolute`; the adapter's `tick * 0.5` sentinel keeps working | bracket + trailing probe through the builder: exact where TR5 / FP7 are inactive (on-grid levels) (F); the zero-offset trail test asserts the run no longer fails (O); parent-limit + stop sibling, parent rejection, replacement, parent-cycle revival (S) |
| L8 | `NativePriceGrid { None, QuantizeFills, QuantizeFillsAndTriggers }`, `NativeGridRounding` → A.7 | `None` for the adapter, permanently — **native-only by ruling, audit lane P6: §3.6.2** | a host with `QuantizeFills` books on-grid prices; a second test pins that `QuantizeFillsAndTriggers` still differs from TV's half-tick threshold (FP5) (O) |
| L9 | `NativeRiskLimits`, `NativeRiskEvent`, `MatchRejectReason::RiskLimit` → A.8 | `risk` unset for the adapter; its ledger (pine_adapter.hpp:650-675) untouched — **retained after measurement, audit lane N12; native-only by ruling, audit lane P6: §3.6.1** | risk probes: same halt bar (F); each limit + the day-boundary basis (O); breach, forced close, cancellation, next-day reset (S) |
| L10 | `PINEFORGE_EXPORT_NATIVE_STRATEGY(Class)` (§2.v) | build-only | the examples run in ctest; the independence checker compiles the relocated examples |
| L11 | — (renames / moves, §2.ii a-i) | rename / move only; hashed enumerator values pinned; sweep unchanged | — |
| L12 | — (§2.ii j-m) | **not neutral**: coordinated sweep, waiver updates, hash-domain plan (`"pineforge-broker-state/v17"` engine_state_hash.cpp:23, pinned to one occurrence by check_broker_state_hash_coverage.py:205) | trades identical, hashes re-baselined once |
| L13 | `pf_native_*` symbols (§2.iii) | additive symbols; `check_c_abi_runtime.py` exits 0 | a pure-C twin of the market example reproduces the C++ trade rows (O) and the event-history hash (F); C test: submit a market, replace a limit, cancel a child, read the Applied event (S) |

### 3.4 Input dependency notes against the R5-11 order

Consistent and kept: price grid after sizing (O); margin after report truth (O); ergonomics after sizing for the shared `native_order` epoch (F); risk after margin and report (F, O); C API after sizing + ergonomics (F, O); examples and neutral extraction after L1 (F, O); hash-visible extraction after L4 / L6 (F, O); HTF after the build split (S).

Flagged:

| # | Input note | Against R5-11 | Disposition |
|---|---|---|---|
| 1 | S: sizing depends on brackets; brackets are "L, prerequisite for everything" | L3 runs before L7 | rejected by R5-2 |
| 2 | S: report / metrics lane depends on sizing, margin and calc-timing | L2 runs second | S's lane was wider (fill model + excursion + report); F and O see no such dependency |
| 3 | F: L2's virtuals need the host-epoch bump | L2 lands long before the shared bump | L2 uses O's spec-policy form (no virtual); only `hash_host_extension` waits for the batch (A.1) |
| 4 | R5-11 shares one bump between L5 and L6 | L4, earlier, also adds host virtuals | `engine_script_run_v18` opens at L4 and closes after L5 ∥ L6, or L4's hook ships with that batch — supervisor call (§4 Q1) |
| 5 | F: the sub-bar hook is optional for v1 | R5-5 / R5-11 put it in v1 | ruling applied |
| 6 | O: price grid, calc-on-fill and HTF deferrable past 1.0 | v1 | R5-3, R5-5, R5-6 |
| 7 | S: examples after the C API "as applicable" | L10 runs before L13 | the C example joins with L13 |
| 8 | O: HTF needs a lookahead decision before it starts | — | U1 blocks the L6 brief, not L0-L4 |
| 9 | F: move all `request.security` machinery to the source host after native HTF | R5-6 keeps the feed store kernel-side | narrowed (§2.ii m) |
| 10 | S: margin + risk in one lane, both v1 prerequisites | L4 v1, L9 post-v1 | R5-9 |

### 3.5 Prerequisites for a v1.0.0 that promises "usable without Pine, in C++"

| Tier | Lanes | Why |
|---|---|---|
| Day-one blockers (O) | L1, L2, L3, L10 | Without L2 `fill_report` silently returns an empty curve and degenerate equity metrics (engine_metrics.cpp:168) and drops an open final position. Without L3 every non-unit order needs a hand-written `resolve_execution_terms`, and a mistake is a run-terminating `TermsUnresolved` (native_execution_consumer.cpp:2753-2755). Without L1 "kernel" is a claim, not an artifact. L10 is the minimum buildable proof. |
| Required by the promise (R5-11) | L0, L4, L5, L6, L7, L8 | A kernel that cannot liquidate is not a broker model (O); bracket / trailing corner cases are hit immediately (O); calc-timing and HTF are Pine features the user wants natively (R5-5, R5-6); the price grid is generic and missing (R5-3). |
| Wanted with v1, not blocking | L11 a, d | removes the last `source::` name from kernel signatures and the orphaned TV commentary (F) |
| In scope, 1.x | L9 | R5-9; a toolkit stand-in is acceptable meanwhile (F) |
| Post-v1 | L11 rest, L12, L13 | L12 is required before claiming the adapter is a *thin optional layer* rather than a runtime-dependent sibling (S); L13 only if the promise is widened to C hosts (U2) |

### 3.6 Kernel features the Pine adapter never declares — native-only by ruling (audit lanes N12, N13, P6)

The second independent R5 audit (§6, lane P6) asked for a decision on the two
kernel features no adapter run declares, `NativeRunSpec::price_grid` (L8, L8b)
and `NativeRunSpec::risk` (L9): either close the measured divergence in the
kernel so that `project()` can declare them, or record them as native-only by
ruling, with the measurement and with a native example that exercises each.
**Both are native-only by ruling.** Neither divergence can be closed by a
generic kernel capability: what separates the adapter from the kernel is, in
both cases, a TradingView rule that would have to be spelled into the kernel
to disappear (ADR-0001 rule 2). §3.6.1 is lane N12's measurement of the risk
rules with the P6 decision appended; §3.6.2 is the price grid's, drawn from
lanes R7, L8b and N13; §3.6.3 is the inventory that shows these two are not
the only fields `project()` leaves unset, and that none of the others is
undecided either. The normative table is ADR-0001's "Kernel capabilities the
Pine adapter does not declare"; `scripts/check_native_feature_rulings.py`
holds it against the header, the adapter and the examples.

A native-only feature is not dead code. Its consumers are the hosts the kernel
exists for: C++ hosts (`examples/native/native_price_grid_strategy.cpp`,
`native_risk_limits_strategy.cpp`, `native_trail_risk_strategy.cpp`), C hosts
(`examples/native/native_price_grid_c.c`; `PF_NATIVE_SPEC_EXT_RISK` in
`tests/test_native_c_api.c`) and the kernel-only tests
(`tests/test_native_price_grid.cpp` 1913 checks, `tests/test_native_risk_limits.cpp`
327 checks), all of which run in the kernel-only CI profile.

#### 3.6.1 `risk` — the TradingView risk rules against `NativeRunSpec::risk` (lane N12: measured, retained)

The independent R5 audit (§6, lane N12) found that no adapter run declares
`spec.risk` (`rg 'spec\.risk|NativeRiskLimits' src/source/` = 0 hits) while
`update_risk_state`, `SourceDayLedger`, `submit_intraday_loss_close` and the
`compat/pine` intraday cap stay the live path, and that the only artefact was
a twin (`tests/test_native_risk_limits.cpp` `twin_of_adapter_risk_halts`) that
itemized differences without a ruling. This section is the ruling, with the
measurement that decides it: **every `strategy.risk.*` rule stays in the
adapter; `project()` keeps `risk` unset.** No kernel or adapter source
changed; the measurement lives in `tests/test_adapter_risk_relower.cpp`
(nine paired scenarios, each run through the adapter and through a bare
`NativeStrategyHost` on the same tape, the adapter half harvested from main
683a82f, the kernel half pinned) and in the seeded corpus experiment below.

**The structural reason, common to all four rules.** Pine's
`strategy.risk.*` calls are script statements: the transpiler emits them
inside the per-bar body (corpus 442d497,
`ta-closedtrades-risk-introspection-01/generated.cpp` `on_source_bar` →
`set_pine_risk_max_drawdown(20, true)`; `cap-risk-gates-allow-max-intraday-01`
`set_pine_risk_direction(1)` / `set_pine_risk_max_position_size(2)` /
`set_pine_risk_max_intraday_filled_orders(3)`), so the adapter first sees a
limit on script bar 0, from `PineStrategyHost::set_pine_risk_*`
(pine_strategy_host.cpp:866-888) — after `prepare_native_begin` has already
projected the spec (pine_strategy_host.cpp:314) and `configure_native` has
folded it into the continuation digest. Measured: an env-gated probe printing
the adapter's risk configuration inside `project()` on the corpus reads
`max_drawdown=0 cap.active=0` for every risk probe. `NativeRunSpec::risk` is a
begin-time declaration (`native_run_spec.hpp:294-301,504`, folded at
native_execution_consumer.cpp:175-176); a statement-time limit has nowhere to
land. Routing any rule needs either the transpiler to hoist constant risk
statements into the constructor (out of this repository) or a kernel
re-declaration API mid-run (a hash-visible mutation of a digested field: an
epoch decision the audit lane does not budget). The witness is the `SW`
check of the test: under every rule the adapter host's kernel ledger stays
inert (`native_risk_state()` has no day, no peak, no block) while the
adapter's own rule acts.

**Per rule (STEP 1), assuming the values were known at begin time.** Adapter
line numbers are this tree's (683a82f); kernel semantics are
`native_run_spec.hpp:238-301` and native_execution_consumer.cpp:3079-3260
(evaluated at the script bar's open :6213/:6437, its close calculation
:6163, and after each applied drain :5924; fills counted at :4571; the block
refuses `would_open` at :2518).

| Pine rule | adapter mechanism | kernel counterpart | class | measured divergence (test scenario) | ruling |
|---|---|---|---|---|---|
| `strategy.risk.allow_entry_in` | `risk_.direction`; `project()` already declares `spec.allowed_open_directions` (pine_adapter.cpp:1508); the forbidden opposite entry is reshaped to `CloseOpposite` in `resolve_execution_terms` (:10509-10523) | per-opening cap `allowed_open_directions` | (i)+(ii), **already re-lowered** | none to measure: the cap is the kernel's, the close-only reshaping is the retained TV policy | done before N12; nothing to move |
| `strategy.risk.max_position_size` | precommit refuses a flat/same-side ENTRY at its fill when the LIVE book already holds ≥ the limit (:11572-11575, legacy pine_risk.cpp:115) | `max_abs_units`: refuses a fill whose RESULTING book would exceed it (:2531) | (iv) | `PS`: two-unit entries against 3 — adapter admits the second (live 2 < 3, book 4) and refuses the third; kernel refuses the second (`MaxAbsUnits` at bars 1, 2), book 2 | retained: a pre-fill-book gate is a TV emulation fact, not a generic cap |
| `strategy.risk.max_drawdown` | `update_risk_state(bar.close)` once per script bar at the close mark (:14918, :12158-12185): peak / running max drawdown at close marks, `>=` exact, percent of the current peak; the latch `risk_.halted` gates flat/same-side entries at precommit (:11565), never an opposite entry | `max_drawdown`: current drawdown vs percent-of-peak, at all three points; `BlockOpenings` refuses every opening | (ii) cadence + (iv) scope | `MG10-a`: bar 1 opens 600 under the entry and closes 100 under — kernel breaches at the open mark and refuses bar 2's add (book 100), adapter admits it (book 200). `MG10-b`: latched at bar 1's close, the adapter still reverses twice (`L→S` −600, `S→L`), the kernel refuses both reversals (`RiskLimit` at bars 2, 3) and holds. Corpus: `ta-closedtrades-risk-introspection-01` (20 % of peak) identical 1502/1502 rows with the kernel seeded — the rule never fires there | retained: the latched-reversal exemption cannot be composed on a kernel block; a close-only cadence knob alone would not close it |
| `strategy.risk.max_cons_loss_days` | `SourceDayLedger` (pine_adapter.hpp:650-658): +1 per losing TRADE on a new chart day, reset to 0 by any winning trade at its fill (:16075-16095); gated immediately at precommit (:11566-11569) and latched at the close (:12181-12184) | `max_consecutive_loss_days`: a day's NET realized result settles the streak when the next day opens | (iv) | `MG11-a` (loss; win then loss; loss): adapter latches at day 3's first loss (4 rows), kernel's netted streak never reaches 2 (6 rows, no event). `MG11-b` (loss then smaller win, twice): adapter resets daily (5 rows), kernel blocks at day 3's open (4 rows, one event). No corpus probe declares the rule | retained: an order-dependent per-trade streak is not a generic day result |
| `strategy.risk.max_intraday_loss` | day-open equity at the day's first bar (:14487-14492); checked at the bar open (:14589), along the bar's PATH at its adverse extreme with a resting `Stop` flatten (:14590, :13235-13275), and at a closing fill with that fill's own P&L unbooked (:15249-15266); epsilon `1e-9·max(1,|limit|)`; ticket `"Close Position (Max intraday Loss)"` → `CloseCause::RiskLimit`; the block refuses ALL placements for the chart day (`entry`/`order`/`close`/`close_all`/`exit`, :4325, :5980, :6702, :6830, :9774) and `cancel_all()` withdraws the working book (:16165-16168) | `max_intraday_loss` at the three points; `FlattenAndBlock` closes `AsPresented` under `"__kernel_risk__"` / `"Risk limit"`, blocks openings only, leaves working orders | (ii) ticket, day basis + (iv) path check, placement scope, unbooked fill, epsilon | `MG12-a`: day 2's first bar 100→85→95 — adapter closes at the extreme (85, −1500, cause 4) and refuses the day's later entry, kernel measures 0 at the open and 500 at the close, never breaches, ends with 300 units. `MG12-b`: breach at a bar open inside the day — same money on both sides (100 @ 88, −1200) under different tickets; the adapter withdraws a resting limit entry, the kernel leaves it working and it fills on day 3. Both sides mark the day's opening equity at its first bar: a gap there is not an intraday loss for either | retained: the path-extreme check is the rule's substance and the kernel has no path evaluation point; a risk ticket field and a `PathAdverseExtreme` check kind would be generic additions (mirroring `NativeMarginModel`) but would still leave the placement scope, the unbooked fill and the epsilon |
| `strategy.risk.max_intraday_filled_orders` | `compat::pine::IntradayCap` + `IntradayOrderBudget` (243 + 94 lines): charged slots per risk day, quota transfer, POOC deferral, noop-market skip, the close at the fill price or the bar's better extreme (`post_dispatch`), latch → all placements denied (`cap_placement_denied` :12150); ticket `"Close Position (Max number of filled orders in one day)"` → `CloseCause::FillCap`; day = `session_trading_day_index` for a `HHMM-HHMM` session, else the chart day (`chart_day_key` :12052-12077, `mday·100+month` in the CHART timezone) | `max_fills_per_day`: applied fills counted as they settle, evaluated at the three points; `FlattenAndBlock` at the drain's cursor; `SessionDay` / `CalendarDayInTimezone` on the SPEC timezone | (i) count + (ii) ticket + (iv) close price, charged-slot budget, placement scope, chart-day key | `MG13`: the second fill reaches the cap on a bar that closes above its open — adapter closes at that bar's HIGH (103) under its ticket, kernel at the close (101) under its own. `MG14`: chart timezone `America/New_York` over a UTC symbol — bar 4 (04:00 UTC) is a new chart day for the adapter and the same UTC day for the kernel. Corpus, kernel seeded with the declared cap: `cap-risk-gates-allow-max-intraday-01` identical 1464/1464 (the third fill of each day flattens the book, so block and latch coincide); `cap-max-intraday-filled-orders-isolate-01` 3840 of 3916 rows differ from row 77 (2025-04-08); `cap-gatekeeper-intraday-risk-01` 312 of 604 differ from row 293 (2025-10-25); `composite-bracket-cap-range-pending-stop-01` 2370 of 2384 differ from row 17 (2025-04-02 02:45), two extra rows | retained: the count alone is expressible, but the kernel's block would refuse fills the transfer admits and the close price is TV's; a report-only action would give the adapter a count it cannot use (charged slots ≠ applied fills) |

**Proposed generic additions (not implemented — no consumer today).** A
risk ticket (`label` / `comment` on `NativeRiskLimits`, folded only when set,
exactly as `NativeMarginModel::liquidation_label`); a `NativeRiskDay`
timezone override for a host that reports by a wall-clock other than the
spec's; a `PathAdverseExtreme` check kind for the intraday limit mirroring
`NativeLiquidationCheck`; a report-only action. Each is generic on its own
merits, none unblocks a rule above while the statement-time delivery stands,
and adding them now would be the dead weight the same audit rules against
(G2). The order in which they would matter, if the transpiler ever hoists
the statements: ticket → day timezone → path check.

**Acceptance evidence.** `tests/test_adapter_risk_relower.cpp`: 62 checks,
the adapter half reproduced bit for bit against the 683a82f harvest, the
kernel half pinned; corpus byte-identity not applicable (no `src/` or
`include/` change); the five corpus risk probes measured as above with the
kernel seeded in an uncommitted, env-gated experiment.

**The P6 decision: native-only.** Re-verified on main `b8e7976e`: the ordering
stands (`project()` at pine_strategy_host.cpp:315, `configure_native` at :316,
the `set_pine_risk_*` setters at :867-888, and corpus 442d497
`cap-risk-gates-allow-max-intraday-01/generated.cpp:188-191` still emits the
three calls inside `on_source_bar`); `rg 'spec\.risk|NativeRiskLimits' src/source
include/pineforge/source` is still empty; `test_adapter_risk_relower` still
reads `62 checks, 0 failures`. Closing the divergence instead would take, in
order: the transpiler hoisting constant risk statements into the constructor
(another repository) or a mid-run re-declaration of a digested spec field (an
epoch decision no lane has been granted); then the four generic additions
above; and then, per rule, exactly the parts the table calls TradingView's —
the pre-fill live-book gate, the latched-reversal exemption, the per-trade
streak, the all-placement block with the unbooked fill and the epsilon, the
charged-slot budget. Those are not knobs a second broker model would ask for:
each exists to reproduce one emulator, which is rule 2's test for "belongs in
the adapter". So the adapter keeps `strategy.risk.*`, the kernel keeps a
generic account-risk block, and the four additions stay unimplemented until a
native host asks for one (no consumer today, and adding them would be the
dead weight this section exists to rule out).

#### 3.6.2 `price_grid` / `grid_rounding` — TradingView's tick rules against the kernel grid (lanes R7, L8b, N13: measured, waived)

**The ruling predates the measurement.** R5-3 made the price grid its own
kernel lane and left "TV's exact half-tick rule … in the adapter on top"; E7
(§4.1) states the containment as "`NativePriceGrid::None` for the adapter
permanently; never 'unify'"; the L8 row of §3.3 gives its neutrality proof as
"`None` for the adapter, permanently". The adapter was never meant to declare
the grid. Lane R7 tried the re-lowering anyway, and gap lane N13 repeated the
trial after L8b; row PG (§1.7) holds the counts. In summary:

| blocker | what it is | state |
|---|---|---|
| B1 | under `QuantizeFillsAndTriggers` the matcher reported a cursor print inside the quantized region but short of the raw level, and the core's raw re-validation aborted the run ("native working-request preparation failed": nine zero-offset-trail tapes, six `process_orders_on_close` panels) | **closed generically by L8b**: the tick-quantized print is the reached price, and the core re-validates on the same ladder (`native_order::ActivationGrid`); 0 aborts under the N13 re-run |
| B2 | TradingView's tick quantization is a property of the ORDER KIND — stop and limit legs and a trail's activation are tested on the quantized bar; the trail stop, the running best, stop-limit entries and the `calc_on_order_fills` cursors on the raw path — while `price_grid` is a property of the RUN and the matcher hands one threshold to every trigger | **open, and not closable**: still moves 30 pinned checks in 4 units (`test_coof_market_limit_recross_l4c` 24, `test_stop_tick_rounding_l4d` 3, `test_adapter_grid_relower` 2, `test_zero_offset_trail_rides_l4c` 1) with no adapter-side remedy; 12 more checks in 3 units have an adapter-side cause |

The corpus cannot arbitrate: all 312 probes run a 0.01 tick on an on-grid
feed, and under the trial 5 of them differ, in the engine-only
entry-incarnation column only.

**Why B2 is not closed in the kernel (route (a) rejected).** The only kernel
change that would let `project()` declare the grid is a per-order-kind
quantization mask: quantize a stop leg but not a trail stop, a trail's
activation but not its running best, a limit but not a stop-limit. No broker
model asks for that; it is one emulator's inconsistency, and the kernel's own
rule (L8b) is the opposite on purpose — one rule for every kind. Rule 2's test
applies literally: the mask cannot be justified without the word
"TradingView". And what the adapter keeps is not a duplicate of the kernel
mechanism that a re-lowering would delete: `source_trigger_threshold`
(pine_adapter.cpp:329), `source_level_on_price_grid` (:315) and the
`nearest_tick` / `source_bar_fill_tick` / `directional_tick` spellings (:253,
:274, :298) are a *different* rule — a half-tick threshold per order kind with
the fill booked elsewhere (E7) — which is why re-lowering moves outcomes
instead of preserving them.

**Re-verified on main `b8e7976e`.** `rg 'spec\.price_grid|\.price_grid\b'
src/source include/pineforge/source src/compat` is empty.
`test_adapter_grid_relower` — the permanent witness: its section 4 pins
TradingView's per-kind rule on boundary prints as a neutrality differential
harvested from the unchanged adapter, and its section 5 pins the measured
divergence, the grid firing a trail stop one bar early where `best - offset`
lands one ULP under its ladder point — reads `33878 checks, 0 failures`;
`test_native_price_grid` `1913 checks, 0 failures`; its adapter twin 9 of 9.
The trial itself (raw levels submitted, the grid declared) was temporary in
both lanes and is not in the tree; its counts above are N13's, not re-run here.

**What landed for the ruling.** Until P6 no example exercised the grid (the
second audit's §5: "lanes with no example: price grid"), and the C spelling
(`PF_NATIVE_SPEC_EXT_PRICE_GRID`, translated at native_c_host.cpp:1313-1317)
had no executed consumer anywhere in the repository — no C test and no C
example set the bit. `examples/native/native_price_grid_strategy.cpp` runs one
strategy over one sub-tick tape (a 0.25 ladder under a composite feed) in the
four modes and asserts every fill's raw and booked price against hand-computed
values; `examples/native/native_price_grid_c.c` is the same host through the C
API and reads the same numbers back from `strategy_native_events_v1`:

| mode | entry (open 100.10) | target (limit 100.75) | protect (stop 99.50, open 99.40) | breakout (stop 99.75, high 99.65) | its exit (open 99.60) | net |
|---|---|---|---|---|---|---|
| `None` | 100.10 | 100.75 | 99.40 | — | — | −0.05 |
| `QuantizeFills`, `HalfUp` | 100.00 | 100.75 | 99.50 | — | — | +0.25 |
| `QuantizeFills`, `Directional` | 100.25 | 100.75 | 99.25 | — | — | −0.50 |
| `QuantizeFillsAndTriggers`, `HalfUp` | 100.00 | 100.75 | 99.50 | 99.75 | 99.50 | 0.00 |

#### 3.6.3 The inventory: every `NativeRunSpec` field `project()` leaves unset

`scripts/check_native_feature_rulings.py --list` on main `b8e7976e` plus this
lane: 43 fields, 33 assigned by `project()`, 10 not. Each of the ten has a row
in ADR-0001's ruling table, and the check fails when a field has neither an
adapter declaration nor a row, when a row's consumers do not exist or never
spell the field, when a native-only row has no `examples/native/` host or no
`tests/` unit, or when `project()` starts assigning a field whose ruling is
still in the table.

| field | ruling | decided by |
|---|---|---|
| `price_grid`, `grid_rounding` | native-only | §3.6.2 |
| `risk` | native-only | §3.6.1 |
| `max_abs_units` | adapter-policy: the pre-fill live-book gate is TradingView's | row MG2; N12's `PS` measurement (book 4 against 2) |
| `max_open_lots` | adapter-policy: Pine pyramiding is a per-cycle entry count | row MG3; the contract comment in `project()` |
| `initial_margin_fraction` | adapter-policy: TradingView's money admission answers `AdmitWithHostMargin`; the declared `margin` model is maintenance-only | row MG4; the wave-4 ruling in `project()` |
| `report_open_position_at_end` | adapter-policy: TradingView's range-end report is report shape, not a mark-to-market row | row RP5 |
| `open_bar_view` | adapter-policy: TradingView's open scheduling and its fill callback read the whole bar | rows CT4, E5 |
| `subscriptions` | adapter-hook: declared through `declare_timeframe_subscriptions` at begin (lane R3b) | §2.iv |
| `auxiliary_feed` | adapter-policy: the adapter's own auxiliary drive, retained on three measurements (lane N7) | §2.iv |

Out of this inventory's scope: request kinds and host members the adapter
never emits or calls (`Sized`, `ScopeFraction`, `native_open_lots()`, …). E3
rules `Sized`; the C header's COVERAGE block rules the host surface.

---

## 4. Risks and open questions

### 4.1 Entanglements: a generic feature tangled with a TV quirk

| # | Risk | Why it bites | Containment | Src |
|---|---|---|---|---|
| E1 | **Spec hashing.** `hash_spec` folds every field unconditionally (native_execution_consumer.cpp:60-81) | Any new field, even defaulted, changes every continuation / broker-state hash, adapter runs included | Fold new blocks only when set, as a tagged extension (precedent native_execution_consumer.hpp:414-418); test that a default spec hashes to the pre-change constant | F |
| E2 | **Margin.** The TV margin call is built host-side from kernel primitives (a Stop-triggered `Reduce` bound to openings, or `execute_current`; pine_adapter.cpp:11539-11606, pine_adapter.cpp:11653); its chronology lives at pine_adapter.cpp:12157-12234 and its excursion sample differs by chronology (pine_strategy_host.cpp:709-739) | A kernel model under an adapter run would liquidate twice or reorder settlement; a checkpoint that reduces mid-path changes lot state exactly where the adapter's chronology lives | The adapter never sets `spec.margin` and keeps `AdmitWithHostMargin`; an absent model is an early return, not a computed no-op (O). Do not re-express the TV margin call on the generic model (F) | F O S |
| E3 | **Sizing.** TV sizes from the tick-rounded signal close, 10-digit-rounded equity and fee-reserved cash, frozen at signal, with fill-time exceptions in COOF paths (pine_adapter.cpp:1551-1591) | One "generic" percent sizing serving both would break parity or import TV rounding into the kernel; the last ulp differs on lot-stepped instruments | Two paths on purpose: `Sized` (kernel, plain arithmetic, timing as a first-class field) vs `HostSized` + the adapter resolver. Never "simplify" the adapter onto the kernel basis | F O S |
| E4 | **Calc on fill.** Kernel suffix eligibility is continuous along the segment; TV refills only at O/H/L/C waypoints (pine_scheduler_native.cpp:588-640, `coof_next_waypoint` pine_adapter.cpp:3803) | Changing `born_on_remaining_path` or the notification order shifts adapter fills | Freeze native_execution_consumer.cpp:3248-3257 and `drain_applied_notifications` (native_execution_consumer.cpp:4146-4159); new behaviour only behind new opt-ins | F |
| E5 | **Open-bar lookahead.** The adapter reads the full bar in `on_bar_open` (margin path scheduling uses H/L, pine_adapter.cpp:12206-12233) | The complete bar cannot be masked globally | Opt-in `OpenOnly` view (A.4); loud doc warning in L0 | F |
| E6 | **Double equity recording.** The source host already records per bar (pine_strategy_host.cpp:1582-1601) | Kernel recording would duplicate points and shift drawdown | Report policy / ownership (A.1); the adapter stays `HostRecorded` | F O |
| E7 | **Price grid.** TV does not merely quantize: it tests triggers against a quantized bar while booking the fill on the *unrounded* level, so the adapter shifts the threshold (pine_adapter.cpp:328-376) and books elsewhere | A kernel grid that quantizes both sides is correct and not TV; moving rounding into the kernel would change both native results and the adapter | `NativePriceGrid::None` for the adapter permanently; never "unify". Before L8: a byte-level experiment that the default price spelling on decimal ticks (one-ULP representations) does not move (S) | O S |
| E8 | **HTF.** The evaluator machinery carries TV publication semantics (lookahead, boundary deferral, early close, `syminfo.type` branching engine_security.cpp:66-72). The feed store is TV-calibrated too: "W" / "M" from native daily bars, native stamps as the period partition (engine_aux_security.cpp:48-87, *fresh*). Two aggregators exist: script buckets use `native_calendar`, `request.security` uses `TimeframeAggregator`, whose completion rules are TV-pinned (timeframe.hpp:312-354) | Reuse risks changing the *order* of the Pine scheduler's pumps; a bare host inherits TV partition rules; native HTF may differ from Pine HTF on early-close sessions | Registration-gated pump; no change to pine_strategy_host.cpp:1316-1400; full corpus diff; supplied-bar provenance preserved, never silently replaced by chart aggregation (S); differences on early-close sessions are recorded, not "fixed" (F) | F O S |
| E9 | **TA numerics are TV-calibrated** (1e-10 band in DMI / MFI / percentrank; conditional-call extremum ring ta.hpp:60-103) | Non-Pine users may expect IEEE semantics; changing them breaks parity | Rename only (§2.ii b). For an every-bar caller the ring equals a positional window (ta.hpp:79-80). Document. → U6 | F O S |
| E10 | **Dead-but-hashed lot flags** (§2.ii j) | Deleting them changes `broker_state_hash` | Separate, explicitly non-neutral lane (L12) | F O |
| E11 | **OCA and ownership.** Pine's default-sized OCA and `from_entry` reservation rules are source policies | Source IDs, callsite tokens or Pine labels leaking into `WorkingRequestCore` | Opaque keys and receipts only (R5-4) | S |
| E12 | **Legacy input tolerance.** `LegacyTolerant` changes bar-structure and slot-label admission (native_run_spec.hpp:45-74, market_driver.cpp:14-55) | A rename must preserve the Pine route's hash and failure shape | Values pinned (§2.ii c); strict stays the `NativeRunSpec` default | S |

### 4.2 Process risks

| # | Risk | Containment | Src |
|---|---|---|---|
| P1 | **Epoch bumps** move the frozen C++ ABI fixtures and the settlement ABI checker `CLAUDE.md` names; a neutral-refactor worker may not edit frozen headers | Bump lanes are not neutral-refactor lanes; the brief lists the header extension (§3.1 c) | F O S |
| P2 | **Stale fence.** `CLAUDE.md` names "measured sources" `engine_strategy_commands.cpp`, `engine_fills.cpp`, `engine_risk.cpp`, `engine_market_admission.cpp`; none exists in `src/` at `73817c1` (only `engine_orders.cpp` and `engine_run.cpp` do) | The supervisor restates the fence for the post-R4-C tree (likely `native_execution_consumer.cpp`, `native_order.cpp`, `engine_execution.cpp`, `src/source/*`) | F |
| P3 | **Corpus gate state.** S's audit found `scripts/run_corpus.sh` stopping at the corpus strategy compile: committed `generated.cpp` files no longer match the engine (8/8 drifted from the pinned codegen commit under `VERIFY=1 ONLY=validation/analyzer scripts/regen_corpus_cpp.sh`) | §3.1 a relies on that script. Re-pin the engine / codegen corpus tuple before L1. Not re-verified in this merge (no builds were run) | S (single-source) |
| P4 | **Hash compatibility.** The source hash covers hundreds of adapter and scheduler fields (pine_state_hash.cpp:206-605); L4 / L9 add hashed state, L12 removes some | A hash mismatch is a diagnostic until the trade diff is known, never a reason to relax parity (S). Open: does the campaign gate compare broker-state hashes across releases, or only trades (O)? → Q3 | O S |
| P5 | **Test asymmetry:** 430 adapter test files vs 14 native | native-only tests in every lane (§3.1 b) | O |
| P6 | **Stream vs batch.** The stream refuses monthly input, mixed modes, in-session gaps, non-empty FX curves, security feeds and source setters (native-engine.md:406-420) | L5 and L6 must state which refusals are generic and which are deliberate source limits | S |
| P7 | **C ABI variant growth** | §2.iii hardening rules | S |
| P8 | **Unverified in the inputs:** whether `execution::LifecycleEffects` (execution.hpp:110-113) is ever non-empty from the Pine path, else `PlacementSnapshot::legs` (pine_adapter.hpp:266) is the only live lifecycle (O); the `strategy.risk.allow_entry_in` setter chain beyond pine_strategy_host.cpp:803-804 → pine_adapter.cpp:15759 (O); what the ABI receipts pin (F); the probe corpus itself lives outside this tree (F) | verify inside the lane that touches each | F O |

### 4.3 Open design questions (supervisor level)

| # | Question | Recommendation in the inputs |
|---|---|---|
| Q1 | Does `engine_script_run_v18` open at L4 and close after L5 ∥ L6, or does L4's sizing hook ship with that batch (§3.4 row 4)? | — (raised by this merge) |
| Q2 | Is a hash-domain bump (needed by §2.ii j) acceptable before the R5 final audit (phase table, native-refactor-progress.md:23, *fresh*), or must L12 wait? | F asks; none |
| Q3 | Does the 4190-probe gate compare broker-state hashes across releases, or only trades? If hashes, L4 and L12 need a hash-version plan | O asks; none |
| Q4 | `SizeTime::AtAcceptance` needs a current execution point (native_host.hpp:477); between realtime inputs there is none — reject or fall back to `AtMatch`? | F: reject with `InvalidQuantityBasis` |
| Q5 | Liquidation check granularity: `PathAdverseExtreme` default; is a cheaper `CalculationOnly` worth shipping in v1? Pure spec value or injectable policy object (S)? | F: path-accurate default; merged A.3 = spec value + host sizing hook |
| Q6 | Does a native series return the current value, a retained series object, or both? It decides C ABI ownership and hash size | S asks; A.5 offers push + current-bar pull |
| Q7 | Is a close-reservation key stable across host restarts or only within one `RunIdentity`? | S: identity rules suggest run scope |
| Q8 | S's single-source proposals without a lane: neutral tail / trade-window policy (CT10), excursion-mode enum (RP1), stream-safe FX epoch (FP6 → U4) | not adopted; revisit on demand |
| Q9 | Which risk limits belong in the settlement kernel and which in a portfolio / risk service above it? | S asks; L9 keeps the four Pine-equivalent limits |

### 4.4 Questions that need the USER's decision

| # | Question | Positions |
|---|---|---|
| **U1** | **HTF lookahead default (R5-6).** Should native subscriptions offer lookahead at all? | F: none, by construction (a bar is delivered only when complete). O, S: a `lookahead` flag, default off. Blocks the L6 brief. |
| **U2** | **Does v1.0.0 promise C hosts, or C++ only?** | R5-7 currently says C++ only, C API last. F recommends C in 1.1, once `pf_native_request_v1` can carry `Sized` and the working view; S makes it a prerequisite only if "usable" includes C hosts. |
| **U3** | **Do risk limits ship in v1?** | R5-9: in scope, not a blocker. S wanted them as a prerequisite; F accepts a toolkit stand-in; O defers. |
| **U4** | **Stream-safe FX (FP6): in the roadmap or not?** It has no R5 lane. | F: out of scope here ("O7 clock"). S: its P10, an FX epoch consumed while flat, same curve in batch and stream. |
| — | **U4, answered by audit lane N6.** In: the curve declared through `configure_native_fx_curve` is the FX epoch of the run, batch or stream; the account converts at the rate of each driver point's effective time, so the epoch is consumed by the walk itself, flat or not, and a confirmed-bar stream is the batch of the same bars. No `NativeFxEpoch` type and no `on_native_fx_epoch` callback were needed. Out, permanently: realtime rate ingestion (a new rate is a new run) and tick-driven input under a curve (its hooks and partial slots read a stale conversion clock — measured). | — |
| — | **N6 follow-ups (measured on the adapter, not fixed — neither is trivial or generic).** (1) `apply_fx_open_margin_slice` (pine_adapter.cpp:3266-3282) throws on a rate step over a carried position whose margin is not 100 %: a 2× long carried through a 1.0 → 1.5 step ends the whole run with status 1 and an empty report, where the same book without a series, or with a series that never steps, completes. The kernel's `FxRoll` point is the generic mechanism that case can re-lower onto. (2) `default_sizing_intent` (pine_adapter.cpp:1745-1750) returns nullopt whenever any FX series is staged — even one point that restates the scalar rate — so an FX run silently loses the kernel `Sized` path R2b landed (measured: 1 kernel `Sized` request without a series, 0 with a one-point 1.0 series, same 50-unit book). | — |
| **U5** | **Open-bar view default.** Should `OpenOnly` be the default for hosts that are not `LegacyTolerant`? | F: safer for users, but a behaviour change for the ~20 existing native tests; default `Complete` proposed. |
| **U6** | **TA comparison band.** Is TradingView's 1e-10 tie-breaking a feature of the TA library, or a default to make configurable? | F, O: rename only; a switch changes indicator outputs and needs its own lane. S: separate policies, hash the policy identity. |

---

## Appendix A. Native API proposals (signature level)

Common rules: opt-in by spec field or request kind; hash only when present; defaults reproduce today's behaviour (§3.1). Each block merges the inputs' proposals (F P1-P7, O G1-G13, S P1-P10) and names what was not adopted.

### A.1 L2 — report truth (F P6 + O G11 + S P9)

```cpp
// native_run_spec.hpp
enum class NativeReportPolicy : std::uint32_t { HostRecorded = 0, KernelRecorded = 1 };   // O
NativeReportPolicy report_policy = NativeReportPolicy::HostRecorded;
bool report_open_position_at_end = false;    // F: mark-to-market rows at the last close; reporting only
// native_host.hpp
virtual void hash_host_extension(BrokerStateHashSink&) const {}   // F (S: hash_native_extension); folded after hash_source_extension; ships with the shared bump
NativeMetricsView native_metrics() const;                          // S: net_profit, equity, closed_trades, open_lots; non-virtual
```

- `KernelRecorded`: the consumer calls the existing `update_equity_extremes()` + `record_equity_point(script_open_ms)` (engine.hpp:2386, engine.hpp:2404) once per script calculation (after `invoke_callback` native_execution_consumer.cpp:4449, native_execution_consumer.cpp:4615, native_execution_consumer.cpp:4656) and after the `AfterCalculation` close; appends the per-bar broker hash when recording is on; synthesizes the open-position row at run end / `stream_end` with the existing `build_close_trade_with_costs` (engine_orders.cpp:237).
- Publish read-only `closed_trade_*` accessors (RP6).
- Alternative: F's virtual `owns_equity_recording()` (the A48 ownership pattern, native_host.hpp:465-475). Same semantics, but it costs a host-epoch bump, so the spec form is chosen to let L2 land early (§3.4 row 3).
- TUs: `native_execution_consumer.cpp` (seal path native_execution_consumer.cpp:4615-4656), `engine_run.cpp`, `engine_report.cpp`, `engine_state_hash.cpp`, `native_run_spec.{hpp,cpp}`.

### A.2 L3 — sizing, fractional reduce, sibling reservation (F P1, P7 + O G2 + S P2, P3)

```cpp
// native_order.hpp — native_order_v6; OrderIntent grows 5 -> 6 (static_assert native_order.hpp:1342)
struct CashValue      { double cash = 0.0; };         // account currency
struct EquityFraction { double fraction = 0.0; };     // of marked equity at the sizing point (0.10 = 10 %)
using SizeBasis = std::variant<CashValue, EquityFraction>;
enum class SizeTime : std::uint8_t { AtMatch = 0, AtAcceptance = 1 };      // F; S: MatchingPoint / Placement
struct Sized {                                         // O's name; F: SizedOpen
    Side side;
    SizeBasis basis;
    SizeTime time = SizeTime::AtMatch;
    ExecutionGridPolicy grid_policy = ExecutionGridPolicy::SnapToGrid;      // existing enum, native_order.hpp:442-456
    bool reserve_percent_fee = false;                  // divide cash by (1 + fee) for NativeFeeKind::Percent
};
using OrderIntent = std::variant<Flatten, Reduce, Transact, ReverseTo, HostSized, Sized>;
enum class ScopeClaim : std::uint8_t { Gross = 0, NetOfSiblings = 1 };      // R5-4: the generic half of S P2
struct ScopeFraction { double fraction = 1.0; ScopeClaim claim = ScopeClaim::Gross; };   // (0, 1] of the bound scope at match
using ReductionSize = std::variant<ExplicitUnits, OwnerOpenedUnits, ScopeFraction>;      // F
```

- Resolution: `units = cash / (price * point_value * fx)`, `cash` = the value or `fraction * marked_equity(price)`, `price` = the candidate's `default_resolved_price`. It plugs in where `HostSized` units resolve today (native_execution_consumer.cpp:2728; terms resolution native_execution_consumer.cpp:2721-2772). A host override of `resolve_execution_terms` keeps the last word. Non-representable → `MatchRejectReason::TermsUnresolved` (native_order.hpp:427); validation reuses `RequestRejectReason::InvalidQuantityBasis` (native_order.hpp:391).
- `NetOfSiblings`: a fraction-sized reduce claims the bound scope minus the units already claimed by live sibling reduces on it; keys are opaque, never source IDs (S).
- Not adopted: S's spec-level `default_sizing` + virtual `resolve_native_sizing` (duplicates the per-request intent, costs an epoch); S's `PercentFreeMargin` (needs L4's free-margin definition) and `EveryCalculation` timing; O's spec-level `size_net_of_fees` (per-request flag, 2 of 3); O's `PercentOfScope` basis (expressed as `ScopeFraction`); S's `ReplaceByKey` / `Fifo` modes (callsite semantics → adapter, R5-4).
- TUs: `native_order.{hpp,cpp}`, `native_execution_consumer.cpp`, `native_run_spec.cpp`, the definition hash.

### A.3 L4 — margin model (F P2 + O G6 + S P4)

```cpp
// native_run_spec.hpp
enum class NativeLiquidationSizing : std::uint32_t { RestoreMinimum = 0, ShortfallMultiple = 1, Flatten = 2 };
enum class NativeLiquidationCheck  : std::uint32_t { PathAdverseExtreme = 0, CalculationOnly = 1 };
struct NativeMarginModel {
    double initial_long = 0.0, initial_short = 0.0;                // fractions, > 0
    std::optional<double> maintenance_long, maintenance_short;     // absent = never liquidate
    NativeLiquidationSizing sizing = NativeLiquidationSizing::RestoreMinimum;
    double shortfall_multiple = 1.0;                               // TV's 4.0 is the adapter's choice, never the default (R5-1)
    std::optional<double> liquidation_min_units;                   // O: broker minimum trade
    NativeLiquidationCheck check = NativeLiquidationCheck::PathAdverseExtreme;
};
std::optional<NativeMarginModel> margin;    // mutually exclusive with initial_margin_fraction (kept as the one-scalar spelling)
// native_host.hpp
std::optional<double> native_liquidation_price() const;                                                   // F
struct NativeMarginCallView { NativePhysicalPosition position; double mark, equity, required; native_order::MatchCursor cursor; };
virtual std::optional<double> resolve_margin_call_units(const NativeMarginCallView&) const { return std::nullopt; }   // O
virtual void on_native_margin_call(const native_order::MarginCallEvent&) {}                                // O, S
```

- Mechanism (F): a **kernel-originated request**. At each script-bar open and after each applied fill the kernel computes the liquidation level and rests a `Reduce` / `Flatten` with `Stop{level}` bound to the live openings — the shape the adapter builds host-side today (pine_adapter.cpp:11558-11579). New hashed `RequestDefinition::origin { Host, KernelLiquidation, KernelRisk }`, new `CancelReason::Superseded` on re-price, `MarginCallEvent` joins `CommandEvent` (17 → 18, native_order.hpp:1346). O's alternative, a checkpoint on the excursion walk (native_execution_consumer.cpp:1769-1782), is not chosen: a reduction mid-path is the entanglement O itself flags (E2).
- FX revaluation (MG9) falls out once `native_fx_curve` drives the mark (`native_fx_curve.cpp`).
- TUs: `native_run_spec.{hpp,cpp}`, `native_execution_consumer.cpp` (`admit_opening_inspect` native_execution_consumer.cpp:1982-2015 per side; scheduling next to `invoke_bar_open_callback` / `drain_after_applied`), `native_order.*`, `engine_state_hash.cpp` + the coverage checker, `c_abi.cpp` + `pineforge.h` (`pf_native_run_spec_v2` or an extension struct; `CLAUDE.md` SOP).

### A.4 L5 — calculation timing (F P4 + O G8, G9 + S P6)

```cpp
// native_run_spec.hpp
enum class NativeCalculationTrigger : std::uint32_t { BarClose = 0, BarCloseAndFills = 1, EveryModeledPoint = 2 };   // O
NativeCalculationTrigger calculation = NativeCalculationTrigger::BarClose;
std::uint32_t max_recalculations_per_point = 8;                                   // O; the adapter keeps its own kCoofLoopGuard
enum class NativeOpenBarView : std::uint32_t { Complete = 0, OpenOnly = 1 };      // F: OpenOnly hands H = L = C = open, volume 0
// native_host.hpp
enum class NativeCalculationReason : std::uint8_t { BarClose, OrderFill, Tick, SubBar };                // S
virtual void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx, NativeCalculationReason,
                                   const native_order::ExecutionAppliedEvent* cause) { on_native_bar(bar, ctx); }   // O + S
virtual void on_native_sub_bar(const Bar& sub, const NativeDecisionContext&) {}    // F: after each lower-TF sub-bar's path; covers CT8, HT3
std::optional<Bar> current_partial_bar() const;                                     // F: O/H/L/C up to the cursor; non-virtual
```

Callback chronology contract (the normative part of the lane; F P4.1 + O G8):

1. At one point: match and settle → `on_native_applied` per applied event, FIFO (rule already at native-engine.md:240-247) → with `BarCloseAndFills`, one recalculation at the fill cursor (reason `OrderFill`), driven from the existing notification drain (native_execution_consumer.cpp:4146-4158) under its re-entrancy guard, bounded by `max_recalculations_per_point`. S would run it *before* the drain; not chosen, the drain order is frozen (E4).
2. Requests born in any of these callbacks follow the existing birth rule: eligible on the unconsumed rest of the bar (native_execution_consumer.cpp:3248-3257). `born_on_remaining_path` (native_execution_consumer.cpp:3253-3257) is not touched — the adapter's COOF deferral (`pending_coof_requests_`, element type `PendingCoofRequest` pine_adapter.hpp:983-990) is calibrated against it.
3. `EveryModeledPoint`: a calculation at each magnifier sample / observed print, in batch as well as stream (delivery loops native_execution_consumer.cpp:4457-4599, native_execution_consumer.cpp:4382-4438). `on_native_tick` stays the observation hook (S).
4. `current_partial_bar()` derives from the driver points already recorded (`record_driver` native_execution_consumer.cpp:1753); valid in bar-open, applied and tick callbacks.
5. The kernel never attempts language-state rollback; that stays Pine's (`snapshot / restore / commit_coof_script_state`, pine_scheduler_native.cpp:53-104) (O).

### A.5 L6 — native HTF subscriptions (F §2.iv + O G10 + S P7; design in §2.iv)

```cpp
// native_run_spec.hpp — hashed only when non-empty
struct NativeTimeframeSubscription {
    std::string tf;                        // must pair with input_tf like script_tf does (native_calendar compatibility)
    std::vector<Bar> authoritative_bars;   // optional: replaces the aggregated OHLCV of a completed bucket (HT2)
    // no lookahead field until U1 is decided
};
std::vector<NativeTimeframeSubscription> subscriptions;
// native_host.hpp
struct NativeTimeframeBarContext { std::size_t subscription; native_calendar::NativeInterval interval;
                                   NativeCompletionKind completion; std::int64_t delivered_at_ms; };
virtual void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) {}    // F
std::optional<Bar> native_series_bar(std::size_t subscription) const;                     // O: legal in callbacks
```

- Not adopted: S's `gaps_on` / `lower_tf_array` request fields and `strategy_native_security_*` C symbols (C surface belongs to L13); O's and S's edits to `engine_security.cpp` (R5-6).
- TUs: `native_run_spec.{hpp,cpp}`, `native_execution_consumer.{hpp,cpp}`, `engine.hpp` (access only), `native_calendar` (pairing validation).

### A.6 L7 — order ergonomics, brackets, trailing (F P7 + O G4, G5, G12 + S P1)

```cpp
// native_host.hpp — non-virtual, additive
struct NativeWorkingRequest { native_order::DefinitionRef definition; native_order::RemainingProjection remaining;
                              native_order::TriggerState trigger_state; };
std::vector<NativeWorkingRequest> native_working_requests() const;   // F: owning snapshot (S: NativePendingView)
std::size_t cancel_all();                                            // F, O: one CancelledEvent per live request
std::size_t cancel_where(std::string_view label);                    // O: optional kernel label -> live-handle index (labels exist, native_order.hpp:141)
// native_order.hpp — native_order_v6, shared with L3
struct Absolute {};
struct FromOwnerFill { double offset = 0.0; bool ticks = false; };   // O: signed, adverse = negative
using TriggerAnchor = std::variant<Absolute, FromOwnerFill>;          // Request::anchor = Absolute{}
struct TrailTicks { double ticks; };                                   // O: offset spelling resolved against price_tick
struct ReplaceOptions { bool retain_trigger_state = false; };          // O: keeps TrailTrack::best (native_order.hpp:281-283)
// Trail::offset == 0.0 is accepted: "exit on the first adverse move past the best" (today the run fails, native_execution_consumer.cpp:3424-3429)

// include/pineforge/native_toolkit.hpp — header-only, Pine-free, covered by the include-independence checker (F; shapes from S P1)
struct BracketSpec    { native_order::RequestHandle parent; std::optional<native_order::Request> take_profit, stop_loss, trail;
                        native_order::GroupEffect sibling_effect = native_order::GroupEffect::Cancel; };
struct BracketReceipt { native_order::RequestHandle parent; std::optional<native_order::RequestHandle> take_profit, stop_loss, trail; };
BracketReceipt submit_bracket(NativeStrategyHost&, const BracketSpec&);   // emits exactly the owner / group shapes of OL10
template <class Key> class OrderBook;                                     // id -> handle: submit-or-replace, cancel-by-id
```

- A leg with owner `WaitForApplied` / `BindOpening` and a non-absolute anchor gets its level when the owner's fill arms it (`ArmedEvent` native_order.hpp:716-723 already exists) (O). Fallback with no kernel change: the toolkit materializes relative levels from the host's `on_native_applied` (F).
- Gap lane N13 (after R4d): two facts of the arm were missing and are now opt-in fields appended to `WaitForApplied`, each default byte-identical and folded only when set. `NativeArmFirstMatch { AtArmPrint, AfterArmPrint }`: whether the armed leg is a candidate at its owner's fill print or, like a request born in the owner's fill callback, only after it. `NativeArmScope { OwnerLot, Book }`: whether an armed closing leg closes the lot its owner opened or is bound at the arm to the whole position (a `BookClose`), under which it may be `HostSized`. Ruling: generic — both are broker models of a contingent child, neither names a source language — and they are what makes an anchored child the very request a host would submit from `on_native_applied`. The Pine adapter's relative `strategy.exit` legs use both; R4d's owner-lot spelling left a later same-id add open where the source leg closes the book (`rel-pyramid-set-once`, tests/test_adapter_brackets_relower.cpp).
- Not adopted (R5-2): S's kernel bracket object, its C symbols `strategy_native_bracket_*` and its TU list (`engine_orders.cpp`, `engine_execution.cpp`).
- TUs: `native_order.{hpp,cpp}` (arming), `native_matching.hpp` (`checked_trail_stop`), `native_execution_consumer.cpp` (`prepare_owner_applied` path, command surface).

### A.7 L8 — price grid (O G1)

```cpp
// native_run_spec.hpp
enum class NativePriceGrid : std::uint32_t { None = 0, QuantizeFills = 1, QuantizeFillsAndTriggers = 2 };
enum class NativeGridRounding : std::uint32_t { HalfUp = 0, Directional = 1 };
NativePriceGrid price_grid = NativePriceGrid::None;
NativeGridRounding grid_rounding = NativeGridRounding::HalfUp;
```

- `QuantizeFills`: the kernel rounds `resolved_price` onto `price_tick` **before** slippage (`HalfUp`) or in the direction that favours the resting order (`Directional`). `QuantizeFillsAndTriggers`: the matcher additionally compares the quantized path against the raw level.
- TUs: `native_run_spec.cpp` (validation), `native_execution_consumer.cpp` (resolve path native_execution_consumer.cpp:3671-3697, `execute_current` path native_execution_consumer.cpp:3849-3859), `native_matching.hpp` (threshold form). The dead helpers engine.hpp:1338, engine.hpp:1348, engine.hpp:1388 are the reference arithmetic (§2.ii e).

### A.8 L9 — risk limits (F P3 + O G7 + S P5)

```cpp
struct NativeLossLimit { double value = 0.0; bool percent = false; };                          // F
enum class NativeRiskDay    : std::uint32_t { SessionDay = 0, CalendarDayInTimezone = 1 };     // O
enum class NativeRiskAction : std::uint32_t { BlockOpenings = 0, FlattenAndBlock = 1 };        // O
struct NativeRiskLimits {
    std::optional<NativeLossLimit> max_drawdown, max_intraday_loss;
    std::optional<std::uint32_t>  max_consecutive_loss_days, max_fills_per_day;
    NativeRiskDay day_basis = NativeRiskDay::SessionDay;
    NativeRiskAction action = NativeRiskAction::BlockOpenings;
};
std::optional<NativeRiskLimits> risk;    // NativeRunSpec
```

- Breach: `MatchRejectReason::RiskLimit` for openings (O; F: `RiskHalted`), an optional kernel-issued flatten through A.3's kernel-originated request (`origin = KernelRisk`, F), and a `NativeRiskEvent` in `native_events` (native_host.hpp:502) (O) or a callback `on_native_risk_event` (S — a virtual, so only inside an epoch batch). The day key uses the existing calendar (native_calendar.hpp:363-366), not a chart clock.
- S's `directions` / `max_units` / `max_position_units` are existing spec fields (MG1-MG3), not duplicated.
- Interim (F): a C++ host can build all four today from `native_marked_equity` + `on_native_applied`, which is what the adapter does (pine_adapter.cpp:11387-11430); a toolkit helper is an acceptable stand-in until L9.
- TUs: `native_run_spec.{hpp,cpp}`, `native_execution_consumer.cpp` (`admit_opening_inspect` native_execution_consumer.cpp:1982-2015 and the per-point walk), `native_order.hpp`, the state hash.

### A.9 Single-source proposals not adopted

| Proposal | Src | Why not now |
|---|---|---|
| `NativeExcursionMode { PathExtremes, FillOnly, HostOwned }` + per-lot sample events | S P8 | The kernel sampler + A48 ownership already cover RP1 (F, O); no input found a gap |
| `NativeFxEpoch` + `on_native_fx_epoch` (stream-safe FX) | S P10 | No R5 lane; F rules it out of scope → U4 |
| Generic `FillModel` interface | S §2.2, lane G | R5-3 + R5-1 (§2.ii n) |
| TA `ComparePolicy` split | S §2.2 | not neutral (§2.ii b) → U6 |
| Kernel bracket object + close-reservation modes `ReplaceByKey` / `Fifo` | S P1, P2 | R5-2, R5-4 |
