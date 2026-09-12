#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace pineforge::native_order {
inline namespace native_order_v1 {

// Stable native-v1 identity leaf. Request/core/event values live in
// native_order_v2; do not duplicate these types there.

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

struct Birth {
    uint64_t acceptance_ordinal = 0;
    int64_t decision_time_lower_bound = 0;
};

inline bool operator==(const Birth& a, const Birth& b) {
    return a.acceptance_ordinal == b.acceptance_ordinal
        && a.decision_time_lower_bound == b.decision_time_lower_bound;
}
inline bool operator!=(const Birth& a, const Birth& b) { return !(a == b); }

inline bool point_eligible(const Birth& birth,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_ordinal > birth.acceptance_ordinal
        && effective_time_ms >= birth.decision_time_lower_bound;
}

static_assert(std::is_nothrow_move_constructible_v<RunIdentity>);
static_assert(std::is_nothrow_move_assignable_v<RunIdentity>);
static_assert(std::is_nothrow_move_constructible_v<RequestHandle>);
static_assert(std::is_nothrow_move_assignable_v<RequestHandle>);
static_assert(std::is_nothrow_move_constructible_v<Birth>);

}  // inline namespace native_order_v1
}  // namespace pineforge::native_order
