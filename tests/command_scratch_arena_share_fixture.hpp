#pragma once

// Set B only: independent, checked arithmetic for finite fixture sizes.
// Deliberately does not call the production capacity/needed helpers.
#include "command_scratch_arena_fixture.hpp"

#include <algorithm>
#include <limits>

namespace command_arena_test {

inline std::uint64_t chunk_ceil(std::uint64_t bytes) {
    const auto chunk = ScratchArena::kChunkBytes;
    const auto chunks = bytes / chunk + (bytes % chunk != 0 ? 1 : 0);
    require(chunks <= std::numeric_limits<std::uint64_t>::max() / chunk,
            "fixture chunk ceiling is representable");
    return chunks * chunk;
}

inline std::uint64_t fixture_product(std::uint64_t a, std::uint64_t b) {
    require(b != 0 && a <= std::numeric_limits<std::uint64_t>::max() / b,
            "fixture ratio product is representable");
    return a * b;
}

inline std::uint64_t share_capacity(std::uint64_t supplied) {
    const auto bytes = fixture_product(supplied, kCommandScratchArenaShare.num)
        / kCommandScratchArenaShare.den;
    return bytes - bytes % ScratchArena::kChunkBytes;
}

inline std::uint64_t required_arena(std::uint64_t extent) {
    const auto product = fixture_product(chunk_ceil(extent), kCommandScratchArenaShare.den);
    const auto numerator = static_cast<std::uint64_t>(kCommandScratchArenaShare.num);
    return product / numerator + (product % numerator != 0 ? 1 : 0);
}

inline std::uint64_t arena_for_store_chunks(std::uint64_t chunks) {
    return chunk_ceil(required_arena(fixture_product(chunks, ScratchArena::kChunkBytes)));
}

inline std::uint64_t compute_placement(std::uint64_t previous, std::uint64_t claimed,
                                     std::uint64_t needed) {
    require(previous <= std::numeric_limits<std::uint64_t>::max() / 2
            && claimed <= std::numeric_limits<std::uint64_t>::max() - needed,
            "fixture COMPUTE placement is representable");
    return chunk_ceil(std::max(2 * previous, claimed + needed));
}

} // namespace command_arena_test
