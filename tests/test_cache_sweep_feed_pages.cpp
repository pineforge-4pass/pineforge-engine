#include "cache_sweep_fixture.hpp"
#include <pineforge/detail/feed_projection.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace feed_cache_sweep {
using cache_sweep::require;
namespace pf = pineforge;
using TapeRow = pf::source::RecordedSeriesRow;
constexpr std::size_t large_capacity_bytes = 512 * 1024;

template<class Row> struct Rows;
template<> struct Rows<std::int64_t> {
    static std::int64_t value(std::size_t i) {
        return static_cast<std::int64_t>((i * 97) % 100003) - 50001;
    }
    static void check(std::int64_t actual, std::size_t i) {
        require(actual == value(i), "calendar projection changed a logical row");
    }
    static void append(std::string& bytes, std::int64_t row) {
        cache_sweep::scalar(bytes, row);
    }
};
template<> struct Rows<TapeRow> {
    static TapeRow value(std::size_t i) {
        TapeRow row{INT64_C(1704067200000) + static_cast<std::int64_t>(i) * 60000,
                    (static_cast<int>(i % 997) - 498) * .125};
        if (i % 29 == 0) row.value = -0.0;
        if (i % 31 == 5) {
            const std::uint64_t bits = UINT64_C(0x7ff8000000000042);
            std::memcpy(&row.value, &bits, sizeof bits);
        }
        return row;
    }
    static void check(const TapeRow& actual, std::size_t i) {
        const auto expected = value(i);
        require(actual.open_ms == expected.open_ms
            && std::memcmp(&actual.value, &expected.value, sizeof actual.value) == 0,
            "recorded projection changed timestamp or binary64 bytes");
    }
    static void append(std::string& bytes, const TapeRow& row) {
        cache_sweep::scalar(bytes, row.open_ms);
        cache_sweep::scalar(bytes, row.value);
    }
};

// Faults occur below the shared cache. Complete one real disk transfer before
// throwing, so failure/retry checks cannot pass on cache-owned counters.
template<class Row>
class FaultReader final : public pf::FeedReader<Row> {
public:
    explicit FaultReader(std::shared_ptr<const pf::detail::ScratchFeedReader<Row>> reader)
        : reader_(std::move(reader)) {}
    bool fail = false;
    std::size_t size() const noexcept override { return reader_->size(); }
    void read(std::size_t first, Row* destination, std::size_t count) const override {
        if (fail && count) {
            reader_->read(first, destination, 1);
            throw pf::detail::AuxiliaryScratchIoError(ENOSPC, "fixture projection backing");
        }
        reader_->read(first, destination, count);
    }
private:
    std::shared_ptr<const pf::detail::ScratchFeedReader<Row>> reader_;
};

template<class Row>
cache_sweep::Result run(cache_sweep::Setting setting) {
    using Projection = pf::detail::FeedProjection<Row>;
    const auto line = Projection::page_bytes();
    const auto payload = Projection::page_payload_bytes();
    // Fixed for every setting: more pages than twice the largest cache and
    // one exact partial tail. Cache capacity never selects the population.
    const auto pages = 2 * (large_capacity_bytes / line) + 1;
    const auto count = (pages - 1) * Projection::rows_per_page() + 17;
    pf::detail::FeedProjectionBuilder<Row> builder;
    for (std::size_t i = 0; i < count; ++i) builder.append(Rows<Row>::value(i));
    const auto provider = builder.finish(0);
    const auto disk = std::dynamic_pointer_cast<
        const pf::detail::ScratchFeedReader<Row>>(provider.reader());
    require(bool(disk), "feed sweep did not construct its disk provider");
    auto faults = std::make_shared<FaultReader<Row>>(disk);
    Projection projection = setting.defaults ? Projection(faults) : Projection(faults, setting.units);
    const auto configured = setting.defaults ? pf::detail::kFeedProjectionCacheBytes : setting.units;
    const auto slots = configured / line;
    cache_sweep::Result result;
    auto& s = result.cache;
    s.configured_payload = slots * payload;
    s.configured_metadata = slots * (line - payload);
    s.fixed_control = sizeof(Projection) + sizeof(FaultReader<Row>)
        + sizeof(pf::detail::ScratchFeedReader<Row>);
    s.fixed_workspace = Projection::transfer_workspace_bytes();
    // The reader transfer page and the test's copy-out page coexist.
    s.workspace = s.fixed_workspace + payload;
    require(projection.cache_policy().replacement
                == pf::detail::ReadThroughCacheReplacement::direct_mapped
            && projection.cache_policy().write_policy
                == pf::detail::ReadThroughCacheWritePolicy::write_through,
            "feed projection changed its default replacement or write policy");

    const auto read = [&](std::size_t page) {
        const auto first = page * Projection::rows_per_page();
        const auto rows = std::min(Projection::rows_per_page(), count - first);
        std::array<Row, Projection::rows_per_page()> output;
        projection.read(first, output.data(), rows);
        for (std::size_t i = 0; i < rows; ++i) {
            Rows<Row>::check(output[i], first + i);
            Rows<Row>::append(result.output.records, output[i]);
        }
    };
    const auto before_cold = disk->read_calls();
    read(0);
    s.cold_reads = disk->read_calls() - before_cold;
    require(s.cold_reads == 1, "first projection page never reached disk");
    const auto before_warm = disk->read_calls();
    read(0);
    require(disk->read_calls() == before_warm + (slots ? 0 : 1),
            "projection warm read did not follow its cache policy");
    for (std::size_t page = 1; page < pages; ++page) read(page);
    const auto before_reread = disk->read_calls();
    read(0);
    s.evicted_reread_reads = disk->read_calls() - before_reread;
    require(s.evicted_reread_reads == 1, "old projection page was never physically reread");

    std::array<Row, 23> slice;
    const auto slice_first = Projection::rows_per_page() - 5;
    projection.read(slice_first, slice.data(), slice.size());
    for (std::size_t i = 0; i < slice.size(); ++i) {
        Rows<Row>::check(slice[i], slice_first + i);
        Rows<Row>::append(result.output.records, slice[i]);
    }
    const auto stats = projection.cache_stats();
    require(stats.current_payload_bytes == slots * payload
        && stats.current_metadata_bytes == slots * (line - payload),
        "projection did not allocate the configured payload and metadata");
    s.payload = stats.current_payload_bytes;
    s.metadata = stats.current_metadata_bytes;
    s.peak_resident = stats.peak_bytes;
    s.hits = stats.hits;
    s.misses = stats.misses;
    s.evictions = stats.evictions;

    // Reconstruct a cold cache at the same budget for every setting.
    projection.set_capacity_bytes(configured);
    faults->fail = true;
    const auto before_fault = disk->read_calls();
    bool failed = false;
    try { (void)projection[0]; }
    catch (const pf::detail::AuxiliaryScratchIoError& error) {
        failed = error.code().value() == ENOSPC;
    }
    s.fault_reads = disk->read_calls() - before_fault;
    require(failed && s.fault_reads == 1, "cold projection backing error was hidden");
    faults->fail = false;
    const auto before_retry = disk->read_calls();
    Rows<Row>::check(projection[0], 0);
    require(disk->read_calls() == before_retry + 1,
            "partial failed projection fill published a tag");
    s.inner_reads = disk->read_calls();
    s.inner_bytes = disk->read_bytes();
    return result;
}
} // namespace feed_cache_sweep

int main() {
    try {
        using namespace feed_cache_sweep;
        cache_sweep::sweep("feed_calendar_columns",
            pf::detail::FeedProjection<std::int64_t>::page_bytes(),
            pf::detail::kFeedProjectionCacheBytes, large_capacity_bytes, run<std::int64_t>);
        cache_sweep::sweep("recorded_series_pages",
            pf::detail::FeedProjection<TapeRow>::page_bytes(),
            pf::detail::kFeedProjectionCacheBytes, large_capacity_bytes, run<TapeRow>);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "feed page sweep FAIL: %s\n", error.what());
        return 1;
    }
}
