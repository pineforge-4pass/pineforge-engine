#include <pineforge/math.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>

namespace {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
int failures = 0;

std::uint64_t bits(double x) {
    std::uint64_t result;
    static_assert(sizeof result == sizeof x);
    std::memcpy(&result, &x, sizeof result);
    return result;
}

void check(const char* series, int length, int bar, const char* output,
           double got, double want) {
    if (bits(got) == bits(want)) return;
    if (++failures <= 12)
        std::fprintf(stderr, "%s n=%d bar=%d %s: got %a, want %a\n",
                     series, length, bar, output, got, want);
}

// Chronological FIFO. A tick is evaluated on a copy of the previous bar;
// the reference has no ring indices, undo operation or engine dependencies.
struct Reference {
    std::deque<double> sources;
    int length;
    double total = 0.0, correction = 0.0;

    explicit Reference(int n) : length(n) {}

    void add(double v) {
        const double next = total + v;
        const double bb = next - total;
        const double error = (total - (next - bb)) + (v - bb);
        correction += error;
        total = next;
    }

    double bar(double x) {
        if (!std::isnan(x)) {
            if (static_cast<int>(sources.size()) == length) {
                add(-sources.front());
                sources.pop_front();
            }
            add(x);
            sources.push_back(x);
        }
        return static_cast<int>(sources.size()) == length ? total + correction : missing;
    }
};

struct Outputs {
    pineforge::math::Sum sum;
    pineforge::ta::SMA sma;
    int length;
    const char* series;

    Outputs(int n, const char* name) : sum(n), sma(n), length(n), series(name) {}

    void check_bar(int bar, double x, double want, bool replace = false) {
        check(series, length, bar, replace ? "sum.recompute" : "sum.compute",
              replace ? sum.recompute(x) : sum.compute(x), want);
        check(series, length, bar, replace ? "sma.recompute" : "sma.compute",
              replace ? sma.recompute(x) : sma.compute(x),
              std::isnan(want) ? missing : want / length);
    }
};

void first_eviction() {
    constexpr double sources[] = {1.0, 1.0, 0x1p-26 * 0x1p-27, 0x1p-55 * 0x1p-55};
    Reference ref(3);
    Outputs actual(3, "midpoint");
    for (int bar = 0; bar != 4; ++bar) {
        const double want = ref.bar(sources[bar]);
        if (bar == 3) {
            // The exact window is 1 + 2^-53 + 2^-110, strictly above the
            // midpoint. Its correctly rounded sum (math.fsum) is 1's successor.
            // In the running correction 2^-53 + 2^-110 rounds to 2^-53;
            // the emitted addition lands ON the midpoint and ties to even.
            const double exact = std::nextafter(1.0, 2.0);
            check("midpoint", 3, bar, "reference", want, 1.0);
            if (bits(want) == bits(exact)) {
                std::fprintf(stderr, "midpoint witness did not distinguish the exact sum\n");
                ++failures;
            }
        }
        actual.check_bar(bar, sources[bar], want);
    }
}

double pulse(int bar) {
    const int block = bar / 43;
    switch (bar % 43) {
        case 0: return 0.17 * (1 + block % 13);
        case 1: return -0.31 * (1 + block % 7);
        case 9: return 0x1p-100 * (1 + block % 5);
        default: return 0.0;
    }
}

double scales(int bar) {
    constexpr double factors[] = {0x1p100, 1.0, 0x1p-100, -0x1p100, -1.0, 0.0, -0.13};
    return factors[bar % 7] * (1 + (bar / 7) % 11);
}

double plateau(int bar) {
    constexpr double levels[] = {0.19, -0.19, 0.0, 0x1p-100};
    return levels[(bar / 239) % 4] * 7.0;
}

void history(const char* name, double (*source)(int), int n) {
    Reference ref(n);
    Outputs actual(n, name);
    for (int bar = 0; bar < 1200; ++bar) {
        const double x = source(bar);
        if (bar == n - 1 || bar % 41 == 7) {
            // NA -> finite -> NA -> final source, including the seed bar
            // and evictions. Only the final tick may enter the next bar.
            auto tick = ref;
            actual.check_bar(bar, missing, tick.bar(missing));
            tick = ref;
            const double provisional = -0x1p58 * (1 + bar % 5);
            actual.check_bar(bar, provisional, tick.bar(provisional), true);
            tick = ref;
            actual.check_bar(bar, missing, tick.bar(missing), true);
            actual.check_bar(bar, x, ref.bar(x), true);
        } else {
            actual.check_bar(bar, x, ref.bar(x));
        }
    }
}

// Infinity enters the window; NA does not. A retry of an exceptional bar
// must restore its finite predecessor, even if the provisional sum overflowed.
void exceptional() {
    const double inf = std::numeric_limits<double>::infinity();
    const double max = std::numeric_limits<double>::max();
    Outputs a(2, "infinities");
    a.check_bar(0, inf, missing);
    a.check_bar(1, 4.0, inf);
    // IEEE addition does not prescribe the payload/sign of infinity - infinity.
    const double sum = a.sum.recompute(-inf);
    const double mean = a.sma.recompute(-inf);
    if (!std::isnan(sum) || !std::isnan(mean)) {
        std::fprintf(stderr, "opposite infinities did not give NaN\n");
        ++failures;
    }
    a.check_bar(1, 4.0, inf, true);
    a.check_bar(2, 2.0, 6.0);
    a.check_bar(2, missing, inf, true);
    a.check_bar(2, -1.0, 3.0, true);

    Outputs negative(2, "negative infinity");
    negative.check_bar(0, -inf, missing);
    negative.check_bar(1, 8.0, -inf);
    negative.check_bar(2, missing, -inf);
    negative.check_bar(3, 4.0, 12.0);

    Outputs b(2, "overflow");
    b.check_bar(0, max, missing);
    b.check_bar(1, max, inf);
    b.check_bar(1, -max, 0.0, true);
    b.check_bar(1, max, inf, true);
    b.check_bar(2, -max, 0.0);
    b.check_bar(3, max, 0.0);
}

} // namespace

int main() {
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53);
    first_eviction();
    for (int length : {1, 2, 3, 5, 7, 14, 20, 200}) {
        history("pulses", pulse, length);
        history("scales", scales, length);
        history("plateaus", plateau, length);
    }
    exceptional();
    return failures == 0 ? 0 : 1;
}
