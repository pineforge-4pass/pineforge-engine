#pragma once

#include <cstdint>

namespace pineforge::source::detail {
// Compile-time seam only. The shipping adapter has the identity expression;
// neither a run option nor an environment variable can select the mask.
static constexpr std::uint64_t source_key_test_mask(std::uint64_t value) noexcept {
#ifdef PINEFORGE_TEST_SOURCE_KEY_MASK
    return value & PINEFORGE_TEST_SOURCE_KEY_MASK;
#else
    return value;
#endif
}
} // namespace pineforge::source::detail
