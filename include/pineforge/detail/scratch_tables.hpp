#pragma once

#include <cstdint>
#include <memory>

namespace pineforge {
namespace source { class PineExecutionAdapter; }
namespace native_order { inline namespace native_order_v7 { class WorkingRequestCore; } }

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

// Begin-only binders. An empty owner selects disk scratch and detaches any
// previous binding. A nonempty owner supplies the complete fixed directory
// and its copied manifest, never a configuration/environment lookup. Each
// backend retains a copy of this owner until its old readers/reset retire;
// shared_ptr copies allocate nothing and keep a preceding run's slices alive
// across a refused/repeated begin. Engine copies discard their run owner.
// The foundation definitions refuse nonempty owners until the corresponding
// backend lane replaces the stub; there is no silent disk fallback.
void bind_tree_scratch_tables(source::PineExecutionAdapter& adapter,
                              const std::shared_ptr<ScratchSlab>& slab);
void bind_id_pair_scratch_tables(source::PineExecutionAdapter& adapter,
                                 const std::shared_ptr<ScratchSlab>& slab);
void bind_successor_root_scratch_tables(native_order::WorkingRequestCore& requests,
                                        const std::shared_ptr<ScratchSlab>& slab);

} // namespace detail
} // namespace pineforge
