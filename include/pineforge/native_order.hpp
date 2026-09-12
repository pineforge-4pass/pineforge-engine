#pragma once

#include "execution.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge::native_order {
inline namespace native_order_v1 {

// Isolated working-request/value core: current LIVE requests and immutable
// command history. It does not own positions, cash, paid fees, matching,
// calendar, host phase, or a second physical book.

struct RunIdentity {
    std::string session_key;
    uint64_t run_number = 0;
};

inline bool operator==(const RunIdentity& a, const RunIdentity& b) {
    return a.run_number == b.run_number && a.session_key == b.session_key;
}
inline bool operator!=(const RunIdentity& a, const RunIdentity& b) { return !(a == b); }

struct RequestHandle {
    RunIdentity run;
    uint64_t incarnation = 0;
};

inline bool operator==(const RequestHandle& a, const RequestHandle& b) {
    return a.incarnation == b.incarnation && a.run == b.run;
}
inline bool operator!=(const RequestHandle& a, const RequestHandle& b) { return !(a == b); }

struct Request {
    execution::Action action;
    std::string label;
    std::string comment;
};

struct Birth {
    uint64_t acceptance_ordinal = 0;
    int64_t decision_time_lower_bound = 0;
};

inline bool operator==(const Birth& a, const Birth& b) {
    return a.acceptance_ordinal == b.acceptance_ordinal
        && a.decision_time_lower_bound == b.decision_time_lower_bound;
}

struct LiveRequest {
    RequestHandle handle;
    Request request;
    Birth birth;
    std::optional<RequestHandle> predecessor;
};

inline bool point_eligible(const Birth& birth,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_ordinal > birth.acceptance_ordinal
        && effective_time_ms >= birth.decision_time_lower_bound;
}

inline bool point_eligible(const LiveRequest& live,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_eligible(live.birth, point_ordinal, effective_time_ms);
}

// v2 exact binary64 grid: r=abs(q)/s, n=round(r) half away from zero, g=n*s.
// Intermediates finite, 1<=n<=2^53, abs(abs(q)-g) <= 4*ulp(max(abs(q),abs(g)))
// and < s/2. ulp is nextafter toward +inf. Does not rewrite q.
inline bool quantity_on_grid(double q, double step) noexcept {
    if (!std::isfinite(q) || !std::isfinite(step) || step <= 0.0) return false;
    const double abs_q = std::abs(q);
    const double r = abs_q / step;
    if (!std::isfinite(r)) return false;
    const double n = std::round(r);
    if (!std::isfinite(n) || n < 1.0 || n > 0x1p53) return false;
    const double g = n * step;
    if (!std::isfinite(g)) return false;
    const double err = std::abs(abs_q - g);
    const double span = std::max(abs_q, std::abs(g));
    const double ulp = std::nextafter(span, std::numeric_limits<double>::infinity()) - span;
    const double ulp_bound = 4.0 * ulp;
    const double half_step = step / 2.0;
    if (!std::isfinite(err) || !std::isfinite(ulp_bound) || !std::isfinite(half_step))
        return false;
    return err <= ulp_bound && err < half_step;
}

enum class RequestRejectReason { InvalidQuantity, OffGrid };

enum class SubmitStatus { Accepted, Rejected };
enum class ReplaceStatus { Replaced, ReplaceRejected, NotWorking, InvalidHandle };
enum class CancelStatus { Cancelled, NotWorking, InvalidHandle };

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Rejected;
    uint64_t event_ordinal = 0;
    std::optional<RequestHandle> handle;
    std::optional<RequestRejectReason> reason;
};

struct ReplaceResult {
    ReplaceStatus status = ReplaceStatus::InvalidHandle;
    uint64_t event_ordinal = 0;
    std::optional<RequestHandle> successor;
    std::optional<RequestRejectReason> reason;
};

struct CancelResult {
    CancelStatus status = CancelStatus::InvalidHandle;
    uint64_t event_ordinal = 0;
};

struct AcceptedEvent {
    uint64_t ordinal = 0;
    RequestHandle handle;
    Request request;
    Birth birth;
};

struct RejectedEvent {
    uint64_t ordinal = 0;
    Request request;
    RequestRejectReason reason = RequestRejectReason::InvalidQuantity;
};

struct ReplacedEvent {
    uint64_t ordinal = 0;
    RequestHandle predecessor;
    Request predecessor_request;
    RequestHandle successor;
    Request successor_request;
    Birth successor_birth;
};

struct ReplaceRejectedEvent {
    uint64_t ordinal = 0;
    RequestHandle target;
    Request live_request;
    Request attempted;
    RequestRejectReason reason = RequestRejectReason::InvalidQuantity;
};

struct CancelledEvent {
    uint64_t ordinal = 0;
    RequestHandle handle;
    Request request;
};

struct NotWorkingEvent {
    uint64_t ordinal = 0;
    RequestHandle target;
    std::optional<Request> attempted;
};

struct InvalidHandleEvent {
    uint64_t ordinal = 0;
    RequestHandle target;
    std::optional<Request> attempted;
};

enum class MatchRejectReason : std::uint8_t {
    NonpositivePrice = 0,
    OpeningDirection = 1,
    MaxAbsUnits = 2,
    MaxOpenLots = 3,
    InitialMargin = 4,
};

struct NoEffectEvent {
    uint64_t ordinal = 0;
    RequestHandle handle;
    Request request;
    Birth birth;
};

struct MatchRejectedEvent {
    uint64_t ordinal = 0;
    RequestHandle handle;
    Request request;
    Birth birth;
    MatchRejectReason reason = MatchRejectReason::OpeningDirection;
};

struct ExecutionAppliedEvent {
    uint64_t ordinal = 0;
    RequestHandle handle;
    Request request;
    Birth birth;
    int64_t effective_time_ms = 0;
    int64_t interval_open_ms = 0;
    int64_t interval_last_traded_close_ms = 0;
    int interval_index = 0;
    double raw_price = 0.0;
    double resolved_price = 0.0;
    double current_ticket = 0.0;
    std::size_t first_trade_index = 0;
    std::size_t closed_trade_count = 0;
    uint64_t opened_lot_incarnation = 0;
    std::uint8_t provenance = 0;
};

using CommandEvent = std::variant<AcceptedEvent,
                                  RejectedEvent,
                                  ReplacedEvent,
                                  ReplaceRejectedEvent,
                                  CancelledEvent,
                                  NotWorkingEvent,
                                  InvalidHandleEvent,
                                  NoEffectEvent,
                                  MatchRejectedEvent,
                                  ExecutionAppliedEvent>;

class WorkingRequestCore {
public:
    explicit WorkingRequestCore(RunIdentity identity);
    WorkingRequestCore(const WorkingRequestCore&) = delete;
    WorkingRequestCore& operator=(const WorkingRequestCore&) = delete;
    // Move transfers the complete run. The source becomes empty/unbound;
    // commands throw invalid_argument without mutation until reset rebinds it.
    // Self move-assignment preserves the current run.
    WorkingRequestCore(WorkingRequestCore&& other) noexcept;
    WorkingRequestCore& operator=(WorkingRequestCore&& other) noexcept;

    void reset(RunIdentity identity);

    const RunIdentity& identity() const noexcept { return identity_; }
    const std::vector<LiveRequest>& live() const noexcept { return live_; }
    const std::vector<CommandEvent>& history() const noexcept { return history_; }
    const LiveRequest* find_live(const RequestHandle& handle) const;

    SubmitResult submit(const Request& request,
                        int64_t decision_time_ms,
                        uint64_t& next_order_incarnation,
                        uint64_t& next_timeline_ordinal,
                        std::optional<double> quantity_grid = std::nullopt);

    ReplaceResult replace(const RequestHandle& target,
                          const Request& request,
                          int64_t decision_time_ms,
                          uint64_t& next_order_incarnation,
                          uint64_t& next_timeline_ordinal,
                          std::optional<double> quantity_grid = std::nullopt);

    CancelResult cancel(const RequestHandle& target, uint64_t& next_timeline_ordinal);

    // Consumer-only terminal install. Reserve history before physical commit;
    // install is the nonthrowing live-erase + history-append suffix.
    friend struct TerminalCommit;

private:
    enum class TargetKind { Live, NotWorking, InvalidHandle };

    static void require_identity(const RunIdentity& identity);
    static void require_grid(std::optional<double> quantity_grid);
    static void require_distinct_counters(uint64_t& next_order_incarnation,
                                          uint64_t& next_timeline_ordinal);
    static std::optional<RequestRejectReason> validate_request(
            const Request& request, std::optional<double> quantity_grid);

    uint64_t usable_ordinal(uint64_t next) const;
    uint64_t usable_incarnation(uint64_t next) const;
    uint64_t last_command_ordinal() const;
    uint64_t last_consumed_incarnation() const;
    TargetKind classify(const RequestHandle& handle, std::size_t* live_index) const;

    RunIdentity identity_;
    std::vector<LiveRequest> live_;
    std::vector<CommandEvent> history_;
};

static_assert(std::is_nothrow_move_constructible_v<RunIdentity>);
static_assert(std::is_nothrow_move_assignable_v<RunIdentity>);
static_assert(std::is_nothrow_move_constructible_v<RequestHandle>);
static_assert(std::is_nothrow_move_assignable_v<RequestHandle>);
static_assert(std::is_nothrow_move_constructible_v<Request>);
static_assert(std::is_nothrow_move_assignable_v<Request>);
static_assert(std::is_nothrow_move_constructible_v<Birth>);
static_assert(std::is_nothrow_move_constructible_v<LiveRequest>);
static_assert(std::is_nothrow_move_assignable_v<LiveRequest>);
static_assert(std::is_nothrow_move_constructible_v<CommandEvent>);
static_assert(std::is_nothrow_move_assignable_v<CommandEvent>);
static_assert(std::is_nothrow_move_constructible_v<SubmitResult>);
static_assert(std::is_nothrow_move_constructible_v<ReplaceResult>);
static_assert(std::is_nothrow_move_constructible_v<CancelResult>);
static_assert(std::is_nothrow_move_constructible_v<NoEffectEvent>);
static_assert(std::is_nothrow_move_constructible_v<MatchRejectedEvent>);
static_assert(std::is_nothrow_move_constructible_v<ExecutionAppliedEvent>);

struct TerminalCommit {
    static uint64_t usable_ordinal(const WorkingRequestCore& core, uint64_t next);
    static void reserve_history(WorkingRequestCore& core);
    static void install(WorkingRequestCore& core,
                        std::size_t live_index,
                        CommandEvent&& event) noexcept;
};

}  // inline namespace native_order_v1
}  // namespace pineforge::native_order
