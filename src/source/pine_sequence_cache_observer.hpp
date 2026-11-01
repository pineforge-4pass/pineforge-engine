#pragma once

#include <pineforge/detail/scratch_backing.hpp>

namespace pineforge::source::detail {

// Internal diagnostics at lazy backing construction. The observer only
// borrows the owner, never a cache line; it must not retain it past table
// clear/rebuild/destruction or mutate it. Logical I/O charging is separate.
struct SequenceCacheObserver {
    void* context = nullptr;
    void (*created)(void*, const ::pineforge::detail::ScratchBacking&) noexcept = nullptr;
};
inline thread_local SequenceCacheObserver sequence_cache_observer{};
inline SequenceCacheObserver set_sequence_cache_observer(SequenceCacheObserver observer) noexcept {
    const auto previous = sequence_cache_observer;
    sequence_cache_observer = observer;
    return previous;
}

} // namespace pineforge::source::detail
