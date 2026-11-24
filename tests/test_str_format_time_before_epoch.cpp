#include <pineforge/str_utils.hpp>

#include <cstdio>
#include <string>

int main() {
    constexpr auto civil = "yyyy-MM-dd HH:mm:ss";
    constexpr long long day = 86'400'000;
    constexpr long long hour = 3'600'000;
    const struct {
        const char* name;
        long long ms;
        const char* pattern;
        const char* zone;
        const char* expected;
    } cases[] = {
        {"last millisecond UTC", -1, civil, "UTC", "1969-12-31 23:59:59"},
        {"previous second UTC", -1500, civil, "UTC", "1969-12-31 23:59:58"},
        {"last millisecond New York", -1, civil, "America/New_York", "1969-12-31 18:59:59"},
        {"previous second New York", -1500, civil, "America/New_York", "1969-12-31 18:59:58"},
        {"whole negative UTC", -1000, civil, "UTC", "1969-12-31 23:59:59"},
        {"whole negative New York", -1000, civil, "America/New_York", "1969-12-31 18:59:59"},
        {"epoch", 0, civil, "UTC", "1970-01-01 00:00:00"},
        {"whole positive UTC", 1000, civil, "UTC", "1970-01-01 00:00:01"},
        {"whole positive New York", 1000, civil, "America/New_York", "1969-12-31 19:00:01"},
        {"positive UTC", 1500, civil, "UTC", "1970-01-01 00:00:01"},
        {"positive New York", 1500, civil, "America/New_York", "1969-12-31 19:00:01"},
        // SSS is currently literal; the formatter has only whole-second fields.
        {"day of year and Unix second", -1, "ss.SSS EEE ww %j %s Z", "UTC",
         "59.SSS EEE ww 365 -1 +0000"},
        // Sunday December 28 precedes the Monday starting ISO week 1 of 1970.
        {"week boundary", -3 * day - 1, "%u %W %G-W%V", "UTC", "7 51 1969-W52"},
        // October 26, 1969: New York went from EDT to EST at 06:00 UTC.
        {"offset transition", -67 * day + 6 * hour - 1, "HH:mm:ss Z %Z", "America/New_York",
         "01:59:59 -0400 EDT"},
    };
    int failures = 0;
    for (const auto& c : cases) {
        const auto got = pineforge::pine_str_format_time(c.ms, c.pattern, c.zone);
        if (got != c.expected) {
            std::printf("FAIL %s: [%s] expected [%s]\n", c.name, got.c_str(), c.expected);
            ++failures;
        } else {
            std::printf("PASS %s\n", c.name);
        }
    }
    return failures != 0;
}
