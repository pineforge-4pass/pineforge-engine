#include "cr257_io.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace cr257 {
namespace {

Count scaled(Count value, Count factor) {
    require(factor != 0 && value <= std::numeric_limits<Count>::max() / factor,
            "CR-257 scaled class bound overflow");
    return value * factor;
}
Count cold_window_calls(const Calls& reads) {
    if (reads.calls == 0) {
        require(reads.bytes == 0, "CR-257 class has bytes without reads");
        return 0;
    }
    require(reads.bytes >= reads.calls, "CR-257 class contains zero-sized reads");
    // mean = bytes/calls; ceil(kCacheBytes/mean), without floating rounding.
    return ceil_div(scaled(reads.calls, kCacheBytes), reads.bytes);
}
void class_growth(FileClass file, const SourceRun& small, const SourceRun& large, Count scale) {
    const auto& before = small.io.classes[class_index(file)].pread;
    const auto& after = large.io.classes[class_index(file)].pread;
    // Use the smaller nonzero mean read size across both runs, conservatively
    // rounding the number of calls in ONE cold 64 KiB window upward. Inactive
    // classes have a zero window. Neither the budget nor scale is a run-tuned
    // literal: scale = Nlarge/Nsmall, tolerance = ceil(cache/mean)/Nsmall.
    const Count window_calls = std::max(cold_window_calls(before), cold_window_calls(after));
    const Count window_bytes = window_calls != 0 ? kCacheBytes : 0;
    const long double mean_before = before.calls != 0
        ? static_cast<long double>(before.bytes) / before.calls : 0;
    const long double mean_after = after.calls != 0
        ? static_cast<long double>(after.bytes) / after.calls : 0;
    const long double mean = before.calls == 0 ? mean_after
        : after.calls == 0 ? mean_before : std::min(mean_before, mean_after);
    std::printf("X257 class=%s mean_read_bytes=%.9Lf cold_window_bytes=%llu"
                " cold_window_calls=%llu tolerance=%.9Lf byte_tolerance=%.9Lf"
                " scale=%llu Nsmall=%llu Nlarge=%llu INFO\n",
        kClassNames[class_index(file)], mean,
        static_cast<unsigned long long>(window_bytes),
        static_cast<unsigned long long>(window_calls),
        static_cast<long double>(window_calls) / small.context.n,
        static_cast<long double>(window_bytes) / small.context.n,
        static_cast<unsigned long long>(scale),
        static_cast<unsigned long long>(small.context.n),
        static_cast<unsigned long long>(large.context.n));
    const Count scaled_calls = scaled(before.calls, scale);
    const Count scaled_bytes = scaled(before.bytes, scale);
    const Count calls_delta = after.calls > scaled_calls
        ? after.calls - scaled_calls : scaled_calls - after.calls;
    const Count bytes_delta = after.bytes > scaled_bytes
        ? after.bytes - scaled_bytes : scaled_bytes - after.bytes;
    // With an integral population ratio, these integer checks are exactly
    // abs(after/Nlarge - before/Nsmall) <= one_window/Nsmall, in both units.
    class_bound(file, "pread_calls_flatness", calls_delta, scaled(window_calls, scale),
                large.context);
    class_bound(file, "pread_bytes_flatness", bytes_delta, scaled(window_bytes, scale),
                large.context);
    require(before.calls <= std::numeric_limits<Count>::max() - window_calls
            && before.bytes <= std::numeric_limits<Count>::max() - window_bytes,
            "CR-257 class window bound overflow");
    // Also state the large-run total limits explicitly: scaled baseline plus
    // its one-window tolerance, separately for each class (including other).
    class_bound(file, "pread_calls_scaled", after.calls,
                scaled(before.calls + window_calls, scale), large.context);
    class_bound(file, "pread_bytes_scaled", after.bytes,
                scaled(before.bytes + window_bytes, scale), large.context);
}
} // namespace

void source_100k() {
    // This selectable row runs BOTH populations itself; it never relies on
    // another CTest's output, scheduling order, or a stored timing/count table.
    const auto small = source_run(kSmallN);
    const auto large = source_run(kLargeN);
    source_bounds(small);
    source_bounds(large);
    require(small.context.n != 0 && large.context.n > small.context.n
            && large.context.n % small.context.n == 0,
            "CR-257 class flatness needs an integral population ratio");
    const Count scale = large.context.n / small.context.n;
    for (std::size_t i = 0; i < kClassNames.size(); ++i) {
        const auto file = static_cast<FileClass>(i);
        // These two have independent absolute acceptance bounds at BOTH Ns:
        // command <= 2*extent_pages, payload == 0. Leftovers are flatness only.
        if (file != FileClass::command && file != FileClass::trade_payload)
            class_growth(file, small, large, scale);
    }
    // Saturating subtraction expresses an upper bound on growth; a decrease
    // is allowed and must not become an unsigned-underflow failure.
    const Count total_growth = large.io.total.pread.calls > small.io.total.pread.calls
        ? large.io.total.pread.calls - small.io.total.pread.calls : 0;
    const Count page_growth = large.io.pages.pread.calls > small.io.pages.pread.calls
        ? large.io.pages.pread.calls - small.io.pages.pread.calls : 0;
    total_info("total_pread_growth_100k_minus_10k", total_growth, large.context);
    bound("command.page_pread_growth_100k_minus_10k", page_growth,
          large.context.extent_pages, large.context);
}

} // namespace cr257
