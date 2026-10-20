// CR-150 L7c: the metric point source over a fixed-size scratch read window.
#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/spill_curve_reader.hpp>

#include "report_scratch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sys/types.h>
#include <type_traits>

namespace pineforge {
namespace metrics {
namespace {

constexpr std::size_t window_bytes = 64 * 1024;
constexpr std::int64_t window_points =
    static_cast<std::int64_t>(window_bytes / sizeof(pf_equity_point_t));
static_assert(window_points > 0, "scratch window holds a complete point");
static_assert(std::is_trivially_copyable<pf_equity_point_t>::value,
              "scratch points preserve their binary representation");

// Both production backends borrow the same facade region. Keep raw-descriptor
// compatibility in the shared adapter too; no backend decision belongs here.
void read_scratch_curve_region(const void* context, std::int64_t index,
                               std::int64_t count, pf_equity_point_t* output) {
    const auto& region = *static_cast<const ScratchCurveRegion*>(context);
    if (index < 0 || count <= 0 || index > region.point_count ||
        count > region.point_count - index) {
        throw std::out_of_range("scratch curve read outside its region");
    }
    report_sink::read_scratch_points(region, index, output, count);
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
            (region.point_count != 0 && !region.scratch && region.descriptor < 0)) {
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
