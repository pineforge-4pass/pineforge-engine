#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pineforge::native_order {
inline namespace native_order_v1 {
namespace {

template <class T>
void reserve_one(std::vector<T>& values) {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    if (values.size() == values.max_size()) {
        throw std::length_error("native working-request capacity exhausted");
    }
    const std::size_t required = values.size() + 1;
    if (required <= values.capacity()) return;
    const std::size_t grown = values.capacity() == 0
            ? 1
            : (values.capacity() > values.max_size() / 2 ? values.max_size()
                                                         : values.capacity() * 2);
    values.reserve(std::max(required, grown));
}

uint64_t event_ordinal(const CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

}  // namespace

void WorkingRequestCore::require_identity(const RunIdentity& identity) {
    if (identity.session_key.empty() || identity.run_number == 0) {
        throw std::invalid_argument(
                "native run identity requires a nonempty session key and positive run number");
    }
}

void WorkingRequestCore::require_distinct_counters(uint64_t& next_order_incarnation,
                                                   uint64_t& next_timeline_ordinal) {
    if (&next_order_incarnation == &next_timeline_ordinal) {
        throw std::invalid_argument(
                "native order incarnation and timeline counters must be distinct");
    }
}

uint64_t WorkingRequestCore::last_command_ordinal() const {
    if (history_.empty()) return 0;
    return event_ordinal(history_.back());
}

uint64_t WorkingRequestCore::last_consumed_incarnation() const {
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (const auto* accepted = std::get_if<AcceptedEvent>(&*it)) {
            return accepted->handle.incarnation;
        }
        if (const auto* replaced = std::get_if<ReplacedEvent>(&*it)) {
            return replaced->successor.incarnation;
        }
    }
    return 0;
}

uint64_t WorkingRequestCore::usable_ordinal(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native timeline ordinal must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    if (next <= last_command_ordinal()) {
        throw std::invalid_argument("native timeline ordinal reused or regressed");
    }
    return next;
}

uint64_t WorkingRequestCore::usable_incarnation(uint64_t next) const {
    if (next == 0) {
        throw std::invalid_argument("native order incarnation must be nonzero");
    }
    if (next == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native order incarnation exhausted");
    }
    if (next <= last_consumed_incarnation()) {
        throw std::invalid_argument("native order incarnation reused or regressed");
    }
    return next;
}

void WorkingRequestCore::require_grid(std::optional<double> quantity_grid) {
    if (!quantity_grid) return;
    if (!std::isfinite(*quantity_grid) || *quantity_grid <= 0.0) {
        throw std::invalid_argument("quantity grid requires a finite positive step");
    }
}

std::optional<RequestRejectReason> WorkingRequestCore::validate_request(
        const Request& request, std::optional<double> quantity_grid) {
    require_grid(quantity_grid);
    if (std::holds_alternative<execution::Flatten>(request.action)) return std::nullopt;
    if (const auto* reduce = std::get_if<order_action::Reduce>(&request.action)) {
        if (!std::isfinite(reduce->units) || reduce->units <= 0.0) {
            return RequestRejectReason::InvalidQuantity;
        }
        if (quantity_grid && !quantity_on_grid(reduce->units, *quantity_grid)) {
            return RequestRejectReason::OffGrid;
        }
        return std::nullopt;
    }
    const auto* transact = std::get_if<order_action::Transact>(&request.action);
    if (!transact || !std::isfinite(transact->signed_units) || transact->signed_units == 0.0) {
        return RequestRejectReason::InvalidQuantity;
    }
    if (quantity_grid && !quantity_on_grid(transact->signed_units, *quantity_grid)) {
        return RequestRejectReason::OffGrid;
    }
    return std::nullopt;
}

WorkingRequestCore::WorkingRequestCore(RunIdentity identity) {
    require_identity(identity);
    identity_ = std::move(identity);
}

WorkingRequestCore::WorkingRequestCore(WorkingRequestCore&& other) noexcept {
    *this = std::move(other);
}

WorkingRequestCore& WorkingRequestCore::operator=(WorkingRequestCore&& other) noexcept {
    if (this == &other) return *this;
    identity_ = std::move(other.identity_);
    live_ = std::move(other.live_);
    history_ = std::move(other.history_);
    other.identity_.session_key.clear();
    other.identity_.run_number = 0;
    other.live_.clear();
    other.history_.clear();
    return *this;
}

void WorkingRequestCore::reset(RunIdentity identity) {
    require_identity(identity);
    WorkingRequestCore next(std::move(identity));
    *this = std::move(next);
}

WorkingRequestCore::TargetKind WorkingRequestCore::classify(
        const RequestHandle& handle, std::size_t* live_index) const {
    if (handle.incarnation == 0 || handle.run.run_number == 0 || handle.run.session_key.empty()
        || handle.run != identity_) {
        return TargetKind::InvalidHandle;
    }
    for (std::size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].handle.incarnation == handle.incarnation) {
            if (live_index) *live_index = i;
            return TargetKind::Live;
        }
    }
    return TargetKind::NotWorking;
}

const LiveRequest* WorkingRequestCore::find_live(const RequestHandle& handle) const {
    std::size_t index = 0;
    if (classify(handle, &index) != TargetKind::Live) return nullptr;
    return &live_[index];
}

SubmitResult WorkingRequestCore::submit(const Request& request,
                                        int64_t decision_time_ms,
                                        uint64_t& next_order_incarnation,
                                        uint64_t& next_timeline_ordinal,
                                        std::optional<double> quantity_grid) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    Request staged_request = request;
    const auto reason = validate_request(staged_request, quantity_grid);
    if (reason) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        SubmitResult result{SubmitStatus::Rejected, ordinal, std::nullopt, reason};
        CommandEvent event{RejectedEvent{ordinal, std::move(staged_request), *reason}};
        reserve_one(history_);
        history_.push_back(std::move(event));
        ++next_timeline_ordinal;
        return result;
    }
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle handle{identity_, incarnation};
    Birth birth{ordinal, decision_time_ms};
    LiveRequest live_row{handle, staged_request, birth, std::nullopt};
    CommandEvent event{AcceptedEvent{ordinal, handle, std::move(staged_request), birth}};
    SubmitResult result{SubmitStatus::Accepted, ordinal, std::move(handle), std::nullopt};
    reserve_one(live_);
    reserve_one(history_);
    live_.push_back(std::move(live_row));
    history_.push_back(std::move(event));
    ++next_timeline_ordinal;
    ++next_order_incarnation;
    return result;
}

ReplaceResult WorkingRequestCore::replace(const RequestHandle& target,
                                          const Request& request,
                                          int64_t decision_time_ms,
                                          uint64_t& next_order_incarnation,
                                          uint64_t& next_timeline_ordinal,
                                          std::optional<double> quantity_grid) {
    require_identity(identity_);
    require_distinct_counters(next_order_incarnation, next_timeline_ordinal);
    RequestHandle staged_target = target;
    Request staged_request = request;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    if (kind != TargetKind::Live) {
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        const ReplaceStatus status = kind == TargetKind::InvalidHandle
                ? ReplaceStatus::InvalidHandle
                : ReplaceStatus::NotWorking;
        ReplaceResult result{status, ordinal, std::nullopt, std::nullopt};
        CommandEvent event = kind == TargetKind::InvalidHandle
                ? CommandEvent{InvalidHandleEvent{ordinal, std::move(staged_target),
                                                  std::move(staged_request)}}
                : CommandEvent{NotWorkingEvent{ordinal, std::move(staged_target),
                                               std::move(staged_request)}};
        reserve_one(history_);
        history_.push_back(std::move(event));
        ++next_timeline_ordinal;
        return result;
    }
    const auto reason = validate_request(staged_request, quantity_grid);
    if (reason) {
        Request live_request = live_[live_index].request;
        const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
        ReplaceResult result{ReplaceStatus::ReplaceRejected, ordinal, std::nullopt, reason};
        CommandEvent event{ReplaceRejectedEvent{ordinal, std::move(staged_target),
                                                std::move(live_request), std::move(staged_request),
                                                *reason}};
        reserve_one(history_);
        history_.push_back(std::move(event));
        ++next_timeline_ordinal;
        return result;
    }
    LiveRequest predecessor = live_[live_index];
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    const uint64_t incarnation = usable_incarnation(next_order_incarnation);
    RequestHandle successor_handle{identity_, incarnation};
    Birth birth{ordinal, decision_time_ms};
    LiveRequest successor{successor_handle, staged_request, birth, predecessor.handle};
    CommandEvent event{ReplacedEvent{ordinal, predecessor.handle, std::move(predecessor.request),
                                     successor_handle, std::move(staged_request), birth}};
    ReplaceResult result{ReplaceStatus::Replaced, ordinal, std::move(successor_handle),
                         std::nullopt};
    reserve_one(history_);
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(live_index));
    live_.push_back(std::move(successor));
    history_.push_back(std::move(event));
    ++next_timeline_ordinal;
    ++next_order_incarnation;
    return result;
}

uint64_t TerminalCommit::usable_ordinal(const WorkingRequestCore& core, uint64_t next) {
    return core.usable_ordinal(next);
}

void TerminalCommit::reserve_history(WorkingRequestCore& core) {
    reserve_one(core.history_);
}

void TerminalCommit::install(WorkingRequestCore& core,
                             std::size_t live_index,
                             CommandEvent&& event) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<CommandEvent>);
    static_assert(std::is_nothrow_move_assignable_v<LiveRequest>);
    core.history_.push_back(std::move(event));
    core.live_.erase(core.live_.begin() + static_cast<std::ptrdiff_t>(live_index));
}

CancelResult WorkingRequestCore::cancel(const RequestHandle& target,
                                        uint64_t& next_timeline_ordinal) {
    require_identity(identity_);
    RequestHandle staged_target = target;
    std::size_t live_index = 0;
    const TargetKind kind = classify(staged_target, &live_index);
    const uint64_t ordinal = usable_ordinal(next_timeline_ordinal);
    if (kind != TargetKind::Live) {
        const CancelStatus status = kind == TargetKind::InvalidHandle
                ? CancelStatus::InvalidHandle
                : CancelStatus::NotWorking;
        CancelResult result{status, ordinal};
        CommandEvent event = kind == TargetKind::InvalidHandle
                ? CommandEvent{InvalidHandleEvent{ordinal, std::move(staged_target), std::nullopt}}
                : CommandEvent{NotWorkingEvent{ordinal, std::move(staged_target), std::nullopt}};
        reserve_one(history_);
        history_.push_back(std::move(event));
        ++next_timeline_ordinal;
        return result;
    }
    LiveRequest removed = live_[live_index];
    CancelResult result{CancelStatus::Cancelled, ordinal};
    CommandEvent event{CancelledEvent{ordinal, std::move(removed.handle),
                                      std::move(removed.request)}};
    reserve_one(history_);
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(live_index));
    history_.push_back(std::move(event));
    ++next_timeline_ordinal;
    return result;
}

}  // inline namespace native_order_v1
}  // namespace pineforge::native_order
