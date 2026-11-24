#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    constexpr double m = std::numeric_limits<double>::max();
    pineforge::ta::EMA ema(3, pineforge::ta::EmaSeeding::SimpleAverage);
    const auto check = [](const char* name, double got, double expected) {
        if (got == expected || (std::isnan(got) && std::isnan(expected))) {
            std::printf("PASS %s\n", name);
            return true;
        }
        std::printf("FAIL %s: got %a, expected %a\n", name, got, expected);
        return false;
    };
    bool ok = check("first seed source", ema.compute(m), pineforge::na<double>());
    ok &= check("second seed source", ema.compute(m), pineforge::na<double>());
    // M + M overflows in binary64, but the complete seed sums to M.
    ok &= check("EMA seed M/3", ema.compute(-m), m / 3);
    return ok ? 0 : 1;
}
