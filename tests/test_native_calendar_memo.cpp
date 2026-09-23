// R5 lane PERF-K1: the session-day memo answers exactly what the memo-free
// calendar answers.
//
// native_calendar_memo.hpp gives interval_containing, session_day_at and
// in_session a caller-owned memo of the calendar's resolved session days. The
// memo-free functions of native_calendar.hpp are the reference, unchanged:
// this row asks both, for the same calendar, timeframes and instant, and
// requires the same answer bit for bit -- presence and every field.
//
// The instants are what a run and a careless caller would ask: walks one
// input width apart across a clock change, a year boundary and a session
// break (the memo's steady state), random instants within three days of them
// (days evicted and re-resolved, colliding slots), random instants over
// 1972-2037, and the int64 extremes. The calendars are the six the lane names
// -- UTC, America/New_York, Asia/Tokyo with a lunch break, Europe/London, a
// POSIX rule zone and a fixed offset -- under all-day, masked daytime, split,
// masked overnight and multi-window sessions, at fixed and calendar
// timeframes, equal and aggregating.
//
// One calendar is asked only where the running C library lets the reference
// answer at all. America/Sitka's October 1867 (local mean time +14:58:47
// until the 18th, -09:01:13 after) is where "the last day resolved holds this
// instant, so it is the answer" -- the shortcut the memo does NOT take --
// answers a different session day than the memo-free lookup under a
// "2330-2300" session: the memo must keep the lookup's own local-date
// candidate order. But glibc's mktime starts from the offset of its previous
// call, and around that date it resolves the same civil time to instants a day
// apart depending on what was asked before, so the memo-free answer itself
// depends on call history there (measured, lane PERF-K1); macOS's mktime does
// not. The section therefore first asks the reference every instant twice
// under two different histories, and compares the memo only where the two
// agree everywhere; otherwise it says it was skipped.
//
// Then the memo's contract: reset() forgets a calendar reassigned in place, a
// memo handed a calendar at another address starts over, and a moved memo
// keeps answering.
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/native_calendar.hpp>

#include "../src/native_calendar_memo.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::native_calendar;

int failures = 0;
long long compared = 0;
const char* scenario = "initialization";

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", scenario, __FILE__,      \
                     __LINE__, #condition);                                    \
    }                                                                          \
} while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::int64_t in(std::int64_t lo, std::int64_t hi) {  // [lo, hi)
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo);
        return lo + static_cast<std::int64_t>(next() % span);
    }
};

bool same(const std::optional<NativeInterval>& a, const std::optional<NativeInterval>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->open_ms == b->open_ms && a->eligible_open_ms == b->eligible_open_ms
        && a->last_traded_close_ms == b->last_traded_close_ms
        && a->next_period_open_ms == b->next_period_open_ms
        && a->next_input_open_ms == b->next_input_open_ms;
}

bool same(const std::optional<NativeSessionDay>& a, const std::optional<NativeSessionDay>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->origin_ms == b->origin_ms && a->next_origin_ms == b->next_origin_ms
        && a->ordinal == b->ordinal && a->spans == b->spans;
}

void show(const char* what, const std::string& label, std::int64_t ms) {
    std::fprintf(stderr, "  %s differs: %s at %lld\n", what, label.c_str(),
                 static_cast<long long>(ms));
}

// Asks the memo-free calendar and the memo the same three questions about
// `ms`. Reports the first few differences of a batch by name.
struct Pair {
    const SessionCalendar* calendar = nullptr;
    Timeframe script;
    Timeframe input;
    std::string label;
    int shown = 0;

    void ask(std::int64_t ms, SessionDayMemo& memo) {
        const auto iv_plain = interval_containing(*calendar, script, input, ms);
        const auto iv_memo = interval_containing(*calendar, script, input, ms, memo);
        const auto day_plain = session_day_at(*calendar, ms);
        const auto day_memo = session_day_at(*calendar, ms, memo);
        // in_session is session_day_at's spans read at one instant; asked on
        // every fourth instant.
        const bool ask_in = compared++ % 4 == 0;
        const bool in_plain = ask_in && in_session(*calendar, ms);
        const bool in_memo = ask_in && in_session(*calendar, ms, memo);
        const bool iv_ok = same(iv_plain, iv_memo);
        const bool day_ok = same(day_plain, day_memo);
        const bool in_ok = in_plain == in_memo;
        if ((!iv_ok || !day_ok || !in_ok) && shown++ < 3) {
            if (!iv_ok) show("interval_containing", label, ms);
            if (!day_ok) show("session_day_at", label, ms);
            if (!in_ok) show("in_session", label, ms);
        }
        CHECK(iv_ok);
        CHECK(day_ok);
        CHECK(in_ok);
    }
};

Timeframe tf(const char* text) {
    auto parsed = parse_timeframe(text);
    CHECK(parsed.has_value());
    return parsed ? *parsed : Timeframe{};
}

SessionCalendar calendar(const char* session, const char* zone) {
    auto parsed = parse_session(session, zone);
    CHECK(parsed.has_value());
    return parsed ? *parsed : SessionCalendar{};
}

// A fixed timeframe's width, or a day for a calendar one: the step a run's
// bars take.
std::int64_t width(const Timeframe& t) {
    switch (t.unit()) {
    case TimeframeUnit::Second: return 1000LL * t.count();
    case TimeframeUnit::Minute: return kMinute * t.count();
    default: return kDay;
    }
}

struct Anchor {
    const char* what;
    std::int64_t ms;
};

// Every probe of one (calendar, script, input) over the anchors: a walk of
// `walk` input widths centered on each anchor at a random phase, then
// `scatter` random instants within three days of it, then `wide` random
// instants over 1972-2037 -- all through one memo, as a run would ask. (1972:
// Europe/London's October 1971 fold changes no daylight-saving flag, the
// history-dependent case the date-line section describes.)
void probe(const SessionCalendar& cal, const char* script, const char* input,
           const std::vector<Anchor>& anchors, int walk, int scatter, int wide,
           std::uint64_t seed) {
    Pair pair;
    pair.calendar = &cal;
    pair.script = tf(script);
    pair.input = tf(input);
    pair.label = cal.timezone() + " '" + cal.literal() + "' " + script + " over " + input;
    Rng rng{seed};
    SessionDayMemo memo;
    const std::int64_t step = width(pair.input);
    for (const Anchor& anchor : anchors) {
        std::int64_t ms = anchor.ms - step * (walk / 2) - rng.in(0, step);
        for (int i = 0; i < walk; ++i, ms += step) pair.ask(ms, memo);
        for (int i = 0; i < scatter; ++i) {
            pair.ask(rng.in(anchor.ms - 3 * kDay, anchor.ms + 3 * kDay), memo);
        }
    }
    for (int i = 0; i < wide; ++i) pair.ask(rng.in(63072000000LL, 2145830400000LL), memo);
}

// 2024-2025 instants the scenarios center on, UTC.
constexpr std::int64_t kUtcYearEnd = 1735689600000LL;      // 2025-01-01 00:00 UTC
constexpr std::int64_t kNySpring = 1710054000000LL;        // 2024-03-10 07:00 UTC (03:00 EDT)
constexpr std::int64_t kNyFall = 1730613600000LL;          // 2024-11-03 06:00 UTC (01:00 EST)
constexpr std::int64_t kLondonSpring = 1743296400000LL;    // 2025-03-30 01:00 UTC
constexpr std::int64_t kLondonFall = 1761440400000LL;      // 2025-10-26 01:00 UTC
constexpr std::int64_t kTokyoYearEnd = 1735657200000LL;    // 2025-01-01 00:00 JST
constexpr std::int64_t kPosixSpring = 1741503600000LL;     // 2025-03-09 07:00 UTC
constexpr std::int64_t kPosixFall = 1762063200000LL;       // 2025-11-02 06:00 UTC
constexpr std::int64_t kIstYearEnd = 1735669800000LL;      // 2025-01-01 00:00 +05:30

struct Zone {
    const char* name;
    std::vector<Anchor> anchors;
};

const std::vector<const char*> kSessions = {
    "24x7",
    "0930-1600:23456",
    "0900-1130,1230-1500",
    "1800-1700:23456",
    "0000-0230,0330-0600,1300-2400",
};

// (script, input): equal and aggregating, fixed and calendar.
const std::vector<std::pair<const char*, const char*>> kFixed = {
    {"5", "5"}, {"15", "5"}, {"60", "60"}, {"30S", "30S"},
};
const std::vector<std::pair<const char*, const char*>> kCalendarTf = {
    {"D", "60"}, {"2D", "D"}, {"W", "D"}, {"M", "D"},
};

void the_memo_answers_what_the_calendar_answers() {
    const std::vector<Zone> zones = {
        {"UTC", {{"year end", kUtcYearEnd}}},
        {"America/New_York", {{"spring", kNySpring}, {"fall", kNyFall}}},
        {"Asia/Tokyo", {{"year end", kTokyoYearEnd}}},
        {"Europe/London", {{"spring", kLondonSpring}, {"fall", kLondonFall}}},
        {"EST5EDT,M3.2.0,M11.1.0", {{"spring", kPosixSpring}, {"fall", kPosixFall}}},
        {"UTC+05:30", {{"year end", kIstYearEnd}}},
    };
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (const Zone& zone : zones) {
        scenario = zone.name;
        for (const char* session : kSessions) {
            const SessionCalendar cal = calendar(session, zone.name);
            for (const auto& [script, input] : kFixed) {
                probe(cal, script, input, zone.anchors, 32, 8, 2, seed += 0x1234567);
            }
            for (const auto& [script, input] : kCalendarTf) {
                probe(cal, script, input, zone.anchors, 3, 1, 1, seed += 0x1234567);
            }
        }
    }
}

// America/Sitka, 9-12 October 1867, instants ten minutes apart under a
// "2330-2300" session: the memo must answer what the memo-free lookup answers
// where "the last resolved day holds this instant" does not (see the header),
// and only where that lookup has one answer on this C library.
void the_memo_keeps_the_candidate_order_across_a_date_line_change() {
    scenario = "America/Sitka 1867";
    const std::int64_t from = -3226068000000LL;  // 1867-10-09 06:00 UTC
    const std::int64_t to = -3225787200000LL;    // 1867-10-12 12:00 UTC
    const std::int64_t step = 10 * kMinute;
    const Timeframe five = tf("5");
    for (const char* session : {"2330-2300"}) {
        const SessionCalendar cal = calendar(session, "America/Sitka");
        std::vector<std::optional<NativeSessionDay>> days;
        std::vector<std::optional<NativeInterval>> intervals;
        bool one_answer = true;
        for (std::int64_t ms = from; ms < to && one_answer; ms += step) {
            (void)resolve_civil("America/Sitka", 1867, 10, 5, 12, 0);
            days.push_back(session_day_at(cal, ms));
            intervals.push_back(interval_containing(cal, five, ms));
            (void)resolve_civil("America/Sitka", 1867, 10, 25, 12, 0);
            one_answer = same(days.back(), session_day_at(cal, ms))
                && same(intervals.back(), interval_containing(cal, five, ms));
        }
        if (!one_answer) {
            std::printf("  America/Sitka 1867 '%s': skipped, the memo-free lookup's answer "
                        "depends on mktime's call history on this C library\n", session);
            continue;
        }
        SessionDayMemo memo;
        std::optional<NativeSessionDay> last;
        int memo_differs = 0;
        int shortcut_differs = 0;
        std::size_t i = 0;
        for (std::int64_t ms = from; ms < to; ms += step, ++i) {
            ++compared;
            const bool day_ok = same(session_day_at(cal, ms, memo), days[i]);
            const bool interval_ok = same(interval_containing(cal, five, ms, memo), intervals[i]);
            if (!day_ok || !interval_ok) ++memo_differs;
            CHECK(day_ok);
            CHECK(interval_ok);
            if (last && last->holds(ms) && !same(last, days[i])) ++shortcut_differs;
            if (days[i] && days[i]->holds(ms)) last = days[i];
        }
        std::printf("  America/Sitka 1867 '%s': %zu instants, the memo differs at %d, the "
                    "holds-shortcut would differ at %d\n", session, i, memo_differs,
                    shortcut_differs);
    }
}

void the_memo_answers_at_the_extremes() {
    scenario = "extremes";
    const std::vector<std::int64_t> instants = {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::min() + 1,
        -62135596800001LL,  // one ms before 0001-01-01
        -62135596800000LL,  // 0001-01-01 00:00 UTC
        -1,
        0,
        1,
        253402214400000LL,  // 9999-12-31 00:00 UTC
        253402300799999LL,  // 9999-12-31 23:59:59.999 UTC
        253402300800000LL,  // 10000-01-01 00:00 UTC
        std::numeric_limits<std::int64_t>::max() - 1,
        std::numeric_limits<std::int64_t>::max(),
    };
    for (const char* zone : {"UTC", "America/New_York", "Asia/Tokyo", "UTC+05:30"}) {
        for (const char* session : {"24x7", "0930-1600", "1800-1700"}) {
            const SessionCalendar cal = calendar(session, zone);
            for (const auto& [script, input] : {std::pair<const char*, const char*>{"5", "5"},
                                                {"D", "60"}, {"M", "D"}}) {
                Pair pair;
                pair.calendar = &cal;
                pair.script = tf(script);
                pair.input = tf(input);
                pair.label = std::string(zone) + " " + session + " " + script;
                SessionDayMemo memo;
                for (std::int64_t ms : instants) pair.ask(ms, memo);
                for (std::int64_t ms : instants) pair.ask(ms, memo);
            }
        }
    }
}

void the_memo_answers_nothing_for_an_invalid_calendar_or_timeframe() {
    scenario = "invalid";
    const SessionCalendar invalid{};
    const SessionCalendar utc = calendar("24x7", "UTC");
    SessionDayMemo memo;
    CHECK(!interval_containing(invalid, tf("5"), 1735689600000LL, memo));
    CHECK(!session_day_at(invalid, 1735689600000LL, memo));
    CHECK(!in_session(invalid, 1735689600000LL, memo));
    CHECK(!interval_containing(utc, Timeframe{}, 1735689600000LL, memo));
    CHECK(!interval_containing(utc, tf("5"), Timeframe{}, 1735689600000LL, memo));
    CHECK(interval_containing(utc, tf("5"), 1735689600000LL, memo).has_value());
}

// The owner's side of the contract: a calendar reassigned in place is
// forgotten by reset(); a calendar at another address is never confused with
// the last one; a moved memo keeps answering.
void the_memo_forgets_a_rebuilt_calendar() {
    scenario = "lifecycle";
    const Timeframe five = tf("5");
    const std::int64_t t = kNyFall + 3 * kHour;  // 2024-11-03 09:00 UTC

    SessionCalendar cal = calendar("0930-1600", "America/New_York");
    SessionDayMemo memo;
    const auto ny = interval_containing(cal, five, t, memo);
    CHECK(same(ny, interval_containing(cal, five, t)));

    cal = calendar("0900-1130,1230-1500", "Asia/Tokyo");
    memo.reset();
    const auto tokyo = interval_containing(cal, five, t, memo);
    CHECK(same(tokyo, interval_containing(cal, five, t)));
    CHECK(!same(ny, tokyo));
    CHECK(same(session_day_at(cal, t, memo), session_day_at(cal, t)));

    const SessionCalendar london = calendar("0800-1630", "Europe/London");
    const SessionCalendar utc = calendar("24x7", "UTC");
    for (int i = 0; i < 400; ++i) {
        const std::int64_t ms = t + static_cast<std::int64_t>(i) * 37 * kMinute;
        const SessionCalendar& which = (i % 3 == 0) ? london : (i % 3 == 1) ? utc : cal;
        CHECK(same(interval_containing(which, five, ms, memo),
                   interval_containing(which, five, ms)));
        CHECK(same(session_day_at(which, ms, memo), session_day_at(which, ms)));
    }

    SessionDayMemo moved = std::move(memo);
    CHECK(same(interval_containing(utc, five, t, moved), interval_containing(utc, five, t)));
    SessionDayMemo assigned;
    assigned = std::move(moved);
    CHECK(same(interval_containing(london, five, t, assigned),
               interval_containing(london, five, t)));
}

}  // namespace

int main() {
    the_memo_answers_what_the_calendar_answers();
    the_memo_keeps_the_candidate_order_across_a_date_line_change();
    the_memo_answers_at_the_extremes();
    the_memo_answers_nothing_for_an_invalid_calendar_or_timeframe();
    the_memo_forgets_a_rebuilt_calendar();
    if (failures == 0) {
        std::printf("test_native_calendar_memo: %lld instants, memo == calendar\n", compared);
    }
    return failures == 0 ? 0 : 1;
}
