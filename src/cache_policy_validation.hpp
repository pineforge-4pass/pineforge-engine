#pragma once

#include <pineforge/cache_policy.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pineforge::detail {

struct CachePolicyField {
    std::uint32_t kind;
    CacheCapacity CacheDefaults::* member = nullptr;
};

[[noreturn]] inline void reject_cache_policy(const char* reason, const char* message) {
    throw coded<std::invalid_argument>(
        RunFailureCode::setting_rejected,
        {{"input", "cache_policy"}, {"reason", reason}}, message);
}

// No descriptor storage or caller pointer survives this function. The fixed
// seen array is indexed only after checking the append-only header vocabulary.
// Recognition is independent of the active store map: a listed kind whose
// member is absent/null remains an accepted retired no-op.
// Build the replacement in isolation so every malformed entry is atomic.
template<std::size_t N>
CacheDefaults copy_cache_policy_v1(
        const pf_cache_policy_v1* descriptor,
        const std::array<CachePolicyField, N>& fields) {
    CacheDefaults next;
    if (!descriptor) return next;
    if (descriptor->struct_size != sizeof(pf_cache_policy_v1))
        reject_cache_policy("unparseable_value", "cache policy descriptor size is invalid");
    if (descriptor->version != 1)
        reject_cache_policy("invalid_enum_option", "cache policy version is unsupported");
    for (const auto reserved : descriptor->reserved)
        if (reserved != 0)
            reject_cache_policy("unparseable_value", "cache policy reserved word is nonzero");
    const auto count = descriptor->entry_count;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(pf_cache_policy_entry_v1)
        || count > static_cast<std::uint64_t>(
            std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(pf_cache_policy_entry_v1))
        reject_cache_policy("value_above_maximum", "cache policy entry array is not representable");
    if (count != 0 && !descriptor->entries)
        reject_cache_policy("unparseable_value", "cache policy entries pointer is null");
    // The header keeps contiguous kind values, including retired kinds.
    constexpr std::size_t kind_count = PF_CACHE_KIND_PROJECTION_EXIT_PAGES;
    // With duplicates forbidden, more entries than declared kinds cannot
    // describe a valid policy. This is a shape check, not a data/cache limit.
    if (count > kind_count)
        reject_cache_policy("unparseable_value", "cache policy contains too many distinct entries");
    std::array<bool, kind_count> seen{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        const pf_cache_policy_entry_v1 entry = descriptor->entries[i];
        if (entry.reserved != 0)
            reject_cache_policy("unparseable_value", "cache policy entry reserved word is nonzero");
        if (entry.kind == 0 || entry.kind > kind_count)
            reject_cache_policy("unknown_key", "cache policy kind is unsupported");
        const auto kind_index = static_cast<std::size_t>(entry.kind - 1);
        if (seen[kind_index])
            reject_cache_policy("ambiguous_key", "cache policy kind is duplicated");
        if (entry.capacity_bytes > std::numeric_limits<std::size_t>::max())
            reject_cache_policy("value_above_maximum", "cache policy capacity is not representable");
        seen[kind_index] = true;
        std::size_t field = 0;
        while (field != N && fields[field].kind != entry.kind) ++field;
        if (field != N && fields[field].member != nullptr) {
            const CacheCapacity capacity{static_cast<std::size_t>(entry.capacity_bytes)};
            if (entry.kind == PF_CACHE_KIND_LEDGER_CACHE)
                (void)CacheDefaults::ledger_cache_rows(capacity);
            next.*fields[field].member = capacity;
        }
    }
    return next;
}

} // namespace pineforge::detail
