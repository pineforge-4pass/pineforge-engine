// R5 lane V19-A: the v19 continuation is a fold over the consumer's live
// state (native-consumer/v9), and the broker-state hash folds the closed rows
// through a running digest (pineforge-broker-state/v19).
//
// The witness holds the contract PERF-D1 §2.1 proposed and V19-A shipped:
//   1. Completeness, field by field. Every member of the fold list, perturbed
//      alone on a finished host, moves the continuation, and restoring it
//      restores the value: equal state answers an equal value, and no folded
//      field is dead. Members a spec folds only when it opts in (margin, risk,
//      calculation timing, declared series, an auxiliary feed, a host margin
//      verdict) are perturbed on a host whose spec opts in.
//   2. What commands leave in the live tables moves it too: a live request's
//      fields, a roster's members, one more command's counters and record.
//   3. Equal state, equal value: two fresh hosts, and a reused host's second
//      run against a fresh host's.
//   4. History independence: two command histories that reach one state --
//      a request placed at another price and label, then re-priced to the
//      same one; a request of another size and label, placed and cancelled
//      -- answer one continuation and one broker-state hash. The base folded
//      the whole command history, so they differed there.
//   5. The strength the replay check keeps: a Cancelled receipt's reason
//      (OwnerGone for a child its parent's cancel took, User for a child the
//      host cancelled itself) moves the continuation although the two runs
//      leave the same state, because every committed event folds a compact
//      record of its kind and reason.
//   6. Scaling. Kernel-recorded per-bar broker hashes cost linear time: four
//      times the bars cost less than five times the CPU (the base walked every
//      closed row at every row, quadratic). A continuation read costs the same
//      at a quarter and at the whole of a run.
//   7. Closed-row finality. A host may amend a booked row while the applied
//      notification of the execution that booked it runs, and the digest takes
//      the amended row; a read between the booking and that notification folds
//      the row but does not freeze it, so reads are pure; an amendment of a
//      final row is what closed_rows_digest_holds -- the Debug check a run's
//      end makes -- refuses, unless the host names it
//      (native_closed_rows_amended), when the rows fold again from there; rows
//      removed from the end are forgotten without a name.
//
// Portability: the zone is the fixed offset "UTC+0", resolved from its
// definition alone, so no tzdata file enters the continuation (E23).
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

// One member of the fold list, perturbed and restored in place.
struct FoldField {
    std::string name;
    std::function<void(NativeExecutionConsumer&)> perturb;
    std::function<void(NativeExecutionConsumer&)> restore;
};

// The consumer's private members, for the completeness differential.
struct NativeExecutionConsumerProbe {
    using C = NativeExecutionConsumer;

    template <class Get, class Change>
    static FoldField field(std::string name, Get get, Change change) {
        using T = std::decay_t<decltype(get(std::declval<C&>()))>;
        auto saved = std::make_shared<std::optional<T>>();
        return FoldField{std::move(name),
                         [=](C& c) { auto& v = get(c); *saved = v; change(v); },
                         [=](C& c) { get(c) = **saved; }};
    }

    template <class T>
    static void toggle(std::optional<T>& v) {
        if (v) v.reset(); else v.emplace();
    }

    // Every consumer member the v9 fold reads for any spec.
    static std::vector<FoldField> always(C&) {
        std::vector<FoldField> out;
        const auto flip = [](bool& v) { v = !v; };
        const auto bump = [](auto& v) { v += 1; };
        out.push_back(field("lifecycle (completion)", [](C& c) -> NativeLifecycle& { return c.state_; },
            [](NativeLifecycle& v) {
                auto& done = std::get<NativeCompleted>(v);
                done.completion = done.completion == NativeCompletion::BatchComplete
                    ? NativeCompletion::StreamEnded : NativeCompletion::BatchComplete;
            }));
        out.push_back(field("bound_session_key_", [](C& c) -> std::string& { return c.bound_session_key_; },
            [](std::string& v) { v += "x"; }));
        out.push_back(field("decision_floor_ms_", [](C& c) -> int64_t& { return c.decision_floor_ms_; }, bump));
        out.push_back(field("has_floor_", [](C& c) -> bool& { return c.has_floor_; }, flip));
        out.push_back(field("next_timeline_ordinal_", [](C& c) -> uint64_t& { return c.next_timeline_ordinal_; }, bump));
        out.push_back(field("in_callback_", [](C& c) -> bool& { return c.in_callback_; }, flip));
        out.push_back(field("callback_phase_", [](C& c) -> C::CallbackPhase& { return c.callback_phase_; },
            [](C::CallbackPhase& v) {
                v = v == C::CallbackPhase::Bar ? C::CallbackPhase::Tick : C::CallbackPhase::Bar;
            }));
        out.push_back(field("preparing_begin_", [](C& c) -> bool& { return c.preparing_begin_; }, flip));
        out.push_back(field("input_callback_context_",
            [](C& c) -> std::optional<NativeInputContext>& { return c.input_callback_context_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("input_callback_bar_", [](C& c) -> std::optional<Bar>& { return c.input_callback_bar_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("tick_callback_context_",
            [](C& c) -> std::optional<NativeTickContext>& { return c.tick_callback_context_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("tick_callback_bar_", [](C& c) -> std::optional<Bar>& { return c.tick_callback_bar_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("callback_context_.coordinate", [](C& c) -> uint64_t& {
            return c.callback_context_.coordinate.ordinal; }, bump));
        out.push_back(field("callback_context_.decision_floor_ms", [](C& c) -> int64_t& {
            return c.callback_context_.decision_floor_ms; }, bump));
        out.push_back(field("callback_context_.input_interval", [](C& c) -> int64_t& {
            return c.callback_context_.input_interval.open_ms; }, bump));
        out.push_back(field("callback_context_.script_interval", [](C& c) -> int64_t& {
            return c.callback_context_.script_interval.next_period_open_ms; }, bump));
        out.push_back(field("callback_context_.sub_index", [](C& c) -> int& {
            return c.callback_context_.sub_index; }, bump));
        out.push_back(field("callback_context_.sub_count", [](C& c) -> int& {
            return c.callback_context_.sub_count; }, bump));
        out.push_back(field("callback_context_.is_terminal_sub_bar", [](C& c) -> bool& {
            return c.callback_context_.is_terminal_sub_bar; }, flip));
        out.push_back(field("callback_context_.sub_bar_open_ms", [](C& c) -> int64_t& {
            return c.callback_context_.sub_bar_open_ms; }, bump));
        out.push_back(field("callback_context_.script_bar_open_ms", [](C& c) -> int64_t& {
            return c.callback_context_.script_bar_open_ms; }, bump));
        out.push_back(field("callback_context_.driver_statistics", [](C& c) -> uint64_t& {
            return c.callback_context_.driver_statistics.sample_ticks_processed; }, bump));
        out.push_back(field("consuming_request_", [](C& c) -> bool& { return c.consuming_request_; }, flip));
        out.push_back(field("draining_notifications_", [](C& c) -> bool& { return c.draining_notifications_; }, flip));
        out.push_back(field("current_frame_",
            [](C& c) -> std::optional<C::CurrentExecutionFrame>& { return c.current_frame_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("pre_open_birth_point_ordinal_", [](C& c) -> uint64_t& {
            return c.pre_open_birth_point_ordinal_; }, bump));
        out.push_back(field("pre_open_birth_time_ms_", [](C& c) -> int64_t& {
            return c.pre_open_birth_time_ms_; }, bump));
        out.push_back(field("pre_open_births_",
            [](C& c) -> std::vector<native_order::RequestHandle>& { return c.pre_open_births_; },
            [](auto& v) { v.push_back(native_order::RequestHandle{{"probe", 1}, 7}); }));
        out.push_back(field("applied_notifications_",
            [](C& c) -> std::vector<C::AppliedNotification>& { return c.applied_notifications_; },
            [](auto& v) { v.push_back(C::AppliedNotification{}); }));
        out.push_back(field("processing_input_", [](C& c) -> bool& { return c.processing_input_; }, flip));
        out.push_back(field("input_mode_", [](C& c) -> C::InputMode& { return c.input_mode_; },
            [](C::InputMode& v) {
                v = v == C::InputMode::ObservedTicks ? C::InputMode::ConfirmedBars
                                                     : C::InputMode::ObservedTicks;
            }));
        out.push_back(field("next_interval_index_", [](C& c) -> int& { return c.next_interval_index_; }, bump));
        out.push_back(field("staged_ingress_fx_", [](C& c) -> bool& { return c.staged_ingress_fx_; }, flip));
        out.push_back(field("staged_fx_curve_",
            [](C& c) -> std::optional<NativeFxCurve>& { return c.staged_fx_curve_; },
            [](auto& v) {
                if (v) v.reset(); else v = NativeFxCurve{{0}, {1.0}};
            }));
        out.push_back(field("tz_identity_",
            [](C& c) -> std::optional<native_calendar::TimezoneIdentityDescriptor>& {
                return c.tz_identity_; },
            [](auto& v) { v->resource_digest += 1; }));
        out.push_back(field("cohort_receipts_ (digest)", [](C& c) -> uint64_t& {
            return c.cohort_receipts_.h; }, [](uint64_t& v) { v ^= 1; }));
        out.push_back(field("group_receipts_ (digest)", [](C& c) -> uint64_t& {
            return c.group_receipts_.h; }, [](uint64_t& v) { v ^= 1; }));
        out.push_back(field("event_records_ (digest)", [](C& c) -> uint64_t& {
            return c.event_records_.h; }, [](uint64_t& v) { v ^= 1; }));
        out.push_back(field("current_input_open_", [](C& c) -> std::optional<int64_t>& {
            return c.current_input_open_; }, [](auto& v) { toggle(v); }));
        out.push_back(field("observed_input_cursor_", [](C& c) -> std::optional<int64_t>& {
            return c.observed_input_cursor_; }, [](auto& v) { toggle(v); }));
        out.push_back(field("next_tradable_synthesis_cursor_", [](C& c) -> std::optional<int64_t>& {
            return c.next_tradable_synthesis_cursor_; }, [](auto& v) { toggle(v); }));
        out.push_back(field("last_accepted_input_",
            [](C& c) -> std::optional<native_calendar::NativeInterval>& { return c.last_accepted_input_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("last_observed_slot_open_", [](C& c) -> std::optional<int64_t>& {
            return c.last_observed_slot_open_; }, [](auto& v) { toggle(v); }));
        out.push_back(field("last_finalized_input_",
            [](C& c) -> std::optional<native_calendar::NativeInterval>& { return c.last_finalized_input_; },
            [](auto& v) { toggle(v); }));
        out.push_back(field("has_tick_sequence_", [](C& c) -> bool& { return c.has_tick_sequence_; }, flip));
        out.push_back(field("last_tick_sequence_", [](C& c) -> uint64_t& { return c.last_tick_sequence_; }, bump));
        out.push_back(field("script_.key", [](C& c) -> int64_t& { return c.script_.key; }, bump));
        out.push_back(field("script_.has_data", [](C& c) -> bool& { return c.script_.has_data; }, flip));
        out.push_back(field("script_.sealed", [](C& c) -> bool& { return c.script_.sealed; }, flip));
        out.push_back(field("script_.interval", [](C& c) -> int64_t& { return c.script_.interval.eligible_open_ms; }, bump));
        out.push_back(field("script_.agg", [](C& c) -> double& { return c.script_.agg.close; }, bump));
        out.push_back(field("script_.first_open_ms", [](C& c) -> int64_t& { return c.script_.first_open_ms; }, bump));
        out.push_back(field("script_.first_source_time_ms", [](C& c) -> int64_t& {
            return c.script_.first_source_time_ms; }, bump));
        out.push_back(field("script_.latest_close_ms", [](C& c) -> int64_t& { return c.script_.latest_close_ms; }, bump));
        out.push_back(field("script_.first_index", [](C& c) -> int& { return c.script_.first_index; }, bump));
        out.push_back(field("script_.last_index", [](C& c) -> int& { return c.script_.last_index; }, bump));
        out.push_back(field("script_.modeled_ohlc", [](C& c) -> bool& { return c.script_.modeled_ohlc; }, flip));
        out.push_back(field("has_forming_", [](C& c) -> bool& { return c.has_forming_; }, flip));
        out.push_back(field("has_last_price_", [](C& c) -> bool& { return c.has_last_price_; }, flip));
        out.push_back(field("last_price_", [](C& c) -> double& { return c.last_price_; }, bump));
        out.push_back(field("last_print_time_ms_", [](C& c) -> int64_t& { return c.last_print_time_ms_; }, bump));
        out.push_back(field("driver_statistics_", [](C& c) -> uint64_t& {
            return c.driver_statistics_.sub_bars_processed; }, bump));
        out.push_back(field("pairing_.pairing",
            [](C& c) -> native_calendar::TimeframePairing& { return c.pairing_.pairing; },
            [](native_calendar::TimeframePairing& v) {
                v = v == native_calendar::TimeframePairing::Passthrough
                    ? native_calendar::TimeframePairing::SameUnitMultiple
                    : native_calendar::TimeframePairing::Passthrough;
            }));
        out.push_back(field("pairing_.group_factor", [](C& c) -> int64_t& { return c.pairing_.group_factor; }, bump));
        return out;
    }

    // Members folded only under a precondition, perturbed with it set.
    // Members folded only under a precondition. `setup` establishes it in the
    // base the field is then perturbed against; `teardown` removes it again.
    struct Conditional {
        std::string name;
        std::function<void(C&)> setup;
        FoldField field;
        std::function<void(C&)> teardown;
    };
    static std::vector<Conditional> conditional(C&) {
        std::vector<Conditional> out;
        const auto bump = [](auto& v) { v += 1; };
        out.push_back({"input_callback_context_->input_index",
            [](C& c) { c.input_callback_context_.emplace(); },
            field("input_index", [](C& c) -> int& { return c.input_callback_context_->input_index; }, bump),
            [](C& c) { c.input_callback_context_.reset(); }});
        out.push_back({"current_frame_->acceptance_cutoff",
            [](C& c) { c.current_frame_.emplace(); },
            field("acceptance_cutoff", [](C& c) -> uint64_t& { return c.current_frame_->acceptance_cutoff; }, bump),
            [](C& c) { c.current_frame_.reset(); }});
        out.push_back({"current_frame_->point.price",
            [](C& c) { c.current_frame_.emplace(); },
            field("point.price", [](C& c) -> double& { return c.current_frame_->point.price; }, bump),
            [](C& c) { c.current_frame_.reset(); }});
        out.push_back({"forming_ (with has_forming_)",
            [](C& c) { c.has_forming_ = !c.has_forming_; },
            field("forming_", [](C& c) -> double& { return c.forming_.high; }, bump),
            [](C& c) { c.has_forming_ = !c.has_forming_; }});
        out.push_back({"queued notification history_index",
            [](C& c) { c.applied_notifications_.push_back(C::AppliedNotification{}); },
            field("history_index", [](C& c) -> std::size_t& {
                return c.applied_notifications_.back().history_index; }, bump),
            [](C& c) { c.applied_notifications_.pop_back(); }});
        out.push_back({"queued notification ordinal",
            [](C& c) { c.applied_notifications_.push_back(C::AppliedNotification{}); },
            field("ordinal", [](C& c) -> uint64_t& { return c.applied_notifications_.back().ordinal; }, bump),
            [](C& c) { c.applied_notifications_.pop_back(); }});
        out.push_back({"queued notification point",
            [](C& c) { c.applied_notifications_.push_back(C::AppliedNotification{}); },
            field("point", [](C& c) -> double& { return c.applied_notifications_.back().point.price; }, bump),
            [](C& c) { c.applied_notifications_.pop_back(); }});
        out.push_back({"queued notification margin_call_index",
            [](C& c) { c.applied_notifications_.push_back(C::AppliedNotification{}); },
            field("margin_call_index", [](C& c) -> std::optional<std::size_t>& {
                return c.applied_notifications_.back().margin_call_index; },
                [](auto& v) { toggle(v); }),
            [](C& c) { c.applied_notifications_.pop_back(); }});
        return out;
    }

    static std::vector<FoldField> margin(C&) {
        std::vector<FoldField> out;
        const auto flip = [](bool& v) { v = !v; };
        const auto bump = [](auto& v) { v += 1; };
        out.push_back(field("margin_liquidation_",
            [](C& c) -> std::optional<C::MarginLiquidation>& { return c.margin_liquidation_; },
            [](auto& v) { if (v) v.reset(); else v.emplace(); }));
        out.push_back(field("has_margin_path_", [](C& c) -> bool& { return c.has_margin_path_; }, flip));
        out.push_back(field("margin_point_ordinal_", [](C& c) -> uint64_t& { return c.margin_point_ordinal_; }, bump));
        out.push_back(field("margin_point_calls_", [](C& c) -> std::uint32_t& { return c.margin_point_calls_; }, bump));
        return out;
    }

    static std::vector<FoldField> risk(C&) {
        std::vector<FoldField> out;
        const auto flip = [](bool& v) { v = !v; };
        const auto bump = [](auto& v) { v += 1; };
        out.push_back(field("risk_.has_day", [](C& c) -> bool& { return c.risk_.has_day; }, flip));
        out.push_back(field("risk_.fills_today", [](C& c) -> std::uint64_t& { return c.risk_.fills_today; }, bump));
        out.push_back(field("risk_.consecutive_loss_days", [](C& c) -> std::uint32_t& {
            return c.risk_.consecutive_loss_days; }, bump));
        out.push_back(field("risk_.has_peak", [](C& c) -> bool& { return c.risk_.has_peak; }, flip));
        out.push_back(field("risk_.day_open_equity", [](C& c) -> double& { return c.risk_.day_open_equity; }, bump));
        out.push_back(field("risk_.day_open_realized", [](C& c) -> double& { return c.risk_.day_open_realized; }, bump));
        out.push_back(field("risk_.run_block",
            [](C& c) -> std::optional<native_order::RiskLimitKind>& { return c.risk_.run_block; },
            [](auto& v) { if (v) v.reset(); else v = native_order::RiskLimitKind{}; }));
        out.push_back(field("risk_.day_block",
            [](C& c) -> std::optional<native_order::RiskLimitKind>& { return c.risk_.day_block; },
            [](auto& v) { if (v) v.reset(); else v = native_order::RiskLimitKind{}; }));
        return out;
    }

    static std::vector<FoldField> calc_timing(C&) {
        std::vector<FoldField> out;
        const auto flip = [](bool& v) { v = !v; };
        const auto bump = [](auto& v) { v += 1; };
        out.push_back(field("recalc_epoch_", [](C& c) -> uint64_t& { return c.recalc_epoch_; }, bump));
        out.push_back(field("recalc_epoch_count_", [](C& c) -> uint32_t& { return c.recalc_epoch_count_; }, bump));
        out.push_back(field("recalculations_", [](C& c) -> uint64_t& { return c.recalculations_; }, bump));
        out.push_back(field("recalculations_skipped_", [](C& c) -> uint64_t& {
            return c.recalculations_skipped_; }, bump));
        out.push_back(field("partial_has_", [](C& c) -> bool& { return c.partial_has_; }, flip));
        return out;
    }

    static std::vector<FoldField> subscriptions(C&) {
        std::vector<FoldField> out;
        const auto bump = [](auto& v) { v += 1; };
        out.push_back(field("subscription_warmup_inputs_", [](C& c) -> int& {
            return c.subscription_warmup_inputs_; }, bump));
        out.push_back(field("subscriptions_[0].bucket_first_index", [](C& c) -> int& {
            return c.subscriptions_.at(0).bucket_first_index; }, bump));
        out.push_back(field("subscriptions_[0].projected_cursor", [](C& c) -> std::size_t& {
            return c.subscriptions_.at(0).projected_cursor; }, bump));
        out.push_back(field("subscriptions_[0].latest", [](C& c) -> std::optional<Bar>& {
            return c.subscriptions_.at(0).latest; }, [](auto& v) { toggle(v); }));
        return out;
    }

    static std::vector<FoldField> auxiliary(C&) {
        std::vector<FoldField> out;
        out.push_back(field("auxiliary_appended_digest_", [](C& c) -> std::uint64_t& {
            return c.auxiliary_appended_digest_; }, [](std::uint64_t& v) { v ^= 1; }));
        out.push_back(field("auxiliary_appended_", [](C& c) -> std::vector<Bar>& {
            return c.auxiliary_appended_; }, [](std::vector<Bar>& v) { v.push_back(Bar{}); }));
        return out;
    }

    static std::vector<FoldField> precommit(C&) {
        std::vector<FoldField> out;
        out.push_back(field("precommit_digest_", [](C& c) -> uint64_t& {
            return c.precommit_digest_.h; }, [](uint64_t& v) { v ^= 1; }));
        return out;
    }

    // The applied spec folds as a cached digest; the kernel's own spec-changing
    // paths clear the cache, as this does.
    static FoldField spec_field() {
        auto saved = std::make_shared<double>();
        return FoldField{"spec (initial_capital)",
            [=](C& c) {
                auto& spec = std::get<NativeCompleted>(c.state_).spec;
                *saved = spec.initial_capital;
                spec.initial_capital += 1.0;
                c.forget_spec_digests();
            },
            [=](C& c) {
                std::get<NativeCompleted>(c.state_).spec.initial_capital = *saved;
                c.forget_spec_digests();
            }};
    }

    static std::uint64_t precommit_count(const C& c) { return c.precommit_digest_.count; }
    static bool holds(const C& c, const std::vector<Trade>& rows) {
        return c.closed_rows_digest_holds(rows);
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t T = 1736121600000LL;  // 2025-01-06 00:00 UTC
constexpr std::int64_t kMinute = 60000;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 8;
        const double p = 100.0 + 0.5 * (phase < 4 ? phase : 8 - phase) + 0.25 * (i % 3);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0, T + static_cast<std::int64_t>(i) * kMinute});
    }
    return bars;
}

NativeRunSpec base_spec(std::uint64_t run_number = 1) {
    NativeRunSpec spec;
    spec.identity = {"v19a-state-continuation", run_number};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:V19A";
    spec.timezone = "UTC+0";
    spec.session = "24x7";
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 1;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    return spec;
}

// A bare host whose every callback is a script.
struct Host : NativeStrategyHost {
    std::function<void(Host&, int)> bar_script;
    std::function<void(Host&, const no::ExecutionAppliedEvent&)> applied_script;
    std::function<no::ExecutionTerms(const NativeExecutionTermsFacts&)> terms;
    NativePrecommitVerdict verdict = NativePrecommitVerdict::Admit;
    int bar = 0;

    void on_native_run_begin() override { bar = 0; }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bar_script) bar_script(*this, bar);
        ++bar;
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (applied_script) applied_script(*this, event);
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        (void)NativeStrategyHost::validate_execution_precommit(view);
        return verdict;
    }
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const std::vector<Trade>& rows() const { return trades_; }
    std::vector<Trade>& mutable_rows() { return trades_; }
};

bool run(Host& host, const NativeRunSpec& spec, int bars) {
    const auto setup = host.configure_native(spec);
    if (setup.status != NativeSetupStatus::Applied) {
        std::fprintf(stderr, "configure refused: %s\n", host.last_error().c_str());
        return false;
    }
    const auto input = tape(bars);
    host.run(input.data(), bars);
    if (host.native_state().kind != NativeLifecycleKind::Completed) {
        std::fprintf(stderr, "run did not complete: %s\n", host.last_error().c_str());
        return false;
    }
    return true;
}

// A script that leaves every table a continuation folds populated: fills and
// closed rows, a resting request, a replaced one, a cancelled one, a roster.
void busy_script(Host& h, int bar) {
    if (bar == 0) {
        const auto cohort = h.cohort_open();
        const auto buy = h.submit(no::Request{no::Transact{2.0}, "buy", "entry"});
        if (buy.handle) h.cohort_add(cohort, *buy.handle);
    }
    if (bar == 2) h.submit(no::Request{no::Flatten{}, "flat", ""});
    if (bar == 3) {
        no::Request rest{no::Transact{1.0}, "rest", "resting"};
        rest.trigger = no::Limit{90.0};
        const auto placed = h.submit(rest);
        if (placed.handle) {
            no::Request moved = rest;
            moved.trigger = no::Limit{91.0};
            (void)h.replace(*placed.handle, moved);
        }
        no::Request gone{no::Transact{1.0}, "gone", ""};
        gone.trigger = no::Limit{80.0};
        const auto doomed = h.submit(gone);
        if (doomed.handle) h.cancel(*doomed.handle);
    }
}

void differential(const char* label, Host& host, std::vector<FoldField> fields) {
    auto& c = host.consumer();
    const std::uint64_t base = host.native_continuation_hash();
    for (auto& fld : fields) {
        fld.perturb(c);
        const std::uint64_t moved = host.native_continuation_hash();
        fld.restore(c);
        const std::uint64_t back = host.native_continuation_hash();
        if (moved == base) std::fprintf(stderr, "  %s: %s does not move the value\n", label, fld.name.c_str());
        if (back != base) std::fprintf(stderr, "  %s: %s does not restore the value\n", label, fld.name.c_str());
        CHECK(moved != base);
        CHECK(back == base);
    }
}

// 1. Completeness, field by field.
void every_folded_member_moves_the_value() {
    {
        Host host;
        host.bar_script = busy_script;
        if (!run(host, base_spec(), 8)) { CHECK(false); return; }
        differential("plain", host, NativeExecutionConsumerProbe::always(host.consumer()));
        // Conditional members, each with its precondition set in the base.
        for (auto& item : NativeExecutionConsumerProbe::conditional(host.consumer())) {
            auto& c = host.consumer();
            const std::uint64_t bare = host.native_continuation_hash();
            item.setup(c);
            differential(item.name.c_str(), host, {item.field});
            item.teardown(c);
            CHECK(host.native_continuation_hash() == bare);
        }
        differential("spec", host, {NativeExecutionConsumerProbe::spec_field()});
    }
    {
        // A margin model and risk limits, both folded only when declared.
        Host host;
        host.bar_script = busy_script;
        auto spec = base_spec();
        NativeMarginModel margin;
        margin.initial_long = margin.initial_short = 0.5;
        margin.maintenance_long = margin.maintenance_short = 0.25;
        spec.margin = margin;
        NativeRiskLimits risk;
        risk.max_fills_per_day = 100;
        spec.risk = risk;
        if (!run(host, spec, 8)) { CHECK(false); return; }
        differential("margin", host, NativeExecutionConsumerProbe::margin(host.consumer()));
        differential("risk", host, NativeExecutionConsumerProbe::risk(host.consumer()));
    }
    {
        Host host;
        host.bar_script = busy_script;
        auto spec = base_spec();
        spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
        if (!run(host, spec, 8)) { CHECK(false); return; }
        differential("calc timing", host, NativeExecutionConsumerProbe::calc_timing(host.consumer()));
    }
    {
        Host host;
        auto spec = base_spec();
        NativeTimeframeSubscription five;
        five.tf = "5";
        spec.subscriptions.push_back(five);
        if (!run(host, spec, 16)) { CHECK(false); return; }
        differential("subscription", host, NativeExecutionConsumerProbe::subscriptions(host.consumer()));
    }
    {
        Host host;
        auto spec = base_spec();
        spec.input_tf = "5";
        spec.script_tf = "5";
        NativeAuxiliaryFeed feed;
        feed.tf = "1";
        spec.auxiliary_feed = feed;
        if (!run(host, spec, 0)) { /* an empty batch still applies the spec */ }
        if (host.native_state().kind == NativeLifecycleKind::Completed) {
            differential("auxiliary", host, NativeExecutionConsumerProbe::auxiliary(host.consumer()));
        } else {
            std::fprintf(stderr, "  auxiliary host did not complete: %s\n", host.last_error().c_str());
            CHECK(false);
        }
    }
    {
        // A host margin verdict folds only once the spec exposes the gate.
        Host host;
        host.bar_script = busy_script;
        host.verdict = NativePrecommitVerdict::AdmitWithHostMargin;
        auto spec = base_spec();
        spec.initial_margin_fraction = 0.5;
        if (!run(host, spec, 8)) { CHECK(false); return; }
        CHECK(NativeExecutionConsumerProbe::precommit_count(host.consumer()) != 0);
        differential("precommit", host, NativeExecutionConsumerProbe::precommit(host.consumer()));
    }
}

struct Outcome {
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::size_t trades = 0;
};

Outcome outcome(Host& host) {
    return {host.native_continuation_hash(), host.broker_state_hash(), host.rows().size()};
}

Outcome scripted(std::function<void(Host&, int)> script, int bars = 6,
                 NativeRunSpec spec = base_spec()) {
    Host host;
    host.bar_script = std::move(script);
    if (!run(host, spec, bars)) { CHECK(false); return {}; }
    return outcome(host);
}

// 2. What commands leave in the live tables.
void live_tables_move_the_value() {
    const auto resting = [](double price, double units, const char* label, const char* comment) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            no::Request r{no::Transact{units}, label, comment};
            r.trigger = no::Limit{price};
            h.submit(r);
        };
    };
    const auto base = scripted(resting(90.0, 1.0, "a", "c"));
    CHECK(scripted(resting(90.25, 1.0, "a", "c")).continuation != base.continuation);  // trigger
    CHECK(scripted(resting(90.0, 2.0, "a", "c")).continuation != base.continuation);   // quantity
    CHECK(scripted(resting(90.0, 1.0, "b", "c")).continuation != base.continuation);   // label
    CHECK(scripted(resting(90.0, 1.0, "a", "d")).continuation != base.continuation);   // comment
    CHECK(scripted(resting(90.0, 1.0, "a", "c")).continuation == base.continuation);   // the same
    // A roster member.
    const auto roster = [](bool add) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            const auto cohort = h.cohort_open();
            no::Request r{no::Transact{1.0}, "r", ""};
            r.trigger = no::Limit{80.0};
            const auto placed = h.submit(r);
            if (add && placed.handle) h.cohort_add(cohort, *placed.handle);
        };
    };
    CHECK(scripted(roster(true)).continuation != scripted(roster(false)).continuation);
    // One more command: its ordinal, the core's counter and its record.
    const auto refused = [](bool extra) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            h.submit(no::Request{no::Transact{1.0}, "x", ""});
            if (extra) {
                no::Request bad{no::Transact{0.0}, "zero", ""};
                (void)h.submit(bad);
            }
        };
    };
    const auto one = scripted(refused(false));
    const auto two = scripted(refused(true));
    CHECK(two.continuation != one.continuation);
    CHECK(two.trades == one.trades);
}

// 3. Equal state, equal value.
void equal_state_equal_value() {
    const auto a = scripted(busy_script, 8);
    const auto b = scripted(busy_script, 8);
    CHECK(a.continuation == b.continuation);
    CHECK(a.broker == b.broker);
    // A reused host's second run against a fresh host's first run of the same
    // spec: run generations fold relative to the run (A41).
    Host reused;
    reused.bar_script = busy_script;
    CHECK(run(reused, base_spec(1), 8));
    CHECK(run(reused, base_spec(2), 8));
    Host fresh;
    fresh.bar_script = busy_script;
    CHECK(run(fresh, base_spec(2), 8));
    CHECK(reused.native_continuation_hash() == fresh.native_continuation_hash());
    CHECK(reused.broker_state_hash() == fresh.broker_state_hash());
}

// 4. Two histories, one state.
void two_histories_one_state() {
    // Placed at another price, label and comment, then re-priced to the same one.
    const auto repriced = [](double first, const char* label) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            no::Request r{no::Transact{1.0}, label, "draft"};
            r.trigger = no::Limit{first};
            const auto placed = h.submit(r);
            if (!placed.handle) return;
            no::Request final_form{no::Transact{1.0}, "final", "live"};
            final_form.trigger = no::Limit{91.0};
            (void)h.replace(*placed.handle, final_form);
        };
    };
    const auto a = scripted(repriced(90.0, "a"));
    const auto b = scripted(repriced(95.5, "a-much-longer-label"));
    CHECK(a.continuation == b.continuation);
    CHECK(a.broker == b.broker);
    // Another size, label and trigger, placed and cancelled.
    const auto cancelled = [](double units, const char* label, double price) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            no::Request r{no::Transact{units}, label, ""};
            r.trigger = no::Limit{price};
            const auto placed = h.submit(r);
            if (placed.handle) h.cancel(*placed.handle);
        };
    };
    const auto c = scripted(cancelled(1.0, "x", 80.0));
    const auto d = scripted(cancelled(5.0, "other", 70.25));
    CHECK(c.continuation == d.continuation);
    CHECK(c.broker == d.broker);
}

// 5. A Cancelled receipt's reason still differs.
void cancelled_reason_is_kept() {
    const auto parent_first = [](bool host_cancels_child) {
        return [=](Host& h, int bar) {
            if (bar != 1) return;
            no::Request parent{no::Transact{1.0}, "parent", ""};
            parent.trigger = no::Limit{80.0};
            const auto p = h.submit(parent);
            if (!p.handle) return;
            no::Request child{no::Flatten{}, "child", ""};
            child.trigger = no::Stop{70.0};
            child.owner = no::WaitForApplied{*p.handle};
            const auto c = h.submit(child);
            if (host_cancels_child && c.handle) h.cancel(*c.handle);
            h.cancel(*p.handle);
        };
    };
    Host a_host;
    a_host.bar_script = parent_first(false);   // child: OwnerGone
    CHECK(run(a_host, base_spec(), 6));
    Host b_host;
    b_host.bar_script = parent_first(true);    // child: User
    CHECK(run(b_host, base_spec(), 6));
    // The same book and the same live tables; the reasons alone differ.
    CHECK(a_host.native_working_requests().empty());
    CHECK(b_host.native_working_requests().empty());
    CHECK(a_host.native_events(0).size() == b_host.native_events(0).size());
    CHECK(a_host.native_continuation_hash() != b_host.native_continuation_hash());
}

double cpu_seconds() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }

// One bracket round trip every ten bars, a kernel-recorded report and the
// per-bar broker hash recorded: the recording's cost per bar.
double recorded_seconds(int bars) {
    double best = 1e30;
    for (int round = 0; round < 3; ++round) {
        Host host;
        host.bar_script = [](Host& h, int bar) {
            if (bar % 10 == 0) h.submit(no::Request{no::Transact{1.0}, "in", ""});
            if (bar % 10 == 5) h.submit(no::Request{no::Flatten{}, "out", ""});
        };
        auto spec = base_spec();
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        if (host.configure_native(spec).status != NativeSetupStatus::Applied) return 0.0;
        host.set_broker_state_hash_recording(true);
        const auto input = tape(bars);
        const double start = cpu_seconds();
        host.run(input.data(), bars);
        best = std::min(best, cpu_seconds() - start);
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.rows().size() == static_cast<std::size_t>(bars / 10));
    }
    return best;
}

// A continuation read at a bar, timed over many reads.
double read_seconds(int bars, int read_at) {
    Host host;
    double spent = 0.0;
    host.bar_script = [&](Host& h, int bar) {
        if (bar % 10 == 0) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar % 10 == 5) h.submit(no::Request{no::Flatten{}, "out", ""});
        if (bar == read_at) {
            double best = 1e30;
            std::uint64_t sink = 0;
            for (int round = 0; round < 5; ++round) {
                const double start = cpu_seconds();
                for (int i = 0; i < 2000; ++i) sink ^= h.native_continuation_hash();
                best = std::min(best, cpu_seconds() - start);
            }
            spent = best;
            CHECK(sink != 1);
        }
    };
    if (!run(host, base_spec(), bars)) return 0.0;
    return spent;
}

// 6. Scaling.
void recording_and_reads_scale() {
    const double quarter = recorded_seconds(5000);
    const double whole = recorded_seconds(20000);
    const double ratio = quarter > 0.0 ? whole / quarter : 0.0;
    std::printf("  kernel-recorded per-bar broker hashes: 5000 bars %.4f s, 20000 bars %.4f s "
                "(x%.2f for 4x the bars)\n", quarter, whole, ratio);
    CHECK(ratio > 0.0);
    CHECK(ratio < 5.0);
    const double early = read_seconds(10000, 2500);
    const double late = read_seconds(10000, 9999);
    const double read_ratio = early > 0.0 ? late / early : 0.0;
    std::printf("  continuation read, 2000 reads: at bar 2500 %.4f s, at bar 9999 %.4f s "
                "(x%.2f)\n", early, late, read_ratio);
    CHECK(read_ratio > 0.0);
    CHECK(read_ratio < 2.0);
}

// 7. Closed-row finality.
void closed_rows_are_final_after_their_notification() {
    // The row a Flatten books is amended inside its own notification, and
    // read (recording on) before it: the digest takes the amended row.
    const auto amend = [](bool read_before) {
        return [=](Host& h, int bar) {
            if (bar == 1) h.submit(no::Request{no::Transact{1.0}, "in", ""});
            if (bar == 3) {
                h.submit(no::Request{no::Flatten{}, "out", ""});
                if (read_before) (void)h.broker_state_hash();
            }
        };
    };
    const auto amended_host = [&](bool read_before) {
        auto host = std::make_unique<Host>();
        host->bar_script = amend(read_before);
        host->applied_script = [](Host& h, const no::ExecutionAppliedEvent& event) {
            for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
                h.mutable_rows()[event.first_trade_index + i].exit_time += 1000;
            }
        };
        return host;
    };
    auto quiet = amended_host(false);
    auto reader = amended_host(true);
    CHECK(run(*quiet, base_spec(), 6));
    CHECK(run(*reader, base_spec(), 6));
    CHECK(quiet->rows().size() == 1);
    CHECK(quiet->broker_state_hash() == reader->broker_state_hash());
    CHECK(NativeExecutionConsumerProbe::holds(reader->consumer(), reader->rows()));
    // An unamended run answers another value: the amendment was taken.
    Host plain;
    plain.bar_script = amend(false);
    CHECK(run(plain, base_spec(), 6));
    CHECK(plain.broker_state_hash() != quiet->broker_state_hash());
    // A final row amended afterwards is what the Debug check refuses. The
    // amendment is undone before the run ends, so a Debug build completes.
    Host late;
    bool refused = false;
    late.bar_script = [&](Host& h, int bar) {
        if (bar == 1) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar == 3) h.submit(no::Request{no::Flatten{}, "out", ""});
        if (bar == 5 && !h.rows().empty()) {
            (void)h.broker_state_hash();   // folds the final row
            h.mutable_rows()[0].exit_price += 1.0;
            refused = !NativeExecutionConsumerProbe::holds(h.consumer(), h.rows());
            h.mutable_rows()[0].exit_price -= 1.0;
        }
    };
    CHECK(run(late, base_spec(), 8));
    CHECK(refused);
    CHECK(NativeExecutionConsumerProbe::holds(late.consumer(), late.rows()));
    // The same amendment made after the row is final and named through
    // native_closed_rows_amended is folded again: the value equals the host's
    // that amended the row inside its notification. Unnamed, the running
    // digest keeps the row it took.
    Host named;
    named.bar_script = amend(false);
    CHECK(run(named, base_spec(), 6));
    const std::uint64_t before = named.broker_state_hash();
    named.mutable_rows()[0].exit_time += 1000;
    CHECK(named.broker_state_hash() == before);
    CHECK(!NativeExecutionConsumerProbe::holds(named.consumer(), named.rows()));
    named.native_closed_rows_amended(0);
    CHECK(named.broker_state_hash() == quiet->broker_state_hash());
    CHECK(NativeExecutionConsumerProbe::holds(named.consumer(), named.rows()));
    // A reorder of final rows, named from its first moved row (the source
    // host's same-bar exit order): the rows fold in their new order.
    const auto two_rows = [](Host& h, int bar) {
        if (bar == 1) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar == 2) h.submit(no::Request{no::Flatten{}, "out", ""});
        if (bar == 4) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar == 5) h.submit(no::Request{no::Flatten{}, "out", ""});
    };
    Host sorted;
    sorted.bar_script = two_rows;
    CHECK(run(sorted, base_spec(), 8));
    CHECK(sorted.rows().size() == 2);
    const std::uint64_t in_order = sorted.broker_state_hash();
    std::swap(sorted.mutable_rows()[0], sorted.mutable_rows()[1]);
    sorted.native_closed_rows_amended(0);
    CHECK(sorted.broker_state_hash() != in_order);
    CHECK(NativeExecutionConsumerProbe::holds(sorted.consumer(), sorted.rows()));
    std::swap(sorted.mutable_rows()[0], sorted.mutable_rows()[1]);
    sorted.native_closed_rows_amended(0);
    CHECK(sorted.broker_state_hash() == in_order);
    // Rows removed from the end need no name: the kernel only appends, so a
    // read over fewer rows, or a booking below the final mark, forgets them.
    // Both runs end folding exactly the rows left, so a Debug build completes.
    Host cleared;
    cleared.bar_script = [](Host& h, int bar) {
        if (bar == 1) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar == 2) h.submit(no::Request{no::Flatten{}, "out", ""});
        if (bar == 3) {
            (void)h.broker_state_hash();
            h.mutable_rows().clear();
            (void)h.broker_state_hash();
        }
        if (bar == 4) h.submit(no::Request{no::Transact{2.0}, "in", ""});
        if (bar == 5) h.submit(no::Request{no::Flatten{}, "out", ""});
    };
    CHECK(run(cleared, base_spec(), 8));
    CHECK(cleared.rows().size() == 1);
    CHECK(NativeExecutionConsumerProbe::holds(cleared.consumer(), cleared.rows()));
    Host regrown;
    regrown.bar_script = [](Host& h, int bar) {
        if (bar == 1) h.submit(no::Request{no::Transact{1.0}, "in", ""});
        if (bar == 2) h.submit(no::Request{no::Flatten{}, "out", ""});
        if (bar == 3) {
            (void)h.broker_state_hash();
            h.mutable_rows().clear();   // no read until a new row is booked
        }
        if (bar == 4) h.submit(no::Request{no::Transact{2.0}, "in", ""});
        if (bar == 5) h.submit(no::Request{no::Flatten{}, "out", ""});
    };
    CHECK(run(regrown, base_spec(), 8));
    CHECK(regrown.rows().size() == 1);
    CHECK(NativeExecutionConsumerProbe::holds(regrown.consumer(), regrown.rows()));
    CHECK(regrown.broker_state_hash() == cleared.broker_state_hash());
}

}  // namespace

int main() {
    every_folded_member_moves_the_value();
    live_tables_move_the_value();
    equal_state_equal_value();
    two_histories_one_state();
    cancelled_reason_is_kept();
    recording_and_reads_scale();
    closed_rows_are_final_after_their_notification();
    if (failures) {
        std::fprintf(stderr, "test_native_state_continuation: %d of %d checks failed\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_native_state_continuation: ok (%d checks)\n", checks);
    return 0;
}
