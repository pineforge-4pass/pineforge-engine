#include <pineforge/str_utils.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

struct FormatTimeCase {
    const char* name;
    std::int64_t timestamp_ms;
    const char* pattern;
    const char* timezone;
    const char* expected;
    std::size_t expected_length;
};

constexpr FormatTimeCase cases[] = {
    // 2024-02-01 00:07:09 UTC and 2024-07-01 00:07:09 UTC.
    {"full_utc_winter", 1706746029000LL, "yyyy-MM-dd'T'HH:mm:ssZ", "UTC",
     "2024-02-01T00:07:09+0000", 24},
    {"full_utc_summer", 1719792429000LL, "yyyy-MM-dd'T'HH:mm:ssZ", "UTC",
     "2024-07-01T00:07:09+0000", 24},
    {"full_gmt_plus_three_winter", 1706746029000LL, "yyyy-MM-dd'T'HH:mm:ssZ", "GMT+3",
     "2024-02-01T03:07:09+0300", 24},
    {"full_gmt_plus_three_summer", 1719792429000LL, "yyyy-MM-dd'T'HH:mm:ssZ", "GMT+3",
     "2024-07-01T03:07:09+0300", 24},
    {"full_utc_minus_five_thirty_winter", 1706746029000LL, "yyyy-MM-dd'T'HH:mm:ssZ",
     "UTC-05:30", "2024-01-31T18:37:09-0530", 24},
    {"full_utc_minus_five_thirty_summer", 1719792429000LL, "yyyy-MM-dd'T'HH:mm:ssZ",
     "UTC-05:30", "2024-06-30T18:37:09-0530", 24},
    {"full_new_york_winter", 1706746029000LL, "yyyy-MM-dd'T'HH:mm:ssZ",
     "America/New_York", "2024-01-31T19:07:09-0500", 24},
    {"full_new_york_summer", 1719792429000LL, "yyyy-MM-dd'T'HH:mm:ssZ",
     "America/New_York", "2024-06-30T20:07:09-0400", 24},

    {"offset_utc_winter", 1706746029000LL, "Z", "UTC", "+0000", 5},
    {"offset_utc_summer", 1719792429000LL, "Z", "UTC", "+0000", 5},
    {"offset_gmt_plus_three_winter", 1706746029000LL, "Z", "GMT+3", "+0300", 5},
    {"offset_gmt_plus_three_summer", 1719792429000LL, "Z", "GMT+3", "+0300", 5},
    {"offset_utc_minus_five_thirty_winter", 1706746029000LL, "Z", "UTC-05:30",
     "-0530", 5},
    {"offset_utc_minus_five_thirty_summer", 1719792429000LL, "Z", "UTC-05:30",
     "-0530", 5},
    {"offset_new_york_winter", 1706746029000LL, "Z", "America/New_York", "-0500", 5},
    {"offset_new_york_summer", 1719792429000LL, "Z", "America/New_York", "-0400", 5},

    {"quoted_date_tokens_utc_winter", 1706746029000LL,
     "yyyy 'yyyy' MM 'MM' dd 'dd' 'T' HH:mm:ss Z", "UTC",
     "2024 yyyy 02 MM 01 dd T 00:07:09 +0000", 38},
    {"doubled_apostrophe_utc_winter", 1706746029000LL, "HH 'o''clock' mm", "UTC",
     "00 o'clock 07", 13},
};

bool check_format_time(const FormatTimeCase& test_case) {
    const std::string actual = pineforge::str_format_time(
        test_case.timestamp_ms, test_case.pattern, test_case.timezone);
    const bool text_matches = actual == test_case.expected;
    const bool length_matches = actual.size() == test_case.expected_length;
    if (!text_matches || !length_matches) {
        std::fprintf(stderr,
                     "FAIL: %s timestamp_ms=%lld pattern=\"%s\" timezone=\"%s\"\n"
                     "  got \"%s\" (length %zu), expected \"%s\" (length %zu)\n",
                     test_case.name, static_cast<long long>(test_case.timestamp_ms),
                     test_case.pattern, test_case.timezone, actual.c_str(), actual.size(),
                     test_case.expected, test_case.expected_length);
        return false;
    }
    std::printf("PASS: %s\n", test_case.name);
    return true;
}

} // namespace

int main() {
    int failed = 0;
    for (const auto& test_case : cases) {
        if (!check_format_time(test_case)) {
            ++failed;
        }
    }
    std::printf("%zu cases, %d failed\n", sizeof(cases) / sizeof(cases[0]), failed);
    return failed == 0 ? 0 : 1;
}
