/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_c_api.h — versioned C-level native host API (R5 lane L13).
 *
 * A host that is not written in C++ drives the kernel through this header:
 * it hands the runtime a callback table, runs a batch of bars, and submits,
 * replaces, cancels or executes order requests from inside those callbacks.
 * It is the C spelling of <pineforge/native_host.hpp>'s NativeStrategyHost —
 * the same kernel, the same events, no PineScript and no codegen.
 *
 * SCOPE
 * ─────
 * ✓ Create / free a native host backed by a C callback table
 * ✓ Run a batch of OHLCV bars and fill a pf_report_t
 * ✓ submit / replace / cancel / cancel_all / cancel_where / execute_current
 * ✓ Read the physical position, the live working book, the open lots (the
 *   book lot by lot, marked at a price — strategy.opentrades.* for a C host)
 *   and the event history
 * ✓ Read the run's lifecycle state and its typed failure
 * ✓ Read a closed row's own identifiers — its entry and exit ticket, its exit
 *   comment and its close cause — with the pineforge.h accessors, which take
 *   any handle this header produces
 * ✓ Extend the run specification with the fields pf_native_run_spec_v1 predates
 * ✓ Declare an auxiliary finer feed, build a series from it, and append its
 *   later bars to a realtime stream
 * ✓ Declare, from `on_applied`, where a lot's opening fill sat on its entry
 *   bar, for a host that owns its lots' excursions
 *
 * ✗ Streaming has no new symbols: strategy_stream_begin / _push_bar /
 *   _push_tick / _advance_time / _end / _fill_report take any handle this
 *   header produces, unchanged.
 * ✗ resolve_execution_terms is exposed only as its UNITS half, for a
 *   host-sized close (pf_native_callbacks_v1::on_close_units). Its price and
 *   opening-shape halves, validate_execution_precommit and
 *   resolve_anchored_level stay C++-only; see the exclusion list below.
 * ✗ hash_host_extension is not exposed: the callback table carries no hash
 *   hook, so a C host's broker-state hash is the kernel's own fold. The
 *   per-bar rows need no new symbol: `report_policy` = KernelRecorded with
 *   strategy_set_broker_state_hash_recording on fills
 *   pf_report_t::broker_state_hash, one row per script bar.
 * ✗ WaitForApplied::first_match and WaitForApplied::scope are not exposed:
 *   a WAIT_FOR_APPLIED child submitted here keeps the defaults (AtArmPrint,
 *   OwnerLot). NativeArmScope::Book exists for a HostSized close, whose
 *   units the unexposed terms hook answers.
 *
 * COVERAGE
 * ────────
 * Every public member of NativeStrategyHost, and either its C spelling or the
 * reason it has none. scripts/check_native_c_api_surface.py proves this list is
 * exactly that class's public surface: a member added there without a row here,
 * or a row here naming a member that no longer exists, fails CI.
 *
 *   [C]  on_bar                            strategy_native_run_v1 / the strategy_stream_* ingress
 *   [--] prepare_native_begin              borrows the codegen ingress (InputsMap, SymInfo, the opaque
 *                                          overrides) that no C host supplies; a C run is declared up front
 *                                          with strategy_configure_native_ext_v1
 *   [C]  on_native_run_begin               pf_native_callbacks_v1::on_run_begin
 *   [C]  on_native_input                   pf_native_callbacks_v1::on_input
 *   [C]  on_native_tick                    pf_native_callbacks_v1::on_tick
 *   [C]  on_native_timeframe_bar           pf_native_callbacks_v1::on_timeframe_bar
 *   [C]  on_native_bar_open                pf_native_callbacks_v1::on_bar_open
 *   [C]  on_native_bar                     pf_native_callbacks_v1::on_bar
 *   [C]  on_native_recalculate             pf_native_callbacks_v1::on_recalculate
 *   [C]  on_native_sub_bar                 pf_native_callbacks_v1::on_sub_bar
 *   [C]  on_native_applied                 pf_native_callbacks_v1::on_applied
 *   [C]  on_native_margin_call             pf_native_callbacks_v1::on_margin_call
 *   [C]  resolve_execution_terms           pf_native_callbacks_v1::on_close_units -- the UNITS half only; the
 *                                          price and the opening shape stay the kernel's
 *   [--] validate_execution_precommit      its view is an ExecutionPlan, an AccountEffectProjection and a
 *                                          variable-length closed-row P&L vector, none of which has a size-
 *                                          prefixed POD; a C host gates an opening with
 *                                          PF_NATIVE_INTENT_SIZED's placement-time admission or with
 *                                          on_margin_requirement
 *   [--] resolve_anchored_level            the generic knob for where an anchored level sits is
 *                                          pf_native_request_v1::anchor_rounding; the hook exists for a source
 *                                          language's own level arithmetic, and a C host that wants another
 *                                          level replaces the leg on its PF_NATIVE_EVENT_ARMED
 *   [C]  resolve_margin_requirement        pf_native_callbacks_v1::on_margin_requirement
 *   [C]  margin_check_allowed              pf_native_callbacks_v1::on_margin_check
 *   [C]  resolve_margin_call_units         pf_native_callbacks_v1::on_margin_call_units
 *   [C]  owns_lot_excursions               pf_native_callbacks_v1::on_lot_excursion -- installing it IS
 *                                          declaring ownership
 *   [C]  closed_lot_excursion              pf_native_callbacks_v1::on_lot_excursion
 *   [C]  current_partial_bar               strategy_native_partial_bar_v1
 *   [C]  native_recalculation_count        strategy_native_recalculations_v1
 *   [C]  native_recalculations_skipped     strategy_native_recalculations_v1
 *   [C]  current_execution_point           pf_native_decision_v1::price / ::quote_kind, on every callback
 *   [C]  trail_state                       strategy_native_trail_state_v1
 *   [--] inspect_current_execution         its preview carries the account-effect projection and a variable-
 *                                          length closed-row P&L vector with no size-prefixed POD;
 *                                          strategy_native_execute_current_v1 answers the same verdicts as
 *                                          pf_native_execute_outcome_e and pf_native_refusal_e
 *   [C]  execute_current                   strategy_native_execute_current_v1
 *   [C]  native_series_bar                 strategy_native_series_bar_v1
 *   [C]  declare_timeframe_subscriptions   strategy_native_declare_subscriptions_v1 -- a row is
 *                                          pf_native_subscription_v1, whose `lookahead` / `gaps` bools are
 *                                          pf_native_lookahead_e / pf_native_gaps_e words
 *   [--] declare_auxiliary_feed            a C host declares the run's auxiliary finer feed up front, in
 *                                          pf_native_run_spec_ext_v1's auxiliary tail under
 *                                          PF_NATIVE_SPEC_EXT_AUXILIARY_FEED; the begin-time REPLACEMENT
 *                                          takes a std::optional<NativeAuxiliaryFeed> with no size-
 *                                          prefixed POD, and withdrawing a feed the spec declared has no
 *                                          C caller
 *   [C]  append_auxiliary_bars             strategy_native_append_auxiliary_bars_v1
 *   [C]  configure_native                  strategy_configure_native_v1 / strategy_configure_native_ext_v1
 *   [C]  configure_native_fx_curve         strategy_configure_native_fx_curve_v1 (pineforge.h)
 *   [C]  native_state                      strategy_native_state_v1
 *   [C]  submit                            strategy_native_submit_v1
 *   [C]  replace                           strategy_native_replace_v1
 *   [--] submit_market                     a C++ convenience that REFUSES non-market extras instead of dropping
 *                                          them; the same request is strategy_native_submit_v1 with
 *                                          PF_NATIVE_TRIGGER_MARKET and a zero-filled struct
 *   [--] replace_market                    the same convenience for a replace; see submit_market
 *   [C]  cancel                            strategy_native_cancel_v1
 *   [C]  native_working_requests           strategy_native_working_len_v1 / strategy_native_working_get_v1;
 *                                          a Trail's arm_price presence is
 *                                          pf_native_working_v1::trail_has_arm_price
 *   [C]  native_open_lots                  strategy_native_open_lot_count_v1 / strategy_native_open_lot_get_v1
 *   [C]  cancel_all                        strategy_native_cancel_all_v1
 *   [C]  cancel_where                      strategy_native_cancel_where_v1
 *   [C]  cohort_open                       strategy_native_cohort_open_v1
 *   [C]  cohort_add                        strategy_native_cohort_add_v1
 *   [C]  cohort_remove                     strategy_native_cohort_remove_v1
 *   [C]  physical_position                 strategy_native_position_v1
 *   [C]  native_marked_equity              strategy_native_marked_equity_v1
 *   [C]  native_liquidation_price          strategy_native_liquidation_price_v1
 *   [C]  native_risk_state                 strategy_native_risk_state_v1
 *   [C]  native_events                     strategy_native_events_v1
 *   [C]  native_decision_floor             pf_native_state_v1::decision_floor_ms
 *   [C]  native_consumed_high_water        pf_native_state_v1::consumed_high_water
 *   [C]  native_continuation_hash          strategy_native_continuation_hash_v1
 *   [--] native_sized_units                the basis it converts is the C++ native_order::Sized variant,
 *                                          which has no size-prefixed POD of its own (pf_native_request_v1
 *                                          carries a sizing BLOCK, not the variant); a C host submits
 *                                          PF_NATIVE_INTENT_SIZED and reads the units the kernel resolved
 *                                          from the applied execution
 *
 * One asymmetry this list does not reach, recorded here because a C host will
 * look for it: pf_trade_t carries no exit ticket. That POD is the codegen
 * ABI's, runtime-allocated and iterated with the caller's own sizeof, so a
 * tail costs a PF_ABI_VERSION bump for every existing consumer — and it needs
 * none. strategy_closed_trade_entry_id / _exit_id / _exit_comment /
 * _close_cause (pineforge.h) index exactly the rows of pf_report_t::trades,
 * are implemented in the kernel archive, and take any handle, so a C host
 * reads back the ticket its own margin model declared
 * (pf_native_run_spec_ext_v1::margin_liquidation_label) without a new symbol.
 * Executed by the fx-roll scenario of tests/test_native_c_api.c.
 *
 * And one member this list cannot reach, because it is not a member of
 * NativeStrategyHost: the entry-side half of on_lot_excursion's capability
 * (RULING A48) is a protected member of the base,
 * BacktestEngine::declare_opened_lot_entry_bar_mask, through which the owner
 * of a lot's excursion says where the lot's opening fill sat on its entry bar
 * and the kernel derives the lot's entry-bar mask. Its C spelling is
 * strategy_native_declare_opened_lot_entry_bar_mask_v1. The census above is
 * the public surface of NativeStrategyHost, so the check neither requires
 * nor admits a row for a member of its base. Executed by the entry-bar mask
 * scenario of tests/test_native_c_api.c.
 *
 * HARDENING RULES
 * ───────────────
 *  - Every struct is tagged and size-prefixed: `struct_size` is the exact
 *    sizeof of the version the caller compiled against, `version` is that
 *    layout's version constant. A mismatch is refused with PF_NATIVE_E_STRUCT
 *    and mutates nothing. The exception is a deliberately additive tail: a
 *    struct that grew one publishes every earlier layout's length as a
 *    `*_SIZE` constant and the runtime accepts each — pf_native_request_v1,
 *    pf_native_run_spec_ext_v1 and pf_native_callbacks_v1 as inputs,
 *    pf_native_working_v1 as a readout. A readout is written only as far as
 *    the length its caller sent.
 *  - Every enum-valued field is translated by an exhaustive switch. A value
 *    outside its enumeration is refused with PF_NATIVE_E_TAG; a value this
 *    version deliberately cannot represent is refused with
 *    PF_NATIVE_E_UNSUPPORTED. No C value is ever cast onto a C++ variant.
 *  - C callbacks must not unwind. A callback that returns non-zero latches
 *    NativeFailureCode::CallbackException (PF_NATIVE_FAILURE_CALLBACK) and the
 *    run ends Failed; the code is readable with strategy_native_state_v1.
 *  - Commands follow the kernel's existing legality rule: inside a callback,
 *    or between realtime inputs. A command issued anywhere else returns
 *    PF_NATIVE_E_STATE and changes nothing.
 *
 * STABILITY
 * ─────────
 * Same guarantee as <pineforge/pineforge.h>: within a major version the
 * layouts below are append-only and the signatures never change. A later
 * revision appends fields and raises the version constant; the size prefix
 * keeps an old caller refused rather than silently misread.
 */

#ifndef PINEFORGE_NATIVE_C_API_H
#define PINEFORGE_NATIVE_C_API_H

#include <stdint.h>
#include <stddef.h>

/* pf_strategy_t, pf_bar_t, pf_report_t, PF_API. pineforge.h includes this
 * header back at its end; both guards make either include order work. */
#include <pineforge/pineforge.h>

/** Feature probe for the C-level native host API.
 *  When defined, #strategy_native_host_create_v1 is available. */
#define PINEFORGE_HAS_NATIVE_C_API_V1 1

/** Monotonic version of this header's native-C layouts. Every versioned
 *  struct below carries it in its `version` field. */
#define PF_NATIVE_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup pf_native_c_status Status codes
 *  @brief Every `int`-returning symbol here answers 0 or one of these.
 *
 *  Negative values are errors and never mutate run state.  Non-negative
 *  values are outcomes: 0 is success everywhere, and
 *  #strategy_native_execute_current_v1 additionally answers the positive
 *  #pf_native_execute_outcome_t codes.
 *  @{ */
#define PF_NATIVE_OK                 0   /**< Success. */
#define PF_NATIVE_E_HANDLE          -1   /**< NULL handle, or not a C-callback native host. */
#define PF_NATIVE_E_STRUCT          -2   /**< `struct_size` / `version` mismatch. */
#define PF_NATIVE_E_TAG             -3   /**< An enum field outside its enumeration. */
#define PF_NATIVE_E_ARGUMENT        -4   /**< NULL output, negative count, out-of-range index. */
#define PF_NATIVE_E_STATE           -5   /**< Illegal here: the command legality rule, or the
                                       *   kernel's own lifecycle, refused it. */
#define PF_NATIVE_E_REJECTED        -6   /**< The kernel rejected the request (reason written out). */
#define PF_NATIVE_E_UNSUPPORTED     -7   /**< A tag this API version cannot represent. */
#define PF_NATIVE_E_EXCEPTION       -8   /**< A C++ exception was contained at the boundary. */
#define PF_NATIVE_E_NOT_WORKING     -9   /**< The target handle is no longer a live request. */
#define PF_NATIVE_E_INVALID_TARGET -10   /**< The target handle was never issued by this run. */
#define PF_NATIVE_E_RUN_FAILED     -11   /**< The run did not reach Completed; read the state. */
#define PF_NATIVE_E_REFUSED        -12   /**< execute_current refused; see pf_native_refusal_e. */
/** Non-negative outcome: the kernel HAS no answer here and the output was
 *  left at its documented empty. It is not an error — the C++ spelling of
 *  each accessor that returns it is a `std::optional`, whose empty is a
 *  legitimate answer (no path walk, an unarmed trail, a series that has not
 *  delivered, a run with no margin model). Only the accessors whose own
 *  documentation names it can return it; #strategy_native_execute_current_v1
 *  never does, its positive codes being #pf_native_execute_outcome_t. */
#define PF_NATIVE_ABSENT             1
/** @} */

/** `NativeFailureCode::CallbackException` — the code latched when a C callback
 *  returns non-zero. Mirrors the C++ enumerator; pinned by a static_assert. */
#define PF_NATIVE_FAILURE_CALLBACK 5

/** @defgroup pf_native_c_enums Translated enumerations
 *  @brief Every value below is the exact integer of the C++ alternative it
 *  names, pinned by static_asserts in src/native_c_host.cpp.
 *  @{ */

/** Order intent — the alternative index of `native_order::OrderIntent`. */
typedef enum pf_native_intent_e {
    PF_NATIVE_INTENT_FLATTEN    = 0, /**< Close the whole book. */
    PF_NATIVE_INTENT_REDUCE     = 1, /**< Reduce; see #pf_native_reduction_t. */
    PF_NATIVE_INTENT_TRANSACT   = 2, /**< `intent_value` signed units. */
    PF_NATIVE_INTENT_REVERSE_TO = 3, /**< `intent_value` target signed exposure. */
    PF_NATIVE_INTENT_HOST_SIZED = 4, /**< The cohort close, and nothing else;
                                      *   see #PF_NATIVE_OWNER_BIND_COHORT.
                                      *   Refused PF_NATIVE_E_UNSUPPORTED
                                      *   under any other owner. */
    PF_NATIVE_INTENT_SIZED      = 5  /**< Kernel-sized opening (L3). */
} pf_native_intent_t;

/** Reduction size — the alternative index of `native_order::ReductionSize`. */
typedef enum pf_native_reduction_e {
    PF_NATIVE_REDUCE_EXPLICIT_UNITS = 0, /**< `intent_value` units. */
    PF_NATIVE_REDUCE_OWNER_OPENED   = 1, /**< Exactly what the owner opened. */
    PF_NATIVE_REDUCE_SCOPE_FRACTION = 2  /**< `intent_value` fraction in (0, 1]. */
} pf_native_reduction_t;

/** Scope claim of a fractional reduce. */
typedef enum pf_native_scope_claim_e {
    PF_NATIVE_SCOPE_GROSS            = 0,
    PF_NATIVE_SCOPE_NET_OF_SIBLINGS  = 1
} pf_native_scope_claim_t;

/** Side of a kernel-sized opening. */
typedef enum pf_native_side_e {
    PF_NATIVE_SIDE_LONG  = 0,
    PF_NATIVE_SIDE_SHORT = 1
} pf_native_side_t;

/** Sizing basis — the alternative index of `native_order::SizeBasis`. */
typedef enum pf_native_size_basis_e {
    PF_NATIVE_SIZE_BASIS_CASH            = 0, /**< `intent_value` account-currency cash. */
    PF_NATIVE_SIZE_BASIS_EQUITY_FRACTION = 1  /**< `intent_value` fraction of marked equity. */
} pf_native_size_basis_t;

/** When a sizing basis resolves. */
typedef enum pf_native_size_time_e {
    PF_NATIVE_SIZE_AT_MATCH      = 0,
    PF_NATIVE_SIZE_AT_ACCEPTANCE = 1
} pf_native_size_time_t;

/** Whether resolved sizing snaps onto the run's quantity grid. */
typedef enum pf_native_grid_policy_e {
    PF_NATIVE_GRID_SNAP           = 0,
    PF_NATIVE_GRID_EXPLICIT_UNITS = 1
} pf_native_grid_policy_t;

/** Trigger — the alternative index of `native_order::Trigger`. */
typedef enum pf_native_trigger_e {
    PF_NATIVE_TRIGGER_MARKET     = 0, /**< p1, p2 ignored. */
    PF_NATIVE_TRIGGER_LIMIT      = 1, /**< p1 = price; `fill_through` makes it market-if-touched. */
    PF_NATIVE_TRIGGER_STOP       = 2, /**< p1 = price. */
    PF_NATIVE_TRIGGER_STOP_LIMIT = 3, /**< p1 = stop, p2 = limit. */
    PF_NATIVE_TRIGGER_TRAIL      = 4  /**< p1 = offset, p2 = arm price when `trail_has_arm_price`. */
} pf_native_trigger_t;

/** Where a trigger level comes from — `native_order::TriggerAnchor` (L7). */
typedef enum pf_native_anchor_e {
    PF_NATIVE_ANCHOR_ABSOLUTE        = 0, /**< The level written in the trigger. */
    PF_NATIVE_ANCHOR_FROM_OWNER_FILL = 1  /**< owner fill + `anchor_offset` (signed). */
} pf_native_anchor_t;

/** How a materialized anchored level snaps onto the run's price tick ladder —
 *  `native_order::NativeAnchorRounding` (L7b). RAW is the established
 *  behaviour; the other two need `price_tick > 0` at acceptance. */
typedef enum pf_native_anchor_rounding_e {
    PF_NATIVE_ANCHOR_ROUNDING_RAW         = 0, /**< fill + offset exactly. */
    PF_NATIVE_ANCHOR_ROUNDING_HALF_UP     = 1, /**< Nearest tick, ties away from zero. */
    PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL = 2  /**< Toward the region the leg needs. */
} pf_native_anchor_rounding_t;

/** Whether a WAIT_FOR_APPLIED child is a working order before its arm —
 *  `native_order::NativeArmVisibility` (L7b). PENDING_UNTIL_ARMED keeps it
 *  out of #strategy_native_working_len_v1 / #strategy_native_working_get_v1
 *  until its ArmedEvent; it stays a live request the whole time (replace,
 *  cancel, cancel_all still address it, and it never matches before the arm
 *  under either value). */
typedef enum pf_native_arm_visibility_e {
    PF_NATIVE_ARM_VISIBILITY_WORKING             = 0,
    PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED = 1
} pf_native_arm_visibility_t;

/** Capacity — the alternative index of `native_order::Capacity`. */
typedef enum pf_native_capacity_e {
    PF_NATIVE_CAPACITY_IMMEDIATE    = 0, /**< Whole remaining at one point. */
    PF_NATIVE_CAPACITY_POINT_BUDGET = 1  /**< `capacity_units` per matching point. */
} pf_native_capacity_t;

/** Owner — the alternative index of `native_order::Owner`. */
typedef enum pf_native_owner_e {
    PF_NATIVE_OWNER_INDEPENDENT      = 0, /**< No owner. */
    PF_NATIVE_OWNER_WAIT_FOR_APPLIED = 1, /**< One parent in `owner_incarnations`. */
    PF_NATIVE_OWNER_BIND_OPENING     = 2, /**< One opening + `owner_cycle`. */
    PF_NATIVE_OWNER_BIND_OPENINGS    = 3, /**< `owner_n` openings + `owner_cycle`. */
    /** `cohort` from #strategy_native_cohort_open_v1. The kernel pairs this
     *  owner with exactly one intent — a host-sized CLOSE — so a cohort close
     *  is spelled #PF_NATIVE_INTENT_HOST_SIZED with this owner and no
     *  quantity of its own: the roster's live openings are the target and the
     *  cohort authority decides the units. That pairing is the only shape in
     *  which HOST_SIZED is accepted here. */
    PF_NATIVE_OWNER_BIND_COHORT      = 4
} pf_native_owner_t;

/** Group — the alternative index of `native_order::Group`. */
typedef enum pf_native_group_e {
    PF_NATIVE_GROUP_NONE   = 0,
    PF_NATIVE_GROUP_MEMBER = 1 /**< `group_id`, `group_cohort`, `group_effect`. */
} pf_native_group_t;

/** What a filled group member does to its siblings. */
typedef enum pf_native_group_effect_e {
    PF_NATIVE_GROUP_CANCEL = 0,
    PF_NATIVE_GROUP_REDUCE = 1
} pf_native_group_effect_t;

/** Which identity text #strategy_native_cancel_where_v1 compares.
 *
 *  Both are the free text the request carried: `comment` is the established
 *  selector, `label` is where a host that names its orders puts its own id,
 *  so LABEL is the one call that withdraws every live request issued under
 *  one such id. Neither is indexed; both walk the live book once. */
typedef enum pf_native_request_field_e {
    PF_NATIVE_FIELD_COMMENT = 0,
    PF_NATIVE_FIELD_LABEL   = 1
} pf_native_request_field_t;

/** Price rule of an immediate execution. */
typedef enum pf_native_price_rule_e {
    PF_NATIVE_PRICE_AS_PRESENTED = 0,
    PF_NATIVE_PRICE_NEAREST_TICK = 1
} pf_native_price_rule_t;

/** Outcome of #strategy_native_execute_current_v1 (non-negative returns). */
typedef enum pf_native_execute_outcome_e {
    PF_NATIVE_EXECUTED_APPLIED        = 0, /**< An execution was applied. */
    PF_NATIVE_EXECUTED_NO_EFFECT      = 1, /**< Legal, nothing to execute. */
    PF_NATIVE_EXECUTED_MATCH_REJECTED = 2, /**< Terms/admission rejected it. */
    PF_NATIVE_EXECUTED_CANCELLED      = 3  /**< The request was cancelled instead. */
} pf_native_execute_outcome_t;

/** Why execute_current refused. Written to `*refusal`, if supplied, when
 *  #strategy_native_execute_current_v1 answers PF_NATIVE_E_REFUSED. Mirrors
 *  `NativeCurrentRefusal`. */
typedef enum pf_native_refusal_e {
    PF_NATIVE_REFUSAL_NO_EXECUTION_CONTEXT   = 0,
    PF_NATIVE_REFUSAL_REENTRANT              = 1,
    PF_NATIVE_REFUSAL_INVALID_HANDLE         = 2,
    PF_NATIVE_REFUSAL_NOT_WORKING            = 3,
    PF_NATIVE_REFUSAL_NOT_ACCEPTED_IN_CALLBACK = 4,
    PF_NATIVE_REFUSAL_UNSUPPORTED_REQUEST    = 5,
    PF_NATIVE_REFUSAL_UNREADY_OWNER          = 6,
    PF_NATIVE_REFUSAL_INVALID_SELECTION      = 7,
    PF_NATIVE_REFUSAL_CONFIGURATION_MISMATCH = 8
} pf_native_refusal_t;

/** Lifecycle of the native run — mirrors `NativeLifecycleKind`. */
typedef enum pf_native_lifecycle_e {
    PF_NATIVE_LIFECYCLE_UNCONFIGURED = 0,
    PF_NATIVE_LIFECYCLE_READY        = 1,
    PF_NATIVE_LIFECYCLE_RUNNING      = 2,
    PF_NATIVE_LIFECYCLE_COMPLETED    = 3,
    PF_NATIVE_LIFECYCLE_FAILED       = 4
} pf_native_lifecycle_t;

/** Event tag of #pf_native_event_v1.
 *
 *  1..18 are the first eighteen alternatives of `native_order::CommandEvent`,
 *  in variant order plus one; 19 and 20 are the driver-point and account
 *  observations `native_events()` also carries. 21 is L9's `NativeRiskEvent`,
 *  the nineteenth CommandEvent alternative: it was added after this header
 *  froze, so it keeps a tag of its own past the two observations rather than
 *  taking 19 and renumbering them. A reader compiled before it skips it by
 *  tag, exactly as it must skip any tag it does not know.
 *
 *  One kind named in the design is NOT represented and never appears here: a
 *  completed higher-timeframe bucket, delivered only through
 *  `on_timeframe_bar` and never recorded in the event history. */
typedef enum pf_native_event_kind_e {
    PF_NATIVE_EVENT_ACCEPTED            = 1,
    PF_NATIVE_EVENT_REJECTED            = 2,
    PF_NATIVE_EVENT_REPLACED            = 3,
    PF_NATIVE_EVENT_REPLACE_REJECTED    = 4,
    PF_NATIVE_EVENT_CANCELLED           = 5,
    PF_NATIVE_EVENT_NOT_WORKING         = 6,
    PF_NATIVE_EVENT_INVALID_HANDLE      = 7,
    PF_NATIVE_EVENT_NO_EFFECT           = 8,
    PF_NATIVE_EVENT_MATCH_REJECTED      = 9,
    PF_NATIVE_EVENT_APPLIED             = 10,
    PF_NATIVE_EVENT_CLOSE_BOUND         = 11,
    PF_NATIVE_EVENT_ACTIVATED           = 12,
    PF_NATIVE_EVENT_RESERVATION_REDUCED = 13,
    PF_NATIVE_EVENT_DEFERRED_GROUP      = 14,
    PF_NATIVE_EVENT_QUANTITY_BOUND      = 15,
    PF_NATIVE_EVENT_ARMED               = 16,
    PF_NATIVE_EVENT_TERMS_RESOLVED      = 17,
    PF_NATIVE_EVENT_MARGIN_CALL         = 18,
    PF_NATIVE_EVENT_DRIVER_POINT        = 19,
    PF_NATIVE_EVENT_ACCOUNT             = 20,
    PF_NATIVE_EVENT_RISK                = 21
} pf_native_event_kind_t;

/** Which generic risk limit a #PF_NATIVE_EVENT_RISK event reports — the
 *  `reason` field of #pf_native_event_v1, mirroring
 *  `native_order::RiskLimitKind` (L9). */
typedef enum pf_native_risk_limit_e {
    PF_NATIVE_RISK_MAX_DRAWDOWN              = 0,
    PF_NATIVE_RISK_MAX_INTRADAY_LOSS         = 1,
    PF_NATIVE_RISK_MAX_CONSECUTIVE_LOSS_DAYS = 2,
    PF_NATIVE_RISK_MAX_FILLS_PER_DAY         = 3
} pf_native_risk_limit_t;

/** Which day a risk limit's "day" is — `NativeRiskDay`. SESSION is the run's
 *  own session calendar (an overnight session is one day); CALENDAR_TIMEZONE
 *  is the civil date in the spec's scheduling timezone. */
typedef enum pf_native_risk_day_e {
    PF_NATIVE_RISK_DAY_SESSION           = 0,
    PF_NATIVE_RISK_DAY_CALENDAR_TIMEZONE = 1
} pf_native_risk_day_t;

/** What a breach does — `NativeRiskAction`. BLOCK_OPENINGS refuses every
 *  opening while the block lasts and leaves the live book alone;
 *  FLATTEN_AND_BLOCK first closes the book with one kernel-originated
 *  Flatten. */
typedef enum pf_native_risk_action_e {
    PF_NATIVE_RISK_BLOCK_OPENINGS    = 0,
    PF_NATIVE_RISK_FLATTEN_AND_BLOCK = 1
} pf_native_risk_action_t;

/** Remaining projection of a live request — `native_order::RemainingProjection`. */
typedef enum pf_native_remaining_e {
    PF_NATIVE_REMAINING_UNBOUND     = 0,
    PF_NATIVE_REMAINING_FLATTEN_ALL = 1,
    PF_NATIVE_REMAINING_UNITS       = 2, /**< `remaining_units` is meaningful. */
    PF_NATIVE_REMAINING_DEFERRED    = 3,
    PF_NATIVE_REMAINING_NO_TARGET   = 4
} pf_native_remaining_t;

/** Trigger state of a live request — `native_order::TriggerState`. */
typedef enum pf_native_trigger_state_e {
    PF_NATIVE_TRIGGER_STATE_MARKET_READY      = 0,
    PF_NATIVE_TRIGGER_STATE_LIMIT_READY       = 1,
    PF_NATIVE_TRIGGER_STATE_STOP_IDLE         = 2,
    PF_NATIVE_TRIGGER_STATE_STOP_ACTIVE       = 3,
    PF_NATIVE_TRIGGER_STATE_STOP_LIMIT_PENDING = 4,
    PF_NATIVE_TRIGGER_STATE_STOP_LIMIT_LIVE   = 5,
    PF_NATIVE_TRIGGER_STATE_TRAIL_WAIT_ARM    = 6,
    PF_NATIVE_TRIGGER_STATE_TRAIL_TRACK       = 7,
    PF_NATIVE_TRIGGER_STATE_TRAIL_ACTIVE      = 8
} pf_native_trigger_state_t;

/** Which extension blocks of #pf_native_run_spec_ext_v1 are meaningful. */
typedef enum pf_native_spec_ext_mask_e {
    PF_NATIVE_SPEC_EXT_REPORT        = 1u << 0,
    PF_NATIVE_SPEC_EXT_PRICE_GRID    = 1u << 1,
    PF_NATIVE_SPEC_EXT_CALCULATION   = 1u << 2,
    PF_NATIVE_SPEC_EXT_OPEN_BAR_VIEW = 1u << 3,
    PF_NATIVE_SPEC_EXT_MARGIN        = 1u << 4,
    PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS = 1u << 5,
    /** L9's generic risk limits. Only a caller whose
     *  pf_native_run_spec_ext_v1 carries the risk tail may set this bit; a
     *  caller sending the base layout is refused with PF_NATIVE_E_STRUCT. */
    PF_NATIVE_SPEC_EXT_RISK          = 1u << 6,
    /** The retained intrabar execution path. Needs the N8 tail. */
    PF_NATIVE_SPEC_EXT_INTRABAR      = 1u << 7,
    /** The four feed-shape and presentation policies: slot labels, feed
     *  tolerance, the forced path order and abort reporting. Needs the N8
     *  tail. */
    PF_NATIVE_SPEC_EXT_FEED_POLICY   = 1u << 8,
    /** The auxiliary finer feed. Only a caller whose
     *  pf_native_run_spec_ext_v1 carries the auxiliary tail may set this bit;
     *  a caller sending any earlier layout is refused with
     *  PF_NATIVE_E_STRUCT. */
    PF_NATIVE_SPEC_EXT_AUXILIARY_FEED = 1u << 9
} pf_native_spec_ext_mask_t;

/** The bars a declared series is built from — `NativeSeriesSource`. */
typedef enum pf_native_series_source_e {
    PF_NATIVE_SERIES_SOURCE_INPUT          = 0, /**< The accepted input (the default). */
    PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED = 1  /**< The run's auxiliary finer feed. */
} pf_native_series_source_t;

/** When a declared series delivers a completed bucket —
 *  `NativeTimeframeSubscription::lookahead`, the `lookahead` word of
 *  #pf_native_subscription_v1. AT_FIRST_INPUT resolves the historical input
 *  ahead; a stream's live inputs have nothing ahead to resolve, so under
 *  either value they deliver each bucket at its completion. */
typedef enum pf_native_lookahead_e {
    PF_NATIVE_LOOKAHEAD_AT_COMPLETION  = 0, /**< On the input that completes the
                                             *   bucket (the default). */
    PF_NATIVE_LOOKAHEAD_AT_FIRST_INPUT = 1  /**< The bucket's final values, on its
                                             *   first contributing input. */
} pf_native_lookahead_t;

/** What a declared series holds on an accepted input that delivers no bucket
 *  of it — `NativeTimeframeSubscription::gaps`, the `gaps` word of
 *  #pf_native_subscription_v1. Neither value changes which buckets complete,
 *  when they are delivered or what they contain. */
typedef enum pf_native_gaps_e {
    PF_NATIVE_GAPS_HOLD  = 0, /**< The last delivered bucket, until the next
                               *   delivery replaces it (the default). */
    PF_NATIVE_GAPS_CLEAR = 1  /**< Nothing: #strategy_native_series_bar_v1
                               *   answers #PF_NATIVE_ABSENT on that input. */
} pf_native_gaps_t;

/** WHICH price a kernel-sized basis converts at — `native_order::SizePrice`
 *  (L3b). RESOLVED is the established behaviour: the price the kernel would
 *  otherwise settle at. SIGNAL is the decision-point price at placement,
 *  carried to the expected fill by the side's slippage and rounded onto the
 *  run's fill grid. SIGNAL_ON_TICK is that same rule measured on the
 *  INSTRUMENT's own tick ladder (`price_tick`) instead of the fill grid,
 *  rounded before the slippage as well as after. */
typedef enum pf_native_size_price_e {
    PF_NATIVE_SIZE_PRICE_RESOLVED       = 0,
    PF_NATIVE_SIZE_PRICE_SIGNAL         = 1,
    PF_NATIVE_SIZE_PRICE_SIGNAL_ON_TICK = 2
} pf_native_size_price_t;

/** WHICH measurement of the bound scope a fractional reduce takes its
 *  fraction of — `native_order::ScopeBasis`. AT_MATCH reads the scope as it
 *  stands at the matching candidate. AT_ACCEPTANCE freezes the scope SIZE
 *  when the request is accepted, so two 50 % siblings on one 10-unit lot both
 *  claim 5 under GROSS even after the first has executed. */
typedef enum pf_native_scope_basis_e {
    PF_NATIVE_SCOPE_BASIS_AT_MATCH      = 0,
    PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE = 1
} pf_native_scope_basis_t;

/** When the kernel tests the maintenance requirement —
 *  `NativeLiquidationCheck`, the `margin_check` field of
 *  #pf_native_run_spec_ext_v1. */
typedef enum pf_native_liquidation_check_e {
    PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME      = 0, /**< Solve and rest at the level. */
    PF_NATIVE_LIQUIDATION_CALCULATION_ONLY          = 1, /**< Test the mark; rest nothing. */
    PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK = 2  /**< Rest AT the adverse mark. */
} pf_native_liquidation_check_t;

/** Which equity the maintenance requirement is tested against —
 *  `NativeMarginEquityBasis`. */
typedef enum pf_native_margin_equity_basis_e {
    PF_NATIVE_MARGIN_EQUITY_MARKED                 = 0,
    PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION = 1
} pf_native_margin_equity_basis_t;

/** Which base the liquidation level is solved from —
 *  `NativeLiquidationLevelBase`. */
typedef enum pf_native_margin_level_base_e {
    PF_NATIVE_MARGIN_LEVEL_MARKED_EQUITY = 0,
    PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY = 1
} pf_native_margin_level_base_t;

/** Which intrabar execution path the run retains — the alternative of
 *  `IntrabarPath`. NONE is the whole default surface. LOWER_TF retains the
 *  caller's own finer bars (and is the one mode that delivers
 *  #pf_native_callbacks_v1::on_sub_bar). SYNTHESIZED samples each script
 *  bar's own OHLC path through the generic sampler and retains no feed. */
typedef enum pf_native_intrabar_kind_e {
    PF_NATIVE_INTRABAR_NONE        = 0,
    PF_NATIVE_INTRABAR_LOWER_TF    = 1,
    PF_NATIVE_INTRABAR_SYNTHESIZED = 2
} pf_native_intrabar_kind_t;

/** Whether matching stays continuous between generated samples —
 *  `IntrabarPath::SampleEligibility`. LOWER_TF only; a synthesized path's
 *  point-only eligibility is inherent to that mode. */
typedef enum pf_native_sample_eligibility_e {
    PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS   = 0,
    PF_NATIVE_SAMPLE_DISTRIBUTION_SAMPLES  = 1
} pf_native_sample_eligibility_t;

/** Whether a confirmed bar must name a canonical input slot —
 *  `NativeSlotLabelPolicy`. A feed-shape policy, not a source-language one;
 *  the two modes never share a continuation. */
typedef enum pf_native_slot_label_e {
    PF_NATIVE_SLOT_LABEL_CANONICAL     = 0,
    PF_NATIVE_SLOT_LABEL_FEED_TOLERANT = 1
} pf_native_slot_label_t;

/** Opt-in admission exceptions for a tolerated input-feed shape —
 *  `NativeFeedTolerance`. A BIT MASK, not an enumerator: the values combine,
 *  and any bit outside this set is PF_NATIVE_E_TAG. */
typedef enum pf_native_feed_tolerance_e {
    PF_NATIVE_FEED_TOLERANCE_NONE               = 0,
    /** Finite OHLC need not be positive; NaN volume means unavailable. */
    PF_NATIVE_FEED_TOLERANCE_BATCH_STRUCTURAL   = 1u << 0,
    /** Stream warmups admit finite, non-negative interim OHLC. */
    PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE = 1u << 1
} pf_native_feed_tolerance_t;

/** Generic ordering for a modeled OHLC path — `NativePathOrder`. AUTO keeps
 *  the open-proximity rule; the forced modes make the first excursion
 *  explicit for replay and live hosts. */
typedef enum pf_native_path_order_e {
    PF_NATIVE_PATH_ORDER_AUTO       = 0,
    PF_NATIVE_PATH_ORDER_HIGH_FIRST = 1,
    PF_NATIVE_PATH_ORDER_LOW_FIRST  = 2
} pf_native_path_order_t;

/** How a cooperative abort is presented — `NativeAbortReporting`. */
typedef enum pf_native_abort_reporting_e {
    PF_NATIVE_ABORT_ERROR = 0,
    PF_NATIVE_ABORT_QUIET = 1
} pf_native_abort_reporting_t;

/** Why the kernel is asking the host to calculate — `NativeCalculationReason`
 *  (L5), the `reason` argument of #pf_native_callbacks_v1::on_recalculate.
 *  SUB_BAR is reserved and never delivered there: a lower-timeframe sub-bar
 *  has its own hook, #pf_native_callbacks_v1::on_sub_bar. */
typedef enum pf_native_calc_reason_e {
    PF_NATIVE_CALC_BAR_CLOSE  = 0, /**< The script bar's own calculation; `cause` NULL. */
    PF_NATIVE_CALC_ORDER_FILL = 1, /**< At an applied execution's cursor; `cause` is it. */
    PF_NATIVE_CALC_TICK       = 2, /**< At a modeled point or print; `cause` NULL. */
    PF_NATIVE_CALC_SUB_BAR    = 3  /**< Reserved; see on_sub_bar. */
} pf_native_calc_reason_t;

/** Which kernel check point is about to test the maintenance requirement —
 *  `NativeMarginCheckKind` (L4b). These are the kernel's own points; a broker
 *  model that checks somewhere else is host policy, expressed by suppressing
 *  the points it does not share. */
typedef enum pf_native_margin_check_kind_e {
    PF_NATIVE_MARGIN_CHECK_BAR_OPEN      = 0, /**< The script bar's open. */
    PF_NATIVE_MARGIN_CHECK_AFTER_APPLIED = 1, /**< The re-arm after a point's fills. */
    PF_NATIVE_MARGIN_CHECK_CALCULATION   = 2, /**< A CalculationOnly model's calculation. */
    PF_NATIVE_MARGIN_CHECK_FX_ROLL       = 3  /**< A step of the run's declared
                                               *   #pf_native_fx_curve_v1: the first point
                                               *   the account converts at a new rate,
                                               *   offered immediately before that point is
                                               *   matched. A run that declares no curve has
                                               *   none, and a CalculationOnly model, which
                                               *   measures at its calculation alone, is not
                                               *   offered it. */
} pf_native_margin_check_kind_t;

/** What an ANSWERING callback's return value means.
 *
 *  The four answering hooks of #pf_native_callbacks_v1 do not report success:
 *  their return value selects WHOSE answer the kernel uses, so every value is
 *  in contract and an answering hook can never fail the run. A host that
 *  needs to abort does it from an observation callback, which keeps the
 *  "non-zero ends the run Failed" rule exactly where it already was. */
typedef enum pf_native_answer_e {
    PF_NATIVE_ANSWER_DEFAULT  = 0, /**< Keep the kernel's own; the output is ignored. */
    PF_NATIVE_ANSWER_PROVIDED = 1  /**< Use the output. Any non-zero value means this. */
} pf_native_answer_t;

/** Where an opened lot's own fill sits on its entry bar's modeled path —
 *  `OpenedLotFillPoint`, the one fact
 *  #strategy_native_declare_opened_lot_entry_bar_mask_v1 carries. ON_PATH is
 *  a fill at a price the path reaches: the kernel derives which end of the
 *  bar, if either, the path had already reached before it. AFTER_PATH is a
 *  fill at the bar's closing point, after the whole path: both ends precede
 *  it. */
typedef enum pf_native_opened_lot_fill_point_e {
    PF_NATIVE_OPENED_LOT_FILL_POINT_ON_PATH    = 0,
    PF_NATIVE_OPENED_LOT_FILL_POINT_AFTER_PATH = 1
} pf_native_opened_lot_fill_point_t;

/** @} */ /* end of pf_native_c_enums */

/** @defgroup pf_native_c_types Transport types
 *  @{ */

/** Where the kernel is, presented to every callback.
 *
 *  A read-only snapshot of `NativeDecisionContext` plus the current quote:
 *  `price` is the execution point's price where one exists and NaN where the
 *  callback has no execution point (the bar's own close calculation). */
typedef struct pf_native_decision_v1 {
    uint32_t struct_size;        /**< sizeof(pf_native_decision_v1). */
    uint32_t version;            /**< PF_NATIVE_API_VERSION. */
    uint64_t ordinal;            /**< Matching-point ordinal. */
    int32_t  interval_index;     /**< Script interval index. */
    int32_t  sub_index;          /**< Sub-bar index inside the script bar. */
    int32_t  sub_count;          /**< Sub-bars in this script bar (1 when unmagnified). */
    int32_t  is_terminal_sub_bar; /**< 1 on the script bar's last sub-bar. */
    int64_t  effective_time_ms;  /**< The point's effective decision time. */
    int64_t  script_bar_open_ms; /**< Open of the script bar under delivery. */
    int64_t  sub_bar_open_ms;    /**< Open of the sub-bar under delivery. */
    int64_t  decision_floor_ms;  /**< Lower bound a new request may be matched at. */
    double   price;              /**< Current quote, NaN outside an execution point. */
    uint8_t  provenance;         /**< NativePriceProvenance. */
    uint8_t  path_phase;         /**< NativePathPhase. */
    uint8_t  completion;         /**< NativeCompletionKind. */
    uint8_t  quote_kind;         /**< NativeCurrentQuoteKind; 0 when price is NaN. */
} pf_native_decision_v1;

/** One applied execution, presented to `on_applied`. */
typedef struct pf_native_applied_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_applied_v1). */
    uint32_t version;       /**< PF_NATIVE_API_VERSION. */
    uint64_t ordinal;       /**< Event ordinal. */
    uint64_t incarnation;   /**< The request that executed. */
    uint64_t opened_lot_incarnation; /**< The lot this fill opened, 0 when none. */
    double   raw_price;     /**< The path price the match was found at. */
    double   resolved_price; /**< The booked price after terms and slippage. */
    double   closed_units;  /**< Units closed by this fill. */
    double   opened_units;  /**< Units opened by this fill. */
    double   filled_working; /**< Working units this fill consumed. */
    double   ticket;        /**< `current_ticket` at the fill. */
    int64_t  cycle_before;  /**< Position cycle before the fill. */
    int64_t  cycle_after;   /**< Position cycle after the fill. */
    uint8_t  terminal;      /**< 1 when the request is finished. */
    uint8_t  terminal_reason;     /**< AppliedTerminalReason, valid when `has_terminal_reason`. */
    uint8_t  has_terminal_reason; /**< 1 when `terminal_reason` is meaningful. */
    uint8_t  reserved0;
} pf_native_applied_v1;

/** One recorded event, read back by #strategy_native_events_v1.
 *
 *  `kind` tags the union: every field below is documented per kind and is
 *  zero where that kind has no such fact.
 *   - ACCEPTED / ARMED / CLOSE_BOUND / QUANTITY_BOUND: `incarnation`.
 *   - REJECTED / REPLACE_REJECTED: `reason` is a RequestRejectReason.
 *   - REPLACED: `incarnation` is the predecessor, `successor` the new handle.
 *   - CANCELLED: `reason` is a CancelReason.
 *   - MATCH_REJECTED: `reason` is a MatchRejectReason, cursor fields set.
 *   - APPLIED: every price/unit/cycle field, `terminal`, cursor fields.
 *   - MARGIN_CALL: `price` = mark, `closed_units` = liquidated units,
 *     `raw_price` = the re-solved liquidation price.
 *   - ACTIVATED: `reason` is an ActivationKind, `price` the reached price.
 *   - DRIVER_POINT: cursor fields and `raw_price`.
 *   - ACCOUNT: `price` = marked equity, `raw_price` = realized balance,
 *     `opened_units` = signed position units.
 *   - RISK: `reason` is a #pf_native_risk_limit_t, `price` = the observed
 *     value that reached the limit, `raw_price` = the limit it was measured
 *     against (account currency for the two loss limits — a percent limit is
 *     already resolved against its basis equity — days or fills for the two
 *     counts), `cycle_before` = the risk day the breach happened on, on the
 *     spec's own day basis, `successor` = the cursor's matching-point
 *     ordinal, cursor fields set. The event names no request: it is an
 *     account fact, so `incarnation` stays 0. */
typedef struct pf_native_event_v1 {
    uint32_t struct_size;       /**< sizeof(pf_native_event_v1). */
    uint32_t version;           /**< PF_NATIVE_API_VERSION. */
    uint32_t kind;              /**< #pf_native_event_kind_t. */
    uint32_t reason;            /**< Per-kind reason enumerator, 0 when none. */
    uint64_t ordinal;           /**< Event ordinal; strictly increasing. */
    uint64_t incarnation;       /**< Subject request, 0 when the kind has none. */
    uint64_t successor;         /**< REPLACED: the new handle. RISK: the cursor's
                                 *   matching-point ordinal. 0 otherwise. */
    double   raw_price;
    double   resolved_price;
    double   price;
    double   closed_units;
    double   opened_units;
    int64_t  cycle_before;
    int64_t  cycle_after;
    int64_t  effective_time_ms; /**< Cursor effective time, 0 when the kind has no cursor. */
    int32_t  interval_index;    /**< Cursor interval index. */
    uint8_t  provenance;        /**< Cursor NativePriceProvenance. */
    uint8_t  path_phase;        /**< Cursor NativePathPhase. */
    uint8_t  terminal;          /**< APPLIED only. */
    uint8_t  reserved0[3];
} pf_native_event_v1;

/** One live request, copied out by #strategy_native_working_get_v1.
 *
 *  `label` and `comment` borrow the snapshot taken by the most recent
 *  #strategy_native_working_len_v1 call on this handle. They stay valid until
 *  the next call to that function, or until the host is freed; copy them if
 *  the host keeps them longer.
 *
 *  This readout has TWO published layouts and the runtime fills either: the
 *  base layout the L13 lane first shipped (#PF_NATIVE_WORKING_V1_BASE_SIZE)
 *  and the current one, which appends `trail_has_arm_price`. A caller
 *  compiled against the base layout keeps working unchanged: its row is
 *  written up to `comment` and never past it. Any other `struct_size` is
 *  PF_NATIVE_E_STRUCT.
 *
 *  `trail_has_arm_price` is the request's own flag read back, so a TRAIL with
 *  no arm price and one armed at 0.0 are two different rows even though both
 *  read `p2` = 0. An anchored trail (#PF_NATIVE_ANCHOR_FROM_OWNER_FILL) reads
 *  back the spelling it was submitted with until its owner fills; the arm
 *  then installs the level, and from there it reads 1 with that level in
 *  `p2`. A tick-spelled offset needs no such flag: acceptance resolves it
 *  into `p1` as a price distance. */
typedef struct pf_native_working_v1 {
    uint32_t struct_size;     /**< sizeof(pf_native_working_v1). */
    uint32_t version;         /**< PF_NATIVE_API_VERSION. */
    uint64_t incarnation;     /**< The request's handle. */
    uint32_t intent;          /**< #pf_native_intent_t as accepted. */
    uint32_t trigger;         /**< #pf_native_trigger_t as accepted. */
    uint32_t owner;           /**< #pf_native_owner_t as accepted. */
    uint32_t capacity;        /**< #pf_native_capacity_t as accepted. */
    uint32_t group_kind;      /**< #pf_native_group_t as accepted. */
    uint32_t group_effect;    /**< #pf_native_group_effect_t, 0 when no group. */
    uint32_t remaining_kind;  /**< #pf_native_remaining_t. */
    uint32_t trigger_state;   /**< #pf_native_trigger_state_t. */
    uint32_t origin;          /**< RequestOrigin: 0 host, 1 kernel liquidation, 2 kernel risk. */
    uint32_t reserved0;
    double   intent_value;    /**< The intent's own scalar, 0 when it has none. */
    double   p1;              /**< Trigger level 1 (limit / stop / trail offset). */
    double   p2;              /**< Trigger level 2 (stop-limit limit / trail arm
                               *   when `trail_has_arm_price`). */
    double   capacity_units;  /**< Point budget, 0 for immediate capacity. */
    double   remaining_units; /**< Valid when `remaining_kind` is UNITS. */
    uint64_t group_id;
    int64_t  group_cohort;
    uint64_t acceptance_ordinal;      /**< Birth: the accepting event. */
    int64_t  decision_time_lower_bound; /**< Birth: earliest matchable time. */
    const char* label;        /**< Borrowed; see the struct note. */
    const char* comment;      /**< Borrowed; see the struct note. */

    /* ── The additive arm-presence tail. Written only when `struct_size` is
     * the current sizeof; a caller sending the base layout is filled up to
     * `comment` above and never past it. ── */
    uint32_t trail_has_arm_price; /**< TRAIL: 1 when `p2` is the arm price, 0 when
                                   *   the trail carries none. 0 for every other
                                   *   trigger. */
    uint32_t reserved1;           /**< Always 0. */
} pf_native_working_v1;

/** Byte length of #pf_native_working_v1 as the L13 lane first published it,
 *  before the arm-presence tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds
 *  for — it is not a literal. #strategy_native_working_get_v1 accepts this
 *  length as well as the current `sizeof`, and writes no byte past the
 *  length it was handed, which is what makes a readout's tail additive
 *  rather than a layout break. */
#define PF_NATIVE_WORKING_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_working_v1, trail_has_arm_price))

/** One open physical lot, copied out by #strategy_native_open_lot_get_v1 —
 *  the C spelling of `NativeOpenLot` (R5 gap lane N18): the book that
 *  #strategy_native_position_v1 aggregates, lot by lot, marked at the price
 *  #strategy_native_open_lot_count_v1 was given. Every field is what the
 *  kernel already holds for the lot; reading it moves nothing.
 *
 *  `unrealized_pnl` is the lot's own term of the marked equity: the move from
 *  `entry_price` to `mark`, in account currency, less `entry_commission`.
 *  The two excursions are the largest moves for and against the lot the
 *  kernel has sampled along the delivered path, in account currency, gross
 *  of fees, with `mark` folded in. A NaN `mark` keeps every booking fact,
 *  leaves `unrealized_pnl` NaN and folds nothing into the excursions.
 *
 *  `entry_label` and `entry_comment` borrow the snapshot taken by the most
 *  recent #strategy_native_open_lot_count_v1 call on this handle. They stay
 *  valid until the next call to that function, or until the host is freed;
 *  copy them if the host keeps them longer. */
typedef struct pf_native_open_lot_v1 {
    uint32_t struct_size;        /**< sizeof(pf_native_open_lot_v1). */
    uint32_t version;            /**< PF_NATIVE_API_VERSION. */
    uint64_t ordinal;            /**< Position in the book, oldest first. */
    uint64_t entry_incarnation;  /**< The request whose fill opened the lot; never
                                  *   reused; 0 only for a legacy synthetic lot. */
    int64_t  cycle;              /**< The position cycle the lot belongs to — the
                                  *   `owner_cycle` a BIND_OPENING(S) request names. */
    uint32_t side;               /**< #pf_native_side_t. */
    int32_t  entry_bar_index;    /**< Script-bar index of the opening fill. */
    int64_t  entry_time_ms;      /**< Effective time of the opening fill. */
    double   entry_price;        /**< Booked entry price. */
    double   signed_units;       /**< Remaining units: > 0 long, < 0 short. */
    double   entry_commission;   /**< Entry fee still on the lot, account currency;
                                  *   a partial realization takes its share with it. */
    double   mark;               /**< The price the three fields below were marked at. */
    double   unrealized_pnl;     /**< Fee-net move to `mark`, account currency; NaN for a NaN mark. */
    double   favorable_excursion; /**< Largest move for the lot so far, account currency, >= 0. */
    double   adverse_excursion;  /**< Largest move against the lot so far, account currency, >= 0. */
    const char* entry_label;     /**< Borrowed; see the struct note. */
    const char* entry_comment;   /**< Borrowed; see the struct note. */
} pf_native_open_lot_v1;

/** The run's lifecycle and its typed failure. */
typedef struct pf_native_state_v1 {
    uint32_t struct_size;      /**< sizeof(pf_native_state_v1). */
    uint32_t version;          /**< PF_NATIVE_API_VERSION. */
    uint32_t lifecycle;        /**< #pf_native_lifecycle_t. */
    uint32_t failure_code;     /**< NativeFailureCode; PF_NATIVE_FAILURE_CALLBACK for a
                                *   callback that returned non-zero. */
    uint32_t failure_operation; /**< NativeFailureOperation. */
    uint32_t failure_discriminator;
    uint64_t failure_ordinal;  /**< The point the failure was latched at, 0 when absent. */
    uint64_t consumed_high_water;
    int64_t  decision_floor_ms;
    uint32_t phase;            /**< NativeRunPhase while Running. */
    uint32_t completion;       /**< NativeCompletion once Completed. */
} pf_native_state_v1;

/** The trail projection of #strategy_native_trail_state_v1 — the C spelling
 *  of `NativeTrailState`. Before the arm is reached `activated` is 0 and
 *  every other field is 0; once armed, `best_price` and `current_level` are
 *  the exact raw matcher values and `activation_ordinal` identifies the
 *  TrailArm event that began tracking. */
typedef struct pf_native_trail_state_v1 {
    uint32_t struct_size;        /**< sizeof(pf_native_trail_state_v1). */
    uint32_t version;            /**< PF_NATIVE_API_VERSION. */
    uint32_t activated;          /**< 0/1: the arm price has been reached. */
    uint32_t reserved0;
    double   best_price;         /**< Running best; 0 before the arm. */
    double   current_level;      /**< The stop the best is riding; 0 before the arm. */
    uint64_t activation_ordinal; /**< The TrailArm event; 0 before the arm. */
} pf_native_trail_state_v1;

/** The facts one margin hook is handed — the C spelling of
 *  `NativeMarginRequirementView`, `NativeMarginCheckPoint` and
 *  `NativeMarginCallView`, which differ only in which of these facts they
 *  carry. Every field is documented per hook and is zero where that hook has
 *  no such fact, exactly as #pf_native_event_v1's union is:
 *
 *   - on_margin_check: `kind`, `liquidation_resting`, the position, `mark`
 *     and the cursor. `equity` and `required` are 0 — nothing has been
 *     evaluated yet at that point.
 *   - on_margin_requirement: `kind`, the position, `mark`, and the two
 *     numbers the kernel is ABOUT to compare (`equity`, `required`) — exactly
 *     what it would compare if the host answered PF_NATIVE_ANSWER_DEFAULT.
 *   - on_margin_call_units: the position being liquidated, the sizing `mark`,
 *     the `equity` and `required` measured there, and the cursor. `kind` and
 *     `liquidation_resting` are 0: a call is not a check point. */
typedef struct pf_native_margin_view_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_margin_view_v1). */
    uint32_t version;       /**< PF_NATIVE_API_VERSION. */
    uint32_t kind;          /**< #pf_native_margin_check_kind_t. */
    uint32_t liquidation_resting; /**< 0/1: a liquidation rests from an earlier point. */
    double   signed_units;  /**< The book being measured. */
    double   average_price;
    uint64_t lot_count;
    double   mark;          /**< The price the breach is measured at. */
    double   equity;        /**< Marked equity on the model's basis. */
    double   required;      /**< Maintenance requirement of the whole position at `mark`. */
    uint64_t cursor_ordinal;
    int64_t  cursor_effective_time_ms;
    double   cursor_t;
    int32_t  cursor_interval_index;
    uint8_t  cursor_provenance; /**< NativePriceProvenance. */
    uint8_t  cursor_path_phase; /**< NativePathPhase. */
    uint8_t  reserved0[2];
} pf_native_margin_view_v1;

/** The host's answer to one requirement view — `NativeMarginDecision`.
 *  `required` and `equity` replace the kernel's two numbers for that check
 *  point only: they are a broker's money rule, never a second account.
 *  `force_breach` makes the kernel proceed past `required > equity` even when
 *  the answered numbers do not meet it. Read only when the hook answers
 *  #PF_NATIVE_ANSWER_PROVIDED; a nonfinite `required` or `equity` is refused
 *  by the kernel and the check point is abandoned, exactly as in C++. */
typedef struct pf_native_margin_decision_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_margin_decision_v1). */
    uint32_t version;       /**< PF_NATIVE_API_VERSION. */
    uint32_t force_breach;  /**< 0/1. */
    uint32_t reserved0;
    double   required;
    double   equity;
} pf_native_margin_decision_v1;

/** The facts a host-sized CLOSE is resolved from, handed to
 *  #pf_native_callbacks_v1::on_close_units.
 *
 *  It is the UNITS half of `NativeExecutionTermsFacts`, and only that half:
 *  `scope_exposure_units` is what the bound scope holds at this candidate —
 *  for a cohort close, the live units of the roster's own openings and
 *  nothing else. The price half and the opening shape stay the kernel's. */
typedef struct pf_native_close_view_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_close_view_v1). */
    uint32_t version;       /**< PF_NATIVE_API_VERSION. */
    uint64_t incarnation;   /**< The request being resolved. */
    double   scope_exposure_units;   /**< What the bound scope holds here. */
    double   default_resolved_price; /**< The price the kernel would settle at. */
    double   position_units;         /**< The whole physical book, signed. */
    uint64_t cursor_ordinal;
    int64_t  cursor_effective_time_ms;
    uint32_t is_buy;        /**< 0/1: the side this candidate would trade on. */
    uint32_t reserved0;
} pf_native_close_view_v1;

/** One closing lot's booking facts — `ClosedLotExcursionFacts` (RULING A48).
 *  Handed to #pf_native_callbacks_v1::on_lot_excursion, whose two outputs are
 *  the favorable and adverse magnitudes the closing row then carries. Facts
 *  in, magnitudes out: nothing about the host's price model crosses the
 *  boundary in either direction. The two entry-bar masks are what
 *  #strategy_native_declare_opened_lot_entry_bar_mask_v1 derived for the lot,
 *  and 0 for a lot nobody declared. */
typedef struct pf_native_lot_excursion_v1 {
    uint32_t struct_size;      /**< sizeof(pf_native_lot_excursion_v1). */
    uint32_t version;          /**< PF_NATIVE_API_VERSION. */
    uint64_t entry_incarnation; /**< The lot's opening request. */
    int64_t  entry_time_ms;
    double   entry_price;
    double   lot_qty;          /**< The whole lot. */
    double   closed_qty;       /**< What this fill closes of it. */
    double   fill_price;
    double   carried_favorable; /**< What the lot carried in, for a partial close. */
    double   carried_adverse;
    int32_t  entry_bar_index;
    int32_t  exit_bar_index;
    uint8_t  is_long;
    uint8_t  entry_bar_high_masked; /**< The entry bar's high is not this trade's. */
    uint8_t  entry_bar_low_masked;  /**< The entry bar's low is not this trade's. */
    uint8_t  reserved0;
} pf_native_lot_excursion_v1;

/** The run's generic risk ledger — the C spelling of `NativeRiskState` (L9).
 *
 *  Every field is its zero for a run that declares no risk block, exactly as
 *  the C++ value is. `blocked` is whether openings are refused right now and
 *  `reason` names the limit that did it (valid only when `has_reason`);
 *  `day_ordinal` is the risk day the ledger is on, on the spec's own day
 *  basis, and is meaningful only when `has_day`. Observation only: reading it
 *  moves nothing. */
typedef struct pf_native_risk_state_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_risk_state_v1). */
    uint32_t version;       /**< PF_NATIVE_API_VERSION. */
    uint32_t blocked;       /**< 0/1: openings are refused right now. */
    uint32_t has_reason;    /**< 0/1: `reason` is meaningful. */
    uint32_t reason;        /**< #pf_native_risk_limit_t that blocked. */
    uint32_t has_day;       /**< 0/1: the ledger has reached a day. */
    uint32_t consecutive_loss_days; /**< Days that closed with a realized loss. */
    uint32_t reserved0;
    int64_t  day_ordinal;   /**< The risk day, on the spec's day basis. */
    uint64_t fills_today;   /**< Applied fills counted in that day. */
    double   peak_equity;      /**< Running peak of the marked equity. */
    double   day_open_equity;  /**< Equity the current day opened at. */
} pf_native_risk_state_v1;

/** One order request. Translated field by field into `native_order::Request`;
 *  it is never cast. Zero-initialise it, set `struct_size` and `version`, then
 *  set only the fields the chosen `intent` and `trigger` document.
 *
 *  This struct has THREE published layouts and the runtime accepts any of
 *  them: the base layout the L13 lane first shipped
 *  (#PF_NATIVE_REQUEST_V1_BASE_SIZE), that layout plus L7b's anchored-leg
 *  tail (#PF_NATIVE_REQUEST_V1_ANCHOR_SIZE), and the current one, which
 *  appends L3b's sizing detail (`size_price`, `reduce_basis`). A caller
 *  compiled against an earlier layout keeps working unchanged and simply gets
 *  the later tails' defaults (RAW, WORKING, RESOLVED, AT_MATCH). Any other
 *  `struct_size` is PF_NATIVE_E_STRUCT. Both tails are append-only: nothing
 *  above them moved. */
typedef struct pf_native_request_v1 {
    uint32_t struct_size;     /**< sizeof(pf_native_request_v1). */
    uint32_t version;         /**< PF_NATIVE_API_VERSION. */

    /* Intent */
    uint32_t intent;              /**< #pf_native_intent_t. */
    uint32_t reduce_size;         /**< #pf_native_reduction_t, REDUCE only. */
    uint32_t reduce_claim;        /**< #pf_native_scope_claim_t, SCOPE_FRACTION only. */
    uint32_t side;                /**< #pf_native_side_t, SIZED only. */
    uint32_t size_basis;          /**< #pf_native_size_basis_t, SIZED only. */
    uint32_t size_time;           /**< #pf_native_size_time_t, SIZED only. */
    uint32_t grid_policy;         /**< #pf_native_grid_policy_t, SIZED only. */
    uint32_t reserve_percent_fee; /**< 0/1, SIZED only. */
    double   intent_value;        /**< The intent's own scalar; see #pf_native_intent_t. */

    /* Trigger */
    uint32_t trigger;             /**< #pf_native_trigger_t. */
    uint32_t anchor;              /**< #pf_native_anchor_t (L7). */
    double   p1;                  /**< Limit/stop price, or trail offset. */
    double   p2;                  /**< Stop-limit limit, or trail arm price. */
    double   anchor_offset;       /**< FROM_OWNER_FILL: signed offset. */
    uint8_t  fill_through;        /**< LIMIT only: market-if-touched. */
    uint8_t  trail_offset_in_ticks;  /**< TRAIL: p1 is a tick count, not a distance. */
    uint8_t  trail_has_arm_price;    /**< TRAIL: p2 is the arm price. */
    uint8_t  anchor_offset_in_ticks; /**< FROM_OWNER_FILL: the offset is a tick count. */

    /* Capacity */
    uint32_t capacity;            /**< #pf_native_capacity_t. */
    double   capacity_units;      /**< POINT_BUDGET only. */

    /* Owner */
    uint32_t owner;               /**< #pf_native_owner_t. */
    uint32_t owner_n;             /**< Length of `owner_incarnations`. */
    const uint64_t* owner_incarnations; /**< Borrowed for the call only. */
    int64_t  owner_cycle;         /**< BIND_OPENING / BIND_OPENINGS. */
    uint64_t cohort;              /**< BIND_COHORT. */

    /* Group */
    uint32_t group_kind;          /**< #pf_native_group_t. */
    uint32_t group_effect;        /**< #pf_native_group_effect_t. */
    uint64_t group_id;            /**< MEMBER only. */
    int64_t  group_cohort;        /**< MEMBER only. */

    /* Identity. Both borrowed for the call only; the kernel copies them.
     * NULL is the empty string. */
    const char* label;
    const char* comment;

    /* ── The additive anchored-leg tail (L7b). Read only when `struct_size`
     * is the current sizeof; a caller sending the base layout stops at
     * `comment` above and gets every default (RAW, WORKING). ── */
    uint32_t anchor_rounding;     /**< #pf_native_anchor_rounding_t, FROM_OWNER_FILL only. */
    uint32_t visibility;          /**< #pf_native_arm_visibility_t, WAIT_FOR_APPLIED only. */

    /* ── The additive sizing-detail tail (L3b). Read only when `struct_size`
     * is the current sizeof; a caller sending either earlier layout stops
     * above and gets both defaults (RESOLVED, AT_MATCH), which is what every
     * request accepted before this tail already resolved as. ── */
    uint32_t size_price;          /**< #pf_native_size_price_t, SIZED only. */
    uint32_t reduce_basis;        /**< #pf_native_scope_basis_t, SCOPE_FRACTION only. */
} pf_native_request_v1;

/** Byte length of #pf_native_request_v1 as the L13 lane first published it,
 *  before the anchored-leg tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds
 *  for — it is not a literal. The runtime accepts this length as well as the
 *  current `sizeof`, which is what makes the tail additive rather than a
 *  layout break. */
#define PF_NATIVE_REQUEST_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, anchor_rounding))

/** Byte length of #pf_native_request_v1 with L7b's anchored-leg tail but
 *  without L3b's sizing-detail tail — the second of its three published
 *  layouts. Defined as the offset of the first field appended after it, for
 *  the same reason #PF_NATIVE_REQUEST_V1_BASE_SIZE is. */
#define PF_NATIVE_REQUEST_V1_ANCHOR_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, size_price))

/** One declared higher-timeframe series of #pf_native_run_spec_ext_v1.
 *
 *  A row is a series INSTANCE, not a period: several rows may carry the same
 *  `tf`, each delivered under its own index (the `subscription` argument of
 *  #pf_native_callbacks_v1::on_timeframe_bar). Same-period rows may not carry
 *  DIFFERENT `authoritative_bars`.
 *
 *  `gaps` occupies the word this struct published as `reserved0`, which every
 *  layout required to be zero — so a caller that zero-fills the struct keeps
 *  PF_NATIVE_GAPS_HOLD, and the struct's size and field offsets are unchanged.
 *  A value outside either word's enumeration is PF_NATIVE_E_TAG. */
typedef struct pf_native_subscription_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_subscription_v1). */
    uint32_t lookahead;     /**< #pf_native_lookahead_e. */
    const char* tf;         /**< Non-NULL timeframe literal. */
    const pf_bar_t* authoritative_bars; /**< Optional exchange bars; copied. */
    int32_t authoritative_n;            /**< Length of `authoritative_bars`. */
    uint32_t gaps;          /**< #pf_native_gaps_e. */
} pf_native_subscription_v1;

/** The run-specification fields #pf_native_run_spec_v1 predates.
 *
 *  It is a companion to #pf_native_run_spec_v1, not a second configure step:
 *  the kernel configures a host exactly once, so both halves are handed to
 *  #strategy_configure_native_ext_v1 together and that call replaces
 *  #strategy_configure_native_v1 for a host that needs these fields.
 *  `present_mask` selects the blocks that are meaningful; an absent block
 *  keeps the kernel's own default, so an all-zero mask configures exactly
 *  what #strategy_configure_native_v1 would have.
 *
 *  Every field of NativeRunSpec that is not fixed by
 *  #pf_native_run_spec_v1 now travels here. The one deliberate omission is
 *  `identity`, which the base spec owns.
 *
 *  This struct has FOUR published layouts and the runtime accepts any of
 *  them: the base layout the L13 lane first shipped
 *  (#PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE); that layout plus L9's `risk_*`
 *  tail (#PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE); that one plus N8's intrabar
 *  path, the four feed-shape and presentation policies, and the margin
 *  model's equity basis, level base and liquidation strings
 *  (#PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE); and the current one, which
 *  appends the `auxiliary_*` tail after them. A caller compiled against an
 *  earlier layout keeps working unchanged and simply cannot set the mask
 *  bits its struct has no fields for (#PF_NATIVE_SPEC_EXT_RISK,
 *  #PF_NATIVE_SPEC_EXT_INTRABAR, #PF_NATIVE_SPEC_EXT_FEED_POLICY,
 *  #PF_NATIVE_SPEC_EXT_AUXILIARY_FEED): doing so is PF_NATIVE_E_STRUCT. Any
 *  other `struct_size` is PF_NATIVE_E_STRUCT too. Every tail is append-only:
 *  nothing above them moved. */
typedef struct pf_native_run_spec_ext_v1 {
    uint32_t struct_size;    /**< sizeof(pf_native_run_spec_ext_v1). */
    uint32_t version;        /**< PF_NATIVE_API_VERSION. */
    uint32_t present_mask;   /**< #pf_native_spec_ext_mask_t bits. */

    uint32_t report_policy;               /**< NativeReportPolicy. */
    uint32_t report_open_position_at_end; /**< 0/1; KernelRecorded only. */

    uint32_t price_grid;      /**< NativePriceGrid. */
    uint32_t grid_rounding;   /**< NativeGridRounding. */

    uint32_t calculation;                  /**< NativeCalculationTrigger. */
    uint32_t max_recalculations_per_point;  /**< Fill-cascade bound; 0 is legal. */

    uint32_t open_bar_view;   /**< NativeOpenBarView. */

    uint32_t margin_sizing;   /**< NativeLiquidationSizing. */
    uint32_t margin_check;    /**< #pf_native_liquidation_check_t. */
    uint32_t margin_has_maintenance_long;
    uint32_t margin_has_maintenance_short;
    uint32_t margin_has_min_units;
    double   margin_initial_long;   /**< 0 = maintenance-only side: no kernel
                                         opening requirement, host owns
                                         admission; legal only with that
                                         side's maintenance set. */
    double   margin_initial_short;  /**< 0 = maintenance-only; see above. */
    double   margin_maintenance_long;
    double   margin_maintenance_short;
    double   margin_shortfall_multiple; /**< The C++ default is 1.0, not 0: a
                                         *   PRESENT block has every field read,
                                         *   so a zero-filled struct must still
                                         *   write it — including under a sizing
                                         *   policy that never scales. */
    double   margin_min_units;

    const pf_native_subscription_v1* subscriptions; /**< Borrowed for the call. */
    uint32_t subscriptions_n;
    uint32_t reserved0;

    /* ── The additive risk tail (L9). Read only when `present_mask` carries
     * PF_NATIVE_SPEC_EXT_RISK; a caller sending the base layout stops at
     * `reserved0` above. Each limit is opt-in through its own `has_` flag,
     * and a block whose four flags are all 0 is a declared-but-empty block —
     * which is exactly what the C++ `NativeRiskLimits{}` is. ── */
    uint32_t risk_has_max_drawdown;          /**< 0/1. */
    uint32_t risk_max_drawdown_percent;      /**< 0/1: the value is a percent of the
                                              *   running equity peak, out of 100. */
    double   risk_max_drawdown;              /**< Threshold; account currency unless percent. */
    uint32_t risk_has_max_intraday_loss;     /**< 0/1. */
    uint32_t risk_max_intraday_loss_percent; /**< 0/1: percent of the day's opening equity. */
    double   risk_max_intraday_loss;         /**< Threshold; account currency unless percent. */
    uint32_t risk_has_max_consecutive_loss_days; /**< 0/1. */
    uint32_t risk_max_consecutive_loss_days;     /**< Days, when the flag is 1. */
    uint32_t risk_has_max_fills_per_day;     /**< 0/1. */
    uint32_t risk_max_fills_per_day;         /**< Applied fills, when the flag is 1. */
    uint32_t risk_day_basis;                 /**< #pf_native_risk_day_t. */
    uint32_t risk_action;                    /**< #pf_native_risk_action_t. */

    /* ── The additive intrabar / policy tail (N8). Read only when
     * `struct_size` is the current sizeof; a caller sending either earlier
     * layout stops at `risk_action` above and keeps every kernel default.
     * The two blocks below have mask bits of their own; the four margin
     * fields at the end extend the EXISTING PF_NATIVE_SPEC_EXT_MARGIN block
     * and are read only when that bit is set AND this tail is present. ── */
    uint32_t intrabar_kind;          /**< #pf_native_intrabar_kind_t. */
    int32_t  intrabar_samples;       /**< Samples per script bar; LOWER_TF and SYNTHESIZED. */
    uint32_t intrabar_distribution;  /**< #pf_magnifier_distribution_t. */
    uint32_t intrabar_volume_weighted;             /**< 0/1. */
    int32_t  intrabar_volume_weighted_min_samples;
    int32_t  intrabar_volume_weighted_max_samples;
    uint32_t intrabar_sample_eligibility; /**< #pf_native_sample_eligibility_t, LOWER_TF only. */
    int32_t  intrabar_n;             /**< Length of `intrabar_bars`; LOWER_TF only. */
    const char* intrabar_tf;         /**< The finer timeframe; LOWER_TF only, non-NULL. */
    const pf_bar_t* intrabar_bars;   /**< The finer feed; borrowed for the call, copied. */

    uint32_t slot_label_policy;   /**< #pf_native_slot_label_t. FEED_TOLERANT keeps
                                   *   the caller's own labels, and a
                                   *   #PF_NATIVE_INTRABAR_LOWER_TF path then
                                   *   delivers no sub-bar: its bars are not
                                   *   keyed to canonical input slots. */
    uint32_t feed_tolerance;      /**< #pf_native_feed_tolerance_t bits. */
    uint32_t path_order;          /**< #pf_native_path_order_t. */
    uint32_t abort_reporting;     /**< #pf_native_abort_reporting_t. */

    uint32_t margin_equity_basis; /**< #pf_native_margin_equity_basis_t. */
    uint32_t margin_level_base;   /**< #pf_native_margin_level_base_t. */
    const char* margin_liquidation_label;   /**< Ticket of a kernel liquidation; NULL is "". */
    const char* margin_liquidation_comment; /**< Comment of the same; NULL is "". */

    /* ── The additive auxiliary-feed tail. Read only when `present_mask`
     * carries PF_NATIVE_SPEC_EXT_AUXILIARY_FEED; a caller sending an earlier
     * layout stops at `margin_liquidation_comment`, `risk_action` or
     * `reserved0` above. The feed is the
     * run's own symbol at a timeframe strictly finer than the input, routed
     * by time into the series that name it (`NativeAuxiliaryFeed`). ── */
    const char*     auxiliary_tf;   /**< Non-NULL feed timeframe literal. */
    const pf_bar_t* auxiliary_bars; /**< Strictly increasing bars; copied. May be
                                     *   NULL when `auxiliary_n` is 0. */
    int32_t         auxiliary_n;    /**< Length of `auxiliary_bars`. */
    uint32_t        reserved1;      /**< Must be 0. */
    /** Optional, borrowed for the call: `subscriptions_n` entries of
     *  #pf_native_series_source_t, one per row of `subscriptions`. NULL means
     *  every series is built from the input. Meaningful only together with
     *  PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS. */
    const uint32_t* subscription_sources;
} pf_native_run_spec_ext_v1;

/** Byte length of #pf_native_run_spec_ext_v1 as the L13 lane first published
 *  it, before the `risk_*` tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds for
 *  — it is not a literal. The runtime accepts this length as well as the
 *  current `sizeof`, which is what makes the tail additive rather than a
 *  layout break. */
#define PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_run_spec_ext_v1, risk_has_max_drawdown))

/** Byte length of #pf_native_run_spec_ext_v1 with L9's risk tail but without
 *  N8's intrabar / policy tail — the second of its four published layouts. */
#define PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE \
    ((uint32_t)offsetof(pf_native_run_spec_ext_v1, intrabar_kind))

/** Byte length of #pf_native_run_spec_ext_v1 with N8's intrabar / policy tail
 *  but before the `auxiliary_*` tail was appended — the third of its four
 *  published layouts. It is the offset of the first auxiliary field, for the
 *  same reason #PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE is an offset. */
#define PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE \
    ((uint32_t)offsetof(pf_native_run_spec_ext_v1, auxiliary_tf))

/** The C host's strategy logic.
 *
 *  Every entry may be NULL, which is exactly the C++ default: the kernel does
 *  nothing for that hook. `user` is handed back unchanged to every callback.
 *  A callback must never let an exception, a longjmp or any other non-local
 *  exit escape.
 *
 *  There are two classes of entry, and they read their return value
 *  differently. An OBSERVATION callback — everything down to and including
 *  `on_sub_bar` — returns 0 to continue; any other value ends the run Failed
 *  with PF_NATIVE_FAILURE_CALLBACK. An ANSWERING callback — the four margin
 *  and excursion hooks at the end — returns a #pf_native_answer_t selecting
 *  WHOSE answer the kernel uses; every value is in contract, so an answering
 *  hook can never fail the run. That split is deliberate: the answering hooks
 *  are consulted from kernel paths that are not inside the callback guard, so
 *  a failure raised there could not be latched without unwinding through
 *  them. A host that must abort does it from an observation callback.
 *
 *  Commands are legal inside `on_bar_open`, `on_bar`, `on_tick` and
 *  `on_applied`. `on_run_begin`, `on_input`, `on_timeframe_bar` and
 *  `on_margin_call` are observation-only: a command there answers
 *  PF_NATIVE_E_STATE and changes nothing.
 *  #strategy_native_declare_opened_lot_entry_bar_mask_v1 is legal inside
 *  `on_applied` alone.
 *
 *  `on_bar` is also the recalculation hook: with a calculation trigger above
 *  BarClose the kernel calls it again at each fill cursor or modeled point,
 *  which is exactly what the C++ `on_native_recalculate` default does. */
typedef struct pf_native_callbacks_v1 {
    uint32_t struct_size;  /**< sizeof(pf_native_callbacks_v1). */
    uint32_t version;      /**< PF_NATIVE_API_VERSION. */
    void*    user;         /**< Opaque; handed back unchanged. */
    int (*on_run_begin)(void* user);
    int (*on_input)(void* user, const pf_bar_t* bar, int32_t input_index,
                    int32_t completes_script_interval);
    int (*on_bar_open)(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at);
    int (*on_bar)(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at);
    int (*on_tick)(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at);
    int (*on_applied)(void* user, const pf_native_applied_v1* applied,
                      const pf_native_decision_v1* at);
    int (*on_timeframe_bar)(void* user, const pf_bar_t* bar, uint32_t subscription,
                            uint32_t completion, int64_t delivered_at_ms);
    int (*on_margin_call)(void* user, const pf_native_event_v1* margin_call);

    /* ── The additive hook tail. Read only when `struct_size` is the current
     * sizeof; a caller sending the base layout stops at `on_margin_call`
     * above and gets exactly the kernel's own defaults for all six, which is
     * what every host compiled before this tail already had. ── */
    /** EVERY calculation of the run, including the script bar's own close —
     *  `on_native_recalculate`. `reason` is a #pf_native_calc_reason_t and
     *  `cause` is the applied execution of an ORDER_FILL recalculation, valid
     *  only for that call and NULL otherwise. `bar` is the COMPLETE script
     *  bar even mid-path; #strategy_native_partial_bar_v1 is the
     *  lookahead-free bar so far. Commands are legal here.
     *
     *  Installing it REPLACES `on_bar` for every calculation, exactly as
     *  overriding `on_native_recalculate` replaces the C++ default forwarding:
     *  a host that wants both calls `on_bar` itself from here. Leaving it
     *  NULL keeps the established contract, where the kernel forwards every
     *  calculation to `on_bar`. Observation callback: non-zero ends the run. */
    int (*on_recalculate)(void* user, const pf_bar_t* bar, const pf_native_decision_v1* at,
                          uint32_t reason, const pf_native_applied_v1* cause);

    /** One completed lower-timeframe sub-bar of a run that retains a lower
     *  feed — `on_native_sub_bar`. Delivered
     *  after that sub-bar's whole matching path and before the next one's;
     *  never called for a run with no retained lower feed. The decision point
     *  is the sub-bar's last modeled point, so commands and
     *  #strategy_native_execute_current_v1 are legal.
     *  Observation callback: non-zero ends the run. */
    int (*on_sub_bar)(void* user, const pf_bar_t* sub, const pf_native_decision_v1* at);

    /** The two numbers one check point is about to compare —
     *  `resolve_margin_requirement`. ANSWERING callback: return
     *  #PF_NATIVE_ANSWER_DEFAULT to keep the kernel's own, any other value to
     *  use @p out. The kernel keeps the whole mechanism — the level solve,
     *  the check points, its own request, the receipt, `on_margin_call`; this
     *  supplies only the money rule brokers legitimately differ on. */
    int (*on_margin_requirement)(void* user, const pf_native_margin_view_v1* view,
                                 pf_native_margin_decision_v1* out);

    /** Whether this kernel check point is one the host's broker model shares
     *  — `margin_check_allowed`. ANSWERING callback: return
     *  #PF_NATIVE_ANSWER_DEFAULT to admit the point (the kernel's own
     *  answer), any other value to use @p allowed (0 suppresses it). A
     *  suppressed point is not evaluated, re-armed or withdrawn: the margin
     *  state is left exactly as the last admitted point left it. */
    int (*on_margin_check)(void* user, const pf_native_margin_view_v1* at, int32_t* allowed);

    /** The size of a kernel-issued liquidation, before it rests —
     *  `resolve_margin_call_units`. ANSWERING callback: return
     *  #PF_NATIVE_ANSWER_DEFAULT to keep the run spec's sizing policy, any
     *  other value to use @p units, which the kernel clamps into (0, held].
     *  It has the last word on units, including over a forced breach. */
    int (*on_margin_call_units)(void* user, const pf_native_margin_view_v1* view,
                                double* units);

    /** The favorable and adverse magnitudes of one closing lot —
     *  `closed_lot_excursion`. Installing it at all is
     *  `owns_lot_excursions() == true`: the consumer then stops sampling
     *  excursion at matched trigger prices for the WHOLE run and every
     *  closing row takes both magnitudes from here. ANSWERING callback:
     *  return #PF_NATIVE_ANSWER_DEFAULT to answer the kernel's own zero
     *  magnitudes — which, ownership having been declared, is what a declined
     *  lot gets — or any other value to use @p favorable and @p adverse. */
    int (*on_lot_excursion)(void* user, const pf_native_lot_excursion_v1* facts,
                            double* favorable, double* adverse);

    /** How many units a host-sized CLOSE takes — the units half of
     *  `resolve_execution_terms`, and the only half this header exposes.
     *  Consulted for #PF_NATIVE_INTENT_HOST_SIZED candidates and nothing
     *  else; without it a cohort close resolves no quantity and stands
     *  deferred, which is exactly what the C++ default does. ANSWERING
     *  callback: return #PF_NATIVE_ANSWER_DEFAULT to keep that default, any
     *  other value to close @p units of
     *  #pf_native_close_view_v1::scope_exposure_units. */
    int (*on_close_units)(void* user, const pf_native_close_view_v1* view, double* units);
} pf_native_callbacks_v1;

/** Byte length of #pf_native_callbacks_v1 as the L13 lane first published it,
 *  before the six-hook tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds for
 *  — it is not a literal. #strategy_native_host_create_v1 accepts this length
 *  as well as the current `sizeof`, which is what makes the tail additive
 *  rather than a layout break. */
#define PF_NATIVE_CALLBACKS_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_callbacks_v1, on_recalculate))

/** @} */ /* end of pf_native_c_types */

/** @defgroup pf_native_c_api Entry points
 *  @{ */

/** This header's layout version. Mirrors #strategy_stream_api_version. */
PF_API int strategy_native_api_version(void);

/** Allocate a native host that forwards every kernel callback to @p callbacks.
 *
 *  The table is copied; the caller's struct need not outlive the call. The
 *  returned handle is a `pf_strategy_t` the existing runtime symbols accept:
 *  #strategy_configure_native_v1, the whole `strategy_stream_*` family, the
 *  read-only accessors and #report_free all take it unchanged.
 *
 *  @return The handle, or NULL for a NULL/mis-sized table or on allocation
 *  failure. Release it with #strategy_native_host_free — never
 *  #strategy_free, which does not own this allocation. */
PF_API pf_strategy_t strategy_native_host_create_v1(const pf_native_callbacks_v1* callbacks);

/** Release a handle from #strategy_native_host_create_v1. NULL is a no-op;
 *  a handle this API did not create is refused without freeing anything. */
PF_API void strategy_native_host_free(pf_strategy_t s);

/** Run @p n bars as one batch and fill @p out.
 *
 *  The specification must already be Ready (#strategy_configure_native_v1).
 *  @p out may be NULL to skip reporting; otherwise its arrays are
 *  heap-allocated and released by #report_free.
 *  @return PF_NATIVE_OK when the run reached Completed, PF_NATIVE_E_RUN_FAILED
 *  when it did not (read #strategy_native_state_v1 for the code). */
PF_API int strategy_native_run_v1(pf_strategy_t s, const pf_bar_t* bars, int n,
                                  pf_report_t* out);

/** Free the heap arrays inside a report filled by #strategy_native_run_v1.
 *
 *  The unprefixed #report_free is a per-strategy export the transpiler emits,
 *  so it is absent from a runtime a C host links on its own: this is that
 *  host's release path. Idempotent; NULL is a no-op. The `pf_report_t` struct
 *  itself stays caller-owned. */
PF_API void strategy_native_report_free_v1(pf_report_t* report);

/** Submit @p request.
 *
 *  Legal from inside a native callback; the request joins the working book
 *  and is matched by the consumer from the next execution point on. The
 *  handle it answers with is the request's identity for the rest of the run:
 *  replace, cancel and every event carry it.
 *
 *  @param s            The host this run is driving, from #strategy_create.
 *  @param request      Borrowed for the call only; the kernel copies what it
 *                      keeps.
 *  @param incarnation  Optional; receives the accepted request's handle.
 *  @param reject       Optional; receives a RequestRejectReason on rejection.
 *  @return PF_NATIVE_OK when accepted, PF_NATIVE_E_REJECTED when the kernel
 *  rejected it, PF_NATIVE_E_STATE when commands are not legal here.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_submit_v1(pf_strategy_t s, const pf_native_request_v1* request,
                                     uint64_t* incarnation, uint32_t* reject);

/** Replace the live request @p incarnation with @p request.
 *
 *  Amendment, not cancel-and-resubmit: the predecessor leaves the book and
 *  the successor takes its place in one step, and the REPLACED event names
 *  both. A target that is no longer working is PF_NATIVE_E_NOT_WORKING.
 *
 *  @param s           The host this run is driving, from #strategy_create.
 *  @param incarnation The live request to amend, as #strategy_native_submit_v1
 *                     handed it back.
 *  @param request     Borrowed for the call only.
 *  @param successor   Optional; receives the successor's handle.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_REJECTED, PF_NATIVE_E_NOT_WORKING,
 *  PF_NATIVE_E_INVALID_TARGET, or PF_NATIVE_E_STATE.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_replace_v1(pf_strategy_t s, uint64_t incarnation,
                                      const pf_native_request_v1* request,
                                      uint64_t* successor);

/** Cancel one live request.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_NOT_WORKING, PF_NATIVE_E_INVALID_TARGET
 *  or PF_NATIVE_E_STATE. */
PF_API int strategy_native_cancel_v1(pf_strategy_t s, uint64_t incarnation);

/** Cancel every live request, dependants included.
 *  @return The number cancelled (>= 0), or PF_NATIVE_E_STATE. */
PF_API int strategy_native_cancel_all_v1(pf_strategy_t s);

/** Cancel exactly the live requests whose @p field equals @p text.
 *
 *  The C spelling of `NativeStrategyHost::cancel_where`. With
 *  #PF_NATIVE_FIELD_LABEL it is the one call that withdraws every live
 *  request a host issued under one of its own order ids; with
 *  #PF_NATIVE_FIELD_COMMENT it is the established comment predicate. A
 *  dependant of a cancelled owner still leaves the book, but it is counted
 *  only when its own field matched. Text that matches nothing is not a
 *  command.
 *
 *  @param s      The host this run is driving, from #strategy_create.
 *  @param text   Borrowed for the call only; "" matches the requests that
 *                carry no such text. NULL is PF_NATIVE_E_ARGUMENT, not "".
 *  @param field  #pf_native_request_field_t.
 *  @return The number cancelled (>= 0), PF_NATIVE_E_TAG for a field outside
 *  the enumeration, PF_NATIVE_E_ARGUMENT for a NULL @p text, or another
 *  negative status.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_cancel_where_v1(pf_strategy_t s, const char* text,
                                           uint32_t field);

/** Execute one live request at the current execution point.
 *
 *  Legal only inside a callback the kernel has opened an execution point for
 *  (`on_bar`, `on_bar_open`, `on_tick`, `on_applied`, `on_recalculate`);
 *  anywhere else the answer is PF_NATIVE_E_STATE. The request is filled at
 *  this cursor rather than waiting for the consumer's own matching pass.
 *
 *  @param s           The host this run is driving, from #strategy_create.
 *  @param incarnation The live request to execute, as
 *                     #strategy_native_submit_v1 handed it back. A handle
 *                     that is not in the working book is
 *                     PF_NATIVE_E_INVALID_TARGET.
 *  @param price_rule  #pf_native_price_rule_t.
 *  @param refusal     Optional; receives a #pf_native_refusal_t when the
 *                     return is PF_NATIVE_E_REFUSED.
 *  @return A non-negative #pf_native_execute_outcome_t, or a negative status.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_execute_current_v1(pf_strategy_t s, uint64_t incarnation,
                                              uint32_t price_rule, uint32_t* refusal);

/** Read the physical position. Every output is optional.
 *  @return PF_NATIVE_OK, or a negative status. */
PF_API int strategy_native_position_v1(pf_strategy_t s, double* signed_units,
                                       double* average_price, uint64_t* lots);

/** Snapshot the live working book and return its length.
 *
 *  The snapshot is retained on the handle: #strategy_native_working_get_v1
 *  reads from it, so a row already copied out is not invalidated by a later
 *  command. The next call to this function replaces the snapshot.
 *  @return The row count (>= 0), or a negative status. */
PF_API int strategy_native_working_len_v1(pf_strategy_t s);

/** Copy row @p index of the most recent working snapshot into @p out.
 *
 *  @p out is an in/out size prefix: set `out->struct_size` to
 *  `sizeof(pf_native_working_v1)` before the call. Everything else is filled.
 *  A caller built against the base layout sends
 *  #PF_NATIVE_WORKING_V1_BASE_SIZE and is filled exactly that far.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_ARGUMENT for an out-of-range index,
 *  PF_NATIVE_E_STRUCT for a mis-sized row, or another negative status. */
PF_API int strategy_native_working_get_v1(pf_strategy_t s, int index,
                                          pf_native_working_v1* out);

/** Snapshot the open lots marked at @p mark and return their count.
 *
 *  The C spelling of `NativeStrategyHost::native_open_lots(mark)` (R5 gap
 *  lane N18). The snapshot is retained on the handle:
 *  #strategy_native_open_lot_get_v1 reads from it, so a row already copied
 *  out is not invalidated by a later command; the next call to this function
 *  replaces it. Observation only — it moves no fill, no hash and no row — and
 *  legal wherever #strategy_native_position_v1 is. A NaN @p mark keeps every
 *  booking fact and leaves `unrealized_pnl` NaN.
 *  @return The row count (>= 0), or a negative status. */
PF_API int strategy_native_open_lot_count_v1(pf_strategy_t s, double mark);

/** Copy row @p index of the most recent open-lot snapshot into @p out.
 *
 *  @p out is an in/out size prefix: set `out->struct_size` to
 *  `sizeof(pf_native_open_lot_v1)` before the call. Everything else is filled.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_ARGUMENT for an out-of-range index,
 *  PF_NATIVE_E_STRUCT for a mis-sized row, or another negative status. */
PF_API int strategy_native_open_lot_get_v1(pf_strategy_t s, int index,
                                           pf_native_open_lot_v1* out);

/** Copy up to @p cap events with an ordinal strictly greater than
 *  @p after_ordinal into @p out, in the kernel's own recording order.
 *
 *  Ordinals are non-decreasing rather than strictly increasing: an applied
 *  execution and the account observation it produced carry the same one. A
 *  page therefore never ends in the middle of such a group (for @p cap >= 2),
 *  so a poller advances by the last returned `ordinal` without losing or
 *  repeating a row. The history is append-only, so the same @p after_ordinal
 *  always yields the same rows.
 *  @return The number written (>= 0), or a negative status. */
PF_API int strategy_native_events_v1(pf_strategy_t s, uint64_t after_ordinal,
                                     pf_native_event_v1* out, int cap);

/** Read the run's lifecycle and typed failure into @p out.
 *
 *  @p out is an in/out size prefix: set `out->struct_size` to
 *  `sizeof(pf_native_state_v1)` before the call. */
PF_API int strategy_native_state_v1(pf_strategy_t s, pf_native_state_v1* out);

/** Declare this run's higher-timeframe series from inside `on_run_begin` —
 *  `declare_timeframe_subscriptions()`.
 *
 *  The list REPLACES the `subscriptions` staged by
 *  #strategy_configure_native_ext_v1; the kernel registers from the staged
 *  spec after the callback returns, so the run's continuation identity folds
 *  what actually ran. @p n may be 0 (with @p rows NULL), which declares no
 *  series at all.
 *  @return PF_NATIVE_OK when the list was staged; PF_NATIVE_E_STATE anywhere
 *  but inside `on_run_begin` and for a list this run's input timeframe would
 *  refuse — the same validation #strategy_configure_native_ext_v1 applies —
 *  in which case nothing is staged and nothing changes. */
PF_API int strategy_native_declare_subscriptions_v1(pf_strategy_t s,
                                                    const pf_native_subscription_v1* rows,
                                                    int n);

/** The bar so far at the current cursor — `current_partial_bar()`.
 *
 *  Open of the script bar's first modeled point, running high/low, close at
 *  the cursor; volume is the activity actually consumed so far. Valid in the
 *  bar-open, applied, tick, sub-bar and recalculation callbacks.
 *  @return PF_NATIVE_OK when @p out was written, #PF_NATIVE_ABSENT outside a
 *  path walk — including in the bar's own close calculation, where the
 *  callback already holds the complete bar — leaving @p out untouched. */
PF_API int strategy_native_partial_bar_v1(pf_strategy_t s, pf_bar_t* out);

/** How many recalculations the kernel drove, and how many it suppressed
 *  because a point had spent its `max_recalculations_per_point` budget —
 *  `native_recalculation_count()` / `native_recalculations_skipped()`.
 *  Either pointer may be NULL. Observation only. */
PF_API int strategy_native_recalculations_v1(pf_strategy_t s, uint64_t* driven,
                                             uint64_t* skipped);

/** The trail projection of one live request — `trail_state()`.
 *
 *  @p incarnation names a request this run issued.
 *  @return PF_NATIVE_OK when @p out was written, #PF_NATIVE_ABSENT when the
 *  handle is not a live Trail request (unknown, finished, or another
 *  trigger), leaving @p out untouched. */
PF_API int strategy_native_trail_state_v1(pf_strategy_t s, uint64_t incarnation,
                                          pf_native_trail_state_v1* out);

/** The latest completed bucket of a declared subscription —
 *  `native_series_bar()`. @p subscription is the row's index in the
 *  `subscriptions` array #strategy_configure_native_ext_v1 was given (or the
 *  list #strategy_native_declare_subscriptions_v1 installed). Legal inside
 *  every callback, `on_timeframe_bar` included.
 *  @return PF_NATIVE_OK when @p out was written, #PF_NATIVE_ABSENT before the
 *  series' first delivery, for an unknown index, and on every input bar a
 *  #PF_NATIVE_GAPS_CLEAR series publishes nothing on — the empty that stands
 *  for na. */
PF_API int strategy_native_series_bar_v1(pf_strategy_t s, uint32_t subscription,
                                         pf_bar_t* out);

/** The account's marked equity at @p mark — `native_marked_equity()`. */
PF_API int strategy_native_marked_equity_v1(pf_strategy_t s, double mark, double* out);

/** The price at which the marked equity falls below the run's maintenance
 *  requirement for the live position's side — `native_liquidation_price()`.
 *  @return PF_NATIVE_OK when @p out was written, #PF_NATIVE_ABSENT when the
 *  run declares no margin model, the side has no maintenance fraction, the
 *  book is flat, or no finite price solves the breach (a long at full
 *  maintenance); @p out is then written NaN. */
PF_API int strategy_native_liquidation_price_v1(pf_strategy_t s, double* out);

/** The run's generic risk ledger — `native_risk_state()`. Every field is its
 *  zero for a run that declares no risk block. */
PF_API int strategy_native_risk_state_v1(pf_strategy_t s, pf_native_risk_state_v1* out);

/** The run's continuation identity — `native_continuation_hash()`. Two runs
 *  driven the same way that folded the same declarations and the same inputs
 *  answer the same value; a batch and a stream over identical bars booking
 *  identical trades do NOT, because the driving mode is part of a
 *  continuation. It is the C spelling of the hash a stream resumes against,
 *  and the same reason the per-bar broker-state hash is per driving mode
 *  (`pineforge.h`, `strategy_set_broker_state_hash_recording`). */
PF_API int strategy_native_continuation_hash_v1(pf_strategy_t s, uint64_t* out);

/** Open a cohort roster for PF_NATIVE_OWNER_BIND_COHORT. */
PF_API int strategy_native_cohort_open_v1(pf_strategy_t s, uint64_t* cohort);

/** Enrol a live request into a cohort roster. */
PF_API int strategy_native_cohort_add_v1(pf_strategy_t s, uint64_t cohort,
                                         uint64_t incarnation);

/** Remove a request from a cohort roster. */
PF_API int strategy_native_cohort_remove_v1(pf_strategy_t s, uint64_t cohort,
                                            uint64_t incarnation);

/** Configure a native run from the v1 specification plus the extension.
 *
 *  Use this INSTEAD of #strategy_configure_native_v1, not after it: the
 *  kernel configures a host exactly once and refuses (and fails) a second
 *  attempt, so this call takes both halves and applies them together.
 *  @p base is the same #pf_native_run_spec_v1 the other entry point takes.
 *
 *  Refuses without mutation — the handle stays usable — for an
 *  already-configured handle, a mis-sized struct, an unknown enumerator, or a
 *  specification the kernel's own validation rejects.
 *  @return PF_NATIVE_OK, or a negative status. */
PF_API int strategy_configure_native_ext_v1(pf_strategy_t s,
                                            const pf_native_run_spec_v1* base,
                                            const pf_native_run_spec_ext_v1* ext);

/** Append later bars to the run's declared auxiliary feed on a realtime
 *  stream (`NativeStrategyHost::append_auxiliary_bars`).
 *
 *  Legal between stream inputs, after #strategy_stream_begin, on a host whose
 *  specification declared PF_NATIVE_SPEC_EXT_AUXILIARY_FEED. The bars are
 *  copied, join the feed behind every bar it holds, and ride on the next
 *  pushed bar whose period they opened before — the routing a batch of the
 *  same bars applies. @p n of 0 is accepted and appends nothing.
 *
 *  Refused without mutation, the handle staying usable, for bars that are out
 *  of order or not after the feed's last bar, a bar with invalid OHLCV, a bar
 *  that opened inside an input period already accepted, a host that declared
 *  no feed, and a run that is not realtime: PF_NATIVE_E_STATE, the reason
 *  readable with #strategy_get_last_error. A call from inside a callback fails the
 *  run, as every reentrant stream input does.
 *  @return PF_NATIVE_OK, or a negative status. */
PF_API int strategy_native_append_auxiliary_bars_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                    int32_t n);

/** Declare where the fill that opened a lot sits on its entry bar —
 *  `declare_opened_lot_entry_bar_mask()`, the entry-side half of the
 *  excursion capability #pf_native_callbacks_v1::on_lot_excursion owns
 *  (RULING A48).
 *
 *  The host says only WHERE its fill sat. The kernel reads @p entry_bar's own
 *  modeled path — the high first when |high - open| < |open - low|, the low
 *  first otherwise, and where a price is first touched on it — and sets the
 *  two entry-bar masks of every open lot booked under @p entry_incarnation:
 *  the ends of the bar the path had already reached before that lot's own
 *  price. They come back on the lot's closing facts,
 *  #pf_native_lot_excursion_v1::entry_bar_high_masked and
 *  `entry_bar_low_masked`. A fill the path never reaches leaves the lot as
 *  it was, and so does an incarnation that booked no open lot. The masks are
 *  durable lot state, folded into the broker-state hash.
 *
 *  Legal inside `on_applied` alone — the frame in which a host learns that a
 *  fill opened a lot (#pf_native_applied_v1::opened_lot_incarnation) — and
 *  refused with PF_NATIVE_E_STATE everywhere else, changing nothing.
 *
 *  @param entry_incarnation  The request whose fill opened the lot.
 *  @param entry_bar          The whole bar the fill sat on; borrowed.
 *  @param fill_point         #pf_native_opened_lot_fill_point_e.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_ARGUMENT for a NULL @p entry_bar,
 *  PF_NATIVE_E_TAG for a fill point outside the enumeration,
 *  PF_NATIVE_E_STATE outside `on_applied`, or another negative status. */
PF_API int strategy_native_declare_opened_lot_entry_bar_mask_v1(pf_strategy_t s,
                                                                uint64_t entry_incarnation,
                                                                const pf_bar_t* entry_bar,
                                                                uint32_t fill_point);

/** @} */ /* end of pf_native_c_api */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PINEFORGE_NATIVE_C_API_H */
