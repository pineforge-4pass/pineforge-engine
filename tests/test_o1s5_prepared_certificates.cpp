// C02 fail-before prediction (not executed under LB-446): d99a6308's frozen
// header has no cycle_certificate_into declaration. With the LAB-O1C inline
// compatibility body supplied instead, the first cold UTC certificate would
// attempt an allocation despite all four output buffers being prepared.
#include "../src/native_calendar_memo.hpp"

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

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

void certificates(const std::string& session, const char* zone, std::int64_t first,
                  int count, bool certified) {
    const auto calendar = nc::parse_session(session, zone);
    CHECK(calendar.has_value());
    if (!calendar) return;
    global_allocation::recording = true;
    {
        nc::SessionDayMemo memo;
        std::array<nc::CycleCertificate, 4> outputs;
        std::array<const void*, 4> addresses{};
        std::array<std::size_t, 4> capacities{};
        const auto bound = nc::session_day_span_bound(*calendar);
        (void)memo.state(*calendar);
        CHECK(memo.day_resolutions() == 0);
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            outputs[i].spans.reserve(bound);
            addresses[i] = outputs[i].spans.data();
            capacities[i] = outputs[i].spans.capacity();
            CHECK(capacities[i] >= bound);
        }
        const auto bytes = global_allocation::live_bytes;
        for (int day = 0; day < count; ++day) {
            const auto now = first + day * kDay;
            auto& out = outputs[static_cast<std::size_t>(day) % outputs.size()];
            out.origin_ms = 11;
            out.next_origin_ms = 22;
            out.ordinal = 33;
            // Seed an old answer so refusal must clear it without giving
            // back the prepared storage.
            if (out.spans.empty()) out.spans.emplace_back(-2, -1);
            bool present = false;
            without_allocation([&] {
                present = nc::cycle_certificate_into(*calendar, now, memo, out);
            });
            CHECK(memo.bound_to(*calendar));
            CHECK(nc::session_day_span_capacity(*calendar) == bound);
            CHECK(present == certified);
            if (present) {
                // No reference query or certificate warmup precedes dispatch.
                const auto expected = nc::session_day_at(*calendar, now);
                CHECK(expected.has_value());
                if (expected) {
                    CHECK(out.origin_ms == expected->origin_ms);
                    CHECK(out.next_origin_ms == expected->next_origin_ms);
                    CHECK(out.ordinal == expected->ordinal);
                    CHECK(out.spans == expected->spans);
                    CHECK(out.spans.size() <= bound);
                }
                // Also exercise a prepared hit: an owning compatibility
                // implementation would allocate again here.
                without_allocation([&] {
                    CHECK(nc::cycle_certificate_into(*calendar, now, memo, out));
                });
                const auto owning = nc::cycle_certificate(*calendar, now, memo);
                CHECK(owning.has_value());
                if (owning) {
                    CHECK(owning->origin_ms == out.origin_ms);
                    CHECK(owning->next_origin_ms == out.next_origin_ms);
                    CHECK(owning->ordinal == out.ordinal);
                    CHECK(owning->spans == out.spans);
                }
            } else {
                CHECK(out.spans.empty());
                CHECK(out.origin_ms == 11 && out.next_origin_ms == 22 && out.ordinal == 33);
                CHECK(memo.day_resolutions() == 0);
                // Refusal leaves the exact lookup available.
                CHECK(nc::session_day_at(*calendar, now).has_value());
            }
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                CHECK(outputs[i].spans.data() == addresses[i]);
                CHECK(outputs[i].spans.capacity() == capacities[i]);
            }
            CHECK(global_allocation::live_bytes == bytes);
        }
        if (certified)
            CHECK(memo.day_resolutions() > nc::SessionDayMemo::kDayCapacity);
    }
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;
}

void absent_days() {
    const auto calendar = nc::parse_session("24x7", "UTC");
    CHECK(calendar.has_value());
    if (!calendar) return;
    nc::SessionDayMemo memo;
    nc::CycleCertificate out;
    out.spans.reserve(nc::session_day_span_capacity(*calendar));
    out.spans.emplace_back(1, 2);
    out.ordinal = 42;
    const auto* address = out.spans.data();
    (void)memo.state(*calendar);
    without_allocation([&] {
        CHECK(!nc::cycle_certificate_into(*calendar,
                  std::numeric_limits<std::int64_t>::max(), memo, out));
    });
    CHECK(out.spans.empty());
    CHECK(out.spans.data() == address && out.ordinal == 42);
    nc::SessionCalendar invalid;
    (void)memo.state(invalid);
    without_allocation([&] {
        CHECK(!nc::cycle_certificate_into(invalid, kFirst, memo, out));
    });
    CHECK(out.spans.empty());
    CHECK(out.spans.data() == address && out.ordinal == 42);
}
}  // namespace

int main() {
    for (const int count : {1, 17, 257, 1025}) {
        std::string session = "0900-1100,1300-1500";
        for (int i = 1; i < count; ++i) session += ",0900-1100,1300-1500";
        session += ":1234567";
        certificates(session, "UTC", kFirst, 160, true);
        certificates(session, "UTC0", kFirst, 160, false);
    }
    certificates("24x7", "UTC", kFirst, 160, true);
    certificates("0900-1100,1300-1500", "UTC+00:00", kFirst, 160, true);
    certificates("1800-1700:23456", "UTC", kFirst, 160, true);
    certificates("1300-1600,0900-1500", "UTC", kFirst, 160, true);
    certificates("0130-0145", "Europe/London", kFirst, 300, true);
    certificates("0900-1130,1230-1500", "Asia/Tokyo", kFirst, 160, true);
    // The existing authority refuses the date-line discontinuity before any
    // session day is resolved. Ordinary fallback still answers those dates.
    certificates("24x7", "Pacific/Apia",
                 nc::native_civil_days(2011, 12, 29) * kDay, 3, false);
    absent_days();
    return failures == 0 ? 0 : 1;
}
