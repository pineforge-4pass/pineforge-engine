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
 * ✓ submit / replace / cancel / cancel_all / execute_current
 * ✓ Read the physical position, the live working book and the event history
 * ✓ Read the run's lifecycle state and its typed failure
 * ✓ Extend the run specification with the fields pf_native_run_spec_v1 predates
 *
 * ✗ Streaming has no new symbols: strategy_stream_begin / _push_bar /
 *   _push_tick / _advance_time / _end / _fill_report take any handle this
 *   header produces, unchanged.
 * ✗ resolve_execution_terms / validate_execution_precommit /
 *   resolve_anchored_level are not exposed: the callback table carries no
 *   answering hook. A C host that needs kernel sizing uses
 *   PF_NATIVE_INTENT_SIZED; an anchored leg is armed at the kernel level
 *   (fill + offset, snapped per `anchor_rounding`).
 *
 * HARDENING RULES
 * ───────────────
 *  - Every struct is tagged and size-prefixed: `struct_size` is the exact
 *    sizeof of the version the caller compiled against, `version` is that
 *    layout's version constant. A mismatch is refused with PF_NATIVE_E_STRUCT
 *    and mutates nothing. The one exception is the deliberately additive tail
 *    of pf_native_run_spec_ext_v1: that struct has two published layouts and
 *    the runtime accepts either (see PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE).
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
 *  #pf_native_execute_outcome_e codes.
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
    PF_NATIVE_INTENT_REDUCE     = 1, /**< Reduce; see #pf_native_reduction_e. */
    PF_NATIVE_INTENT_TRANSACT   = 2, /**< `intent_value` signed units. */
    PF_NATIVE_INTENT_REVERSE_TO = 3, /**< `intent_value` target signed exposure. */
    PF_NATIVE_INTENT_HOST_SIZED = 4, /**< Adapter-only; refused PF_NATIVE_E_UNSUPPORTED. */
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
    PF_NATIVE_OWNER_BIND_COHORT      = 4  /**< `cohort` from #strategy_native_cohort_open_v1. */
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
    PF_NATIVE_SPEC_EXT_RISK          = 1u << 6
} pf_native_spec_ext_mask_t;

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
 *   - RISK: `reason` is a #pf_native_risk_limit_e, `price` = the observed
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
    uint32_t kind;              /**< #pf_native_event_kind_e. */
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
 *  the host keeps them longer. */
typedef struct pf_native_working_v1 {
    uint32_t struct_size;     /**< sizeof(pf_native_working_v1). */
    uint32_t version;         /**< PF_NATIVE_API_VERSION. */
    uint64_t incarnation;     /**< The request's handle. */
    uint32_t intent;          /**< #pf_native_intent_e as accepted. */
    uint32_t trigger;         /**< #pf_native_trigger_e as accepted. */
    uint32_t owner;           /**< #pf_native_owner_e as accepted. */
    uint32_t capacity;        /**< #pf_native_capacity_e as accepted. */
    uint32_t group_kind;      /**< #pf_native_group_e as accepted. */
    uint32_t group_effect;    /**< #pf_native_group_effect_e, 0 when no group. */
    uint32_t remaining_kind;  /**< #pf_native_remaining_e. */
    uint32_t trigger_state;   /**< #pf_native_trigger_state_e. */
    uint32_t origin;          /**< RequestOrigin: 0 host, 1 kernel liquidation, 2 kernel risk. */
    uint32_t reserved0;
    double   intent_value;    /**< The intent's own scalar, 0 when it has none. */
    double   p1;              /**< Trigger level 1 (limit / stop / trail offset). */
    double   p2;              /**< Trigger level 2 (stop-limit limit / trail arm). */
    double   capacity_units;  /**< Point budget, 0 for immediate capacity. */
    double   remaining_units; /**< Valid when `remaining_kind` is UNITS. */
    uint64_t group_id;
    int64_t  group_cohort;
    uint64_t acceptance_ordinal;      /**< Birth: the accepting event. */
    int64_t  decision_time_lower_bound; /**< Birth: earliest matchable time. */
    const char* label;        /**< Borrowed; see the struct note. */
    const char* comment;      /**< Borrowed; see the struct note. */
} pf_native_working_v1;

/** The run's lifecycle and its typed failure. */
typedef struct pf_native_state_v1 {
    uint32_t struct_size;      /**< sizeof(pf_native_state_v1). */
    uint32_t version;          /**< PF_NATIVE_API_VERSION. */
    uint32_t lifecycle;        /**< #pf_native_lifecycle_e. */
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

/** One order request. Translated field by field into `native_order::Request`;
 *  it is never cast. Zero-initialise it, set `struct_size` and `version`, then
 *  set only the fields the chosen `intent` and `trigger` document.
 *
 *  This struct has TWO published layouts and the runtime accepts either: the
 *  base layout the L13 lane first shipped, whose length is
 *  #PF_NATIVE_REQUEST_V1_BASE_SIZE, and the current one, which appends the
 *  L7b anchored-leg tail (`anchor_rounding`). A caller compiled against the
 *  base layout keeps working unchanged and simply gets the tail's defaults
 *  (RAW). Any other `struct_size` is PF_NATIVE_E_STRUCT. The tail is
 *  append-only: nothing above it moved. */
typedef struct pf_native_request_v1 {
    uint32_t struct_size;     /**< sizeof(pf_native_request_v1). */
    uint32_t version;         /**< PF_NATIVE_API_VERSION. */

    /* Intent */
    uint32_t intent;              /**< #pf_native_intent_e. */
    uint32_t reduce_size;         /**< #pf_native_reduction_e, REDUCE only. */
    uint32_t reduce_claim;        /**< #pf_native_scope_claim_e, SCOPE_FRACTION only. */
    uint32_t side;                /**< #pf_native_side_e, SIZED only. */
    uint32_t size_basis;          /**< #pf_native_size_basis_e, SIZED only. */
    uint32_t size_time;           /**< #pf_native_size_time_e, SIZED only. */
    uint32_t grid_policy;         /**< #pf_native_grid_policy_e, SIZED only. */
    uint32_t reserve_percent_fee; /**< 0/1, SIZED only. */
    double   intent_value;        /**< The intent's own scalar; see #pf_native_intent_e. */

    /* Trigger */
    uint32_t trigger;             /**< #pf_native_trigger_e. */
    uint32_t anchor;              /**< #pf_native_anchor_e (L7). */
    double   p1;                  /**< Limit/stop price, or trail offset. */
    double   p2;                  /**< Stop-limit limit, or trail arm price. */
    double   anchor_offset;       /**< FROM_OWNER_FILL: signed offset. */
    uint8_t  fill_through;        /**< LIMIT only: market-if-touched. */
    uint8_t  trail_offset_in_ticks;  /**< TRAIL: p1 is a tick count, not a distance. */
    uint8_t  trail_has_arm_price;    /**< TRAIL: p2 is the arm price. */
    uint8_t  anchor_offset_in_ticks; /**< FROM_OWNER_FILL: the offset is a tick count. */

    /* Capacity */
    uint32_t capacity;            /**< #pf_native_capacity_e. */
    double   capacity_units;      /**< POINT_BUDGET only. */

    /* Owner */
    uint32_t owner;               /**< #pf_native_owner_e. */
    uint32_t owner_n;             /**< Length of `owner_incarnations`. */
    const uint64_t* owner_incarnations; /**< Borrowed for the call only. */
    int64_t  owner_cycle;         /**< BIND_OPENING / BIND_OPENINGS. */
    uint64_t cohort;              /**< BIND_COHORT. */

    /* Group */
    uint32_t group_kind;          /**< #pf_native_group_e. */
    uint32_t group_effect;        /**< #pf_native_group_effect_e. */
    uint64_t group_id;            /**< MEMBER only. */
    int64_t  group_cohort;        /**< MEMBER only. */

    /* Identity. Both borrowed for the call only; the kernel copies them.
     * NULL is the empty string. */
    const char* label;
    const char* comment;

    /* ── The additive anchored-leg tail (L7b). Read only when `struct_size`
     * is the current sizeof; a caller sending the base layout stops at
     * `comment` above and gets every default (RAW, WORKING). ── */
    uint32_t anchor_rounding;     /**< #pf_native_anchor_rounding_e, FROM_OWNER_FILL only. */
    uint32_t visibility;          /**< #pf_native_arm_visibility_e, WAIT_FOR_APPLIED only. */
} pf_native_request_v1;

/** Byte length of #pf_native_request_v1 as the L13 lane first published it,
 *  before the anchored-leg tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds
 *  for — it is not a literal. The runtime accepts this length as well as the
 *  current `sizeof`, which is what makes the tail additive rather than a
 *  layout break. */
#define PF_NATIVE_REQUEST_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_request_v1, anchor_rounding))

/** One declared higher-timeframe series of #pf_native_run_spec_ext_v1.
 *
 *  A row is a series INSTANCE, not a period: several rows may carry the same
 *  `tf`, each delivered under its own index (the `subscription` argument of
 *  #pf_native_callbacks_v1::on_timeframe_bar). Same-period rows may not carry
 *  DIFFERENT `authoritative_bars`.
 *
 *  `gaps` occupies the word this struct published as `reserved0`, which every
 *  layout required to be zero — so a caller that zero-fills the struct keeps
 *  barmerge.gaps_off, and the struct's size and field offsets are unchanged.
 *  Any value but 0 or 1 is PF_NATIVE_E_TAG. */
typedef struct pf_native_subscription_v1 {
    uint32_t struct_size;   /**< sizeof(pf_native_subscription_v1). */
    uint32_t lookahead;     /**< 0 = barmerge.lookahead_off, 1 = lookahead_on. */
    const char* tf;         /**< Non-NULL timeframe literal. */
    const pf_bar_t* authoritative_bars; /**< Optional exchange bars; copied. */
    int32_t authoritative_n;            /**< Length of `authoritative_bars`. */
    uint32_t gaps;          /**< 0 = barmerge.gaps_off, 1 = gaps_on. */
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
 *  Not represented in ext v1: the intrabar path, the slot-label policy, legacy
 *  tolerance, the forced path order and abort reporting. They keep their
 *  defaults.
 *
 *  This struct has TWO published layouts and the runtime accepts either: the
 *  base layout the L13 lane first shipped, whose length is
 *  #PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE, and the current one, which appends
 *  the `risk_*` tail L9's limits need. A caller compiled against the base
 *  layout keeps working unchanged and simply cannot set
 *  #PF_NATIVE_SPEC_EXT_RISK. Any other `struct_size` is PF_NATIVE_E_STRUCT.
 *  The tail is append-only: nothing above it moved. */
typedef struct pf_native_run_spec_ext_v1 {
    uint32_t struct_size;    /**< sizeof(pf_native_run_spec_ext_v1). */
    uint32_t version;        /**< PF_NATIVE_API_VERSION. */
    uint32_t present_mask;   /**< #pf_native_spec_ext_mask_e bits. */

    uint32_t report_policy;               /**< NativeReportPolicy. */
    uint32_t report_open_position_at_end; /**< 0/1; KernelRecorded only. */

    uint32_t price_grid;      /**< NativePriceGrid. */
    uint32_t grid_rounding;   /**< NativeGridRounding. */

    uint32_t calculation;                  /**< NativeCalculationTrigger. */
    uint32_t max_recalculations_per_point;  /**< Fill-cascade bound; 0 is legal. */

    uint32_t open_bar_view;   /**< NativeOpenBarView. */

    uint32_t margin_sizing;   /**< NativeLiquidationSizing. */
    uint32_t margin_check;    /**< NativeLiquidationCheck. */
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
    double   margin_shortfall_multiple;
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
    uint32_t risk_day_basis;                 /**< #pf_native_risk_day_e. */
    uint32_t risk_action;                    /**< #pf_native_risk_action_e. */
} pf_native_run_spec_ext_v1;

/** Byte length of #pf_native_run_spec_ext_v1 as the L13 lane first published
 *  it, before the `risk_*` tail was appended. It is the offset of the first
 *  appended field, so it stays correct on every target this header builds for
 *  — it is not a literal. The runtime accepts this length as well as the
 *  current `sizeof`, which is what makes the tail additive rather than a
 *  layout break. */
#define PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE \
    ((uint32_t)offsetof(pf_native_run_spec_ext_v1, risk_has_max_drawdown))

/** The C host's strategy logic.
 *
 *  Every entry may be NULL, which is exactly the C++ default: the kernel does
 *  nothing for that hook. `user` is handed back unchanged to every callback.
 *  A callback returns 0 to continue; any other value ends the run Failed with
 *  PF_NATIVE_FAILURE_CALLBACK. A callback must never let an exception, a
 *  longjmp or any other non-local exit escape.
 *
 *  Commands are legal inside `on_bar_open`, `on_bar`, `on_tick` and
 *  `on_applied`. `on_run_begin`, `on_input`, `on_timeframe_bar` and
 *  `on_margin_call` are observation-only: a command there answers
 *  PF_NATIVE_E_STATE and changes nothing.
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
} pf_native_callbacks_v1;

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
 *  @param incarnation  Optional; receives the accepted request's handle.
 *  @param reject       Optional; receives a RequestRejectReason on rejection.
 *  @return PF_NATIVE_OK when accepted, PF_NATIVE_E_REJECTED when the kernel
 *  rejected it, PF_NATIVE_E_STATE when commands are not legal here. */
PF_API int strategy_native_submit_v1(pf_strategy_t s, const pf_native_request_v1* request,
                                     uint64_t* incarnation, uint32_t* reject);

/** Replace the live request @p incarnation with @p request.
 *
 *  @param successor  Optional; receives the successor's handle.
 *  @return PF_NATIVE_OK, PF_NATIVE_E_REJECTED, PF_NATIVE_E_NOT_WORKING,
 *  PF_NATIVE_E_INVALID_TARGET, or PF_NATIVE_E_STATE. */
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

/** Execute one live request at the current execution point.
 *
 *  @param price_rule  #pf_native_price_rule_e.
 *  @param refusal     Optional; receives a #pf_native_refusal_e when the
 *                     return is PF_NATIVE_E_REFUSED.
 *  @return A non-negative #pf_native_execute_outcome_e, or a negative status. */
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
 *  @return PF_NATIVE_OK, PF_NATIVE_E_ARGUMENT for an out-of-range index,
 *  PF_NATIVE_E_STRUCT for a mis-sized row, or another negative status. */
PF_API int strategy_native_working_get_v1(pf_strategy_t s, int index,
                                          pf_native_working_v1* out);

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

/** @} */ /* end of pf_native_c_api */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PINEFORGE_NATIVE_C_API_H */
