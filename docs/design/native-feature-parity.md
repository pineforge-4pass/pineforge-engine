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
| R5-2 | **Brackets are not a new kernel object.** `WaitForApplied{RequestHandle parent}` (native_order.hpp:410-412) and `waiting_children(parent)` (native_order.hpp:1689, *fresh*) exist; L7 delivers a builder/toolkit over them plus relative anchors and the missing offsets. Size M. |
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
| OL1 | Market / limit / stop / stop-limit / trail triggers (`trigger_for` pine_adapter.cpp:1871-1882; `entry()` pine_adapter.cpp:3682, `order()` pine_adapter.cpp:8796) | **yes** — `Trigger` native_order.hpp:296-315; geometric matching `first_region_entry` native_execution_consumer.cpp:5442-5492; `submit` native_host.hpp:1154 | K | — | F:A1 O:A1 S:O2 | — |
| OL2 | Intents: flatten, reduce by units, reduce owner-opened units, transact, reverse-to (`close_all()` pine_adapter.cpp:6657 → `Flatten{}` pine_adapter.cpp:6686-6687; `Reduce{ExplicitUnits}` pine_adapter.cpp:5812-5821; `ReverseTo` pine_adapter.cpp:4646-4647) | **yes** — `OrderIntent` native_order.hpp:229, reduce sizes native_order.hpp:229-236, `ReverseTo` native_order.hpp:60-62, `OpeningShape` native_order.hpp:830-834, `Flatten` native_order.hpp:36 | K | — | F:A4 F:A8 O:A2 O:A3 S:O1 | — |
| OL3 | Amend / cancel one working order (`submit_or_replace` pine_adapter.cpp:1841-2366 → `host.replace` pine_adapter.cpp:1857-1923) | **yes** — `replace` native_host.hpp:1159-1161, `cancel` native_host.hpp:1185; `ReplacedEvent` native_order.hpp:933-943; result and event types `SubmitStatus` native_order.hpp:771-797, `ReplacedEvent` native_order.hpp:917-979 | K | — | F:A2 O:A4 S:O3 | id → handle map is host bookkeeping (F) → OL4 |
| OL4 | Cancel all / cancel by label; label → handle book (`cancel(id)` pine_adapter.cpp:3144-3190, `cancel_all` pine_adapter.cpp:16046-16061; `live_by_source_key_` pine_adapter.hpp:1378; predecessor state hand-carried `projection_predecessor` pine_adapter.cpp:2505-2569) | **partial** — single-handle `cancel` native_host.hpp:1185; no enumeration (`WorkingRequestCore::live()` is consumer-internal native_order.hpp:1559); labels exist but are unindexed (`Request::label` native_order.hpp:459) **Closed:** `cancel_all` native_host.hpp:1203, `cancel_where(comment)` native_host.hpp:1208 and `cancel_where(text, NativeRequestField::Label)` native_host.hpp:1208 withdraw by predicate and answer how many left the book; `native_working_requests()` native_host.hpp:1199 is the enumeration; the id → handle book is `native_toolkit::OrderBook<Key>`, host-side and Pine-free (`tests/test_native_order_ergonomics.cpp`, `tests/test_native_anchored_legs.cpp`). | K+A | L7 | F:A3 F:A2 O:A5 O:A14 S:O3 | **Disagree:** F partial / O no (A5) + partial (A14) / S yes (folded into O3). R5-1: cancel-all / by-label is generic → kernel. TV state inheritance across a same-id re-issue stays A. The adapter's `strategy.cancel` / `strategy.cancel_all` keep their per-handle loops, measured by R5 lane F7 (audit M27): TradingView cancels in the SOURCE book, which holds commands of the same script call that have not reached the kernel yet (and a live `strategy.close` is labelled `__close__<id>` there), so at the same point `cancel_where(id, NativeRequestField::Label)` and `cancel_all()` withdraw 0 — the close flattens at the next open, the reversal lands — where the source cancels withdraw them (`tests/test_adapter_brackets_relower.cpp`, M27 rows); each withdrawn source handle is also retired one by one. |
| OL5 | Per-point fill capacity, partial fills, residual working quantity (Pine uses only `ImmediateRemaining` pine_adapter.cpp:2628; residual closes `enqueue_pooc_fifo_close` pine_adapter.cpp:5497-5848) | **yes** — `Capacity` / `PointBudget` native_order.hpp:353-357, `Remaining` native_order.hpp:531-553, allowances native_order.hpp:531-544 | K | — | O:A16 S:O4 | — |
| OL6 | Ownership: legs bound to a parent / opening(s) / cohort; close the lots of a given entry, FIFO or ANY (`owner_for_close` pine_adapter.cpp:3845-3880, `cohort_for` pine_adapter.cpp:1676; FIFO prefix computed adapter-side `source_fifo_prefix_openings` pine_adapter.cpp:5795-5812) | **yes** — `Owner` native_order.hpp:438-453, `Authority` native_order.hpp:594-595, `CohortClose` native_order.hpp:591-593, `SelectedExposure` native_order.hpp:599-603, `cohort_open` native_host.hpp:1218, `CancelReason::OwnerGone` native_order.hpp:869 | K | — | F:A6 O:A8 O:A9 S:O5 | FIFO *ordering* is host-computed (O) |
| OL7 | OCA cancel / reduce groups (`group_for` pine_adapter.cpp:1880-1886) | **yes** — `Group` / `GroupEffect` native_order.hpp:440-447, `CancelReason::Group` native_order.hpp:868-873, `ReservationReducedEvent` native_order.hpp:1088-1098 (deferred form native_order.hpp:1088-1098) | K | — | F:A12 O:A6 O:A7 S:O7 | — |
| OL8 | Immediate execution at the current point (`immediately=true`) | **yes** — `inspect_current_execution` / `execute_current` native_host.hpp:1053-1054 | K | — | F:A9 | (single-source row; O:F2 cites the same call) |
| OL9 | Birth eligibility: an order accepted at bar N cannot fill on N's already-delivered points | **yes** — `point_eligible` native_order.hpp:722-726, the generic birth record `OrderBirth` include/pineforge/order_birth.hpp:59, contract `docs/pages/native-engine.md` ("Acceptance is not a fill") | K+A | — | F:A13 O:A10 S:R4 | **Disagree:** F yes / O partial / S "no, TV quirk". R5-1: the generic core (birth cursor) is native; TV's bar-granular first-bar rule is the A half → OL14. |
| OL10 | TP/SL bracket per entry, armed before the entry fills, OCA between legs (`exit()` pine_adapter.cpp:11308; lowering `submit_one` pine_adapter.cpp:7349-7361, `BindCohort` pine_adapter.cpp:7871-7877, `submit_leg` pine_adapter.cpp:7895-7919; lifecycle `select_exit_suspension` exit_lifecycle.cpp:10-66) | **partial** — every primitive is native (`WaitForApplied{parent}` native_order.hpp:410-412, `Reduce{OwnerOpenedUnits}` native_order.hpp:220-224, `Member{…, Cancel}` native_order.hpp:442-449, `waiting_children` native_order.hpp:1689); no builder **Closed:** the builder is `native_toolkit::submit_bracket(host, BracketSpec)` — take-profit, stop-loss and trail as one parent's `WaitForApplied` children in one OCA group, with `anchor_rounding` and `visibility` knobs — and it adds no kernel behaviour (`tests/test_native_toolkit_bracket.cpp`). | K (toolkit) | L7 | F:A10 O:A8 S:O6 | **Disagree:** F "yes (primitives)" + toolkit / S partial + new kernel bracket object, lane size L and prerequisite for everything. R5-2: builder over the existing primitives, size M. |
| OL11 | Bracket levels relative to the parent's fill (profit / loss ticks; derived `profit_ticks` pine_adapter.cpp:6959-6962; `materialize_relative_exits` pine_adapter.cpp:15336-15377, pine_adapter.hpp:1228-1240) | **partial** — composable: the host computes levels in `on_native_applied` and submits; suffix eligibility makes that same-bar correct `born_on_remaining_path` native_execution_consumer.cpp:5267-5301. `Trigger` itself is absolute-only native_order.hpp:296 **Closed:** `Request::anchor = FromOwnerFill{offset, ticks, rounding}` (`TriggerAnchor` native_order.hpp:336) defers the level to the owner's fill, the kernel materializes it once at the arm under `NativeAnchorRounding`, and `resolve_anchored_level` native_host.hpp:990 is where a source language's own level arithmetic lives (`tests/test_native_anchored_legs.cpp`, `tests/test_native_arm_options.cpp`). | K | L7 | F:A11 O:A11 S:O6 | **Disagree:** F partial (toolkit is enough) / O no (kernel `TriggerAnchor`). R5-2: L7 delivers relative anchors; kernel anchor primary, toolkit materialization is the zero-kernel fallback. |
| OL12 | Working-order view for the host and outside observers (`strategy_pending_order_*`; `PendingIntentView` pine_adapter.hpp:694-710; the POD is filled only by the adapter `pine_birth_reach` pine_adapter.cpp:17089-17118) | **no** — base returns 0 / -1 `observe_last_bar_dual_entry_path_v1` engine_consumer.cpp:124-133; the POD is TV-shaped (`tv_carry_qty` pending_order_mirror.hpp:73, `pine_birth_reach` pending_order_mirror.hpp:135) **Closed for a bare host:** `native_working_requests()` native_host.hpp:1199 returns one owning `NativeWorkingRequest` native_host.hpp:669 per live request — definition, remaining, trigger state — with the C spelling `strategy_native_working_len_v1` / `_get_v1`. The TV-shaped POD stays frozen and stays the source layer's projection (`tests/test_native_limit_fill_through.cpp`, `tests/test_native_arm_options.cpp`, `tests/test_native_c_api.c`). | K+A | L7 (C++), L13 (C) | F:J5 O:J6 S:X4 | **Disagree:** O "partial + TV-shaped" / F, S no. A bare host reads zeros → no. R5-1: neutral view → kernel; POD shape and Pine journal retention stay A (POD frozen, §2.ii j). |
| OL13 | *(quirk)* Same-bar command batching and the seven deferred queues (`pending_bracket_legs_` pine_adapter.hpp:1380-1398); KI-62 open-order priority (`select` order_priority.cpp:13-60, selected `update_l4c_priority` pine_adapter.cpp:850-909); per-bar priced-entry throttle, BUY-before-SELL batch, short-seed plan / collision (`maybe_activate_short_seed_plan` pine_adapter.cpp:1929-2096, `materialize_snapshot` pine_adapter.cpp:9186-9210), reversal-gap bracket policy (`ShortSeedPlan` pine_adapter.hpp:607-632, `DeferredOpenMarketableSell` pine_adapter.hpp:1025-1048, `CloseCallsiteState` pine_adapter.hpp:1119-1134; `apply_reversal_gap_bracket_policy` pine_adapter.cpp:13818, `reaccept_gapped_bracket_behind_same_id_add` pine_adapter.cpp:14052) | n/a — native priority is acceptance / incarnation order `native_events` native-engine.md:624-626 | A | — | F:A15 O:A15 S:O9 S:R3 | R5-1 names KI-62. S: the kernel exposes deterministic insertion order only. |
| OL14 | *(quirk)* Exit activation / suspension / revival barriers, historical birth reach (`select_exit_activation` exit_activation.cpp:42-88, enforced pine_adapter.cpp:749-756; `select_exit_activation` exit_activation.cpp:18-88, `select_historical_birth_reach` order_birth.cpp:5-14, `capture_order_birth` pine_adapter.cpp:594-626) | n/a | A | — | S:R4 O:A10 | TV half of OL9 |

### 1.2 Trailing

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| TR1 | Trail arm → track → trigger, with a state query | **yes** — `Trail{offset, arm_price}` native_order.hpp:290-293; states `TrailWaitArm` native_order.hpp:616-624; arm / track / fire `TrailWaitArm` native_execution_consumer.cpp:5454-5479; `NativeTrailState` native_host.hpp:657-662, `trail_state` native_host.hpp:1041 | K | — | F:B1 O:B1 S:T1 | — |
| TR2 | Trail activation / offset in ticks or points, relative to the entry fill (`trail_points_to_ticks` include/pineforge/compat/pine/trail_ticks.hpp:39 + grid snap exit_lifecycle.cpp:22-27; `trail_activation_level` pine_adapter.hpp:273-275) | **partial** — absolute prices and a raw price distance only **Closed:** `TrailTicks{n}` (`Trail::ticks` native_order.hpp:293) is resolved against `NativeRunSpec::price_tick` at acceptance, so a tick-spelled trail and its price-spelled equal behave identically bar for bar; a relative arm threshold is the `FromOwnerFill` anchor of OL11 (`tests/test_native_order_ergonomics.cpp`, `tests/test_native_anchored_legs.cpp`). | K | L7 | F:B2 O:B2 S:T2 | **Disagree:** S classes all trail operand conversion as TV quirk. R5-1: tick / relative spelling is generic. |
| TR3 | Zero-distance trail, "ride the best, exit on any adverse tick" (adapter substitutes `tick * 0.5` pine_adapter.cpp:10068-10080) | **no** — the kernel fails the run when `offset <= 0` (`checked_trail_stop` → native_execution_consumer.cpp:4319-4324) **Closed:** a zero offset is legal and means *ride the best* — the level is the running best itself and the exit is the first print strictly past it; negative and nonfinite offsets stay rejected (`tests/test_native_order_ergonomics.cpp`, `tests/test_native_anchored_legs.cpp`). | K+A | L7 | O:B3 F:B3 S:T2 | **Disagree:** O kernel / F, S quirk. R5-1: accepting a zero offset is generic; TV zero-offset *pricing* (`zero_trail_sibling_stop` pine_adapter.cpp:9981-10118) stays A. |
| TR4 | Re-price a trail without losing the running extreme (adapter skips `replace` and reads `trail_state()` pine_adapter.cpp:2473-2508) | **partial** — the read exists `trail_state` native_host.hpp:1041; `replace` always rebuilds trigger state **Closed:** `replace(handle, request, ReplaceOptions{/*retain_trigger_state=*/true})` carries the predecessor's live trigger state — a tracking trail's best, an already active stop — into the successor, refusing the replacement when the retained best leaves no representable level (`tests/test_native_order_ergonomics.cpp`, `tests/test_native_anchored_legs.cpp`). | K+A | L7 | O:B4 F:B3 | Status is single-source (O). R5-1: retain-trail replace is generic; TV's "retained best across re-issue" convention (`retained_trail_best` pine_adapter.hpp:276-278) is A. |
| TR5 | *(quirk)* TV trail conventions: half-tick arm threshold against tick-quantized extremes `has_trail_request` pine_adapter.cpp:7921-7926, zero-offset pricing `zero_trail_sibling_stop` pine_adapter.cpp:9981-10118, retained best `retained_trail_best` pine_adapter.hpp:276-278, operand / restart rules `source_trail_points` pine_adapter.cpp:6929-7150, `trail_one_shot` pine_adapter.cpp:8010-8139 | n/a | A | — | F:B3 S:T2 | R5-1 |

### 1.3 Sizing bases and reservations

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| SZ1 | Explicit units (`Transact{signed}` after a grid floor pine_adapter.cpp:4235-4239, pine_adapter.cpp:4235) | **yes** — `Transact` in `OrderIntent` native_order.hpp:229 | K | — | F:C1 O:C1 S:S1 | — |
| SZ2 | Host override of the resolved price / units (`resolve_terms` pine_adapter.cpp:9221-10396) | **yes** — `HostSized` native_order.hpp:73-78 + `resolve_execution_terms` native_host.hpp:924-928, invoked native_execution_consumer.cpp:4703-4721, guarded native_execution_consumer.cpp:4703-4737 | K | — | F:C2 O:H9 S:S2 | **Disagree:** S "partial" (a hook is not a basis). The hook is native; bases are SZ3-SZ5. It stays the custom-policy seam after L3. |
| SZ3 | Percent-of-equity sizing (`default_sizing_units` pine_adapter.cpp:1770-1784) | **no** — `HostSized` without host units is `TermsUnresolved` native_execution_consumer.cpp:4744-4746 **Closed:** `Sized{Side, EquityFraction{f}}` (`OrderIntent` native_order.hpp:229) is the sixth intent — the kernel resolves the units from a fraction of marked equity at the sizing point, so a bare host needs no terms override (`tests/test_native_sizing_bases.cpp`). | K+A | L3 | F:C3 O:C2 S:S3 | TV parts stay A: rounded equity `source_money_round` pine_adapter.cpp:1722, money floor-to-lot `source_money_floor_lot` pine_adapter.cpp:443 (→ SZ10) |
| SZ4 | Cash-value sizing (`default_qty_type` pine_adapter.cpp:1731-1733) | **no** — same **Closed:** `Sized{Side, CashValue{cash}}`, the same resolution over an account-currency amount (`tests/test_native_sizing_bases.cpp`). | K | L3 | F:C2 O:C3 S:S4 | — |
| SZ5 | Fee-net sizing (commission reserve `/(1+c)`, asked of the kernel as `reserve_percent_fee` pine_adapter.cpp:1762; `percent_commission_live_equity` pine_adapter.cpp:2759-2791) | **no** — only a dead primitive, `reserve_percent_commission` (since deleted), with no caller in `src/` **Closed:** `Sized::reserve_percent_fee` divides the sizing cash by `1 + fee_value / 100` under `NativeFeeKind::Percent` and is an exact no-op for every other fee kind; the dead `reserve_percent_commission` primitive is gone (`tests/test_native_sizing_bases.cpp`, `tests/test_native_sizing_price_rule.cpp`). | K | L3 | F:C3 O:C6 S:S3 | **Disagree:** F lists the reserve under quirk yet offers a knob / O kernel, "partial" / S knob. R5-1: fee-net option is generic. |
| SZ6 | Placement-time vs fill-time sizing (`sizing_snapshot` pine_adapter.cpp:1688-1703, `PineSizingSnapshot` pine_adapter.hpp:142-149; fill-time exceptions `default_sized` pine_adapter.cpp:4967-4993) | **no** **Closed:** `SizeTime::AtMatch` (default) resolves at the matching candidate and `SizeTime::AtAcceptance` freezes the units against the command point, where the run's own opening admission also runs against the frozen quantity (`RequestRejectReason::PlacementAdmission`). Which price the basis converts at is `SizePrice::{Resolved, Signal, SignalOnTick}` (`tests/test_native_sizing_bases.cpp`, `tests/test_native_sizing_price_rule.cpp`). | K+A | L3 | F:C3 O:C7 S:S3 | **Disagree:** O adapter-only / F, S generic timing knob. R5-11 (L3 scope) + R5-1: the knob is kernel; TV's frozen tuple stays A through `HostSized`. |
| SZ7 | Quantity step | **yes** — `quantity_grid` admission native_run_spec.hpp:562, native_order.hpp:1307-1324; host-sized terms snap (`ExecutionGridPolicy` native_order.hpp:71-85); exact grid validation `execution_terms_grid_representable` native_execution_consumer.cpp:4758-4761 | K | — | F:C4 O:C5 S:S1 | **Disagree:** F, O partial / S yes. R5-3: the quantity grid is native already. L3's `Sized` intent reuses the snap policy. |
| SZ8 | Fractional reduce of the position / a cohort (`quantize_close_units` pine_adapter.cpp:10767-10801, `compute_exit_reservation` pine_adapter.cpp:2834-2954) | **no** — sizes are `ExplicitUnits` / `OwnerOpenedUnits` / `HostSized` only native_order.hpp:60-74 **Closed:** `Reduce{ScopeFraction{fraction, claim, basis}}` takes a fraction in `(0, 1]` of the scope the reduce is bound to — the book, one opening, or a cohort roster (`tests/test_native_sizing_bases.cpp`). | K | L3 | F:A7 O:A12 O:C4 S:S5 | F's "partial" counts the units-reduce of OL2. R5-4 → L3. |
| SZ9 | Exit-quantity reservation against sibling exits (`compute_exit_reservation` pine_adapter.cpp:2885-3208, `reserved_exit_qty` pine_adapter.cpp:7276-7591) | **partial** — `PointBudget` native_order.hpp:353-356 and group `Reduce` native_order.hpp:220 give a generic reservation; no net-of-siblings claim for fraction sizes **Closed:** `ScopeClaim::NetOfSiblings` subtracts the units already claimed by live sibling reduces bound to the same scope, and `ScopeBasis::{AtMatch, AtAcceptance}` chooses when that scope is measured (`tests/test_native_sizing_bases.cpp`). | K+A | L3 | F:A14 S:S5 S:O10 O:A12 | F proposed nothing. R5-4: generic half → L3; the Pine ledger → SZ11, SZ12. |
| SZ10 | *(quirk)* Ten-significant-digit money rule (`source_money_round` pine_adapter.cpp:434-441, `source_money_floor_lot` pine_adapter.cpp:443; twin `tv_money_round` pine_policy_support.hpp:9-15); exact percent-exit rounding (`quantize_percent_exit_units` pine_adapter.cpp:2865-2880) | n/a — kernel money is plain binary64 `marked_equity` engine_execution.cpp:1023-1042 | A | — | O:C8 F:C3 S:M4 S:S5 | R5-1 |
| SZ11 | *(quirk)* Same-bar `strategy.close` callsite batching, two-call provenance, entry-id ledger (`enqueue_pooc_fifo_close` pine_adapter.cpp:5497-5848; ledger `close_batch_bar_` pine_adapter.hpp:1417-1428; the kernel's comment-only residue was deleted by R5 lanes E6 and E10) | n/a | A | — | S:O8 F:A15 O:A15 | **Disagree:** S "generic → kernel" (its P2 `ReplaceByKey` / `Fifo`). R5-4: adapter. |
| SZ12 | *(quirk)* POOC reservation-growth market-entry population predicate (`select_reservation_growth_sources` src/compat/pine/reservation_expansion.cpp:9-34, called from pine_adapter.cpp:808-900) | n/a | A | — | S:O10 | (single-source) R5-4 confirms S's own split: concept → SZ9, predicate A. |

### 1.4 Margin, admission, liquidation, risk

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| MG1 | Allowed opening directions (`allowed_open_directions` pine_adapter.cpp:1541) | **yes** — `allowed_open_directions` native_run_spec.hpp:567 → `MatchRejectReason::OpeningDirection` native_execution_consumer.cpp:2939-2947 | K | — | F:D4 O:D7 S:M1 | the one risk rule the adapter delegates (O) |
| MG2 | Max absolute position size | **yes** — `max_abs_units` native_run_spec.hpp:565 tests the resulting book native_execution_consumer.cpp:2953-2956 | K+A | — | F:D5 O:D5 S:M1 S:M5 | **Disagree:** O, S:M5 partial. R5-1: the generic cap is native; the adapter's live-position `>= held` variant (`max_position_size` pine_adapter.cpp:11530-11533) is TV. |
| MG3 | Open-lot cap (pyramiding) | **yes** — `max_open_lots` native_run_spec.hpp:566 → `MaxOpenLots` native_execution_consumer.cpp:2958-2961 | K+A | — | F:A5 O:A13 O:D6 S:M1 | **Disagree:** F, O:A13 partial / O:D6, S yes. R5-1: Pine's per-cycle entry count (`accepted_in_cycle` pine_adapter.cpp:4502-4548, cap left unset `max_open_lots` pine_adapter.cpp:1540-1542) is TV. |
| MG4 | Initial-margin opening gate (adapter answers `AdmitWithHostMargin` from frozen signal-time equity pine_adapter.cpp:11584-11649 and leaves the kernel gate unset pine_adapter.cpp:11583-11585) | **yes** — `initial_margin_fraction` native_run_spec.hpp:568; the requirement `resulting_abs_notional * fraction` native_execution_consumer.cpp:2976 against marked equity. **Conversion instant, R5 follow-up lane E21:** both sites of the gate convert at their own point's rate — the candidate at its cursor, the placement gate at the acceptance point where a `Sized{SizeTime::AtAcceptance}` freezes its units — for the notional, the equity and a percent ticket, and the freeze's equity basis with them. The rate is threaded, never the clock: `make_command_context` native_execution_consumer.cpp:2835 hands the point's rate to the freeze, to `marked_equity_at` and to the engine's rate-explicit settlement inspection `inspect_native_settlement_scoped_at` engine.hpp:645, and the engine's presented clock is never written (`tests/test_native_margin_fx_clock.cpp` sections 8, 10 and 11) | K | — | F:D1 O:D1 S:M2 | **Disagree:** F, O partial (they fold per-side in) / S yes. Row split → MG5. |
| MG5 | Per-side initial margin (`margin_pct` pine_adapter.cpp:12184-12185, `margin_pct` pine_adapter.cpp:11647) | **no** — one `std::optional<double>` `initial_margin_fraction` native_run_spec.hpp:568 **Closed:** `NativeMarginModel::initial_long` / `initial_short` native_run_spec.hpp:241, opt-in and mutually exclusive with `initial_margin_fraction`; `initial_<side> == 0.0` is the maintenance-only spelling that leaves opening admission to the host at both of its gates (`tests/test_native_margin_model.cpp`). | K | L4 | F:D1 O:D2 | R5-1 |
| MG6 | Maintenance margin + forced liquidation (`submit_margin_call_slice` pine_adapter.cpp:12284-12462, `submit_margin_call_units` pine_adapter.cpp:12423-12469, `schedule_margin_call_path` pine_adapter.cpp:16194-16271) | **no** — "no maintenance liquidation" `initial_margin_fraction` native_run_spec.hpp:568-569; margin is checked only when `inspect.would_open` native_execution_consumer.cpp:2935 **Closed:** `maintenance_long` / `maintenance_short` with `NativeLiquidationCheck::{PathAdverseExtreme, PathAdverseExtremeMark, CalculationOnly}`; the kernel solves the level, rests its own `Reduce`/`Flatten` bound to the live book, re-prices it with `CancelReason::Superseded` and books a `MarginCallEvent` (`tests/test_native_margin_model.cpp`, `tests/test_native_margin_hooks.cpp`). | K+A | L4 | F:D2 O:D3 S:M3 | TV parts → MG15 |
| MG7 | Liquidation slice sizing (grid floor, ×4 restore, one-contract fallback `source_margin_units` pine_adapter.cpp:12215-12251) | **no** **Closed:** `NativeLiquidationSizing::{RestoreMinimum, ShortfallMultiple, Flatten}` with `shortfall_multiple` and `liquidation_min_units`, capped at the held quantity and floored onto `quantity_grid`; `resolve_margin_call_units` native_host.hpp:973 gives a host the last word, clamped into `(0, held]` (`tests/test_native_margin_model.cpp`, `tests/test_native_margin_hooks.cpp`). | K+A | L4 | O:D4 F:D2 S:M4 | **Disagree:** F generic knob `ShortfallMultiple` / O "×4 is A" / S quirk, yet a 4.0 default inside its generic model. R5-1: policy knob in the kernel, plain default (restore-minimum). **Decided by R5 lane F7 (the D2 question):** the adapter selects NO kernel knob — `project()` leaves `sizing` / `shortfall_multiple` / `liquidation_min_units` unset and its `resolve_margin_call_units` answers every call with TradingView's slice (the restore floored onto the lot grid BEFORE the ×4, floored again, the one-contract band, the dust gate). Measured: on a one-unit grid, 20 long at 100 on 1000 at 50 % reaching 93.9, TradingView books 4 lots and the kernel's own `ShortfallMultiple 4` books 5 (`tests/test_native_margin_hooks_twin.cpp` MG-F3, an executed run, no longer a comment); without a grid the two agree bit for bit (MG-F/H, 4.2105263157894735), but declaring the knob for those runs alone folds `NativeMarginModel::sizing` into every margin run's identity — an epoch decision F7 does not take. |
| MG8 | Liquidation price query (`compute_liquidation_price` pine_strategy_host.cpp:162-183) | **no** **Closed:** `native_liquidation_price()` native_host.hpp:1256 answers the exact solved level, `nullopt` where none exists; `NativeLiquidationLevelBase` chooses whether the solve answers for the open entries' fees (`tests/test_native_margin_model.cpp`, `tests/test_native_open_lots.cpp`). | K+A | L4 | F:D3 S:R5 | **Disagree:** F generic / S adapter-only. R5-1: liquidation price is generic; the Pine spelling stays A. |
| MG9 | FX revaluation of margin (`apply_fx_open_margin_slice` pine_adapter.cpp:14373-14389, `apply_fx_opening_margin_slice` pine_adapter.cpp:16272-16300) | **no** — staged FX curve `configure_native_fx_curve` native_host.hpp:1143 but no margin re-check. **Closed by audit lane N6:** `NativeMarginCheckKind::FxRoll` — a step of the declared curve is a check point of its own, measured at the unchanged price before the first point the new rate converts (`tests/test_native_margin_fx_roll.cpp`). The adapter refuses the point by kind, for a measured reason since R5 lane F7: TradingView's rollover is a broker-open checkpoint at the open price, now at any positive margin (`apply_fx_open_margin_slice`, on the one TradingView money / units rule every other checkpoint uses), while the roll point measures at the remaining path's adverse mark and rests the slice there — a carried 1x long on a step bar O 100 L 97 books 0.4116 @ 97 on the adapter's run shape and 0.4036 @ 99, measured a bar early, on a strict calendar, against TradingView's 0.3996 @ 100 (`tests/test_native_margin_hooks_twin.cpp` MG-FX). Both N6 follow-ups are closed (§4.4). **Completed by R5 follow-up lane E3:** the conversion INSTANT is now generic too — every check kind converts at its own cursor's rate (`account_currency_fx_at(cursor.point.effective_time_ms)`) for the requirement, the equity and the level, in batch and stream, so a step landing between a fill's cursor and the clock the engine presents no longer breaches at the fill (`tests/test_native_margin_fx_clock.cpp`; N6's local clock swap inside `fx_roll_margin_check` is removed as redundant). **Extended by R5 follow-up lane E21** to the opening gate's placement site (MG4), and the rate-explicit equity E3 restated beside the engine's accessor is now that accessor's one implementation, `BacktestEngine::marked_equity_at` engine.hpp:708 | K | L4 → N6 → E3 → E21 | O:D8 | (single-source) |
| MG10 | Max drawdown halt (`update_risk_state` pine_adapter.cpp:14797-14824) | **no** — observation field only `drawdown_limit` include/pineforge/source/market_admission.hpp:50; composable today from `native_marked_equity` native_host.hpp:1237 **Closed:** `NativeRiskLimits::max_drawdown` native_run_spec.hpp:310, a `NativeLossLimit{value, percent}` measured against the running peak equity at three points of every script bar, blocking openings for the rest of the run (`tests/test_native_risk_limits.cpp`). | K | L9 | F:D6 O:E1 S:M6 | R5-9 |
| MG11 | Max consecutive losing days (`SourceDayLedger` pine_adapter.hpp:651-659; gate `max_cons_loss_days` pine_adapter.cpp:11619-11622) | **no** — observation only `loss_days_limit` include/pineforge/source/market_admission.hpp:49 **Closed:** `NativeRiskLimits::max_consecutive_loss_days`, settled from each day's realized result at the next day's open (`tests/test_native_risk_limits.cpp`). | K | L9 | F:D6 O:E2 S:M6 | R5-9 |
| MG12 | Max intraday loss → flatten + block (`intraday_loss_breached` pine_adapter.cpp:12145-12159, forced flatten `submit_intraday_loss_close` pine_adapter.cpp:13117-13147) | **no** — observation only `intraday_loss_limit` include/pineforge/source/market_admission.hpp:51 **Closed:** `NativeRiskLimits::max_intraday_loss` with `NativeRiskAction::FlattenAndBlock`, under which the kernel issues one `Flatten` of its own at the breach point (`RequestOrigin::KernelRisk`) and latches the block before that flatten settles (`tests/test_native_risk_limits.cpp`). | K | L9 | F:D7 O:E3 S:M6 | R5-9 |
| MG13 | Max filled orders per day (`IntradayCap` intraday_cap.hpp:83-241, `IntradayOrderBudget` intraday_order_budget.hpp:27-92; `execute_due_cap_close` pine_adapter.cpp:13160-13394) | **no** **Closed:** `NativeRiskLimits::max_fills_per_day` counts every applied execution of the day as it settles, whatever issued it (`tests/test_native_risk_limits.cpp`). | K+A | L9 | F:D8 O:E4 S:M7 | Count cap K; quota transfer, POOC deferral, TV close price A |
| MG14 | Risk-day boundary (`chart_day_key` pine_adapter.cpp:12010-12034) | **no** — the calendar exists `session_day_ordinal` native_calendar.hpp:373-376, there is no risk-day ledger **Closed:** `NativeRiskDay::{SessionDay, CalendarDayInTimezone}` keys the risk day, and `native_risk_state()` native_host.hpp:1259 reads the whole ledger back — block and reason, day, fills counted, loss-day streak, peak equity, day-opening equity (`tests/test_native_risk_limits.cpp`). | K+A | L9 | O:E5 S:M7 | The chart-day shortcut stays A; the kernel keys on session / calendar day |
| MG15 | *(quirk)* TV margin call: money rounding `source_margin_money` pine_adapter.cpp:12173, 4× shortfall `4.0 * minimum` pine_adapter.cpp:12232, POOC chronology exceptions `competing_entry` pine_adapter.cpp:12886-12926, 1×-long money call `submit_tv_money_long_margin_call` pine_adapter.cpp:12464-12576, fill pricing (the slice books at its waypoint's mark, `submit_tv_money_long_margin_call` pine_adapter.cpp:12464) | n/a | A | — | F:D2 F:G4 O:D4 S:M4 | R5-1 |
| MG16 | *(quirk)* Historical market-admission review / retention journal (`explicit_pair_scope` src/compat/pine/market_admission.cpp:33 and the review fold it feeds (`original_pair_call` src/compat/pine/market_admission.cpp:47); `admission_allocation` pine_adapter.cpp:2278-2345, `record_market_review` pine_adapter.cpp:13398-13456) | n/a | A | — | S:R2 | (single-source) |

### 1.5 Calculation timing

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| CT1 | Calculate at script-bar close | **yes** — `on_native_bar` native_host.hpp:873, dispatched native_execution_consumer.cpp:6668-6690 (call site native_execution_consumer.cpp:6680) | K | — | F:E1 O:F1 | — |
| CT2 | Process orders on close (`close_execution` pine_adapter.cpp:1498-1499) | **yes** — `NativeCloseExecution::AfterCalculation` native_run_spec.hpp:35-38; `AfterCalculation` native_execution_consumer.cpp:6959-6963 | K | — | F:E2 O:F6 S:C1 | — |
| CT3 | Pre-match hook at bar open (`on_bar_open` pine_adapter.cpp:14284) | **yes** — `on_native_bar_open` native_host.hpp:869 runs before the open match native_execution_consumer.cpp:6526-6530; `execute_current` legal native_host.hpp:1053 | K | — | F:E3 O:F2 | hazard → CT4 |
| CT4 | Partial-bar view / open-bar lookahead guard | **no** — the open hook receives the *complete* script bar: `invoke_bar_open_callback(engine, bar, point)` native_execution_consumer.cpp:6483 with `engine.current_bar_ = open_view` native_execution_consumer.cpp:6520 (the complete bar unless `OpenOnly`) **Closed:** `current_partial_bar()` native_host.hpp:1022 is the lookahead-free bar so far at the cursor, and `NativeOpenBarView::OpenOnly` native_run_spec.hpp:108 masks `on_native_bar_open`'s bar to `H = L = C = open` as presentation only — the complete bar is restored before the open match (`tests/test_native_calc_timing.cpp`). | K | L5 | F:E3 | (single-source) The adapter needs the full bar, so the guard is opt-in. |
| CT5 | Raw input observation before aggregation | **yes** — `on_native_input` native_host.hpp:856, `NativeInputContext` native_host.hpp:754-759 | K | — | O:F3 | (single-source row) |
| CT6 | Calculate on order fills (scheduler `PineScheduler::recalculate` pine_scheduler_native.cpp:582, `coof_recalculation_due` pine_scheduler_native.cpp:557, the first-open chain `kFirstOpenLoopGuard` pine_scheduler_native.cpp:683; `begin_coof_recalc` pine_adapter.cpp:3942, `flush_coof_tail` pine_adapter.cpp:14360, `end_coof_recalc` pine_adapter.cpp:3963) | **partial** — `on_native_applied` native_host.hpp:917 fires mid-path after each fill native_execution_consumer.cpp:6402, native_execution_consumer.cpp:6166-6187; commands are legal there `commands_allowed` native_execution_consumer.cpp:1521-1529; a request born there is eligible on the unconsumed rest of the bar `born_on_remaining_path` native_execution_consumer.cpp:5267-5301, `remaining_path_delivery` native_order.cpp:1704-1712. Missing: calculation re-entry, a documented chronology, a bar-so-far view **Closed:** `NativeCalculationTrigger::BarCloseAndFills` native_run_spec.hpp:95 drives one `OrderFill` recalculation per applied execution at that event's own cursor, delivered as `on_native_recalculate(..., OrderFill, cause)` and bounded by `max_recalculations_per_point` (`tests/test_native_calc_timing.cpp`). | K+A | L5 | F:E4 O:F5 S:C2 | **Disagree:** F partial / O, S no; F, S v1 / O deferrable. R5-5: v1, sub-bar hook + chronology contract, adapter path unchanged. TV parts → CT11. |
| CT7 | Calculate on every tick | **partial** — `on_native_tick` native_host.hpp:859 fires per accepted realtime print native_execution_consumer.cpp:6612-6616; no per-tick calculation in batch; the guide pinned "close-only" (`docs/pages/native-engine.md`, "Batch OHLCV vs ticks vs quiet") **Closed:** `NativeCalculationTrigger::EveryModeledPoint` native_run_spec.hpp:96 adds one `Tick` recalculation at every modeled point of the delivered path and at every observed print, in batch and in a stream alike (`tests/test_native_calc_timing.cpp`). | K | L5 | F:E5 O:F4 S:C3 | **Disagree:** F yes / O partial / S no. R5-5: v1. Not an adapter feature either: absent from `PineStrategyConfig` pine_adapter.hpp:57-71, the runner rejects it (`calc_on_every_tick` main.cpp:213), the Pine stream route refuses `calc_on_order_fills` pine_strategy_host.cpp:267-269. |
| CT8 | Historical intrabar calculation (per lower-TF sub-bar / magnifier sample) | **no** — with an `IntrabarPath` the callback still fires once per script bar native_execution_consumer.cpp:6953-7053 **Closed:** `EveryModeledPoint` recalculates at each intrabar sample, and `on_native_sub_bar` native_host.hpp:907 fires once after each retained lower-timeframe sub-bar's whole matching path (`tests/test_native_calc_timing.cpp`). | K | L5 | F:E6 O:F8 S:C4 | — |
| CT9 | Intrabar (magnifier) matching path + path-order policy (`volume_weighted_cap` pine_adapter.cpp:1628-1679) | **yes** — `IntrabarPath` native_run_spec.hpp:361-401, delivery native_execution_consumer.cpp:6897-7069; `NativePathOrder` native_run_spec.hpp:334-338 | K+A | — | F:E7 F:G5 O:F7 S:C4 | **Disagree:** S partial (it wants callback cadence → CT8). TV sampler choice is A. |
| CT10 | Batch → warmup → realtime forward execution | **yes** — `stream_begin` engine.hpp:1722, `stream_push_bar` engine.hpp:1728, `stream_end` engine.hpp:1732, `NativeRunPhase` native_host.hpp:41, C ABI `strategy_stream_begin` c_abi.cpp:562 | K+A | — | F:E8 O:J3 S:C5 | **Disagree:** S partial. R5-1: the lifecycle is native; Pine trade-start suppression and realtime-tail / probe rules (`trading_window_active` pine_strategy_commands.cpp:15-21, `prepare_native_begin` pine_strategy_host.cpp:216-309) are A. S's "neutral tail + trade-window policy" is single-source and unassigned (§4 Q8). |
| CT11 | *(quirk)* COOF TV specifics: waypoint-only refill (`coof_next_waypoint` pine_adapter.cpp:4090), two-fills-at-open rule, script-state rollback (`PineScheduler` pine_scheduler_native.cpp:53-104; hooks `snapshot_script_state` engine.hpp:1299-1300), the first-open guard `kFirstOpenLoopGuard` pine_scheduler_native.cpp:683, deferral queue of `PendingCoofRequest` pine_adapter.hpp:1100-1107 | n/a | A | — | F:E4 O:F5 S:C2 | R5-5: the adapter keeps its path |

### 1.6 Higher timeframes and auxiliary feeds

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| HT1 | HTF series of the same symbol (`request.security`): evaluators registered `scheduler_configure_security_evaluators` pine_strategy_host.cpp:1504-1507, pumped from the scheduler `scheduler_prepare_security_sequence` pine_strategy_host.cpp:1403-1559; state lives in `BacktestEngine` engine.hpp:352-554, engine.hpp:352-398, engine_security.cpp:26 | **no** sanctioned path — `configure_security_evaluators` is an empty virtual engine.hpp:1289 whose only caller is pine_strategy_host.cpp:1505 **Closed:** `NativeRunSpec::subscriptions` (`NativeTimeframeSubscription` native_run_spec.hpp:484), or `declare_timeframe_subscriptions` native_host.hpp:1077 from inside `on_native_run_begin`, with `on_native_timeframe_bar` native_host.hpp:866 and `native_series_bar(index)` native_host.hpp:1060 reading them back; the kernel registers the declared series after that callback returns, so a host's own evaluators survive (`tests/test_native_htf_subscriptions.cpp`, `tests/test_native_htf_subscriptions_stream.cpp`). | K | L6 | F:F1 O:G-1 S:H1 | **Disagree:** S partial (it counts kernel-side storage → HT2); F, S v1 / O deferrable. R5-6: v1. |
| HT2 | Authoritative (exchange) HTF bars substituted into a series (`set_native_security_feed` engine.hpp:1697, engine_aux_security.cpp:78-129, C ABI c_abi.cpp:799) | **partial (inert)** — pre-run staging is accepted `refuse_source_mutation` native_execution_consumer.cpp:1556-1572 but never consumed: `prepare_native_security_feeds` is protected engine.hpp:1606, sole caller pine_strategy_host.cpp:1537; in-run it latches `UnsupportedSource` native_execution_consumer.cpp:1574-1579 **Closed:** a bare host's subscriptions consume authoritative bars: `NativeTimeframeSubscription::authoritative_bars` native_run_spec.hpp:486 is staged into the feed store by the kernel's own `prepare_native_security_feeds` native_execution_consumer.cpp:7654 call at begin, so a declared series is the venue's own bars (`tests/test_native_htf_subscriptions.cpp`). | K | L6 | F:F2 O:G-2 S:H1 | F says "no" on the same facts. "native" in these names means TradingView's native-timeframe bars, not the native host (`never from the chart's intraday prints` engine_aux_security.cpp:49). |
| HT3 | Lower-timeframe arrays (`request.security_lower_tf`; `register_security_lower_tf_eval` include/pineforge/source/pine_strategy_host.hpp:630) | **no** — `IntrabarPath::lower_tf` drives matching only **Closed:** `on_native_sub_bar` native_host.hpp:907 delivers every retained `lower_tf` sub-bar after its matching path, so a host that put the lower bars in the path reads them bar by bar (`tests/test_native_calc_timing.cpp`); the series form is the auxiliary feed of HT4 (`tests/test_native_auxiliary_feed.cpp`). | K | L5 (sub-bar hook); series form optional in L6 | F:F4 O:G-3 S:H2 | **Disagree on lane:** F via `on_native_sub_bar` (the host already owns the lower bars it put in the path) / O, S via the series API. v1 = the L5 hook (R5-5), landed; the array form Pine returns stays the adapter's. |
| HT4 | Auxiliary finer feed driving `request.security` (`set_aux_security_feed` pine_aux_security.cpp:20-197, `feed_aux_security_for_chart_bar` pine_aux_security.cpp:218-394; the kernel base returns false `set_aux_security_feed` engine_consumer.cpp:158-161) | **no** **Closed:** `NativeRunSpec::auxiliary_feed` (`NativeAuxiliaryFeed` native_run_spec.hpp:513) plus `NativeSeriesSource::AuxiliaryFeed` build a series from the host's finer bars, routed by time alone, with `declare_auxiliary_feed` at begin and `append_auxiliary_bars` on a realtime stream (`tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp`). | K+A | L6 | F:F3 S:H2 | **Disagree:** F quirk (TV split-feed harness construct) / S generic. R5-1 + R5-6: aux feed registration is generic; chart-slice mapping and deferred first-bucket publication stay A. |
| HT5 | Publication mode: lookahead, gaps, session / early-close boundaries (`prepare_historical_security_lookahead_projections` pine_scheduler.cpp:42-132, `calling_bar_complete` pine_aux_security.cpp:232-317) | **no** for a bare host **Closed for a bare host:** `NativeTimeframeSubscription::lookahead` and `::gaps` are the two delivery rules, and the calendar boundaries — session close, early close by instrument class, the lazy seal of a clipped script interval — are the kernel's own (`tests/test_native_htf_subscriptions.cpp`). | K+A | L6 | S:H3 F:F1 O:G-1 | **Disagree:** F "no `lookahead_on`, by construction" / O `lookahead = false` default, available / S `lookahead_on` field, status partial. R5-6: user question (§4 U1); the exact TV projection stays A. |
| HT6 | Self-aggregation recipe (interim) | **yes** — `TimeframeAggregator` is public and engine-free timeframe.hpp:288-486, fed from `on_native_input` native_host.hpp:856; Pine uses the same class `TimeframeAggregator` pine_scheduler_native.cpp:131-142 | K | L0 (document) | O:G-4 F:F1 | R5-6 |

### 1.7 Fill price, slippage, commission, FX

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| FP1 | Trigger geometry on the modeled path; fill at the level or at the gap price; market-if-touched | **yes** — `native_matching` native_execution_consumer.cpp:5218-5268, `first_region_entry` native_matching.hpp:229-257; kind `NativeCandidatePriceKind` native_execution_consumer.cpp:5744-5752; `Limit::fill_through` native_order.hpp:239-244 enforced native_execution_consumer.cpp:4848-4860, re-checked after host terms native_execution_consumer.cpp:4848-4867 | K | — | F:G3 O:H1 O:H2 O:H3 | — |
| FP2 | Slippage in ticks, limit-clamped | **yes** — `slippage_ticks` native_run_spec.hpp:554 applied once native_execution_consumer.cpp:4464-4465, clamp native_execution_consumer.cpp:4464 | K+A | — | F:G1 O:H7 S:F2 | TV's round-then-slip order (`source_bar_fill` pine_adapter.cpp:10235-10244) is A |
| FP3 | Commission: percent / cash per unit / cash per execution (`fee_kind_for` pine_adapter.cpp:453-460) | **yes** — `NativeFeeKind` native_run_spec.hpp:19-23; `quote_execution_commissions` engine_execution.cpp:960-996 | K | — | F:G2 O:H8 S:F1 | — |
| FP4 | Booked fill quantized to the instrument tick, nearest or directional (`nearest_tick` pine_adapter.cpp:246-249, `source_bar_fill` pine_adapter.cpp:10251-10260, `directional_tick` pine_adapter.cpp:291-295) | **no** — `price_tick` is only a slippage multiplier native_execution_consumer.cpp:3982; `bar_fill_price` engine.hpp:750 is reachable only through `NativeCurrentPriceRule::NearestTick` native_execution_consumer.cpp:5839-5840; the reference helpers were dead (since deleted) **Closed:** `NativeRunSpec::price_grid` (`NativePriceGrid` native_run_spec.hpp:117) with `grid_rounding` (`NativeGridRounding` native_run_spec.hpp:127) rounds the fill basis onto `price_tick` before slippage, `HalfUp` or `Directional` (`tests/test_native_price_grid.cpp`). | K | L8 | O:H4 O:H6 F:G4 S:F3 | **Disagree:** O kernel / F, S "stays adapter, no promotion". R5-3: own kernel lane. |
| FP5 | Trigger tested against the tick-quantized bar | **no** — the kernel tests raw prices against raw levels **Closed:** `NativePriceGrid::QuantizeFillsAndTriggers` additionally tests and re-validates every activation against the tick-quantized path, one rule for every trigger kind (`tests/test_native_price_grid.cpp`). | K+A | L8 | O:H5 F:G4 S:F3 | R5-3: the optional quantized trigger test is K; TV's exact half-tick rule (`source_trigger_threshold` pine_adapter.cpp:322-370) is A on top. |
| PG | The Pine adapter on the kernel price grid: raw levels submitted, `project()` declaring `QuantizeFillsAndTriggers` + `HalfUp`, `source_trigger_threshold` deleted (lane R7; re-tested after L8b by gap lane N13) | **measured-infeasible for the Pine adapter** — B1 (the sub-tick cursor-print abort) is closed by L8b's `ActivationGrid`: 0 aborted runs under the trial (R7: 9 zero-offset-trail tapes + 6 POOC panels; now `test_zero_offset_trail_rides_l4c` 449/450, `test_pooc_short_close_tick_l4d` 183/183). B2 (TradingView quantizes per order kind, the grid is one run-wide rule) moved 30 pinned checks in 4 units when N13 last ran the trial, before lanes E14 and E16, with no adapter-side remedy: `test_coof_market_limit_recross_l4c` 24, `test_stop_tick_rounding_l4d` 3, `test_adapter_grid_relower` 2, `test_zero_offset_trail_rides_l4c` 1; 12 more checks in 3 units have an adapter-side cause (`test_trail_fill_snap_l4c` 2, `test_native_margin_hooks` 2, `test_adapter_brackets_relower` 8 book digests). Corpus: 5/312 probes differ, in the engine-only entry-incarnation column only (every probe runs a 0.01 tick on an on-grid feed, so it cannot arbitrate) | A | waived (R7, N13) | — | **Waiver.** No class of triggers is byte-identical on its own: the grid is a run-wide switch and a per-kind mask would spell TradingView's inconsistency into the kernel (not generic, R5-3). The adapter stays on `NativePriceGrid::None` and keeps `source_trigger_threshold` and its call sites; native-engine.md "The Pine adapter's grid re-lowering is waived". **Native-only by ruling (audit lane P6): §3.6.2**, with `examples/native/native_price_grid_strategy.cpp` and its C twin as the feature's hosts. |
| FP6 | Account-currency FX | **partial** — scalar `account_fx` + immutable `NativeFxCurve` native_host.hpp:1143 in batch; the stream then refused a non-empty curve (today only tick-driven input is refused under one, `refuse_fx_curve_tick_input` native_execution_consumer.cpp:8084). **Closed by audit lane N6:** the declared curve is the run's immutable FX epoch and a confirmed-bar stream runs under it exactly as a batch does (stream twin in `tests/test_native_margin_fx_roll.cpp`); no realtime rate ingestion and no tick-driven input under a curve are stated as permanent limits with their reasons (native-engine.md, "Stream FX"). **Re-examined by R5 follow-up lane E3 and KEPT:** the tick refusal was never only about the margin check — it guards the SCRIPT-VISIBLE conversions (`open_trade_profit()`, `marked_equity()`, the equity curve), which read `active_account_currency_fx()`, i.e. the presented clock the observation hook has not yet moved (re-measured: a step at T+90s left the T+100s and T+110s prints converting at the pre-step rate). E3's cursor rule reaches the margin model only, so the refusal stands and is pinned in `tests/test_native_margin_fx_clock.cpp` | K | unassigned → N6 (E3: refusal kept) | F:D9 S:F4 | **Disagree:** F out of scope ("O7 clock") / S its P10, a stream-safe FX epoch. No R5 lane → §4 U4 (answered by N6: S's "same curve in batch and stream", without a new clock or callback). |
| FP7 | *(quirk)* TV tick conventions: half-tick threshold `source_bar_fill_tick` pine_adapter.cpp:329-377 (used `source_trigger_threshold` pine_adapter.cpp:7341-7346, `exit_limit_trigger` pine_adapter.cpp:7896-7917), `source_bar_fill_tick` pine_adapter.cpp:267-280, raw-vs-booked spelling pine_adapter.cpp:9734-9840, margin-call fill pricing pine_adapter.cpp:12260-12269 | n/a — already isolated behind the terms seam (adapter `resolve_terms` pine_adapter.cpp:9960) | A | — | F:G4 O:H5 S:F3 | R5-1, R5-3 |

### 1.8 Excursion, metrics, report, hash

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| RP1 | Per-lot MFE / MAE, kernel-sampled or host-owned | **yes** — sampler `apply_excursion` native_execution_consumer.cpp:2669-2682, closing-row fold `max_runup` engine_orders.cpp:152-166; `owns_lot_excursions` native_host.hpp:999-1003 | K+A | — | F:H1 O:I1 O:I2 S:X1 | **Disagree:** S partial + its P8 excursion-mode enum (single-source, not adopted, Appendix A.9). TV entry-bar masks are A (`sample_open_trade_extremes` pine_adapter.hpp:680-687). |
| RP2 | Closed-trade list, trade statistics, report struct | **yes** — `fill_report` engine_report.cpp:39-66, `get_trade` engine.hpp:1758-1795, `compute_trade_stats` engine_metrics.cpp:72 | K | — | F:H2 O:I3 S:X2 | — |
| RP3 | Equity curve | **no** — `update_equity_extremes` engine.hpp:1329 / `record_equity_point` engine.hpp:1347 are protected and were called only by the adapter **Closed:** `NativeReportPolicy::KernelRecorded` native_run_spec.hpp:62 has the consumer fold the extremes and append one point per script calculation, labelled with the script interval's open; `KernelRecordedAtHostMarks` records the same series at the host's own marks (`tests/test_native_report_truth.cpp`). | K | L2 | F:H3 O:I4 S:X2 | S's "partial" folds RP2 in |
| RP4 | Equity metrics (drawdown, run-up, Sharpe / Sortino, CAGR, time in market) | **no** — `compute_equity_stats` exits early on an empty curve engine_metrics.cpp:153; `fill_metrics_section` engine_report.cpp:119-139 **Closed:** with a recorded curve the drawdown / run-up walk is finite and every equity metric is computed over a real series; `report_open_position_at_end` adds the still-open position as a mark-to-market row (`tests/test_native_report_truth.cpp`). | K | L2 | F:H3 O:I5 | silent degenerate metrics, no error (O) |
| RP5 | Range-end row for a position still open at the end (`scheduler_record_range_end` pine_strategy_host.cpp:1683) | **yes, shared** — one generic producer `NativeExecutionConsumer::append_open_position_report_rows`, called by the kernel's run-end `record_open_position_report_rows` (`KernelRecorded` + `report_open_position_at_end`) and by the adapter at TradingView's mark; `report_trade_count()` engine.hpp:1782 includes the rows | K + A report shape | L2, Q6 | F:H4 O:I6 S:X2 | **Disagree:** S says kernel range-end rows exist (storage only). The ROWS are now the kernel's (§3.7 part 1); TV report shape — the equity re-mark, the extreme re-fold, the same-bar re-sort, three marks — and the short-seed swaps (`project_short_seed_report_rows` pine_strategy_host.cpp:1429) stay A, measured in §3.7 part 2. |
| RP6 | Per-trade accessors | **partial** — `get_trade` is public; the `closed_trade_*` family is protected engine.hpp:1794 **Closed:** `closed_trade_count()` / `closed_trade(i)` and `report_trade_count()` / `get_report_trade(i)` are public, and `closed_trade_close_cause(i)` engine.hpp:1800 answers the typed `execution::CloseCause` the closer recorded (`tests/test_native_report_truth.cpp`). | K | L2 | F:H6 | (single-source) |
| RP7 | Live order-action stream (runner ledger / webhook) | **yes** — `stream_observe_actions_` engine_execution.cpp:698, enabled `stream_observe_actions_` native_execution_consumer.cpp:8548, C ABI `strategy_stream_order_actions_len` c_abi.cpp:595-612 | K | — | F:H5 | (single-source) |
| RP8 | Continuation hash | **yes** — `native_continuation_hash` native_host.hpp:1282, native_execution_consumer.cpp:9720; the spec is folded by `hash_spec` native_execution_consumer.cpp:237-259 | K | — | F:I1 O:J2 S:X3 | — |
| RP9 | Broker-state hash: scalar + per-bar recording | **partial** — scalar `broker_state_hash` engine_state_hash.cpp:21-117, C ABI `strategy_broker_state_hash` c_abi.cpp:476; per-bar rows are appended only by the source scheduler `scheduler_record_broker_hash` pine_strategy_host.cpp:1890 **Closed:** under `KernelRecorded` with `set_broker_state_hash_recording(true)` the consumer appends one `broker_state_hash()` row after each report point, so `broker_state_hash_len == equity_curve_len == script_bars_processed` in batch and across a stream's warmup and realtime legs. A row is the run's continuation identity at that bar, per driving mode — the broker half alone is mode-invariant (`tests/test_native_report_truth.cpp`). | K | L2 | F:I2 O:J1 S:X3 | O's "yes" covers the scalar only. What a recorded row costs — quadratic on the Pine adapter, O(closed rows) per row in the kernel — is measured and ruled in §3.8 |
| RP10 | Fold the host's own durable state into the hash | **partial** — the protected virtual `hash_source_extension` engine.hpp:407 is overridable but source-named; the kernel default folds `"source:none"` engine_state_hash.cpp:18-20 **Closed:** `hash_host_extension(BrokerStateHashSink&)` engine.hpp:401 is the generic seam — protected virtual on `BacktestEngine`, called once per hash, last — and `hash_source_extension` engine.hpp:407 is its deprecated spelling, which the generic default forwards to (`tests/test_native_host_hash_extension.cpp`). | K | L2 (the virtual rides the shared bump) | F:I3 O:J1 S:X3 | — |

### 1.9 Other runtime surface

| ID | Feature (adapter mechanism) | Native today | Class | Lane | Sources | Notes |
|---|---|---|---|---|---|---|
| OT1 | Key / value run parameters (`set_input` engine.hpp:1822, refused in-run by `guard_native_mutation` engine.hpp:1827) | **partial** — staging allowed pre-run, getters protected | K (low) | — | F:J1 | (single-source) Nothing for v1: native hosts take constructor parameters. |
| OT2 | Symbol info, sessions, calendars, timezones (`timezone` pine_adapter.cpp:1434-1435, `chart_timezone` pine_adapter.cpp:1475-1477) | **yes** — spec fields `ticker` native_run_spec.hpp:538-548; `parse_session` native_calendar.hpp:173-419; intervals on every decision context `input_interval` market_driver.hpp:117-118 | K | — | F:J2 O:G-5 | — |
| OT3 | Indicators, series, math, matrix, map, string utils | **yes** — `ta.hpp` includes only `na/series/window_sum` `window_sum` ta.hpp:2-8 | K | L11 (naming only) | F:J3 | (single-source row) TV-calibrated numerics → §2.ii b |
| OT4 | Cooperative abort | **yes** — `request_abort` engine.hpp:2122, `NativeAbortReporting` native_run_spec.hpp:41-44 | K | — | F:J4 | (single-source) |
| OT5 | C-level run configuration, stream, FX curve, contract probe | **yes** — `strategy_configure_native_v1` c_abi.cpp:834, FX curve c_abi.cpp:834, probe c_abi.cpp:834, stream `strategy_stream_begin` c_abi.cpp:562 **Extended:** `pf_native_run_spec_ext_v1`'s policy tail now carries `intrabar`, `path_order`, `abort_reporting` and the slot-label / feed-tolerance policies (`PF_NATIVE_SPEC_EXT_INTRABAR`, `PF_NATIVE_SPEC_EXT_FEED_POLICY`), so nothing of `NativeRunSpec` but `identity` is unreachable from C (`tests/test_native_c_api.c`). | K | — | O:J4 | The v1 C spec `pf_native_run_spec_v1` pineforge.h:524 omitted `intrabar`, `path_order` and `abort_reporting`; they now travel in the extension's policy tail, `pf_native_abort_reporting_e` native_c_api.h:855 among them (F §2.iii) |
| OT6 | C-level order submission + strategy callbacks | **no** — none of the 57 `PF_API` symbols; then listed as a refusal on native-engine.md **Closed:** `strategy_native_host_create_v1` takes a C callback table and `strategy_native_submit_v1` / `_replace_v1` / `_cancel_v1` / `_cancel_all_v1` / `_cancel_where_v1` / `_execute_current_v1` command the same kernel under the same legality rule; the 32 symbols are a second, disjoint inventory beside the 57 codegen-facing ones (`tests/test_native_c_api.c`, `tests/test_native_c_api_frozen_header.cpp`). | K | L13 | F:J6 O:J5 | R5-7: post-v1. S covers it in its §2.3 without a row. |
| OT7 | *(quirk)* Pine language state: series, barstate / session flags, position-view freezing (`PineLanguageState` pine_language_state.hpp:12-68; `publish_series` pine_scheduler_native.cpp:210-245, `chart_index` pine_scheduler_native.cpp:512-624) | n/a | A | — | S:R1 | (single-source) **Closed (E25, lane E20's finding 2; E26, E25's findings 1 and 3):** `session.islastbar` reads one script bar ahead on every driving path: the chart timeframe's next retained bar, an aggregated chart's next bucket (a scheduler lookahead, deleted when the rule moved into the kernel), the calendar for the live tail's bar and for every bar of a stream, warmup included — a stream continues past its replay, so only a batch run's final bar reads "last" from having no successor. Since R4 slice C an aggregated or realtime bar read every in-session bar as last. **What TradingView does:** it ends a session at the session DAY — the flag belongs to the last chart bar whose successor belongs to another session day, in session or not, and `session.isfirstbar` is its dual, the bar after a session's last one. The ordinal is `internal::session_trading_day_index` timeframe.hpp:145, the exchange-timezone day rolling at the symbol's day stamp (09:30 ET on NYSE RTH, midnight on 24x7, 17:00 ET on a 1700-1700 forex session), not local midnight; the unmerged form, since a fused holiday daily bar still holds two intraday sessions. The three `lab tv` tapes (`tests/fixtures/session_islastbar`) give that rule bar for bar with no disagreement, and the engine now holds 255/255 NYSE:F last bars, 255/255 first bars and 370/370 ETH last bars on the chart-timeframe AND the aggregated path (it held 1, 0 and 1 before). Early closes come with it: the three NYSE:F half days flag 12:45 ET without a holiday calendar, because the next bar is the next session day's. **Generic since R5 lane F5:** the kernel computes that reading for every host — `NativeDecisionContext::in_session` market_driver.hpp:154 and the facts after it (`opens_session_day`, `closes_session_day`, `closes_session_day_open_ended`), in C the session bytes of `pf_native_decision_v1` native_c_api.h:1382 — from the run's own calendar and input (`present_session_day` native_execution_consumer.cpp:6735), so a bare host reproduces the tapes flag for flag (`tests/test_native_session_day_facts.cpp`, kernel-only). The adapter computes no rule of its own: `scheduler_update_session_state` pine_strategy_host.cpp:1755 selects the kernel's facts into the three flags, which are `PineStrategyHost` members now (`session_ismarket_` pine_strategy_host.hpp:850; `BacktestEngine` holds no Pine session state), and only its live-probe tail reads the open-ended close at its batch's final bar (`tests/test_session_day_facts_adapter.cpp`). Selecting the kernel also ended a calc_on_order_fills defect of the adapter's lookahead, which read two bars ahead on a bar a fill recalculation had already published (a fill on a day's second-to-last bar flagged that bar last and the day's real last bar first). **Ruled, not open:** a bar with nothing held after it — a stream's bar, a live-tail bar — steps the calendar one width on, so it cannot see an early close the session string does not declare (ADR-0001, "Session-day facts at a bar with nothing held after it"). **Closed (F1):** a `calc_on_order_fills` recalculation reads the flags of the bar it runs on, not the previous bar's (the kernel presents the current bar's facts on that recalculation), and the close callback after it no longer looks one retained bar too far ahead (§3.8, AG3). |
| OT8 | *(quirk)* Live-tail / probe-suppress harness flags (`realtime_tail_` pine_strategy_host.cpp:270-272) | n/a — **contained since N14** (the flags were `BacktestEngine` members until then; now `source::PineStrategyHost` state behind two kernel virtual seams, §2.ii q) | A | N14 | F:J7 S:C5 | parity-campaign tooling |

---

## 2. Structural work

### 2.i Kernel-only build target (L1)

- **Today:** one static library always compiles the source layer (list CMakeLists.txt:81-94, `add_library(pineforge STATIC` CMakeLists.txt:153, splice CMakeLists.txt:153) and installs every header, `include/pineforge/source/**` and `include/pineforge/compat/pine/**` included (CMakeLists.txt:307-310). `src/compat/pine/market_admission.cpp` sits in the *main* list (CMakeLists.txt:111) for historical reasons (added by `3b13183`, before the source-layer list existed — F).
- **Link feasibility:** no kernel TU and no public kernel header includes `source/` or `compat/pine/` (F, O). The only code coupling is the forward-declared opaque `source::StrategyOverrides*` (§2.ii a), which needs no definition.
- **Risk:** the archive member order of `libpineforge.a` changes. Harmless for static linking, but check `PINEFORGE_REQUIRE_ABI_RECEIPTS` (CMakeLists.txt:65); what the receipts pin is unverified (F).

| Aspect | F | O | S | Merged (L1) |
|---|---|---|---|---|
| Option | `PINEFORGE_BUILD_SOURCE_LAYER`, default ON | same | `PINEFORGE_BUILD_KERNEL_ONLY` | `PINEFORGE_BUILD_SOURCE_LAYER` (default ON; OFF = kernel only): 2 of 3, and it names the optional part |
| Targets | object libs `pineforge_kernel_objs` + `pineforge_source_objs`; `pineforge_kernel` STATIC, alias `PineForge::kernel`; `pineforge` STATIC = both | `pineforge_kernel` OBJECT; `pineforge` STATIC = kernel objects + conditional source list | `pineforge_kernel` + `pineforge` as the compatibility aggregate | F's shape. `pineforge` keeps its name, alias and install rules, so codegen-built strategies and `find_package(PineForge)` consumers do not move |
| What moves | `compat/pine/market_admission.cpp` → source list | same + delete dead `admission_retention` | same ("a concrete boundary bug") | **Done.** The unit is in `PINEFORGE_SOURCE_LAYER_SOURCES` CMakeLists.txt:91 with the other five `compat/pine/` units, and `admission_retention` is deleted — the name is nowhere in `src/` or `include/`. Its callers were the adapter only (pine_adapter.cpp:1798, pine_adapter.cpp:12583-12585, pine_adapter.cpp:12778-12779, pine_adapter.cpp:13323-13324) and tests, so the move was link-neutral |
| Installed headers | OFF skips `include/pineforge/source`, `include/pineforge/compat` | install conditional on the option | installed closure with source + compat headers physically removed | Conditional install; S's closure test is the acceptance |
| CI / gate | link the two native examples + the 20 source-free native tests against `pineforge_kernel` only; `nm` assertion in the independence checker (it already forbids `pineforge::source` / `compat::pine` symbols check_native_include_independence.py:72-78) | new CI lane: configure with the source layer OFF, build, run the checker against that prefix | clean kernel-only compile; `nm` shows no source / compat symbol; keep the textual ban (test_native_source_guard.py:18-60) as a fast preflight | All three, one CI lane | <!-- verified HEAD -->
| Size | M | M | L | M (R5-11) |

### 2.ii Coupling extraction (L11 neutral, L12 hash-visible) — R5-8

| # | Item | Where / what it is used for | Class | Plan | Src |
|---|---|---|---|---|---|
| a | `source::StrategyOverrides` opaque pointer | Forward declarations in the kernel headers (since removed: no kernel header names the adapter type now); parameter of `run_rich` (execution_consumer.hpp:46-55, native_execution_consumer.hpp:61-70) and of the rich `BacktestEngine::run` overload (engine.hpp:1747-1752, engine_consumer.cpp:81-93). The kernel only forwards it as `NativeBeginArgs::overrides_opaque` native_host.hpp:739; the source host casts it back `overrides_opaque` pine_strategy_host.cpp:274-277 | NEUTRAL in behaviour; a consumer-vtable signature change, so it rides the shared `engine_script_run` bump | `const void*` in the kernel signatures (F `host_begin_extras`, O `overrides_opaque`), typed convenience overload in `source/pine_strategy_host.hpp`; then drop the checker's single whitelist entry `test_native_example_batch` check_native_include_independence.py:42-46. S's alternative (`NativeRunOverrides`, or removing the rich overload) not adopted. Unverified: generated code calls the typed overload (codegen repo is not in this tree — F) | F O S |
| b | `pine_float_compare.hpp` | 1e-10 absolute equality band, used by kernel TA only: `float_band_le` ta_misc.cpp:28 (percentrank), `float_band_gt` ta_oscillators.cpp:462-463 (MFI), `plus_dm` ta_volatility_trend.cpp:356-359 (DMI); no execution TU includes it (O) | NEUTRAL (rename only) | Rename to a neutral header (F `float_band_compare.hpp`, O `ta_compare_band.hpp`), keep `pine_float_*` inline aliases in a source-layer header for generated code. Numerics must not move. S's behavioural `ComparePolicy` split is not neutral → not adopted (§4 U6). **Status (L11 + N14): done** — `ta_compare_band.hpp` is the kernel header; the alias shim is `include/pineforge/source/pine_float_compare.hpp` (N14 moved it off the kernel include root; it has no in-tree includer, generated code emits its own comparator) | F O S |
| c | `NativeSlotLabelPolicy::LegacyTolerant`, `NativeLegacyTolerance` (now `NativeSlotLabelPolicy::FeedTolerant`, `NativeFeedTolerance`) | `NativeSlotLabelPolicy` native_run_spec.hpp:323-352; read by `legacy_tolerant_slot_labels` market_driver.cpp:53-54 and `native_feed_tolerance_enabled` market_driver.cpp:41-47, and by `legacy_tolerant_slot_labels` native_execution_consumer.cpp:1995-1997, `:2020`, `:8145`; set only by the adapter `slot_label_policy` pine_adapter.cpp:1488-1489. A feed-tolerance policy (raw strictly-increasing labels, zero / NaN-volume legacy bars), not TV semantics (O) | NEUTRAL | Rename with deprecated aliases (F `RawIncreasing` + `NativeInputTolerance::{StructuralBars, WarmupNonNegativeOHLC}`; O `NativeLabelPolicy::ProviderLabels`). Enumerator values stay 0/1 and 1/2: they are hashed `slot_label_policy` native_execution_consumer.cpp:259-260. **Status:** renamed as `FeedTolerant` / `NativeFeedTolerance` (the enumerator values unchanged), and the C transport carries them now, in the extension's feed-policy block (`PF_NATIVE_SPEC_EXT_FEED_POLICY` native_c_api.h:724) | F O S |
| d | Orphaned comments | The KI-62 block of `engine_orders.cpp` (*fresh*: comment-only, the bodies are deleted; S read it as live code) and a comment of `engine.hpp`; wider deleted-declaration blocks in `engine.hpp`; the money-rule essay in `engine.hpp` (the code is `source_money_round` pine_adapter.cpp:435-441) | NEUTRAL | Delete, or move to `docs/` next to the adapter code that implements them. **Status: done** — R5 lanes E6 and E10 deleted the detached comment residue (a census of 0 over the kernel's compile closure) | F O |
| e | Dead helpers in the public kernel header | **Done.** All four are gone from `engine.hpp`: `round_to_mintick_directional` and `apply_slippage` were the pre-R5 spelling of the directional snap and the slippage step and the kernel now fills on `native_matching::grid_round_directional` / `native_matching::apply_slippage` src/native_matching.hpp:259 (the engine.hpp comment that named them went with R5 lane E10's residue sweep); `apply_limit_fill` and `reserve_percent_commission` are deleted outright, and only the adapter still spells the first two names, in its own comments. `round_to_mintick` engine.hpp:732 and `bar_fill_price` engine.hpp:750 stay, live through native_execution_consumer.cpp:5910 | NEUTRAL | Reuse as the reference arithmetic of the price-grid and sizing lanes, delete the rest | O (single-source) |
| f | Dead legacy stream path | **Done.** `stream_feed_input_bar` and `dispatch_source_stream_script_bar` are both gone — neither name is in `src/` or `include/` — and the virtual left with the epoch that carried it. `stream_state_hash` engine.hpp:1741 stays, and the declaration/definition pairing is still enforced by `scripts/test_native_source_guard.py` | NEUTRAL (removing the virtual changes the vtable → shared bump) | Delete both | O; F asked (its Q6) |
| g | Legacy path resolver | **Done, and then deleted.** The move happened: `src/engine_path_resolve.cpp` is 95 lines holding exactly the three generic things a kernel TU needs — `set_path_order_override` src/engine_path_resolve.cpp:36, `bar_path_uses_high_first` src/engine_path_resolve.cpp:40 and `first_touch_position` src/engine_path_resolve.cpp:51 — and `src/source/pine_path_resolve.cpp` is 83 lines holding `entry_stop_first_touch` src/source/pine_path_resolve.cpp:36, the one function of the old file a live caller still reaches — in the leg order its caller asks the kernel for (R5 lane F7 deleted the overload that read the sampler's override, with the TU's unused trail-tick declarations). `resolve_exit_path_fill`, `compute_exit_trail_state`, `tick_quantized_price` and `collect_cross_events` are **gone from the repository**: no declaration, no body, and no test-only copy — the `tests/exit_path_resolver_oracle.hpp` that preserved the body for the trail suites was retired too, so the repository holds ONE fill simulation (`src/native_matching.hpp` driven by `NativeExecutionConsumer`). Those suites now read the product through `tests/trail_exit_product_probe.hpp`, which drives the real host and projects the four fields the resolver reported from `native_events`; the rows that disagree on a coordinate carry their `expectation corrected:` note and are registered in `tests/twin_parity_inventory.json`'s `observableRewrites`. Two test files still name `resolve_exit_path_fill` in a comment, describing what the row used to compare against | NEUTRAL (pure move; verify with the L1 link gate) | Keep the generic functions in a small kernel TU, move the rest to `src/source/` (tests link the full library) | F (single-source) | <!-- verified HEAD -->
| h | `pine_*`-named generic API | `timeframe_time` session_time.hpp:34-195 (live, e.g. `session_in_market`, which generated code reaches through `pine_session_ismarket` session_time.hpp:247), `str_utils.hpp`, `deterministic_random` math.hpp:9, `tz_util` timezone.hpp:8-30; the guard bans `pine_*` only inside `IDENTIFIER_ROOTS` (test_native_source_guard.py:34, test_native_source_guard.py:34-42) | NEUTRAL, M | Neutral aliases; `pine_*` kept as deprecated inlines for codegen. Large blast radius in generated code. **Status (L11 + N14): done** — session_time / str_utils / math / timezone in L11; N14 applied the same treatment to the two public matrix types: `PineMatrix` → `NumericMatrix`, `PineGenericMatrix<T>` → `GenericMatrix<T>`, old spellings kept as exact aliases (no version script pins either name) | O (single-source) |
| i | `pf_pending_order_v1_t` Pine-named fields | `tv_carry_qty` pending_order_mirror.hpp:73, `pine_birth_reach` pending_order_mirror.hpp:135, `pine_exit_activation_owner_cycle_at_birth` pending_order_mirror.hpp:147-156, `pine_frozen_market_instruction_kind` pending_order_mirror.hpp:164-169; enumerated by `PF_PO_FIELD` pending_order_mirror.cpp:144-178; written only by `pine_birth_reach` pine_adapter.cpp:17089-17118 | no change | Freeze (ABI). The neutral view arrives beside it (L7 / L13) | O (single-source) |
| j | Pine-shaped lot flags | **Mostly done.** `market_pyramid_add` moved to the adapter side table the row asked for — `market_pyramid_adds_` src/source/pine_state_hash.cpp:512, whose own comment records that it was `PyramidEntry::market_pyramid_add` — and `ordinary_market_open`, `pooc_terminal_market_entry`, `ordinary_stop_open`, `bracket_slot_shadowed` and `entry_path_position` are all deleted. What is left on the lot is the intrabar-fill excursion mask, `skip_entry_bar_high` / `skip_entry_bar_low` engine.hpp:149, hashed engine_state_hash.cpp:65 and, since R5 lane E6, written by no host at all: the excursion owner declares where its fill sat (`declare_opened_lot_entry_bar_mask` pine_strategy_host.cpp:555) and the kernel derives the pair | HASH-VISIBLE | Move the live flag to an adapter side table keyed by `entry_incarnation` (the adapter already keeps `PlacementTable` pine_adapter.hpp:297-476) and fold it through `hash_source_extension`; delete the dead ones; update the hash-coverage waivers. Whether a kernel TU beyond the hash reads them is unverified (O) | F O S |
| k | `margin_call_enabled_` | **Done.** Neither the flag nor a `set_margin_call_enabled` setter exists anywhere in `src/` or `include/`: what enables the model is the presence of `NativeRunSpec::margin` native_run_spec.hpp:574, which the adapter sets from `strategy(margin_long=, margin_short=)` | HASH-VISIBLE (R5-8) | The kernel toggle is the presence of `spec.margin`; the flag and its C setter semantics move into the source host, the C symbol stays (pinned runtime list) | F O |
| l | Kernel decodes adapter strings | **Done.** `closed_trade_close_cause` engine_trade_accessors.cpp:158 branches on no string at all: it reads `Trade::open_at_end`, then the typed `execution::CloseCause` the closer recorded on the row, then `exit_from_bracket`. A kernel-originated liquidation or risk flatten states its own cause through the settling fill, so a bare host reads `3` / `4` where the retired string derivation answered `1`. The public C contract it implements is `strategy_closed_trade_close_cause` pineforge.h:1223 | HASH-VISIBLE (adds durable state) | Kernel `enum class CloseCause` recorded on the `Trade` at settlement (the kernel sets `MarginCall` when L4 liquidates; a source virtual covers adapter causes). C numbers unchanged. After L4 | O (single-source) |
| m | `request.security` machinery | `register_security_eval` engine_security.cpp:26, `set_native_security_feed` engine_aux_security.cpp:76-127, `SecurityEvalState` engine.hpp:1123-1327 carry Pine semantics (lookahead, gaps, Heikin-Ashi, KI-55 range start) and are driven only by the source layer | HASH-VISIBLE / measured path | F planned to move both TUs and the state into the source host after native HTF. **R5-6 narrows this:** L6 reuses the feed store and routing, so those stay kernel; only the Pine-only evaluator semantics remain candidates. Do last. **Status (lane L12b): done** — the evaluator semantics are `source::PineSecurityEvalState` + `src/source/pine_security_eval.cpp`; the kernel keeps the registry, aggregator, feed store and one generic step | F S |
| n | S-only proposals, not adopted | A generic `FillModel` interface with a TV model installed by the adapter (for the TradingView money / tick / path comment blocks in `engine.hpp` and the dead helpers of item e); "emit generic FIFO facts and let an adapter policy produce the KI-62 scratch" | — | Superseded by R5-3 (price-grid lane) and R5-1 (TV fill specifics stay behind `resolve_execution_terms`); the dead-helper part is item e; there is no scratch code left to re-lower (item d) | S |
| o | `pineforge::admission` (market-admission journal) — **N14 addition** | `include/pineforge/market_admission.hpp` (275 lines, installed by a kernel-only build) + `src/market_admission.cpp` (kernel TU): the observation journal of TradingView admission reviews; every `Configuration` field is a `strategy()` declaration parameter. No kernel TU consumed it (engine.hpp included it and used nothing; the hash helper in `src/broker_state_hash_internal.hpp` had one caller, `src/source/pine_state_hash.cpp`) | NEUTRAL (pure move; the fold is unchanged, so no hash domain moves) | **Status (N14): done** — moved whole to `include/pineforge/source/market_admission.hpp` + `src/source/market_admission.cpp` (`PINEFORGE_SOURCE_LAYER_SOURCES`); `hash_admission_field` moved verbatim into the source hash TU; `check_market_admission_schema.py` re-pointed; `MarketAdmissionDraft` / `MarketAdmissionJournal` spellings and the `market_admission_v2` inline namespace unchanged. ADR-0001 no longer calls it a generic stem | audit (Opus N14) |
| p | TradingView trail tick arithmetic — **N14 addition** | `src/engine_internal.hpp`: `kTrailPointsCeilEps` (5e-5 tolerant ceil of `trail_points`), `trail_points_to_ticks`, `trail_offset_to_ticks`, `snap_trail_level_to_tick_grid`; every value a `lab tv` calibration; readers only in `src/source/` and `src/compat/pine/` (the kernel's trail is `TrailTicks`, L7) | NEUTRAL (bodies byte-identical) | **Status (N14): done** — moved verbatim to `include/pineforge/compat/pine/trail_ticks.hpp` (`pineforge::compat::pine`); nine call sites re-qualified; `test_trail_fill_snap_l4c` includes the compat header (CHECK texts unchanged, twin parity OK) and is therefore a source-layer row | audit (Opus N14) |
| q | OT8 live-tail / probe-suppress overrides — **N14 addition** | `BacktestEngine::realtime_tail_`, `realtime_tail_horizon_bars_`, `probe_suppress_tail_logic_` with public kernel setters and the horizon walk `apply_realtime_tail_horizon` (engine_run.cpp); every reader in `src/source/`; two frozen `PF_API` setters in `c_abi.cpp` | HASH-WAIVED (configuration before and after; no domain moves) | **Status (N14): done** — as 2.ii k: state, setters, readers and the walk move verbatim to `source::PineStrategyHost`; the C exports stay in `c_abi.cpp` and reach the host through two kernel virtual seams (`virtual bool set_realtime_tail`, `virtual bool set_probe_suppress_tail_logic`) whose kernel defaults are accepted-and-inert (answer `false`, no error: the L1 pre-begin ingress contract `test_native_example_batch` pins on a native module); v16→v18 relocation manifest rows added, epoch v18 still open, no bump; kernel-only pin `test_native_tail_override_seam` | audit (Opus N14) |
| r | Lower-timeframe merge-flag rule — **N14 addition** | `internal::ensure_supported_lower_tf_emulation_flags` (engine_lower_tf.cpp) threw "request.security lower TF emulation only supports lookahead=barmerge.lookahead_off and gaps=barmerge.gaps_off" from a kernel TU — the one executable Pine/`barmerge` string in `libpineforge_kernel.a` | NEUTRAL (message and refusal unchanged) | **Status (N14): done** — the predicate is a file-local helper in `src/source/pine_security_eval.cpp`; the kernel TU keeps the generic primitives (fixed intraday TF parser, input : requested ratio, evenly sampled sub-bars), which have kernel-only tests (`test_lower_tf_parse_extra`, `test_lower_tf_seconds_suffix`) and source-only production callers — retained as a generic capability | audit (Opus N14, Codex N4) |
| s | Authoritative-feed period partition (engine_aux_security.cpp) — **N14 ruling** | "W"/"M" from the installed daily feed; feed stamps as the period partition; trade date = session-day of the period's last chart bar (§2.iv item 6, §4 E8). A bare host that installs `authoritative_bars` inherits these | — (documented contract, no gate) | **Ruled (N14): retained as the generic contract.** The three rules follow from "a feed is the venue's own bars of one timeframe" and from nothing platform-specific; the policy knob is the feed itself (install none → plain aggregation of the input; the doc says so, `native-engine.md` "Authoritative bars"). A separate partition-policy field would be hash-visible spec surface for a choice the host already makes by installing or not installing the feed. The TradingView pins stay as the calibration evidence. Listed in ADR-0001's residual table | audit (Opus R-7, Codex "TV-shaped auxiliary-feed policy") |
| t | Early-close completion keyed by instrument class (engine_security.cpp `session_template_knows_early_close`) — **N14 ruling** | Branches on `SymInfo::type` ∈ {forex, cfd, crypto} (continuous-session OTC classes never complete a calendar period at an early close; the OANDA pins in timeframe.hpp) | — (documented contract) | **Ruled (N14): retained.** `SymInfo::type`'s vocabulary is fixed by the frozen C ABI (`strategy_set_syminfo_type`), so classifying by it is the kernel's own market-structure vocabulary, not a source-language one; the adapter's routed sites (R3b) and the bare host's subscriptions share the rule, which keeps the corpus byte-identical by construction. Stated on the function and in ADR-0001's residual table | audit (Opus R-8, design §4 E8) |

### 2.iii C-level order submission and callbacks (L13, post-v1 — R5-7)

Today a C host can configure (`strategy_configure_native_v1` c_abi.cpp:799-851), stream (`strategy_stream_*` c_abi.cpp:527-636) and read results, but strategy logic is C++-only and the batch entry points (`strategy_create`, `run_backtest*`) are per-strategy symbols a native author hand-writes. Design retained from F (most complete), with O's event polling and S's hardening rules:

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
| Legality | The existing rule (`commands_allowed` native_execution_consumer.cpp:1521-1528): inside a callback, or between realtime inputs | F |
| Hardening | Tagged, size-prefixed PODs; `struct_size` + version; unknown-tag refusal; never cast the C request to a C++ variant; a C fixture compiles against the frozen previous header. S puts the API in a separate header `include/pineforge/native_c_api.h` | S |
| SOP | Every symbol goes into `src/c_abi.cpp`, `pineforge.h`, `EXPECTED_RUNTIME` + both counts (check_c_abi_runtime.py:19-79), the README table and the Python harnesses, or all CI matrix jobs fail at the "C ABI runtime source check" step (`CLAUDE.md`) | F O S |
| Not in v1 of the C API | `resolve_execution_terms` / `validate_execution_precommit` are the adapter's seam; a C host that needs sizing uses `Sized` (F). O offers an optional `resolve_terms` callback: deferred | F O |
| Open | Naming: F and O use `pf_native_*`; S uses `strategy_native_*`, which matches the existing runtime symbols. The v1 C spec `pf_native_run_spec_v1` pineforge.h:524 omitted `intrabar`, `path_order` and `abort_reporting`; the extension's policy tail carries all three now (`pf_native_abort_reporting_e` native_c_api.h:855) | F S |

Depends on L7 (working view), L3 (so `Sized` is in v1 of the C request) and the epoch batch.

### 2.iv Native HTF for bare hosts (L6) — chosen design per R5-6

| | F (its §2.iv A) | O (its G10) | S (its P7) | Chosen |
|---|---|---|---|---|
| API shape | spec `subscriptions` + push callback `on_native_timeframe_bar` | `register_native_series` + pull `native_series_bar(id)` / `native_series_slot_is_new(id)` | spec `security_requests` + `configure_native_security_feed` + `native_security(id)` view | F's subscription in the spec + push callback, plus O's pull accessor |
| Engine reuse | none: generalize the consumer's one script bucket (`ScriptBucket` native_execution_consumer.hpp:291-305, `contribute_input` native_execution_consumer.cpp:7393, `seal_script` native_execution_consumer.cpp:7370) to N; rejects exposing the `request.security` path as Pine-shaped end to end | promote the existing evaluator + feed machinery; edits `engine_security.cpp`, `engine_aux_security.cpp`, `engine_lower_tf.cpp` | same TUs + `native_calendar.cpp` | The existing feed machinery, **called, not edited**; `engine_security.cpp` untouched |
| Pump | consumer seal logic | accepted-input path (`invoke_input_callback` native_execution_consumer.cpp:6558), only for natively registered series | copy at Ready, evaluators created before the first input | O's registration-gated pump + S's copy-at-Ready |
| Authoritative bars | `authoritative_bars` on the subscription | `Source::ExchangeFeed` through `set_native_security_feed` | `configure_native_security_feed(id, bars, tf)` | F's field, installed through the existing store |
| Lookahead | none, by construction | `lookahead = false` default, available | `lookahead_on = false`, `gaps_on = true` | open → §4 U1 |

1. **Spec:** `std::vector<NativeTimeframeSubscription> subscriptions` (Appendix A.5), hashed only when non-empty (digest like `native_intrabar_path_digest`).
2. **Wiring**, all inside the native consumer at Ready → begin and only when `subscriptions` is non-empty (zero work for the adapter): register one evaluator state per subscription through the existing `register_security_eval` (engine.hpp:1262-1264, *fresh*); install `authoritative_bars` into the existing store (`set_native_security_feed` engine.hpp:1697, engine_aux_security.cpp:78-129); call `prepare_native_security_feeds` (engine.hpp:1606, defined engine_aux_security.cpp:128, *fresh*) — the "made native-callable" step: reachable from the native consumer, still refused from host code in-run; pump `feed_security_eval_state` (engine.hpp:1279-1281, *fresh*) from the accepted-input path; deliver each completed bucket through `on_native_timeframe_bar`.
3. **Untouched:** `engine_security.cpp`, the Pine scheduler sequence pine_strategy_host.cpp:1316-1400 and its call order. The per-run evaluator init is today a *source-layer* member (`PineStrategyHost::init_security_eval_states_for_run` pine_scheduler.cpp:9-39, declared pine_strategy_host.hpp:689, *fresh*), so L6 adds a kernel-side twin instead of moving it.
4. **Delivery rule (F):** a subscription bar is delivered on the input bar that completes it — its last contributing input bar, or, for a `LazyComplete` bucket, the next period's first input bar — immediately before that input's script calculation, never earlier — and never before an earlier script interval's lazily sealed calculation has run: the pump is ordered against the script interval, `seal(k) → deliver(i+1) → calc(i+1)` (L6d).
5. **Interim, documented in L0:** `on_native_input` + `TimeframeAggregator` (HT6). Doc notes (O): `set_native_security_feed` is public but inert without evaluators and throws in-run (`guard_native_mutation` engine_aux_security.cpp:78, which latches `UnsupportedSource` native_execution_consumer.cpp:1574); `set_aux_security_feed` returns false in the kernel engine_consumer.cpp:158-160.
6. **Entanglement to put in the lane brief** (*fresh* read of engine_aux_security.cpp:48-87): the feed machinery is TV-calibrated. "W" / "M" buckets are built from the installed native daily bars, and the native stamps define the period partition (a holiday session folds into the next trade date's bar). A bare host that supplies `authoritative_bars` inherits those rules. Document them as the contract or gate them behind a partition policy — decide in the lane, not silently (§4 E8).
7. **HT3 / HT4:** lower-TF arrays come from the L5 sub-bar hook in v1. An auxiliary feed is a subscription whose bars come from the host; the Pine chart-slice mapping is not exposed. Whether v1 accepts a feed *finer* than the input is left to the lane brief (S wants it, F calls it a TV harness construct).
   **Status (audit lane N7): HT4 done for a bare host; the adapter's auxiliary path retained, measured.** `NativeRunSpec::auxiliary_feed` (`NativeAuxiliaryFeed{tf, bars}`, strictly finer than `input_tf`, opt-in and hashed only when present) plus `NativeTimeframeSubscription::source = NativeSeriesSource::AuxiliaryFeed` build a series from the host's finer bars through the existing evaluator step (`register_security_eval` against the feed's timeframe, `feed_security_eval_state` per feed bar — called, not edited); routing is by time alone (a feed bar rides on the first accepted input whose period it opened before), with `declare_auxiliary_feed` at begin, `append_auxiliary_bars` on a realtime stream and a C spelling (`PF_NATIVE_SPEC_EXT_AUXILIARY_FEED` tail, `strategy_native_append_auxiliary_bars_v1`). No epoch moved: `native_run_spec_v3` and `engine_script_run_v18` are open (no release tag contains them, no frozen provider fixture of either exists), exactly as L6c's `gaps` and begin-time hook were added. `BacktestEngine::set_aux_security_feed` stays the source host's door and still answers a silent false on a bare host (the pre-begin setter pins, `tests/test_native_example_batch.cpp` among them, hold it to that). **Retained, why:** the adapter does not re-lower its auxiliary sites, on three measurements — 0 of 312 corpus probes install an auxiliary feed, so corpus byte-identity cannot witness the move; TradingView's chart slice leaves pre-range feed coverage inert where the kernel folds it by time (two hourly buckets against three over one feed, `tests/test_native_auxiliary_feed_twin.cpp` row B); and the adapter evaluates a chart bar's slice at its calculation, after that bar's matching pass, where the kernel delivers before it (position 1 against 0 at the same bucket, row C), which no predicate over an opaque generated `evaluate_security` can prove neutral. The congruent shape is the same series on both (row A) and can move once a population exercises it. Full contract: `docs/pages/native-engine.md`, "The auxiliary finer feed".

### 2.v Promote the examples (L10)

- Move `runner/examples/native_market_strategy.cpp` and `native_selected_strategy.cpp` to a top-level `examples/native/`, built by `PINEFORGE_BUILD_EXAMPLES` (today "none yet" and guarding nothing CMakeLists.txt:37; the examples build only under `PINEFORGE_BUILD_LIVE_RUNNER` CMakeLists.txt:427-428, with `native_market_example` runner/CMakeLists.txt:45). Link the kernel target (S), or `PineForge::pineforge` during migration (O) — no SQLite / curl / OpenSSL (runner/CMakeLists.txt:1-4).
- Build (1) standalone executables with a `main()` that runs batch + stream on embedded bars (the guide already contains that `main` native-engine.md:2720-2787) and (2) the same MODULE targets the runner tests load; keep the C-ABI shims so the live runner can still `dlopen` them (F, O, S).
- Update the three places that pin the paths: runner/CMakeLists.txt:37-45, check_native_include_independence.py:36-39, the tests at runner/CMakeLists.txt:98-128.
- Add a minimal "hello, kernel" host (~60 lines, no C ABI — O) and one example per v1 feature lane as it lands; each doubles as that lane's twin host (F).
- Add `include/pineforge/native_module.hpp` with `PINEFORGE_EXPORT_NATIVE_STRATEGY(Class)` to replace the ~70 hand-written `extern "C"` lines per example (F, single-source).
- Ship the native API reference and the standard dual-run harness of §3.1 b (S lane J).

### 2.vi ADR-0001 correction list and `native-engine.md` fixes

**Status: applied.** Every row below has been carried into
`docs/adr/0001-kernel-adapter-boundary.md` and `docs/pages/native-engine.md`, and both pages are
now held by `scripts/check_doc_anchors.py` (every `file:line` names its symbol) and
`scripts/check_doc_lint.py` (no roadmap label, no unverified negative claim, no stale epoch).
Several rows were overtaken by the code while they waited: rows 2, 3, 4, 8 and 9 describe a tree
that no longer exists, and the ADR now says so in its own words. The rows are kept as the
audit's record of what each page asserted, not as a work list.

Union of F §2.vi (9), O §2.vi (5 + 1) and S §2.6 (7), deduplicated. S had no ADR file in its checkout and wrote "what an ADR in this tree must say"; rows 11-12 come from that.

| # | ADR claim | Correct statement | Evidence | Src |
|---|---|---|---|---|
| 1 | Orders are "Market / Limit / Stop" (ADR L72-75) | Also StopLimit and Trail; five intents, five owner kinds, groups, `PointBudget` | native_order.hpp:88-96, native_order.hpp:75, native_order.hpp:104-119, native_order.hpp:121-128, native_order.hpp:99-102 | F |
| 2 | "The kernel matches and fills … with sizing, slippage, fees, margin and settlement" (ADR L74-75) | "with slippage, fees, opening admission and settlement; sizing is resolved by the host". `NativeRunSpec` owns fees, FX, close timing, direction / caps and intrabar paths; it does not own percent / cash sizing, maintenance margin, calc-on-fill, every-tick calculation or bare-host security registration | native_execution_consumer.cpp:2813-2815, native_run_spec.hpp:19-176, native_run_spec.hpp:173-174 | O S |
| 3 | "Read results via `metrics.hpp`" (ADR L76-80) | Trade stats yes. Equity curve and equity metrics are empty for a bare host, silently, and an open final position is missing from the report | engine_metrics.cpp:168, engine_report.cpp:119-139, engine.hpp:1799-1825, pine_strategy_host.cpp:1582-1583, engine_run.cpp:191 | F O |
| 4 | HTF "comes from the kernel feed `set_native_security_feed` / `prepare_native_security_feeds`" (ADR L84-89) | Not usable by a bare host: a partial boundary, not an absent kernel. `prepare_native_security_feeds` is protected with one caller; `configure_security_evaluators` has one caller; the public setter is inert pre-run and refused in-run | `prepare_native_security_feeds` engine.hpp:1606, `configure_security_evaluators` engine.hpp:1289, `set_native_security_feed` engine.hpp:1697, `guard_native_mutation` engine_aux_security.cpp:78 | F O S |
| 5 | "`engine_aux_security.cpp` includes `source/pine_strategy_host.hpp`" (ADR L129-132) | False at `73817c1`: its includes are `engine_internal.hpp`, `ta.hpp` and std headers (*fresh* re-read). No kernel TU includes a source header | engine_aux_security.cpp:5-12 | F |
| 6 | "Kernel TUs reference adapter types (`source::StrategyOverrides` …)" (ADR L129-131) | True only as a forward-declared opaque pointer the kernel never dereferences; the independence checker whitelists exactly that symbol | engine.hpp:406-408, execution_consumer.hpp:15, native_host.hpp:387, check_native_include_independence.py:42-46 | F O |
| 7 | `engine.hpp` hosts "the ten-significant-digit money rule, KI-62, the `strategy.close` batching + entry-id ledger, a TradingView margin-call toggle" (ADR L131-134) | Two of four survive as code. Money rule: a comment block; the arithmetic is in the adapter. KI-62: orphaned comments + one live, hashed lot flag. Close batching + id ledger: gone from the kernel (the ledger is in the adapter; the old names survive only in comments). Live: `margin_call_enabled_`, the KI-62 flag and — unmentioned by the ADR — string-sentinel decoding of adapter comments | the money-rule block was lines 927-961 of `engine.hpp` and R5 lane E6 deleted it (lines 73-180 were and are live code), the arithmetic `source_money_round` pine_adapter.cpp:435-441 and its twin `tv_money_round` pine_policy_support.hpp:9-15; `skip_entry_bar_high` engine.hpp:148, `skip_entry_bar_low` engine_state_hash.cpp:65, `declare_opened_lot_entry_bar_mask` pine_strategy_host.cpp:555; the ledger `close_logical_units_` pine_adapter.hpp:1413-1424; `closed_trade_close_cause` engine_trade_accessors.cpp:158. **Overtaken:** the margin-call toggle and the string decoding are both gone — see ADR-0001, "Current state vs. target" | F O |
| 8 | "No kernel-level C API. `c_abi.cpp` exports only version/descriptor symbols" (ADR L136-138) | 57 runtime `PF_API` symbols: native configure, the 12-symbol stream family, pending-order views, broker-state hash, security feeds, FX curve, reports. Accurate statement: no C symbol submits / replaces / cancels an order and no C strategy callback exists, so strategy logic is C++-only | c_abi.cpp:227-854, check_c_abi_runtime.py:19-79, c_abi.cpp:800, c_abi.cpp:528-647, c_abi.cpp:453-505, c_abi.cpp:432-449, c_abi.cpp:740-771, c_abi.cpp:854 | F O S |
| 9 | "No examples. `PINEFORGE_BUILD_EXAMPLES` is 'none yet'" (ADR L139) | The option is inert, but two Pine-free `NativeStrategyHost` examples exist, are compiled by the independence checker and tested through the live runner, only under `PINEFORGE_BUILD_LIVE_RUNNER`. Wording: "no top-level examples target" | CMakeLists.txt:37, check_native_include_independence.py:36-39, runner/CMakeLists.txt:37-45, CMakeLists.txt:341-342 | F O S |
| 10 | Follow-ups (1)-(4) (ADR L168-170) | Add native reporting, sizing, margin, HTF — and, per R5, calc-timing, price grid and order ergonomics. Without them "usable without Pine" is a build-system claim, not a feature claim | §3.5 | F |
| 11 | Build layout | One `pineforge` library always appends the source list; a kernel-only target does not exist yet; `compat/pine/market_admission.cpp` sits in the main list, a boundary bug. **Overtaken:** `PINEFORGE_KERNEL_SOURCES` CMakeLists.txt:113 and `add_library` CMakeLists.txt:153 build `PineForge::kernel`, and the compat unit is in `PINEFORGE_SOURCE_LAYER_SOURCES` CMakeLists.txt:91 | `add_library` CMakeLists.txt:153 | S (F, O agree in §2.i) |
| 12 | Host constructors | `NativeStrategyHost` is a zero-argument C++ host with the callbacks and the request API; `PineStrategyHost` is the class that takes `CapAttachment` — do not attribute it to the native constructor | `NativeStrategyHost` native_host.hpp:826, `PineStrategyHost` pine_strategy_host.hpp:242 | S (single-source) |

`docs/pages/native-engine.md` fixes, all applied (F unless noted). The page is cited by
section rather than by line, because a reference page's line numbers move with every edit and
the campaign's own anchor audit found that the first thing to rot:

| Where | Said | Tree says |
|---|---|---|
| the page's opening, and its "close-only" claims | callbacks are close-only | The kernel has a bar-open hook (`on_native_bar_open` native_host.hpp:869), a tick hook (`on_native_tick` native_host.hpp:859), a post-fill hook (`on_native_applied` native_host.hpp:917), a recalculation hook (`on_native_recalculate` native_host.hpp:892) and a sub-bar hook (`on_native_sub_bar` native_host.hpp:907). The page's opening now tables all eleven, and `on_native_applied` is documented as the calculate-on-fill hook with suffix eligibility |
| the request-epoch sentence | `native_order_v4` | `native_order_v6` native_order.hpp:25 | <!-- verified HEAD -->
| the two host-epoch sentences | host epoch v16 | `engine_script_run_v18` native_host.hpp:20 |
| "What still requires Pine compatibility to build" | `BacktestEngine` still holds `CapAttachment` / `OrderPriority` / `IntradayCap` | `engine.hpp` has zero references to them; the cap type is `CapAttachment` intraday_cap.hpp:18 and the constructor is `PineStrategyHost`'s pine_strategy_host.hpp:244 |
| the rich-`run` paragraph | the rich `run` overload fails a native host | `run_rich` native_execution_consumer.cpp:8420-8458 shows no such refusal (still unpinned by a test; the page says so) |
| added | — | the self-aggregation recipe beside the subscriptions it is an alternative to; the open-bar lookahead warning with `current_partial_bar` native_host.hpp:1022; and `set_native_security_feed` documented as the pre-run door for a series' authoritative bars, refused in-run |

---

## 3. Roadmap

### 3.1 Parity discipline for every lane (R5-10) — stated once, referenced per lane

- **(a) NEUTRALITY.** Every new behaviour is opt-in by a spec field or a new request kind. The adapter's `project()` (pine_adapter.cpp:1375-1517) never sets the field and never emits the kind, so adapter runs are byte-identical by construction. New spec blocks are folded into `hash_spec` **only when set**: it folds every field unconditionally today (native_execution_consumer.cpp:237-259), so even a defaulted new field would move every continuation hash; the precedent for a conditional digest is `precommit_digest_` (native_execution_consumer.hpp:877-881). Each lane adds a test that a default spec hashes to the pre-change constant. Every new durable field is hashed (`check_broker_state_hash_coverage.py`, fail-closed, waivers in `broker_state_hash_waivers.txt`). Proof per PR: `scripts/run_corpus.sh` + `scripts/verify_corpus.py` locally, then ONE Cloud Run campaign sweep on the final tree (fixed population, 4190 probes, zero tolerance on the hard band — native-refactor-progress.md:41-47; a fresh exact-HEAD verdict is required, AGENTS.md:124-143). A lane that cannot be shown neutral by construction does not merge.
- **(b) TWIN.** One chosen probe per lane runs through the adapter AND a hand-written `NativeStrategyHost` on the same bars and spec. Trade lists are diffed **by identity** (entry / exit time, price, qty, pnl, commission), never by trade number; then event, ownership and hash detail (S's D → E order). A difference is allowed only where a named TV-quirk row of §1 is active, and each one is itemized. Because 430 test files drive the adapter and 14 the native host (O), every lane also adds native-only tests: a lane is not accepted on the corpus alone, nor on a unit test alone (S).
- **(c) EPOCH BUDGET.** Three inline-namespace epochs move — the request epoch, the host epoch and (*fresh*, not in the inputs) the run-spec epoch, which every new spec field moves. They stood at `native_order_v5`, `engine_script_run_v17` and `native_run_spec_v2` when this plan was written; on this tree they are `native_order_v6` (native_order.hpp:25), `engine_script_run_v18` (native_host.hpp:20) and `native_run_spec_v3` (native_run_spec.hpp:15), so the budget below was spent exactly once. One bump each per release batch: `native_order_v6` = L3 + L7 + L4's origin / event; `engine_script_run_v18` = L4's host hooks + L5 + L6 virtuals + L2's `hash_host_extension` + §2.ii a, f; `native_run_spec_v3` = the spec blocks of L2, L4, L5, L6, L8 (L9 if it ships in the batch). A lane that lands before its batch closes stages behind the open epoch, so codegen-built strategies rebuild once per batch. Epoch lanes are not neutral-refactor lanes: the brief lists the header extension explicitly; the frozen C++ ABI fixtures (`tests/fixtures/native_cpp_abi/host-*`) and the ABI checkers (`check_native_cpp_abi.py`, `check_settlement_cpp_abi.py`) move with them; `variant_size` pins change (`OrderIntent` native_order.hpp:229, `CommandEvent` native_order.hpp:1194). Every new test unit compiles first against the frozen previous-epoch header closure (fail-before, `CLAUDE.md`). <!-- verified HEAD -->
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

#### The audit lanes that followed (landed on `main`)

Two independent claimed-vs-actual audits ran after the L-series, each proposing
lanes of its own. These are the ones that landed, with the last commit of each
on `main` — `git log --grep 'lane <id>'` finds the rest of a lane's commits:

| Lane | What it did | Landed |
|---|---|---|
| **P2** | `scripts/check_kernel_residuals.py`: `strings`/`nm` over `libpineforge_kernel.a` held against ADR-0001's residual tables, as a `kernel`-profile stage and a CTest row in every profile. Ruled the eighty-nine pending-row names the first probe was blind to (`coof_*`, `pooc_*`, `market_admission_*`, `tv_carry_qty`) by family, and the ambient EMA seeding default by mechanism — `ta::EmaSeeding` became a named per-instance option | `c421b7a9` |
| **P2b** | Made that gate profile-independent: it reads the linkable surface (`nm` over the archive, `strings` over a debug-stripped copy) rather than debug info, and carves out the source-path literals a sanitizer build writes into rodata | `8e099884` |
| **P3** | TradingView parity can block a merge: `scripts/check_corpus_parity.sh --subset` runs the same byte oracle over a 30-probe subset a pull request can afford, through a status context the ruleset already requires | `9b831117` |
| **P4** | The C surface's asymmetries: `PF_NATIVE_MARGIN_CHECK_FX_ROLL` under its own C name, a C host's recorded route to a closed row's exit ticket, and the calculation cadence on the migration page | `98b94672` |
| **P5** | The untested capabilities: the market-if-touched geometry of `Limit{price, fill_through}` kernel-only, `closed_trade(i)` as the closed row by reference, and a kernel liquidation seen through `native_open_lots()` under `NativeRunSpec::margin` | `41ca2ee8` |
| **P6** | The standing question "is a spec field the adapter never sets a decision or dead weight?" answered for all ten: `scripts/check_native_feature_rulings.py` and ADR-0001's ruling table, with three native examples added | `ceb07e4a` |
| **P7** | Gate hygiene: `RELEASE_MIN_TESTS`, example rows that assert the exit code **and** the summary line through `run_example.cmake`, and `check_adapter_spec_shadowing.py` promoted to a `ci_verify` source guard | `68f590aa` |
| **P9** | One fill simulation: the trail suites' resolver rows became a matcher-side projection, and the second simulation was deleted — see §2.ii row g | `aadfbe7e` |
| **Q4** | The per-bar broker hash is ruled **per driving mode**, pinned in both directions; the broker half alone is mode-invariant | `8e558eb9` |
| **Q6** | One producer for the range-end report rows, with the report shape measured against it — see §3.7 | `b16e11f6` |
| **Q9** | The runtime-budget row gates on process CPU time, not wall clock, so its verdict survives a 4x change in host load | `22e6878d` |
| **Q12** | The CTest row floor counts rows that **ran**; a skipped row is listed, not counted; and `assert()` is live in every `examples/native` target, which `ci_verify` refuses a configure without | `5a1723c4` |

Lanes the audits proposed and this branch carries rather than `main`: **Q5** (ADR
rule 2 against the kernel's TradingView-calibrated semantics — ADR-0001,
"TradingView-calibrated kernel mechanisms") and **Q10** (this section's §1
closure markers and `scripts/check_design_inventory.py`).

### 3.3 Lanes: API, neutrality proof, twin test

| Lane | API proposal (signature level → Appendix) | Neutrality proof (§3.1 a) | Twin test (§3.1 b) |
|---|---|---|---|
| L0 | — | docs only | — |
| L1 | option `PINEFORGE_BUILD_SOURCE_LAYER`, targets `pineforge_kernel` + unchanged `pineforge` (§2.i) | same TUs, same flags; ctest + corpus | kernel-only link of the two examples + the 20 source-free native tests; installed-header closure |
| L2 | `NativeReportPolicy { HostRecorded, KernelRecorded }` (R1 appended a third value, `KernelRecordedAtHostMarks` native_run_spec.hpp:63), `report_open_position_at_end`, `hash_host_extension`, `native_metrics()` → A.1 | at L2 the adapter stayed `HostRecorded`; since R1 it declares `KernelRecordedAtHostMarks` pine_adapter.cpp:1537, so the kernel records the same series at the adapter's own report marks — still one recorder, no double recording, `fill_report` bytes unchanged on the corpus | native example: curve length == script bars, drawdown equals a hand walk (F); 50-bar host asserts a non-empty curve, a finite max drawdown and a range-end row (O); same generic strategy through both hosts → equal rows and curve (S) |
| L3 | `Sized{side, basis, time, grid_policy, reserve_percent_fee}` as the 6th `OrderIntent`; `ScopeFraction{fraction, claim}` in `ReductionSize` → A.2 | at L3 the adapter emitted neither. Since R2 it lowers a default cash / percent opening frozen at the signal onto `Sized{CashValue, AtAcceptance, SignalOnTick, ExplicitUnits}`, since N11 its placement-time quantity is the kernel's `native_sized_units`, and since R5 lane F7 a typed quantity (`qty_type` cash / percent) converts through the same query and a staged FX series keeps `Sized` wherever the acceptance coordinate converts at the source's rate (`tests/test_adapter_sizing_relower.cpp` cases 1-4, 8, 11, 12). It still never emits `ScopeFraction` (case 5) | percent-of-equity probe, zero fee, no grid: exact; with grid / fee: differences explained by SZ10 only (F). Differential: `Sized` vs a host-resolved `HostSized` on the same facts → identical `ExecutionAppliedEvent` (O). Gap, reversal, fee-bearing first lot; two 50 % siblings, partial close, pending parent, replacement (S) |
| L4 | `NativeMarginModel`, `native_liquidation_price()`, `resolve_margin_call_units`, `on_native_margin_call`, `RequestDefinition::origin`, `MarginCallEvent` → A.3 | at L4 the adapter left `margin` unset. Since R5 `project()` declares a maintenance-only model (per-side `initial_*` 0, `maintenance_*` = `margin_long` / `margin_short`, `PathAdverseExtremeMark`, TradingView's equity basis) and answers its three hooks with TradingView's scheduling, money and slice (MG6, MG7), while every opening is still `AdmitWithHostMargin` (MG4) | first a plain maintenance breach in both hosts (S); then a margin-call probe reproduced with `ShortfallMultiple 4`: same bar, same side; quantity deltas itemized against MG15 (F, O) |
| L5 | `NativeCalculationTrigger`, `on_native_recalculate(…, reason, cause)`, `on_native_sub_bar`, `current_partial_bar()`, `NativeOpenBarView` → A.4 | the adapter keeps `BarClose` + its own cascade; `born_on_remaining_path` (native_execution_consumer.cpp:5272-5276) and `drain_applied_notifications` (native_execution_consumer.cpp:5753-5766) are frozen; accessors are inert | COOF probe re-expressed natively: same fills where the TV waypoint rule is inactive (F); a host that flips on its own fill produces the adapter's cascade in shape (O); callback reason, cursor, newborn eligibility and event ordinals compared (S) |
| L6 | `NativeTimeframeSubscription`, `on_native_timeframe_bar`, `native_series_bar` → A.5, §2.iv | `subscriptions` empty for the adapter; registration-gated pump; `engine_security.cpp` and pine_strategy_host.cpp:1316-1400 untouched; corpus diff before / after (O: highest parity risk of any lane) | HTF-filter probe on a 24x7 symbol: exact (F); `"D"` over a 15m feed equals a `TimeframeAggregator` baseline bar for bar (O); installed-feed and no-feed cases (S) |
| L7 | `native_working_requests()`, `cancel_all()`, `cancel_where(label)`, `TriggerAnchor`, `TrailTicks`, zero offset, `ReplaceOptions`; toolkit `submit_bracket`, `OrderBook` → A.6 | new kinds unused by the adapter; anchor defaults to `Absolute`; the adapter's `tick * 0.5` sentinel keeps working | bracket + trailing probe through the builder: exact where TR5 / FP7 are inactive (on-grid levels) (F); the zero-offset trail test asserts the run no longer fails (O); parent-limit + stop sibling, parent rejection, replacement, parent-cycle revival (S) |
| L8 | `NativePriceGrid { None, QuantizeFills, QuantizeFillsAndTriggers }`, `NativeGridRounding` → A.7 | `None` for the adapter, permanently — **native-only by ruling, audit lane P6: §3.6.2** | a host with `QuantizeFills` books on-grid prices; a second test pins that `QuantizeFillsAndTriggers` still differs from TV's half-tick threshold (FP5) (O) |
| L9 | `NativeRiskLimits`, `NativeRiskEvent`, `MatchRejectReason::RiskLimit` → A.8 | `risk` unset for the adapter; its ledger (pine_adapter.hpp:650-675) untouched — **retained after measurement, audit lane N12; native-only by ruling, audit lane P6: §3.6.1** | risk probes: same halt bar (F); each limit + the day-boundary basis (O); breach, forced close, cancellation, next-day reset (S) |
| L10 | `PINEFORGE_EXPORT_NATIVE_STRATEGY(Class)` (§2.v) | build-only | the examples run in ctest; the independence checker compiles the relocated examples |
| L11 | — (renames / moves, §2.ii a-i) | rename / move only; hashed enumerator values pinned; sweep unchanged | — |
| L12 | — (§2.ii j-m) | **not neutral**: coordinated sweep, waiver updates, hash-domain plan (the `"pineforge-broker-state/v17"` literal of that wave, since moved to `"pineforge-broker-state/v18"` engine_state_hash.cpp:32 and pinned to one occurrence by check_broker_state_hash_coverage.py:223) | trades identical, hashes re-baselined once | <!-- verified HEAD -->
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
| Day-one blockers (O) | L1, L2, L3, L10 | Without L2 `fill_report` silently returns an empty curve and degenerate equity metrics (engine_metrics.cpp:3) and drops an open final position. Without L3 every non-unit order needs a hand-written `resolve_execution_terms`, and a mistake is a run-terminating `TermsUnresolved` (native_execution_consumer.cpp:4744-4746). Without L1 "kernel" is a claim, not an artifact. L10 is the minimum buildable proof. |
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
(pine_strategy_host.cpp:964-986) — after `prepare_native_begin` has already
projected the spec (pine_strategy_host.cpp:228) and `configure_native` has
folded it into the continuation digest. Measured: an env-gated probe printing
the adapter's risk configuration inside `project()` on the corpus reads
`max_drawdown=0 cap.active=0` for every risk probe. `NativeRunSpec::risk` is a
begin-time declaration (`native_run_spec.hpp:579-586,504`, folded at
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
`native_run_spec.hpp:238-301` and native_execution_consumer.cpp:3066-3247
(evaluated at the script bar's open :6213/:6437, its close calculation
:6163, and after each applied drain :5924; fills counted at :4571; the block
refuses `would_open` at :2518).

| Pine rule | adapter mechanism | kernel counterpart | class | measured divergence (test scenario) | ruling |
|---|---|---|---|---|---|
| `strategy.risk.allow_entry_in` | `risk_.direction`; `project()` already declares `spec.allowed_open_directions` (pine_adapter.cpp:1541); the forbidden opposite entry is reshaped to `CloseOpposite` in the adapter's own terms branch | per-opening cap `allowed_open_directions` | (i)+(ii), **already re-lowered** | none to measure: the cap is the kernel's, the close-only reshaping is the retained TV policy | done before N12; nothing to move |
| `strategy.risk.max_position_size` | precommit refuses a flat/same-side ENTRY at its fill when the LIVE book already holds ≥ the limit (`max_position_size` pine_adapter.cpp:11530-11531; the retired legacy rule is `git show ab9714be:src/source/pine_risk.cpp`, lines 115 and 2531) | `max_abs_units`: refuses a fill whose RESULTING book would exceed it | (iv) | `PS`: two-unit entries against 3 — adapter admits the second (live 2 < 3, book 4) and refuses the third; kernel refuses the second (`MaxAbsUnits` at bars 1, 2), book 2 | retained: a pre-fill-book gate is a TV emulation fact, not a generic cap |
| `strategy.risk.max_drawdown` | `update_risk_state(bar.close)` once per script bar at the close mark (:14918, :12158-12185): peak / running max drawdown at close marks, `>=` exact, percent of the current peak; the latch `risk_.halted` gates flat/same-side entries at precommit (:11565), never an opposite entry | `max_drawdown`: current drawdown vs percent-of-peak, at all three points; `BlockOpenings` refuses every opening | (ii) cadence + (iv) scope | `MG10-a`: bar 1 opens 600 under the entry and closes 100 under — kernel breaches at the open mark and refuses bar 2's add (book 100), adapter admits it (book 200). `MG10-b`: latched at bar 1's close, the adapter still reverses twice (`L→S` −600, `S→L`), the kernel refuses both reversals (`RiskLimit` at bars 2, 3) and holds. Corpus: `ta-closedtrades-risk-introspection-01` (20 % of peak) identical 1502/1502 rows with the kernel seeded — the rule never fires there | retained: the latched-reversal exemption cannot be composed on a kernel block; a close-only cadence knob alone would not close it |
| `strategy.risk.max_cons_loss_days` | `SourceDayLedger` (pine_adapter.hpp:650-658): +1 per losing TRADE on a new chart day, reset to 0 by any winning trade at its fill (:651-671); gated immediately at precommit and latched at the close | `max_consecutive_loss_days`: a day's NET realized result settles the streak when the next day opens | (iv) | `MG11-a` (loss; win then loss; loss): adapter latches at day 3's first loss (4 rows), kernel's netted streak never reaches 2 (6 rows, no event). `MG11-b` (loss then smaller win, twice): adapter resets daily (5 rows), kernel blocks at day 3's open (4 rows, one event). No corpus probe declares the rule | retained: an order-dependent per-trade streak is not a generic day result |
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
`corpus/validation/cap-risk-gates-allow-max-intraday-01`'s generated strategy still emits the
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
| B2 | TradingView's tick quantization is a property of the ORDER KIND — stop and limit legs and a trail's activation (one-shot or trailing: lane E5; the placement close included: lane E9) are tested on the quantized bar, and a one-shot books the tick that bar reaches, its resting half-tick level rounded away from the position (lane E9); the trail stop, the running best, stop-limit entries and the `calc_on_order_fills` cursors on the raw path — while `price_grid` is a property of the RUN and the matcher hands one threshold to every trigger | **open, and not closable**: moved 30 pinned checks in 4 units when lane N13 last ran the re-lowering trial (`test_coof_market_limit_recross_l4c` 24, `test_stop_tick_rounding_l4d` 3, `test_adapter_grid_relower` 2, `test_zero_offset_trail_rides_l4c` 1) with no adapter-side remedy; 12 more checks in 3 units have an adapter-side cause. The one part of the row that WAS closable is closed (lane E14): where the running best STARTS is not a quantization at all but a level the leg names, so `Trail::best_seed` carries it generically and the kernel no longer begins a ride half a tick short of the activation. Lane E16 closes a second part, on the other side of the row: the trail stop's ULP — `best - offset` landing one binary64 ULP off the ladder point a tick count names, which is what made the grid fire `test_adapter_grid_relower`'s TrailUlp row a bar before the raw path did — was never the per-kind rule but a level the kernel spelled wrong on both paths. A stop a whole number of ticks from a best on the run's declared ladder now IS that ladder point, both paths agree on that row, and the trail stop and running best still ride the raw path. The 2 moved checks this row counts in `test_adapter_grid_relower` are that mechanism; the re-lowering trial itself was not re-run. |

The corpus cannot arbitrate: all 312 probes run a 0.01 tick on an on-grid
feed, and under the trial 5 of them differ, in the engine-only
entry-incarnation column only.

**The offset trail's arm (lane E5, closing lane P9's open question).** P9
found the adapter armed a trail WITH a trailing offset at its raw activation
while the one-shot's activation was quantized, and no tape pinned the
difference. TradingView's own tapes do (`tests/fixtures/offset_trail_arm`,
`lab tv`, ws-report-v1, `rangeProof` covered): on NYSE:F 15m with
`trail_offset` 1, lows 9.415 / 13.041 / 12.641 and highs 11.899 / 13.049 /
13.419 reach, only once quantized, the activations one tick past them, and 6
of 6 trades exit on that bar at activation -/+ 1 tick; the one-shot twins
exit on the same bars at the activation. On the on-grid ETH feed, levels
0.004 / 0.006 off the fill and `trail_points` 0.4 never arm on a bar that
touches the fill (0 of 32): the quantization is the path's, not the level's.
So the arm is B2's per-kind shape again and stays adapter policy — ADR-0001's
"Trail and tick conventions" already names the half-tick arm as the
adapter's, and the kernel's generic rule for a trail's arm under a declared
grid already exists (L8b); selecting it for the arm alone is the per-kind
mask above. The adapter now arms the generic `Trail` at the half-tick
threshold its one-shot leg rests at (`source_trail_arm_level`) and books from
a running best that is never short of the activation, because TradingView's
starts there. The kernel is unchanged and the corpus byte-identical (on-grid,
the two arms admit the same prints); the population sweep over sub-tick feeds
is the real gate. One residual was left, of the same kind: the kernel's best
started at the arm, half a tick short of the activation, so while the raw
extreme stayed inside the activation's tick cell its stop sat up to half a
tick further out than TradingView's and a print between the two fired only
TradingView's (0 of the 6 tapes). Lane E14 closed it, below.

**The one-shot's booked price and the placement print (lane E9, closing
E5's findings).** Two more `lab tv` sets
(`tests/fixtures/trail_activation_tick_reach`, replayed by
`tests/test_trail_activation_tick_reach.cpp`) pin the rest of the rule. A
one-shot (`trail_offset` 0) with a sub-tick `trail_price` — fill 2550.85,
level 2550.854 on ETH 15m — rests at the half-tick boundary 2550.855 and
TradingView books the tick the quantized path reaches there, 2550.86 (5 of
5; the short twin books fill - 0.01, 4 of 4): the resting level rounded AWAY
from the position. The adapter booked the stop-style snap toward it (2550.85),
a price its own resting limit forbids, and the kernel's generic
limit-or-better check refused it as `InvalidTerms` — correctly, so the kernel
is untouched and the adapter books the reach tick
(`source_one_shot_reach_tick`). And a placement close half a tick short of
the activation whose tick IS the activation (NYSE:F 11.295 under 11.30, seven
longs and a short) counts as already reached: 8 of 8 trades exit on the next
bar, `trail_offset` 1 and 0 alike, at activation -/+ the offset, so the best
of a trail armed at placement starts at the activation too
(`source_trail_reached_at`), which until lane E14 the kernel's own best did
not. Both are adapter policy for the reason above and
byte-identical on the on-grid corpus (no print lies in a half cell; the
corpus sets no sub-tick `trail_price`); the population sweep over sub-tick
feeds is the real gate. TradingView's compiler refuses a trail without
`trail_offset` on its own, so the omitted-offset shape has no tape and follows
the same compare.

**Where the running best starts (lane E14, closing both lanes' residual).**
E5's residual and E9's second finding are one sentence: TradingView's running
best starts AT the activation, the kernel's started wherever the arm landed —
the half-tick threshold on a crossing, the next bar's first print when the
placement close armed it. Neither is a quantization, so neither is B2's mask;
they are the absence of any way for a leg to say where its ride begins. The
kernel gained one, `native_order::Trail::best_seed`: absent it is the arm
print, byte for byte as before, and present it is a floor on the start — the
favourable one of the seed and the arm print. It passes rule 2 as amended.
Mechanism: a trail that rides from its activation instead of from the next
print is a broker shape a second venue asks for, and the level is data the
host hands over, as an `arm_price` is; the kernel neither derives it nor
knows what named it. Knob: today's start was not a choice the run had already
made another way — there is no second spelling of it — so a request field is
admissible rather than a hook. The adapter names the activation, or the
favourable one of the activation and the placement print when that print
armed the leg, and the kernel's own crossing then IS the price the adapter
books: three trail twin suites move their `raw_price` / `path_position` /
leg-kind rows onto it, with no booked price, bar or quantity changing. The
corpus stays byte-identical 312/312 (on-grid: the arming bar's own extreme is
folded into the best before any adverse move, so the seed changes nothing
there); the sub-tick sweep is the gate. What is NOT redundant afterwards is
the sibling stop an explicit-zero trail rests beside its `Trail`: a `Stop` is
reached by a touch and a zero-distance ride needs a move strictly past the
best, and a print landing ON the carried best separates them in the booked
price's last bits.

**Anchored relative legs: an open measurement (lane F7).** A trailing
`strategy.exit` issued while its entry is still pending is materialized as the
kernel's anchored child at the fill (`relative_leg_shapes`, M17) and names no
`best_seed`: its ride starts where the kernel arms it. None of the 23 E5 / E9 /
E14 tapes measures that shape — every probe issues its exit once the entry has
filled (`strategy.opentrades.entry_bar_index(0) == bar_index`) — so whether
TradingView's best starts at the activation there too is unknown, and F7
records it rather than changing it. The tape it needs is the E14 shape with the
exit written on the entry's signal bar: a fill bar that reaches the activation
on its tick, a shallow next bar.

**The trail stop's own ULP (lane E16, closing E14's finding).** E14's tape
left one trade of `e14-f-long-shallow-next` pinned as a recorded divergence:
its stop is `11.44 - 5 ticks`, whose binary64 value `11.389999999999998792`
lies one ULP UNDER the ladder point `11.390000000000000568`, so the bar's low
of exactly 11.39 did not reach it, TradingView booked the exit and the engine
ran to the timeout. Lane R7 had measured the same ULP from the other side —
`best - offset` lands one ULP off its ladder point on about 14% of (best,
offset) pairs on a two-decimal feed, which is why `test_adapter_grid_relower`'s
TrailUlp row exits a bar later on the raw path than under the grid — and read
it as B2's per-kind rule. It is neither. A stop that stands a whole number of
ticks from a running best that is itself a ladder point IS the ladder point
that many ticks away; the subtraction simply could not name it, because both
ends are decimal numbers the host measured on its own ladder and only the
ladder index is exact between them. The kernel derives it there now
(`native_matching::ladder_trail_stop`, with the run's declared tick reaching
the core as `ActivationGrid::ladder_tick` so the activation's re-validation
cannot refuse a hit the matcher booked), and it passes rule 2 as amended.
Mechanism: a stop `ticks` ticks behind a best is a ladder distance at every
venue with a tick ladder, read from the run's own `price_tick` — data, no
platform. Knob: the run had already decided it, by declaring the ladder and
spelling the distance on it, so a "raw or ladder" switch would put a
hash-visible choice on the surface for an answer already given. Nothing is
rounded ONTO the ladder — a sub-tick best, a fractional-tick offset and a run
with no declared tick keep the raw subtraction bit for bit, so the trail stop
and the running best still ride the raw path and row TR2's "a tick-spelled
trail and its price-spelled equal behave identically bar for bar" still holds
(the rule reads the resolved distance, not the spelling) — and it is the level
that moves, never the comparison. The kernel-only witness is
`tests/test_native_trail_stop_ladder.cpp` (batch and stream, the sell case and
its buy mirror, three off-ladder rows pinned bit for bit); the tape row is
`e14-f-long-shallow-next` 7/7 and TrailUlp exits on TradingView's bar. The
corpus is byte-identical 312/312 — measured, not assumed: the on-grid 0.01
feed does print exactly on a ladder stop, but no probe's trade moved.

**Why B2 is not closed in the kernel (route (a) rejected).** The only kernel
change that would let `project()` declare the grid is a per-order-kind
quantization mask: quantize a stop leg but not a trail stop, a trail's
activation but not its running best, a limit but not a stop-limit. No broker
model asks for that; it is one emulator's inconsistency, and the kernel's own
rule (L8b) is the opposite on purpose — one rule for every kind. Rule 2's test
applies literally: the mask cannot be justified without the word
"TradingView". And what the adapter keeps is not a duplicate of the kernel
mechanism that a re-lowering would delete: `source_trigger_threshold`
(pine_adapter.cpp:323), `source_level_on_price_grid` (:309) and the
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
(`PF_NATIVE_SPEC_EXT_PRICE_GRID`, translated at native_c_host.cpp:2990-2994)
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

### 3.7 Audit lane Q6 — the range-end report rows (§1.8 RP5, duplicate D5): one producer, a measured report shape

The second independent R5 audit (§6, lane Q6) found the only first-round
duplicate neither gap wave touched: `record_open_position_report_rows` ran a
`build_close_trade_with_costs` loop with `open_at_end = true`, gated to
`NativeReportPolicy::KernelRecorded`, which the adapter can never select
because `project()` declares `KernelRecordedAtHostMarks`; and
`PineStrategyHost::scheduler_record_range_end` ran the same loop again, with
a prose reason and no measurement. This section is the ruling, in two parts.

**Part 1 — the loop is re-lowered, not measured.** The row loop is now one
generic producer,
`NativeExecutionConsumer::append_open_position_report_rows(engine, mark_price,
mark_time_ms, interval_index)`: one `open_at_end` row per open physical lot
through the same non-mutating builder a full close uses, returning the summed
NET row P&L. It is policy-free — the caller decides when to mark, what to
clear first and what it re-derives from the rows — so the kernel's run-end
producer and a host with a different report shape share it instead of each
restating it. The adapter's own loop is gone; `scheduler_record_range_end`
calls the producer at TradingView's mark. Ruling: **a mark-to-market row per
open lot is a generic capability; when to mark and what to re-derive from the
mark is not.**

**Part 2 — the report shape around the loop stays in the adapter, measured.**
TradingView's range-end report is not a mark-to-market row, so the kernel's
run-end producer stays gated out of every Pine run
(`report_policy = KernelRecordedAtHostMarks`,
`report_open_position_at_end = false`). What the adapter keeps is the shape,
and every row below is an executed assertion of
`tests/test_adapter_range_end_relower.cpp` — four paired scenarios, each run
through a `PineStrategyHost` and through a bare `NativeStrategyHost` with
`KernelRecorded` + `report_open_position_at_end` on the same tape, the
adapter half harvested from this lane's base `b8e7976e` and reproduced bit for
bit after the re-lowering.

Common tape: hourly, capital 100000, 100 units a lot, point value 1, fx 1,
slippage 0, `process_orders_on_close`; two lots at 100 and 102 marked at 105,
so the gross open profit is 800.

| retained mechanism | adapter site | kernel counterpart | measured divergence (scenario) | ruling |
|---|---|---|---|---|
| **the rows themselves** | `scheduler_record_range_end` → the shared producer | `record_open_position_report_rows` → the same producer | **none — this is the re-lowering.** `RE1`/`RE2`/`RE3` assert both halves produce the identical range-end rows: side, units, entry price, mark price, P&L, commission, run-up, draw-down, `open_at_end`. With no fee: `pnl=500` and `pnl=300`. With 0.1 %: `pnl=479.5 comm=20.5` and `pnl=279.30000000000001 comm=20.700000000000003` on both sides | re-lowered; nothing left to retain |
| **the equity re-mark** | `last.open_profit = 0; last.equity = initial_capital_ + net_profit_sum_ + range_end_pnl` | none — the kernel leaves the curve exactly as the run marked it | `RE1` (no fee): the adapter reports the curve's last point as `eq=100800 op=0`, the kernel as `eq=100800 op=800` — same equity, the open profit moved into the realized side. `RE2` (0.1 %): `eq=100758.8 op=0` against `eq=100800 op=800`; the adapter's equity is **41.2 lower**, exactly the round-trip commission of both lots (20.5 + 20.7), because the re-mark carries the NET row P&L where the run's own mark carried the GROSS open profit | retained: a report that restates the last equity point off net row P&L is TradingView's report shape. Re-marking the curve from the kernel would move a figure every bare host already reads |
| **the extreme re-fold** | `max_equity_`/`min_equity_` reset to `initial_capital_`, `max_drawdown_`/`max_runup_` to 0, then `fold_equity_extreme` over the whole curve | none — the extremes stay as `update_equity_extremes` sampled them | `RE3` (a dip to 90 at bar 1, a peak of 108 at bar 3, 0.1 %): both sides agree on `maxeq=101400 mineq=99000 maxdd=2400`, and the run-up diverges — adapter `maxru=1758.8000000000029`, kernel `maxru=1800`. The re-fold is a no-op **except** where the re-marked last point differs, so the divergence is again the 41.2 round trip, modulo the binary fold order | retained: it is a consequence of the re-mark above, not a mechanism of its own; it cannot be re-lowered while the re-mark is adapter policy |
| **the same-bar exit re-sort** | `sort_same_bar_exit_trades(trades_, adapter_)` — same-bar bracket exits ordered by script command sequence | none — the kernel never reorders `trades_` | not exercised by the four scenarios (it reorders CLOSED rows, not range-end rows); it is TradingView command-sequence ordering by construction (`git show ab9714be:src/source/pine_fills.cpp`, lines 664-670) | retained: script command sequence is a source-language fact |
| **the mark cadence** | three call sites, each clearing and rebuilding: the terminal sub-bar of the last source bar (`scheduler_record_range_end` pine_strategy_host.cpp:449), after every applied execution once terminal (`record_applied_range_end`, :656), and the scheduler's terminal-source-bar hook (`PineScheduler::applied` pine_scheduler_native.cpp:552, before the bar's broker hash) | once, after the last point of the run (`run_batch` ×3, `stream_end`) | structural, not a number: the adapter's rows are re-derived at each mark so the report reflects the book as of the last event on the terminal bar, and its re-marked equity is visible to that bar's broker hash. A one-shot run-end producer cannot occupy those three points | retained: three marks on a terminal bar is a TradingView report cadence, not a generic run-end fact |
| **the excursion projection gate** | `excursion_range_end_projection_` suppresses the owner's exit-bar path fold for the projection (`excursion_range_end_projection_` pine_strategy_host.cpp:874) | none — the generic builder folds what it folds | `RE1`: identical run-up/draw-down on both halves (`ru=500 dd=0`, `ru=300 dd=0`) because the adapter's own owner hook returns the carried magnitudes for a projection; the gate exists to keep it that way when a host owns lot excursions | retained: it is the host's excursion contract (`owns_lot_excursions`), which the kernel deliberately does not model |
| **the tail / warm-up suppression** | `if (stream_warmup_mode_ \|\| realtime_tail_ \|\| equity_curve_.empty()) return;` | none — the kernel emits at `stream_end` whatever the run left open | structural: the kernel has no warm-up or realtime-tail notion, and `run_corpus`'s probe-tail suppression (`suppress_probe_tail`) is a harness fact | retained: a tail is a source-layer replay concept |

**Control and witness.** `RE4` closes the lot at bar 2: neither side emits a
range-end row (`closed=1 report=1`, `op=0`, extremes equal on both), so every
divergence above is attributable to the range end and not to the tape. `SW`
asserts the structural blocker directly — an adapter run's projected spec is
`KernelRecordedAtHostMarks` with `report_open_position_at_end == false`, i.e.
the kernel's run-end producer is unreachable from Pine by construction.

One difference in the literals is **not** a range-end divergence and shows on
the flat control too: the adapter dates a row on the script bar's OPEN and the
bare host on its close-execution point, so every `@hour` label is one bar apart
between the halves. The mark itself is the same bar on both.

**A dead branch found and reported, not removed.** `fold_exit_trail_peak_`
(engine.hpp:541) is initialised NaN, reset to NaN at run start
(engine_run.cpp:109) and **never assigned a finite value anywhere in the
tree**, so all four `if (!std::isnan(fold_exit_trail_peak_))` carries
(engine_execution.cpp:483/:493, the former adapter loop, the shared producer)
are inert. The shared producer keeps the carry so it folds exactly what the
kernel's settling path folds; removing the member is an epoch question (it is
hashed at engine_state_hash.cpp:71) and is left to a lane that budgets one.

**Acceptance evidence.** `tests/test_adapter_range_end_relower.cpp`: 12
checks, the adapter half reproduced bit for bit against the `b8e7976e`
harvest; a mutation self-test (`marked += row.pnl + 1.0` in the shared
producer) fails 3 of the 4 scenarios **through the adapter half**, which is
the executed proof that the adapter now runs the kernel's producer; whole
corpus byte-identical at gitlink 442d497.

### 3.8 Audit lane F1 — the aggregated chart, as TradingView draws it

The final audit (AUDIT3-opus §3.2, §7 F1) reproduced three Pine-visible
regressions that R4 slice C (`73817c1d`) introduced, two of them on a chart
whose script timeframe is coarser than its input feed (an *aggregated*
chart). No gate could see them: neither aggregated corpus probe reads a bar
index or runs `process_orders_on_close`, and the two SPY session probes have
no baseline rows. The oracle is the pre-R4 engine `ab9714be`, whose plain
aggregated run of one script equals its chart-timeframe run field for field,
plus TradingView's own tapes (`tests/fixtures/session_islastbar`).

**Ruling (supervisor, ADR-0001 rule 2):** all three are TradingView
presentation conventions of the adapter and are fixed in the source layer.
The kernel's interval index, its calculation instant (the instant its FX
lookup, decision floor and ordering read) and every hash recipe are
untouched; no epoch moves.

| ID | What TradingView and `ab9714be` do | What R4 slice C did | Owner and fix | Proof |
|---|---|---|---|---|
| AG1 | A trade row counts **chart** bars: its entry and exit bar are the `bar_index` the script read there, so `bar_index - strategy.opentrades.entry_bar_index(0)` is the bars held, and a tape row one chart bar long says "Duration (bars)" 1 | The kernel books a fill at its interval index, which on the no-path aggregated route names the INPUT bar the script bucket opens on (390 for the 26th 15m bar of a 1m feed). Only the magnifier re-stamped its rows, so on a plain aggregated chart `held` read −364, the held-two-bars exit never fired (1 open trade where `ab9714be` closes 2), and every `Trade` / `PyramidEntry` / `pf_trade_t` bar index and `avg_bars_in_*` followed | **Adapter.** `PineStrategyHost::on_native_applied` pine_strategy_host.cpp:466 re-stamps the lot a fill opened and the rows it closed with the chart bar on every aggregated chart (`aggregates_input_bars` pine_strategy_host.cpp:65), as it always did under the magnifier. Every adapter test of "did this lot open on the current bar" reads the chart bar there too — the extreme samples (`on_native_bar` pine_strategy_host.cpp:402, `on_native_tick` pine_strategy_host.cpp:370), the slippage mask, the same-bar masks of `closed_lot_excursion` pine_strategy_host.cpp:793, the `calc_on_order_fills` sample in `PineScheduler::recalculate` pine_scheduler_native.cpp:582 and the KI-62 same-bar add cover in `PineExecutionAdapter::on_applied` pine_adapter.cpp:14882 — so nothing but the index moves. A stream's quiet carried interval, matched at its own open while the context still names the bar before it, is booked on the bar it opens into (`PineScheduler::source_bar_index_for` pine_scheduler_native.cpp:164) | `tests/test_aggregated_path_regressions.cpp` rows 1-2 (the audit's heldbars and next-open probes, `ab9714be`'s rows on the chart, plain aggregated and magnified paths), row 4 (the chart run equals the plain aggregated run in every field, eight combinations of `process_orders_on_close`, `calc_on_order_fills`, slippage and pyramiding) and the KI-62 add-on row; lane E27's bar-index row of `tests/test_pooc_fill_stamp_aggregation.cpp` flipped |
| AG2 | Every fill of a chart bar is dated at the bar's **open**: the e25-f-islastbar tape dates all 510 of its `process_orders_on_close` rows there, and `ab9714be`'s aggregated rows equal its chart rows | The kernel dates a close point (the calculation, the close after it, a modeled bar's close leg) at `calculation_time()`, the bucket's close, and a fill the host executes at the current point (a `calc_on_order_fills` first-open chain, a same-bar close-then-entry, the KI-62 cover) at its decision floor, which on the no-path route the bucket's inputs have already carried to the close. The chart timeframe's zero-width interval hides both, since every point of a bar sits at its label | **Adapter, at projection time.** `on_native_applied` dates the lot a fill opened (`PyramidEntry::time`, hence `Trade::entry_time`) and the rows it closed (`Trade::exit_time`) at the chart bar's open: on a plain aggregated chart every point of a confirmed bar's modeled path (its open, the segments, the calculation, the close after it), where the chart timeframe's own points all sit; under the magnifier the close points alone (lane E27's measured cut), so its sub-bar fills keep their sub-bar instant as `ab9714be` dated them. A realtime print keeps its own instant, as the chart-timeframe stream keeps it, and so does a stream's quiet carried interval | the audit's pooc probe rows (`ab9714be`'s 19:45Z / 13:30Z on all three paths), row 4, the KI-62 row and the stream row (a chart-timeframe stream and the same stream fed 1m agree on every row, a 10:30:01 print fill and an 11:00 carried fill included); E27's stamp rows flipped. Measured: the chart path unchanged byte for byte, the magnified run moves only its POOC close-fill instants |
| AG3 | A `calc_on_order_fills` recalculation runs the script **on the bar the fill is on** and reads that bar's `session.ismarket` / `isfirstbar` / `islastbar`; `ab9714be` set them before any fill of the bar | The scheduler set the flags in the close callback only, so a recalculation published the PREVIOUS bar's: a fill at the next session day's 09:30 read `islastbar`. On the chart timeframe the close then read its lookahead from a count the recalculation had already advanced, one retained bar too far: a fill on 15:30 flagged 15:30 `islastbar` and, through E26's dual, 15:45 `isfirstbar` | **Adapter** (Pine language state, OT7). `scheduler_update_session_state` stays the one writer and runs once per script bar, at the bar's first publication: `PineScheduler::recalculate` asks before the script runs, `PineScheduler::bar` pine_scheduler_native.cpp:449 only when no recalculation published the bar. Since R5 lane F5 it selects the kernel's session-day facts of that bar, which every callback of the bar carries alike, so the scheduler's lookahead is gone. E26's session-day rule and its `isfirstbar` dual are unchanged | the audit's coof_session probes and the 15:30 case in `tests/test_aggregated_path_regressions.cpp`; section 7 of `tests/test_session_islastbar_aggregation.cpp` (TradingView's NYSE:F days with `calc_on_order_fills`, chart, aggregated and magnified: every callback of a bar reads the tape's flags) |

**Recorded, not changed here.**

- Under the magnifier on an aggregated chart, three entry-bar tests (the
  slippage mask, the same-bar masks of `closed_lot_excursion`, the
  `calc_on_order_fills` extreme sample) compare a lot's re-stamped chart-bar
  index with the kernel's input-bar index, as they did before this lane (an
  entry on chart bar 26 closed on input bar 435). Changing them moves
  magnified outcomes and needs its own measurement against a tape.
- The per-bar broker hash of an aggregated run moves with the re-stamped lot
  and row values; the recipe does not, and no pinned hash covers such a run.
  The magnifier re-stamp has always moved it the same way.

---

### 3.8 Audit lane F9 — recording and continuation costs (E24's STOP bucket)

Per-bar recording (`set_broker_state_hash_recording(true)`, §1.8 RP9) appends
one `broker_state_hash()` per script bar, and every row is a whole fold: the
kernel's broker state, then the host's extension. Lane E24 made the
continuation's own logs fold by their tail and left a STOP bucket; both final
audits (Opus F9, Codex A3-8) carried it forward. Each cost it holds is
measured here on `fd785928` (process CPU, best of 3 to 5, host load 7 to 26)
and ruled. None of them touches a run with recording off: that path latches the
scalar once per run, and its values are what they were (since R5 lane PERF-P1
that latch is a view folded on first read, row C).

| # | cost | measured on `fd785928` | ruling |
|---|---|---|---|
| **A** | the Pine adapter's recording fold: O(retained history) per row | ×3.92 to ×4.02 per doubling — 9.26 s at 800 bars of order-and-cancel, 19.5 s at 2,688 bars of re-issue — against at most 0.015 s with recording off | **open**: blocked on storage and a write barrier outside lane F9's files; no epoch needed; value witness pinned |
| **B** | the kernel recorder's closed-row walk: O(closed rows) per row | ×2.61 rising to ×3.64 per doubling with trades (0.80 s at 24,000 bars and 1,200 closed rows) against ×1.94 to ×2.08 without | **ruled**: retained, cost documented |
| **C** | the terminal continuation capture (E24 STOP1) | 0.048 to 0.052 s, 23 to 24 % of the 43,008-bar gated replay | **revised** by R5 lane PERF-P1 (supervisor ruling): latched as a view, folded on first read; no value moves |

**A. The adapter's recording fold.** `PineExecutionAdapter::hash_state`
pine_state_hash.cpp:236 folds, at every row, every placement snapshot the run
has accepted (`placement_keys` pine_state_hash.cpp:278-281) — one per accepted
request, cleared only by `reset_for_run` pine_adapter.cpp:1294 — and reflects
the whole admission journal (`admission_journal` pine_state_hash.cpp:561).
`PineScheduler::hash_state` pine_state_hash.cpp:564 re-folds the consumed
source prefix too (`consumed` pine_state_hash.cpp:568-574). Measured with
recording on and off, in two shapes:

- the audit's order-and-cancel probe (one `strategy_order` accepted and
  cancelled per bar): 200 / 400 / 800 bars = 0.588 / 2.305 / 9.262 s (×3.92,
  ×4.02), against 0.0014 / 0.0012 / 0.0023 s off;
- the runtime-budget replay's own strategy (`ReissueReplay`
  test_l4g_runtime_budget.cpp:73, an exit re-issued every bar) on its 672-bar
  tape repeated 1, 2 and 4 times: 1.247 / 4.929 / 19.497 s (×3.95, ×3.96),
  against 0.0044 / 0.0078 / 0.0151 s off. At the gated 43,008 bars that is
  about 80 minutes, against 0.21 s.

A `sample` call tree of each run splits the adapter fold: the placement
snapshots are 55 % of it on the order-and-cancel shape and 97 % on the
re-issue shape, the journal reflection the other 45 % and 1 %, the scheduler
prefix under 2 %. Most of a snapshot's bytes are field paths the reflection
builds as strings, one per field (`Reflect::field` market_admission.cpp:190-193).

Why it is not fixed in this lane. The fold can be made incremental without
moving a single value, but not from inside lane F9's files:

1. **No epoch is needed.** The sink is FNV-1a 64 (`BrokerStateHashSink`
   engine.hpp:346), and FNV-1a over a fixed byte segment S is an affine map
   with a low-byte-indexed offset, T_S(h) = p^|S|·h + C_S[h mod 256]
   (mod 2^64); two such maps compose in 256 steps. A row folded once can be
   applied in O(1) to whatever state the fold arrives with, byte for byte.
   Lane F9's probe checked the identity 30,000 times against the sink's own
   byte chain, with no mismatch.
2. **The transforms need a home.** A row's transform, or a composed
   segment's, has to outlive the call. The fold is const; the sink is created
   per call (engine_state_hash.cpp:31); and none of `PlacementTable`
   pine_adapter.hpp:318, `admission::Journal` market_admission.hpp:188 or
   `PineScheduler` has a member it could keep one in. Adding one is a header
   change in `include/pineforge/source/`, outside this lane.
3. **A cache must know which rows changed, and nothing records it.**
   `PlacementTable` hands out mutable rows — the non-const `find`
   pine_adapter.hpp:401, the non-const `at` pine_adapter.hpp:410 and its
   iterators — and the adapter rewrites retained rows in place through them,
   not only rows still working. Lane F9's probe hashed every row callback to
   callback: the order-and-cancel shape rewrites none, the re-issue shape
   1,339 rows each one callback old, an OCA re-price and a trail attach rows
   two callbacks old, and the row of an entry that filled at bar 1 is
   rewritten at bar 60, when its first exit is attached
   (`has_full_entry_bracket` pine_adapter.cpp:7065). Full-table scans
   rewrite rows as well (`suspend_brackets_for_reversal` pine_adapter.cpp:1004,
   `revive_brackets_after_margin` pine_adapter.cpp:1156,
   `preserved_by_close_all` pine_adapter.cpp:9769), and
   `admission::Journal::retain` market_admission.hpp:205 drops events from
   the middle of the journal. So "a row stops changing once its request is
   done" is false, and a cache resting on it would move a value.

What the lane that fixes it needs: a per-row revision (or a `mutate()`
accessor) that `PlacementTable` bumps whenever it hands out a mutable row,
with the adapter's read-only lookups and scans moved to the const accessors so
that they stop bumping it; a home for the composed transforms beside it; the
same for the journal (an append composes, `retain` and `reset` rebuild) and
for the scheduler's consumed prefix. That is at least
`include/pineforge/source/pine_adapter.hpp`, `market_admission.hpp` and
`pine_scheduler.hpp`, `src/source/pine_adapter.cpp` and `market_admission.cpp`
beside `pine_state_hash.cpp`, plus a scaling row that fails the reproduced
×3.9 to ×4.0 per doubling and passes at ×2.3 or less. The alternative — fold
one cached digest per row instead of the row's bytes — needs the same barrier
and the same home, saves only the 256-entry tables, and moves every Pine
hash: an epoch decision (`pineforge-source-adapter/v3` to v4), not a neutral
change.

**The witness, pinned now.** `tests/test_adapter_recording_hash_witness.cpp`
records every row, a read after every command and the final scalar of six
scenarios — submit, replace, cancel, OCA, re-issue with a late bracket, trail —
on a fresh host, on the same host run twice (a reset answers the fresh values)
and with recording off (the same state at every read): 360 checks, harvested on
`fd785928`. Its projection folds a fixed execution hash, as
`tests/test_adapter_report_relower.cpp` does, so the pins do not depend on the
installed tzdata. Three stale-fold mutations fail it at the first point that
moves: never re-folding `has_full_entry_bracket` (first at the re-issue
scenario's row 8, the bar its late bracket attaches), freezing the cancel
receipts of older rows (first at each cancel or re-price), and a
consumed-prefix cache that stops at 16 bars (row 16 in all six scenarios).

**B. The kernel recorder's closed rows.**
`broker_state_hash_from_execution_hash` engine_state_hash.cpp:29 folds six
fields of every closed row (`trades_` engine_state_hash.cpp:107-112) at every
recorded row, so the recorder costs O(closed rows) per row. The audit's bare
host — one round trip per 20 bars, one request accepted and cancelled per bar
— from 1,500 to 24,000 bars and 75 to 1,200 closed rows: recording 0.0087 /
0.0228 / 0.0676 / 0.2201 / 0.8014 s (×2.61, ×2.96, ×3.26, ×3.64); the same
with no trades 0.0056 to 0.0884 s (×1.94 to ×2.08); trading with recording off
0.0038 to 0.0596 s (×2.00). Recording without trades stays linear, so the
growth is the closed-row walk: about 0.71 s of the 0.80 s at 24,000 bars,
some 48 folded bytes per closed row per recorded row.

**Ruling (ADR-0001 rule 2: a cost ruling for every host, with no TradingView
fact in it): the walk stays, and this is its documented cost.** An exact
running digest is possible by the same algebra as A and would move no value,
but it needs two things the kernel does not have. One is storage for the
composed transform: a `BacktestEngine` member changes the layout of the
`engine_script_run_v18` script ABI, which is an epoch decision, while a
member of the heap-owned execution consumer (`ExecutionConsumerSlot`
engine.hpp:1633) is not. The other is a contract that the six folded fields of
a booked row are never rewritten. In the tree they are not; the only writer
after booking, the Pine host, rewrites `close_cause`, `exit_from_bracket`,
`exit_bar_index`, `exit_id`, `exit_comment` and `entry_incarnation` only; its
writes are pine_strategy_host.cpp:452, :536-537, :549-557, :1153 and :1341-1343.
But `trades_` engine.hpp:542 is a protected member any host can write, so a
lane that takes the consumer-side digest has to make that contract explicit.
Folding `(count, digest)` in place of the rows saves the 256-entry table but
still needs the storage and the contract, and it moves every broker hash: an
epoch decision (`pineforge-broker-state/v18`).

**C. The terminal continuation capture (E24 STOP1).** A Pine run's scalar
`broker_state_hash()` folds the continuation as it stood at the run's last
script point, not after the run's teardown, so that it is one value with
recording on or off (`broker_state_hash_projection`
pine_strategy_host.cpp:158-166). `capture_script_continuation_hash`
pine_strategy_host.cpp:341-347 takes that snapshot at the last batch bar
(`last_batch` pine_strategy_host.cpp:443-453), and taking it is one full
continuation fold per run. Measured on the gated replay — `ReissueReplay` on
43,008 bars, `run()` process CPU, best of 5, three rounds — it is 0.2116 /
0.2137 / 0.2141 s with the capture and 0.1632 / 0.1614 / 0.1652 s with it
suppressed (a probe build only): 0.048 to 0.052 s, 23 to 24 % of the replay.
E24 put it at about a third; the Opus final audit at 22.0 %. Suppressing it is
not neutral: the final scalar moves from 235509512418453834 to
14899872126237769279 (at 2,688 bars from 9192851617711936750 to
1972323229210109824), because the fallback folds the continuation after the
terminal point.

**Decision (F9): the capture stays eager.** Deferring it byte-identically needs a
captured continuation *view* — the consumer's state at the terminal script
point, kept until someone asks — which is a kernel design of its own; any
other deferral moves the scalar and is an epoch question. The capture is one
linear fold per run, inside the runtime budget (8.4 to 8.7× against 15× in
the final audits), so neither is worth taking now.

**Revised by R5 lane PERF-P1 (supervisor ruling, 2026-09-23): the capture is a
view.** E24's STOP bucket and the decision above were campaign design notes,
not owner rulings; the owner's red line is hash values, and a view keeps every
one of them by construction. PERF0-P measured the capture at a fifth of every
Pine run — about 56 ns per driver point, four points a bar — paid although
neither the benchmark, the C report nor the corpus sweep reads the scalar.
`capture_script_continuation_hash` pine_strategy_host.cpp:342-356 now latches
a view (`capture_continuation_view` native_execution_consumer.cpp:1905-1929)
unless a recorded row needs the value at once. The view is
`continuation_hash()`'s own fold run into a recording sink (`FnvRecord`
native_execution_consumer.cpp:128): the bytes the fold would consume, with the
command history's and the driver log's digests left as holes at the logs'
lengths. The first read (`latched_continuation`
native_execution_consumer.cpp:1939, from both `broker_state_hash_projection`
overrides) carries both digests to exactly those lengths and folds the bytes
around them. It is exact because both logs only grow within a run, and two
rules keep it so: `continuation_hash()` folds a pending view before it moves
the digests (`fold_continuation_view` native_execution_consumer.cpp:1890), and
`begin_ready` drops a view before it clears the logs (`drop_continuation_view`
native_execution_consumer.cpp:2320). A recorded row and a KernelRecorded
report point still fold at once, and a reader pays the skipped fold once, at
its first read. The witnesses are value for value:
`tests/test_native_continuation_view.cpp` (a bare host, three spellings of one
latch) and `tests/test_adapter_continuation_view.cpp` (every Pine capture
site) assert eager == deferred at every read and pin the values of `fc7aad62`;
PERF0-P's fingerprint sweep (100 public slots, magnifier off and on) and
PERF0-K's 132-configuration battery are identical before and after. Measured
(process CPU, interleaved): the gated replay 0.2618 → 0.2029 s on macOS
(7.71–9.08× → 6.58–7.19× of ab9714be, load 36–41) and 0.4054 → 0.3606 s on
spark (12.87× → 11.45×); the Pine benchmark 0.84× on spark, the median of the
14 shared probes and of the 100 public slots, magnifier off and on.

---

## 4. Risks and open questions

### 4.1 Entanglements: a generic feature tangled with a TV quirk

| # | Risk | Why it bites | Containment | Src |
|---|---|---|---|---|
| E1 | **Spec hashing.** `hash_spec` folds every field unconditionally (native_execution_consumer.cpp:233-255) | Any new field, even defaulted, changes every continuation / broker-state hash, adapter runs included | Fold new blocks only when set, as a tagged extension (precedent native_execution_consumer.hpp:414-418); test that a default spec hashes to the pre-change constant | F |
| E2 | **Margin.** The TV margin call is built host-side from kernel primitives (a Stop-triggered `Reduce` bound to openings, or `execute_current`; pine_adapter.cpp:11445-11512, pine_adapter.cpp:11559); its chronology lives at pine_adapter.cpp:12063-12140 and its excursion sample differs by chronology (pine_strategy_host.cpp:709-739) | A kernel model under an adapter run would liquidate twice or reorder settlement; a checkpoint that reduces mid-path changes lot state exactly where the adapter's chronology lives | Superseded by R5: the adapter declares `spec.margin` — maintenance-only, so no opening is decided twice (`AdmitWithHostMargin`, MG4) — and the kernel owns the mechanism (check points, resting liquidation, receipt) while the adapter's hooks answer TradingView's scheduling, money and slice (ADR-0001 "Margin"). What stays host-side is named there: the chronology exceptions, the pre-open slice, the 1x-long money call and the broker-open FX rollover, whose kernel `FxRoll` point measures elsewhere (MG9). Corrected by R5 lane F7 | F O S |
| E3 | **Sizing.** TV sizes from the tick-rounded signal close, 10-digit-rounded equity and fee-reserved cash, frozen at signal, with fill-time exceptions in COOF paths (pine_adapter.cpp:1535-1575) | One "generic" percent sizing serving both would break parity or import TV rounding into the kernel; the last ulp differs on lot-stepped instruments | Two paths on purpose: `Sized` (kernel, plain arithmetic, timing as a first-class field) vs `HostSized` + the adapter resolver. Never "simplify" the adapter onto the kernel basis. What the two paths share is the conversion: since R5 lanes N11 and F7 the division by price × point value × FX and the percentage fee reserve exist once, in the kernel (`native_sized_units`), for a default AND a typed (`qty_type` cash / percent) quantity. The adapter keeps what is measured TradingView: the money basis (signal-time equity, TradingView's margin equity at a typed fill, the hypothetical Flatten's balance for a typed reversal), the freeze, the fill-time exceptions and both lot floors (`tests/test_adapter_sizing_relower.cpp` cases 8, 9, 11: kernel 0.0392 against the percent floor's 0.0391 on a 0.0001 lot), and a staged FX series keeps `Sized` wherever the acceptance coordinate converts at the source's rate (case 12) | F O S |
| E4 | **Calc on fill.** Kernel suffix eligibility is continuous along the segment; TV refills only at O/H/L/C waypoints (pine_scheduler_native.cpp:588-640, `coof_next_waypoint` pine_adapter.cpp:4090) | Changing `born_on_remaining_path` or the notification order shifts adapter fills | Freeze native_execution_consumer.cpp:3235-3244 and `drain_applied_notifications` (native_execution_consumer.cpp:5753-5766); new behaviour only behind new opt-ins | F |
| E5 | **Open-bar lookahead.** The adapter reads the full bar in `on_bar_open` (margin path scheduling uses H/L, pine_adapter.cpp:14284-14311) | The complete bar cannot be masked globally | Opt-in `OpenOnly` view (A.4); loud doc warning in L0 | F |
| E6 | **Double equity recording.** The source host already records per bar (pine_strategy_host.cpp:1582-1601) | Kernel recording would duplicate points and shift drawdown | Report policy / ownership (A.1); the adapter declares `KernelRecordedAtHostMarks` (R1): one recorder, at the adapter's own marks | F O |
| E7 | **Price grid.** TV does not merely quantize: it tests triggers against a quantized bar while booking the fill on the *unrounded* level, so the adapter shifts the threshold (pine_adapter.cpp:322-370) and books elsewhere | A kernel grid that quantizes both sides is correct and not TV; moving rounding into the kernel would change both native results and the adapter | `NativePriceGrid::None` for the adapter permanently; never "unify". Before L8: a byte-level experiment that the default price spelling on decimal ticks (one-ULP representations) does not move (S) | O S |
| E8 | **HTF.** The evaluator machinery carries TV publication semantics (lookahead, boundary deferral, early close, `syminfo.type` branching engine_security.cpp:66-72). The feed store is TV-calibrated too: "W" / "M" from native daily bars, native stamps as the period partition (engine_aux_security.cpp:48-87, *fresh*). Two aggregators exist: script buckets use `native_calendar`, `request.security` uses `TimeframeAggregator`, whose completion rules are TV-pinned (timeframe.hpp:288-330) | Reuse risks changing the *order* of the Pine scheduler's pumps; a bare host inherits TV partition rules; native HTF may differ from Pine HTF on early-close sessions | Registration-gated pump; no change to pine_strategy_host.cpp:1316-1400; full corpus diff; supplied-bar provenance preserved, never silently replaced by chart aggregation (S); differences on early-close sessions are recorded, not "fixed" (F). The Pine scheduler keeps one more `TimeframeAggregator` pass, before the run (`PineScheduler::run_begin`, audit M26): the script-bar count it yields (`terminal_source_bar`, the probe-tail suppression) and the per-input completion flags it hands `on_native_input` are needed before the first input arrives, while the kernel seals script intervals only as inputs arrive and offers no pre-run interval query (`NativeExecutionConsumer::script_interval_at` is consumer-private). Retained by R5 lane F7: replacing it needs that kernel capability | F O S |
| E9 | **TA numerics are TV-calibrated** (1e-10 band in DMI / MFI / percentrank; conditional-call extremum ring ta.hpp:60-103) | Non-Pine users may expect IEEE semantics; changing them breaks parity | Rename only (§2.ii b). For an every-bar caller the ring equals a positional window (ta.hpp:79-80). Document. → U6 | F O S |
| E10 | **Dead-but-hashed lot flags** (§2.ii j) | Deleting them changes `broker_state_hash` | Separate, explicitly non-neutral lane (L12) | F O |
| E11 | **OCA and ownership.** Pine's default-sized OCA and `from_entry` reservation rules are source policies | Source IDs, callsite tokens or Pine labels leaking into `WorkingRequestCore` | Opaque keys and receipts only (R5-4) | S |
| E12 | **Legacy input tolerance.** `LegacyTolerant` changes bar-structure and slot-label admission (`NativeSlotLabelPolicy` native_run_spec.hpp:323, `NativeFeedTolerance` native_run_spec.hpp:344; `NativeLegacyTolerance` native_run_spec.hpp:356 is the deprecated spelling) | A rename must preserve the Pine route's hash and failure shape | Values pinned (§2.ii c); strict stays the `NativeRunSpec` default | S |

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
| P8 | **Unverified in the inputs:** whether `execution::LifecycleEffects` (execution.hpp:133-136) is ever non-empty from the Pine path, else `PlacementSnapshot::legs` (pine_adapter.hpp:287) is the only live lifecycle (O); the `strategy.risk.allow_entry_in` setter chain beyond pine_strategy_host.cpp:803-804 → pine_adapter.cpp:15583 (O); what the ABI receipts pin (F); the probe corpus itself lives outside this tree (F) | verify inside the lane that touches each | F O |

### 4.3 Open design questions (supervisor level)

| # | Question | Recommendation in the inputs |
|---|---|---|
| Q1 | Does `engine_script_run_v18` open at L4 and close after L5 ∥ L6, or does L4's sizing hook ship with that batch (§3.4 row 4)? | — (raised by this merge) |
| Q2 | Is a hash-domain bump (needed by §2.ii j) acceptable before the R5 final audit (phase table, native-refactor-progress.md:23, *fresh*), or must L12 wait? | F asks; none |
| Q3 | Does the 4190-probe gate compare broker-state hashes across releases, or only trades? If hashes, L4 and L12 need a hash-version plan | O asks; none |
| Q4 | `SizeTime::AtAcceptance` needs a current execution point (native_host.hpp:1243); between realtime inputs there is none — reject or fall back to `AtMatch`? | F: reject with `InvalidQuantityBasis` |
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
| — | **N6 follow-ups (measured on the adapter; both closed by R5 lane F7).** (1) `apply_fx_open_margin_slice` (pine_adapter.cpp:14373-14389) throws on a rate step over a carried position whose margin is not 100 %: a 2× long carried through a 1.0 → 1.5 step ends the whole run with status 1 and an empty report, where the same book without a series, or with a series that never steps, completes. The kernel's `FxRoll` point is the generic mechanism that case can re-lower onto. (2) `default_sizing_intent` (pine_adapter.cpp:1779) returns nullopt whenever any FX series is staged — even one point that restates the scalar rate — so an FX run silently loses the kernel `Sized` path R2b landed (measured: 1 kernel `Sized` request without a series, 0 with a one-point 1.0 series, same 50-unit book). **Closed by R5 lane F7.** (1) The throw is gone: the carried roll takes TradingView's broker-open checkpoint at any positive margin — its money at the open under the new rate and the side's own maintenance ratio, its lot-floored 4x restore, executed at the open before the step bar's script — and a survivor that is not a 1x long is re-checked over the rest of the bar by the kernel's liquidation. A 2x long or short of 15 at 100 on 1000 across 1 → 1.5 takes 6.666666666666667 at the open and keeps 8.33; at a 25 % side ratio it takes nothing; a constant curve never rolls (`tests/test_affordability_fx.cpp` F7 rows; G6's leveraged pin and its L0 oracle twin moved with an `expectation corrected:` note). The kernel's `FxRoll` point stays refused, measured (MG9). (2) `Sized` is withdrawn only where the kernel would convert at another rate than the source sized at — a curve step between the sub-bar open and the acceptance coordinate — so a one-point series lowers exactly as no series does, and on a stepped curve only the step's own bar keeps its host-resolved intent (`tests/test_adapter_sizing_relower.cpp` case 12). | — |
| **U5** | **Open-bar view default.** Should `OpenOnly` be the default for hosts that are not `LegacyTolerant`? | F: safer for users, but a behaviour change for the ~20 existing native tests; default `Complete` proposed. |
| **U6** | **TA comparison band.** Is TradingView's 1e-10 tie-breaking a feature of the TA library, or a default to make configurable? | F, O: rename only; a switch changes indicator outputs and needs its own lane. S: separate policies, hash the policy identity. |

---

## Appendix A. Native API proposals (signature level)

Common rules: opt-in by spec field or request kind; hash only when present; defaults reproduce today's behaviour (§3.1). Each block merges the inputs' proposals (F P1-P7, O G1-G13, S P1-P10) and names what was not adopted.

### A.1 L2 — report truth (F P6 + O G11 + S P9)

```cpp
// native_run_spec.hpp
enum class NativeReportPolicy : std::uint32_t { HostRecorded = 0, KernelRecorded = 1, KernelRecordedAtHostMarks = 2 };   // O; R1 appended the third
NativeReportPolicy report_policy = NativeReportPolicy::HostRecorded;
bool report_open_position_at_end = false;    // F: mark-to-market rows at the last close; reporting only
// native_host.hpp
virtual void hash_host_extension(BrokerStateHashSink&) const {}   // F (S: hash_native_extension); folded after hash_source_extension; ships with the shared bump
NativeMetricsView native_metrics() const;                          // S: net_profit, equity, closed_trades, open_lots; non-virtual
```

- `KernelRecorded`: the consumer calls the existing `update_equity_extremes()` + `record_equity_point(script_open_ms)` (engine.hpp:1347, engine.hpp:1347) once per script calculation (after `invoke_callback` native_execution_consumer.cpp:6656, native_execution_consumer.cpp:6656, native_execution_consumer.cpp:6656) and after the `AfterCalculation` close; appends the per-bar broker hash when recording is on; synthesizes the open-position row at run end / `stream_end` with the existing `build_close_trade_with_costs` (engine_orders.cpp:70).
- `KernelRecordedAtHostMarks` (R1): the same recording, at the points the host marks (`mark_script_report_point`) instead of once per calculation; the consumer never records on its own initiative under it, so the continuation identity stays where `HostRecorded` leaves it. It is what the Pine adapter declares (`KernelRecordedAtHostMarks` pine_adapter.cpp:1537), with `report_open_position_at_end == false`.
- Publish read-only `closed_trade_*` accessors (RP6).
- Alternative: F's virtual `owns_equity_recording()` (the A48 ownership pattern of `owns_lot_excursions` native_host.hpp:1001). Same semantics, but it costs a host-epoch bump, so the spec form was chosen; the virtual was never added, and `NativeReportPolicy` is what landed (§3.4 row 3).
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

- Resolution: `units = cash / (price * point_value * fx)`, `cash` = the value or `fraction * marked_equity(price)`, `price` = the candidate's `default_resolved_price`. It plugs in where `HostSized` units resolve today (native_execution_consumer.cpp:674; terms resolution native_execution_consumer.cpp:2721-2772). A host override of `resolve_execution_terms` keeps the last word. Non-representable → `MatchRejectReason::TermsUnresolved` (native_order.hpp:811); validation reuses `RequestRejectReason::InvalidQuantityBasis` (native_order.hpp:763).
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

- Mechanism (F): a **kernel-originated request**. At each script-bar open and after each applied fill the kernel computes the liquidation level and rests a `Reduce` / `Flatten` with `Stop{level}` bound to the live openings — the shape the adapter builds host-side today (pine_adapter.cpp:11441-11462). New hashed `RequestDefinition::origin { Host, KernelLiquidation, KernelRisk }`, new `CancelReason::Superseded` on re-price, `MarginCallEvent` joins `CommandEvent` (17 → 18, native_order.hpp:1194). O's alternative, a checkpoint on the excursion walk (native_execution_consumer.cpp:1829-1842), is not chosen: a reduction mid-path is the entanglement O itself flags (E2).
- FX revaluation (MG9) falls out once `native_fx_curve` drives the mark (`native_fx_curve.cpp`).
- TUs: `native_run_spec.{hpp,cpp}`, `native_execution_consumer.cpp` (`admit_opening_inspect` native_execution_consumer.cpp:4532-4566 per side; scheduling next to `invoke_bar_open_callback` / `drain_after_applied`), `native_order.*`, `engine_state_hash.cpp` + the coverage checker, `c_abi.cpp` + `pineforge.h` (`pf_native_run_spec_v2` or an extension struct; `CLAUDE.md` SOP).

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

1. At one point: match and settle → `on_native_applied` per applied event, FIFO (rule already at `on_native_applied` native-engine.md:1551-1552) → with `BarCloseAndFills`, one recalculation at the fill cursor (reason `OrderFill`), driven from the existing notification drain (native_execution_consumer.cpp:6456-6468) under its re-entrancy guard, bounded by `max_recalculations_per_point`. S would run it *before* the drain; not chosen, the drain order is frozen (E4).
2. Requests born in any of these callbacks follow the existing birth rule: eligible on the unconsumed rest of the bar (native_execution_consumer.cpp:3248-3257). `born_on_remaining_path` (native_execution_consumer.cpp:5272-5276) is not touched — the adapter's COOF deferral (`pending_coof_requests_`, element type `PendingCoofRequest` pine_adapter.hpp:1100-1107) is calibrated against it.
3. `EveryModeledPoint`: a calculation at each magnifier sample / observed print, in batch as well as stream (delivery loops native_execution_consumer.cpp:6132-6288, native_execution_consumer.cpp:6189-6245). `on_native_tick` stays the observation hook (S).
4. `current_partial_bar()` derives from the driver points already recorded (`record_driver` native_execution_consumer.cpp:2653); valid in bar-open, applied and tick callbacks.
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
// Trail::offset == 0.0 is accepted: "exit on the first adverse move past the best" (today the run fails, native_execution_consumer.cpp:3484-3489)

// include/pineforge/native_toolkit.hpp — header-only, Pine-free, covered by the include-independence checker (F; shapes from S P1)
struct BracketSpec    { native_order::RequestHandle parent; std::optional<native_order::Request> take_profit, stop_loss, trail;
                        native_order::GroupEffect sibling_effect = native_order::GroupEffect::Cancel; };
struct BracketLegOutcome { BracketLegState state; std::optional<native_order::SubmitResult> result; };  // gap lane E8
struct BracketReceipt { native_order::RequestHandle parent; std::optional<native_order::RequestHandle> take_profit, stop_loss, trail;
                        BracketLegOutcome take_profit_outcome, stop_loss_outcome, trail_outcome; };
BracketReceipt submit_bracket(NativeStrategyHost&, const BracketSpec&);   // emits exactly the owner / group shapes of OL10
template <class Key> class OrderBook;                                     // id -> handle: submit-or-replace, cancel-by-id
```

- A leg with owner `WaitForApplied` / `BindOpening` and a non-absolute anchor gets its level when the owner's fill arms it (`ArmedEvent` native_order.hpp:1132-1139 already exists) (O). Fallback with no kernel change: the toolkit materializes relative levels from the host's `on_native_applied` (F).
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
- TUs: `native_run_spec.cpp` (validation), `native_execution_consumer.cpp` (resolve path native_execution_consumer.cpp:3658-3684, `execute_current` path native_execution_consumer.cpp:3751-3761), `native_matching.hpp` (threshold form). The dead helpers engine.hpp:1221, engine.hpp:1231, engine.hpp:1271 are the reference arithmetic (§2.ii e).

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

- Breach: `MatchRejectReason::RiskLimit` for openings (O; F: `RiskHalted`), an optional kernel-issued flatten through A.3's kernel-originated request (`origin = KernelRisk`, F), and a `NativeRiskEvent` in `native_events` (native_host.hpp:1262) (O) or a callback `on_native_risk_event` (S — a virtual, so only inside an epoch batch). The day key uses the existing calendar (native_calendar.hpp:363-366), not a chart clock.
- S's `directions` / `max_units` / `max_position_units` are existing spec fields (MG1-MG3), not duplicated.
- Interim (F): a C++ host can build all four today from `native_marked_equity` + `on_native_applied`, which is what the adapter does (pine_adapter.cpp:11994-12037); a toolkit helper is an acceptable stand-in until L9.
- TUs: `native_run_spec.{hpp,cpp}`, `native_execution_consumer.cpp` (`admit_opening_inspect` native_execution_consumer.cpp:4532-4566 and the per-point walk), `native_order.hpp`, the state hash.

### A.9 Single-source proposals not adopted

| Proposal | Src | Why not now |
|---|---|---|
| `NativeExcursionMode { PathExtremes, FillOnly, HostOwned }` + per-lot sample events | S P8 | The kernel sampler + A48 ownership already cover RP1 (F, O); no input found a gap |
| `NativeFxEpoch` + `on_native_fx_epoch` (stream-safe FX) | S P10 | No R5 lane; F rules it out of scope → U4 |
| Generic `FillModel` interface | S §2.2, lane G | R5-3 + R5-1 (§2.ii n) |
| TA `ComparePolicy` split | S §2.2 | not neutral (§2.ii b) → U6 |
| Kernel bracket object + close-reservation modes `ReplaceByKey` / `Fifo` | S P1, P2 | R5-2, R5-4 |
