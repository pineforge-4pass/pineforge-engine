#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <pineforge/str_utils.hpp>

using namespace pineforge;

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (cond) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); } \
} while(0)

#define CHECK_EQ(actual, expected, msg) do { \
    ++tests_run; \
    if ((actual) == (expected)) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s  (got \"%s\", expected \"%s\")\n", \
           msg, std::string(actual).c_str(), std::string(expected).c_str()); } \
} while(0)

void test_format() {
    printf("\n--- str.format ---\n");

    // Basic substitution
    CHECK_EQ(pine_str_format("{0} is {1}", {"Hello", "world"}),
             "Hello is world", "basic placeholder substitution");

    // Multiple args
    CHECK_EQ(pine_str_format("{0}-{1}-{2}", {"a", "b", "c"}),
             "a-b-c", "multiple args");

    // Repeated placeholder
    CHECK_EQ(pine_str_format("{0} and {0}", {"X"}),
             "X and X", "repeated placeholder");

    // No placeholders
    CHECK_EQ(pine_str_format("no placeholders", {"ignored"}),
             "no placeholders", "no placeholders in format");

    // Empty args
    CHECK_EQ(pine_str_format("{0}", {}),
             "{0}", "missing arg left as-is");
}

void test_format_time() {
    printf("\n--- str.format_time ---\n");

    // 2020-01-01 00:00:00 UTC = 1577836800000 ms
    long long ts = 1577836800000LL;
    CHECK_EQ(pine_str_format_time(ts, "yyyy-MM-dd", ""),
             "2020-01-01", "known timestamp to date");

    CHECK_EQ(pine_str_format_time(ts, "yyyy/MM/dd HH:mm:ss", ""),
             "2020/01/01 00:00:00", "full datetime format");

    CHECK_EQ(pine_str_format_time(ts, "yyyy-MM-dd HH:mm", "America/New_York"),
             "2019-12-31 19:00", "format_time honors timezone argument");

    // 2023-06-15 14:30:45 UTC = 1686839445000 ms
    long long ts2 = 1686839445000LL;
    CHECK_EQ(pine_str_format_time(ts2, "HH:mm:ss", ""),
             "14:30:45", "time-only format");
}

void test_match() {
    printf("\n--- str.match ---\n");

    // Digit match
    CHECK_EQ(pine_str_match("abc123def", "[0-9]+"),
             "123", "digit regex match");

    // No match
    CHECK_EQ(pine_str_match("abcdef", "[0-9]+"),
             "", "no match returns empty");

    // Capture group
    CHECK_EQ(pine_str_match("price: 42.5", "price: ([0-9.]+)"),
             "42.5", "capture group returned");

    // Invalid regex returns empty
    CHECK_EQ(pine_str_match("test", "[invalid"),
             "", "invalid regex returns empty");
}

void test_split() {
    printf("\n--- str.split ---\n");

    // Basic comma split
    auto parts = pine_str_split("a,b,c", ",");
    CHECK(parts.size() == 3, "comma split produces 3 parts");
    if (parts.size() == 3) {
        CHECK_EQ(parts[0], "a", "split part 0");
        CHECK_EQ(parts[1], "b", "split part 1");
        CHECK_EQ(parts[2], "c", "split part 2");
    }

    // No match (separator not found) returns [source]
    auto parts2 = pine_str_split("hello", ",");
    CHECK(parts2.size() == 1, "no-match split returns 1 part");
    if (parts2.size() == 1) {
        CHECK_EQ(parts2[0], "hello", "no-match split returns source");
    }

    // Empty separator returns [source]
    auto parts3 = pine_str_split("hello", "");
    CHECK(parts3.size() == 1, "empty separator returns 1 part");
    if (parts3.size() == 1) {
        CHECK_EQ(parts3[0], "hello", "empty separator returns source");
    }
}

void test_tostring() {
    printf("\n--- str.tostring ---\n");

    // NaN
    CHECK_EQ(pine_str_tostring(NAN), "NaN", "NaN value");

    // mintick precision
    CHECK_EQ(pine_str_tostring(1.23456, "mintick", 0.01),
             "1.23", "mintick 0.01 rounds to 2 decimals");

    CHECK_EQ(pine_str_tostring(1.236, "mintick", 0.01),
             "1.24", "mintick rounding up");

    // percent
    CHECK_EQ(pine_str_tostring(0.1234, "percent"),
             "12.34%", "percent mode");

    // volume abbreviations
    CHECK_EQ(pine_str_tostring(1500000.0, "volume"),
             "1.50M", "volume M suffix");

    CHECK_EQ(pine_str_tostring(2500.0, "volume"),
             "2.50K", "volume K suffix");

    CHECK_EQ(pine_str_tostring(3000000000.0, "volume"),
             "3.00B", "volume B suffix");

    CHECK_EQ(pine_str_tostring(500.0, "volume"),
             "500.00", "volume below 1K no suffix");

    // Default mode
    std::string def = pine_str_tostring(42.0);
    CHECK(def.find("42") != std::string::npos, "default mode contains value");
}

int main() {
    test_format();
    test_format_time();
    test_match();
    test_split();
    test_tostring();

    printf("\n=== Results: %d / %d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
