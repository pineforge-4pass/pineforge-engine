#pragma once

#include <cstdint>
#include <variant>

namespace pineforge::execution {
inline namespace close_scope_v1 {

struct Book {};

// Run-local opening provenance, not a unique individual fill-lot identity.
// The trusted caller owns run identity; settlement checks the current cycle
// and selects every surviving physical fragment of this incarnation.
struct OpeningExposure {
    std::uint64_t incarnation = 0;
    std::int64_t cycle = 0;
};

using CloseScope = std::variant<Book, OpeningExposure>;

} // inline namespace close_scope_v1
} // namespace pineforge::execution
