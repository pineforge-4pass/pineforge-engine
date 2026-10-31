#pragma once

#include "cache_sweep_fixture.hpp"
#include "../src/read_through_window.hpp"
#include "../src/report_scratch.hpp"
#include "../src/source/pine_position_tail.hpp"

#include <cerrno>
#include <cstdlib>
#include <system_error>
#include <unistd.h>

namespace tail_curve_cache_sweep {
namespace pd = pineforge::detail;
namespace rs = pineforge::report_sink;
using cache_sweep::require;
using Tail = pineforge::source::detail::PositionTailSpill;

struct Directory {
    char path[64] = "/tmp/pineforge-tail-curve-sweep-XXXXXX";
    Directory() { require(::mkdtemp(path) != nullptr, "create acceptance scratch directory"); }
    ~Directory() { ::rmdir(path); }
};

inline void telemetry(cache_sweep::Telemetry& out, const pd::ReadThroughCacheStats& stats,
                      std::size_t payload, std::size_t page, std::size_t workspace) {
    const auto slots = payload / page;
    out.configured_payload = slots * page;
    out.configured_metadata = slots * (pd::ReadThroughCache::line_bytes(page) - page);
    out.payload = stats.current_payload_bytes;
    out.metadata = stats.current_metadata_bytes;
    out.peak_resident = stats.peak_bytes;
    out.hits = stats.hits;
    out.misses = stats.misses;
    out.evictions = stats.evictions;
    out.workspace = out.fixed_workspace = workspace;
    out.fixed_control = sizeof(pd::ReadThroughWindow);
}

inline cache_sweep::Result position_tail(cache_sweep::Setting setting) {
    Directory directory;
    const auto capacity = setting.defaults ? Tail::kWindowBytes : setting.units;
    Tail spill(directory.path, capacity);
    // Every ordinary row fills one page. The last id exceeds the default
    // cache too; all configurations run exactly the same retained workload.
    constexpr std::size_t rows = 81;
    std::vector<pineforge::Trade> reference;
    for (std::size_t i = 0; i != rows; ++i) {
        pineforge::Trade row{};
        row.entry_id.resize(Tail::kPageBytes - Tail::kFooterBytes);
        for (std::size_t j = 0; j != row.entry_id.size(); ++j)
            row.entry_id[j] = static_cast<char>((i * 37 + j * 11) & 255);
        row.exit_time = static_cast<std::int64_t>(i * 7919);
        row.qty = i % 2 ? -0.0 : static_cast<double>(i) * .25;
        row.entry_incarnation = i + 1;
        row.is_long = i % 3 != 0;
        row.exit_id = i % 2 ? pineforge::source::kMarginCallLabel : "ordinary";
        spill.append(row);
        reference.push_back(std::move(row));
    }
    auto oversized = reference.front();
    oversized.entry_id.resize(Tail::kWindowBytes + 17, 'z');
    spill.append(oversized);
    cache_sweep::Result result;
    const auto read = [&](std::size_t index) {
        const auto& expected = index == rows ? oversized : reference[index];
        auto cursor = index == rows ? spill.size_bytes()
            : static_cast<std::uint64_t>((index + 1) * Tail::kPageBytes);
        Tail::Row actual;
        require(spill.previous(cursor, actual), "acceptance tail row was refused");
        require(actual.exit_time == expected.exit_time
            && std::memcmp(&actual.qty, &expected.qty, sizeof(double)) == 0
            && actual.entry_incarnation == expected.entry_incarnation
            && actual.is_long == expected.is_long
            && actual.margin_call == (expected.exit_id == pineforge::source::kMarginCallLabel)
            && actual.entry_id_size == expected.entry_id.size()
            && spill.entry_id_equals(actual, expected.entry_id),
            "acceptance tail lost a complete field or identifier byte");
        auto& bytes = result.output.records;
        cache_sweep::scalar(bytes, actual.exit_time);
        cache_sweep::scalar(bytes, actual.qty);
        cache_sweep::scalar(bytes, actual.entry_incarnation);
        cache_sweep::scalar(bytes, actual.is_long);
        cache_sweep::scalar(bytes, actual.margin_call);
        cache_sweep::scalar(bytes, actual.entry_id_offset);
        cache_sweep::scalar(bytes, actual.entry_id_size);
        // Equality above checks every backing id byte without allocating an
        // owning value inside the reader. Serialize that verified value.
        cache_sweep::text(bytes, expected.entry_id);
    };
    const auto cold = spill.backing_read_count();
    read(0);
    result.cache.cold_reads = spill.backing_read_count() - cold;
    read(0);
    for (std::size_t i = 1; i <= rows; ++i) read(i);
    const auto before = spill.backing_read_count();
    read(0);
    result.cache.evicted_reread_reads = spill.backing_read_count() - before;
    telemetry(result.cache, spill.cache_stats(), capacity, Tail::kPageBytes, Tail::kPageBytes);
    result.cache.fixed_control = spill.cache_control_bytes();
    result.cache.inner_reads = spill.backing_read_count();
    result.cache.inner_bytes = spill.backing_read_bytes();
    return result;
}

inline cache_sweep::Result equity_curve(cache_sweep::Setting setting) {
    Directory directory;
    constexpr auto page = sizeof(pf_equity_point_t);
    constexpr auto large = std::size_t{256 * 1024};
    const auto capacity = setting.defaults ? rs::kScratchWindowBytes : setting.units;
    std::vector<pf_equity_point_t> reference;
    rs::ScratchFile file(directory.path);
    for (std::size_t i = 0; i != 2 * (large / page) + 17; ++i)
        reference.push_back({INT64_C(1704067200000) + static_cast<std::int64_t>(i) * 60000,
                             10000.0 + static_cast<double>((i * 71) % 997) / 8,
                             i % 3 ? -0.0 : .125});
    file.write(0, reference.data(), reference.size() * page);
    const pineforge::metrics::ScratchCurveRegion region{
        file.descriptor(), 0, static_cast<std::int64_t>(reference.size())};
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, capacity, &inner);
    cache_sweep::Result result;
    const auto read = [&](std::size_t index) {
        const auto actual = curve[static_cast<std::int64_t>(index)];
        require(std::memcmp(&actual, &reference[index], page) == 0,
                "acceptance curve lost complete point bytes");
        result.output.records.append(reinterpret_cast<const char*>(&actual), page);
    };
    const auto cold = inner.calls;
    read(0);
    result.cache.cold_reads = inner.calls - cold;
    read(0);
    for (std::size_t i = 1; i != reference.size(); ++i) read(i);
    const auto before = inner.calls;
    read(0);
    result.cache.evicted_reread_reads = inner.calls - before;
    // The final point was displaced by the reread of zero at tiny settings.
    // A new independent reader makes the error cold at every setting.
    rs::ScratchCurveWindow fault_reader(region, capacity, &inner);
    require(::ftruncate(file.descriptor(),
                static_cast<off_t>(reference.size() * page - page / 2)) == 0,
            "truncate the final curve point for a real cold I/O failure");
    const auto attempts = inner.calls;
    bool surfaced = false;
    try { (void)fault_reader[fault_reader.size() - 1]; }
    catch (const pd::ReportScratchIoError& error) {
        surfaced = error.code().value() == EIO;
    }
    require(surfaced && inner.calls > attempts, "cold curve backing fault did not surface");
    result.cache.fault_reads = inner.calls - attempts;
    telemetry(result.cache, curve.cache_stats(), capacity, page, page);
    result.cache.fixed_control = curve.cache_control_bytes();
    result.cache.inner_reads = inner.calls;
    result.cache.inner_bytes = inner.bytes;
    return result;
}

} // namespace tail_curve_cache_sweep
