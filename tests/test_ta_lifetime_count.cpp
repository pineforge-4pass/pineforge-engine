#include <pineforge/ta.hpp>
#include <pineforge/window_sum.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>

int main() {
    constexpr int max_count = std::numeric_limits<int>::max();
    static_assert(max_count == 2147483647, "this boundary requires a 32-bit int");
    constexpr std::uint64_t boundary = std::uint64_t{1} << 31;
    constexpr std::uint64_t observations[] = {
        boundary - 2, boundary - 1, boundary, boundary + 1, boundary + 8
    };

    pineforge::KahanWindowSum window(1);
    pineforge::ta::RMA rma(2);
    std::uint64_t pushes = 0;
    double sum = 0.0, average = 0.0;
    bool ok = true;
    const auto start = std::chrono::steady_clock::now();
    for (const auto target : observations) {
        for (; pushes < target; ++pushes) {
            sum = window.push(1.0);
            average = rma.compute(1.0);
        }
        // One retained 1 sums to 1. The Wilder seed is (1 + 1) / 2,
        // and each subsequent step averages that 1 with the next 1.
        const int expected_count = pushes < boundary ? static_cast<int>(pushes) : max_count;
        const bool matches = window.count() == expected_count && window.seeded()
                             && sum == 1.0 && average == 1.0;
        std::cout << "pushes=" << pushes << " count=" << window.count()
                  << " seeded=" << window.seeded() << " sum=" << sum
                  << " rma=" << average << (matches ? " PASS" : " FAIL") << std::endl;
        ok = ok && matches;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::cout << "elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
    return ok ? 0 : 1;
}
