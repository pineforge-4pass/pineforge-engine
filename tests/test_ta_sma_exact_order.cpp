// SMA's mathematical contract: correctly round the exact sum of the current
// N valid binary64 sources, then divide that binary64 sum by N. All inputs
// here are synthetic; the reference is derived from those inputs.
//
// expectation corrected: (0.2 + 2^-55) / 3 -> 0.2 / 3, because a prefix
// outside the current valid window must not contribute to its mean.
// expectation corrected: 0.09999999999999998 -> 0.3 / 3, and the subsequent
// 2^-54 / 3 and 2^-55 / 3 -> +0, because evicted inputs no longer contribute.
// expectation corrected: chained pins 79.30411522630041 / 12.637448559633745
// and unconditional rotation ties -> input-derived means at both stages,
// because each stage rounds its own sum and division. These are contract
// corrections, not a reinterpretation of historical observations.

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include "sma_window_reference.hpp"

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

std::size_t checks = 0;
std::size_t failures = 0;

std::uint64_t bits(double value) {
    std::uint64_t result;
    static_assert(sizeof(result) == sizeof(value), "binary64 storage");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void check_exact(double actual, double expected, const std::string& tag) {
    ++checks;
    if ((is_na(actual) && is_na(expected)) ||
        (!is_na(actual) && !is_na(expected) && bits(actual) == bits(expected))) {
        return;
    }
    ++failures;
    std::cerr << "FAIL " << tag << ": got " << std::hexfloat << actual
              << " want " << expected << std::defaultfloat << '\n';
}

void check_true(bool value, const std::string& tag) {
    ++checks;
    if (value) return;
    ++failures;
    std::cerr << "FAIL " << tag << '\n';
}

// Membership alone is modeled here: oldest first, with NA consuming no slot.
// Arithmetic is always a new floating expansion over the retained sources.
double append(std::vector<double>& valid, std::size_t length, double value) {
    if (!is_na(value)) {
        if (valid.size() == length) valid.erase(valid.begin());
        valid.push_back(value);
    }
    return valid.size() == length ? sma_window_reference::mean(valid)
                                 : na<double>();
}

void reference_controls() {
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double up = std::nextafter(1.0, 2.0);
    check_exact(sma_window_reference::rounded_sum({0x1p53, 1.0, -0x1p53}),
                1.0, "reference: exact cancellation retains the unit");
    check_exact(sma_window_reference::rounded_sum({1.0, 0x1p-53}),
                1.0, "reference: halfway rounds to even below");
    check_exact(sma_window_reference::rounded_sum({up, 0x1p-53}),
                std::nextafter(up, 2.0), "reference: halfway rounds to even above");
    check_exact(sma_window_reference::rounded_sum({1.0, 0x1p-53, tiny}),
                up, "reference: positive tail beyond halfway");
    check_exact(sma_window_reference::rounded_sum({-1.0, -0x1p-53, -tiny}),
                -up, "reference: negative tail beyond halfway");
    check_exact(sma_window_reference::rounded_sum({tiny, tiny, -tiny}),
                tiny, "reference: gradual underflow");

    const std::vector<double> division_window{0.0, 0.0, 5.0};
    const double sum = sma_window_reference::rounded_sum(division_window);
    volatile double divisor = static_cast<double>(division_window.size());
    volatile double reciprocal = 1.0 / divisor;
    check_true(bits(sum / divisor) != bits(sum * reciprocal),
               "division control distinguishes reciprocal multiplication");
}

void exercise_path(const std::vector<double>& window,
                   const std::vector<double>& prefix, bool replace,
                   const std::string& tag) {
    const std::size_t length = window.size();
    ta::SMA native(static_cast<int>(length));
    std::vector<double> valid;
    std::size_t bar = 0;
    const auto compute = [&](double value) {
        const double expected = append(valid, length, value);
        check_exact(native.compute(value), expected,
                    tag + " compute " + std::to_string(bar++));
    };
    compute(na<double>());
    for (double value : prefix) compute(value);
    for (std::size_t i = 0; i + 1 < length; ++i) compute(window[i]);

    if (replace) {
        const auto before_bar = valid;
        const double provisional = window.back() == 0.0 ? 0.5 : 0.0;
        compute(provisional);
        for (double replacement : {window.back(), -0.25, window.back()}) {
            valid = before_bar;
            const double expected = append(valid, length, replacement);
            check_exact(native.recompute(replacement), expected, tag + " recompute");
        }
        // Existing recompute(NA) behavior: returns NA, leaves membership alone.
        check_exact(native.recompute(na<double>()), na<double>(), tag + " recompute NA");
        check_exact(native.recompute(window.back()), sma_window_reference::mean(window),
                    tag + " restore replacement after NA");
    } else {
        compute(window.back());
    }

    check_true(valid == window, tag + " final valid window membership");
    compute(na<double>());
    compute(na<double>());
    for (double value : {0.0, 0.25, -0.5, 8.0, 1.0, 0.0}) compute(value);
}

void exercise_window(const std::vector<double>& window, const std::string& tag) {
    // Independently constructed instances, rather than a copied accumulator.
    exercise_path(window, {}, false, tag + " fresh");
    exercise_path(window, {0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0},
                  false, tag + " after small prefix");
    exercise_path(window, {0x1p52, 1.0, -0x1p52, 0x1p-40, -0x1p-40},
                  true, tag + " after cancelling prefix");
}

void mathematical_windows() {
    const double tiny = std::numeric_limits<double>::denorm_min();
    const std::vector<std::vector<double>> windows{
        {0.0}, {tiny}, {-tiny}, {std::numeric_limits<double>::min()},
        {1.0, 0x1p-53}, {std::nextafter(1.0, 2.0), 0x1p-53},
        {1.0, 0x1p-53, tiny}, {-1.0, -0x1p-53, -tiny},
        {0x1p53, 1.0, -0x1p53}, {1e100, 1.0, -1e100},
        {0.0, 0.0, 5.0}, {0.0, 0.0, -5.0},
        {0.2, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.7, 0.3, 0.7},
        {0.1, 0.3, 0.0}, {tiny, tiny, -tiny},
        {0x1p54, 1.0, 1.0, -0x1p54, 0x1p-30}
    };
    for (std::size_t i = 0; i < windows.size(); ++i) {
        exercise_window(windows[i], "mathematical window " + std::to_string(i));
    }

    // Bounded generated windows with exponent spread, cancellation, both
    // signs, zeros, odd/even lengths and neighboring significands.
    for (int length : {2, 3, 5, 7, 8, 20, 31, 64}) {
        for (int exponent : {-900, -40, 0, 40, 900}) {
            std::vector<double> window;
            for (int i = 0; i < length; ++i) {
                const double unit = std::ldexp(1.0, exponent);
                switch (i % 6) {
                case 0: window.push_back(unit); break;
                case 1: window.push_back(std::ldexp(1.0, exponent - 53)); break;
                case 2: window.push_back(-unit); break;
                case 3: window.push_back(std::nextafter(unit, 2.0 * unit)); break;
                case 4: window.push_back(-unit / 8.0); break;
                default: window.push_back(0.0); break;
                }
            }
            exercise_window(window, "generated N=" + std::to_string(length) +
                                    " exponent=" + std::to_string(exponent));
        }
    }
}

std::uint64_t next_seed(std::uint64_t& state) {
    // Fully specified unsigned arithmetic; no implementation-specific RNG.
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void seeded_windows() {
    std::uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (int sample = 0; sample < 16; ++sample) {
        const std::size_t length = 1 + next_seed(state) % 47;
        std::vector<double> window;
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint64_t random = next_seed(state);
            const int exponent = static_cast<int>((random >> 48) % 1801) - 900;
            const double significand = 1.0 +
                static_cast<double>(random & UINT64_C(0x000fffffffffffff)) * 0x1p-52;
            const double value = std::ldexp(significand, exponent);
            window.push_back((random & (UINT64_C(1) << 63)) ? -value : value);
        }
        exercise_window(window, "seeded window " + std::to_string(sample));
    }
}

void pulse_and_identical_history() {
    ta::SMA first(3), second(3);
    std::vector<double> valid;
    const std::vector<double> series{
        0.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.2, 0.3, 0.0,
        0.0, 0.0, 0.7, 0.1, 0.3, 0.7, 0.0, 0.0
    };
    for (double value : series) {
        const double expected = append(valid, 3, value);
        check_exact(first.compute(value), expected, "identical history first");
        check_exact(second.compute(value), expected, "identical history second");
    }
    ta::SMA pulse(3);
    valid.clear();
    for (double value : {0.1, 0.3}) {
        check_exact(pulse.compute(value), append(valid, 3, value), "pulse warmup");
    }
    for (int i = 0; i < 38; ++i) {
        check_exact(pulse.compute(0.0), append(valid, 3, 0.0),
                    "pulse followed by zeros " + std::to_string(i));
    }
}

void warmup_replacement() {
    ta::SMA native(3), direct(3);
    check_exact(native.recompute(7.0), na<double>(), "empty recompute warms up");
    check_exact(native.recompute(10.0), na<double>(), "warmup replacement keeps count");
    check_exact(direct.compute(10.0), na<double>(), "independent warmup first");
    std::vector<double> valid{10.0};
    for (double value : {na<double>(), 8.0, na<double>(), 9.0, 11.0}) {
        const double expected = append(valid, 3, value);
        check_exact(native.compute(value), expected, "warmup replacement continuation");
        check_exact(direct.compute(value), expected, "independent warmup continuation");
    }
}

void chained_controls() {
    for (double plateau : {0.0, 100.0}) {
        ta::SMA first(3), second(3);
        std::vector<double> first_window, second_window;
        std::vector<double> series(300, plateau);
        series.insert(series.end(), {plateau, plateau, 37.912345678901234,
                                     plateau, plateau});
        for (double value : series) {
            const double expected_first = append(first_window, 3, value);
            const double expected_second = append(second_window, 3, expected_first);
            const double actual_first = first.compute(value);
            const double actual_second = second.compute(actual_first);
            check_exact(actual_first, expected_first, "chained first mean");
            check_exact(actual_second, expected_second, "chained second mean");
        }
    }
}

void saturated_plateau_control() {
    ta::SMA k_sma(3), d_sma(3);
    double k = na<double>(), d = na<double>();
    int crossings = 0;
    for (int i = 0; i < 1200; ++i) {
        double s = ((i / 37) % 2 == 0) ? 100.0 : 0.0;
        double pk = k, pd = d;
        k = k_sma.compute(s);
        d = d_sma.compute(k);
        if (is_na(k) || is_na(d) || is_na(pk) || is_na(pd)) continue;
        bool settled = (k == d) && (pk == pd);
        if (settled) continue;
        if ((k > d && pk <= pd) || (d > k && pd <= pk)) crossings++;
    }
    check_true(crossings == 32,
               "alternating 37-bar plateaus over 1200 bars: 32 transition crossings");
}

void plain_sma_values_unchanged() {
    ta::SMA s(3);
    check_exact(s.compute(1.0), na<double>(), "SMA(3) bar0 NA");
    check_exact(s.compute(2.0), na<double>(), "SMA(3) bar1 NA");
    check_exact(s.compute(3.0), 2.0, "SMA(3) seed = (1+2+3)/3");
    check_exact(s.compute(4.0), 3.0, "SMA(3) = (2+3+4)/3");
    check_exact(s.compute(5.0), 4.0, "SMA(3) = (3+4+5)/3");

    ta::SMA big(50);
    double last = na<double>();
    for (int i = 1; i <= 50; ++i) last = big.compute(static_cast<double>(i));
    check_exact(last, 25.5, "SMA(50) seed = mean(1..50)");
    for (int i = 51; i <= 200; ++i) last = big.compute(static_cast<double>(i));
    check_exact(last, 175.5, "SMA(50) of 151..200 = 175.5");
}

} // namespace

int main() {
    if (std::fegetround() != FE_TONEAREST) {
        std::cerr << "SETUP: the reference requires round-to-nearest/even\n";
        return 2;
    }
    try {
        reference_controls();
        if (failures != 0) {
            std::cerr << "SETUP: reference controls failed\n";
            return 2;
        }
        plain_sma_values_unchanged();
        pulse_and_identical_history();
        warmup_replacement();
        mathematical_windows();
        seeded_windows();
        chained_controls();
        saturated_plateau_control();
    } catch (const std::exception& error) {
        std::cerr << "SETUP: " << error.what() << '\n';
        return 2;
    }
    std::cout << "test_ta_sma_exact_order: checked=" << checks
              << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
