// Included once by pine_adapter.cpp. Kept separate so the complete payload
// schema can be reviewed independently of source trading policy.
namespace pineforge::source::detail {
namespace {
using pineforge::detail::PayloadSchema;
using pineforge::detail::PayloadType;
using pineforge::detail::ScratchSpanSource;
using pineforge::detail::ScratchField;
using pineforge::detail::ScratchDurability;

class PayloadSource final : public ScratchByteSource {
public:
    PayloadSource(PayloadType type, std::string_view name, const ScratchByteSource& bytes) noexcept
        : name_(name), bytes_(bytes) {
        pineforge::detail::payload_encode(static_cast<std::uint64_t>(type), header_.data());
        pineforge::detail::payload_encode(name.size(), header_.data() + 8);
    }
    std::uint64_t size() const noexcept override { return 16 + name_.size() + bytes_.size(); }
    ScratchStatus read(std::uint64_t offset, pineforge::detail::ScratchBuffer out) const noexcept override {
        if (offset > size() || out.size > size() - offset || (out.size && !out.data))
            return ScratchStatus::programming_error;
        const std::array<pineforge::detail::ScratchBytes, 2> prefix{{
            {header_.data(), header_.size()},
            {reinterpret_cast<const std::byte*>(name_.data()), name_.size()}
        }};
        for (const auto& part : prefix) {
            if (offset >= part.size) { offset -= part.size; continue; }
            const auto n = std::min<std::uint64_t>(out.size, part.size - offset);
            if (n) std::memcpy(out.data, part.data + offset, static_cast<std::size_t>(n));
            if (n) out.data += n;
            out.size -= static_cast<std::size_t>(n);
            offset = 0;
            if (!out.size) return ScratchStatus::ok;
        }
        return bytes_.read(offset, out);
    }
private:
    std::array<std::byte, 16> header_{};
    std::string_view name_;
    const ScratchByteSource& bytes_;
};

class PayloadWriter {
public:
    PayloadWriter(ExitScratchContext& context, std::uint64_t scope, PayloadSchema schema)
        : context_(context), scope_(scope), schema_(schema) {
        if (!scope || context.next_scope == std::numeric_limits<std::uint64_t>::max()) {
            status = ScratchStatus::programming_error;
            return;
        }
        status = context.store.begin();
        if (status != ScratchStatus::ok) return;
        active_ = true;
        root_key_ = {scope, pineforge::detail::payload_root_kind, 0, ++context.next_scope};
        publish_root();
    }
    ~PayloadWriter() { if (active_) context_.store.rollback(); }
    template<class T> void member(std::string_view name, const T& value);
    void emit(std::string_view name, PayloadType type, const ScratchByteSource& bytes) {
        if (status != ScratchStatus::ok) return;
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - 16 - name.size()
            || fields_ == std::numeric_limits<std::uint64_t>::max()) {
            status = ScratchStatus::programming_error;
            return;
        }
        const std::array<std::uint64_t, 4> words{
            scope_, pineforge::detail::payload_field_kind, version_, fields_};
        ScratchSpanSource key({reinterpret_cast<const std::byte*>(words.data()), sizeof(words)});
        PayloadSource value(type, name, bytes);
        ScratchHandle ignored = 0;
        status = context_.store.put(key, value, ignored);
        if (status == ScratchStatus::ok) ++fields_;
    }
    void marker(std::string_view name, PayloadType type) {
        ScratchSpanSource empty({nullptr, 0});
        emit(name, type, empty);
    }
    void word(std::string_view name, PayloadType type, std::uint64_t word) {
        std::array<std::byte, 8> bytes{};
        pineforge::detail::payload_encode(word, bytes.data());
        ScratchSpanSource source({bytes.data(), bytes.size()});
        emit(name, type, source);
    }
    // Lifecycle's published logical traversal, without its owning Exact sink.
    void u(std::uint64_t value) { word("value", PayloadType::unsigned_integer, value); }
    void i(std::int64_t value) { word("value", PayloadType::signed_integer, static_cast<std::uint64_t>(value)); }
    void b(bool value) { word("value", PayloadType::boolean, value ? 1 : 0); }
    void d(double value) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        word("value", PayloadType::real, bits);
    }
    void borrowed_text(std::string_view name, const ScratchByteSource& source) {
        marker(name, PayloadType::object_begin);
        word("size", PayloadType::unsigned_integer, source.size());
        std::array<std::byte, 128> buffer{};
        for (std::uint64_t offset = 0; status == ScratchStatus::ok && offset < source.size();) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(
                buffer.size(), source.size() - offset));
            status = source.read(offset, {buffer.data(), n});
            if (status != ScratchStatus::ok) break;
            ScratchSpanSource chunk({buffer.data(), n});
            emit("chunk", PayloadType::text, chunk);
            offset += n;
        }
        marker(name, PayloadType::object_end);
    }
    ScratchStatus finish(ScratchHandle& out) {
        if (!active_) return status;
        if (status == ScratchStatus::ok) publish_root();
        if (status == ScratchStatus::ok)
            status = context_.store.commit(ScratchDurability::buffered);
        if (status != ScratchStatus::ok) {
            const auto rollback = context_.store.rollback();
            if (rollback == ScratchStatus::ok) active_ = false;
            else return rollback;
            return status;
        }
        active_ = false;
        out = version_;
        return ScratchStatus::ok;
    }
    ScratchStatus status = ScratchStatus::ok;
private:
    void publish_root() {
        std::array<std::byte, 24> bytes{};
        pineforge::detail::payload_encode(static_cast<std::uint64_t>(schema_), bytes.data());
        pineforge::detail::payload_encode(scope_, bytes.data() + 8);
        pineforge::detail::payload_encode(fields_, bytes.data() + 16);
        ScratchSpanSource key({reinterpret_cast<const std::byte*>(root_key_.data()), sizeof(root_key_)});
        ScratchSpanSource value({bytes.data(), bytes.size()});
        status = context_.store.put(key, value, version_);
    }
    ExitScratchContext& context_;
    std::uint64_t scope_;
    PayloadSchema schema_;
    std::array<std::uint64_t, 4> root_key_{};
    ScratchHandle version_ = 0;
    std::uint64_t fields_ = 0;
    bool active_ = false;
};

template<class T> void encode_payload(PayloadWriter&, const T&) {
    static_assert(std::is_empty_v<T>, "payload member needs an explicit logical codec");
}
template<class T> void encode_payload(PayloadWriter& w, const std::optional<T>& value) {
    w.member("present", value.has_value());
    if (value) w.member("value", *value);
}
template<class... T> void encode_payload(PayloadWriter& w, const std::variant<T...>& value) {
    if (value.valueless_by_exception()) { w.status = ScratchStatus::programming_error; return; }
    w.member("tag", static_cast<std::uint64_t>(value.index()));
    std::visit([&](const auto& active) { w.member("value", active); }, value);
}
void encode_payload(PayloadWriter& w, const native_order::SessionKey& v) {
    w.member("bytes", v.string());
}
void encode_payload(PayloadWriter& w, const native_order::RunIdentity& v) {
    w.member("session_key", v.session_key); w.member("run_number", v.run_number);
}
void encode_payload(PayloadWriter& w, const native_order::RequestHandle& v) {
    w.member("run", v.run); w.member("incarnation", v.incarnation);
}

#define PAYLOAD_MEMBERS(Type, ...) \
    void encode_payload(PayloadWriter& w, const Type& v) { __VA_ARGS__ }
#define M(field) w.member(#field, v.field);
PAYLOAD_MEMBERS(native_order::Transact, M(signed_units))
PAYLOAD_MEMBERS(native_order::ReverseTo, M(signed_units))
PAYLOAD_MEMBERS(native_order::HostSized, M(kind) M(side))
PAYLOAD_MEMBERS(native_order::CashValue, M(cash))
PAYLOAD_MEMBERS(native_order::EquityFraction, M(fraction))
PAYLOAD_MEMBERS(native_order::Sized, M(side) M(basis) M(time) M(price) M(grid_policy) M(reserve_percent_fee))
PAYLOAD_MEMBERS(native_order::ExplicitUnits, M(units))
PAYLOAD_MEMBERS(native_order::ScopeFraction, M(fraction) M(claim) M(basis))
PAYLOAD_MEMBERS(native_order::Reduce, M(size))
PAYLOAD_MEMBERS(native_order::Limit, M(price) M(fill_through))
PAYLOAD_MEMBERS(native_order::Stop, M(price))
PAYLOAD_MEMBERS(native_order::StopLimit, M(stop) M(limit))
PAYLOAD_MEMBERS(native_order::TrailTicks, M(ticks))
PAYLOAD_MEMBERS(native_order::Trail, M(offset) M(arm_price) M(ticks) M(best_seed))
PAYLOAD_MEMBERS(native_order::PointBudget, M(units))
PAYLOAD_MEMBERS(native_order::WaitForApplied, M(parent) M(visibility) M(first_match) M(scope))
PAYLOAD_MEMBERS(native_order::BindOpening, M(opening) M(cycle))
PAYLOAD_MEMBERS(native_order::CohortHandle, M(value))
PAYLOAD_MEMBERS(native_order::BindCohort, M(cohort))
PAYLOAD_MEMBERS(native_order::Member, M(group) M(cohort) M(effect))
PAYLOAD_MEMBERS(native_order::FromOwnerFill, M(offset) M(ticks) M(rounding))
void encode_payload(PayloadWriter& w, const native_order::BindOpenings& v) {
    M(cycle)
    const auto count = native_order::opening_count(v.openings, v.external_openings.get());
    w.member("count", count);
    for (std::uint64_t i = 0; w.status == ScratchStatus::ok && i != count; ++i) {
        native_order::RequestHandle handle;
        w.status = native_order::opening_at(v.openings, v.external_openings.get(), i, handle);
        if (w.status == ScratchStatus::ok) w.member("opening", handle);
    }
}
PAYLOAD_MEMBERS(native_order::Request,
    M(intent) M(label) M(comment) M(trigger) M(capacity) M(owner) M(group) M(anchor))

void encode_payload(PayloadWriter& w, const OrderBirth& v) {
    w.member("cause", v.cause()); w.member("bar", v.bar()); w.member("timestamp", v.timestamp());
    w.member("cursor_domain", v.cursor().domain()); w.member("cursor_position", v.cursor().position());
    w.member("cursor_index", v.cursor().index()); w.member("cursor_count", v.cursor().count());
    w.member("cursor_price", v.cursor_price()); w.member("first_fill", v.first_fill());
    w.member("last_fill", v.last_fill()); w.member("evaluation_ordinal", v.evaluation_ordinal());
}
PAYLOAD_MEMBERS(PineSizingSnapshot, M(equity) M(price) M(fx) M(mark) M(frozen_units) M(at_fill) M(strategy_equity))
PAYLOAD_MEMBERS(PineExitLevels,
    M(limit) M(stop) M(trail_points) M(trail_offset) M(trail_price) M(profit_ticks) M(loss_ticks))
PAYLOAD_MEMBERS(PineCancellationReceipt,
    M(cause) M(state) M(close_claim_release) M(source_incarnation) M(source_sequence)
    M(target_incarnation) M(target_owner) M(target_revision) M(close_claim_consumed) M(close_claim_retired))
PAYLOAD_MEMBERS(admission::Configuration,
    M(process_on_close) M(calc_on_fills) M(magnifier) M(fill_recalculation) M(scheduler)
    M(slippage) M(pyramiding) M(default_quantity_type) M(default_quantity_value)
    M(long_margin) M(short_margin) M(commission_value) M(commission_type) M(pointvalue)
    M(fx) M(quantity_step) M(mintick) M(risk_direction) M(loss_days_limit)
    M(drawdown_limit) M(intraday_loss_limit) M(position_limit) M(fill_cap_active) M(risk_halted))
PAYLOAD_MEMBERS(admission::PriceRequest, M(limit) M(stop))
PAYLOAD_MEMBERS(admission::SizingObservation, M(quantity) M(equity) M(price) M(mark) M(fx))
PAYLOAD_MEMBERS(admission::CommandObservation,
    M(command) M(kind) M(birth) M(id) M(requested_quantity) M(quantity_type) M(buy)
    M(prices) M(oca_name) M(oca_type) M(configuration) M(bar) M(placement_side)
    M(placement_cycle) M(prior_close_quantity) M(held_quantity) M(held_entries)
    M(realized_equity) M(placement_equity) M(signal_close) M(quantized_fixed_quantity)
    M(original_sizing) M(explicit_equity) M(explicit_price))
PAYLOAD_MEMBERS(admission::ReviewReceipt, M(sequence) M(checkpoint) M(bar) M(target_command))
PAYLOAD_MEMBERS(admission::SizingRevision, M(sequence) M(cause_fill) M(bar) M(target_command))
void encode_payload(PayloadWriter& w, const admission::Draft& v) {
    w.member("observation_present", bool(v.observation()));
    if (v.observation()) w.member("observation", *v.observation());
    w.member("review", v.review()); w.member("sizing_revision", v.sizing_revision());
}
PAYLOAD_MEMBERS(ExitLegActivationBounds, M(position_cycle) M(stop_first_bar) M(limit_first_bar))
void encode_payload(PayloadWriter& w, const ExitLegActivation& v) { w.member("bounds", v.bounds()); }
PAYLOAD_MEMBERS(compat::pine::LimitContinuation, M(cause) M(observed_fill_sequence))
PAYLOAD_MEMBERS(compat::pine::ExitPlacementEvidence,
    M(position_cycle) M(entry_bar) M(direction) M(cursor_price) M(stop_level) M(limit_level)
    M(limit_continuation))
void encode_payload(PayloadWriter& w, const compat::pine::ExitActivationPolicy& v) {
    w.member("evidence", v.evidence());
}
// lifecycle_v1's complete published traversal, including retired generations
// and the last replay receipt. Its ReplaySink emits action-double bits as
// unsigned leaves; preserve those words rather than canonicalizing NaN/zero.
void encode_payload(PayloadWriter& w, const exit_legs::Lifecycle& v) { v.visit(w); }
PAYLOAD_MEMBERS(ReservationExpansionCapture, M(position_cycle) M(side) M(first_later_admission))
void encode_payload(PayloadWriter& w, const ReservationExpansion& v) { w.member("capture", v.capture()); }
void encode_payload(PayloadWriter& w, const ReservationGrowthSource& v) {
    w.member("reservation_owner", v.reservation_owner());
}
PAYLOAD_MEMBERS(PlacementSnapshot,
    M(family) M(source_id) M(from_entry) M(comment) M(oca_name) M(oca_type) M(qty_type)
    M(requested_qty) M(projection_remaining_qty) M(qty_percent) M(is_long) M(immediately)
    M(opening) M(deferred_cohort) M(reservation_deferred_to_pending_entry)
    M(void_issue) M(void_global_issue) M(pending_bound_exit) M(per_entry_origin) M(per_entry_units)
    M(crosses_zero) M(follow_up_fill) M(waypoint_margin_call) M(fixed_exit_reservation)
    M(frozen_market_instruction) M(frozen_market_own_units) M(frozen_market_transaction_units)
    M(frozen_market_targeted_close) M(frozen_market_target_was_long)
    M(direction_gate) M(affordability_policy_active) M(affordability_close_only)
    M(rounded_signal_cost_close_only) M(affordability_keep_mc_close_surplus)
    M(reverse_to) M(replaced_opening) M(replacement_predecessor_market) M(terms_priced_reverse)
    M(frozen_reversal_transaction) M(placement_cycle) M(sequential_group) M(sequential_rank)
    M(has_full_entry_bracket) M(paired_reversal_parent) M(preserved_by_close_all)
    M(preserved_close_all_bar) M(bracket_origin) M(source_sequence) M(command_ordinal)
    M(placement_open_epoch) M(command_sequence) M(close_callsite_token) M(close_batch_calls)
    M(close_first_id) M(close_first_target) M(close_first_ledger_consumed)
    M(close_first_carry_valid) M(close_first_carry_qty) M(close_retire_ledger_whole)
    M(close_pending_later_qty) M(placement_script_open_ms) M(placement_sub_open_ms)
    M(projection_created_bar) M(projection_created_bar_pinned) M(projection_position_side)
    M(projection_after_close) M(close_first_entry) M(signal_price_refused)
    M(projection_over_pyramiding) M(projection_opposite_market_predecessor)
    M(projection_predecessor) M(chain_origin_sequence)
    M(recreated_after_named_cancelled_entry_incarnation) M(named_cancel_surviving_exit_incarnation)
    M(retained_parent_topology) M(defer_until_post_parent_calculation)
    M(projection_predecessor_market) M(projection_predecessor_exit)
    M(projection_created_during_coof) M(projection_coof_at_terminal) M(projection_coof_mid_bar)
    M(forced_execution_price) M(forced_waypoint_print) M(coof_open_stop_next_waypoint)
    M(projection_tv_carry_qty) M(projection_default_stop_equity)
    M(projection_default_stop_signal_close) M(projection_explicit_equity)
    M(projection_explicit_signal_close) M(projection_affordability_equity)
    M(projection_affordability_signal_price) M(projection_affordability_held_qty)
    M(sizing) M(exit_levels) M(trail_activation_level) M(retained_trail_best)
    M(market_admission) M(birth) M(birth_reach) M(leg_activation) M(exit_activation) M(legs)
    M(restored_after_margin) M(reservation_expansion) M(reservation_growth_source)
    M(reservation_growth_owner_incarnation) M(stop_limit_activated) M(coof_cascade_seg_i)
    M(coof_cascade_inflight_fires) M(paired_flat_market_candidate) M(paired_flat_market_own_qty)
    M(paired_flat_market_signal_close) M(paired_flat_market_signal_equity)
    M(paired_flat_market_signal_margin_pct) M(paired_flat_market_signal_pointvalue)
    M(paired_flat_market_signal_fx) M(paired_flat_market_peer_seq)
    M(paired_flat_market_transaction_qty) M(signal_close_mc_bar)
    M(signal_close_mc_entry_incarnation) M(signal_close_mc_fill_seq)
    M(signal_close_mc_remaining_qty) M(pooc_global_full_exit_dynamic_qty)
    M(pooc_global_full_exit_tracks_bound_adds) M(pooc_global_full_exit_bound_add) M(cancellation))
#undef M
#undef PAYLOAD_MEMBERS

template<class T> void PayloadWriter::member(std::string_view name, const T& value) {
    if (status != ScratchStatus::ok) return;
    if constexpr (std::is_same_v<T, std::string>) {
        ScratchSpanSource text({reinterpret_cast<const std::byte*>(value.data()), value.size()});
        emit(name, PayloadType::text, text);
    } else if constexpr (std::is_same_v<T, bool>) {
        word(name, PayloadType::boolean, value ? 1 : 0);
    } else if constexpr (std::is_enum_v<T>) {
        member(name, static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_integral_v<T>) {
        word(name, std::is_signed_v<T> ? PayloadType::signed_integer : PayloadType::unsigned_integer,
             static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_same_v<T, double>) {
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        word(name, PayloadType::real, bits);
    } else {
        marker(name, PayloadType::object_begin);
        encode_payload(*this, value);
        marker(name, PayloadType::object_end);
    }
}
} // namespace

ScratchStatus freeze_placement(ExitScratchContext& context, std::uint64_t scope,
                               const PlacementSnapshot& value, ScratchHandle& version) {
    PayloadWriter writer(context, scope, PayloadSchema::placement);
    encode_payload(writer, value);
    return writer.finish(version);
}
ScratchStatus freeze_request(ExitScratchContext& context, std::uint64_t scope,
                             const native_order::Request& value, ScratchHandle& version) {
    PayloadWriter writer(context, scope, PayloadSchema::request);
    encode_payload(writer, value);
    return writer.finish(version);
}
ScratchStatus freeze_pending(ExitScratchContext& context, std::uint64_t scope,
                             const native_order::Request& request, const PlacementSnapshot& placement,
                             const ScratchByteSource& key, ScratchHandle& version) {
    PayloadWriter writer(context, scope, PayloadSchema::pending);
    writer.member("request", request);
    writer.member("placement", placement);
    writer.borrowed_text("replacement_key", key);
    return writer.finish(version);
}

pineforge::detail::ScratchKeyOrder exit_payload_key_order() noexcept {
    // The x5 order, including its optional text key suffix.
    return {0x5832355832370002ULL, nullptr,
        [](void*, const ScratchByteSource& left, const ScratchByteSource& right,
           int& order) noexcept {
            std::array<std::uint64_t, 4> a{}, b{};
            if (left.size() < sizeof(a) || right.size() < sizeof(b))
                return ScratchStatus::programming_error;
            auto status = left.read(0, {reinterpret_cast<std::byte*>(a.data()), sizeof(a)});
            if (status != ScratchStatus::ok) return status;
            status = right.read(0, {reinterpret_cast<std::byte*>(b.data()), sizeof(b)});
            if (status != ScratchStatus::ok) return status;
            for (unsigned i = 0; i != 3; ++i) {
                if (a[i] == b[i]) continue;
                order = a[i] < b[i] ? -1 : 1;
                return ScratchStatus::ok;
            }
            const auto common = std::min(left.size(), right.size());
            std::array<std::byte, 128> x{}, y{};
            for (std::uint64_t offset = sizeof(a); offset < common;) {
                const auto n = static_cast<std::size_t>(
                    std::min<std::uint64_t>(x.size(), common - offset));
                status = left.read(offset, {x.data(), n});
                if (status != ScratchStatus::ok) return status;
                status = right.read(offset, {y.data(), n});
                if (status != ScratchStatus::ok) return status;
                const auto comparison = std::memcmp(x.data(), y.data(), n);
                if (comparison) { order = comparison; return ScratchStatus::ok; }
                offset += n;
            }
            if (left.size() != right.size()) order = left.size() < right.size() ? -1 : 1;
            else order = a[3] < b[3] ? -1 : (b[3] < a[3] ? 1 : 0);
            return ScratchStatus::ok;
        }};
}

namespace {
void require_payload_status(ScratchStatus status) {
    if (status != ScratchStatus::ok)
        throw ExitPayloadError(status);
}
}
ScratchHandle ExitScratchVersions::placement(std::uint64_t scope, const PlacementSnapshot& value) {
    ScratchHandle version = 0;
    require_payload_status(freeze_placement(context_, scope, value, version));
    return version;
}
PlacementReadback ExitScratchVersions::placement(ScratchHandle version) const noexcept {
    return PlacementReadback(context_.store, version);
}
ScratchHandle ExitScratchVersions::request(std::uint64_t scope, const native_order::Request& value) {
    ScratchHandle version = 0;
    require_payload_status(freeze_request(context_, scope, value, version));
    return version;
}
native_order::RequestReadback ExitScratchVersions::request(ScratchHandle version) const noexcept {
    return native_order::RequestReadback(context_.store, version);
}
ScratchHandle ExitScratchVersions::pending(std::uint64_t scope, const native_order::Request& request,
                                           const PlacementSnapshot& placement, const ScratchByteSource& key) {
    ScratchHandle version = 0;
    require_payload_status(freeze_pending(context_, scope, request, placement, key, version));
    return version;
}
PendingReadback ExitScratchVersions::pending(ScratchHandle version) const noexcept {
    return PendingReadback(context_.store, version);
}
native_order::Owner ExitScratchVersions::freeze_openings(const native_order::RunIdentity& run,
        std::int64_t cycle, std::uint64_t count, void* context, OpeningReader read) {
    if (cycle <= 0) require_payload_status(ScratchStatus::programming_error);
    native_order::OpeningInputCursor input(count, context, read);
    native_order::BindOpenings owner{{}, cycle};
    require_payload_status(native_order::freeze_openings(run, input, owner.external_openings));
    return owner;
}
ScratchStatus ExitScratchVersions::release_scope(std::uint64_t scope) noexcept {
    auto& store = context_.store;
    const std::array<std::uint64_t, 4> words{scope, pineforge::detail::payload_root_kind, 0, 0};
    ScratchSpanSource probe({reinterpret_cast<const std::byte*>(words.data()), sizeof(words)});
    for (;;) {
        ScratchHandle handle = 0;
        auto status = store.lower_bound(probe, handle);
        if (status == ScratchStatus::end) return ScratchStatus::ok;
        if (status != ScratchStatus::ok) return status;
        std::array<std::uint64_t, 4> key{};
        status = store.read(handle, ScratchField::key, 0,
            {reinterpret_cast<std::byte*>(key.data()), sizeof(key)});
        if (status != ScratchStatus::ok) return status;
        if (key[0] != scope || key[1] > pineforge::detail::payload_field_kind)
            return ScratchStatus::ok;
        status = store.begin();
        if (status != ScratchStatus::ok) return status;
        status = store.erase(handle);
        if (status == ScratchStatus::ok) status = store.commit(ScratchDurability::buffered);
        if (status != ScratchStatus::ok) {
            const auto rollback = store.rollback();
            return rollback == ScratchStatus::ok ? status : rollback;
        }
    }
}
} // namespace pineforge::source::detail

namespace pineforge::source {
pineforge::detail::ScratchStatus PendingReadback::replacement_key(
        pineforge::detail::PayloadChunkBytes& out) const noexcept {
    using pineforge::detail::ScratchStatus;
    using pineforge::detail::PayloadType;
    std::uint64_t count = 0, depth = 0;
    auto status = field_count(count);
    if (status != ScratchStatus::ok) return status;
    constexpr std::string_view name = "replacement_key";
    for (std::uint64_t i = 0; i != count; ++i) {
        pineforge::detail::PayloadField value;
        status = field(i, value);
        if (status != ScratchStatus::ok) return status;
        if (depth == 0 && value.type == PayloadType::object_begin
            && value.name.size() == name.size()) {
            std::array<std::byte, name.size()> bytes{};
            status = value.name.read(0, {bytes.data(), bytes.size()});
            if (status != ScratchStatus::ok) return status;
            if (std::memcmp(bytes.data(), name.data(), name.size()) == 0) {
                status = field(i + 1, value);
                if (status != ScratchStatus::ok) return status;
                std::uint64_t size = 0;
                status = value.unsigned_value(size);
                if (status != ScratchStatus::ok) return status;
                const auto chunks = size / 128 + (size % 128 != 0);
                if (chunks > count - i - 2) return ScratchStatus::corrupt;
                status = field(i + 2 + chunks, value);
                if (status != ScratchStatus::ok) return status;
                if (value.type != PayloadType::object_end) return ScratchStatus::corrupt;
                out = pineforge::detail::PayloadChunkBytes(*this, i + 2, size);
                return ScratchStatus::ok;
            }
        }
        if (value.type == PayloadType::object_begin) ++depth;
        if (value.type == PayloadType::object_end) {
            if (!depth) return ScratchStatus::corrupt;
            --depth;
        }
    }
    return depth ? ScratchStatus::corrupt : ScratchStatus::not_found;
}
} // namespace pineforge::source
