#pragma once

#include "book_allowance_run_fixture.hpp"

#include <exception>

namespace book_allowance_run {
constexpr std::uint64_t resident_rows = 4096;
constexpr std::uint64_t points_per_bar = 4;

inline std::uint64_t spilled_refreshes(std::uint64_t bars) {
    const auto n = bars > resident_rows + 1 ? bars - resident_rows - 1 : 0;
    return points_per_bar * n * (n + 1) / 2;
}

inline void check_cost(Host& host, bool final) {
    const auto work = host.core().book_refresh_work();
    const auto expected = spilled_refreshes(host.callbacks);
    if (work.promotions || work.evictions || work.tombstones || work.compactions
        || work.keyed_searches || work.appended_writes
        || work.in_place_writes != expected || work.spilled_refreshes != expected) {
        std::fprintf(stderr,
            "B=%zu expected=%llu promotions=%llu evictions=%llu tombstones=%llu "
            "compactions=%llu searches=%llu overwrites=%llu appends=%llu refreshed=%llu\n",
            host.callbacks, static_cast<unsigned long long>(expected),
            static_cast<unsigned long long>(work.promotions),
            static_cast<unsigned long long>(work.evictions),
            static_cast<unsigned long long>(work.tombstones),
            static_cast<unsigned long long>(work.compactions),
            static_cast<unsigned long long>(work.keyed_searches),
            static_cast<unsigned long long>(work.in_place_writes),
            static_cast<unsigned long long>(work.appended_writes),
            static_cast<unsigned long long>(work.spilled_refreshes));
        throw std::runtime_error("allowance refresh exceeded its derived physical-work bound");
    }
    if (final) {
        require(host.core().book_spilled(), "counter witness exceeds the resident book");
        require(expected > 0, "counter witness actually refreshes spilled winners");
        std::printf("B=%zu spilled_refreshes=%llu in_place_writes=%llu; "
                    "refresh promotions/evictions/tombstones/compactions/searches/appends=0\n",
                    host.callbacks, static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(work.in_place_writes));
    }
}

inline int cost_main(std::size_t bars) {
    try {
        require(bars > resident_rows + 1, "cost population crosses the spill boundary");
        Host host(true, [](Host& current) {
            // Stop the unchanged engine at the first excessive work. A red
            // run never proceeds through millions of promotion swaps.
            if (current.callbacks > resident_rows + 1) check_cost(current, false);
        });
        const auto input = tape(bars);
        host.run(input.data(), static_cast<int>(input.size()));
        finish(host, bars);
        check_cost(host, true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "book allowance cost: %s\n", error.what());
        return 1;
    }
}
} // namespace book_allowance_run
