#include <pineforge/run_failure.hpp>
#include <pineforge/session_time.hpp>

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <system_error>

namespace {

// No expected timestamp means that midnight itself is outside int64_t.
int check(const char* zone, std::int64_t bar, std::optional<std::int64_t> expected) {
    std::cout << zone << ' ' << bar << " expects ";
    if (expected) std::cout << *expected;
    else std::cout << "coded EOVERFLOW";

    bool ok = false;
    try {
        const auto open = pineforge::calendar_day_open_local_ms(bar, zone);
        ok = expected && open == *expected;
        if (!ok) std::cout << " (returned " << open << ')';
    } catch (const std::system_error& error) {
        const auto* info = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        ok = !expected && info
            && info->run_failure_code() == pineforge::RunFailureCode::engine_unclassified_error
            && error.code() == std::error_code(EOVERFLOW, std::generic_category());
        if (!ok) std::cout << " (" << error.what() << ')';
    }
    std::cout << (ok ? " PASS" : " FAIL") << std::endl;
    return ok ? 0 : 1;
}

}  // namespace

int main() {
    constexpr std::int64_t day = 86'400'000;
    constexpr std::int64_t hour = 3'600'000;
    int failures = 0;
    // Both clocks read GMT on Dec 30, Dec 31 and Jan 1 at the epoch.
    const char* const zones[] = {"UTC", "Atlantic/Canary"};
    for (const char* zone : zones) {
        failures += check(zone, -1, -day);
        failures += check(zone, -day - 1, -2 * day);
        failures += check(zone, -1000, -day);
        failures += check(zone, -day - 1000, -2 * day);
        failures += check(zone, -day, -day);
        failures += check(zone, -2 * day, -2 * day);
        failures += check(zone, 0, 0);
        failures += check(zone, 1, 0);
        failures += check(zone, day - 1, 0);
        failures += check(zone, day, day);
    }
    // July 1, 2024 is day 54*365 + 13 + 182. Canary summer midnight is 23:00Z.
    failures += check("Atlantic/Canary", 19'905 * day, 19'905 * day - hour);

    // INT64_MAX = 106751991167 whole days + 25975807 ms. In UTC+1
    // both extreme representable midnights are one hour earlier than in UTC.
    constexpr std::int64_t last = 106'751'991'167LL * day;
    const struct { const char* zone; std::int64_t offset; } clocks[] = {
        {"UTC", 0}, {"UTC+1", hour}
    };
    for (const auto& clock : clocks) {
        const auto first = -last - clock.offset;
        failures += check(clock.zone, first - 1, std::nullopt);
        failures += check(clock.zone, first, first);
        failures += check(clock.zone, std::numeric_limits<std::int64_t>::max(), last - clock.offset);
        failures += check(clock.zone, std::numeric_limits<std::int64_t>::min(), std::nullopt);
    }
    return failures != 0;
}
