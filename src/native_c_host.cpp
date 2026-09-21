/*
 * native_c_host.cpp — runtime side of <pineforge/native_c_api.h> (R5 lane L13).
 *
 * One kernel translation unit. It holds:
 *   - CCallbackHost, a `final` NativeStrategyHost that forwards each existing
 *     virtual to a C callback table. No new virtual is introduced and no
 *     inline epoch moves: this is additive C transport over the kernel that
 *     lanes L2-L8 already built.
 *   - Field-by-field translation between the C PODs and the C++ values. A C
 *     request is never cast onto `native_order::Request`; every enum field is
 *     read by an exhaustive switch whose default refuses.
 *   - The PF_API definitions themselves. They are pinned by the
 *     EXPECTED_NATIVE_C_API inventory in scripts/check_c_abi_runtime.py, the
 *     separate half of the same guard that pins src/c_abi.cpp — adding or
 *     removing one here without updating that list fails CI.
 *
 * Two boundary rules are load-bearing and are implemented here, not hoped for:
 *   - No C++ exception leaves a PF_API function. The kernel's own command
 *     surface throws when a command is illegal at the current point
 *     (native_execution_consumer.cpp `commands_allowed`); that throw is
 *     caught here and answered as PF_NATIVE_E_STATE, so a command issued from
 *     a C callback can never unwind through the callback and kill the run.
 *   - A C callback that returns non-zero raises CallbackFailure, a
 *     std::runtime_error the consumer's existing callback guard converts into
 *     NativeFailureCode::CallbackException. The C function itself never
 *     unwinds; the throw happens in the C++ virtual, above the C frame.
 */

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/bar.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using pineforge::Bar;
using pineforge::NativeStrategyHost;
namespace no = pineforge::native_order;

/* ── Enumerator pins ────────────────────────────────────────────────
 * The C values are the C++ alternatives' own integers. Translation still
 * goes through an exhaustive switch — these only keep the header honest. */

static_assert(PF_NATIVE_INTENT_FLATTEN == 0 && PF_NATIVE_INTENT_SIZED == 5,
              "pf_native_intent_e must span OrderIntent");
static_assert(std::variant_size_v<no::OrderIntent> == 6, "OrderIntent grew");
static_assert(std::variant_size_v<no::Trigger> == 5, "Trigger grew");
static_assert(std::variant_size_v<no::Owner> == 5, "Owner grew");
static_assert(std::variant_size_v<no::Capacity> == 2, "Capacity grew");
static_assert(std::variant_size_v<no::Group> == 2, "Group grew");
static_assert(std::variant_size_v<no::ReductionSize> == 3, "ReductionSize grew");
static_assert(std::variant_size_v<no::SizeBasis> == 2, "SizeBasis grew");
static_assert(std::variant_size_v<no::TriggerAnchor> == 2, "TriggerAnchor grew");
static_assert(std::variant_size_v<no::RemainingProjection> == 5, "RemainingProjection grew");
static_assert(std::variant_size_v<no::TriggerState> == 9, "TriggerState grew");
static_assert(std::variant_size_v<no::CommandEvent> == 19, "CommandEvent grew");
static_assert(PF_NATIVE_EVENT_MARGIN_CALL == 18 && PF_NATIVE_EVENT_RISK == 21,
              "the command-event tags must cover every CommandEvent alternative");
static_assert(static_cast<int>(no::RiskLimitKind::MaxFillsPerDay)
                  == PF_NATIVE_RISK_MAX_FILLS_PER_DAY, "RiskLimitKind drifted");
static_assert(static_cast<int>(pineforge::NativeRiskDay::CalendarDayInTimezone)
                  == PF_NATIVE_RISK_DAY_CALENDAR_TIMEZONE, "NativeRiskDay drifted");
static_assert(static_cast<int>(pineforge::NativeRiskAction::FlattenAndBlock)
                  == PF_NATIVE_RISK_FLATTEN_AND_BLOCK, "NativeRiskAction drifted");
/* The risk tail is append-only: the base layout must still end exactly where
 * PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE says, and the tail must be the twelve
 * fields below it and nothing else. */
static_assert(PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE
                  == PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE + 10u * sizeof(std::uint32_t)
                         + 2u * sizeof(double),
              "the pf_native_run_spec_ext_v1 risk tail moved");
/* So is the auxiliary-feed tail, last of the three: three pointers and two
 * words past the layout N8's intrabar / policy tail left. */
static_assert(sizeof(pf_native_run_spec_ext_v1)
                  == PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE + 3u * sizeof(void*)
                         + 2u * sizeof(std::uint32_t),
              "the pf_native_run_spec_ext_v1 auxiliary tail moved");
static_assert(static_cast<int>(pineforge::NativeSeriesSource::Input)
                      == PF_NATIVE_SERIES_SOURCE_INPUT
                  && static_cast<int>(pineforge::NativeSeriesSource::AuxiliaryFeed)
                         == PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED,
              "NativeSeriesSource drifted");
static_assert(static_cast<int>(pineforge::NativeFailureCode::CallbackException)
                  == PF_NATIVE_FAILURE_CALLBACK,
              "PF_NATIVE_FAILURE_CALLBACK must mirror NativeFailureCode::CallbackException");
static_assert(static_cast<int>(no::Side::Short) == PF_NATIVE_SIDE_SHORT, "Side drifted");
static_assert(static_cast<int>(no::NativeAnchorRounding::Raw) == PF_NATIVE_ANCHOR_ROUNDING_RAW
                  && static_cast<int>(no::NativeAnchorRounding::HalfUp)
                         == PF_NATIVE_ANCHOR_ROUNDING_HALF_UP
                  && static_cast<int>(no::NativeAnchorRounding::Directional)
                         == PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL,
              "NativeAnchorRounding drifted");
static_assert(static_cast<int>(no::NativeArmVisibility::Working)
                      == PF_NATIVE_ARM_VISIBILITY_WORKING
                  && static_cast<int>(no::NativeArmVisibility::PendingUntilArmed)
                         == PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED,
              "NativeArmVisibility drifted");
/* Both request tails are append-only: each published layout must still end
 * exactly where its size constant says, and each tail must be the two fields
 * below it and nothing else. */
static_assert(PF_NATIVE_REQUEST_V1_ANCHOR_SIZE
                  == PF_NATIVE_REQUEST_V1_BASE_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_request_v1 anchored-leg tail moved");
static_assert(sizeof(pf_native_request_v1)
                  == PF_NATIVE_REQUEST_V1_ANCHOR_SIZE + 2u * sizeof(std::uint32_t),
              "the pf_native_request_v1 sizing-detail tail moved");
static_assert(static_cast<int>(no::SizePrice::SignalOnTick)
                  == PF_NATIVE_SIZE_PRICE_SIGNAL_ON_TICK, "SizePrice drifted");
static_assert(static_cast<int>(no::ScopeBasis::AtAcceptance)
                  == PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE, "ScopeBasis drifted");
static_assert(static_cast<int>(pineforge::NativeLiquidationCheck::PathAdverseExtremeMark)
                  == PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK,
              "NativeLiquidationCheck drifted");
static_assert(static_cast<int>(pineforge::NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission)
                  == PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION,
              "NativeMarginEquityBasis drifted");
static_assert(static_cast<int>(pineforge::NativeLiquidationLevelBase::RealizedOnly)
                  == PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY, "NativeLiquidationLevelBase drifted");
static_assert(static_cast<int>(pineforge::NativeSlotLabelPolicy::FeedTolerant)
                  == PF_NATIVE_SLOT_LABEL_FEED_TOLERANT, "NativeSlotLabelPolicy drifted");
static_assert(static_cast<int>(pineforge::NativeFeedTolerance::WarmupNonNegativeOHLC)
                  == PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE, "NativeFeedTolerance drifted");
static_assert(static_cast<int>(pineforge::NativePathOrder::LowFirst)
                  == PF_NATIVE_PATH_ORDER_LOW_FIRST, "NativePathOrder drifted");
static_assert(static_cast<int>(pineforge::NativeAbortReporting::Quiet)
                  == PF_NATIVE_ABORT_QUIET, "NativeAbortReporting drifted");
static_assert(static_cast<int>(pineforge::IntrabarPath::SampleEligibility::DistributionSamples)
                  == PF_NATIVE_SAMPLE_DISTRIBUTION_SAMPLES, "SampleEligibility drifted");
static_assert(static_cast<int>(pineforge::MagnifierDistribution::BACK_LOADED)
                  == PF_MAGNIFIER_BACK_LOADED, "MagnifierDistribution drifted");
/* The N8 tail is append-only in the same way: fourteen words and four
 * pointers past the layout L9 left, and it ends where the auxiliary tail
 * behind it begins. */
static_assert(PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE
                  == PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE + 14u * sizeof(std::uint32_t)
                         + 4u * sizeof(const char*),
              "the pf_native_run_spec_ext_v1 intrabar/policy tail moved");
static_assert(static_cast<int>(no::SizeTime::AtAcceptance) == PF_NATIVE_SIZE_AT_ACCEPTANCE,
              "SizeTime drifted");
static_assert(static_cast<int>(no::ExecutionGridPolicy::ExplicitUnits)
                  == PF_NATIVE_GRID_EXPLICIT_UNITS, "ExecutionGridPolicy drifted");
static_assert(static_cast<int>(no::ScopeClaim::NetOfSiblings) == PF_NATIVE_SCOPE_NET_OF_SIBLINGS,
              "ScopeClaim drifted");
static_assert(static_cast<int>(no::GroupEffect::Reduce) == PF_NATIVE_GROUP_REDUCE,
              "GroupEffect drifted");
static_assert(static_cast<int>(pineforge::NativeCurrentPriceRule::NearestTick)
                  == PF_NATIVE_PRICE_NEAREST_TICK, "NativeCurrentPriceRule drifted");
static_assert(static_cast<int>(pineforge::NativeLifecycleKind::Failed)
                  == PF_NATIVE_LIFECYCLE_FAILED, "NativeLifecycleKind drifted");
static_assert(static_cast<int>(pineforge::NativeCurrentRefusal::ConfigurationMismatch)
                  == PF_NATIVE_REFUSAL_CONFIGURATION_MISMATCH, "NativeCurrentRefusal drifted");
static_assert(sizeof(pf_bar_t) == sizeof(Bar), "pf_bar_t / Bar size mismatch");
static_assert(static_cast<int>(pineforge::NativeCalculationReason::SubBar)
                  == PF_NATIVE_CALC_SUB_BAR, "NativeCalculationReason drifted");
static_assert(static_cast<int>(pineforge::NativeMarginCheckKind::Calculation)
                  == PF_NATIVE_MARGIN_CHECK_CALCULATION, "NativeMarginCheckKind drifted");
/* The hook tail is append-only: the base layout must still end exactly where
 * PF_NATIVE_CALLBACKS_V1_BASE_SIZE says, and the tail must be the six
 * function pointers below it and nothing else. */
static_assert(sizeof(pf_native_callbacks_v1)
                  == PF_NATIVE_CALLBACKS_V1_BASE_SIZE + 7u * sizeof(void (*)(void)),
              "the pf_native_callbacks_v1 hook tail moved");

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/* A C callback said "stop". The consumer's existing callback guard turns any
 * std::exception into NativeFailureCode::CallbackException, so this is how a
 * non-zero C return reaches the documented failure without the C frame ever
 * unwinding. */
class CallbackFailure : public std::runtime_error {
public:
    explicit CallbackFailure(const char* hook)
        : std::runtime_error(std::string("native C callback refused in ") + hook) {}
};

const pf_bar_t* as_c_bar(const Bar& bar) {
    return reinterpret_cast<const pf_bar_t*>(&bar);
}

/* Defined below, beside the other C++ value -> C POD translations; declared
 * here because the recalculation hook flattens an applied cause. */
pf_native_applied_v1 applied_pod(const no::ExecutionAppliedEvent& applied);

/* ── The host ───────────────────────────────────────────────────── */

class CCallbackHost final : public NativeStrategyHost {
public:
    explicit CCallbackHost(const pf_native_callbacks_v1& table) : table_(table) {}

    const pf_native_callbacks_v1& table() const noexcept { return table_; }

    std::vector<pineforge::NativeWorkingRequest>& working_cache() noexcept {
        return working_cache_;
    }

    /* N18: the open-lot snapshot the copy-out reads from. Host-side only,
     * like working_cache_: never durable engine state, never hashed. */
    std::vector<pineforge::NativeOpenLot>& open_lot_cache() noexcept {
        return open_lot_cache_;
    }

    pf_native_decision_v1 decision(const pineforge::NativeDecisionContext& ctx) const;

private:
    void on_native_run_begin() override {
        if (!table_.on_run_begin) return;
        if (table_.on_run_begin(table_.user) != 0) throw CallbackFailure("on_run_begin");
    }

    void on_native_input(const Bar& bar, const pineforge::NativeInputContext& ctx) override {
        if (!table_.on_input) return;
        if (table_.on_input(table_.user, as_c_bar(bar), ctx.input_index,
                            ctx.completes_script_interval ? 1 : 0) != 0) {
            throw CallbackFailure("on_input");
        }
    }

    void on_native_bar_open(const Bar& bar,
                            const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_bar_open) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_bar_open(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_bar_open");
        }
    }

    void on_native_bar(const Bar& bar, const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_bar) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_bar(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_bar");
        }
    }

    void on_native_tick(const Bar& bar, const pineforge::NativeTickContext& ctx) override {
        if (!table_.on_tick) return;
        const pf_native_decision_v1 at = decision(ctx.decision);
        if (table_.on_tick(table_.user, as_c_bar(bar), &at) != 0) {
            throw CallbackFailure("on_tick");
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& applied,
                           const pineforge::NativeDecisionContext& ctx) override;

    void on_native_timeframe_bar(const Bar& bar,
                                 const pineforge::NativeTimeframeBarContext& ctx) override {
        if (!table_.on_timeframe_bar) return;
        if (table_.on_timeframe_bar(table_.user, as_c_bar(bar),
                                    static_cast<std::uint32_t>(ctx.subscription),
                                    static_cast<std::uint32_t>(ctx.completion),
                                    ctx.delivered_at_ms) != 0) {
            throw CallbackFailure("on_timeframe_bar");
        }
    }

    void on_native_margin_call(const no::MarginCallEvent& call) override;

    void on_native_recalculate(const Bar& bar, const pineforge::NativeDecisionContext& ctx,
                               pineforge::NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        /* No hook installed is the established contract: the base forwards
         * every calculation to on_native_bar, which is table_.on_bar. */
        if (!table_.on_recalculate) {
            NativeStrategyHost::on_native_recalculate(bar, ctx, reason, cause);
            return;
        }
        const pf_native_decision_v1 at = decision(ctx);
        pf_native_applied_v1 cause_pod;
        const pf_native_applied_v1* cause_ptr = nullptr;
        if (cause) {
            cause_pod = applied_pod(*cause);
            cause_ptr = &cause_pod;
        }
        if (table_.on_recalculate(table_.user, as_c_bar(bar), &at,
                                  static_cast<std::uint32_t>(reason), cause_ptr) != 0) {
            throw CallbackFailure("on_recalculate");
        }
    }

    void on_native_sub_bar(const Bar& sub,
                           const pineforge::NativeDecisionContext& ctx) override {
        if (!table_.on_sub_bar) return;
        const pf_native_decision_v1 at = decision(ctx);
        if (table_.on_sub_bar(table_.user, as_c_bar(sub), &at) != 0) {
            throw CallbackFailure("on_sub_bar");
        }
    }

    /* The four answering hooks. They are consulted from kernel paths outside
     * the callback guard, so none of them can throw: the return value selects
     * whose answer is used and every value is in contract. */
    std::optional<pineforge::NativeMarginDecision> resolve_margin_requirement(
            const pineforge::NativeMarginRequirementView& view) const override {
        if (!table_.on_margin_requirement) return std::nullopt;
        pf_native_margin_view_v1 pod = margin_view_pod(view.kind, view.position, view.mark,
                                                       view.equity, view.required,
                                                       view.cursor, false);
        pf_native_margin_decision_v1 answer;
        std::memset(&answer, 0, sizeof(answer));
        answer.struct_size = static_cast<std::uint32_t>(sizeof(answer));
        answer.version = PF_NATIVE_API_VERSION;
        answer.required = view.required;
        answer.equity = view.equity;
        if (table_.on_margin_requirement(table_.user, &pod, &answer)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return std::nullopt;
        }
        pineforge::NativeMarginDecision decision_out;
        decision_out.required = answer.required;
        decision_out.equity = answer.equity;
        decision_out.force_breach = answer.force_breach != 0u;
        return decision_out;
    }

    bool margin_check_allowed(const pineforge::NativeMarginCheckPoint& point) const override {
        if (!table_.on_margin_check) return true;
        pf_native_margin_view_v1 pod = margin_view_pod(point.kind, point.position, point.mark,
                                                       0.0, 0.0, point.cursor,
                                                       point.liquidation_resting);
        std::int32_t allowed = 1;
        if (table_.on_margin_check(table_.user, &pod, &allowed) == PF_NATIVE_ANSWER_DEFAULT) {
            return true;
        }
        return allowed != 0;
    }

    std::optional<double> resolve_margin_call_units(
            const pineforge::NativeMarginCallView& view) const override {
        if (!table_.on_margin_call_units) return std::nullopt;
        pf_native_margin_view_v1 pod = margin_view_pod(
            pineforge::NativeMarginCheckKind::BarOpen, view.position, view.mark, view.equity,
            view.required, view.cursor, false);
        /* A call is not a check point: neither field is a fact here. */
        pod.kind = 0;
        double units = 0.0;
        if (table_.on_margin_call_units(table_.user, &pod, &units)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return std::nullopt;
        }
        return units;
    }

    no::ExecutionTerms resolve_execution_terms(
            const pineforge::NativeExecutionTermsFacts& facts) const override {
        const no::ExecutionTerms fallback{facts.default_resolved_price, std::nullopt,
                                          no::OpeningShape::Transact};
        if (!table_.on_close_units) return fallback;
        if (!facts.definition
            || !std::holds_alternative<no::HostSized>(facts.definition->request.intent)) {
            return fallback;
        }
        pf_native_close_view_v1 view;
        std::memset(&view, 0, sizeof(view));
        view.struct_size = static_cast<std::uint32_t>(sizeof(view));
        view.version = PF_NATIVE_API_VERSION;
        view.incarnation = facts.target.incarnation;
        view.scope_exposure_units = facts.scope_exposure_units;
        view.default_resolved_price = facts.default_resolved_price;
        view.position_units = facts.position.signed_units;
        view.cursor_ordinal = facts.cursor.point.ordinal;
        view.cursor_effective_time_ms = facts.cursor.point.effective_time_ms;
        view.is_buy = facts.is_buy ? 1u : 0u;
        double units = 0.0;
        if (table_.on_close_units(table_.user, &view, &units) == PF_NATIVE_ANSWER_DEFAULT) {
            return fallback;
        }
        return {facts.default_resolved_price, units, no::OpeningShape::Transact};
    }

    bool owns_lot_excursions() const noexcept override {
        return table_.on_lot_excursion != nullptr;
    }

    pineforge::ClosedLotExcursion closed_lot_excursion(
            const pineforge::ClosedLotExcursionFacts& facts) const override {
        if (!table_.on_lot_excursion) return {};
        pf_native_lot_excursion_v1 pod;
        std::memset(&pod, 0, sizeof(pod));
        pod.struct_size = static_cast<std::uint32_t>(sizeof(pod));
        pod.version = PF_NATIVE_API_VERSION;
        pod.entry_incarnation = facts.entry_incarnation;
        pod.entry_time_ms = facts.entry_time_ms;
        pod.entry_price = facts.entry_price;
        pod.lot_qty = facts.lot_qty;
        pod.closed_qty = facts.closed_qty;
        pod.fill_price = facts.fill_price;
        pod.carried_favorable = facts.carried_favorable;
        pod.carried_adverse = facts.carried_adverse;
        pod.entry_bar_index = facts.entry_bar_index;
        pod.exit_bar_index = facts.exit_bar_index;
        pod.is_long = facts.is_long ? 1u : 0u;
        pod.entry_bar_high_masked = facts.entry_bar_high_masked ? 1u : 0u;
        pod.entry_bar_low_masked = facts.entry_bar_low_masked ? 1u : 0u;
        double favorable = 0.0;
        double adverse = 0.0;
        if (table_.on_lot_excursion(table_.user, &pod, &favorable, &adverse)
            == PF_NATIVE_ANSWER_DEFAULT) {
            return {};
        }
        return pineforge::ClosedLotExcursion{favorable, adverse};
    }

    static pf_native_margin_view_v1 margin_view_pod(
            pineforge::NativeMarginCheckKind kind,
            const pineforge::NativePhysicalPosition& position, double mark, double equity,
            double required, const no::MatchCursor& cursor, bool liquidation_resting) {
        pf_native_margin_view_v1 out;
        std::memset(&out, 0, sizeof(out));
        out.struct_size = static_cast<std::uint32_t>(sizeof(out));
        out.version = PF_NATIVE_API_VERSION;
        out.kind = static_cast<std::uint32_t>(kind);
        out.liquidation_resting = liquidation_resting ? 1u : 0u;
        out.signed_units = position.signed_units;
        out.average_price = position.average_price;
        out.lot_count = static_cast<std::uint64_t>(position.lot_count);
        out.mark = mark;
        out.equity = equity;
        out.required = required;
        out.cursor_ordinal = cursor.point.ordinal;
        out.cursor_effective_time_ms = cursor.point.effective_time_ms;
        out.cursor_t = cursor.t;
        out.cursor_interval_index = cursor.point.interval_index;
        out.cursor_provenance = static_cast<std::uint8_t>(cursor.point.provenance);
        out.cursor_path_phase = static_cast<std::uint8_t>(cursor.point.path_phase);
        return out;
    }

    pf_native_callbacks_v1 table_{};
    std::vector<pineforge::NativeWorkingRequest> working_cache_;
    std::vector<pineforge::NativeOpenLot> open_lot_cache_;
};

pf_native_decision_v1 CCallbackHost::decision(
        const pineforge::NativeDecisionContext& ctx) const {
    pf_native_decision_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = ctx.coordinate.ordinal;
    out.interval_index = ctx.coordinate.interval_index;
    out.sub_index = ctx.sub_index;
    out.sub_count = ctx.sub_count;
    out.is_terminal_sub_bar = ctx.is_terminal_sub_bar ? 1 : 0;
    out.effective_time_ms = ctx.coordinate.effective_time_ms;
    out.script_bar_open_ms = ctx.script_bar_open_ms;
    out.sub_bar_open_ms = ctx.sub_bar_open_ms;
    out.decision_floor_ms = ctx.decision_floor_ms;
    out.provenance = static_cast<std::uint8_t>(ctx.coordinate.provenance);
    out.path_phase = static_cast<std::uint8_t>(ctx.coordinate.path_phase);
    out.completion = static_cast<std::uint8_t>(ctx.coordinate.completion);
    out.price = kNaN;
    if (const auto point = current_execution_point()) {
        out.price = point->price;
        out.quote_kind = static_cast<std::uint8_t>(point->quote_kind);
    }
    return out;
}

/* ── C++ value → C POD ──────────────────────────────────────────── */

void fill_cursor(pf_native_event_v1& out, const no::MatchCursor& cursor) {
    out.effective_time_ms = cursor.point.effective_time_ms;
    out.interval_index = cursor.point.interval_index;
    out.provenance = static_cast<std::uint8_t>(cursor.point.provenance);
    out.path_phase = static_cast<std::uint8_t>(cursor.point.path_phase);
}

pf_native_event_v1 blank_event(std::uint32_t kind, std::uint64_t ordinal) {
    pf_native_event_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.kind = kind;
    out.ordinal = ordinal;
    return out;
}

pf_native_event_v1 applied_event_pod(const no::ExecutionAppliedEvent& applied) {
    pf_native_event_v1 out = blank_event(PF_NATIVE_EVENT_APPLIED, applied.ordinal);
    out.incarnation = applied.handle().incarnation;
    out.raw_price = applied.raw_price;
    out.resolved_price = applied.resolved_price;
    out.price = applied.resolved_price;
    out.closed_units = applied.closed_units;
    out.opened_units = applied.opened_units;
    out.cycle_before = applied.cycle_before;
    out.cycle_after = applied.cycle_after;
    out.terminal = applied.terminal ? 1 : 0;
    if (applied.terminal_reason) {
        out.reason = static_cast<std::uint32_t>(*applied.terminal_reason);
    }
    fill_cursor(out, applied.cursor);
    return out;
}

pf_native_event_v1 margin_call_pod(const no::MarginCallEvent& call) {
    pf_native_event_v1 out = blank_event(PF_NATIVE_EVENT_MARGIN_CALL, call.ordinal);
    out.incarnation = call.handle().incarnation;
    out.reason = static_cast<std::uint32_t>(call.side);
    out.price = call.mark;
    out.resolved_price = call.mark;
    out.raw_price = call.liquidation_price;
    out.closed_units = call.units;
    out.opened_units = call.position_after;
    out.cycle_before = 0;
    out.cycle_after = 0;
    fill_cursor(out, call.cursor);
    return out;
}

pf_native_applied_v1 applied_pod(const no::ExecutionAppliedEvent& applied) {
    pf_native_applied_v1 out;
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = applied.ordinal;
    out.incarnation = applied.handle().incarnation;
    out.opened_lot_incarnation = applied.opened_lot_incarnation;
    out.raw_price = applied.raw_price;
    out.resolved_price = applied.resolved_price;
    out.closed_units = applied.closed_units;
    out.opened_units = applied.opened_units;
    out.filled_working = applied.filled_working;
    out.ticket = applied.current_ticket;
    out.cycle_before = applied.cycle_before;
    out.cycle_after = applied.cycle_after;
    out.terminal = applied.terminal ? 1 : 0;
    if (applied.terminal_reason) {
        out.has_terminal_reason = 1;
        out.terminal_reason = static_cast<std::uint8_t>(*applied.terminal_reason);
    }
    return out;
}

void CCallbackHost::on_native_applied(const no::ExecutionAppliedEvent& applied,
                                      const pineforge::NativeDecisionContext& ctx) {
    if (!table_.on_applied) return;
    const pf_native_applied_v1 pod = applied_pod(applied);
    const pf_native_decision_v1 at = decision(ctx);
    if (table_.on_applied(table_.user, &pod, &at) != 0) throw CallbackFailure("on_applied");
}

void CCallbackHost::on_native_margin_call(const no::MarginCallEvent& call) {
    if (!table_.on_margin_call) return;
    const pf_native_event_v1 pod = margin_call_pod(call);
    if (table_.on_margin_call(table_.user, &pod) != 0) throw CallbackFailure("on_margin_call");
}

/* One recorded event, flattened into the tagged POD. Every alternative is
 * named: a new CommandEvent alternative fails the static_assert above and
 * then this visit. */
bool translate_event(const pineforge::NativeMarketEvent& event, pf_native_event_v1& out) {
    switch (event.kind) {
    case pineforge::NativeEventKind::Driver: {
        if (!event.driver) return false;
        out = blank_event(PF_NATIVE_EVENT_DRIVER_POINT, event.ordinal);
        out.raw_price = event.driver->raw_price;
        out.price = event.driver->raw_price;
        out.effective_time_ms = event.driver->coordinate.effective_time_ms;
        out.interval_index = event.driver->coordinate.interval_index;
        out.provenance = static_cast<std::uint8_t>(event.driver->coordinate.provenance);
        out.path_phase = static_cast<std::uint8_t>(event.driver->coordinate.path_phase);
        return true;
    }
    case pineforge::NativeEventKind::Account: {
        if (!event.account) return false;
        out = blank_event(PF_NATIVE_EVENT_ACCOUNT, event.ordinal);
        out.price = event.account->marked_equity;
        out.raw_price = event.account->realized_balance;
        out.opened_units = event.account->signed_units;
        out.effective_time_ms = event.account->effective_time_ms;
        return true;
    }
    case pineforge::NativeEventKind::Command:
        break;
    }
    if (!event.command) return false;
    const std::uint64_t ordinal = event.ordinal;
    return std::visit([&](const auto& payload) -> bool {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, no::AcceptedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ACCEPTED, ordinal);
            out.incarnation = payload.handle().incarnation;
        } else if constexpr (std::is_same_v<T, no::RejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REJECTED, ordinal);
            out.reason = static_cast<std::uint32_t>(payload.reason);
        } else if constexpr (std::is_same_v<T, no::ReplacedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REPLACED, ordinal);
            out.incarnation = payload.predecessor().incarnation;
            out.successor = payload.successor().incarnation;
        } else if constexpr (std::is_same_v<T, no::ReplaceRejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_REPLACE_REJECTED, ordinal);
            out.incarnation = payload.target().incarnation;
            out.reason = static_cast<std::uint32_t>(payload.reason);
        } else if constexpr (std::is_same_v<T, no::CancelledEvent>) {
            out = blank_event(PF_NATIVE_EVENT_CANCELLED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.reason = static_cast<std::uint32_t>(payload.reason);
        } else if constexpr (std::is_same_v<T, no::NotWorkingEvent>) {
            out = blank_event(PF_NATIVE_EVENT_NOT_WORKING, ordinal);
            out.incarnation = payload.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::InvalidHandleEvent>) {
            out = blank_event(PF_NATIVE_EVENT_INVALID_HANDLE, ordinal);
            out.incarnation = payload.target.incarnation;
        } else if constexpr (std::is_same_v<T, no::NoEffectEvent>) {
            out = blank_event(PF_NATIVE_EVENT_NO_EFFECT, ordinal);
            out.incarnation = payload.handle().incarnation;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::MatchRejectedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_MATCH_REJECTED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.reason = static_cast<std::uint32_t>(payload.reason);
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ExecutionAppliedEvent>) {
            out = applied_event_pod(payload);
        } else if constexpr (std::is_same_v<T, no::CloseBoundEvent>) {
            out = blank_event(PF_NATIVE_EVENT_CLOSE_BOUND, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.reason = static_cast<std::uint32_t>(payload.side);
            out.cycle_after = payload.cycle;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ActivatedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ACTIVATED, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.reason = static_cast<std::uint32_t>(payload.kind);
            out.price = payload.reached_price;
            out.raw_price = payload.reached_price;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::ReservationReducedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_RESERVATION_REDUCED, ordinal);
            out.incarnation = payload.recipient.incarnation;
            out.reason = static_cast<std::uint32_t>(payload.effect);
            out.closed_units = payload.actual_deduction;
        } else if constexpr (std::is_same_v<T, no::DeferredGroupAdjustmentEvent>) {
            out = blank_event(PF_NATIVE_EVENT_DEFERRED_GROUP, ordinal);
            out.incarnation = payload.recipient.incarnation;
            out.reason = static_cast<std::uint32_t>(payload.effect);
            out.closed_units = payload.deferred_delta;
        } else if constexpr (std::is_same_v<T, no::QuantityBoundEvent>) {
            out = blank_event(PF_NATIVE_EVENT_QUANTITY_BOUND, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
            out.opened_units = payload.source_units;
        } else if constexpr (std::is_same_v<T, no::ArmedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_ARMED, ordinal);
            out.incarnation = payload.definition->handle.incarnation;
        } else if constexpr (std::is_same_v<T, no::TermsResolvedEvent>) {
            out = blank_event(PF_NATIVE_EVENT_TERMS_RESOLVED, ordinal);
            out.incarnation = payload.handle().incarnation;
            out.raw_price = payload.input.raw_price;
            out.resolved_price = payload.input.terms.resolved_price;
            out.price = payload.input.terms.resolved_price;
            fill_cursor(out, payload.cursor);
        } else if constexpr (std::is_same_v<T, no::MarginCallEvent>) {
            out = margin_call_pod(payload);
        } else if constexpr (std::is_same_v<T, no::NativeRiskEvent>) {
            /* An account fact, bound to no request: `incarnation` stays 0 and
             * the matching point travels in `successor` instead. `limit` is
             * already in the unit the breach was measured in — a percent
             * limit was resolved against its basis equity in the kernel. */
            out = blank_event(PF_NATIVE_EVENT_RISK, ordinal);
            out.reason = static_cast<std::uint32_t>(payload.kind);
            out.price = payload.observed;
            out.raw_price = payload.limit;
            out.cycle_before = payload.day_ordinal;
            out.successor = payload.cursor.point.ordinal;
            fill_cursor(out, payload.cursor);
        } else {
            static_assert(!sizeof(T), "untranslated native command event");
        }
        return true;
    }, *event.command);
}

/* ── C POD → C++ value ──────────────────────────────────────────── */

int translate_intent(const pf_native_request_v1& in, bool has_sizing_tail,
                     no::OrderIntent& out) {
    switch (in.intent) {
    case PF_NATIVE_INTENT_FLATTEN:
        out = no::Flatten{};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_TRANSACT:
        out = no::Transact{in.intent_value};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_REVERSE_TO:
        out = no::ReverseTo{in.intent_value};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_HOST_SIZED:
        /* HostSized is the adapter's sizing seam and a C host sizes with
         * SIZED -- with ONE exception the kernel leaves no other spelling
         * for. native_order.cpp accepts BindCohort only for
         * HostSized{Close}, and a cohort close has no quantity to write: the
         * roster's own live openings are the target, so the accepted request
         * carries NoTarget and the cohort authority decides the units.
         * Every other HostSized shape needs resolve_execution_terms to
         * answer the quantity, which this header does not expose, so it
         * stays unsupported rather than being accepted and then rejected at
         * the candidate. */
        if (in.owner != PF_NATIVE_OWNER_BIND_COHORT) return PF_NATIVE_E_UNSUPPORTED;
        out = no::HostSized{no::HostSizedKind::Close, std::nullopt};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTENT_REDUCE: {
        no::Reduce reduce{no::ExplicitUnits{in.intent_value}};
        switch (in.reduce_size) {
        case PF_NATIVE_REDUCE_EXPLICIT_UNITS:
            reduce.size = no::ExplicitUnits{in.intent_value};
            break;
        case PF_NATIVE_REDUCE_OWNER_OPENED:
            reduce.size = no::OwnerOpenedUnits{};
            break;
        case PF_NATIVE_REDUCE_SCOPE_FRACTION: {
            no::ScopeFraction fraction;
            fraction.fraction = in.intent_value;
            switch (in.reduce_claim) {
            case PF_NATIVE_SCOPE_GROSS: fraction.claim = no::ScopeClaim::Gross; break;
            case PF_NATIVE_SCOPE_NET_OF_SIBLINGS:
                fraction.claim = no::ScopeClaim::NetOfSiblings;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            if (has_sizing_tail) {
                switch (in.reduce_basis) {
                case PF_NATIVE_SCOPE_BASIS_AT_MATCH:
                    fraction.basis = no::ScopeBasis::AtMatch;
                    break;
                case PF_NATIVE_SCOPE_BASIS_AT_ACCEPTANCE:
                    fraction.basis = no::ScopeBasis::AtAcceptance;
                    break;
                default: return PF_NATIVE_E_TAG;
                }
            }
            reduce.size = fraction;
            break;
        }
        default: return PF_NATIVE_E_TAG;
        }
        out = reduce;
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_INTENT_SIZED: {
        no::Sized sized;
        switch (in.side) {
        case PF_NATIVE_SIDE_LONG: sized.side = no::Side::Long; break;
        case PF_NATIVE_SIDE_SHORT: sized.side = no::Side::Short; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.size_basis) {
        case PF_NATIVE_SIZE_BASIS_CASH: sized.basis = no::CashValue{in.intent_value}; break;
        case PF_NATIVE_SIZE_BASIS_EQUITY_FRACTION:
            sized.basis = no::EquityFraction{in.intent_value};
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.size_time) {
        case PF_NATIVE_SIZE_AT_MATCH: sized.time = no::SizeTime::AtMatch; break;
        case PF_NATIVE_SIZE_AT_ACCEPTANCE: sized.time = no::SizeTime::AtAcceptance; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.grid_policy) {
        case PF_NATIVE_GRID_SNAP:
            sized.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
            break;
        case PF_NATIVE_GRID_EXPLICIT_UNITS:
            sized.grid_policy = no::ExecutionGridPolicy::ExplicitUnits;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        if (has_sizing_tail) {
            switch (in.size_price) {
            case PF_NATIVE_SIZE_PRICE_RESOLVED: sized.price = no::SizePrice::Resolved; break;
            case PF_NATIVE_SIZE_PRICE_SIGNAL: sized.price = no::SizePrice::Signal; break;
            case PF_NATIVE_SIZE_PRICE_SIGNAL_ON_TICK:
                sized.price = no::SizePrice::SignalOnTick;
                break;
            default: return PF_NATIVE_E_TAG;
            }
        }
        if (in.reserve_percent_fee > 1u) return PF_NATIVE_E_TAG;
        sized.reserve_percent_fee = in.reserve_percent_fee != 0u;
        out = sized;
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_trigger(const pf_native_request_v1& in, no::Trigger& out) {
    if (in.fill_through > 1u || in.trail_offset_in_ticks > 1u
        || in.trail_has_arm_price > 1u) {
        return PF_NATIVE_E_TAG;
    }
    switch (in.trigger) {
    case PF_NATIVE_TRIGGER_MARKET:
        out = no::Market{};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_LIMIT:
        out = no::Limit{in.p1, in.fill_through != 0u};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_STOP:
        out = no::Stop{in.p1};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_STOP_LIMIT:
        out = no::StopLimit{in.p1, in.p2};
        return PF_NATIVE_OK;
    case PF_NATIVE_TRIGGER_TRAIL: {
        no::Trail trail;
        if (in.trail_offset_in_ticks != 0u) {
            trail.offset = 0.0;
            trail.ticks = no::TrailTicks{in.p1};
        } else {
            trail.offset = in.p1;
        }
        if (in.trail_has_arm_price != 0u) trail.arm_price = in.p2;
        out = trail;
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_owner(const pf_native_request_v1& in, const no::RunIdentity& run,
                    no::Owner& out) {
    const auto handle_at = [&](std::uint32_t index) {
        return no::RequestHandle{run, in.owner_incarnations[index]};
    };
    switch (in.owner) {
    case PF_NATIVE_OWNER_INDEPENDENT:
        out = no::Independent{};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_WAIT_FOR_APPLIED:
        if (!in.owner_incarnations || in.owner_n != 1u) return PF_NATIVE_E_ARGUMENT;
        out = no::WaitForApplied{handle_at(0)};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_BIND_OPENING:
        if (!in.owner_incarnations || in.owner_n != 1u) return PF_NATIVE_E_ARGUMENT;
        out = no::BindOpening{handle_at(0), in.owner_cycle};
        return PF_NATIVE_OK;
    case PF_NATIVE_OWNER_BIND_OPENINGS: {
        if (!in.owner_incarnations || in.owner_n == 0u) return PF_NATIVE_E_ARGUMENT;
        no::BindOpenings openings;
        openings.openings.reserve(in.owner_n);
        for (std::uint32_t i = 0; i < in.owner_n; ++i) openings.openings.push_back(handle_at(i));
        openings.cycle = in.owner_cycle;
        out = openings;
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_OWNER_BIND_COHORT:
        if (in.cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        out = no::BindCohort{no::CohortHandle{in.cohort}};
        return PF_NATIVE_OK;
    default:
        return PF_NATIVE_E_TAG;
    }
}

int translate_request(const pf_native_request_v1& in, const no::RunIdentity& run,
                      no::Request& out) {
    /* Three published layouts: the base one the L13 lane first shipped, that
     * plus L7b's anchored-leg tail, and the current one with L3b's sizing
     * detail. An earlier caller's later tails are never read; it gets their
     * defaults. */
    const bool has_sizing_tail = in.struct_size == sizeof(pf_native_request_v1);
    const bool has_anchor_tail =
        has_sizing_tail || in.struct_size == PF_NATIVE_REQUEST_V1_ANCHOR_SIZE;
    if ((!has_anchor_tail && in.struct_size != PF_NATIVE_REQUEST_V1_BASE_SIZE)
        || in.version != PF_NATIVE_API_VERSION) {
        return PF_NATIVE_E_STRUCT;
    }
    if (int rc = translate_intent(in, has_sizing_tail, out.intent); rc != PF_NATIVE_OK) {
        return rc;
    }
    if (int rc = translate_trigger(in, out.trigger); rc != PF_NATIVE_OK) return rc;
    if (int rc = translate_owner(in, run, out.owner); rc != PF_NATIVE_OK) return rc;

    switch (in.capacity) {
    case PF_NATIVE_CAPACITY_IMMEDIATE: out.capacity = no::ImmediateRemaining{}; break;
    case PF_NATIVE_CAPACITY_POINT_BUDGET:
        out.capacity = no::PointBudget{in.capacity_units};
        break;
    default: return PF_NATIVE_E_TAG;
    }

    switch (in.group_kind) {
    case PF_NATIVE_GROUP_NONE:
        out.group = no::NoGroup{};
        break;
    case PF_NATIVE_GROUP_MEMBER: {
        no::Member member;
        member.group = in.group_id;
        member.cohort = in.group_cohort;
        switch (in.group_effect) {
        case PF_NATIVE_GROUP_CANCEL: member.effect = no::GroupEffect::Cancel; break;
        case PF_NATIVE_GROUP_REDUCE: member.effect = no::GroupEffect::Reduce; break;
        default: return PF_NATIVE_E_TAG;
        }
        out.group = member;
        break;
    }
    default: return PF_NATIVE_E_TAG;
    }

    if (in.anchor_offset_in_ticks > 1u) return PF_NATIVE_E_TAG;
    no::NativeAnchorRounding rounding = no::NativeAnchorRounding::Raw;
    if (has_anchor_tail) {
        /* Visibility belongs to the one owner relation that arms; on any
         * other owner a non-default value is a tag error, not a silent drop. */
        auto* wait = std::get_if<no::WaitForApplied>(&out.owner);
        switch (in.visibility) {
        case PF_NATIVE_ARM_VISIBILITY_WORKING: break;
        case PF_NATIVE_ARM_VISIBILITY_PENDING_UNTIL_ARMED:
            if (!wait) return PF_NATIVE_E_TAG;
            wait->visibility = no::NativeArmVisibility::PendingUntilArmed;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (in.anchor_rounding) {
        case PF_NATIVE_ANCHOR_ROUNDING_RAW: break;
        case PF_NATIVE_ANCHOR_ROUNDING_HALF_UP:
            rounding = no::NativeAnchorRounding::HalfUp;
            break;
        case PF_NATIVE_ANCHOR_ROUNDING_DIRECTIONAL:
            rounding = no::NativeAnchorRounding::Directional;
            break;
        default: return PF_NATIVE_E_TAG;
        }
    }
    switch (in.anchor) {
    case PF_NATIVE_ANCHOR_ABSOLUTE:
        if (rounding != no::NativeAnchorRounding::Raw) return PF_NATIVE_E_TAG;
        out.anchor = no::Absolute{};
        break;
    case PF_NATIVE_ANCHOR_FROM_OWNER_FILL: {
        no::FromOwnerFill anchor;
        anchor.offset = in.anchor_offset;
        anchor.ticks = in.anchor_offset_in_ticks != 0u;
        anchor.rounding = rounding;
        out.anchor = anchor;
        break;
    }
    default: return PF_NATIVE_E_TAG;
    }

    out.label = in.label ? std::string(in.label) : std::string();
    out.comment = in.comment ? std::string(in.comment) : std::string();
    return PF_NATIVE_OK;
}

/* ── Handle plumbing ────────────────────────────────────────────── */

CCallbackHost* host_of(pf_strategy_t s) {
    if (!s) return nullptr;
    auto* engine = static_cast<pineforge::BacktestEngine*>(s);
    return dynamic_cast<CCallbackHost*>(engine);
}

/* The staged/running specification's identity, which every RequestHandle a C
 * caller names belongs to. Absent before the first configure. */
const no::RunIdentity* run_identity(const CCallbackHost& host) {
    const auto state = host.native_state();
    return state.spec ? &state.spec->identity : nullptr;
}

std::uint32_t working_intent_tag(const no::OrderIntent& intent, double& value) {
    value = 0.0;
    return std::visit([&](const auto& alternative) -> std::uint32_t {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, no::Flatten>) {
            return PF_NATIVE_INTENT_FLATTEN;
        } else if constexpr (std::is_same_v<T, no::Reduce>) {
            if (const auto* units = std::get_if<no::ExplicitUnits>(&alternative.size)) {
                value = units->units;
            } else if (const auto* fraction = std::get_if<no::ScopeFraction>(&alternative.size)) {
                value = fraction->fraction;
            }
            return PF_NATIVE_INTENT_REDUCE;
        } else if constexpr (std::is_same_v<T, no::Transact>) {
            value = alternative.signed_units;
            return PF_NATIVE_INTENT_TRANSACT;
        } else if constexpr (std::is_same_v<T, no::ReverseTo>) {
            value = alternative.signed_units;
            return PF_NATIVE_INTENT_REVERSE_TO;
        } else if constexpr (std::is_same_v<T, no::HostSized>) {
            return PF_NATIVE_INTENT_HOST_SIZED;
        } else {
            if (const auto* cash = std::get_if<no::CashValue>(&alternative.basis)) {
                value = cash->cash;
            } else {
                value = std::get<no::EquityFraction>(alternative.basis).fraction;
            }
            return PF_NATIVE_INTENT_SIZED;
        }
    }, intent);
}

std::uint32_t working_trigger_tag(const no::Trigger& trigger, double& p1, double& p2) {
    p1 = 0.0;
    p2 = 0.0;
    return std::visit([&](const auto& alternative) -> std::uint32_t {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, no::Market>) {
            return PF_NATIVE_TRIGGER_MARKET;
        } else if constexpr (std::is_same_v<T, no::Limit>) {
            p1 = alternative.price;
            return PF_NATIVE_TRIGGER_LIMIT;
        } else if constexpr (std::is_same_v<T, no::Stop>) {
            p1 = alternative.price;
            return PF_NATIVE_TRIGGER_STOP;
        } else if constexpr (std::is_same_v<T, no::StopLimit>) {
            p1 = alternative.stop;
            p2 = alternative.limit;
            return PF_NATIVE_TRIGGER_STOP_LIMIT;
        } else {
            p1 = alternative.offset;
            if (alternative.arm_price) p2 = *alternative.arm_price;
            return PF_NATIVE_TRIGGER_TRAIL;
        }
    }, trigger);
}

/* N18: one NativeOpenLot row into its C POD. The strings borrow the cached
 * snapshot exactly as fill_working's label/comment do. */
void fill_open_lot(const pineforge::NativeOpenLot& lot, pf_native_open_lot_v1& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    out.ordinal = static_cast<std::uint64_t>(lot.ordinal);
    out.entry_incarnation = lot.entry_incarnation;
    out.cycle = lot.cycle;
    out.side = static_cast<std::uint32_t>(lot.side);
    out.entry_bar_index = lot.entry_bar_index;
    out.entry_time_ms = lot.entry_time_ms;
    out.entry_price = lot.entry_price;
    out.signed_units = lot.signed_units;
    out.entry_commission = lot.entry_commission;
    out.mark = lot.mark;
    out.unrealized_pnl = lot.unrealized_pnl;
    out.favorable_excursion = lot.favorable_excursion;
    out.adverse_excursion = lot.adverse_excursion;
    out.entry_label = lot.entry_label.c_str();
    out.entry_comment = lot.entry_comment.c_str();
}

void fill_working(const pineforge::NativeWorkingRequest& live, pf_native_working_v1& out) {
    std::memset(&out, 0, sizeof(out));
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.version = PF_NATIVE_API_VERSION;
    const auto& definition = *live.definition;
    out.incarnation = definition.handle.incarnation;
    out.intent = working_intent_tag(definition.request.intent, out.intent_value);
    out.trigger = working_trigger_tag(definition.request.trigger, out.p1, out.p2);
    out.owner = static_cast<std::uint32_t>(definition.request.owner.index());
    out.capacity = static_cast<std::uint32_t>(definition.request.capacity.index());
    if (const auto* budget = std::get_if<no::PointBudget>(&definition.request.capacity)) {
        out.capacity_units = budget->units;
    }
    out.group_kind = static_cast<std::uint32_t>(definition.request.group.index());
    if (const auto* member = std::get_if<no::Member>(&definition.request.group)) {
        out.group_id = member->group;
        out.group_cohort = member->cohort;
        out.group_effect = static_cast<std::uint32_t>(member->effect);
    }
    out.remaining_kind = static_cast<std::uint32_t>(live.remaining.index());
    if (const auto* units = std::get_if<no::RemainingProjectionUnits>(&live.remaining)) {
        out.remaining_units = units->q;
    }
    out.trigger_state = static_cast<std::uint32_t>(live.trigger_state.index());
    out.origin = static_cast<std::uint32_t>(definition.origin);
    out.acceptance_ordinal = definition.birth.acceptance_ordinal;
    out.decision_time_lower_bound = definition.birth.decision_time_lower_bound;
    out.label = definition.request.label.c_str();
    out.comment = definition.request.comment.c_str();
}

/* Every PF_API body below runs inside one of these: a C++ exception must
 * never cross the boundary, and the kernel throws on an illegal command. */
template <typename Fn>
int guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return PF_NATIVE_E_EXCEPTION;
    } catch (const std::exception&) {
        return PF_NATIVE_E_STATE;
    } catch (...) {
        return PF_NATIVE_E_EXCEPTION;
    }
}

/* The v1 base specification, translated exactly as
 * strategy_configure_native_v1 translates it (src/c_abi.cpp). It is
 * duplicated rather than shared because that symbol both translates and
 * configures in one step, and this lane may not change its behaviour. */
int translate_base_spec(const pf_native_run_spec_v1& in, pineforge::NativeRunSpec& out) {
    if (!in.session_key || !in.input_tf || !in.script_tf || !in.ticker || !in.tickerid
        || !in.type || !in.currency || !in.basecurrency || !in.description
        || !in.volumetype || !in.timezone || !in.session || !in.chart_timezone) {
        return PF_NATIVE_E_ARGUMENT;
    }
    if (in.optional_mask & ~0xfu) return PF_NATIVE_E_TAG;
    out.identity.session_key = in.session_key;
    out.identity.run_number = in.run_number;
    out.input_tf = in.input_tf;
    out.script_tf = in.script_tf;
    out.ticker = in.ticker;
    out.tickerid = in.tickerid;
    out.type = in.type;
    out.currency = in.currency;
    out.basecurrency = in.basecurrency;
    out.description = in.description;
    out.volumetype = in.volumetype;
    out.timezone = in.timezone;
    out.session = in.session;
    out.chart_timezone = in.chart_timezone;
    out.initial_capital = in.initial_capital;
    out.point_value = in.point_value;
    out.account_fx = in.account_fx;
    out.price_tick = in.price_tick;
    out.slippage_ticks = in.slippage_ticks;
    switch (in.fee_kind) {
    case 0: out.fee_kind = pineforge::NativeFeeKind::Percent; break;
    case 1: out.fee_kind = pineforge::NativeFeeKind::CashPerUnit; break;
    case 2: out.fee_kind = pineforge::NativeFeeKind::CashPerExecution; break;
    default: return PF_NATIVE_E_TAG;
    }
    out.fee_value = in.fee_value;
    switch (in.close_execution) {
    case 0: out.close_execution = pineforge::NativeCloseExecution::NextEligiblePoint; break;
    case 1: out.close_execution = pineforge::NativeCloseExecution::AfterCalculation; break;
    default: return PF_NATIVE_E_TAG;
    }
    switch (in.allowed_open_directions) {
    case 0: out.allowed_open_directions = pineforge::NativeOpenDirections::None; break;
    case 1: out.allowed_open_directions = pineforge::NativeOpenDirections::Long; break;
    case 2: out.allowed_open_directions = pineforge::NativeOpenDirections::Short; break;
    case 3: out.allowed_open_directions = pineforge::NativeOpenDirections::Both; break;
    default: return PF_NATIVE_E_TAG;
    }
    if (in.optional_mask & 1u) out.quantity_grid = in.quantity_grid;
    if (in.optional_mask & 2u) out.max_abs_units = in.max_abs_units;
    if (in.optional_mask & 4u) out.initial_margin_fraction = in.initial_margin_fraction;
    if (in.optional_mask & 8u) out.max_open_lots = in.max_open_lots;
    return PF_NATIVE_OK;
}

/* `has_risk_tail` is false for a caller compiled against the base layout of
 * pf_native_run_spec_ext_v1 (PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE): its struct
 * stops at `reserved0`, so the risk fields must not be read at all.
 * `has_policy_tail` is false for that caller and for one compiled against the
 * risk layout, whose struct stops at `risk_action`. `has_auxiliary_tail` is
 * false for all three earlier layouts (the last of them stops at
 * `margin_liquidation_comment`): the auxiliary fields must not be read at
 * all either. */
/* The retained intrabar execution path. NONE keeps the whole default
 * surface; the two sampled alternatives share their sampler knobs and differ
 * in whether a finer feed is retained, which is also what decides whether
 * on_sub_bar is ever delivered. */
int translate_intrabar(const pf_native_run_spec_ext_v1& ext, pineforge::IntrabarPath& out) {
    if (ext.intrabar_volume_weighted > 1u) return PF_NATIVE_E_TAG;
    pineforge::MagnifierDistribution distribution;
    switch (ext.intrabar_distribution) {
    case PF_MAGNIFIER_UNIFORM: distribution = pineforge::MagnifierDistribution::UNIFORM; break;
    case PF_MAGNIFIER_COSINE: distribution = pineforge::MagnifierDistribution::COSINE; break;
    case PF_MAGNIFIER_TRIANGLE: distribution = pineforge::MagnifierDistribution::TRIANGLE; break;
    case PF_MAGNIFIER_ENDPOINTS:
        distribution = pineforge::MagnifierDistribution::ENDPOINTS;
        break;
    case PF_MAGNIFIER_FRONT_LOADED:
        distribution = pineforge::MagnifierDistribution::FRONT_LOADED;
        break;
    case PF_MAGNIFIER_BACK_LOADED:
        distribution = pineforge::MagnifierDistribution::BACK_LOADED;
        break;
    default: return PF_NATIVE_E_TAG;
    }
    switch (ext.intrabar_kind) {
    case PF_NATIVE_INTRABAR_NONE:
        out = pineforge::IntrabarPath{};
        return PF_NATIVE_OK;
    case PF_NATIVE_INTRABAR_SYNTHESIZED: {
        pineforge::IntrabarPath::synthesized path;
        path.samples = ext.intrabar_samples;
        path.distribution = distribution;
        path.volume_weighted = ext.intrabar_volume_weighted != 0u;
        path.volume_weighted_min_samples = ext.intrabar_volume_weighted_min_samples;
        path.volume_weighted_max_samples = ext.intrabar_volume_weighted_max_samples;
        out.value = std::move(path);
        return PF_NATIVE_OK;
    }
    case PF_NATIVE_INTRABAR_LOWER_TF: {
        if (!ext.intrabar_tf) return PF_NATIVE_E_ARGUMENT;
        if (ext.intrabar_n < 0 || (ext.intrabar_n > 0 && !ext.intrabar_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        pineforge::IntrabarPath::lower_tf path;
        path.tf = ext.intrabar_tf;
        const auto* bars = reinterpret_cast<const Bar*>(ext.intrabar_bars);
        path.bars.assign(bars, bars + ext.intrabar_n);
        path.samples = ext.intrabar_samples;
        path.distribution = distribution;
        path.volume_weighted = ext.intrabar_volume_weighted != 0u;
        path.volume_weighted_min_samples = ext.intrabar_volume_weighted_min_samples;
        path.volume_weighted_max_samples = ext.intrabar_volume_weighted_max_samples;
        switch (ext.intrabar_sample_eligibility) {
        case PF_NATIVE_SAMPLE_CONTINUOUS_SEGMENTS:
            path.sample_eligibility =
                pineforge::IntrabarPath::SampleEligibility::ContinuousSegments;
            break;
        case PF_NATIVE_SAMPLE_DISTRIBUTION_SAMPLES:
            path.sample_eligibility =
                pineforge::IntrabarPath::SampleEligibility::DistributionSamples;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        out.value = std::move(path);
        return PF_NATIVE_OK;
    }
    default:
        return PF_NATIVE_E_TAG;
    }
}

/* One subscription list, shared by the configure-time block and the
 * begin-time declaration so both read a row the same way. */
int translate_subscriptions(const pf_native_subscription_v1* rows, std::uint32_t n,
                            const std::uint32_t* sources,
                            std::vector<pineforge::NativeTimeframeSubscription>& out) {
    if (n > 0 && !rows) return PF_NATIVE_E_ARGUMENT;
    std::vector<pineforge::NativeTimeframeSubscription> subscriptions;
    subscriptions.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& row = rows[i];
        if (row.struct_size != sizeof(pf_native_subscription_v1)) return PF_NATIVE_E_STRUCT;
        if (!row.tf) return PF_NATIVE_E_ARGUMENT;
        if (row.lookahead > 1u) return PF_NATIVE_E_TAG;
        if (row.gaps > 1u) return PF_NATIVE_E_TAG;
        if (row.authoritative_n < 0
            || (row.authoritative_n > 0 && !row.authoritative_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        pineforge::NativeTimeframeSubscription subscription;
        subscription.tf = row.tf;
        subscription.lookahead = row.lookahead != 0u;
        subscription.gaps = row.gaps != 0u;
        /* The source column rides in the auxiliary tail, so a caller that
         * does not carry that tail declares input-built series only. */
        if (sources) {
            switch (sources[i]) {
            case PF_NATIVE_SERIES_SOURCE_INPUT:
                subscription.source = pineforge::NativeSeriesSource::Input;
                break;
            case PF_NATIVE_SERIES_SOURCE_AUXILIARY_FEED:
                subscription.source = pineforge::NativeSeriesSource::AuxiliaryFeed;
                break;
            default: return PF_NATIVE_E_TAG;
            }
        }
        const auto* bars = reinterpret_cast<const Bar*>(row.authoritative_bars);
        subscription.authoritative_bars.assign(bars, bars + row.authoritative_n);
        subscriptions.push_back(std::move(subscription));
    }
    out = std::move(subscriptions);
    return PF_NATIVE_OK;
}

int apply_spec_ext(pineforge::NativeRunSpec& spec, const pf_native_run_spec_ext_v1& ext,
                   bool has_risk_tail, bool has_policy_tail, bool has_auxiliary_tail) {
    if (ext.present_mask & ~0x3ffu) return PF_NATIVE_E_TAG;
    if ((ext.present_mask & PF_NATIVE_SPEC_EXT_RISK) && !has_risk_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if ((ext.present_mask & PF_NATIVE_SPEC_EXT_AUXILIARY_FEED) && !has_auxiliary_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_AUXILIARY_FEED) {
        if (!ext.auxiliary_tf || ext.auxiliary_n < 0
            || (ext.auxiliary_n > 0 && !ext.auxiliary_bars)) {
            return PF_NATIVE_E_ARGUMENT;
        }
        if (ext.reserved1 != 0u) return PF_NATIVE_E_TAG;
        pineforge::NativeAuxiliaryFeed feed;
        feed.tf = ext.auxiliary_tf;
        const auto* bars = reinterpret_cast<const Bar*>(ext.auxiliary_bars);
        feed.bars.assign(bars, bars + ext.auxiliary_n);
        spec.auxiliary_feed = std::move(feed);
    }

    if (ext.present_mask & PF_NATIVE_SPEC_EXT_REPORT) {
        switch (ext.report_policy) {
        case 0: spec.report_policy = pineforge::NativeReportPolicy::HostRecorded; break;
        case 1: spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded; break;
        default: return PF_NATIVE_E_TAG;
        }
        if (ext.report_open_position_at_end > 1u) return PF_NATIVE_E_TAG;
        spec.report_open_position_at_end = ext.report_open_position_at_end != 0u;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_PRICE_GRID) {
        switch (ext.price_grid) {
        case 0: spec.price_grid = pineforge::NativePriceGrid::None; break;
        case 1: spec.price_grid = pineforge::NativePriceGrid::QuantizeFills; break;
        case 2: spec.price_grid = pineforge::NativePriceGrid::QuantizeFillsAndTriggers; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (ext.grid_rounding) {
        case 0: spec.grid_rounding = pineforge::NativeGridRounding::HalfUp; break;
        case 1: spec.grid_rounding = pineforge::NativeGridRounding::Directional; break;
        default: return PF_NATIVE_E_TAG;
        }
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_CALCULATION) {
        switch (ext.calculation) {
        case 0: spec.calculation = pineforge::NativeCalculationTrigger::BarClose; break;
        case 1: spec.calculation = pineforge::NativeCalculationTrigger::BarCloseAndFills; break;
        case 2: spec.calculation = pineforge::NativeCalculationTrigger::EveryModeledPoint; break;
        default: return PF_NATIVE_E_TAG;
        }
        spec.max_recalculations_per_point = ext.max_recalculations_per_point;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_OPEN_BAR_VIEW) {
        switch (ext.open_bar_view) {
        case 0: spec.open_bar_view = pineforge::NativeOpenBarView::Complete; break;
        case 1: spec.open_bar_view = pineforge::NativeOpenBarView::OpenOnly; break;
        default: return PF_NATIVE_E_TAG;
        }
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_MARGIN) {
        pineforge::NativeMarginModel margin;
        margin.initial_long = ext.margin_initial_long;
        margin.initial_short = ext.margin_initial_short;
        if (ext.margin_has_maintenance_long > 1u || ext.margin_has_maintenance_short > 1u
            || ext.margin_has_min_units > 1u) {
            return PF_NATIVE_E_TAG;
        }
        if (ext.margin_has_maintenance_long) margin.maintenance_long = ext.margin_maintenance_long;
        if (ext.margin_has_maintenance_short) {
            margin.maintenance_short = ext.margin_maintenance_short;
        }
        switch (ext.margin_sizing) {
        case 0: margin.sizing = pineforge::NativeLiquidationSizing::RestoreMinimum; break;
        case 1: margin.sizing = pineforge::NativeLiquidationSizing::ShortfallMultiple; break;
        case 2: margin.sizing = pineforge::NativeLiquidationSizing::Flatten; break;
        default: return PF_NATIVE_E_TAG;
        }
        margin.shortfall_multiple = ext.margin_shortfall_multiple;
        if (ext.margin_has_min_units) margin.liquidation_min_units = ext.margin_min_units;
        switch (ext.margin_check) {
        case PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME:
            margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtreme;
            break;
        case PF_NATIVE_LIQUIDATION_CALCULATION_ONLY:
            margin.check = pineforge::NativeLiquidationCheck::CalculationOnly;
            break;
        case PF_NATIVE_LIQUIDATION_PATH_ADVERSE_EXTREME_MARK:
            margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtremeMark;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        if (has_policy_tail) {
            switch (ext.margin_equity_basis) {
            case PF_NATIVE_MARGIN_EQUITY_MARKED:
                margin.basis = pineforge::NativeMarginEquityBasis::MarkedEquity;
                break;
            case PF_NATIVE_MARGIN_EQUITY_BEFORE_OPEN_COMMISSION:
                margin.basis =
                    pineforge::NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            switch (ext.margin_level_base) {
            case PF_NATIVE_MARGIN_LEVEL_MARKED_EQUITY:
                margin.level_base = pineforge::NativeLiquidationLevelBase::MarkedEquity;
                break;
            case PF_NATIVE_MARGIN_LEVEL_REALIZED_ONLY:
                margin.level_base = pineforge::NativeLiquidationLevelBase::RealizedOnly;
                break;
            default: return PF_NATIVE_E_TAG;
            }
            if (ext.margin_liquidation_label) margin.liquidation_label = ext.margin_liquidation_label;
            if (ext.margin_liquidation_comment) {
                margin.liquidation_comment = ext.margin_liquidation_comment;
            }
        }
        spec.margin = margin;
    }
    if ((ext.present_mask & (PF_NATIVE_SPEC_EXT_INTRABAR | PF_NATIVE_SPEC_EXT_FEED_POLICY))
        && !has_policy_tail) {
        return PF_NATIVE_E_STRUCT;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_RISK) {
        pineforge::NativeRiskLimits risk;
        if (ext.risk_has_max_drawdown > 1u || ext.risk_max_drawdown_percent > 1u
            || ext.risk_has_max_intraday_loss > 1u || ext.risk_max_intraday_loss_percent > 1u
            || ext.risk_has_max_consecutive_loss_days > 1u
            || ext.risk_has_max_fills_per_day > 1u) {
            return PF_NATIVE_E_TAG;
        }
        if (ext.risk_has_max_drawdown) {
            pineforge::NativeLossLimit limit;
            limit.value = ext.risk_max_drawdown;
            limit.percent = ext.risk_max_drawdown_percent != 0u;
            risk.max_drawdown = limit;
        }
        if (ext.risk_has_max_intraday_loss) {
            pineforge::NativeLossLimit limit;
            limit.value = ext.risk_max_intraday_loss;
            limit.percent = ext.risk_max_intraday_loss_percent != 0u;
            risk.max_intraday_loss = limit;
        }
        if (ext.risk_has_max_consecutive_loss_days) {
            risk.max_consecutive_loss_days = ext.risk_max_consecutive_loss_days;
        }
        if (ext.risk_has_max_fills_per_day) {
            risk.max_fills_per_day = ext.risk_max_fills_per_day;
        }
        switch (ext.risk_day_basis) {
        case 0: risk.day_basis = pineforge::NativeRiskDay::SessionDay; break;
        case 1: risk.day_basis = pineforge::NativeRiskDay::CalendarDayInTimezone; break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (ext.risk_action) {
        case 0: risk.action = pineforge::NativeRiskAction::BlockOpenings; break;
        case 1: risk.action = pineforge::NativeRiskAction::FlattenAndBlock; break;
        default: return PF_NATIVE_E_TAG;
        }
        spec.risk = risk;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_SUBSCRIPTIONS) {
        if (int rc = translate_subscriptions(
                ext.subscriptions, ext.subscriptions_n,
                has_auxiliary_tail ? ext.subscription_sources : nullptr, spec.subscriptions);
            rc != PF_NATIVE_OK) {
            return rc;
        }
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_INTRABAR) {
        if (int rc = translate_intrabar(ext, spec.intrabar); rc != PF_NATIVE_OK) return rc;
    }
    if (ext.present_mask & PF_NATIVE_SPEC_EXT_FEED_POLICY) {
        switch (ext.slot_label_policy) {
        case PF_NATIVE_SLOT_LABEL_CANONICAL:
            spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::Canonical;
            break;
        case PF_NATIVE_SLOT_LABEL_FEED_TOLERANT:
            spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        /* A bit mask, not an enumerator: any bit outside the published set
         * is a value this version cannot represent. */
        constexpr std::uint32_t kTolerance = PF_NATIVE_FEED_TOLERANCE_BATCH_STRUCTURAL
                                           | PF_NATIVE_FEED_TOLERANCE_WARMUP_NONNEGATIVE;
        if (ext.feed_tolerance & ~kTolerance) return PF_NATIVE_E_TAG;
        spec.legacy_tolerance = static_cast<pineforge::NativeFeedTolerance>(ext.feed_tolerance);
        switch (ext.path_order) {
        case PF_NATIVE_PATH_ORDER_AUTO: spec.path_order = pineforge::NativePathOrder::Auto; break;
        case PF_NATIVE_PATH_ORDER_HIGH_FIRST:
            spec.path_order = pineforge::NativePathOrder::HighFirst;
            break;
        case PF_NATIVE_PATH_ORDER_LOW_FIRST:
            spec.path_order = pineforge::NativePathOrder::LowFirst;
            break;
        default: return PF_NATIVE_E_TAG;
        }
        switch (ext.abort_reporting) {
        case PF_NATIVE_ABORT_ERROR:
            spec.abort_reporting = pineforge::NativeAbortReporting::Error;
            break;
        case PF_NATIVE_ABORT_QUIET:
            spec.abort_reporting = pineforge::NativeAbortReporting::Quiet;
            break;
        default: return PF_NATIVE_E_TAG;
        }
    }
    return PF_NATIVE_OK;
}

}  // namespace

extern "C" {

PF_API int strategy_native_api_version(void) { return PF_NATIVE_API_VERSION; }

PF_API pf_strategy_t strategy_native_host_create_v1(const pf_native_callbacks_v1* callbacks) {
    try {
        if (!callbacks) return nullptr;
        /* Two published layouts, and only two: the base one the lane first
         * shipped and the current one with the six-hook tail. A base-sized
         * caller's tail is never read; it is zero-filled here, which is
         * exactly "no hook installed". */
        const bool has_hook_tail = callbacks->struct_size == sizeof(pf_native_callbacks_v1);
        if (!has_hook_tail && callbacks->struct_size != PF_NATIVE_CALLBACKS_V1_BASE_SIZE) {
            return nullptr;
        }
        if (callbacks->version != PF_NATIVE_API_VERSION) return nullptr;
        pf_native_callbacks_v1 table;
        std::memset(&table, 0, sizeof(table));
        std::memcpy(&table, callbacks, callbacks->struct_size);
        /* The retained copy is always the current layout. */
        table.struct_size = static_cast<std::uint32_t>(sizeof(table));
        auto host = std::make_unique<CCallbackHost>(table);
        /* Convert through the base the rest of the C ABI casts back to, so
         * `static_cast<BacktestEngine*>(handle)` in c_abi.cpp is exact. */
        auto* engine = static_cast<pineforge::BacktestEngine*>(host.release());
        return static_cast<pf_strategy_t>(engine);
    } catch (...) {
        return nullptr;
    }
}

PF_API void strategy_native_host_free(pf_strategy_t s) {
    try {
        if (!s) return;
        auto* host = host_of(s);
        if (!host) return;  /* not ours: freeing it would be a type error */
        delete host;
    } catch (...) {
    }
}

PF_API int strategy_native_run_v1(pf_strategy_t s, const pf_bar_t* bars, int n,
                                  pf_report_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !bars)) return PF_NATIVE_E_ARGUMENT;
        host->run(reinterpret_cast<const Bar*>(bars), n);
        if (out) host->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
        return host->native_state().kind == pineforge::NativeLifecycleKind::Completed
            ? PF_NATIVE_OK
            : PF_NATIVE_E_RUN_FAILED;
    });
}

PF_API void strategy_native_report_free_v1(pf_report_t* report) {
    try {
        if (!report) return;
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
    } catch (...) {
    }
}

PF_API int strategy_native_submit_v1(pf_strategy_t s, const pf_native_request_v1* request,
                                     uint64_t* incarnation, uint32_t* reject) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!request) return PF_NATIVE_E_ARGUMENT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        no::Request translated;
        if (int rc = translate_request(*request, *run, translated); rc != PF_NATIVE_OK) return rc;
        const auto result = host->submit(translated);
        if (result.status == no::SubmitStatus::Accepted) {
            if (incarnation && result.handle) *incarnation = result.handle->incarnation;
            return PF_NATIVE_OK;
        }
        if (reject && result.reason) *reject = static_cast<std::uint32_t>(*result.reason);
        return PF_NATIVE_E_REJECTED;
    });
}

PF_API int strategy_native_replace_v1(pf_strategy_t s, uint64_t incarnation,
                                      const pf_native_request_v1* request,
                                      uint64_t* successor) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!request) return PF_NATIVE_E_ARGUMENT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        no::Request translated;
        if (int rc = translate_request(*request, *run, translated); rc != PF_NATIVE_OK) return rc;
        const auto result = host->replace(no::RequestHandle{*run, incarnation}, translated);
        switch (result.status) {
        case no::ReplaceStatus::Replaced:
            if (successor && result.successor) *successor = result.successor->incarnation;
            return PF_NATIVE_OK;
        case no::ReplaceStatus::ReplaceRejected:
            return PF_NATIVE_E_REJECTED;
        case no::ReplaceStatus::NotWorking:
            return PF_NATIVE_E_NOT_WORKING;
        case no::ReplaceStatus::InvalidHandle:
            return PF_NATIVE_E_INVALID_TARGET;
        }
        return PF_NATIVE_E_EXCEPTION;
    });
}

PF_API int strategy_native_cancel_v1(pf_strategy_t s, uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        const auto result = host->cancel(no::RequestHandle{*run, incarnation});
        switch (result.status) {
        case no::CancelStatus::Cancelled: return PF_NATIVE_OK;
        case no::CancelStatus::NotWorking: return PF_NATIVE_E_NOT_WORKING;
        case no::CancelStatus::InvalidHandle: return PF_NATIVE_E_INVALID_TARGET;
        }
        return PF_NATIVE_E_EXCEPTION;
    });
}

PF_API int strategy_native_cancel_all_v1(pf_strategy_t s) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        return static_cast<int>(host->cancel_all());
    });
}

PF_API int strategy_native_cancel_where_v1(pf_strategy_t s, const char* text, uint32_t field) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        // NULL is not the empty string here: "" is the text every request
        // that carries no comment or label matches, so a caller that meant
        // one and passed the other would cancel a different set.
        if (!text) return PF_NATIVE_E_ARGUMENT;
        pineforge::NativeRequestField selector = pineforge::NativeRequestField::Comment;
        switch (field) {
        case PF_NATIVE_FIELD_COMMENT:
            selector = pineforge::NativeRequestField::Comment;
            break;
        case PF_NATIVE_FIELD_LABEL:
            selector = pineforge::NativeRequestField::Label;
            break;
        default:
            return PF_NATIVE_E_TAG;
        }
        return static_cast<int>(host->cancel_where(std::string_view(text), selector));
    });
}

PF_API int strategy_native_execute_current_v1(pf_strategy_t s, uint64_t incarnation,
                                              uint32_t price_rule, uint32_t* refusal) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        pineforge::NativeCurrentExecution selection;
        selection.target = no::RequestHandle{*run, incarnation};
        switch (price_rule) {
        case PF_NATIVE_PRICE_AS_PRESENTED:
            selection.price_rule = pineforge::NativeCurrentPriceRule::AsPresented;
            break;
        case PF_NATIVE_PRICE_NEAREST_TICK:
            selection.price_rule = pineforge::NativeCurrentPriceRule::NearestTick;
            break;
        default:
            return PF_NATIVE_E_TAG;
        }
        const auto result = host->execute_current(selection);
        return std::visit([&](const auto& outcome) -> int {
            using T = std::decay_t<decltype(outcome)>;
            if constexpr (std::is_same_v<T, pineforge::NativeCurrentRefusal>) {
                if (refusal) *refusal = static_cast<std::uint32_t>(outcome);
                return PF_NATIVE_E_REFUSED;
            } else if constexpr (std::is_same_v<T, no::ExecutionAppliedEvent>) {
                return PF_NATIVE_EXECUTED_APPLIED;
            } else if constexpr (std::is_same_v<T, no::NoEffectEvent>) {
                return PF_NATIVE_EXECUTED_NO_EFFECT;
            } else if constexpr (std::is_same_v<T, no::MatchRejectedEvent>) {
                return PF_NATIVE_EXECUTED_MATCH_REJECTED;
            } else {
                return PF_NATIVE_EXECUTED_CANCELLED;
            }
        }, result);
    });
}

PF_API int strategy_native_position_v1(pf_strategy_t s, double* signed_units,
                                       double* average_price, uint64_t* lots) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto position = host->physical_position();
        if (signed_units) *signed_units = position.signed_units;
        if (average_price) *average_price = position.average_price;
        if (lots) *lots = static_cast<std::uint64_t>(position.lot_count);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_working_len_v1(pf_strategy_t s) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        host->working_cache() = host->native_working_requests();
        return static_cast<int>(host->working_cache().size());
    });
}

PF_API int strategy_native_working_get_v1(pf_strategy_t s, int index,
                                          pf_native_working_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_working_v1)) return PF_NATIVE_E_STRUCT;
        const auto& cache = host->working_cache();
        if (index < 0 || static_cast<std::size_t>(index) >= cache.size()) {
            return PF_NATIVE_E_ARGUMENT;
        }
        fill_working(cache[static_cast<std::size_t>(index)], *out);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_open_lot_count_v1(pf_strategy_t s, double mark) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        host->open_lot_cache() = host->native_open_lots(mark);
        return static_cast<int>(host->open_lot_cache().size());
    });
}

PF_API int strategy_native_open_lot_get_v1(pf_strategy_t s, int index,
                                           pf_native_open_lot_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_open_lot_v1)) return PF_NATIVE_E_STRUCT;
        const auto& cache = host->open_lot_cache();
        if (index < 0 || static_cast<std::size_t>(index) >= cache.size()) {
            return PF_NATIVE_E_ARGUMENT;
        }
        fill_open_lot(cache[static_cast<std::size_t>(index)], *out);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_events_v1(pf_strategy_t s, uint64_t after_ordinal,
                                     pf_native_event_v1* out, int cap) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (cap < 0 || (cap > 0 && !out)) return PF_NATIVE_E_ARGUMENT;
        const auto events = host->native_events(after_ordinal);
        int written = 0;
        std::size_t consumed = 0;
        for (; consumed < events.size() && written < cap; ++consumed) {
            pf_native_event_v1 row;
            if (!translate_event(events[consumed], row)) continue;
            out[written++] = row;
        }
        /* Ordinals are non-decreasing, not strictly increasing: an applied
         * execution and the account observation it produced share one. A
         * caller resumes from the last returned ordinal, so a page that ended
         * in the middle of such a group would lose its siblings forever.
         * Give the whole group back on the next page instead. A cap below the
         * group size (2 today) cannot honour that and is left as written. */
        if (written == cap && written > 1) {
            const std::uint64_t tail = out[written - 1].ordinal;
            for (std::size_t next = consumed; next < events.size(); ++next) {
                pf_native_event_v1 probe;
                if (!translate_event(events[next], probe)) continue;
                if (probe.ordinal == tail) {
                    while (written > 1 && out[written - 1].ordinal == tail) --written;
                }
                break;
            }
        }
        return written;
    });
}

PF_API int strategy_native_state_v1(pf_strategy_t s, pf_native_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto state = host->native_state();
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->lifecycle = static_cast<std::uint32_t>(state.kind);
        out->failure_code = static_cast<std::uint32_t>(state.failure.code);
        out->failure_operation = static_cast<std::uint32_t>(state.failure.operation);
        out->failure_discriminator = state.failure.discriminator;
        out->failure_ordinal = state.failure.ordinal;
        out->consumed_high_water = state.consumed_high_water;
        out->decision_floor_ms = state.decision_floor_ms;
        out->phase = static_cast<std::uint32_t>(state.phase);
        out->completion = static_cast<std::uint32_t>(state.completion);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_declare_subscriptions_v1(pf_strategy_t s,
                                                    const pf_native_subscription_v1* rows,
                                                    int n) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !rows)) return PF_NATIVE_E_ARGUMENT;
        std::vector<pineforge::NativeTimeframeSubscription> declared;
        /* The begin-time declaration carries no source column: it rides in
         * the run spec's auxiliary tail, so a series declared here is built
         * from the input. */
        if (int rc = translate_subscriptions(rows, static_cast<std::uint32_t>(n), nullptr,
                                             declared);
            rc != PF_NATIVE_OK) {
            return rc;
        }
        return host->declare_timeframe_subscriptions(std::move(declared))
            ? PF_NATIVE_OK
            : PF_NATIVE_E_STATE;
    });
}

PF_API int strategy_native_partial_bar_v1(pf_strategy_t s, pf_bar_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto bar = host->current_partial_bar();
        if (!bar) return PF_NATIVE_ABSENT;
        std::memcpy(out, &*bar, sizeof(pf_bar_t));
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_recalculations_v1(pf_strategy_t s, uint64_t* driven,
                                             uint64_t* skipped) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (driven) *driven = host->native_recalculation_count();
        if (skipped) *skipped = host->native_recalculations_skipped();
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_trail_state_v1(pf_strategy_t s, uint64_t incarnation,
                                          pf_native_trail_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_trail_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        const auto state = host->trail_state(no::RequestHandle{*run, incarnation});
        if (!state) return PF_NATIVE_ABSENT;
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->activated = state->activated ? 1u : 0u;
        out->best_price = state->best_price;
        out->current_level = state->current_level;
        out->activation_ordinal = state->activation_ordinal;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_series_bar_v1(pf_strategy_t s, uint32_t subscription,
                                         pf_bar_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto bar = host->native_series_bar(static_cast<std::size_t>(subscription));
        if (!bar) return PF_NATIVE_ABSENT;
        std::memcpy(out, &*bar, sizeof(pf_bar_t));
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_marked_equity_v1(pf_strategy_t s, double mark, double* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        *out = host->native_marked_equity(mark);
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_liquidation_price_v1(pf_strategy_t s, double* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        const auto price = host->native_liquidation_price();
        if (!price) {
            *out = kNaN;
            return PF_NATIVE_ABSENT;
        }
        *out = *price;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_risk_state_v1(pf_strategy_t s, pf_native_risk_state_v1* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        if (out->struct_size != sizeof(pf_native_risk_state_v1)) return PF_NATIVE_E_STRUCT;
        const auto state = host->native_risk_state();
        const std::uint32_t struct_size = out->struct_size;
        std::memset(out, 0, sizeof(*out));
        out->struct_size = struct_size;
        out->version = PF_NATIVE_API_VERSION;
        out->blocked = state.blocked ? 1u : 0u;
        if (state.reason) {
            out->has_reason = 1u;
            out->reason = static_cast<std::uint32_t>(*state.reason);
        }
        out->has_day = state.has_day ? 1u : 0u;
        out->consecutive_loss_days = state.consecutive_loss_days;
        out->day_ordinal = state.day_ordinal;
        out->fills_today = state.fills_today;
        out->peak_equity = state.peak_equity;
        out->day_open_equity = state.day_open_equity;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_continuation_hash_v1(pf_strategy_t s, uint64_t* out) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!out) return PF_NATIVE_E_ARGUMENT;
        *out = host->native_continuation_hash();
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_open_v1(pf_strategy_t s, uint64_t* cohort) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!cohort) return PF_NATIVE_E_ARGUMENT;
        *cohort = host->cohort_open().value;
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_add_v1(pf_strategy_t s, uint64_t cohort,
                                         uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        if (cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        host->cohort_add(no::CohortHandle{cohort}, no::RequestHandle{*run, incarnation});
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_native_cohort_remove_v1(pf_strategy_t s, uint64_t cohort,
                                            uint64_t incarnation) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        const auto* run = run_identity(*host);
        if (!run) return PF_NATIVE_E_STATE;
        if (cohort == 0u) return PF_NATIVE_E_ARGUMENT;
        host->cohort_remove(no::CohortHandle{cohort}, no::RequestHandle{*run, incarnation});
        return PF_NATIVE_OK;
    });
}

PF_API int strategy_configure_native_ext_v1(pf_strategy_t s,
                                            const pf_native_run_spec_v1* base,
                                            const pf_native_run_spec_ext_v1* ext) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (!base || !ext) return PF_NATIVE_E_ARGUMENT;
        if (base->struct_size != sizeof(pf_native_run_spec_v1)) return PF_NATIVE_E_STRUCT;
        /* Four published layouts, and only four: the base one the lane
         * first shipped, that plus L9's risk tail, that plus N8's intrabar /
         * policy tail, and the current one with the auxiliary-feed tail
         * behind it. Anything else is a caller this runtime cannot read. */
        const bool has_auxiliary_tail = ext->struct_size == sizeof(pf_native_run_spec_ext_v1);
        const bool has_policy_tail =
            has_auxiliary_tail || ext->struct_size == PF_NATIVE_RUN_SPEC_EXT_V1_POLICY_SIZE;
        const bool has_risk_tail =
            has_policy_tail || ext->struct_size == PF_NATIVE_RUN_SPEC_EXT_V1_RISK_SIZE;
        if ((!has_risk_tail && ext->struct_size != PF_NATIVE_RUN_SPEC_EXT_V1_BASE_SIZE)
            || ext->version != PF_NATIVE_API_VERSION) {
            return PF_NATIVE_E_STRUCT;
        }
        /* The kernel configures a host exactly once and FAILS it on a second
         * attempt (native_execution_consumer.cpp, "configure refused while
         * ready"), so the readiness check happens here, before the kernel
         * sees anything: a refused extension must leave the handle usable. */
        if (host->native_state().kind != pineforge::NativeLifecycleKind::Unconfigured) {
            return PF_NATIVE_E_STATE;
        }
        pineforge::NativeRunSpec spec;
        if (int rc = translate_base_spec(*base, spec); rc != PF_NATIVE_OK) return rc;
        if (int rc = apply_spec_ext(spec, *ext, has_risk_tail, has_policy_tail,
                                    has_auxiliary_tail);
            rc != PF_NATIVE_OK) {
            return rc;
        }
        return host->configure_native(spec).status == pineforge::NativeSetupStatus::Applied
            ? PF_NATIVE_OK
            : PF_NATIVE_E_ARGUMENT;
    });
}

PF_API int strategy_native_append_auxiliary_bars_v1(pf_strategy_t s, const pf_bar_t* bars,
                                                    int32_t n) {
    return guarded([&] {
        auto* host = host_of(s);
        if (!host) return PF_NATIVE_E_HANDLE;
        if (n < 0 || (n > 0 && !bars)) return PF_NATIVE_E_ARGUMENT;
        return host->append_auxiliary_bars(reinterpret_cast<const Bar*>(bars),
                                           static_cast<std::size_t>(n))
            ? PF_NATIVE_OK
            : PF_NATIVE_E_STATE;
    });
}

}  /* extern "C" */
