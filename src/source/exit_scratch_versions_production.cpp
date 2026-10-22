#include "exit_scratch_versions_production.hpp"

#include <pineforge/source/pine_adapter.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge::source::detail {
namespace {

// The tree has no exit payload codec. This local schema writes logical fields,
// never object images, padding, SessionKey pointers or shared_ptr control blocks.
// All scalars are LE64, doubles preserve their bits, and variable values have
// lengths/tags. Output borrows the caller only during put; input constructs the
// returned value using a fixed byte window. Neither is retained by the provider.
constexpr std::uint64_t payload_schema = 1;
enum : std::uint64_t {
    sequence_kind = 2, placement_kind = 3, request_kind = 4, pending_kind = 5
};

// These value types expose their complete state for hashing but have no
// restoration API. C++17 explicit instantiation permits naming these members
// without changing their headers/ABI. Keep the typed access local to this codec;
// no layout casts or public mutators that fabricate lifecycle actions.
template<class Tag, auto Member> struct PayloadMember {
    friend auto payload_member(Tag) { return Member; }
};
#define PF_EXIT_PAYLOAD_MEMBER(Tag, Owner, Name) \
    struct Tag { friend auto payload_member(Tag); }; \
    template struct PayloadMember<Tag, &Owner::Name>
PF_EXIT_PAYLOAD_MEMBER(LifecycleDefinition, exit_legs::Lifecycle, definition_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleTarget, exit_legs::Lifecycle, target_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleRevision, exit_legs::Lifecycle, revision_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleGenerations, exit_legs::Lifecycle, generations_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleRetirements, exit_legs::Lifecycle, retired_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleSuspension, exit_legs::Lifecycle, suspension_);
PF_EXIT_PAYLOAD_MEMBER(LifecycleReplay, exit_legs::Lifecycle, last_);
PF_EXIT_PAYLOAD_MEMBER(DefinitionIncarnation, exit_legs::Definition, incarnation_);
PF_EXIT_PAYLOAD_MEMBER(DefinitionRevision, exit_legs::Definition, revision_);
PF_EXIT_PAYLOAD_MEMBER(DefinitionPrices, exit_legs::Definition, value_);
PF_EXIT_PAYLOAD_MEMBER(ActivationBounds, ExitLegActivation, bounds_);
PF_EXIT_PAYLOAD_MEMBER(ActivationEvidence, compat::pine::ExitActivationPolicy, evidence_);
PF_EXIT_PAYLOAD_MEMBER(ExpansionCapture, ReservationExpansion, capture_);
PF_EXIT_PAYLOAD_MEMBER(GrowthOwner, ReservationGrowthSource, reservation_owner_);
PF_EXIT_PAYLOAD_MEMBER(AdmissionObservation, admission::Draft, observation_);
PF_EXIT_PAYLOAD_MEMBER(AdmissionReview, admission::Draft, review_);
PF_EXIT_PAYLOAD_MEMBER(AdmissionSizing, admission::Draft, sizing_revision_);
#undef PF_EXIT_PAYLOAD_MEMBER

template<class T, class Tag> decltype(auto) member(T& value, Tag tag) {
    return (value.*payload_member(tag));
}

template<class T> struct IsOptional : std::false_type {};
template<class T> struct IsOptional<std::optional<T>> : std::true_type {};
template<class T> struct IsVector : std::false_type {};
template<class T> struct IsVector<std::vector<T>> : std::true_type {};
template<class T> struct IsArray : std::false_type {};
template<class T, std::size_t N> struct IsArray<std::array<T, N>> : std::true_type {};
template<class T> struct IsVariant : std::false_type {};
template<class... T> struct IsVariant<std::variant<T...>> : std::true_type {};
template<class T> struct IsShared : std::false_type {};
template<class T> struct IsShared<std::shared_ptr<T>> : std::true_type {};
template<class T> inline constexpr bool unsupported_payload = false;

template<class Archive, class T> void payload_fields(Archive&, T&);

class PayloadOutput {
public:
    static constexpr bool reading = false;
    explicit PayloadOutput(std::uint64_t offset = 0, ScratchBuffer destination = {}) noexcept
        : skip_(offset), destination_(destination) {}

    template<class... T> void operator()(const T&... value) noexcept {
        (payload_fields(*this, value), ...);
    }
    void invalid() noexcept { status = ScratchStatus::programming_error; }
    void bytes(const char* data, std::size_t count) noexcept {
        if (status != ScratchStatus::ok) return;
        if (count > std::numeric_limits<std::uint64_t>::max() - size) {
            invalid();
            return;
        }
        size += count;
        if (skip_ >= count) { skip_ -= count; return; }
        const auto copied = std::min<std::size_t>(
            destination_.size, count - static_cast<std::size_t>(skip_));
        if (copied) {
            std::memcpy(destination_.data, data + static_cast<std::size_t>(skip_), copied);
            destination_.data += copied;
            destination_.size -= copied;
        }
        skip_ = 0;
    }
    template<class T> void scalar(const T& value) noexcept {
        if constexpr (std::is_enum_v<T>) {
            scalar(static_cast<std::underlying_type_t<T>>(value));
        } else {
            std::uint64_t word = 0;
            if constexpr (std::is_same_v<T, double>)
                std::memcpy(&word, &value, sizeof(word));
            else
                word = static_cast<std::uint64_t>(value);
            std::array<unsigned char, 8> encoded{};
            for (unsigned i = 0; i < encoded.size(); ++i)
                encoded[i] = static_cast<unsigned char>((word >> (i * 8)) & 0xff);
            bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        }
    }

    std::uint64_t size = 0;
    ScratchStatus status = ScratchStatus::ok;
private:
    std::uint64_t skip_;
    ScratchBuffer destination_;
};

class PayloadInput {
public:
    static constexpr bool reading = true;
    PayloadInput(const CommandScratchStore& store, ScratchHandle handle, std::uint64_t size) noexcept
        : store_(store), handle_(handle), size_(size) {}

    template<class... T> void operator()(T&... value) {
        (payload_fields(*this, value), ...);
    }
    [[noreturn]] void invalid() const {
        exit_scratch_failure(ScratchStatus::corrupt);
    }
    std::uint64_t remaining() const noexcept { return size_ - offset_; }
    void bytes(char* destination, std::size_t count) {
        if (count > remaining()) invalid();
        while (count) {
            if (begin_ == end_) {
                const auto available = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buffer_.size(), remaining()));
                exit_scratch_check(store_.read(handle_, ScratchField::value, offset_,
                    {buffer_.data(), available}), store_);
                begin_ = 0;
                end_ = available;
            }
            const auto copied = std::min(count, end_ - begin_);
            std::memcpy(destination, buffer_.data() + begin_, copied);
            destination += copied;
            count -= copied;
            begin_ += copied;
            offset_ += copied;
        }
    }
    template<class T> void scalar(T& value) {
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> underlying{};
            scalar(underlying);
            value = static_cast<T>(underlying);
        } else {
            std::array<unsigned char, 8> encoded{};
            bytes(reinterpret_cast<char*>(encoded.data()), encoded.size());
            std::uint64_t word = 0;
            for (unsigned i = 0; i < encoded.size(); ++i)
                word |= static_cast<std::uint64_t>(encoded[i]) << (i * 8);
            if constexpr (std::is_same_v<T, double>) {
                std::memcpy(&value, &word, sizeof(word));
            } else if constexpr (std::is_signed_v<T>) {
                std::int64_t signed_word = 0;
                std::memcpy(&signed_word, &word, sizeof(word));
                if (signed_word < std::numeric_limits<T>::min()
                    || signed_word > std::numeric_limits<T>::max()) invalid();
                value = static_cast<T>(signed_word);
            } else {
                if (word > std::numeric_limits<T>::max()) invalid();
                value = static_cast<T>(word);
            }
        }
    }
private:
    const CommandScratchStore& store_;
    ScratchHandle handle_;
    std::uint64_t size_;
    std::uint64_t offset_ = 0;
    std::array<std::byte, 512> buffer_{};
    std::size_t begin_ = 0, end_ = 0;
};

template<std::size_t I = 0, class Archive, class Variant>
void read_variant(Archive& archive, Variant& value, std::uint64_t index) {
    if constexpr (I < std::variant_size_v<Variant>) {
        if (index == I) archive(value.template emplace<I>());
        else read_variant<I + 1>(archive, value, index);
    } else {
        archive.invalid();
    }
}

template<class Archive, class T> void payload_fields(Archive& a, T& v) {
    using V = std::remove_const_t<T>;
    if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        static_assert(!std::is_floating_point_v<V> || std::is_same_v<V, double>);
        a.scalar(v);
    } else if constexpr (std::is_same_v<V, std::string>) {
        std::uint64_t size = v.size();
        a(size);
        if constexpr (Archive::reading) {
            if (size > a.remaining() || size > v.max_size()) a.invalid();
            v.resize(static_cast<std::size_t>(size));
        }
        a.bytes(v.data(), v.size());
    } else if constexpr (IsOptional<V>::value) {
        bool present = v.has_value();
        a(present);
        if constexpr (Archive::reading) {
            if (present) a(v.emplace());
            else v.reset();
        } else if (present) a(*v);
    } else if constexpr (IsShared<V>::value) {
        bool present = static_cast<bool>(v);
        a(present);
        if constexpr (Archive::reading) {
            if (present) {
                auto value = std::make_shared<std::remove_const_t<typename V::element_type>>();
                a(*value);
                v = std::move(value);
            } else v.reset();
        } else if (present) a(*v);
    } else if constexpr (IsVector<V>::value) {
        std::uint64_t size = v.size();
        a(size);
        if constexpr (Archive::reading) {
            // Every supported element has at least one scalar. Bound allocation
            // by the stored bytes before constructing any owning return value.
            if (size > v.max_size() || size > a.remaining() / 8) a.invalid();
            v.resize(static_cast<std::size_t>(size));
        }
        for (auto& value : v) a(value);
    } else if constexpr (IsArray<V>::value) {
        for (auto& value : v) a(value);
    } else if constexpr (IsVariant<V>::value) {
        std::uint64_t index = v.index();
        a(index);
        if constexpr (Archive::reading) read_variant(a, v, index);
        else {
            if (v.valueless_by_exception()) { a.invalid(); return; }
            std::visit([&](const auto& value) { a(value); }, v);
        }
    } else if constexpr (std::is_same_v<V, native_order::SessionKey>) {
        if constexpr (Archive::reading) {
            std::string value;
            a(value);
            v = value;
        } else a(v.string());
    } else if constexpr (std::is_same_v<V, native_order::RunIdentity>) {
        a(v.session_key, v.run_number);
    } else if constexpr (std::is_same_v<V, native_order::RequestHandle>) {
        a(v.run, v.incarnation);
    } else if constexpr (std::is_same_v<V, native_order::Request>) {
        a(v.intent, v.label, v.comment, v.trigger, v.capacity, v.owner, v.group, v.anchor);
    } else if constexpr (std::is_same_v<V, native_order::Transact>
                      || std::is_same_v<V, native_order::ReverseTo>) {
        a(v.signed_units);
    } else if constexpr (std::is_same_v<V, native_order::Reduce>) {
        a(v.size);
    } else if constexpr (std::is_same_v<V, native_order::ExplicitUnits>
                      || std::is_same_v<V, native_order::PointBudget>) {
        a(v.units);
    } else if constexpr (std::is_same_v<V, native_order::ScopeFraction>) {
        a(v.fraction, v.claim, v.basis);
    } else if constexpr (std::is_same_v<V, native_order::HostSized>) {
        a(v.kind, v.side);
    } else if constexpr (std::is_same_v<V, native_order::Sized>) {
        a(v.side, v.basis, v.time, v.price, v.grid_policy, v.reserve_percent_fee);
    } else if constexpr (std::is_same_v<V, native_order::CashValue>) {
        a(v.cash);
    } else if constexpr (std::is_same_v<V, native_order::EquityFraction>) {
        a(v.fraction);
    } else if constexpr (std::is_same_v<V, native_order::Limit>) {
        a(v.price, v.fill_through);
    } else if constexpr (std::is_same_v<V, native_order::Stop>) {
        a(v.price);
    } else if constexpr (std::is_same_v<V, native_order::StopLimit>) {
        a(v.stop, v.limit);
    } else if constexpr (std::is_same_v<V, native_order::Trail>) {
        a(v.offset, v.arm_price, v.ticks, v.best_seed);
    } else if constexpr (std::is_same_v<V, native_order::TrailTicks>) {
        a(v.ticks);
    } else if constexpr (std::is_same_v<V, native_order::FromOwnerFill>) {
        a(v.offset, v.ticks, v.rounding);
    } else if constexpr (std::is_same_v<V, native_order::WaitForApplied>) {
        a(v.parent, v.visibility, v.first_match, v.scope);
    } else if constexpr (std::is_same_v<V, native_order::BindOpening>) {
        a(v.opening, v.cycle);
    } else if constexpr (std::is_same_v<V, native_order::BindOpenings>) {
        a(v.openings, v.cycle);
    } else if constexpr (std::is_same_v<V, native_order::BindCohort>) {
        a(v.cohort.value);
    } else if constexpr (std::is_same_v<V, native_order::Member>) {
        a(v.group, v.cohort, v.effect);
    } else if constexpr (std::is_same_v<V, PlacementSnapshot>) {
        a(v.family, v.source_id, v.from_entry, v.comment, v.oca_name, v.oca_type, v.qty_type,
          v.requested_qty, v.projection_remaining_qty, v.qty_percent, v.is_long,
          v.immediately, v.opening, v.filled_exit, v.deferred_cohort, v.reservation_deferred_to_pending_entry,
          v.void_issue, v.void_global_issue, v.pending_bound_exit, v.per_entry_origin,
          v.per_entry_units, v.crosses_zero, v.follow_up_fill, v.waypoint_margin_call,
          v.fixed_exit_reservation, v.frozen_market_instruction, v.frozen_market_own_units,
          v.frozen_market_transaction_units, v.frozen_market_targeted_close,
          v.frozen_market_target_was_long, v.direction_gate, v.affordability_policy_active,
          v.affordability_close_only, v.rounded_signal_cost_close_only,
          v.affordability_keep_mc_close_surplus, v.reverse_to, v.replaced_opening,
          v.replacement_predecessor_market, v.terms_priced_reverse, v.frozen_reversal_transaction,
          v.placement_cycle, v.sequential_group, v.sequential_rank, v.has_full_entry_bracket,
          v.paired_reversal_parent, v.preserved_by_close_all, v.preserved_close_all_bar,
          v.bracket_origin, v.source_sequence, v.command_ordinal, v.placement_open_epoch,
          v.command_sequence, v.close_callsite_token, v.close_batch_calls, v.close_first_id,
          v.close_first_target, v.close_first_ledger_consumed, v.close_first_carry_valid,
          v.close_first_carry_qty, v.close_retire_ledger_whole, v.close_pending_later_qty,
          v.placement_script_open_ms, v.placement_sub_open_ms, v.projection_created_bar,
          v.projection_created_bar_pinned, v.projection_position_side, v.projection_after_close,
          v.close_first_entry, v.signal_price_refused, v.projection_over_pyramiding,
          v.projection_opposite_market_predecessor, v.projection_predecessor,
          v.chain_origin_sequence, v.recreated_after_named_cancelled_entry_incarnation,
          v.named_cancel_surviving_exit_incarnation, v.retained_parent_topology,
          v.defer_until_post_parent_calculation, v.projection_predecessor_market,
          v.projection_predecessor_exit, v.projection_created_during_coof,
          v.projection_coof_at_terminal, v.projection_coof_mid_bar, v.forced_execution_price,
          v.forced_waypoint_print, v.coof_open_stop_next_waypoint, v.projection_tv_carry_qty,
          v.projection_default_stop_equity, v.projection_default_stop_signal_close,
          v.projection_explicit_equity, v.projection_explicit_signal_close,
          v.projection_affordability_equity, v.projection_affordability_signal_price,
          v.projection_affordability_held_qty, v.sizing, v.exit_levels, v.trail_activation_level,
          v.retained_trail_best, v.market_admission, v.birth, v.birth_reach, v.leg_activation,
          v.exit_activation, v.legs, v.restored_after_margin, v.reservation_expansion,
          v.reservation_growth_source, v.reservation_growth_owner_incarnation,
          v.stop_limit_activated, v.coof_cascade_seg_i, v.coof_cascade_inflight_fires,
          v.paired_flat_market_candidate, v.paired_flat_market_own_qty,
          v.paired_flat_market_signal_close, v.paired_flat_market_signal_equity,
          v.paired_flat_market_signal_margin_pct, v.paired_flat_market_signal_pointvalue,
          v.paired_flat_market_signal_fx, v.paired_flat_market_peer_seq,
          v.paired_flat_market_transaction_qty, v.signal_close_mc_bar,
          v.signal_close_mc_entry_incarnation, v.signal_close_mc_fill_seq,
          v.signal_close_mc_remaining_qty, v.pooc_global_full_exit_dynamic_qty,
          v.pooc_global_full_exit_tracks_bound_adds, v.pooc_global_full_exit_bound_add,
          v.cancellation);
    } else if constexpr (std::is_same_v<V, PineExitLevels>) {
        a(v.limit, v.stop, v.trail_points, v.trail_offset, v.trail_price,
          v.profit_ticks, v.loss_ticks);
    } else if constexpr (std::is_same_v<V, PineSizingSnapshot>) {
        a(v.equity, v.price, v.fx, v.mark, v.frozen_units, v.at_fill, v.strategy_equity);
    } else if constexpr (std::is_same_v<V, PineCancellationReceipt>) {
        a(v.cause, v.state, v.close_claim_release, v.source_incarnation, v.source_sequence,
          v.target_incarnation, v.target_owner, v.target_revision,
          v.close_claim_consumed, v.close_claim_retired);
    } else if constexpr (std::is_same_v<V, BirthCursor>) {
        auto domain = v.domain();
        auto position = v.position();
        auto index = v.index();
        auto count = v.count();
        a(domain, position, index, count);
        if constexpr (Archive::reading) {
            if (position == BirthCursorPosition::None) {
                if (domain != BirthCursorDomain::None || index != -1 || count != 0) a.invalid();
                v = {};
            } else if (position == BirthCursorPosition::Point) {
                v = BirthCursor::point(domain, index, count);
            } else if (position == BirthCursorPosition::Segment) {
                v = BirthCursor::segment(domain, index, count);
            } else a.invalid();
        }
    } else if constexpr (std::is_same_v<V, OrderBirth>) {
        auto cause = v.cause();
        auto bar = v.bar();
        auto timestamp = v.timestamp();
        auto cursor = v.cursor();
        auto price = v.cursor_price();
        auto first = v.first_fill();
        auto last = v.last_fill();
        auto evaluation = v.evaluation_ordinal();
        a(cause, bar, timestamp, cursor, price, first, last, evaluation);
        if constexpr (Archive::reading) {
            switch (cause) {
            case OrderBirthCause::Unattributed: v = {}; break;
            case OrderBirthCause::DirectCommand:
                v = OrderBirth::direct_command(bar, timestamp); break;
            case OrderBirthCause::ChartEvaluation:
                v = OrderBirth::chart_evaluation(bar, timestamp); break;
            case OrderBirthCause::FillEvaluation:
                v = OrderBirth::fill_evaluation(
                    bar, timestamp, cursor, price, first, last, evaluation); break;
            default: a.invalid();
            }
        }
    } else if constexpr (std::is_same_v<V, ExitLegActivation>) {
        a(member(v, ActivationBounds{}));
    } else if constexpr (std::is_same_v<V, ExitLegActivationBounds>) {
        a(v.position_cycle, v.stop_first_bar, v.limit_first_bar);
    } else if constexpr (std::is_same_v<V, compat::pine::ExitActivationPolicy>) {
        a(member(v, ActivationEvidence{}));
    } else if constexpr (std::is_same_v<V, compat::pine::ExitPlacementEvidence>) {
        a(v.position_cycle, v.entry_bar, v.direction, v.cursor_price,
          v.stop_level, v.limit_level, v.limit_continuation);
    } else if constexpr (std::is_same_v<V, compat::pine::LimitContinuation>) {
        a(v.cause, v.observed_fill_sequence);
    } else if constexpr (std::is_same_v<V, ReservationExpansion>) {
        a(member(v, ExpansionCapture{}));
    } else if constexpr (std::is_same_v<V, ReservationExpansionCapture>) {
        a(v.position_cycle, v.side, v.first_later_admission);
    } else if constexpr (std::is_same_v<V, ReservationGrowthSource>) {
        a(member(v, GrowthOwner{}));
    } else if constexpr (std::is_same_v<V, admission::Draft>) {
        a(member(v, AdmissionObservation{}), member(v, AdmissionReview{}),
          member(v, AdmissionSizing{}));
    } else if constexpr (std::is_same_v<V, admission::CommandObservation>) {
        a(v.command, v.kind, v.birth, v.id, v.requested_quantity, v.quantity_type,
          v.buy, v.prices, v.oca_name, v.oca_type, v.configuration, v.bar,
          v.placement_side, v.placement_cycle, v.prior_close_quantity, v.held_quantity,
          v.held_entries, v.realized_equity, v.placement_equity, v.signal_close,
          v.quantized_fixed_quantity, v.original_sizing, v.explicit_equity, v.explicit_price);
    } else if constexpr (std::is_same_v<V, admission::Configuration>) {
        a(v.process_on_close, v.calc_on_fills, v.magnifier, v.fill_recalculation, v.scheduler,
          v.slippage, v.pyramiding, v.default_quantity_type, v.default_quantity_value,
          v.long_margin, v.short_margin, v.commission_value, v.commission_type,
          v.pointvalue, v.fx, v.quantity_step, v.mintick, v.risk_direction, v.loss_days_limit,
          v.drawdown_limit, v.intraday_loss_limit, v.position_limit, v.fill_cap_active, v.risk_halted);
    } else if constexpr (std::is_same_v<V, admission::PriceRequest>) {
        a(v.limit, v.stop);
    } else if constexpr (std::is_same_v<V, admission::SizingObservation>) {
        a(v.quantity, v.equity, v.price, v.mark, v.fx);
    } else if constexpr (std::is_same_v<V, admission::ReviewReceipt>) {
        a(v.sequence, v.checkpoint, v.bar, v.target_command);
    } else if constexpr (std::is_same_v<V, admission::SizingRevision>) {
        a(v.sequence, v.cause_fill, v.bar, v.target_command);
    } else if constexpr (std::is_same_v<V, exit_legs::Lifecycle>) {
        a(member(v, LifecycleTarget{}), member(v, LifecycleRevision{}),
          member(v, LifecycleDefinition{}), member(v, LifecycleGenerations{}),
          member(v, LifecycleRetirements{}), member(v, LifecycleSuspension{}),
          member(v, LifecycleReplay{}));
    } else if constexpr (std::is_same_v<V, exit_legs::Definition>) {
        a(member(v, DefinitionIncarnation{}), member(v, DefinitionRevision{}),
          member(v, DefinitionPrices{}));
    } else if constexpr (std::is_same_v<V, exit_legs::Prices>) {
        a(v.limit_price, v.stop_price, v.trail_points, v.trail_price,
          v.trail_offset, v.profit_ticks, v.loss_ticks);
    } else if constexpr (std::is_same_v<V, exit_legs::Target>) {
        a(v.incarnation, v.owner);
    } else if constexpr (std::is_same_v<V, exit_legs::Frame>) {
        a(v.event, v.bar, v.domain, v.phase);
    } else if constexpr (std::is_same_v<V, exit_legs::Retirement>) {
        a(v.generation, v.cause);
    } else if constexpr (std::is_same_v<V, exit_legs::Barrier>) {
        a(v.requested, v.target, v.revision);
    } else if constexpr (std::is_same_v<V, exit_legs::ObservationWindow>) {
        a(v.excluded, v.best, v.prefix);
    } else if constexpr (std::is_same_v<V, exit_legs::Replacement>) {
        a(v.queue_predecessor, v.revival_definition, v.release);
    } else if constexpr (std::is_same_v<V, exit_legs::Suspension>) {
        a(v.cause, v.legs, v.hold, v.revival_definition, v.replacement, v.window);
    } else if constexpr (std::is_same_v<V, exit_legs::Action>) {
        a(v.target, v.expected_revision, v.cause, v.operation);
    } else if constexpr (std::is_same_v<V, exit_legs::BindOwner>) {
        a(v.owner);
    } else if constexpr (std::is_same_v<V, exit_legs::Suspend>) {
        a(v.legs, v.hold, v.window, v.retire);
    } else if constexpr (std::is_same_v<V, exit_legs::StageReplacement>) {
        a(v.relation);
    } else if constexpr (std::is_same_v<V, exit_legs::Restore>
                      || std::is_same_v<V, exit_legs::Cancel>) {
        a(v.legs);
    } else if constexpr (std::is_same_v<V, exit_legs::CompleteBarrier>) {
        a(v.completed, v.requested);
    } else if constexpr (std::is_same_v<V, exit_legs::Observe>) {
        a(v.high, v.low, v.direction, v.fold);
    } else if constexpr (std::is_empty_v<V>) {
        // Quantity-free intents and marker alternatives have no fields.
    } else {
        static_assert(unsupported_payload<V>, "exit payload fields must be enumerated");
    }
}

template<class... T> class PayloadSource final : public ScratchByteSource {
public:
    explicit PayloadSource(const T&... values) : values_(values...) {
        PayloadOutput output;
        emit(output);
        exit_scratch_check(output.status);
        size_ = output.size;
    }
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > size_ || destination.size > size_ - offset
            || (destination.size && !destination.data))
            return ScratchStatus::programming_error;
        PayloadOutput output(offset, destination);
        emit(output);
        return output.status;
    }
private:
    void emit(PayloadOutput& output) const noexcept {
        output(payload_schema);
        std::apply([&](const auto&... value) { output(value...); }, values_);
    }
    std::tuple<const T&...> values_;
    std::uint64_t size_ = 0;
};

template<class... T> void read_payload(const CommandScratchStore& store, ScratchHandle version,
                                      std::uint64_t kind, T&... values) {
    ScratchRecordInfo info{};
    exit_scratch_check(store.point(version, info), store);
    ExitScratchKeyWords words{};
    if (info.key_bytes != sizeof(words))
        exit_scratch_failure(ScratchStatus::programming_error);
    exit_scratch_check(store.read(version, ScratchField::key, 0,
        {reinterpret_cast<std::byte*>(words.data()), sizeof(words)}), store);
    if (words[0] == 0 || words[1] != kind || words[3] != 0
        || (kind == sequence_kind ? words[2] != 0 : words[2] == 0))
        exit_scratch_failure(ScratchStatus::programming_error);
    PayloadInput input(store, version, info.value_bytes);
    std::uint64_t schema = 0;
    input(schema);
    if (schema != payload_schema) input.invalid();
    input(values...);
    if (input.remaining()) input.invalid();
}

template<class... T> std::uint64_t put_version(CommandScratchStore& store,
        std::uint64_t scope, std::uint64_t kind, const T&... values) {
    if (!scope) exit_scratch_failure(ScratchStatus::programming_error);
    ExitScratchContext context{store, {}};
    const ExitScratchKey counter_key(scope, sequence_kind);
    const auto counter = exit_scratch_seek(context, counter_key);
    std::uint64_t sequence = 0;
    if (counter && exit_scratch_words(context, counter)
            == ExitScratchKeyWords{scope, sequence_kind, 0, 0})
        read_payload(store, counter, sequence_kind, sequence);
    if (sequence == std::numeric_limits<std::uint64_t>::max())
        exit_scratch_failure(ScratchStatus::programming_error);
    ++sequence;
    const PayloadSource<std::uint64_t> counter_value(sequence);
    const PayloadSource<T...> payload(values...);
    const ExitScratchKey key(scope, kind, sequence);
    ScratchHandle version = 0;
    exit_scratch_edit(context, [&] {
        ScratchHandle ignored = 0;
        auto status = store.put(counter_key, counter_value, ignored);
        if (status == ScratchStatus::ok) status = store.put(key, payload, version);
        return status;
    });
    return version;
}

} // namespace

std::uint64_t ExitScratchVersionsProduction::placement(
        std::uint64_t scope, const PlacementSnapshot& value) {
    return put_version(store_, scope, placement_kind, value);
}

PlacementSnapshot ExitScratchVersionsProduction::placement(std::uint64_t version) const {
    PlacementSnapshot value;
    read_payload(store_, version, placement_kind, value);
    return value;
}

std::uint64_t ExitScratchVersionsProduction::request(
        std::uint64_t scope, const native_order::Request& value) {
    return put_version(store_, scope, request_kind, value);
}

native_order::Request ExitScratchVersionsProduction::request(std::uint64_t version) const {
    native_order::Request value;
    read_payload(store_, version, request_kind, value);
    return value;
}

std::uint64_t ExitScratchVersionsProduction::pending(std::uint64_t scope,
        const native_order::Request& request, const PlacementSnapshot& placement,
        const std::string& key) {
    return put_version(store_, scope, pending_kind, request, placement, key);
}

void ExitScratchVersionsProduction::pending(std::uint64_t version,
        native_order::Request& request, PlacementSnapshot& placement, std::string& key) const {
    native_order::Request saved_request;
    PlacementSnapshot saved_placement;
    std::string saved_key;
    read_payload(store_, version, pending_kind, saved_request, saved_placement, saved_key);
    request = std::move(saved_request);
    placement = std::move(saved_placement);
    key = std::move(saved_key);
}

native_order::Owner ExitScratchVersionsProduction::freeze_openings(
        const native_order::RunIdentity& run, std::int64_t cycle, std::uint64_t count,
        void* context, OpeningReader read) {
    if (!read || cycle <= 0)
        exit_scratch_failure(ScratchStatus::programming_error);
    // The existing Owner ABI carries fixed membership as a native-owned vector.
    // Transfer that value to the native caller, never retain it in this provider
    // or substitute a live BindCohort whose membership could change.
    native_order::BindOpenings owner;
    owner.cycle = cycle;
    if (count > owner.openings.max_size())
        exit_scratch_failure(ScratchStatus::programming_error);
    owner.openings.resize(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        auto& handle = owner.openings[static_cast<std::size_t>(index)];
        exit_scratch_check(read(context, index, handle), store_);
        if (handle.run != run || !handle.incarnation)
            exit_scratch_failure(ScratchStatus::programming_error);
    }
    return native_order::Owner{std::move(owner)};
}

void ExitScratchVersionsProduction::release_scope(std::uint64_t scope) noexcept {
    ExitScratchContext context{store_, {}};
    const ExitScratchKey key(scope, sequence_kind);
    for (;;) {
        const auto handle = exit_scratch_seek(context, key);
        if (!handle) break;
        const auto words = exit_scratch_words(context, handle);
        if (words[0] != scope || words[1] > pending_kind) break;
        // Like ExitScratchScope's destructor, an unrecoverable backing failure
        // cannot be ignored or escape this noexcept lifetime boundary.
        exit_scratch_edit(context, [&] { return store_.erase(handle); });
    }
}

} // namespace pineforge::source::detail
