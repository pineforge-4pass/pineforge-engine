#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pineforge {
struct CacheCapacity;
struct Trade;
namespace detail { struct ReadThroughCacheStats; }
namespace source::detail {

// DESIGN 3.4: private binary storage, never the text report sink. Records
// have no padding: entry_id bytes followed by the 30-byte backwards footer.
// A row holds offsets, not an owning string, so even an id larger than the
// read window does not allocate another history-sized buffer.
class PositionTailSpill {
public:
    static constexpr std::size_t kWindowBytes = 64 * 1024;
    static constexpr std::size_t kPageBytes = 4096;
    static constexpr std::size_t kFooterBytes = 30;

    struct Row {
        std::int64_t exit_time = 0;
        double qty = 0.0;
        std::uint64_t entry_incarnation = 0;
        bool is_long = false;
        bool margin_call = false;
        std::uint64_t entry_id_offset = 0;
        std::uint32_t entry_id_size = 0;
    };

    // A second unlinked file in L6b-4's bound scratch directory (DESIGN
    // 3.8 explicitly permits this). The directory must already be resolved.
    // Payload bytes, independent of retained rows/id lengths. Complete shared
    // line metadata is additional; zero/failed optional allocation bypasses.
    // Pages are at most 4096 bytes; larger allowances keep complete pages.
    explicit PositionTailSpill(const std::string& scratch_directory,
                               std::size_t cache_bytes = kWindowBytes);
    // Inclusive shared-line allowance with fixed 4096-byte geometry.
    PositionTailSpill(const std::string& scratch_directory, CacheCapacity capacity);
    ~PositionTailSpill();
    PositionTailSpill(const PositionTailSpill&) = delete;
    PositionTailSpill& operator=(const PositionTailSpill&) = delete;

    void append(const Trade& trade);
    void clear();
    std::uint64_t size_bytes() const noexcept;
    std::uint64_t row_count() const noexcept;
    std::size_t cache_capacity_bytes() const noexcept;
    // Counts actual backing calls, not cache lookups.
    std::uint64_t backing_read_count() const noexcept;
    std::uint64_t backing_read_bytes() const noexcept;
    ::pineforge::detail::ReadThroughCacheStats cache_stats() const noexcept;
    std::size_t cache_control_bytes() const noexcept;

    // Start cursor at size_bytes(); each successful call moves it to the
    // preceding record. Separate cursors can repeat/interleave full walks.
    bool previous(std::uint64_t& cursor, Row& row) const;
    bool entry_id_equals(const Row& row, std::string_view entry_id) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace source::detail
} // namespace pineforge
