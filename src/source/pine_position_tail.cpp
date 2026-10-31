#include "pine_position_tail.hpp"
#include "pine_position_tail_binding.hpp"
#include "../read_through_window.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge::source::detail {
namespace {

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
              "position-tail scratch records preserve binary64 quantities");

[[noreturn]] void invalid_record() {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "invalid Pine position-tail spill record");
}

} // namespace

struct PositionTailSpill::State {
    PositionTailBinding::File file;
    std::uint64_t end = 0;
    std::uint64_t rows = 0;
    mutable ::pineforge::detail::ReadThroughWindow cache;
    mutable std::array<unsigned char, kPageBytes> transfer{};

    State(const std::string& directory, std::size_t cache_bytes)
        : file(directory),
          cache(cache_bytes ? std::min(cache_bytes, kPageBytes) : kPageBytes,
                cache_bytes, this, read_backing) {}
    State(const std::string& directory, CacheCapacity capacity)
        : file(directory), cache(kPageBytes, capacity, this, read_backing) {}

    static void read_backing(void* context, std::uint64_t offset,
                             void* destination, std::size_t size) {
        static_cast<State*>(context)->file.read(offset, destination, size);
    }

    void write(std::uint64_t offset, const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t done = 0; done < size;) {
            // Also bounds each syscall for identifiers larger than a window.
            const std::size_t count = std::min(size - done, kWindowBytes);
            file.write(offset + done, bytes + done, count);
            done += count;
        }
    }

    void read(std::uint64_t offset, void* data, std::size_t size) const {
        if (offset > end || size > end - offset) invalid_record();
        auto* bytes = static_cast<unsigned char*>(data);
        const auto page_size = cache.page_size();
        // Copy backwards, including footers and ids crossing page boundaries.
        // Request only the existing prefix of the final backing page.
        std::size_t left = size;
        while (left != 0) {
            const std::uint64_t last = offset + left - 1;
            const auto page = last / page_size;
            const auto begin = page * page_size;
            const auto prefix = static_cast<std::size_t>(
                std::min<std::uint64_t>(page_size, end - begin));
            cache.get(page, transfer.data(), prefix);
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(left, last - begin + 1));
            std::memcpy(bytes + left - count, transfer.data() + last - begin + 1 - count, count);
            left -= count;
        }
    }
};

PositionTailSpill::PositionTailSpill(const std::string& scratch_directory,
                                   std::size_t cache_bytes)
    : state_(std::make_unique<State>(scratch_directory, cache_bytes)) {}

PositionTailSpill::PositionTailSpill(const std::string& scratch_directory,
                                   CacheCapacity capacity)
    : state_(std::make_unique<State>(scratch_directory, capacity)) {}

PositionTailSpill::~PositionTailSpill() = default;

void PositionTailSpill::append(const Trade& trade) {
    const auto length = trade.entry_id.size();
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (length > std::numeric_limits<std::uint32_t>::max()
        || state_->end > maximum - kFooterBytes
        || length > maximum - kFooterBytes - state_->end
        || state_->rows == std::numeric_limits<std::uint64_t>::max()) {
        throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                         "Pine position-tail spill offset exhausted");
    }
    std::array<unsigned char, kFooterBytes> footer{};
    const auto id_size = static_cast<std::uint32_t>(length);
    std::memcpy(footer.data(), &trade.exit_time, 8);
    std::memcpy(footer.data() + 8, &trade.qty, 8);
    std::memcpy(footer.data() + 16, &trade.entry_incarnation, 8);
    footer[24] = trade.is_long ? 1 : 0;
    footer[25] = trade.exit_id == kMarginCallLabel ? 1 : 0;
    std::memcpy(footer.data() + 26, &id_size, 4);
    state_->cache.invalidate();
    state_->write(state_->end, trade.entry_id.data(), length);
    state_->write(state_->end + length, footer.data(), footer.size());
    // Publish only after both writes. A failed append leaves the logical
    // end intact, so the caller never drops the row whose write failed.
    state_->end += static_cast<std::uint64_t>(length) + kFooterBytes;
    ++state_->rows;
}

void PositionTailSpill::clear() {
    state_->cache.invalidate();
    state_->file.clear();
    state_->end = 0;
    state_->rows = 0;
}

std::uint64_t PositionTailSpill::size_bytes() const noexcept { return state_->end; }
std::uint64_t PositionTailSpill::row_count() const noexcept { return state_->rows; }
std::size_t PositionTailSpill::cache_capacity_bytes() const noexcept {
    return state_->cache.payload_bytes();
}
std::uint64_t PositionTailSpill::backing_read_count() const noexcept {
    return state_->file.read_count();
}
std::uint64_t PositionTailSpill::backing_read_bytes() const noexcept {
    return state_->file.read_bytes();
}
::pineforge::detail::ReadThroughCacheStats PositionTailSpill::cache_stats() const noexcept {
    return state_->cache.stats();
}
std::size_t PositionTailSpill::cache_control_bytes() const noexcept {
    return sizeof(PositionTailSpill) + sizeof(State) - sizeof(state_->transfer);
}

bool PositionTailSpill::previous(std::uint64_t& cursor, Row& row) const {
    if (cursor > state_->end) invalid_record();
    if (cursor == 0) return false;
    if (cursor < kFooterBytes) invalid_record();
    std::array<unsigned char, kFooterBytes> footer{};
    state_->read(cursor - kFooterBytes, footer.data(), footer.size());
    Row next;
    std::memcpy(&next.exit_time, footer.data(), 8);
    std::memcpy(&next.qty, footer.data() + 8, 8);
    std::memcpy(&next.entry_incarnation, footer.data() + 16, 8);
    if (footer[24] > 1 || footer[25] > 1) invalid_record();
    next.is_long = footer[24] != 0;
    next.margin_call = footer[25] != 0;
    std::memcpy(&next.entry_id_size, footer.data() + 26, 4);
    if (next.entry_id_size > cursor - kFooterBytes) invalid_record();
    next.entry_id_offset = cursor - kFooterBytes - next.entry_id_size;
    cursor = next.entry_id_offset;
    row = next;
    return true;
}

bool PositionTailSpill::entry_id_equals(const Row& row, std::string_view entry_id) const {
    if (entry_id.size() != row.entry_id_size) return false;
    if (row.entry_id_offset > state_->end
        || row.entry_id_size > state_->end - row.entry_id_offset) invalid_record();
    // Compare in fixed-sized pieces; no owning copy of a spilled id.
    std::array<unsigned char, 256> bytes{};
    std::size_t left = entry_id.size();
    while (left != 0) {
        const std::size_t count = std::min(left, bytes.size());
        const std::size_t begin = left - count;
        state_->read(row.entry_id_offset + begin, bytes.data(), count);
        if (std::memcmp(bytes.data(), entry_id.data() + begin, count) != 0) return false;
        left = begin;
    }
    return true;
}

} // namespace pineforge::source::detail
