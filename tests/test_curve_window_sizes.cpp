#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/report_scratch.hpp"
#include <pineforge/detail/read_through_cache.hpp>

#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
namespace rs = pineforge::report_sink;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Directory {
    char path[64] = "/tmp/pineforge-curve-windows-XXXXXX";
    Directory() { require(::mkdtemp(path) != nullptr, "create curve directory"); }
    ~Directory() { ::rmdir(path); }
};

bool equal(const pf_equity_point_t& a, const pf_equity_point_t& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

constexpr std::size_t kBoundaryBlockAllowance = 2;

std::size_t block_count(std::size_t points) {
    return points == 0 ? 0
        : (points + rs::kScratchWindowPoints - 1) / rs::kScratchWindowPoints;
}

void curve_pass_read_cost(const pineforge::metrics::ScratchCurveRegion& region,
                          const std::vector<pf_equity_point_t>& points, bool reverse) {
    require(points.size() > rs::kScratchWindowPoints,
            "curve read-cost row must exceed the configured point capacity");
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, rs::kScratchWindowBytes, &inner);
    require(equal(curve[0], points[0]), "first boundary probe is exact");
    require(equal(curve[static_cast<std::int64_t>(points.size() - 1)], points.back()),
            "last boundary probe is exact");
    const auto probes = inner.calls;
    // The two nonadjacent endpoints are distinct cold reads before the pass.
    require(probes == 2, "two boundary probes reach backing before the pass");
    if (!reverse) {
        for (std::size_t i = 0; i != points.size(); ++i)
            require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                    "forward block walk is exact");
    } else {
        for (std::size_t i = points.size(); i-- > 0;)
            require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                    "reverse block walk is exact");
    }
    const auto blocks = block_count(points.size());
    const auto bound = blocks + kBoundaryBlockAllowance;
    std::printf("curve %s points=%zu blocks=%zu allowance=%zu probes=%llu "
                "backing_reads=%llu backing_bytes=%llu bound=%zu\n",
                reverse ? "reverse" : "forward", points.size(), blocks,
                kBoundaryBlockAllowance, static_cast<unsigned long long>(probes),
                static_cast<unsigned long long>(inner.calls),
                static_cast<unsigned long long>(inner.bytes), bound);
    require(inner.calls <= bound, "sequential curve reads stay at block cost");
}

void bypass_pass_read_cost(const pineforge::metrics::ScratchCurveRegion& region,
                           const std::vector<pf_equity_point_t>& points) {
    for (const auto bytes : {std::size_t{0}, sizeof(pf_equity_point_t) - 1}) {
        rs::ScratchReadStats inner;
        rs::ScratchCurveWindow curve(region, bytes, &inner);
        for (std::size_t i = 0; i != points.size(); ++i)
            require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                    "bypass curve walk is exact");
        std::printf("curve bypass requested=%zu points=%zu backing_reads=%llu "
                    "backing_bytes=%llu\n",
                    bytes, points.size(), static_cast<unsigned long long>(inner.calls),
                    static_cast<unsigned long long>(inner.bytes));
        require(inner.calls == points.size()
                    && inner.bytes == points.size() * sizeof(pf_equity_point_t),
                "zero and sub-point capacities read one point per bypass access");
    }
}

void random_access_read_cost(const pineforge::metrics::ScratchCurveRegion& region,
                            const std::vector<pf_equity_point_t>& points) {
    constexpr std::size_t kRandomSamples = 257;
    std::mt19937_64 random(UINT64_C(0x4d7a2c91b8e60315));
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, rs::kScratchWindowBytes, &inner);
    std::size_t adjacent_pairs = 0;
    std::size_t previous_index = 0;
    for (std::size_t sample = 0; sample != kRandomSamples; ++sample) {
        const auto index = static_cast<std::size_t>(
            random() % static_cast<std::uint64_t>(points.size()));
        if (sample != 0
            && (index == previous_index + 1 || index + 1 == previous_index))
            ++adjacent_pairs;
        require(equal(curve[static_cast<std::int64_t>(index)], points[index]),
                "random curve access is exact");
        previous_index = index;
    }
    // read_backing requests one point on a nonadjacent miss and can fill an
    // aligned block only for an index +/-1 from the previous lookup. Reserve
    // sizeof(pf_equity_point_t) per sample plus one complete block workspace
    // per adjacent pair in the generated sequence; hits and short final
    // blocks can only reduce the actual backing bytes.
    const auto byte_bound = kRandomSamples * sizeof(pf_equity_point_t)
        + adjacent_pairs * rs::kScratchWindowPoints * sizeof(pf_equity_point_t);
    std::printf("curve random samples=%zu adjacent_pairs=%zu backing_reads=%llu backing_bytes=%llu "
                "read_bound=%zu byte_bound=%zu\n",
                kRandomSamples, adjacent_pairs, static_cast<unsigned long long>(inner.calls),
                static_cast<unsigned long long>(inner.bytes), kRandomSamples, byte_bound);
    require(inner.calls <= kRandomSamples && inner.bytes <= byte_bound,
            "random accesses do not exceed one backing operation per lookup");
}

void extremes_read_cost(const pineforge::metrics::ScratchCurveRegion& region,
                        const std::vector<pf_equity_point_t>& points) {
    pf_equity_point_t held = points.back();
    held.time_ms += 1;
    held.equity = points.back().equity + 17.0;
    double expected[] = {10000.0, 10000.0, 0.0, 0.0};
    const auto fold = [&](double equity) {
        if (equity > expected[0]) {
            expected[0] = equity;
            expected[1] = equity;
        }
        if (equity < expected[1]) expected[1] = equity;
        const double drawdown = expected[0] - equity;
        if (drawdown > expected[2]) expected[2] = drawdown;
        const double runup = equity - expected[1];
        if (runup > expected[3]) expected[3] = runup;
    };
    for (const auto& point : points) fold(point.equity);
    fold(held.equity);
    // The extremes check builds its own default-capacity window over the
    // prefix and exposes no read counter of its own: its backing reads are the
    // forward-pass reads bounded by curve_pass_read_cost, over the same window
    // class. This row pins that the check still folds every point exactly (it
    // throws on a mismatch) after the window changes.
    rs::check_scratch_equity_extremes(region, &held, 10000.0, expected);
    std::printf("curve extremes points=%zu exact\n", points.size());
}

std::vector<pf_equity_point_t> bit_pattern_points(std::size_t count) {
    std::mt19937_64 random(UINT64_C(0x9a31f0c44b7e2086));
    std::vector<pf_equity_point_t> points(count);
    for (std::size_t i = 0; i != points.size(); ++i) {
        points[i].time_ms = static_cast<std::int64_t>(random());
        const auto equity = random();
        const auto open_profit = random();
        std::memcpy(&points[i].equity, &equity, sizeof(equity));
        std::memcpy(&points[i].open_profit, &open_profit, sizeof(open_profit));
    }
    return points;
}

void bit_exact_block_fill(const char* directory_path) {
    const auto points = bit_pattern_points(2 * rs::kScratchWindowPoints + 17);
    rs::ScratchCurveWriter writer(directory_path);
    for (const auto& point : points) writer.append(point);
    const auto region = writer.region();
    rs::ScratchCurveWindow curve(region, rs::kScratchWindowBytes);
    for (std::size_t i = 0; i != points.size(); ++i)
        require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                "forward block fill preserves every point bit");
    for (std::size_t i = points.size(); i-- > 0;)
        require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                "reverse block fill preserves every point bit");
    std::puts("curve bit-exact block fill passed");
}

void curve_read_cost_rows(const pineforge::metrics::ScratchCurveRegion& region,
                          const std::vector<pf_equity_point_t>& points) {
    curve_pass_read_cost(region, points, false);
    curve_pass_read_cost(region, points, true);
    bypass_pass_read_cost(region, points);
    random_access_read_cost(region, points);
    extremes_read_cost(region, points);
}

void sweep(const pineforge::metrics::ScratchCurveRegion& region,
           const std::vector<pf_equity_point_t>& points, std::size_t bytes) {
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, bytes, &inner);
    const auto expected_capacity = bytes / sizeof(pf_equity_point_t) * sizeof(pf_equity_point_t);
    require(curve.cache_capacity_bytes() == expected_capacity,
            "curve capacity counts only complete points");
    const auto allocations = global_allocation::allocations;
    global_allocation::live_bytes = 0;
    global_allocation::recording = true;
    const auto first = curve[0];
    global_allocation::recording = false;
    require(equal(first, points[0]), "first point reads exactly");
    require(global_allocation::live_bytes == curve.cache_stats().current_bytes
                && global_allocation::allocations - allocations == (expected_capacity ? 1 : 0),
            "global allocation census matches every shared curve payload and metadata byte");
    const auto saved = curve[0];
    require(points.size() > expected_capacity / sizeof(pf_equity_point_t),
            "curve workload exceeds every configured cache");
    // A distant point alone need not replace point zero in a multi-line
    // cache. Visit the entire larger-than-cache curve before the cold reread.
    for (std::int64_t i = 1; i != curve.size(); ++i)
        require(equal(curve[i], points[static_cast<std::size_t>(i)]),
                "complete eviction pass reads exact curve bytes");
    require(equal(curve[curve.size() - 1], points.back()), "last point reads exactly");
    const auto before = inner.calls;
    require(equal(curve[0], points[0]), "early evicted point remains readable");
    require(inner.calls > before, "cold curve read reaches the inner backing");
    require(equal(saved, points[0]), "returned point survives cache replacement");
    for (std::int64_t i = 0; i != curve.size(); ++i)
        require(equal(curve[i], points[static_cast<std::size_t>(i)]),
                "complete forward curve bytes match");
    for (auto i = curve.size(); i != 0; --i)
        require(equal(curve[i - 1], points[static_cast<std::size_t>(i - 1)]),
                "complete reverse curve bytes match");
    const pineforge::metrics::ArrayCurve array(points.data(), curve.size());
    const auto n = curve.size();
    const auto reference = pineforge::metrics::compute_equity_stats(
        array, n, 10000.0, "UTC", 100.0, 97.0, n / 3, points.back().equity - 10000.0);
    const auto actual = pineforge::metrics::compute_equity_stats(
        curve, n, 10000.0, "UTC", 100.0, 97.0, n / 3, points.back().equity - 10000.0);
    require(std::memcmp(&actual, &reference, sizeof(actual)) == 0,
            "all metric bits are independent of curve cache capacity");
    const auto stats = curve.cache_stats();
    require(stats.get_calls > 0 && stats.misses > 0,
            "curve reads use the shared cache, including bypass");
    require(stats.current_payload_bytes == expected_capacity
                && stats.current_bytes == stats.current_payload_bytes + stats.current_metadata_bytes,
            "curve shared cache discloses complete payload and line charges");
    if (expected_capacity == 0) {
        const auto uncached = inner.calls;
        (void)curve[0];
        (void)curve[0];
        require(inner.calls == uncached + 2, "zero and sub-point capacities bypass each read");
    }
    std::printf("curve requested=%zu payload=%zu backing_reads=%llu backing_bytes=%llu points=%lld\n",
                bytes, curve.cache_capacity_bytes(), static_cast<unsigned long long>(inner.calls),
                static_cast<unsigned long long>(inner.bytes), static_cast<long long>(curve.size()));
}

void allocation_bypass(const pineforge::metrics::ScratchCurveRegion& region,
                       const std::vector<pf_equity_point_t>& points) {
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, rs::kScratchWindowBytes, &inner);
    const auto refused = global_allocation::refused;
    global_allocation::fail_after = 0; // refuse only the first read's lazy cache allocation
    const auto first = curve[0];
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    require(global_allocation::refused == refused + 1
                && curve.cache_stats().allocation_bypass_count == 1
                && curve.cache_capacity_bytes() == 0,
            "optional curve allocation failure selects permanent shared-cache bypass");
    require(equal(first, points[0]) && equal(curve[curve.size() - 1], points.back())
                && inner.calls == 2,
            "curve allocation bypass preserves complete point bytes");
}

void failed_refill(const char* directory, std::size_t bytes) {
    rs::ScratchFile backing(directory);
    const auto capacity = std::max<std::size_t>(1, bytes / sizeof(pf_equity_point_t));
    std::vector<pf_equity_point_t> points(2 * capacity + 1);
    for (std::size_t i = 0; i != points.size(); ++i)
        points[i] = {static_cast<std::int64_t>(i), 10000.0 + i * 0.25, -0.0};
    backing.write(0, points.data(), points.size() * sizeof(points[0]));
    // Exercise the older raw-descriptor binding, whose physical counter is
    // incremented beside pread rather than in the cache/refill path.
    const pineforge::metrics::ScratchCurveRegion region{
        backing.descriptor(), 0, static_cast<std::int64_t>(points.size())};
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, bytes, &inner);
    require(equal(curve[0], points[0]), "prime the old reader window");
    const auto cut = capacity * sizeof(points[0]) + sizeof(points[0]) / 2;
    require(::ftruncate(backing.descriptor(), static_cast<off_t>(cut)) == 0,
            "truncate the next window inside a point");
    const auto before = inner.calls;
    bool failed = false;
    try {
        (void)curve[static_cast<std::int64_t>(capacity)];
    } catch (const std::system_error& error) {
        const auto failure = pineforge::classify_run_failure(error);
        require(error.code().value() == EIO
                    && failure.code == pineforge::RunFailureCode::report_sink_failed,
                "cold short backing keeps its typed I/O failure");
        failed = true;
    }
    require(failed && inner.calls > before, "short cold read fails at the actual backing");
    backing.write(0, points.data(), points.size() * sizeof(points[0]));
    const auto retry = inner.calls;
    require(equal(curve[static_cast<std::int64_t>(capacity)], points[capacity])
                && inner.calls > retry,
            "failed cold refill publishes no tag and retries backing after repair");
    require(equal(curve[0], points[0]),
            "failed refill preserves every unchanged early point");
    require(equal(curve[curve.size() - 1], points.back()), "repaired cold backing is retryable");
}

void failed_adjacent_block_refill(const char* directory) {
    rs::ScratchFile backing(directory);
    const auto points = bit_pattern_points(2 * rs::kScratchWindowPoints + 1);
    backing.write(0, points.data(), points.size() * sizeof(points[0]));
    const pineforge::metrics::ScratchCurveRegion region{
        backing.descriptor(), 0, static_cast<std::int64_t>(points.size())};
    rs::ScratchReadStats inner;
    rs::ScratchCurveWindow curve(region, sizeof(pf_equity_point_t), &inner);
    // One point of payload reserves exactly one complete shared cache line.
    require(curve.cache_capacity_bytes() == sizeof(pf_equity_point_t),
            "adjacent short-read row uses a single point cache line");
    // The recovery probe must distinguish the old block from its replacement.
    require(!equal(points[5], points[rs::kScratchWindowPoints + 5]),
            "old and replacement block probes have distinct bits");
    // Lookup 0 reads and retains one point; lookup 1 is already an adjacent
    // miss and fills block 0. Every expected point was written above verbatim.
    for (std::size_t i = 0; i != rs::kScratchWindowPoints; ++i)
        require(equal(curve[static_cast<std::int64_t>(i)], points[i]),
                "prime the complete old block with exact point bytes");
    // One initial point plus one complete block; later misses use that block.
    require(inner.bytes == (rs::kScratchWindowPoints + 1) * sizeof(points[0]),
            "forward walk fills the aligned old block");

    const auto cut = (rs::kScratchWindowPoints + 2000) * sizeof(points[0])
        + sizeof(points[0]) / 2;
    // This open descriptor can be shortened inside block 1, after point 5
    // of that block and halfway through its point 2000.
    require(::ftruncate(backing.descriptor(), static_cast<off_t>(cut)) == 0,
            "truncate the adjacent block inside a point");
    const auto before = inner.calls;
    const auto before_bytes = inner.bytes;
    bool failed = false;
    try {
        // The preceding lookup was the last point of block 0, so this miss
        // requests all of block 1 rather than its intact first point alone.
        (void)curve[static_cast<std::int64_t>(rs::kScratchWindowPoints)];
    } catch (const std::system_error& error) {
        const auto failure = pineforge::classify_run_failure(error);
        // read_exact reaches EOF after a positive prefix and raises EIO
        // through ReportScratchIoError, whose code is report_sink_failed.
        require(error.code().value() == EIO
                    && failure.code == pineforge::RunFailureCode::report_sink_failed,
                "adjacent short block keeps its typed I/O failure");
        failed = true;
    }
    // EOF must throw after reaching the raw descriptor, not publish a block.
    require(failed && inner.calls > before,
            "short adjacent block read fails at the actual backing");
    // read_exact counts the 2000 complete points and half-point before EOF.
    require(inner.bytes - before_bytes
                == cut - rs::kScratchWindowPoints * sizeof(points[0]),
            "failed adjacent fill overwrites a positive block prefix");

    const auto retry_bytes = inner.bytes;
    // Point 5's file bytes are intact and its shared line is cold. Clearing
    // count forces a reread; retaining block 0's tag would return point
    // kScratchWindowPoints + 5 from the overwritten array element instead.
    require(equal(curve[5], points[5]),
            "failed adjacent fill leaves no partially overwritten block in use");
    // Index 5 is nonadjacent to the last successful lookup, so only it is read.
    require(inner.bytes == retry_bytes + sizeof(points[0]),
            "recovery reads the intact early point from backing");
}
} // namespace

int main() {
    try {
        Directory directory;
        rs::ScratchCurveWriter writer(directory.path);
        std::vector<pf_equity_point_t> points;
        for (std::size_t i = 0; i != 2 * rs::kScratchWindowPoints + 17; ++i) {
            points.push_back({1704067199000LL + static_cast<std::int64_t>(i) * 86400000LL,
                              10000.0 + static_cast<double>((i * 71) % 997) / 8.0,
                              i % 3 ? -0.0 : 0.125});
            writer.append(points.back());
        }
        const auto region = writer.region();
        curve_read_cost_rows(region, points);
        // Endpoint probes collide in the default point cache at these sizes.
        for (const auto count : {rs::kScratchWindowPoints + 1, 2 * rs::kScratchWindowPoints + 1}) {
            auto prefix = region;
            prefix.point_count = static_cast<std::int64_t>(count);
            const std::vector<pf_equity_point_t> prefix_points(points.begin(), points.begin() + count);
            curve_pass_read_cost(prefix, prefix_points, false);
            curve_pass_read_cost(prefix, prefix_points, true);
        }
        bit_exact_block_fill(directory.path);
        for (const auto bytes : {std::size_t{0}, sizeof(pf_equity_point_t) - 1,
                                 sizeof(pf_equity_point_t), 2 * sizeof(pf_equity_point_t),
                                 rs::kScratchWindowBytes})
            sweep(region, points, bytes);
        allocation_bypass(region, points);
        for (const auto bytes : {std::size_t{0}, sizeof(pf_equity_point_t),
                                 2 * sizeof(pf_equity_point_t), rs::kScratchWindowBytes})
            failed_refill(directory.path, bytes);
        failed_adjacent_block_refill(directory.path);
        std::puts("curve capacity sweep passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "curve capacity sweep: %s\n", error.what());
        return 1;
    }
}
