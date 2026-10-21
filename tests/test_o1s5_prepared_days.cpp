// C01 fail-before prediction (not executed under LB-446): this TU uses the
// frozen header's existing state()/in_session() API. On d99a6308 the very first
// in_session() after binding tries to allocate a day-resolution vector and
// fails the allocation-refusal check. No day is resolved as setup or warmup.
#include "../src/native_calendar_memo.hpp"

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace {
namespace nc = pineforge::native_calendar;
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #value); } } while (false)

constexpr std::int64_t kDay = 86'400'000;
constexpr std::int64_t kFirst = nc::native_civil_days(2024, 3, 8) * kDay + 10 * 3'600'000;

template <class F>
void without_allocation(F&& work) {
    const auto allocations = global_allocation::allocations;
    const auto refused = global_allocation::refused;
    bool threw = false;
    global_allocation::refusing = true;
    try {
        work();
    } catch (...) {
        threw = true;
    }
    global_allocation::refusing = false;
    CHECK(!threw);
    CHECK(global_allocation::allocations == allocations);
    CHECK(global_allocation::refused == refused);
}

void cold_days(const std::string& session, const char* zone, int count) {
    const auto calendar = nc::parse_session(session, zone);
    CHECK(calendar.has_value());
    if (!calendar) return;
    global_allocation::recording = true;
    {
        nc::SessionDayMemo memo;
        (void)memo.state(*calendar);  // Binding only, before the first query.
        CHECK(memo.day_resolutions() == 0);
        CHECK(memo.retained_days() == 0);
        const auto capacity = memo.retained_span_capacity();
        CHECK(capacity >= nc::SessionDayMemo::kDayCapacity
                              * nc::session_day_span_bound(*calendar));
        const auto bytes = global_allocation::live_bytes;
        for (int day = 0; day < count; ++day) {
            const auto now = kFirst + day * kDay;
            const auto before = memo.day_resolutions();
            bool answer = false;
            without_allocation([&] { answer = nc::in_session(*calendar, now, memo); });
            CHECK(memo.day_resolutions() > before);
            // The reference is deliberately asked AFTER the first cold query.
            CHECK(answer == nc::in_session(*calendar, now));
            const auto resolved = memo.day_resolutions();
            without_allocation([&] {
                CHECK(nc::in_session(*calendar, now, memo) == answer);
            });
            CHECK(memo.day_resolutions() == resolved);
            CHECK(memo.retained_span_capacity() == capacity);
            CHECK(global_allocation::live_bytes == bytes);
        }
        CHECK(memo.day_resolutions() >= static_cast<std::uint64_t>(count));
        CHECK(memo.retained_days() == nc::SessionDayMemo::kDayCapacity);
        // A date evicted many times is cold again, without a new allocation.
        const auto before = memo.day_resolutions();
        without_allocation([&] { (void)nc::in_session(*calendar, kFirst, memo); });
        CHECK(memo.day_resolutions() > before);
        CHECK(memo.retained_span_capacity() == capacity);
        CHECK(global_allocation::live_bytes == bytes);
    }
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;
}

void rebind_and_move() {
    auto calendar = nc::parse_session("0900-1100,1300-1500", "UTC");
    const auto smaller = nc::parse_session("24x7", "UTC");
    CHECK(calendar && smaller);
    if (!calendar || !smaller) return;
    nc::SessionDayMemo memo;
    (void)memo.state(*calendar);
    const auto wide = memo.retained_span_capacity();
    calendar = smaller;
    memo.reset();  // The calendar's address is unchanged.
    CHECK(memo.retained_span_capacity() == 0);
    (void)memo.state(*calendar);
    CHECK(memo.retained_span_capacity() < wide);
    CHECK(memo.day_resolutions() == 0);
    nc::SessionDayMemo moved = std::move(memo);
    without_allocation([&] { CHECK(nc::in_session(*calendar, kFirst, moved)); });
    CHECK(moved.day_resolutions() == 1);
    nc::SessionCalendar invalid;
    (void)moved.state(invalid);
    without_allocation([&] { CHECK(!nc::in_session(invalid, kFirst, moved)); });
    CHECK(moved.retained_days() == 0);
    CHECK(moved.retained_span_capacity() == 0);
}
}  // namespace

int main() {
    for (const int count : {1, 17, 257, 1025}) {
        std::string session = "0900-1100,1300-1500";
        for (int i = 1; i < count; ++i) session += ",0900-1100,1300-1500";
        session += ":1234567";
        cold_days(session, "UTC", 160);
    }
    cold_days("24x7", "UTC", 4096);
    cold_days("0900-1100,1300-1500", "UTC+00:00", 160);
    cold_days("0900-1100,1300-1500", "GMT-0000", 160);
    cold_days("1800-1700:23456", "UTC", 160);
    cold_days("1300-1600,0900-1500", "UTC", 160);
    cold_days("0900-1130,1230-1500", "Asia/Tokyo", 160);
    // Spring gap, autumn fold and fully collapsed gap windows, without
    // pre-resolving any of those dates.
    cold_days("0100-0400:23456", "Europe/London", 300);
    cold_days("0130-0145", "Europe/London", 300);
    rebind_and_move();
    return failures == 0 ? 0 : 1;
}
