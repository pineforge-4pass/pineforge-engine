#pragma once

#include <pineforge/pineforge.h>

#include <cstdint>

namespace pineforge {
namespace metrics {

/// Minimal read boundary for the private binary curve (DESIGN 3.8).
/// The descriptor/reader owner keeps an immutable snapshot of point_count
/// consecutive 24-byte pf_equity_point_t records alive throughout a walk.
/// Selected snapshots contain the anchor followed by the window points.
///
/// read(context, index, count, output) copies EXACTLY count records beginning
/// at index into caller-owned storage, preserving every bit, or throws.
/// Calls are positional, may repeat/restart, and never advance a file cursor.
/// The backing writer must flush its pending bytes before exposing a snapshot.
/// No sink-text parsing, mapping, growing read buffer or implicit tail point.
/// SpillCurve requests at most floor(64 KiB / sizeof(pf_equity_point_t))
/// records, always with 0 <= index and 0 < count <= point_count - index.
///
/// The backing data and context are borrowed; SpillCurve copies the three
/// fields, not the backing data. A null callback is allowed only for an empty
/// snapshot. Readers may throw std::system_error for I/O failures; failures
/// propagate without publishing a partially filled cache. Concurrent use of
/// one SpillCurve needs external synchronization; separate sources may share
/// immutable backing storage.
struct ScratchCurveReader {
    using Read = void (*)(const void* context, std::int64_t index,
                         std::int64_t count, pf_equity_point_t* output);

    const void* context = nullptr;
    std::int64_t point_count = 0;
    Read read = nullptr;
};

} // namespace metrics
} // namespace pineforge
