#include <pineforge/str_utils.hpp>

#include <clocale>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
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
    // Expectations characterize the previous formatter in the C time locale.
    {"literal_ZZ", 1706746029000LL, "ZZ", "UTC", "ZZ", 2},
    {"literal_ZZZ", 1706746029000LL, "ZZZ", "UTC", "ZZZ", 3},
    {"literal_z", 1706746029000LL, "z", "UTC", "z", 1},
    {"literal_zzz", 1706746029000LL, "zzz", "UTC", "zzz", 3},
    {"literal_yy", 1706746029000LL, "yy", "UTC", "yy", 2},
    {"literal_M", 1706746029000LL, "M", "UTC", "M", 1},
    {"literal_d", 1706746029000LL, "d", "UTC", "d", 1},
    {"literal_H", 1706746029000LL, "H", "UTC", "H", 1},
    {"literal_m", 1706746029000LL, "m", "UTC", "m", 1},
    {"literal_s", 1706746029000LL, "s", "UTC", "s", 1},
    {"repeated_year", 1706746029000LL, "yyyyyyyy", "UTC", "20242024", 8},
    {"repeated_month", 1706746029000LL, "MMMM", "UTC", "0202", 4},
    {"repeated_minute", 1706746029000LL, "mmmm", "UTC", "0707", 4},
    {"ordered_mapping_mMMm", 1706746029000LL, "mMMm", "UTC", "m%M", 3},
    {"ordered_mapping_MMmm", 1706746029000LL, "MMmm", "UTC", "%Mm", 3},

    {"raw_percent_Y", 1706746029000LL, "%Y", "UTC", "2024", 4},
    {"raw_percent_m", 1706746029000LL, "%m", "UTC", "02", 2},
#if defined(__GLIBC__)
    // glibc's UTC %z spelling has not been established on macOS here.
    {"raw_percent_z", 1706746029000LL, "%z", "UTC", "+0000", 5},
    // glibc's GMT conversion normalizes TZ to UTC for %Z and mktime-based %s.
    // These spellings and the C-locale %c layout are unproved on macOS here.
    {"raw_percent_Z", 1706746029000LL, "%Z", "GMT", "UTC", 3},
    {"raw_percent_c", 1706746029000LL, "%c", "UTC", "Thu Feb  1 00:07:09 2024", 24},
    {"raw_percent_s", 1706746029000LL, "%s", "GMT", "1706746029", 10},
#endif
    {"raw_percent_escape", 1706746029000LL, "%%", "UTC", "%", 1},
#if defined(__GLIBC__)
    // glibc padding and C-locale E/O fallbacks are unproved on macOS here.
    {"raw_flag_width", 1706746029000LL, "%_6Y", "UTC", "  2024", 6},
    {"raw_zero_width", 1706746029000LL, "%010Y", "UTC", "0000002024", 10},
    {"raw_E_modifier", 1706746029000LL, "%EY", "UTC", "2024", 4},
    {"raw_O_modifier", 1706746029000LL, "%Od", "UTC", "01", 2},
#endif
    {"raw_percent_yyyy", 1706746029000LL, "%yyyy", "UTC", "%Y", 2},
    {"raw_percent_MM", 1706746029000LL, "%MM", "UTC", "%m", 2},
    {"raw_percent_mm", 1706746029000LL, "%mm", "UTC", "%M", 2},

    {"unmeasured_unmatched_quote", 1706746029000LL, "'yyyy Z", "UTC", "'2024 Z", 7},
    {"unmeasured_unmatched_tail", 1706746029000LL, "yyyy 'T' HH 'tail", "UTC",
     "2024 'T' 00 'tail", 17},
    {"empty_format", 1706746029000LL, "", "UTC", "", 0},

    // Measured TradingView strings, including their lengths.
    {"tv_measured_quoted_percent", 1706746029000LL, "'%Y'", "UTC", "%Y", 2},
    {"tv_measured_quoted_percent_with_offset", 1706746029000LL, "yyyy '%Y' Z", "UTC",
     "2024 %Y +0000", 13},

    // Engine-generic consequences of literal quoting, not TradingView measurements.
    {"engine_generic_quoted_percent_month", 1706746029000LL, "'%m'", "UTC", "%m", 2},
    {"engine_generic_quoted_percent_pair", 1706746029000LL, "'%%'", "UTC", "%%", 2},
    {"engine_generic_quoted_percent_lone", 1706746029000LL, "'%'", "UTC", "%", 1},
    {"engine_generic_quoted_percent_mixed", 1706746029000LL, "HH '%H' mm", "UTC",
     "00 %H 07", 8},

    // 32 escaped years plus 26 ordinary years: 264 input bytes, 148 mapped
    // bytes, 168 output bytes. The output fits the previous 256-byte buffer;
    // the marker plus output requires growth from the new 149-byte buffer.
    {"long_pattern_growth", 1706746029000LL,
     "%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy"
     "%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy"
     "%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy"
     "%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy%yyyy"
     "yyyyyyyy" "yyyyyyyy" "yyyyyyyy" "yyyyyyyy"
     "yyyyyyyy" "yyyyyyyy" "yyyyyyyy" "yyyyyyyy"
     "yyyyyyyy" "yyyyyyyy" "yyyyyyyy" "yyyyyyyy"
     "yyyyyyyy", "UTC",
     "%Y%Y%Y%Y%Y%Y%Y%Y"
     "%Y%Y%Y%Y%Y%Y%Y%Y"
     "%Y%Y%Y%Y%Y%Y%Y%Y"
     "%Y%Y%Y%Y%Y%Y%Y%Y"
     "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024"
     "20242024", 168},

    // Eighty yyyy groups give eighty 2024 groups: 320 output bytes.
    // This is red-first growth: the old failed strftime buffer is undefined.
    {"red_first_growth_past_256", 1706746029000LL,
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
     "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy", "UTC",
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024"
     "20242024" "20242024" "20242024" "20242024" "20242024", 320},

#if defined(__GLIBC__)
    // glibc retains this unknown conversion; its macOS spelling is unproved.
    {"unmeasured_raw_directive_quote_edge", 1706746029000LL, "%'x'Y", "UTC",
     "%'x'Y", 5},
#endif

#if defined(__GLIBC__)
    // Engine arithmetic, not TradingView truth. These preserve the existing
    // raw %z path; the date and clock also expose the offset's full effect.
    // glibc POSIX-TZ parsing and %z spellings are unproved on macOS here.
    {"engine_generic_arithmetic_not_tv_half_hour", 1706746029000LL,
     "yyyy-MM-dd HH:mm:ss %z", "UTC+09:30", "2024-02-01 09:37:09 +0930", 25},
    {"engine_generic_arithmetic_not_tv_negative_fractional_hour", 1706746029000LL,
     "yyyy-MM-dd HH:mm:ss %z", "UTC-03:45", "2024-01-31 20:22:09 -0345", 25},
    {"engine_generic_arithmetic_not_tv_offset_seconds", 1706746029000LL,
     "yyyy-MM-dd HH:mm:ss %z", "XST-0:00:30", "2024-02-01 00:07:39 +0000", 25},
#endif
};

bool check_format_time(const FormatTimeCase& test_case) {
    try {
        const std::string actual = pineforge::str_format_time(
            test_case.timestamp_ms, test_case.pattern, test_case.timezone);
        if (actual != test_case.expected || actual.size() != test_case.expected_length) {
            std::fprintf(stderr,
                         "FAIL: %s pattern=\"%s\" timezone=\"%s\" "
                         "got=\"%s\" (length %zu) expected=\"%s\" (length %zu)\n",
                         test_case.name, test_case.pattern, test_case.timezone,
                         actual.c_str(), actual.size(), test_case.expected,
                         test_case.expected_length);
            return false;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s exception=\"%s\"\n", test_case.name, error.what());
        return false;
    } catch (...) {
        std::fprintf(stderr, "FAIL: %s unknown exception\n", test_case.name);
        return false;
    }
    std::printf("PASS: %s\n", test_case.name);
    return true;
}

} // namespace

int main() {
    if (std::setlocale(LC_TIME, "C") == nullptr) {
        std::fprintf(stderr, "FAIL: C time locale unavailable\n");
        return 1;
    }
    int failed = 0;
    for (const auto& test_case : cases) {
        if (!check_format_time(test_case)) {
            ++failed;
        }
    }
    std::printf("%zu cases, %d failed\n", sizeof(cases) / sizeof(cases[0]), failed);
    return failed == 0 ? 0 : 1;
}
