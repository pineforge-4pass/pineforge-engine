#include "engine_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
namespace {

bool valid_lifecycle_phase(exit_legs::Phase phase) {
    return static_cast<unsigned>(phase)
        <= static_cast<unsigned>(exit_legs::Phase::AfterMargin);
}

bool same_target(exit_legs::Target a, exit_legs::Target b) {
    return a.incarnation == b.incarnation && a.owner == b.owner;
}

struct PendingLegCopy {
    uint64_t incarnation = 0;
    int64_t created_seq = 0;
    OrderType type = OrderType::EXIT;
    exit_legs::Lifecycle legs;
    bool removed = false;
};

PendingLegCopy* find_leg_copy(std::vector<PendingLegCopy>& copies,
                              uint64_t incarnation, int64_t created_seq) {
    PendingLegCopy* found = nullptr;
    for (auto& copy : copies) {
        if (copy.removed) continue;
        if (copy.incarnation != incarnation || copy.created_seq != created_seq)
            continue;
        if (found) return nullptr;
        found = &copy;
    }
    return found;
}

struct IdentityKey {
    uint64_t incarnation = 0;
    int64_t created_seq = 0;
    bool operator==(const IdentityKey& other) const {
        return incarnation == other.incarnation && created_seq == other.created_seq;
    }
};

struct IdentityKeyHash {
    size_t operator()(const IdentityKey& key) const {
        return std::hash<uint64_t>{}(key.incarnation)
            ^ (std::hash<int64_t>{}(key.created_seq) << 1);
    }
};

} // namespace










} // namespace pineforge
