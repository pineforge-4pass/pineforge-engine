#pragma once

#include <cstdint>

namespace pineforge {
namespace detail {

enum class ScratchTableBackend : std::uint32_t {
    disk_scratch = 0,
    bounded_memory = 1,
};

// Admission maxima, not observed populations. K/B include erased keys and
// generated identifiers; all B fields count bytes, including embedded NULs.
struct ScratchManifest {
    std::uint64_t K_q = 0;
    std::uint64_t B_q = 0;
    std::uint64_t K_e = 0;
    std::uint64_t B_e = 0;
    std::uint64_t K_c = 0;
    std::uint64_t B_c = 0;
    std::uint64_t U = 0;
    std::uint64_t B_U = 0;
    std::uint64_t V = 0;
    std::uint64_t B_V = 0;
    std::uint64_t R = 0;
};

struct ScratchTableConfig {
    ScratchTableBackend backend = ScratchTableBackend::disk_scratch;
    ScratchManifest manifest{};
};

class ScratchSlab;

} // namespace detail
} // namespace pineforge
