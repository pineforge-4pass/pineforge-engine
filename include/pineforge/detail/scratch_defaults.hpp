#pragma once

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_tables.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pineforge::detail {

// Physical resident rows <= 2x the rows kept at the last spill pass + 64
// (+ pinned rows); this is not a bound in terms of currently live rows.
// Admission still limits non-spillable cohort rows in the bounded scratch
// profile. This does not bound the number or heap size of pending orders.
inline constexpr std::size_t kMaxResidentCohorts = 4096;

// Object-record reservation constants. The 16-byte object header and 16-byte
// (id, chunk) key are defined in src/detail/o1_store.cpp:50-51. ScratchStore
// stores blobs in 64-byte cells with 56 bytes of payload
// (include/pineforge/detail/scratch_store_impl.hpp:69-72), and the default
// object chunk is 1024 bytes (include/pineforge/detail/o1_store.hpp:83-90).
// The 40-byte index share is the 32-byte handle Entry
// (include/pineforge/detail/scratch_store_impl.hpp:102-108) plus the 8-byte
// ordered-index handle (include/pineforge/detail/scratch_store_impl.hpp:861-866).
inline constexpr std::uint64_t kObjectHeaderBytes = 16;
inline constexpr std::uint64_t kObjectKeyBytes = 16;
inline constexpr std::uint64_t kObjectCellBytes = 64;
inline constexpr std::uint64_t kObjectCellPayloadBytes = 56;
inline constexpr std::uint64_t kObjectChunkBytes = 1024;
inline constexpr std::uint64_t kObjectInlinePayloadBytes = 40;
inline constexpr std::uint64_t kObjectIndexShareBytes = 40;
inline constexpr std::uint64_t kObjectSmallBytes = 168;

// ScratchArena keeps four directory bytes per claimed 1 MiB chunk
// (include/pineforge/detail/scratch_arena.hpp:142-148 and
// include/pineforge/detail/scratch_arena.hpp:112-113). This tolerance is
// deliberately separate from each object's physical record charge.
inline constexpr std::uint64_t kObjectArenaChunkBytes = 1048576;
inline constexpr std::uint64_t kObjectArenaDirectoryBytesPerChunk = 4;

namespace scratch_object_reservation_detail {

inline std::uint64_t saturating_add(std::uint64_t left,
                                    std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

inline std::uint64_t saturating_multiply(std::uint64_t left,
                                         std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return left != 0 && right > maximum / left ? maximum : left * right;
}

// A quotient plus its remainder bit avoids the usual dividend-plus-divisor-1
// overflow while retaining integer-only arithmetic.
inline std::uint64_t saturating_ceil_div(std::uint64_t value,
                                         std::uint64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient == std::numeric_limits<std::uint64_t>::max()
                   || value % divisor == 0
               ? quotient
               : quotient + 1;
}

} // namespace scratch_object_reservation_detail

// Backing bytes for one object record, excluding the arena directory. A
// payload through 40 bytes fits the 16-byte object header and one 56-byte
// value-cell payload, so its 64-byte key cell, 64-byte value cell and 40-byte
// index share total 168 bytes. Larger payloads charge the value cells for each
// independently stored object chunk, plus one 64-byte key cell per object
// chunk and the index share. The value cells are not one ceil((header +
// payload) / 56) over the whole payload: put_chunk creates a separate
// ScratchStore value blob for every object chunk, and scratch_store_impl.hpp
// rounds each blob independently at 493-520.
// Object chunk_count follows src/detail/o1_store.cpp:401-404; the object layer
// writes chunk zero with its header and each later chunk as another record at
// src/detail/o1_store.cpp:530-541. The separate arena-directory tolerance is
// kObjectArenaDirectoryBytesPerChunk per kObjectArenaChunkBytes.
inline std::uint64_t object_physical_bytes(std::uint64_t payload_bytes) noexcept {
    using namespace scratch_object_reservation_detail;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (payload_bytes <= kObjectInlinePayloadBytes) return kObjectSmallBytes;
    if (payload_bytes > maximum - kObjectHeaderBytes) return maximum;

    const auto chunks = 1 + (payload_bytes - 1) / kObjectChunkBytes;
    const auto first_payload =
        payload_bytes < kObjectChunkBytes ? payload_bytes : kObjectChunkBytes;
    const auto first_value_cells = saturating_ceil_div(
        first_payload + kObjectHeaderBytes, kObjectCellPayloadBytes);
    const auto remaining = payload_bytes - first_payload;
    const auto full_following_chunks = remaining / kObjectChunkBytes;
    const auto trailing_payload = remaining % kObjectChunkBytes;
    const auto following_value_cells = saturating_add(
        saturating_multiply(full_following_chunks,
                            saturating_ceil_div(kObjectChunkBytes,
                                                kObjectCellPayloadBytes)),
        saturating_ceil_div(trailing_payload, kObjectCellPayloadBytes));
    const auto value_cells = saturating_add(first_value_cells, following_value_cells);
    const auto value_bytes_charged = saturating_multiply(value_cells, kObjectCellBytes);
    const auto key_bytes_charged = saturating_multiply(chunks, kObjectCellBytes);
    return saturating_add(
        saturating_add(
            key_bytes_charged,
            value_bytes_charged),
        kObjectIndexShareBytes);
}

// Reserve object records for bars script bars and k objects per script
// execution. A bar close and every calc_on_order_fills recalculation are
// executions, so k is per execution rather than per bar. UINT64_MAX is the
// unbounded sentinel (also used for an unbounded receipt or
// calc_on_order_fills): return it directly so the host uses its real arena cap
// and reports the term UNCERTIFIED, rather than inventing a finite byte count.
// Zero bars or zero executions wins over that sentinel. All products saturate
// at UINT64_MAX and never wrap. The caller takes the max with its arena floor;
// when the run also owns the three scratch tables, object bytes are added to
// the table bytes because they occupy separate regions.
inline std::uint64_t object_store_reserve_bytes(
    std::uint64_t bars, std::uint64_t objects_per_execution,
    std::uint64_t record_bytes) noexcept {
    using namespace scratch_object_reservation_detail;
    if (bars == 0 || objects_per_execution == 0) return 0;
    if (objects_per_execution == std::numeric_limits<std::uint64_t>::max())
        return std::numeric_limits<std::uint64_t>::max();
    const auto per_object = object_physical_bytes(record_bytes);
    return saturating_multiply(
        saturating_multiply(bars, objects_per_execution), per_object);
}

// P0 is a POLICY admission tier, not a measured bound or a proof that a
// strategy fits. Counts include historical keys and internally generated
// successors. The finite-batch replacement reserve deliberately scales with
// input bars; actual exhaustion must fail, never grow or fall back to disk.
inline ScratchManifest default_scratch_manifest(std::uint64_t bars) {
    if (bars > std::numeric_limits<std::uint64_t>::max() / 2)
        throw ScratchCapacityError(ScratchResource::layout,
                                   "default scratch replacement reserve overflow");
    return {
        0, 0, // K_q/B_q retained in the descriptor; the q store is removed.
        262144, 25165824,
        32768, 4227072,
        65536, 4194304,
        27, 6912,
        2 * bars,
    };
}

// A stream without a declared lifetime bar count has its own fixed policy
// tier. A zero-bar finite batch remains distinct and admits zero roots.
inline ScratchManifest default_scratch_manifest() {
    auto manifest = default_scratch_manifest(0);
    manifest.R = 65536;
    return manifest;
}

} // namespace pineforge::detail
