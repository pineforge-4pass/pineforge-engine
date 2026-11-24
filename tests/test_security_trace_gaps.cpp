#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>

int main() {
    // Enable the switch before its first use in this process. Capture the
    // actual records, with an inclusive one-hour gap as a positive control.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> log(std::tmpfile(), &std::fclose);
    if (!log || ::setenv("PF_SEC_TRACE", "1", 1) != 0 || std::fflush(stderr) != 0
        || ::dup2(fileno(log.get()), fileno(stderr)) < 0) {
        std::puts("FAIL cannot capture the enabled trace");
        return 1;
    }
    constexpr auto first = std::numeric_limits<std::int64_t>::min();
    constexpr auto last = std::numeric_limits<std::int64_t>::max();
    const struct { std::int64_t prev, curr; bool prints; } gaps[] = {
        {-1, 3'599'999, true}, {-1, 3'600'000, false}, {1, 1, false},
        {10'000'000, 1, true}, {first, last, false}, {1, first, true}, {last, first, true},
    };
    int failures = 0;
    for (const auto& g : gaps) {
        // The stream and its stderr duplicate share a file offset. Read only
        // this call's bytes, then leave both descriptors at end for the next.
        const long begin = std::ftell(log.get());
        (void)pineforge::crosses_boundary(g.prev, g.curr, pineforge::CalendarPeriod::NONE);
        if (begin < 0 || std::fflush(stderr) != 0 || std::fseek(log.get(), begin, SEEK_SET) != 0)
            return 1;
        std::string actual;
        char buffer[256];
        while (const auto count = std::fread(buffer, 1, sizeof(buffer), log.get()))
            actual.append(buffer, count);
        if (std::ferror(log.get()) || std::fseek(log.get(), 0, SEEK_END) != 0) return 1;
        const auto prev = std::to_string(g.prev), curr = std::to_string(g.curr);
        const std::string expected = g.prints
            ? "[xsb] prev=" + prev + " clock=" + prev + " | curr=" + curr + " clock=" + curr + "\n"
            : "";
        if (actual != expected) {
            std::printf("FAIL %s -> %s: %zu bytes expected %zu\n", prev.c_str(), curr.c_str(),
                        actual.size(), expected.size());
            ++failures;
        } else {
            std::printf("PASS %s -> %s\n", prev.c_str(), curr.c_str());
        }
    }
    return failures != 0;
}
