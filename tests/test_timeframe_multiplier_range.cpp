#include <pineforge/run_failure.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

int main() {
    static_assert(std::numeric_limits<int>::max() == 2147483647, "32-bit timeframe multiplier");
    int failures = 0;
    for (const char* text : {"2147483648D", "3000000000W", "999999999999999999999999M"}) {
        try {
            const int got = pineforge::tf_multiplier(text);
            std::printf("FAIL %s: returned %d instead of a range error\n", text, got);
        } catch (const std::out_of_range& e) {
            const auto failure = pineforge::classify_run_failure(e);
            if (failure.code == pineforge::RunFailureCode::pine_invalid_argument && failure.args
                && *failure.args == "{\"argument\":\"timeframe\",\"rule\":\"unparseable\"}") {
                std::printf("PASS %s\n", text);
                continue;
            }
            std::printf("FAIL %s: wrong range error\n", text);
        }
        ++failures;
    }
    const struct { const char* text; int expected; } controls[] = {
        {"0002147483647M", 2147483647}, {"15", 15}, {"12x99999999999", 12},
        {"", 1}, {"D", 1}, {"0S", 1}, {"-4", 1}, {" 15", 1},
    };
    for (const auto& c : controls) {
        const int got = pineforge::tf_multiplier(c.text);
        if (got == c.expected) {
            std::printf("PASS [%s]\n", c.text);
        } else {
            std::printf("FAIL [%s]: %d expected %d\n", c.text, got, c.expected);
            ++failures;
        }
    }
    return failures != 0;
}
