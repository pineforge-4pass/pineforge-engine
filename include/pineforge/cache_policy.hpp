#pragma once

#include <pineforge/detail/cache_capacity.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pineforge {

// Successful disk-write traffic between sync/discard boundaries. This is
// independent of resident cache capacity, logical history and physical RAM.
// All uint64_t values are supported; zero completes each positive transfer's
// sync/discard before write/write_some can return success.
struct ScratchIoPolicy {
    std::uint64_t write_batch_bytes = 65536;
};

// Presence is policy, not a special byte value. Keep the familiar byte
// assignment spelling while recording an explicit finite allowance even
// when it equals the initial AUTO geometry (or is zero).
class LedgerCachePolicy {
public:
    class CapacityBytes {
    public:
        CapacityBytes() = default;
        explicit CapacityBytes(std::size_t bytes) noexcept
            : bytes_(bytes), automatic_(false) {}
        CapacityBytes& operator=(std::size_t bytes) noexcept {
            bytes_ = bytes;
            automatic_ = false;
            return *this;
        }
        operator std::size_t() const noexcept { return bytes_; }
        bool automatic() const noexcept { return automatic_; }
    private:
        std::size_t bytes_ = TradeLedger::cache_capacity_bytes(TradeLedger::kDefaultCacheRows);
        bool automatic_ = true;
    };

    LedgerCachePolicy() = default;
    LedgerCachePolicy(CacheCapacity capacity) noexcept : capacity_bytes(capacity.capacity_bytes) {}
    operator CacheCapacity() const noexcept { return {static_cast<std::size_t>(capacity_bytes)}; }
    bool automatic() const noexcept { return capacity_bytes.automatic(); }

    CapacityBytes capacity_bytes;
};

// Per-owner physical policy, copied at admission and never folded into
// continuation state. Zero bypasses the optional cache. Except for the ledger
// setting below, sub-line allowances also bypass. Allowances include metadata.
// Defaults preserve the old payload line counts and add their exact common
// cache tag/pin/replacement charge. Fixed workspace is accounted separately.
struct CacheDefaults {
    static CacheCapacity lines(std::size_t page_size, std::size_t count) {
        if (count == 0) return {};
        const auto charged = detail::ReadThroughCache::line_bytes(page_size);
        if (charged == 0 || count > std::numeric_limits<std::size_t>::max() / charged)
            throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                {{"input", "cache_policy"}, {"reason", "value_above_maximum"}},
                "cache policy line geometry is not representable");
        return {charged * count};
    }
    static CacheCapacity platform_command_pages() noexcept;

    // The host ledger setting includes entries and the private page table.
    // It shares the standalone conversion and retains the existing row range.
    static std::size_t ledger_cache_rows(CacheCapacity capacity) {
        const auto rows = TradeLedger::cache_rows_for_capacity(capacity);
        if (capacity.capacity_bytes != 0 && rows == 0)
            throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                {{"input", "cache_policy"}, {"reason", "value_below_minimum"}},
                "ledger cache capacity is below one decoded entry and page pointer");
        if (rows > TradeLedger::kMaxCacheRows)
            throw coded<std::invalid_argument>(RunFailureCode::setting_rejected,
                {{"input", "cache_policy"}, {"reason", "value_above_maximum"}},
                "ledger cache capacity exceeds the existing row setting range");
        return rows;
    }

    // Omission selects ledger_cache_max_bytes. Assigning CacheCapacity or
    // capacity_bytes selects the existing explicit finite row-policy route.
    LedgerCachePolicy ledger_rows;
    CacheCapacity trade_pages = lines(512, 128);
    CacheCapacity dependency_pages = lines(512, 128);
    CacheCapacity exit_pages = platform_command_pages();
    CacheCapacity book_pages = lines(512, 128);
    CacheCapacity retained_sequence_pages = lines(512, 128);

    CacheCapacity cohort_id_pages = lines(4096, 16);
    CacheCapacity exit_rank_pages = lines(4096, 16);
    CacheCapacity erased_minima_pages = lines(4096, 8);
    CacheCapacity consumed_cycle_pages = lines(4096, 8);

    CacheCapacity position_tail_pages = lines(4096, 64 * 1024 / 4096);
    CacheCapacity successor_root_pages = lines(16, 64);
    CacheCapacity receipt_tail_pages = lines(32, 64);
    CacheCapacity chain_segment_pages = lines(24, 16);
    CacheCapacity equity_curve_pages = lines(24, 64 * 1024 / 24);

    // History keeps its existing semantic lookback. These are cache bytes,
    // never lengths, max_bars_back, bars of input or a number of feed records.
    CacheCapacity ta_history_pages{std::numeric_limits<std::size_t>::max()};
    CacheCapacity feed_calendar_pages{256 * 1024};
    // Run default for tapes installed without an explicit C++ capacity.
    CacheCapacity recorded_series_pages{256 * 1024};
    CacheCapacity projection_exit_pages = lines(512, 128);

    // Inclusive optional ledger hot-cache bytes, including all tags, packed
    // replacement ranks and page pointers. Provisional source default: 64 MiB;
    // not measurement-selected/frozen. Valid [0,SIZE_MAX]; tiny values bypass.
    // Explicit ledger_rows takes precedence, preserving its installed range.
    std::size_t ledger_cache_max_bytes = TradeLedger::kDefaultLedgerCacheMaxBytes;

    // Native admission must invoke this on its frozen per-run policy copy.
    // No conversion of an omitted LedgerCachePolicy into a finite row cap.
    void apply_ledger_cache(TradeLedger& ledger) const {
        if (ledger_rows.automatic())
            ledger.set_ledger_cache_max_bytes(ledger_cache_max_bytes);
        else
            ledger.set_cache_rows(ledger_cache_rows(ledger_rows));
    }
};

} // namespace pineforge
