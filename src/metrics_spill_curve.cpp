// CR-150 L7c: the metric point source over a fixed-size scratch read window.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/spill_curve_reader.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace pineforge {
namespace metrics {
namespace {

constexpr std::size_t window_bytes = 64 * 1024;
constexpr std::int64_t window_points =
    static_cast<std::int64_t>(window_bytes / sizeof(pf_equity_point_t));
constexpr std::int64_t point_bytes = sizeof(pf_equity_point_t);
static_assert(window_points > 0, "scratch window holds a complete point");
static_assert(std::is_trivially_copyable<pf_equity_point_t>::value,
              "scratch points preserve their binary representation");

// Match L6b-4's scratch I/O transport while keeping this lane independently
// linkable before that dependency lands. errno remains visible to callers
// catching system_error; the engine's classifier also retains the sink code.
class ScratchCurveReadError final : public std::system_error, public RunFailureInfo {
public:
    ScratchCurveReadError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, {{"reason", "scratch_io"}}) {}
};

// The ONE L6b-4 binding: the frozen P00 region supplies the borrowed descriptor,
// byte offset and committed count. If L6b-4 provides an exact-read helper, only
// this adapter needs to delegate to it; the cache and metric walks stay shared.
void read_scratch_curve_region(const void* context, std::int64_t index,
                               std::int64_t count, pf_equity_point_t* output) {
    const auto& region = *static_cast<const ScratchCurveRegion*>(context);
    if (index < 0 || count <= 0 || index > region.point_count ||
        count > region.point_count - index) {
        throw std::out_of_range("scratch curve read outside its region");
    }
    // State validates the whole region before installing this callback, so
    // both the multiplication and every positional-read offset fit off_t.
    off_t offset = static_cast<off_t>(region.byte_offset + index * point_bytes);
    auto* bytes = reinterpret_cast<unsigned char*>(output);
    std::size_t remaining = static_cast<std::size_t>(count) * sizeof(*output);
    while (remaining != 0) {
        const ssize_t got = ::pread(region.descriptor, bytes, remaining, offset);
        if (got < 0) {
            const int error = errno;
            if (error == EINTR) continue;
            throw ScratchCurveReadError(error, "read report scratch curve");
        }
        if (got == 0) {
            throw ScratchCurveReadError(EIO, "short report scratch curve");
        }
        const auto copied = static_cast<std::size_t>(got);
        bytes += copied;
        remaining -= copied;
        offset += static_cast<off_t>(got);
    }
}

} // namespace

struct SpillCurve::State {
    ScratchCurveRegion region{-1, 0, 0};
    ScratchCurveReader reader;
    // 2,730 whole records occupy 65,520 bytes; the remaining 16 bytes of the
    // 64 KiB budget cannot hold another record.
    std::array<pf_equity_point_t, static_cast<std::size_t>(window_points)> window{};
    std::int64_t first = 0;
    std::int64_t count = 0;

    explicit State(const ScratchCurveRegion& value)
        : region(value), reader{&region, region.point_count, read_scratch_curve_region} {
        if (region.byte_offset < 0 || region.point_count < 0 ||
            (region.point_count != 0 && region.descriptor < 0)) {
            throw std::invalid_argument("invalid scratch curve region");
        }
        const auto limit = std::min<std::uintmax_t>(
            static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()),
            static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()));
        const auto offset = static_cast<std::uintmax_t>(region.byte_offset);
        if (offset > limit ||
            static_cast<std::uintmax_t>(region.point_count) >
                (limit - offset) / sizeof(pf_equity_point_t)) {
            throw std::invalid_argument("scratch curve region exceeds file offset range");
        }
    }

    explicit State(const ScratchCurveReader& value) : reader(value) {
        if (reader.point_count < 0 || (reader.point_count != 0 && reader.read == nullptr)) {
            throw std::invalid_argument("invalid scratch curve reader");
        }
    }

    pf_equity_point_t at(std::int64_t index) {
        if (index < 0 || index >= reader.point_count) {
            throw std::out_of_range("scratch curve index outside its snapshot");
        }
        // Subtraction avoids overflowing first + count for a large snapshot.
        if (index < first || index - first >= count) {
            const std::int64_t next_first = (index / window_points) * window_points;
            const std::int64_t next_count =
                std::min(window_points, reader.point_count - next_first);
            // A throwing reader may already have overwritten part of window.
            // Invalidate BEFORE calling it; only a complete fill is readable.
            count = 0;
            reader.read(reader.context, next_first, next_count, window.data());
            first = next_first;
            count = next_count;
        }
        pf_equity_point_t result;
        std::memcpy(&result, &window[static_cast<std::size_t>(index - first)], sizeof(result));
        return result;
    }
};

SpillCurve::SpillCurve(const ScratchCurveRegion& region)
    : state_(std::make_unique<State>(region)) {}

SpillCurve::SpillCurve(const ScratchCurveReader& reader)
    : state_(std::make_unique<State>(reader)) {}

SpillCurve::~SpillCurve() = default;

std::int64_t SpillCurve::size() const {
    return state_->reader.point_count;
}

pf_equity_point_t SpillCurve::operator[](std::int64_t index) const {
    return state_->at(index);
}

} // namespace metrics
} // namespace pineforge
