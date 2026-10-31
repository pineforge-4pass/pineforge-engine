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
        for (const auto bytes : {std::size_t{0}, sizeof(pf_equity_point_t) - 1,
                                 sizeof(pf_equity_point_t), 2 * sizeof(pf_equity_point_t),
                                 rs::kScratchWindowBytes})
            sweep(region, points, bytes);
        allocation_bypass(region, points);
        for (const auto bytes : {std::size_t{0}, sizeof(pf_equity_point_t),
                                 2 * sizeof(pf_equity_point_t), rs::kScratchWindowBytes})
            failed_refill(directory.path, bytes);
        std::puts("curve capacity sweep passed");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        std::fprintf(stderr, "curve capacity sweep: %s\n", error.what());
        return 1;
    }
}
