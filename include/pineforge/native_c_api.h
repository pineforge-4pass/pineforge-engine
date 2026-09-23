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
 * ✓ Read, at every decision point, the script interval and the session day
 *   of its open and of the next input — the facts session-day flags are
 *   derived from
 * ✓ Read the run's lifecycle state and its typed failure
 * ✓ Read a margin call's whole economics — equity, requirement and the
 *   position on either side of it — by its event ordinal
 * ✓ Read a closed row's own identifiers — its entry and exit ticket, its exit
 *   comment and its close cause — with the pineforge.h accessors, which take
 *   any handle this header produces
 * ✓ Arm a WAIT_FOR_APPLIED child after its owner's print, or bound to the
 *   whole book, and size its close with on_close_units
 * ✓ Ask the kernel what a SIZED request would resolve to, before submitting
 * ✓ Extend the run specification with the fields pf_native_run_spec_v1 predates
 * ✓ Declare an auxiliary finer feed — up front, or from `on_run_begin` —
 *   build a series from it, and append its later bars to a realtime stream,
 *   each refusal named by the kernel's own typed answer
 * ✓ Declare, from `on_applied`, where a lot's opening fill sat on its entry
 *   bar, for a host that owns its lots' excursions
 *
 * ✗ Streaming has no new symbols: strategy_stream_begin / _push_bar /
 *   _push_tick / _advance_time / _end / _fill_report take any handle this
 *   header produces, unchanged.
 * ✓ Answer the kernel's policy hooks: a host-sized close's units
 *   (on_close_units), a candidate's settlement price and opening shape
 *   (on_execution_terms), the last gate before a fill (on_precommit), where
 *   an anchored leg is armed (on_anchored_level), and the host's own state
 *   folded into the broker-state hash (on_hash_extension). The per-bar hash
 *   rows need no new symbol: `report_policy` =
 *   PF_NATIVE_REPORT_KERNEL_RECORDED with
 *   strategy_set_broker_state_hash_recording fills
 *   pf_report_t::broker_state_hash, one row per script bar.
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
 *   [C]  on_native_margin_call             pf_native_callbacks_v1::on_margin_call -- the call's whole
 *                                          MarginCallEvent is strategy_native_margin_call_v1, by the
 *                                          ordinal the hook is handed
 *   [C]  resolve_execution_terms           pf_native_callbacks_v1::on_close_units (the units of a host-sized
 *                                          close) and pf_native_callbacks_v1::on_execution_terms (the price,
 *                                          the opening shape and the grid policy)
 *   [C]  validate_execution_precommit      pf_native_callbacks_v1::on_precommit -- the plan, the inspection
 *                                          and the projected account flattened into
 *                                          pf_native_precommit_view_v1, the closed rows' P&L borrowed
 *   [C]  resolve_anchored_level            pf_native_callbacks_v1::on_anchored_level
 *   [C]  resolve_margin_requirement        pf_native_callbacks_v1::on_margin_requirement
 *   [C]  margin_check_allowed              pf_native_callbacks_v1::on_margin_check
 *   [C]  resolve_margin_call_units         pf_native_callbacks_v1::on_margin_call_units
 *   [C]  owns_lot_excursions               pf_native_callbacks_v1::on_lot_excursion -- installing it IS
 *                                          declaring ownership
 *   [C]  closed_lot_excursion              pf_native_callbacks_v1::on_lot_excursion
 *   [C]  current_partial_bar               strategy_native_partial_bar_v1
 *   [C]  native_recalculation_count        strategy_native_recalculations_v1
 *   [C]  native_recalculations_skipped     strategy_native_recalculations_v1
 *   [C]  current_execution_point           pf_native_decision_v1::price / ::quote_kind, on every callback;
 *                                          its script interval and session days are the decision's
 *                                          session tail
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
 *   [C]  declare_timeframe_subscriptions_result  strategy_native_declare_subscriptions_ext_v1 -- the
 *                                          same call, its NativeRunSpecValidation written to `error` /
 *                                          `field` (pf_native_spec_error_e / pf_native_spec_field_e), plus
 *                                          a series source per row
 *   [C]  declare_auxiliary_feed            strategy_native_declare_auxiliary_feed_v1 -- a NULL `tf`
 *                                          withdraws the staged feed
 *   [C]  declare_auxiliary_feed_result      strategy_native_declare_auxiliary_feed_v1, whose `error` /
 *                                          `field` out-parameters are the typed answer
 *   [C]  append_auxiliary_bars             strategy_native_append_auxiliary_bars_v1
 *   [C]  append_auxiliary_bars_result      strategy_native_append_auxiliary_bars_ext_v1 --
 *                                          NativeAuxiliaryAppendError (pf_native_append_error_e) and the
 *                                          bar of the call it stopped on
 *   [C]  configure_native                  strategy_configure_native_v1 / strategy_configure_native_ext_v1 --
 *                                          the two specs' enum-valued words are pf_native_fee_kind_e,
 *                                          pf_native_close_execution_e, pf_native_open_directions_e,
 *                                          pf_native_report_policy_e, pf_native_price_grid_e,
 *                                          pf_native_grid_rounding_e, pf_native_calc_trigger_e,
 *                                          pf_native_open_bar_view_e, pf_native_liquidation_sizing_e
 *                                          and pf_native_event_retention_e words
 *   [C]  configure_native_fx_curve         strategy_configure_native_fx_curve_v1 (pineforge.h)
 *   [C]  native_state                      strategy_native_state_v1
 *   [C]  submit                            strategy_native_submit_v1 -- a WAIT_FOR_APPLIED child's
 *                                          first_match / scope are pf_native_request_v1::arm_first_match /
 *                                          arm_scope
 *   [C]  replace                           strategy_native_replace_v1, or strategy_native_replace_ext_v1
 *                                          for the ReplaceResult::reason the first one drops
 *   [--] submit_market                     a C++ convenience that REFUSES non-market extras instead of dropping
 *                                          them; the same request is strategy_native_submit_v1 with
 *                                          PF_NATIVE_TRIGGER_MARKET and a zero-filled struct
 *   [--] replace_market                    the same convenience for a replace; see submit_market
 *   [C]  cancel                            strategy_native_cancel_v1
 *   [C]  native_working_requests           strategy_native_working_len_v1 / strategy_native_working_get_v1;
 *                                          a Trail's arm_price presence is
 *                                          pf_native_working_v1::trail_has_arm_price, the anchor and
 *                                          owner relation are pf_native_working_v1::anchor .. arm_scope
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
 *   [C]  native_acknowledge_events         strategy_native_acknowledge_events_v1
 *   [C]  native_event_window_start         strategy_native_event_window_v1
 *   [C]  native_decision_floor             pf_native_state_v1::decision_floor_ms
 *   [C]  native_consumed_high_water        pf_native_state_v1::consumed_high_water
 *   [C]  native_continuation_hash          strategy_native_continuation_hash_v1
 *   [--] native_closed_rows_amended        a C host reads the closed rows and never writes one -- the kernel
 *                                          books them -- so it has no amendment to name
 *   [C]  native_sized_units                strategy_native_sized_units_v1 -- a PF_NATIVE_INTENT_SIZED
 *                                          pf_native_request_v1 is the Sized basis; its sizing block is
 *                                          read, nothing is submitted
 *
 * Three asymmetries this list does not reach, recorded here because a C host
 * will look for them.
 *
 * pf_native_working_v1 has two trigger numbers, p1 and p2 -- plus, in its own
 * additive tail, trail_has_arm_price, which tells an absent arm from an arm at
 * 0.0 -- while a trail now has three numbers: offset, arm price and
 * pf_native_request_v1::trail_best_seed. The readback keeps the first two and
 * the presence flag; it does NOT carry the seed. The seed is a SUBMISSION
 * input -- a floor on where the ride starts, consumed once at the arm -- and
 * what a host wants back afterwards is the ride itself, which
 * strategy_native_trail_state_v1 already answers as best_price and
 * current_level. Another readout tail would cost every caller that sends the
 * current sizeof a recompile, for a number the caller wrote.
 * Executed by the trail-seed and arm-presence scenarios of
 * tests/test_native_c_api.c.
 *
 * pf_trade_t carries no exit ticket. That POD is the codegen
 * ABI's, runtime-allocated and iterated with the caller's own sizeof, so a
 * tail costs a PF_ABI_VERSION bump for every existing consumer — and it needs
 * none. strategy_closed_trade_entry_id / _exit_id / _exit_comment /
 * _close_cause (pineforge.h) index exactly the rows of pf_report_t::trades,
 * are implemented in the kernel archive, and take any handle, so a C host
 * reads back the ticket its own margin model declared
 * (pf_native_run_spec_ext_v1::margin_liquidation_label) without a new symbol.
 * Executed by the fx-roll scenario of tests/test_native_c_api.c.
 *
 * pf_native_decision_v1 carries three of NativeDecisionContext's four
 * session-day facts (in_session, opens_session_day, closes_session_day, in
 * the base layout's former tail padding, so a table of every published length
 * is handed them) and not the fourth, closes_session_day_open_ended. That one
 * differs from closes_session_day on a batch's final bar alone, for a host
 * that recomputes a batch whose last input is still forming; a C host's live
 * edge is the strategy_stream_* ingress, whose bars already read the calendar
 * there, and that padding holds exactly the three facts and their presence
 * byte. Executed by the session-day scenario of tests/test_native_c_api.c.
 *
 * BASE-CLASS SEAMS
 * ────────────────
 * The rows at the top of this block census NativeStrategyHost's own surface,
 * so they cannot see a member of its base. The BacktestEngine members a host
 * is documented to call or override from its callbacks are opted in one by
 * one: engine.hpp marks each with a `@host-seam` line, and
 * scripts/check_native_c_api_surface.py proves this list is exactly the
 * marked set, with a C spelling or a reason for each, exactly as it does for
 * those rows. A new protected member a host is meant to reach takes the
 * marker and a row here.
 *
 *   [C]  declare_opened_lot_entry_bar_mask strategy_native_declare_opened_lot_entry_bar_mask_v1 -- legal
 *                                          inside on_applied alone; executed by the entry-bar mask
 *                                          scenario of tests/test_native_c_api.c
 *   [C]  hash_host_extension               pf_native_callbacks_v1::on_hash_extension -- the host folds a
 *                                          64-bit digest of its own state after the kernel's bytes
 *   [--] hash_source_extension             the deprecated spelling of hash_host_extension, kept for C++
 *                                          subclasses written against it; a C host has only the current
 *                                          spelling, on_hash_extension
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
 *    the length its caller sent. A struct the runtime PRESENTS to a callback
 *    (pf_native_decision_v1) is presented at the layout the caller's
 *    callback table was published with, and its `struct_size` says so.
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
 * layouts below are append-only and the signatures never change. A field is
 * appended one of two ways. A deliberately additive tail (HARDENING RULES
 * above) keeps the version constant: the struct publishes every earlier
 * layout's length as a `*_SIZE` constant and the runtime accepts each, so a
 * caller compiled against an earlier layout keeps working unchanged, and a
 * readout is written only as far as the length that caller sent. Any other
 * layout change is a new revision that raises the version constant; there
 * the size prefix keeps an old caller refused rather than silently misread.
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
 *  returns non-zero. The historical spelling of
 *  #PF_NATIVE_FAILURE_CALLBACK_EXCEPTION, kept for the callers written
 *  against it; pinned by a static_assert. */
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
    PF_NATIVE_INTENT_HOST_SIZED = 4, /**< A close sized by on_close_units: the
                                      *   cohort close (#PF_NATIVE_OWNER_BIND_COHORT)
                                      *   or a WAIT_FOR_APPLIED child that carries
                                      *   the arm tail (#pf_native_arm_scope_t;
                                      *   the kernel admits it under BOOK only).
                                      *   Refused PF_NATIVE_E_UNSUPPORTED under
                                      *   any other owner. */
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
    PF_NATIVE_TRIGGER_TRAIL      = 4  /**< p1 = offset, p2 = arm price when `trail_has_arm_price`;
                                       *   `trail_best_seed` when `trail_has_best_seed`. */
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

/** Whether an armed WAIT_FOR_APPLIED child may trade on its owner's own fill
 *  print — `native_order::NativeArmFirstMatch`. AT_ARM_PRINT (the default) is
 *  the established book: the armed child is a candidate at that very cursor,
 *  so a level the print already satisfies matches there. AFTER_ARM_PRINT
 *  gives it the birth rule of a request submitted from the owner's own
 *  `on_applied`: on the point that armed it, it sees only the path after the
 *  print. It governs a priced trigger's level test; a market trigger has no
 *  level. Both are broker models. */
typedef enum pf_native_arm_first_match_e {
    PF_NATIVE_ARM_FIRST_MATCH_AT_ARM_PRINT    = 0,
    PF_NATIVE_ARM_FIRST_MATCH_AFTER_ARM_PRINT = 1
} pf_native_arm_first_match_t;

/** What an armed CLOSING child (REDUCE, FLATTEN, a host-sized close) closes —
 *  `native_order::NativeArmScope`. OWNER_LOT (the default) is the lot its
 *  owner's fill opened, and nothing a later add brings. BOOK binds it, at the
 *  arm, to the whole position that fill left, so a protective leg placed with
 *  its entry covers later adds. A host-sized close may wait for its owner only
 *  under BOOK (its units are #pf_native_callbacks_v1::on_close_units'); a
 *  waiting transaction closes nothing and must keep OWNER_LOT. */
typedef enum pf_native_arm_scope_e {
    PF_NATIVE_ARM_SCOPE_OWNER_LOT = 0,
    PF_NATIVE_ARM_SCOPE_BOOK      = 1
} pf_native_arm_scope_t;

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

/** What one execution costs — `NativeFeeKind`, the `fee_kind` word of
 *  #pf_native_run_spec_v1, charged at its `fee_value`. */
typedef enum pf_native_fee_kind_e {
    PF_NATIVE_FEE_PERCENT            = 0, /**< `fee_value` percent of |units| × price ×
                                           *   point value × account FX (the default). */
    PF_NATIVE_FEE_CASH_PER_UNIT      = 1, /**< `fee_value` account currency per unit. */
    PF_NATIVE_FEE_CASH_PER_EXECUTION = 2  /**< `fee_value` account currency per execution. */
} pf_native_fee_kind_t;

/** When a request born at a script calculation may first match —
 *  `NativeCloseExecution`, the `close_execution` word of
 *  #pf_native_run_spec_v1. */
typedef enum pf_native_close_execution_e {
    PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT = 0, /**< A later eligible point — the
                                                        *   next modeled opening, an
                                                        *   observed print or a carried
                                                        *   open — never the bar's
                                                        *   presented prices (the
                                                        *   default). */
    PF_NATIVE_CLOSE_EXECUTION_AFTER_CALCULATION   = 1  /**< Also a modeled close point
                                                        *   right after that
                                                        *   calculation. */
} pf_native_close_execution_t;

/** Which opening directions the run admits — `NativeOpenDirections`, the
 *  `allowed_open_directions` word of #pf_native_run_spec_v1; BOTH is
 *  LONG | SHORT. A refused opening is a match rejection of the whole
 *  transaction, and a pure reduction stays legal under NONE. Zero is NOT the
 *  kernel's default here, unlike every other word these enumerations type: a
 *  zero-filled spec admits no opening at all. */
typedef enum pf_native_open_directions_e {
    PF_NATIVE_OPEN_DIRECTIONS_NONE  = 0,
    PF_NATIVE_OPEN_DIRECTIONS_LONG  = 1,
    PF_NATIVE_OPEN_DIRECTIONS_SHORT = 2,
    PF_NATIVE_OPEN_DIRECTIONS_BOTH  = 3  /**< The kernel's default. */
} pf_native_open_directions_t;

/** Who records the per-script-bar report series — `NativeReportPolicy`, the
 *  `report_policy` word of #pf_native_run_spec_ext_v1. The kernel's third
 *  policy, `KernelRecordedAtHostMarks`, has no C value: under it the host
 *  names each report point itself, from inside its own callbacks, and the
 *  callback table carries no call that marks one — a C host could only
 *  declare a series nobody records. Its integer, 2, is PF_NATIVE_E_TAG like
 *  any other word outside this enumeration. */
typedef enum pf_native_report_policy_e {
    PF_NATIVE_REPORT_HOST_RECORDED   = 0, /**< The host's; the kernel appends no point
                                           *   (the default). */
    PF_NATIVE_REPORT_KERNEL_RECORDED = 1  /**< One equity point per script calculation;
                                           *   `report_open_position_at_end` applies. */
} pf_native_report_policy_t;

/** A generic instrument price grid — `NativePriceGrid`, the `price_grid`
 *  word of #pf_native_run_spec_ext_v1. Both quantizing values need
 *  `price_tick > 0`. */
typedef enum pf_native_price_grid_e {
    PF_NATIVE_PRICE_GRID_NONE                        = 0, /**< Every price booked as the
                                                           *   path presents it (the
                                                           *   default). */
    PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS              = 1, /**< Each fill booked on the
                                                           *   tick ladder. */
    PF_NATIVE_PRICE_GRID_QUANTIZE_FILLS_AND_TRIGGERS = 2  /**< And each resting trigger
                                                           *   tested against the
                                                           *   quantized path. */
} pf_native_price_grid_t;

/** How a quantizing grid rounds — `NativeGridRounding`, the `grid_rounding`
 *  word of #pf_native_run_spec_ext_v1, read beside `price_grid`. */
typedef enum pf_native_grid_rounding_e {
    PF_NATIVE_GRID_ROUNDING_HALF_UP     = 0, /**< The nearest tick, ties away from zero
                                              *   (the default). */
    PF_NATIVE_GRID_ROUNDING_DIRECTIONAL = 1  /**< Toward the region the order needs: a
                                              *   buy limit down and a sell limit up, a
                                              *   stop the other way, a market fill to
                                              *   its adverse side. */
} pf_native_grid_rounding_t;

/** When the kernel asks the host to calculate — `NativeCalculationTrigger`,
 *  the `calculation` word of #pf_native_run_spec_ext_v1. Each value is a
 *  strict superset of the one before it. */
typedef enum pf_native_calc_trigger_e {
    PF_NATIVE_CALC_TRIGGER_BAR_CLOSE           = 0, /**< Once per script bar, at its close
                                                     *   (the default). */
    PF_NATIVE_CALC_TRIGGER_BAR_CLOSE_AND_FILLS = 1, /**< And at each applied execution's
                                                     *   cursor, bounded by
                                                     *   `max_recalculations_per_point`. */
    PF_NATIVE_CALC_TRIGGER_EVERY_MODELED_POINT = 2  /**< And at every modeled point and
                                                     *   every observed print. */
} pf_native_calc_trigger_t;

/** What #pf_native_callbacks_v1::on_bar_open is handed — `NativeOpenBarView`,
 *  the `open_bar_view` word of #pf_native_run_spec_ext_v1. Neither value
 *  changes a match, a fill or any other callback. */
typedef enum pf_native_open_bar_view_e {
    PF_NATIVE_OPEN_BAR_VIEW_COMPLETE  = 0, /**< The whole script bar (the default). */
    PF_NATIVE_OPEN_BAR_VIEW_OPEN_ONLY = 1  /**< Its lookahead masked: H = L = C = open,
                                            *   volume 0. */
} pf_native_open_bar_view_t;

/** Which units a kernel-issued liquidation reduces —
 *  `NativeLiquidationSizing`, the `margin_sizing` word of
 *  #pf_native_run_spec_ext_v1. Clamped to the position held;
 *  #pf_native_callbacks_v1::on_margin_call_units has the last word. */
typedef enum pf_native_liquidation_sizing_e {
    PF_NATIVE_LIQUIDATION_SIZING_RESTORE_MINIMUM    = 0, /**< The fewest units that restore
                                                          *   the requirement at the sizing
                                                          *   mark (the default). */
    PF_NATIVE_LIQUIDATION_SIZING_SHORTFALL_MULTIPLE = 1, /**< That restore times
                                                          *   `margin_shortfall_multiple`. */
    PF_NATIVE_LIQUIDATION_SIZING_FLATTEN            = 2  /**< The whole position. */
} pf_native_liquidation_sizing_t;

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
    PF_NATIVE_SPEC_EXT_AUXILIARY_FEED = 1u << 9,
    /** What the run keeps of its event record (`event_retention`). Only a
     *  caller whose pf_native_run_spec_ext_v1 carries the retention tail may
     *  set this bit; a caller sending any earlier layout is refused with
     *  PF_NATIVE_E_STRUCT. Without it the run keeps
     *  #PF_NATIVE_EVENT_RETENTION_FULL. */
    PF_NATIVE_SPEC_EXT_EVENT_RETENTION = 1u << 10
} pf_native_spec_ext_mask_t;

/** What a run keeps of its event record — `NativeRunSpec::event_retention`,
 *  the `event_retention` word of #pf_native_run_spec_ext_v1, read under
 *  #PF_NATIVE_SPEC_EXT_EVENT_RETENTION.
 *
 *  WINDOW keeps the command journal only until the host has read it: a host
 *  that polls #strategy_native_events_v1 acknowledges what it has read
 *  (#strategy_native_acknowledge_events_v1) and the kernel drops the
 *  acknowledged events at the next script-bar boundary, while a host that
 *  never acknowledges is served by its callbacks alone and its window closes
 *  at every script-bar end. No driver point and no account row is kept.
 *  COMMANDS keeps the whole command journal and every account row, and no
 *  driver point. FULL keeps everything, O(run) in memory.
 *
 *  A C host that does not set the bit -- every caller of
 *  #strategy_configure_native_v1, and every caller of an earlier
 *  pf_native_run_spec_ext_v1 layout -- runs under FULL, the record its
 *  layout was published with, so reading a whole run's events after it has
 *  ended keeps working unchanged. The C++ default is WINDOW. */
typedef enum pf_native_event_retention_e {
    PF_NATIVE_EVENT_RETENTION_WINDOW   = 0, /**< The unread journal only. */
    PF_NATIVE_EVENT_RETENTION_FULL     = 1, /**< Journal, driver points, account rows. */
    PF_NATIVE_EVENT_RETENTION_COMMANDS = 2  /**< Journal and account rows. */
} pf_native_event_retention_t;

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

/* ── The readout words ─────────────────────────────────────────────
 * Every enumeration below names a word the runtime WRITES for a C host: a
 * field of a struct it fills or presents, an event's reason, a callback
 * argument or an out-parameter. The fields stay the integer words they were
 * published as, so no layout moves; each value is written by an exhaustive
 * translation of its kernel enumeration, and a kernel value no enumeration
 * here names cannot build (src/native_c_host.cpp). */

/** Where a point's price came from — `NativePriceProvenance`: the
 *  `provenance` word of #pf_native_decision_v1 and #pf_native_event_v1 and
 *  #pf_native_margin_view_v1::cursor_provenance. */
typedef enum pf_native_price_provenance_e {
    PF_NATIVE_PROVENANCE_CONFIRMED               = 0, /**< A confirmed input bar's own label,
                                                       *   or a point on its modeled path. */
    PF_NATIVE_PROVENANCE_OBSERVED_PRINT          = 1, /**< A realtime print. */
    PF_NATIVE_PROVENANCE_MODELED_OHLC_OPEN       = 2, /**< A modeled path's opening point. */
    PF_NATIVE_PROVENANCE_MODELED_OHLC_CLOSE      = 3, /**< A modeled path's closing point. */
    PF_NATIVE_PROVENANCE_CARRIED_OPEN            = 4, /**< A quiet tradable interval's carried
                                                       *   last price. */
    PF_NATIVE_PROVENANCE_AFTER_CALCULATION_CLOSE = 5, /**< The close point right after a
                                                       *   calculation. */
    PF_NATIVE_PROVENANCE_PARTIAL_FINALIZED       = 6, /**< A partially finalized observed slot. */
    PF_NATIVE_PROVENANCE_CALCULATION             = 7, /**< The script calculation point itself. */
    PF_NATIVE_PROVENANCE_CURRENT_EXECUTION       = 8  /**< A synchronous
                                                       *   #strategy_native_execute_current_v1. */
} pf_native_price_provenance_t;

/** Which leg of a modeled OHLC walk a point sits on — `NativePathPhase`: the
 *  `path_phase` word of #pf_native_decision_v1 and #pf_native_event_v1 and
 *  #pf_native_margin_view_v1::cursor_path_phase. NONE is a discrete point (a
 *  calculation, an observed print); the other four are the waypoints, in the
 *  order the run's path order resolves. */
typedef enum pf_native_path_phase_e {
    PF_NATIVE_PATH_PHASE_NONE  = 0,
    PF_NATIVE_PATH_PHASE_OPEN  = 1,
    PF_NATIVE_PATH_PHASE_HIGH  = 2,
    PF_NATIVE_PATH_PHASE_LOW   = 3,
    PF_NATIVE_PATH_PHASE_CLOSE = 4
} pf_native_path_phase_t;

/** How a script interval or a higher-timeframe bucket was closed —
 *  `NativeCompletionKind`: #pf_native_decision_v1::completion and the
 *  `completion` argument of #pf_native_callbacks_v1::on_timeframe_bar. */
typedef enum pf_native_completion_kind_e {
    PF_NATIVE_COMPLETION_KIND_CONFIRMED         = 0, /**< Its own last contributing bar. */
    PF_NATIVE_COMPLETION_KIND_LAZY_COMPLETE     = 1, /**< The NEXT interval's first input: a
                                                      *   session-clipped bar, or a hole over
                                                      *   the last slot. */
    PF_NATIVE_COMPLETION_KIND_SESSION_SHORTENED = 2, /**< The session close clipped it. */
    PF_NATIVE_COMPLETION_KIND_PARTIAL_FINALIZED = 3  /**< A stream end finalized a forming
                                                      *   observed slot. */
} pf_native_completion_kind_t;

/** Which quote #pf_native_decision_v1::price is — `NativeCurrentQuoteKind`:
 *  the callback's own market decision point, or the execution it is anchored
 *  to (inside `on_applied`). */
typedef enum pf_native_quote_kind_e {
    PF_NATIVE_QUOTE_MARKET_DECISION  = 0,
    PF_NATIVE_QUOTE_EXECUTION_ANCHOR = 1
} pf_native_quote_kind_t;

/** Why an applied execution finished its request —
 *  `native_order::AppliedTerminalReason`: #pf_native_applied_v1::terminal_reason
 *  and the `reason` of a terminal #PF_NATIVE_EVENT_APPLIED row. */
typedef enum pf_native_terminal_reason_e {
    PF_NATIVE_TERMINAL_WORKING_UNITS_SATISFIED = 0,
    PF_NATIVE_TERMINAL_FLATTENED               = 1,
    PF_NATIVE_TERMINAL_TARGET_EXHAUSTED        = 2
} pf_native_terminal_reason_t;

/** Why the kernel refused a request — `native_order::RequestRejectReason`:
 *  the `reject` out-parameter of #strategy_native_submit_v1 and
 *  #strategy_native_replace_ext_v1, and the `reason` of a
 *  #PF_NATIVE_EVENT_REJECTED or #PF_NATIVE_EVENT_REPLACE_REJECTED row. */
typedef enum pf_native_reject_reason_e {
    PF_NATIVE_REJECT_INVALID_QUANTITY       = 0,
    PF_NATIVE_REJECT_OFF_GRID               = 1,
    PF_NATIVE_REJECT_INVALID_TRIGGER        = 2,
    PF_NATIVE_REJECT_INVALID_CAPACITY       = 3,
    PF_NATIVE_REJECT_INVALID_OWNER          = 4,
    PF_NATIVE_REJECT_INVALID_QUANTITY_BASIS = 5,
    PF_NATIVE_REJECT_INVALID_GROUP          = 6,
    PF_NATIVE_REJECT_PLACEMENT_ADMISSION    = 7  /**< A SIZED request resolved at acceptance
                                                  *   that the run's opening admission
                                                  *   refuses at the sizing price. */
} pf_native_reject_reason_t;

/** Why a live request left the book unfilled — `native_order::CancelReason`:
 *  the `reason` of a #PF_NATIVE_EVENT_CANCELLED row. */
typedef enum pf_native_cancel_reason_e {
    PF_NATIVE_CANCEL_USER                 = 0, /**< The host's own cancel. */
    PF_NATIVE_CANCEL_GROUP                = 1, /**< A group sibling's fill withdrew it. */
    PF_NATIVE_CANCEL_OWNER_GONE           = 2, /**< Its owner left the book first. */
    PF_NATIVE_CANCEL_UNSUPPORTED_RELATION = 3,
    PF_NATIVE_CANCEL_SUPERSEDED           = 4  /**< A kernel-originated request the kernel
                                                *   itself withdrew: its level or units
                                                *   moved, or the breach is over. */
} pf_native_cancel_reason_t;

/** Why a matching candidate was refused — `native_order::MatchRejectReason`:
 *  the `reason` of a #PF_NATIVE_EVENT_MATCH_REJECTED row. */
typedef enum pf_native_match_reject_e {
    PF_NATIVE_MATCH_REJECT_NONPOSITIVE_PRICE     = 0,
    PF_NATIVE_MATCH_REJECT_OPENING_DIRECTION     = 1, /**< `allowed_open_directions`. */
    PF_NATIVE_MATCH_REJECT_MAX_ABS_UNITS         = 2,
    PF_NATIVE_MATCH_REJECT_MAX_OPEN_LOTS         = 3,
    PF_NATIVE_MATCH_REJECT_INITIAL_MARGIN        = 4,
    PF_NATIVE_MATCH_REJECT_TERMS_UNRESOLVED      = 5,
    PF_NATIVE_MATCH_REJECT_INVALID_TERMS         = 6,
    PF_NATIVE_MATCH_REJECT_NO_OPPOSITE_EXPOSURE  = 7,
    PF_NATIVE_MATCH_REJECT_HOST_PRECOMMIT        = 8,
    PF_NATIVE_MATCH_REJECT_RISK_LIMIT            = 9  /**< An opening refused while a risk
                                                       *   limit blocks. */
} pf_native_match_reject_t;

/** Which trigger a price reached — `native_order::ActivationKind`: the
 *  `reason` of a #PF_NATIVE_EVENT_ACTIVATED row. */
typedef enum pf_native_activation_e {
    PF_NATIVE_ACTIVATION_STOP          = 0,
    PF_NATIVE_ACTIVATION_STOP_LIMIT    = 1,
    PF_NATIVE_ACTIVATION_TRAIL_ARM     = 2,
    PF_NATIVE_ACTIVATION_TRAIL_TRIGGER = 3
} pf_native_activation_t;

/** Who issued a live request — `native_order::RequestOrigin`:
 *  #pf_native_working_v1::origin. */
typedef enum pf_native_origin_e {
    PF_NATIVE_ORIGIN_HOST               = 0,
    PF_NATIVE_ORIGIN_KERNEL_LIQUIDATION = 1, /**< The run's margin model. */
    PF_NATIVE_ORIGIN_KERNEL_RISK        = 2  /**< The run's risk limits. */
} pf_native_origin_t;

/** A run's durable first failure — `NativeFailureCode`:
 *  #pf_native_state_v1::failure_code. #PF_NATIVE_FAILURE_CALLBACK is the
 *  historical spelling of #PF_NATIVE_FAILURE_CALLBACK_EXCEPTION. */
typedef enum pf_native_failure_code_e {
    PF_NATIVE_FAILURE_NONE                  = 0,
    PF_NATIVE_FAILURE_INVALID_SPECIFICATION = 1,  /**< A spec refused at configure. */
    PF_NATIVE_FAILURE_CONTRACT              = 2,  /**< A lifecycle misuse. */
    PF_NATIVE_FAILURE_PREFLIGHT             = 3,  /**< A bar array the driver refused. */
    PF_NATIVE_FAILURE_UNSUPPORTED_SOURCE    = 4,
    PF_NATIVE_FAILURE_CALLBACK_EXCEPTION    = 5,  /**< A callback returned non-zero. */
    PF_NATIVE_FAILURE_SETTLEMENT_FAILURE    = 6,
    PF_NATIVE_FAILURE_ALLOCATION            = 7,
    PF_NATIVE_FAILURE_COUNTER_EXHAUSTED     = 8,
    PF_NATIVE_FAILURE_ABORTED               = 9,  /**< A cooperative abort. */
    PF_NATIVE_FAILURE_PROJECTION_MISMATCH   = 10,
    PF_NATIVE_FAILURE_CALENDAR              = 11,
    PF_NATIVE_FAILURE_UNEXPECTED            = 12
} pf_native_failure_code_t;

/** Which operation was in flight when the failure latched —
 *  `NativeFailureOperation`: #pf_native_state_v1::failure_operation. */
typedef enum pf_native_failure_operation_e {
    PF_NATIVE_OPERATION_NONE       = 0,
    PF_NATIVE_OPERATION_CONFIGURE  = 1,
    PF_NATIVE_OPERATION_BEGIN      = 2,
    PF_NATIVE_OPERATION_COMMAND    = 3,
    PF_NATIVE_OPERATION_INPUT      = 4,
    PF_NATIVE_OPERATION_CALLBACK   = 5,
    PF_NATIVE_OPERATION_SETTLEMENT = 6,
    PF_NATIVE_OPERATION_MUTATION   = 7,
    PF_NATIVE_OPERATION_STREAM     = 8
} pf_native_failure_operation_t;

/** Which driving a Running run is in — `NativeRunPhase`:
 *  #pf_native_state_v1::phase. */
typedef enum pf_native_run_phase_e {
    PF_NATIVE_PHASE_BATCH    = 0,
    PF_NATIVE_PHASE_WARMUP   = 1, /**< A stream's warmup replay. */
    PF_NATIVE_PHASE_REALTIME = 2  /**< A stream's realtime leg. */
} pf_native_run_phase_t;

/** How a Completed run ended — `NativeCompletion`:
 *  #pf_native_state_v1::completion. */
typedef enum pf_native_completion_e {
    PF_NATIVE_COMPLETION_BATCH_COMPLETE = 0,
    PF_NATIVE_COMPLETION_STREAM_ENDED   = 1
} pf_native_completion_t;

/* ── The typed refusal words of a declaration and an append ──────── */

/** Why a declaration was refused — `NativeRunSpecError`, the kernel's own
 *  validation word: the `error` out-parameter of
 *  #strategy_native_declare_subscriptions_ext_v1 and
 *  #strategy_native_declare_auxiliary_feed_v1. Read beside the
 *  #pf_native_spec_field_t the same call writes. Every value the run spec's
 *  validation can answer is named, the ones a begin-time declaration cannot
 *  reach included, so one enumeration reads every setup refusal. */
typedef enum pf_native_spec_error_e {
    PF_NATIVE_SPEC_ERROR_NONE                                   = 0, /**< Applied. */
    PF_NATIVE_SPEC_ERROR_EMPTY_REQUIRED_STRING                  = 1,
    PF_NATIVE_SPEC_ERROR_EMBEDDED_NUL                           = 2,
    PF_NATIVE_SPEC_ERROR_INVALID_UTF8                           = 3,
    PF_NATIVE_SPEC_ERROR_ZERO_RUN_NUMBER                        = 4,
    PF_NATIVE_SPEC_ERROR_INVALID_TIMEFRAME                      = 5,
    PF_NATIVE_SPEC_ERROR_INCOMPATIBLE_TIMEFRAMES                = 6,
    PF_NATIVE_SPEC_ERROR_UNRESOLVED_TIMEZONE                    = 7,
    PF_NATIVE_SPEC_ERROR_INVALID_SESSION                        = 8,
    PF_NATIVE_SPEC_ERROR_NOT_FINITE_POSITIVE                    = 9,
    PF_NATIVE_SPEC_ERROR_SLIPPAGE_OUT_OF_RANGE                  = 10,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_FEE_KIND                       = 11,
    PF_NATIVE_SPEC_ERROR_NOT_FINITE_NONNEGATIVE                 = 12,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_CLOSE_EXECUTION                = 13,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_ABORT_REPORTING                = 14,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_DIRECTIONS                = 15,
    PF_NATIVE_SPEC_ERROR_ZERO_LOT_LIMIT                         = 16,
    PF_NATIVE_SPEC_ERROR_ALLOCATION_FAILURE                     = 17, /**< Staging threw. */
    PF_NATIVE_SPEC_ERROR_CALENDAR_FAILURE                       = 18, /**< No calendar resolved it. */
    PF_NATIVE_SPEC_ERROR_INVALID_INTRABAR_PATH                  = 19,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_INTRABAR_SAMPLE_ELIGIBILITY    = 20,
    PF_NATIVE_SPEC_ERROR_INVALID_UNDETECTED_TIMEFRAME           = 21,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_SLOT_LABEL_POLICY              = 22,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_FEED_TOLERANCE                 = 23, /**< `feed_tolerance`: the
                                                                       *   kernel's
                                                                       *   `legacy_tolerance`. */
    PF_NATIVE_SPEC_ERROR_UNKNOWN_PATH_ORDER                     = 24,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_REPORT_POLICY                  = 25,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_PRICE_GRID                     = 26,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_GRID_ROUNDING                  = 27,
    PF_NATIVE_SPEC_ERROR_GRID_REQUIRES_PRICE_TICK               = 28,
    PF_NATIVE_SPEC_ERROR_INVALID_SUBSCRIPTION_TIMEFRAME         = 29,
    PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_INPUT          = 30,
    PF_NATIVE_SPEC_ERROR_DUPLICATE_SUBSCRIPTION_TIMEFRAME       = 31,
    PF_NATIVE_SPEC_ERROR_UNORDERED_SUBSCRIPTION_BARS            = 32,
    PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_TIMEFRAME         = 33,
    PF_NATIVE_SPEC_ERROR_MARGIN_MODEL_CONFLICT                  = 34,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_SIZING             = 35,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_CHECK              = 36,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_MARGIN_EQUITY_BASIS            = 37,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_LIQUIDATION_LEVEL_BASE         = 38,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_CALCULATION_TRIGGER            = 39,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_OPEN_BAR_VIEW                  = 40,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_DAY                       = 41,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_RISK_ACTION                    = 42,
    PF_NATIVE_SPEC_ERROR_ZERO_RISK_LIMIT                        = 43,
    PF_NATIVE_SPEC_ERROR_MARGIN_SIDE_UNDECLARED                 = 44,
    PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_TIMEFRAME       = 45,
    PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_NOT_FINER_THAN_INPUT    = 46,
    PF_NATIVE_SPEC_ERROR_UNORDERED_AUXILIARY_FEED_BARS          = 47,
    PF_NATIVE_SPEC_ERROR_INVALID_AUXILIARY_FEED_BAR             = 48,
    PF_NATIVE_SPEC_ERROR_AUXILIARY_FEED_WITHOUT_TIMEFRAME       = 49,
    PF_NATIVE_SPEC_ERROR_UNKNOWN_SERIES_SOURCE                  = 50,
    PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_WITHOUT_AUXILIARY_FEED    = 51,
    PF_NATIVE_SPEC_ERROR_SUBSCRIPTION_FINER_THAN_AUXILIARY_FEED = 52,
    PF_NATIVE_SPEC_ERROR_WRONG_PHASE                            = 53, /**< The CALL was refused
                                                                       *   before any field was
                                                                       *   judged: not in the
                                                                       *   phase it is legal
                                                                       *   in. Read with
                                                                       *   FIELD_NONE. */
    PF_NATIVE_SPEC_ERROR_UNKNOWN_EVENT_RETENTION                = 54  /**< An `event_retention`
                                                                       *   word outside
                                                                       *   #pf_native_event_retention_t. */
} pf_native_spec_error_t;

/** Where a declaration was refused — `NativeRunSpecField`, the first field
 *  the kernel's validation found: the `field` out-parameter of
 *  #strategy_native_declare_subscriptions_ext_v1 and
 *  #strategy_native_declare_auxiliary_feed_v1. */
typedef enum pf_native_spec_field_e {
    PF_NATIVE_SPEC_FIELD_NONE                        = 0, /**< Not about one field. */
    PF_NATIVE_SPEC_FIELD_SESSION_KEY                 = 1,
    PF_NATIVE_SPEC_FIELD_RUN_NUMBER                  = 2,
    PF_NATIVE_SPEC_FIELD_INPUT_TIMEFRAME             = 3,
    PF_NATIVE_SPEC_FIELD_SCRIPT_TIMEFRAME            = 4,
    PF_NATIVE_SPEC_FIELD_TICKER                      = 5,
    PF_NATIVE_SPEC_FIELD_TICKER_ID                   = 6,
    PF_NATIVE_SPEC_FIELD_TYPE                        = 7,
    PF_NATIVE_SPEC_FIELD_CURRENCY                    = 8,
    PF_NATIVE_SPEC_FIELD_BASE_CURRENCY               = 9,
    PF_NATIVE_SPEC_FIELD_DESCRIPTION                 = 10,
    PF_NATIVE_SPEC_FIELD_VOLUME_TYPE                 = 11,
    PF_NATIVE_SPEC_FIELD_TIMEZONE                    = 12,
    PF_NATIVE_SPEC_FIELD_SESSION                     = 13,
    PF_NATIVE_SPEC_FIELD_CHART_TIMEZONE              = 14,
    PF_NATIVE_SPEC_FIELD_INITIAL_CAPITAL             = 15,
    PF_NATIVE_SPEC_FIELD_POINT_VALUE                 = 16,
    PF_NATIVE_SPEC_FIELD_ACCOUNT_FX                  = 17,
    PF_NATIVE_SPEC_FIELD_PRICE_TICK                  = 18,
    PF_NATIVE_SPEC_FIELD_SLIPPAGE_TICKS              = 19,
    PF_NATIVE_SPEC_FIELD_FEE_KIND                    = 20,
    PF_NATIVE_SPEC_FIELD_FEE_VALUE                   = 21,
    PF_NATIVE_SPEC_FIELD_QUANTITY_GRID               = 22,
    PF_NATIVE_SPEC_FIELD_CLOSE_EXECUTION             = 23,
    PF_NATIVE_SPEC_FIELD_ABORT_REPORTING             = 24,
    PF_NATIVE_SPEC_FIELD_MAX_ABS_UNITS               = 25,
    PF_NATIVE_SPEC_FIELD_MAX_OPEN_LOTS               = 26,
    PF_NATIVE_SPEC_FIELD_ALLOWED_OPEN_DIRECTIONS     = 27,
    PF_NATIVE_SPEC_FIELD_INITIAL_MARGIN_FRACTION     = 28,
    PF_NATIVE_SPEC_FIELD_INTRABAR_TIMEFRAME          = 29,
    PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLES            = 30,
    PF_NATIVE_SPEC_FIELD_INTRABAR_DISTRIBUTION       = 31,
    PF_NATIVE_SPEC_FIELD_INTRABAR_VOLUME_SAMPLES     = 32,
    PF_NATIVE_SPEC_FIELD_INTRABAR_SAMPLE_ELIGIBILITY = 33,
    PF_NATIVE_SPEC_FIELD_TIMEFRAME_UNDETECTED        = 34,
    PF_NATIVE_SPEC_FIELD_SLOT_LABEL_POLICY           = 35,
    PF_NATIVE_SPEC_FIELD_FEED_TOLERANCE              = 36, /**< `feed_tolerance`: the kernel's
                                                            *   `legacy_tolerance`. */
    PF_NATIVE_SPEC_FIELD_PATH_ORDER                  = 37,
    PF_NATIVE_SPEC_FIELD_REPORT_POLICY               = 38,
    PF_NATIVE_SPEC_FIELD_PRICE_GRID                  = 39,
    PF_NATIVE_SPEC_FIELD_GRID_ROUNDING               = 40,
    PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_TIMEFRAME      = 41,
    PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_BARS           = 42,
    PF_NATIVE_SPEC_FIELD_MARGIN_MODEL                = 43,
    PF_NATIVE_SPEC_FIELD_MARGIN_INITIAL              = 44,
    PF_NATIVE_SPEC_FIELD_MARGIN_MAINTENANCE          = 45,
    PF_NATIVE_SPEC_FIELD_MARGIN_SIZING               = 46,
    PF_NATIVE_SPEC_FIELD_MARGIN_SHORTFALL_MULTIPLE   = 47,
    PF_NATIVE_SPEC_FIELD_MARGIN_MIN_UNITS            = 48,
    PF_NATIVE_SPEC_FIELD_MARGIN_CHECK                = 49,
    PF_NATIVE_SPEC_FIELD_MARGIN_EQUITY_BASIS         = 50,
    PF_NATIVE_SPEC_FIELD_MARGIN_LEVEL_BASE           = 51,
    PF_NATIVE_SPEC_FIELD_CALCULATION                 = 52,
    PF_NATIVE_SPEC_FIELD_OPEN_BAR_VIEW               = 53,
    PF_NATIVE_SPEC_FIELD_RISK_LIMITS                 = 54,
    PF_NATIVE_SPEC_FIELD_RISK_DRAWDOWN               = 55,
    PF_NATIVE_SPEC_FIELD_RISK_INTRADAY_LOSS          = 56,
    PF_NATIVE_SPEC_FIELD_RISK_LOSS_DAYS              = 57,
    PF_NATIVE_SPEC_FIELD_RISK_FILLS_PER_DAY          = 58,
    PF_NATIVE_SPEC_FIELD_RISK_DAY_BASIS              = 59,
    PF_NATIVE_SPEC_FIELD_RISK_ACTION                 = 60,
    PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_TIMEFRAME    = 61,
    PF_NATIVE_SPEC_FIELD_AUXILIARY_FEED_BARS         = 62,
    PF_NATIVE_SPEC_FIELD_SUBSCRIPTION_SOURCE         = 63,
    PF_NATIVE_SPEC_FIELD_EVENT_RETENTION             = 64
} pf_native_spec_field_t;

/** Why an append was refused — `NativeAuxiliaryAppendError`: the `error`
 *  out-parameter of #strategy_native_append_auxiliary_bars_ext_v1. None of
 *  them fails the host but REENTRANT, which latches the contract failure
 *  every reentrant stream input is, and ALLOCATION_FAILURE. */
typedef enum pf_native_append_error_e {
    PF_NATIVE_APPEND_ERROR_NONE                          = 0, /**< Applied. */
    PF_NATIVE_APPEND_ERROR_HOST_FAILED                   = 1, /**< The host had already
                                                               *   failed. */
    PF_NATIVE_APPEND_ERROR_REENTRANT                     = 2, /**< From inside a callback or
                                                               *   an in-flight input. */
    PF_NATIVE_APPEND_ERROR_NOT_REALTIME                  = 3, /**< Not on a stream's
                                                               *   realtime leg. */
    PF_NATIVE_APPEND_ERROR_NO_AUXILIARY_FEED             = 4, /**< The run declared no
                                                               *   feed. */
    PF_NATIVE_APPEND_ERROR_INVALID_BAR_ARRAY             = 5, /**< A NULL array with a
                                                               *   non-zero count. */
    PF_NATIVE_APPEND_ERROR_INVALID_BAR                   = 6, /**< `index` has invalid
                                                               *   OHLCV. */
    PF_NATIVE_APPEND_ERROR_UNORDERED_BARS                = 7, /**< `index` is not strictly
                                                               *   after its predecessor
                                                               *   (the feed's last bar
                                                               *   for index 0). */
    PF_NATIVE_APPEND_ERROR_INPUT_PERIOD_ALREADY_ACCEPTED = 8, /**< The first bar opened
                                                               *   inside an input period
                                                               *   already accepted. */
    PF_NATIVE_APPEND_ERROR_ALLOCATION_FAILURE            = 9  /**< Growing the feed threw;
                                                               *   this fails the host. */
} pf_native_append_error_t;

/* ── The policy hooks' words ───────────────────────────────────── */

/** How an opening settles against an opposite book —
 *  `native_order::OpeningShape`, #pf_native_terms_v1::shape. Only an OPENING
 *  may name a shape other than TRANSACT: a kernel-sized one
 *  (#PF_NATIVE_INTENT_SIZED) that the answer is about; the kernel refuses any
 *  other with #PF_NATIVE_MATCH_REJECT_INVALID_TERMS. */
typedef enum pf_native_opening_shape_e {
    PF_NATIVE_OPENING_SHAPE_TRANSACT       = 0, /**< Net against the book (the default). */
    PF_NATIVE_OPENING_SHAPE_REVERSE_TO     = 1, /**< Close the opposite book, then open the
                                                 *   units. */
    PF_NATIVE_OPENING_SHAPE_CLOSE_OPPOSITE = 2  /**< Close the opposite book and open the
                                                 *   remainder. */
} pf_native_opening_shape_t;

/** Which price a matching candidate is at — `native_order::NativeCandidatePriceKind`,
 *  #pf_native_terms_view_v1::price_kind. */
typedef enum pf_native_candidate_price_e {
    PF_NATIVE_CANDIDATE_PRICE_POINT_PRICE   = 0, /**< The driver point's own price. */
    PF_NATIVE_CANDIDATE_PRICE_TRIGGER_LEVEL = 1, /**< A resting level the path crossed. */
    PF_NATIVE_CANDIDATE_PRICE_CURRENT_QUOTE = 2  /**< A current execution's quote. */
} pf_native_candidate_price_t;

/** Which kind of driver point a candidate matched at —
 *  `native_order::DriverEligibilityClass`,
 *  #pf_native_terms_view_v1::driver_class. */
typedef enum pf_native_driver_class_e {
    PF_NATIVE_DRIVER_CLASS_OBSERVED_PRINT                    = 0,
    PF_NATIVE_DRIVER_CLASS_CARRIED_OPEN                      = 1,
    PF_NATIVE_DRIVER_CLASS_TICK_AFTER_CALCULATION            = 2,
    PF_NATIVE_DRIVER_CLASS_CONFIRMED_OPEN                    = 3,
    PF_NATIVE_DRIVER_CLASS_CONFIRMED_EXCURSION               = 4,
    PF_NATIVE_DRIVER_CLASS_CONFIRMED_AFTER_CALCULATION_CLOSE = 5,
    PF_NATIVE_DRIVER_CLASS_CURRENT_EXECUTION                 = 6
} pf_native_driver_class_t;

/** The settlement shape an execution is about to take — the alternative of
 *  `native_order::ExecutionPlan`, #pf_native_precommit_view_v1::plan. */
typedef enum pf_native_plan_e {
    PF_NATIVE_PLAN_FLATTEN    = 0, /**< Close the whole book; `plan_units` 0. */
    PF_NATIVE_PLAN_REDUCE     = 1, /**< Close `plan_units` units. */
    PF_NATIVE_PLAN_TRANSACT   = 2, /**< Trade `plan_units` signed units. */
    PF_NATIVE_PLAN_REVERSE_TO = 3  /**< Reach `plan_units` signed exposure. */
} pf_native_plan_t;

/** What #pf_native_callbacks_v1::on_precommit answers —
 *  `NativePrecommitVerdict`. */
typedef enum pf_native_precommit_verdict_e {
    PF_NATIVE_PRECOMMIT_ADMIT                  = 0, /**< The kernel's own path. */
    PF_NATIVE_PRECOMMIT_REFUSE                 = 1, /**< A nonfinancial
                                                     *   #PF_NATIVE_MATCH_REJECT_HOST_PRECOMMIT. */
    PF_NATIVE_PRECOMMIT_ADMIT_WITH_HOST_MARGIN = 2  /**< Admit, and the host owns this one
                                                     *   opening's margin check: the
                                                     *   kernel's initial-margin gate is
                                                     *   skipped for it. */
} pf_native_precommit_verdict_t;

/** Which trigger of an anchored leg the owner's fill supplies —
 *  `NativeAnchoredTrigger`, #pf_native_anchored_level_view_v1::trigger. */
typedef enum pf_native_anchored_trigger_e {
    PF_NATIVE_ANCHORED_TRIGGER_LIMIT     = 0,
    PF_NATIVE_ANCHORED_TRIGGER_STOP      = 1,
    PF_NATIVE_ANCHORED_TRIGGER_TRAIL_ARM = 2
} pf_native_anchored_trigger_t;

/** @} */ /* end of pf_native_c_enums */

/** @defgroup pf_native_c_types Transport types
 *  @{ */

/** Where the kernel is, presented to every callback.
 *
 *  A read-only snapshot of `NativeDecisionContext` plus the current quote:
 *  `price` is `current_execution_point()`'s price (the bar's own close at its
 *  close calculation) and NaN only where that answers nullopt.
 *
 *  It has TWO published layouts, and the runtime PRESENTS the one the
 *  caller's callback table was published with: a table sent at the current
 *  length is handed the whole struct, with its session tail; a table sent at
 *  an earlier published length is handed `struct_size` =
 *  #PF_NATIVE_DECISION_V1_BASE_SIZE — the sizeof that caller's own header
 *  compiled — and nothing past it is filled. So an older caller's exact-size
 *  check keeps holding, and a caller reads a field only below `struct_size`.
 *
 *  The four session-day bytes after `quote_kind` (R5 lane F5) sit in what
 *  was the base layout's tail padding: its size and every offset before them
 *  are the ones a caller compiled against the first layout reads, so that
 *  caller keeps working unchanged, and a table of either length is handed
 *  them. `session_facts` is 1 when the runtime wrote the three after it, and
 *  0 from a runtime that predates them. Each is 0 or 1, and every callback of
 *  one script bar carries the same values:
 *   - `in_session`: the script bar's label is in session on the run's own
 *     calendar (`session` / `timezone`);
 *   - `opens_session_day`: in session, and the bar before it is not, or is
 *     on another session day;
 *   - `closes_session_day`: in session, and the bar after it is not, or is on
 *     another session day.
 *  The session day is the run calendar's (it rolls at the first window's
 *  start and is keyed to its trading date, so an overnight session is one day
 *  across local midnight). The bar before / after is the one the run holds --
 *  the batch input or stream warmup -- and otherwise the calendar's slot one
 *  script width away; a run's first bar opens its day and a batch's final bar
 *  closes it. A D/W/M bar holds whole days: all three are 1. The C++ context's
 *  fourth fact, `closes_session_day_open_ended`, has no byte here: it differs
 *  only on a batch's final bar, for a host that recomputes a batch whose last
 *  input is still forming, and a C host's live edge is a stream, whose
 *  `closes_session_day` already reads the calendar.
 *
 *  The session tail is the script interval under delivery and the session
 *  day of its open and of the NEXT input's open, on the run's own calendar
 *  (its `session` and `timezone`): the calendar facts under the session-day
 *  bytes above, for a host that applies a rule of its own. On the calendar
 *  alone, a script bar is the last of its session day when
 *  `next_input_session_day_ordinal` differs from `session_day_ordinal`, and
 *  the first when the bar before it was the last. */
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
    uint8_t  provenance;         /**< #pf_native_price_provenance_t. */
    uint8_t  path_phase;         /**< #pf_native_path_phase_t. */
    uint8_t  completion;         /**< #pf_native_completion_kind_t. */
    uint8_t  quote_kind;         /**< #pf_native_quote_kind_t; 0 when price is NaN. */
    uint8_t  session_facts;      /**< 1 when the three bytes below are written. */
    uint8_t  in_session;         /**< The script bar is in session. */
    uint8_t  opens_session_day;  /**< It opens its session day. */
    uint8_t  closes_session_day; /**< It closes its session day. */

    /* ── The additive session tail (R5 lane F4). Presented only to a callback
     * table of the current layout; see the struct note. ── */
    int64_t  script_interval_open_ms;             /**< The script interval's nominal origin. */
    int64_t  script_interval_eligible_open_ms;    /**< Its first in-session instant. */
    int64_t  script_interval_last_traded_close_ms; /**< Its exclusive end of trading. */
    int64_t  script_interval_next_period_open_ms; /**< The next interval's nominal open,
                                                   *   a closed slot included. */
    int64_t  script_interval_next_input_open_ms;  /**< The next actual eligible input
                                                   *   open at or after that close:
                                                   *   closed time skipped. */
    int64_t  session_day_ordinal;                 /**< The session day of
                                                   *   `script_interval_open_ms`: days
                                                   *   since 1970-01-01 of its trading
                                                   *   date. */
    int64_t  next_input_session_day_ordinal;      /**< The session day of
                                                   *   `script_interval_next_input_open_ms`. */
    uint8_t  has_script_interval;                 /**< 1 when the point has a script
                                                   *   interval; the seven fields above
                                                   *   are 0 otherwise. */
    uint8_t  has_session_day;                     /**< 1 when the calendar keyed
                                                   *   `session_day_ordinal`. */
    uint8_t  has_next_input_session_day;          /**< 1 when it keyed
                                                   *   `next_input_session_day_ordinal`. */
    uint8_t  reserved0[5];                        /**< Always 0. */
} pf_native_decision_v1;

/** Byte length of #pf_native_decision_v1 as the L13 lane first published it,
 *  before the session tail was appended — the `struct_size` a callback table
 *  of an earlier published length is presented. It is the offset of the first
 *  appended field, which is that layout's sizeof on every target (it ended in
 *  padding the tail's first 8-byte field begins after). */
#define PF_NATIVE_DECISION_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_decision_v1, script_interval_open_ms))

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
    uint8_t  terminal_reason;     /**< #pf_native_terminal_reason_t, valid when
                                   *   `has_terminal_reason`. */
    uint8_t  has_terminal_reason; /**< 1 when `terminal_reason` is meaningful. */
    uint8_t  reserved0;
} pf_native_applied_v1;

/** One recorded event, read back by #strategy_native_events_v1.
 *
 *  `kind` tags the union: every field below is documented per kind and is
 *  zero where that kind has no such fact. `reason` is read by the kind's own
 *  enumeration, and is 0 for a kind that names none.
 *   - ACCEPTED / ARMED: `incarnation`.
 *   - REJECTED: `reason` is a #pf_native_reject_reason_t.
 *   - REPLACE_REJECTED: `incarnation` is the target, `reason` a
 *     #pf_native_reject_reason_t.
 *   - REPLACED: `incarnation` is the predecessor, `successor` the new handle.
 *   - CANCELLED: `reason` is a #pf_native_cancel_reason_t.
 *   - NOT_WORKING / INVALID_HANDLE: `incarnation` is the target.
 *   - NO_EFFECT: `incarnation`, cursor fields.
 *   - MATCH_REJECTED: `reason` is a #pf_native_match_reject_t, cursor fields.
 *   - APPLIED: every price/unit/cycle field, `terminal`, cursor fields; when
 *     `terminal` is 1, `reason` is the #pf_native_terminal_reason_t that
 *     finished the request.
 *   - CLOSE_BOUND: `reason` is the #pf_native_side_t of the book the close
 *     bound to, `cycle_after` its cycle, cursor fields.
 *   - RESERVATION_REDUCED / DEFERRED_GROUP: `incarnation` is the recipient,
 *     `reason` its #pf_native_group_effect_t, `closed_units` the deduction
 *     (applied, or deferred).
 *   - QUANTITY_BOUND: `incarnation`, `opened_units` = the source units.
 *   - TERMS_RESOLVED: `raw_price`, `resolved_price` (= `price`), cursor fields.
 *   - MARGIN_CALL: `reason` is the #pf_native_side_t of the liquidated
 *     position, `price` = mark, `closed_units` = liquidated units,
 *     `opened_units` = the signed position after it, `raw_price` = the
 *     re-solved liquidation price. The call's equity, requirement and the
 *     position before it are #strategy_native_margin_call_v1's.
 *   - ACTIVATED: `reason` is a #pf_native_activation_t, `price` the reached
 *     price.
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
    uint8_t  provenance;        /**< Cursor #pf_native_price_provenance_t. */
    uint8_t  path_phase;        /**< Cursor #pf_native_path_phase_t. */
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
 *  This readout has THREE published layouts and the runtime fills any: the
 *  base layout the L13 lane first shipped (#PF_NATIVE_WORKING_V1_BASE_SIZE),
 *  that plus `trail_has_arm_price` (#PF_NATIVE_WORKING_V1_ARM_SIZE), and the
 *  current one, which appends the anchor and the owner relation. A caller
 *  compiled against an earlier layout keeps working unchanged: its row is
 *  written up to its own length and never past it. Any other `struct_size`
 *  is PF_NATIVE_E_STRUCT.
 *
 *  The relation tail is the request's own anchor and owner as the kernel
 *  holds them now: an anchored leg reads FROM_OWNER_FILL, its rounding and
 *  its offset (a tick-spelled one already resolved to price units at
 *  acceptance) until its owner fills, and ABSOLUTE with the installed level
 *  in `p1` / `p2` after; the owner relation names the owner handle, how many
 *  there are and the bound cycle, and a WAIT_FOR_APPLIED child's visibility,
 *  first-match rule and scope.
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
    uint32_t origin;          /**< #pf_native_origin_t. */
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

    /* ── The additive relation tail (R5 lane F4). Written only when
     * `struct_size` is the current sizeof; see the struct note. ── */
    uint32_t anchor;              /**< #pf_native_anchor_t as the request stands. */
    uint32_t anchor_rounding;     /**< #pf_native_anchor_rounding_t, FROM_OWNER_FILL only. */
    double   anchor_offset;       /**< FROM_OWNER_FILL: the signed offset, price units. */
    uint64_t owner_handle;        /**< WAIT_FOR_APPLIED: the parent; BIND_OPENING: the
                                   *   opening; BIND_OPENINGS: the first of them as
                                   *   the kernel ordered the list; BIND_COHORT: the
                                   *   cohort; 0 for INDEPENDENT. */
    int64_t  owner_cycle;         /**< BIND_OPENING / BIND_OPENINGS: the bound cycle. */
    uint32_t owner_n;             /**< How many owner handles the relation names. */
    uint32_t visibility;          /**< #pf_native_arm_visibility_t, WAIT_FOR_APPLIED only. */
    uint32_t arm_first_match;     /**< #pf_native_arm_first_match_t, WAIT_FOR_APPLIED only. */
    uint32_t arm_scope;           /**< #pf_native_arm_scope_t, WAIT_FOR_APPLIED only. */
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

/** Byte length of #pf_native_working_v1 with the arm-presence tail but
 *  without the relation tail — the second of its three published layouts,
 *  and the `sizeof` every caller compiled before that tail existed sends.
 *  The offset of the first relation field, for the same reason
 *  #PF_NATIVE_WORKING_V1_BASE_SIZE is an offset. */
#define PF_NATIVE_WORKING_V1_ARM_SIZE \
    ((uint32_t)offsetof(pf_native_working_v1, anchor))

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
    uint32_t failure_code;     /**< #pf_native_failure_code_t;
                                *   PF_NATIVE_FAILURE_CALLBACK_EXCEPTION for a callback
                                *   that returned non-zero. */
    uint32_t failure_operation; /**< #pf_native_failure_operation_t. */
    uint32_t failure_discriminator; /**< Reserved beside the code: the kernel records no
                                     *   discriminator on this surface, so it reads 0. */
    uint64_t failure_ordinal;  /**< The point the failure was latched at, 0 when absent. */
    uint64_t consumed_high_water;
    int64_t  decision_floor_ms;
    uint32_t phase;            /**< #pf_native_run_phase_t while Running. */
    uint32_t completion;       /**< #pf_native_completion_t once Completed. */
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
    uint8_t  cursor_provenance; /**< #pf_native_price_provenance_t. */
    uint8_t  cursor_path_phase; /**< #pf_native_path_phase_t. */
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

/** The facts of one matching candidate, handed to
 *  #pf_native_callbacks_v1::on_execution_terms — the price half of
 *  `NativeExecutionTermsFacts`. Every field is what the kernel measured at
 *  this candidate; `default_resolved_price` is the price it would settle at
 *  (slippage and the grid applied), i.e. what the host answers DEFAULT to. */
typedef struct pf_native_terms_view_v1 {
    uint32_t struct_size;             /**< sizeof(pf_native_terms_view_v1). */
    uint32_t version;                 /**< PF_NATIVE_API_VERSION. */
    uint64_t incarnation;             /**< The request being resolved. */
    uint32_t intent;                  /**< #pf_native_intent_t of that request. */
    uint32_t trigger;                 /**< #pf_native_trigger_t of that request. */
    uint32_t trigger_state;           /**< #pf_native_trigger_state_t. */
    uint32_t remaining_kind;          /**< #pf_native_remaining_t. */
    double   remaining_units;         /**< Valid when `remaining_kind` is UNITS. */
    uint32_t driver_class;            /**< #pf_native_driver_class_t. */
    uint32_t price_kind;              /**< #pf_native_candidate_price_t. */
    uint32_t quote_kind;              /**< #pf_native_quote_kind_t. */
    uint32_t price_rule;              /**< #pf_native_price_rule_t. */
    uint32_t is_buy;                  /**< 0/1: the side this candidate trades on. */
    uint32_t shared_cursor_collision; /**< 0/1: a rounded quote shared with another level. */
    double   raw_price;               /**< The path price the match was found at. */
    double   trigger_level;           /**< The level crossed, when `has_trigger_level`. */
    uint32_t has_trigger_level;       /**< 0/1. */
    uint32_t reserved0;               /**< Always 0. */
    double   default_resolved_price;  /**< The kernel's own settlement price. */
    double   scope_exposure_units;    /**< What the bound scope holds here. */
    double   position_units;          /**< The whole physical book, signed. */
    double   position_average_price;
    uint64_t position_lot_count;
    double   opposite_book_units;
    double   pending_group_deduction;
    int64_t  fx_effective_time_ms;    /**< Where the account rate is read. */
    double   active_fx;               /**< The account rate there. */
    uint64_t cursor_ordinal;
    int64_t  cursor_effective_time_ms;
    double   cursor_t;
    int32_t  cursor_interval_index;
    uint8_t  cursor_provenance;       /**< #pf_native_price_provenance_t. */
    uint8_t  cursor_path_phase;       /**< #pf_native_path_phase_t. */
    uint8_t  reserved1[2];            /**< Always 0. */
} pf_native_terms_view_v1;

/** The price half of the terms #pf_native_callbacks_v1::on_execution_terms
 *  answers — `ExecutionTerms` less its units, which
 *  #pf_native_callbacks_v1::on_close_units answers for a host-sized close.
 *  The runtime fills it with the kernel's default (`default_resolved_price`,
 *  TRANSACT, SNAP) before the call. */
typedef struct pf_native_terms_v1 {
    uint32_t struct_size;    /**< sizeof(pf_native_terms_v1). */
    uint32_t version;        /**< PF_NATIVE_API_VERSION. */
    double   resolved_price; /**< The price to settle at. */
    uint32_t shape;          /**< #pf_native_opening_shape_t. */
    uint32_t grid_policy;    /**< #pf_native_grid_policy_t. */
} pf_native_terms_v1;

/** The last facts before one execution's physical effect, handed to
 *  #pf_native_callbacks_v1::on_precommit — `NativePrecommitView`: the plan,
 *  the prices, the settlement inspection, and the account the fill would
 *  leave (the projection's closed rows' P&L, borrowed for the call, in
 *  roster order). Offered once per ready attempt, never for an inspection,
 *  and only when the settlement is ready to apply. */
typedef struct pf_native_precommit_view_v1 {
    uint32_t struct_size;              /**< sizeof(pf_native_precommit_view_v1). */
    uint32_t version;                  /**< PF_NATIVE_API_VERSION. */
    uint64_t incarnation;              /**< The request about to execute. */
    uint32_t intent;                   /**< #pf_native_intent_t of that request. */
    uint32_t plan;                     /**< #pf_native_plan_t. */
    double   plan_units;               /**< See #pf_native_plan_t. */
    double   raw_price;
    double   resolved_price;
    double   inspected_closed_units;
    double   inspected_opened_units;
    double   inspected_current_ticket;
    uint32_t current;                  /**< 1 for #strategy_native_execute_current_v1. */
    uint32_t would_open;               /**< 0/1: the fill opens a lot. */
    uint32_t incoming_short;           /**< 0/1: the opened lot is short. */
    uint32_t reserved0;                /**< Always 0. */
    double   resulting_abs_units;
    uint64_t resulting_lot_count;
    double   resulting_abs_notional;
    double   realized_balance;         /**< After the fill, account currency. */
    double   remaining_entry_cost;     /**< Paid costs on the resulting book. */
    double   marked_equity;            /**< After the fill, at its price. */
    int64_t  cycle_after;
    double   signed_units_after;
    const double* closed_row_pnl;      /**< `closed_row_count` values; borrowed. */
    uint64_t closed_row_count;
    uint64_t cursor_ordinal;
    int64_t  cursor_effective_time_ms;
    double   cursor_t;
    int32_t  cursor_interval_index;
    uint8_t  cursor_provenance;        /**< #pf_native_price_provenance_t. */
    uint8_t  cursor_path_phase;        /**< #pf_native_path_phase_t. */
    uint8_t  reserved1[2];             /**< Always 0. */
} pf_native_precommit_view_v1;

/** One anchored leg's materialization, handed to
 *  #pf_native_callbacks_v1::on_anchored_level exactly once, before its armed
 *  event — `NativeAnchoredLevelView`. `kernel_level` is fill + offset after
 *  the anchor's own rounding: what the host answers DEFAULT to. */
typedef struct pf_native_anchored_level_view_v1 {
    uint32_t struct_size;             /**< sizeof(pf_native_anchored_level_view_v1). */
    uint32_t version;                 /**< PF_NATIVE_API_VERSION. */
    uint64_t owner;                   /**< The request whose fill arms the leg. */
    uint64_t owner_applied_ordinal;   /**< That fill's applied execution. */
    uint64_t owner_lot_incarnation;   /**< The lot it opened. */
    double   owner_fill_price;        /**< Its resolved price. */
    uint64_t leg;                     /**< The anchored request. */
    uint32_t leg_side;                /**< #pf_native_side_t the leg trades on. */
    uint32_t trigger;                 /**< #pf_native_anchored_trigger_t. */
    double   offset;                  /**< The anchor's offset, price units. */
    double   price_tick;              /**< The run's tick, 0 when none. */
    double   kernel_level;            /**< The level the kernel would install. */
    uint64_t owner_cursor_ordinal;
    int64_t  owner_cursor_effective_time_ms;
    double   owner_cursor_t;
    int32_t  owner_cursor_interval_index;
    uint8_t  owner_cursor_provenance; /**< #pf_native_price_provenance_t. */
    uint8_t  owner_cursor_path_phase; /**< #pf_native_path_phase_t. */
    uint8_t  reserved0[2];            /**< Always 0. */
} pf_native_anchored_level_view_v1;

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

/** One kernel-issued liquidation that filled, whole — the C spelling of
 *  `native_order::MarginCallEvent`, copied out by
 *  #strategy_native_margin_call_v1.
 *
 *  It carries the margin facts of that fill, so a host reconstructs the
 *  outcome without recomputing the account: `mark` is the booked resolved
 *  price, `equity` and `required` are the marked equity and the maintenance
 *  requirement of the SURVIVING book at that price, `liquidation_price` is
 *  the level re-solved for what is left, and `position_before` /
 *  `position_after` are the signed book on either side of the reduction.
 *  `applied_ordinal` names the #PF_NATIVE_EVENT_APPLIED row that booked it;
 *  the cursor is the point it filled at. The #PF_NATIVE_EVENT_MARGIN_CALL row
 *  of the same `ordinal` carries a subset of these facts. */
typedef struct pf_native_margin_call_v1 {
    uint32_t struct_size;       /**< sizeof(pf_native_margin_call_v1). */
    uint32_t version;           /**< PF_NATIVE_API_VERSION. */
    uint64_t ordinal;           /**< The call's event ordinal. */
    uint64_t incarnation;       /**< The kernel's liquidation request. */
    uint64_t applied_ordinal;   /**< The applied execution that booked it. */
    uint32_t side;              /**< #pf_native_side_t of the liquidated position. */
    uint32_t reserved0;         /**< Always 0. */
    double   mark;              /**< The booked resolved price. */
    double   equity;            /**< Marked equity of the surviving book at `mark`. */
    double   required;          /**< Maintenance requirement of the surviving book at `mark`. */
    double   liquidation_price; /**< The level re-solved for what is left. */
    double   units;             /**< Units liquidated. */
    double   position_before;   /**< Signed book before the reduction. */
    double   position_after;    /**< Signed book after it. */
    uint64_t cursor_ordinal;
    int64_t  cursor_effective_time_ms;
    double   cursor_t;
    int32_t  cursor_interval_index;
    uint8_t  cursor_provenance; /**< #pf_native_price_provenance_t. */
    uint8_t  cursor_path_phase; /**< #pf_native_path_phase_t. */
    uint8_t  reserved1[2];      /**< Always 0. */
} pf_native_margin_call_v1;

/** One order request. Translated field by field into `native_order::Request`;
 *  it is never cast. Zero-initialise it, set `struct_size` and `version`, then
 *  set only the fields the chosen `intent` and `trigger` document.
 *
 *  This struct has FIVE published layouts and the runtime accepts any of
 *  them: the base layout the L13 lane first shipped
 *  (#PF_NATIVE_REQUEST_V1_BASE_SIZE), that layout plus L7b's anchored-leg
 *  tail (#PF_NATIVE_REQUEST_V1_ANCHOR_SIZE), that plus L3b's sizing detail
 *  `size_price`, `reduce_basis` (#PF_NATIVE_REQUEST_V1_SIZING_SIZE), that
 *  plus E14's trail seed (`trail_best_seed`, `trail_has_best_seed`;
 *  #PF_NATIVE_REQUEST_V1_SEED_SIZE), and the current one, which appends the
 *  arm relation (`arm_first_match`, `arm_scope`). A caller compiled against
 *  an earlier layout keeps working unchanged and simply gets the later tails'
 *  defaults (RAW, WORKING, RESOLVED, AT_MATCH, no seed, AT_ARM_PRINT,
 *  OWNER_LOT). Any other `struct_size` is PF_NATIVE_E_STRUCT. Every tail is
 *  append-only: nothing above it moved. */
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

    /* ── The additive trail-seed tail (E14). Read only when `struct_size`
     * is the current sizeof; a caller sending any earlier layout stops above
     * and gets no seed, which is what every trail accepted before this tail
     * already rode. ── */
    double   trail_best_seed;     /**< TRAIL: where the running best starts —
                                   *   a floor on it, the favourable one of
                                   *   this level and the arm's own print.
                                   *   Absolute (an anchor moves the arm
                                   *   threshold, not this), finite and
                                   *   positive, else PF_NATIVE_E_REJECTED.
                                   *   It is a submission input: the working
                                   *   readback carries the live best, not the
                                   *   seed — see the asymmetry note above. */
    uint8_t  trail_has_best_seed; /**< TRAIL: `trail_best_seed` is set. */
    uint8_t  reserved2[7];        /**< Must be 0. The seed layout's own padding,
                                   *   named so the tail below starts where
                                   *   that layout's sizeof ended. */

    /* ── The additive arm-relation tail (R5 lane F4). Read only when
     * `struct_size` is the current sizeof; a caller sending any earlier
     * layout stops above and keeps both defaults (AT_ARM_PRINT, OWNER_LOT),
     * which is what every child accepted before this tail already armed as.
     * A non-default value is legal under WAIT_FOR_APPLIED alone. ── */
    uint32_t arm_first_match;     /**< #pf_native_arm_first_match_t. */
    uint32_t arm_scope;           /**< #pf_native_arm_scope_t. */
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
 *  without L3b's sizing-detail tail — the second of its five published
 *  layouts. Defined as the offset of the first field appended after it, for
 *  the same reason #PF_NATIVE_REQUEST_V1_BASE_SIZE is. */
#define PF_NATIVE_REQUEST_V1_ANCHOR_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, size_price))

/** Byte length of #pf_native_request_v1 with L3b's sizing-detail tail but
 *  without E14's trail seed — the third of its five published layouts, and
 *  the `sizeof` every caller compiled before that seed existed sends.
 *  Defined as the offset of the first field appended after it, for the same
 *  reason #PF_NATIVE_REQUEST_V1_BASE_SIZE is. */
#define PF_NATIVE_REQUEST_V1_SIZING_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, trail_best_seed))

/** Byte length of #pf_native_request_v1 with E14's trail seed but without
 *  the arm-relation tail — the fourth of its five published layouts, and the
 *  `sizeof` every caller compiled before that tail existed sends. The seed
 *  layout ended in padding after `trail_has_best_seed`; `reserved2` names
 *  exactly that padding, so the offset of the first arm field IS that sizeof
 *  on every target. */
#define PF_NATIVE_REQUEST_V1_SEED_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, arm_first_match))

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
    uint32_t lookahead;     /**< #pf_native_lookahead_t. */
    const char* tf;         /**< Non-NULL timeframe literal. */
    const pf_bar_t* authoritative_bars; /**< Optional exchange bars; copied. */
    int32_t authoritative_n;            /**< Length of `authoritative_bars`. */
    uint32_t gaps;          /**< #pf_native_gaps_t. */
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
 *  This struct has FIVE published layouts and the runtime accepts any of
 *  them: the base layout the L13 lane first shipped
 *  (#PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE); that layout plus L9's `risk_*`
 *  tail (#PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE); that one plus N8's intrabar
 *  path, the four feed-shape and presentation policies, and the margin
 *  model's equity basis, level base and liquidation strings
 *  (#PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE); that one plus the
 *  `auxiliary_*` tail (#PF_NATIVE_RUN_SPEC_EXT_V1_AUXILIARY_SIZE); and the
 *  current one, which appends the `event_retention` tail after them. A
 *  caller compiled against an earlier layout keeps working unchanged and
 *  simply cannot set the mask bits its struct has no fields for
 *  (#PF_NATIVE_SPEC_EXT_RISK, #PF_NATIVE_SPEC_EXT_INTRABAR,
 *  #PF_NATIVE_SPEC_EXT_FEED_POLICY, #PF_NATIVE_SPEC_EXT_AUXILIARY_FEED,
 *  #PF_NATIVE_SPEC_EXT_EVENT_RETENTION): doing so is PF_NATIVE_E_STRUCT. Any
 *  other `struct_size` is PF_NATIVE_E_STRUCT too. Every tail is append-only:
 *  nothing above them moved. */
typedef struct pf_native_run_spec_ext_v1 {
    uint32_t struct_size;    /**< sizeof(pf_native_run_spec_ext_v1). */
    uint32_t version;        /**< PF_NATIVE_API_VERSION. */
    uint32_t present_mask;   /**< #pf_native_spec_ext_mask_t bits. */

    uint32_t report_policy;               /**< #pf_native_report_policy_t. */
    uint32_t report_open_position_at_end; /**< 0/1; #PF_NATIVE_REPORT_KERNEL_RECORDED only. */

    uint32_t price_grid;      /**< #pf_native_price_grid_t. */
    uint32_t grid_rounding;   /**< #pf_native_grid_rounding_t. */

    uint32_t calculation;                  /**< #pf_native_calc_trigger_t. */
    uint32_t max_recalculations_per_point;  /**< Fill-cascade bound; 0 is legal. */

    uint32_t open_bar_view;   /**< #pf_native_open_bar_view_t. */

    uint32_t margin_sizing;   /**< #pf_native_liquidation_sizing_t. */
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

    /* ── The additive event-retention tail (V19-B). Read only when
     * `present_mask` carries PF_NATIVE_SPEC_EXT_EVENT_RETENTION; a caller
     * sending an earlier layout stops at `subscription_sources` or above and
     * runs under PF_NATIVE_EVENT_RETENTION_FULL. ── */
    uint32_t event_retention;  /**< #pf_native_event_retention_t. */
    uint32_t reserved2;        /**< Must be 0. */
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

/** Byte length of #pf_native_run_spec_ext_v1 with the `auxiliary_*` tail but
 *  before the `event_retention` tail was appended — the fourth of its five
 *  published layouts, an offset for the same reason
 *  #PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE is. */
#define PF_NATIVE_RUN_SPEC_EXT_V1_AUXILIARY_SIZE \
    ((uint32_t)offsetof(pf_native_run_spec_ext_v1, event_retention))

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
 *  with PF_NATIVE_FAILURE_CALLBACK. An ANSWERING callback — every hook from
 *  `on_margin_requirement` on — returns a #pf_native_answer_t selecting
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
 *  #PF_NATIVE_CALC_TRIGGER_BAR_CLOSE the kernel calls it again at each fill
 *  cursor or modeled point, which is exactly what the C++
 *  `on_native_recalculate` default does. */
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
    /** One completed bucket of a declared series. `completion` is a
     *  #pf_native_completion_kind_t: CONFIRMED when the bucket's own last
     *  contributing bar closed it, LAZY_COMPLETE when the next period's first
     *  input did. */
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

    /* ── The additive policy-hook tail (R5 lane F4). Read only when
     * `struct_size` is the current sizeof; a caller sending an earlier layout
     * stops at `on_close_units` (or at `on_margin_call`) and gets the
     * kernel's own answers for all four, exactly the C++ defaults. All four
     * are ANSWERING callbacks. ── */

    /** The price half of `resolve_execution_terms`, consulted at EVERY
     *  matching candidate once installed. ANSWERING callback: return
     *  #PF_NATIVE_ANSWER_DEFAULT to keep the kernel's terms, any other value
     *  to settle at @p out's price, shape and grid policy. @p out arrives
     *  holding the kernel's own. A shape or grid word outside its enumeration
     *  is the kernel's #PF_NATIVE_MATCH_REJECT_INVALID_TERMS, as is a shape
     *  other than TRANSACT on anything but an opening. The units half stays
     *  `on_close_units`'. */
    int (*on_execution_terms)(void* user, const pf_native_terms_view_v1* view,
                              pf_native_terms_v1* out);

    /** The last gate before one execution's physical effect —
     *  `validate_execution_precommit`. ANSWERING callback: return
     *  #PF_NATIVE_ANSWER_DEFAULT to admit it on the kernel's own path, any
     *  other value to use @p verdict (#pf_native_precommit_verdict_t). A
     *  verdict word outside the enumeration refuses, the conservative
     *  reading of an answer the kernel cannot act on. */
    int (*on_precommit)(void* user, const pf_native_precommit_view_v1* view,
                        uint32_t* verdict);

    /** Where an anchored leg is armed — `resolve_anchored_level`. ANSWERING
     *  callback: return #PF_NATIVE_ANSWER_DEFAULT to install the kernel's
     *  level, any other value to install @p level; the kernel's
     *  representability check still applies. */
    int (*on_anchored_level)(void* user, const pf_native_anchored_level_view_v1* view,
                             double* level);

    /** The host's own durable state, folded into the broker-state hash —
     *  `hash_host_extension`. Called once per hash (every per-bar row, the
     *  final and the stream hash) after the kernel's own bytes. ANSWERING
     *  callback: return #PF_NATIVE_ANSWER_DEFAULT to fold nothing more, any
     *  other value to fold a domain tag and @p digest, a 64-bit digest of
     *  whatever the host's next decision depends on. It must be a pure
     *  function of that state. The continuation hash is the kernel's alone
     *  and does not move. */
    int (*on_hash_extension)(void* user, uint64_t* digest);
} pf_native_callbacks_v1;

/** Byte length of #pf_native_callbacks_v1 as the L13 lane first published it,
 *  before the six-hook tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds for
 *  — it is not a literal. #strategy_native_host_create_v1 accepts this length
 *  as well as #PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE and the current `sizeof`,
 *  which is what makes each tail additive rather than a layout break. */
#define PF_NATIVE_CALLBACKS_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_callbacks_v1, on_recalculate))

/** Byte length of #pf_native_callbacks_v1 with the six-hook tail but without
 *  the policy-hook tail — the second of its three published layouts, and the
 *  `sizeof` every caller compiled before that tail existed sends. */
#define PF_NATIVE_CALLBACKS_V1_HOOKS_SIZE \
    ((uint32_t)offsetof(pf_native_callbacks_v1, on_execution_terms))

/** @} */ /* end of pf_native_c_types */

/** @defgroup pf_native_c_api Entry points
 *  @{ */

/** This header's layout version. Mirrors #strategy_stream_api_version. */
PF_API int strategy_native_api_version(void);

/** Allocate a native host that forwards every kernel callback to @p callbacks.
 *
 *  The table is copied; the caller's struct need not outlive the call. The
 *  returned handle is a `pf_strategy_t` the existing runtime symbols accept:
 *  #strategy_configure_native_v1, the whole `strategy_stream_*` family and the
 *  read-only accessors all take it unchanged.
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
 *  heap-allocated and released by #strategy_native_report_free_v1.
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
 *  @param reject       Optional; receives a #pf_native_reject_reason_t on
 *                      rejection.
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
 *  PF_NATIVE_E_REJECTED is where this signature stops: it has nowhere to put
 *  the RequestRejectReason that #strategy_native_submit_v1 writes to its
 *  `reject`, so use #strategy_native_replace_ext_v1 when the reason matters.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_replace_v1(pf_strategy_t s, uint64_t incarnation,
                                      const pf_native_request_v1* request,
                                      uint64_t* successor);

/** Replace the live request @p incarnation, reporting a rejection's reason.
 *
 *  The same call as #strategy_native_replace_v1 — same legality, same
 *  amendment, same verdicts, same successor — with the `reject`
 *  out-parameter #strategy_native_submit_v1 has always had. That asymmetry
 *  was the whole gap: a C++ replace answers ReplaceResult::reason and the C
 *  submit writes it out, while the C replace dropped it, so a C host learned
 *  "rejected" and no more. (#strategy_native_cancel_v1 needs no such
 *  spelling: its status IS its reason.)
 *
 *  It is an additional symbol rather than a wider signature because this
 *  header's STABILITY rule is that a signature never changes within a major
 *  version — the same reason #strategy_configure_native_ext_v1 exists beside
 *  #strategy_configure_native_v1. Use either; #strategy_native_replace_v1
 *  stays exactly what it was and is this call with @p reject NULL.
 *
 *  @param s           The host this run is driving, from #strategy_create.
 *  @param incarnation The live request to amend.
 *  @param request     Borrowed for the call only.
 *  @param successor   Optional; receives the successor's handle. Untouched
 *                     unless the replace is accepted.
 *  @param reject      Optional; receives a #pf_native_reject_reason_t, and only
 *                     on PF_NATIVE_E_REJECTED. Untouched otherwise — including
 *                     for NOT_WORKING and INVALID_TARGET, which are the
 *                     target's verdict and not the request's.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_REJECTED, PF_NATIVE_E_NOT_WORKING,
 *  PF_NATIVE_E_INVALID_TARGET, or PF_NATIVE_E_STATE.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_replace_ext_v1(pf_strategy_t s, uint64_t incarnation,
                                          const pf_native_request_v1* request,
                                          uint64_t* successor, uint32_t* reject);

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
 *  A caller built against an earlier layout sends
 *  #PF_NATIVE_WORKING_V1_BASE_SIZE or #PF_NATIVE_WORKING_V1_ARM_SIZE and is
 *  filled exactly that far.
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

/** The host has read every event through @p through_ordinal —
 *  `native_acknowledge_events()`.
 *
 *  Under the Window event retention the kernel drops the acknowledged command
 *  events at the next script-bar boundary and keeps every event above the
 *  acknowledgement; the first call also marks the host as one that polls, so
 *  a polling host calls it from `on_run_begin` (0 is legal). A later, smaller
 *  acknowledgement never lowers an earlier one, one above the event high
 *  water acknowledges the high water, and outside a running run the call
 *  records nothing. The Full and Commands retentions keep the whole journal
 *  whatever is acknowledged.
 *  @return PF_NATIVE_OK, or a negative status for a bad handle. */
PF_API int strategy_native_acknowledge_events_v1(pf_strategy_t s, uint64_t through_ordinal);

/** The oldest ordinal a read can still return — `native_event_window_start()`
 *  — written to @p out_first_ordinal: every command event at or above it is
 *  retained, and #strategy_native_events_v1 with an `after_ordinal` below it
 *  starts there. 1 while nothing was dropped.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_ARGUMENT for a NULL @p out_first_ordinal,
 *  or another negative status. */
PF_API int strategy_native_event_window_v1(pf_strategy_t s, uint64_t* out_first_ordinal);

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
 *  @return PF_NATIVE_OK when the list was staged. Each row is read first:
 *  PF_NATIVE_E_STRUCT for its `struct_size`, PF_NATIVE_E_ARGUMENT for a NULL
 *  `tf` or bad authoritative bars (and for a negative @p n or NULL @p rows),
 *  PF_NATIVE_E_TAG for a `lookahead` / `gaps` word outside its enumeration;
 *  then PF_NATIVE_E_STATE anywhere but inside `on_run_begin` and for a list this
 *  run's input timeframe would refuse — the same validation
 *  #strategy_configure_native_ext_v1 applies. A refusal stages nothing.
 *  #strategy_native_declare_subscriptions_ext_v1 also names which validation
 *  refused the list. */
PF_API int strategy_native_declare_subscriptions_v1(pf_strategy_t s,
                                                    const pf_native_subscription_v1* rows,
                                                    int n);

/** The same declaration, answered by name — `declare_timeframe_subscriptions_result()`.
 *
 *  The same call as #strategy_native_declare_subscriptions_v1 — same
 *  legality, same staging, same statuses — with the refusal's words written
 *  out, and with a series source per row, so a series declared here may be
 *  built from the auxiliary feed #strategy_native_declare_auxiliary_feed_v1
 *  staged. #strategy_native_declare_subscriptions_v1 stays exactly what it
 *  was: this call with @p sources, @p error and @p field NULL.
 *
 *  @param s        The host this run is driving.
 *  @param rows     @p n rows, borrowed for the call; NULL when @p n is 0.
 *  @param n        Row count (0 declares no series at all).
 *  @param sources  Optional: @p n #pf_native_series_source_t words, one per
 *                  row. NULL builds every series from the input.
 *  @param error    Optional; receives a #pf_native_spec_error_t whenever the
 *                  kernel judged the call: #PF_NATIVE_SPEC_ERROR_NONE when
 *                  staged, #PF_NATIVE_SPEC_ERROR_WRONG_PHASE outside
 *                  `on_run_begin`, else the first error the list's
 *                  validation found — the validation
 *                  #strategy_configure_native_ext_v1 applies.
 *  @param field    Optional; receives the #pf_native_spec_field_t beside it.
 *  @return PF_NATIVE_OK when the list was staged; PF_NATIVE_E_STATE for every
 *  refusal the kernel names in @p error, in which case nothing is staged;
 *  PF_NATIVE_E_ARGUMENT / E_STRUCT / E_TAG for a row the C layer refuses
 *  before the kernel sees the list, leaving @p error and @p field untouched.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_declare_subscriptions_ext_v1(pf_strategy_t s,
                                                        const pf_native_subscription_v1* rows,
                                                        int n, const uint32_t* sources,
                                                        uint32_t* error, uint32_t* field);

/** Declare, replace or withdraw this run's auxiliary finer feed from inside
 *  `on_run_begin` — `declare_auxiliary_feed_result()`.
 *
 *  The feed REPLACES the one staged by #strategy_configure_native_ext_v1's
 *  auxiliary tail, and the kernel registers from the staged spec after the
 *  callback returns. It is judged together with the series staged at that
 *  moment, so a host that declares both here declares the feed first and its
 *  feed-built series second (#strategy_native_declare_subscriptions_ext_v1).
 *
 *  @param s      The host this run is driving.
 *  @param tf     The feed's timeframe literal, or NULL to withdraw the staged
 *                feed (with @p n 0 and @p bars NULL).
 *  @param bars   @p n strictly increasing bars, copied; NULL when @p n is 0.
 *  @param n      Bar count.
 *  @param error  Optional; receives a #pf_native_spec_error_t whenever the
 *                kernel judged the call (#PF_NATIVE_SPEC_ERROR_NONE when
 *                staged, #PF_NATIVE_SPEC_ERROR_WRONG_PHASE outside
 *                `on_run_begin`).
 *  @param field  Optional; receives the #pf_native_spec_field_t beside it.
 *  @return PF_NATIVE_OK when the feed was staged or withdrawn;
 *  PF_NATIVE_E_STATE for every refusal the kernel names in @p error — a feed
 *  the input timeframe refuses, or a withdrawal that would strand a staged
 *  feed-built series — changing nothing; PF_NATIVE_E_ARGUMENT for a negative
 *  @p n, NULL @p bars with @p n > 0, or a withdrawal carrying bars, leaving
 *  @p error and @p field untouched.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_declare_auxiliary_feed_v1(pf_strategy_t s, const char* tf,
                                                     const pf_bar_t* bars, int32_t n,
                                                     uint32_t* error, uint32_t* field);

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

/** The units a kernel-sized request resolves to under this run's spec, as a
 *  pure query — `native_sized_units()`.
 *
 *  @p sized is a #PF_NATIVE_INTENT_SIZED request, read for its sizing block
 *  alone (side, basis and its value, reserve, grid policy): units =
 *  cash / (price * point_value * fx), the cash being the basis value or the
 *  fraction of @p equity, net of the percent fee reserve when the request
 *  asks for it, then the grid policy. It is the very function the kernel
 *  sizes with, at acceptance and at the candidate, so a host that gates a
 *  command on its quantity reads the number here before it submits.
 *  Observation only: it moves and freezes nothing.
 *
 *  @param s       The host whose run spec sizes the request.
 *  @param sized   The #PF_NATIVE_INTENT_SIZED request to size, borrowed for
 *                 the call.
 *  @param price   The sizing price.
 *  @param equity  The marked equity an EQUITY_FRACTION basis is a share of.
 *  @param fx      The account rate the conversion divides by.
 *  @param units   Receives the units; NaN with #PF_NATIVE_ABSENT.
 *  @return PF_NATIVE_OK, #PF_NATIVE_ABSENT when the run is not configured or
 *  the basis is unresolvable there (non-positive money or denominator, a
 *  below-one-step quotient under a snapping grid), PF_NATIVE_E_ARGUMENT for a
 *  request that is not SIZED or a NULL pointer, or the request's own
 *  PF_NATIVE_E_STRUCT / PF_NATIVE_E_TAG.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_sized_units_v1(pf_strategy_t s, const pf_native_request_v1* sized,
                                          double price, double equity, double fx,
                                          double* units);

/** The run's generic risk ledger — `native_risk_state()`. Every field is its
 *  zero for a run that declares no risk block. */
PF_API int strategy_native_risk_state_v1(pf_strategy_t s, pf_native_risk_state_v1* out);

/** The whole economics of one margin call — what a C++ host's
 *  `on_native_margin_call` receives as `MarginCallEvent`.
 *
 *  @p ordinal is the call's event ordinal: the `ordinal` of the
 *  #pf_native_event_v1 #pf_native_callbacks_v1::on_margin_call is handed, or
 *  of a #PF_NATIVE_EVENT_MARGIN_CALL row #strategy_native_events_v1 returns.
 *  Legal inside `on_margin_call` and anywhere the event history is;
 *  observation only.
 *
 *  @p out is an in/out size prefix: set `out->struct_size` to
 *  `sizeof(pf_native_margin_call_v1)` before the call.
 *  @return PF_NATIVE_OK when @p out was written, #PF_NATIVE_ABSENT when no
 *  margin call of this run has that ordinal (@p out untouched),
 *  PF_NATIVE_E_STRUCT for a mis-sized row, or another negative status.
 *
 *  Exercised by the fx-roll scenario of `tests/test_native_c_api.c`. */
PF_API int strategy_native_margin_call_v1(pf_strategy_t s, uint64_t ordinal,
                                          pf_native_margin_call_v1* out);

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
 *  specification the kernel's own validation rejects, which answers
 *  PF_NATIVE_E_ARGUMENT: the call validates the whole specification before it
 *  configures, so a rejected one leaves the handle Unconfigured.
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
 *  readable with #strategy_get_last_error and, by name, from
 *  #strategy_native_append_auxiliary_bars_ext_v1. A call from inside a callback fails the
 *  run, as every reentrant stream input does.
 *  @return PF_NATIVE_OK, or a negative status. */
PF_API int strategy_native_append_auxiliary_bars_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                    int32_t n);

/** The same append, answered by name — `append_auxiliary_bars_result()`.
 *
 *  The same call as #strategy_native_append_auxiliary_bars_v1 — same
 *  legality, same appended bars, same statuses, the same latched contract
 *  failure for a call from inside a callback — with the refusal written out.
 *  #strategy_native_append_auxiliary_bars_v1 stays this call with both
 *  out-parameters NULL.
 *
 *  @param s      The host this run is driving.
 *  @param bars   @p n bars, strictly increasing and after the feed's last
 *                bar, copied; NULL when @p n is 0.
 *  @param n      Bar count; 0 appends nothing.
 *  @param error  Optional; receives a #pf_native_append_error_t whenever the
 *                kernel judged the call (#PF_NATIVE_APPEND_ERROR_NONE when
 *                appended).
 *  @param index  Optional; receives the bar OF THIS CALL the refusal stopped
 *                on, 0 for a refusal that is not about one bar and for an
 *                applied append.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_STATE for every refusal the kernel names
 *  in @p error, or PF_NATIVE_E_ARGUMENT for a negative @p n or NULL @p bars
 *  with @p n > 0, which the C layer refuses itself, leaving both
 *  out-parameters untouched.
 *
 *  Exercised by `tests/test_native_c_api.c`. */
PF_API int strategy_native_append_auxiliary_bars_ext_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                        int32_t n, uint32_t* error,
                                                        int32_t* index);

/** Declare where the fill that opened a lot sits on its entry bar —
 *  `declare_opened_lot_entry_bar_mask()`, the entry-side half of the
 *  excursion capability #pf_native_callbacks_v1::on_lot_excursion owns
 *  (RULING A48).
 *
 *  The host says only WHERE its fill sat. The kernel walks @p entry_bar's
 *  modeled path in the run's own leg order, the one its matcher walks —
 *  #pf_native_run_spec_ext_v1::path_order when HIGH_FIRST or LOW_FIRST, and
 *  under AUTO the high first when |high - open| < |open - low|, the low first
 *  otherwise — finds where a price is first touched on it, and sets the
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
 *  @param s                  The host this run is driving, from #strategy_create.
 *  @param entry_incarnation  The request whose fill opened the lot.
 *  @param entry_bar          The whole bar the fill sat on; borrowed.
 *  @param fill_point         #pf_native_opened_lot_fill_point_t.
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
