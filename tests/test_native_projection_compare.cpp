// R5 lane PERF-P23: the projection check's string equality against
// std::string's own, bit for bit.
//
// NativeExecutionConsumer::projection_ok compares ten strings at every input
// and callback boundary. The lane replaced std::string's operator== there with
// same_text -- equal length, then every byte inside one of a few word loads,
// the last overlapping the word before it -- which must answer exactly what
// operator== answers for every pair of strings. This witness holds the two
// against each other: exhaustively, for every length up to 40 with the pair
// differing at each single byte position in turn (so every byte a load has to
// cover is shown to be covered), and over two million randomized pairs of
// lengths 0..72 -- equal, one byte apart, a prefix of each other, or
// arbitrary -- with every byte value, NUL and high bytes included, and in
// both argument orders. The consumer exposes the function it compares with
// as NativeExecutionConsumer::same_bytes, reached through its test probe.
//
// Fail-before: at the lane's base the consumer has no same_bytes, so this TU
// does not compile there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    static bool same_bytes(const std::string& a, const std::string& b) {
        return NativeExecutionConsumer::same_bytes(a, b);
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

namespace {

using pineforge::NativeExecutionConsumerProbe;

std::uint64_t compared = 0;
std::uint64_t mismatched = 0;

void hold(const std::string& a, const std::string& b) {
    const bool expected = a == b;
    for (int order = 0; order < 2; ++order) {
        const bool observed = order == 0 ? NativeExecutionConsumerProbe::same_bytes(a, b)
                                         : NativeExecutionConsumerProbe::same_bytes(b, a);
        ++compared;
        if (observed != expected) {
            if (++mismatched <= 10) {
                std::fprintf(stderr, "MISMATCH sizes %zu/%zu order %d: same_bytes %d, == %d\n",
                             a.size(), b.size(), order, observed ? 1 : 0, expected ? 1 : 0);
            }
        }
    }
}

std::string random_text(std::mt19937_64& rng, std::size_t n) {
    std::string text(n, '\0');
    for (char& c : text) c = static_cast<char>(rng() & 0xFF);
    return text;
}

// Every length up to 40, the pair differing at each single position, by every
// one-bit flip and by the high bit: a load that skipped a byte would miss it.
void every_position_of_every_length() {
    std::mt19937_64 rng(0xC0FFEEULL);
    for (std::size_t n = 0; n <= 40; ++n) {
        const std::string base = random_text(rng, n);
        hold(base, base);
        hold(base, std::string(base));
        for (std::size_t i = 0; i < n; ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                std::string other = base;
                other[i] = static_cast<char>(other[i] ^ (1 << bit));
                hold(base, other);
            }
        }
        // One byte longer or shorter, sharing the whole common prefix.
        hold(base, base + static_cast<char>(rng() & 0xFF));
        if (n > 0) hold(base, base.substr(0, n - 1));
    }
}

void randomized_pairs() {
    std::mt19937_64 rng(0x5EEDF00DULL);
    for (int i = 0; i < 2000000; ++i) {
        const std::size_t n = static_cast<std::size_t>(rng() % 73);
        const std::string a = random_text(rng, n);
        std::string b = a;
        switch (rng() % 4) {
        case 0:
            break;  // equal
        case 1:
            if (n > 0) {
                const std::size_t at = static_cast<std::size_t>(rng() % n);
                b[at] = static_cast<char>(b[at] ^ static_cast<char>(1 + rng() % 255));
            }
            break;
        case 2:
            b = rng() % 2 == 0 ? a.substr(0, static_cast<std::size_t>(rng() % (n + 1)))
                               : a + random_text(rng, 1 + rng() % 8);
            break;
        default:
            b = random_text(rng, n);  // same length, arbitrary bytes
            break;
        }
        hold(a, b);
    }
}

}  // namespace

int main() {
    every_position_of_every_length();
    randomized_pairs();
    std::printf("  %llu comparisons, %llu mismatches\n",
                static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(mismatched));
    const bool ok = mismatched == 0 && compared > 4000000;
    std::printf("test_native_projection_compare: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
