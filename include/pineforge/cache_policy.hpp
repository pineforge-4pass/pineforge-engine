#pragma once

#include <pineforge/detail/cache_capacity.hpp>
#include <pineforge/trade_ledger.hpp>
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <limits>

namespace pineforge {

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

    CacheCapacity ledger_rows{
        TradeLedger::cache_capacity_bytes(TradeLedger::kDefaultCacheRows)};
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
};

} // namespace pineforge
