// The session-day memo owns a fixed heap, and owning it moves
// no answer.
//
// The L0b heap census (4,000 against 16,000 one-minute UTC bars) found a
// growth site in the kernel calendar that no lane owned: the span vectors of
// the memo's session days, +0.012 bytes a bar. The memo was never unbounded:
// it is a direct-mapped table of kDayCapacity days that replaces a day on a
// collision. What grew was the heap its slots allocated one buffer at a time, a
// new day and a new buffer, until the table was full -- three days of bars
// against eleven sit inside that fill. The lane reserves every slot's span
// buffer when the memo binds its calendar, to the most spans a session day of
// that calendar has (session_day_span_bound), and assigns into it after, so the
// heap is the same whatever days were asked, and however many.
//
// The reference is the memo-free calendar of native_calendar.hpp, which keeps
// nothing and is what any memo, bounded or not, must say; the memo of the lane
// base was the same table with a buffer per slot and is not kept. This row
// drives one memo through thousands of consecutive session days, a run's walk,
// for calendars with weekday masks (closed days), a declared break, an
// overnight session, a session of three windows and no declared session at all,
// and asks the questions the memo answers -- the interval of an instant, its
// session day, whether it is in session -- of the memo and of the calendar.
// Every answer must agree bit for bit, presence and every field. Every few
// days a memo that has been asked nothing answers the same question too (the
// recomputation a day the table dropped is answered by); and after the walk the
// instants well behind its front, days the table dropped long ago, are asked of
// it, and then asked again. The footprint is read after every day of every
// walk: the capacity of the day slots' buffers is what the first lookup left,
// the days held never pass kDayCapacity, and by the end the table is full and
// every later day has replaced one, so the cap is exercised, not only stated.
// The calendars of an exchange zone (clock changes inside the walk) go through
// the same walk over fewer days, because every resolution there is a mktime.
//
// Fail-before: at the lane's base SessionDayMemo has no kDayCapacity, no
// retained_span_capacity and no day_resolutions, and native_calendar_memo.hpp
// declares no session_day_span_bound, so this TU does not compile there (the
// lane report records the first diagnostic).
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/native_calendar.hpp>

#include "../src/native_calendar_memo.hpp"

#include <cstddef>
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
long long compared_cold = 0;
std::string scenario = "initialization";

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", scenario.c_str(),         \
                     __FILE__, __LINE__, #condition);                          \
    }                                                                          \
} while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kUtcFrom = 1420416000000LL;    // 2015-01-05 00:00 UTC, a Monday
constexpr std::int64_t kZonedFrom = 1704067200000LL;  // 2024-01-01 00:00 UTC, a Monday

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

// The questions the memo answers, asked of the memo-free calendar and of the
// memo for the same instant: they must agree field for field. The interval is
// left out of an ask where a zoned calendar makes the reference dear.
void ask(const SessionCalendar& cal, const Timeframe& script, const Timeframe& input,
         std::int64_t ms, SessionDayMemo& memo, bool with_interval) {
    ++compared;
    const auto day = session_day_at(cal, ms);
    CHECK(same(day, session_day_at(cal, ms, memo)));
    CHECK(in_session(cal, ms) == in_session(cal, ms, memo));
    if (with_interval) {
        CHECK(same(interval_containing(cal, script, input, ms),
                   interval_containing(cal, script, input, ms, memo)));
    }
    // No session day has more spans than the bound every slot's buffer holds.
    if (day) CHECK(day->spans.size() <= session_day_span_bound(cal));
}

// The same questions of a memo that has been asked nothing: the recomputation a
// day dropped from the table is answered by, which the memo that kept or
// dropped any days must say the same as.
void ask_cold(const SessionCalendar& cal, const Timeframe& script, const Timeframe& input,
              std::int64_t ms, SessionDayMemo& memo) {
    ++compared_cold;
    SessionDayMemo cold;
    CHECK(same(session_day_at(cal, ms, cold), session_day_at(cal, ms, memo)));
    CHECK(in_session(cal, ms, cold) == in_session(cal, ms, memo));
    CHECK(same(interval_containing(cal, script, input, ms, cold),
               interval_containing(cal, script, input, ms, memo)));
}

// One calendar under one memo, a run's walk: `days` consecutive days from
// `from`, two instants each (one anywhere in the day, one where sessions
// open), every answer compared with the memo-free calendar's, the interval on
// every `interval_every`th day and a cold memo's on every `cold_every`th (never
// when 0). The footprint is read after every day. Then the look-back.
void sweep(const char* session, const char* zone, const char* script, const char* input,
           std::int64_t from, int days, int interval_every, int cold_every,
           std::uint64_t seed) {
    scenario = std::string(zone) + " '" + session + "' " + script + " over " + input;
    const SessionCalendar cal = calendar(session, zone);
    const Timeframe script_tf = tf(script);
    const Timeframe input_tf = tf(input);
    Rng rng{seed};
    SessionDayMemo memo;
    CHECK(memo.retained_days() == 0);
    CHECK(memo.retained_span_capacity() == 0);

    std::size_t reserved = 0;
    for (int d = 0; d < days; ++d) {
        const std::int64_t start = from + static_cast<std::int64_t>(d) * kDay;
        const bool with_interval = d % interval_every == 0;
        ask(cal, script_tf, input_tf, rng.in(start, start + kDay), memo, with_interval);
        ask(cal, script_tf, input_tf, start + 9 * kHour + rng.in(0, 30 * kMinute), memo,
            with_interval);
        if (cold_every > 0 && d % cold_every == 0) {
            ask_cold(cal, script_tf, input_tf, rng.in(start, start + kDay), memo);
        }
        if (d == 0) {
            // What the first lookup reserved: every slot, to the bound.
            reserved = memo.retained_span_capacity();
            CHECK(reserved > 0);
            CHECK(reserved >= SessionDayMemo::kDayCapacity * session_day_span_bound(cal));
        }
        CHECK(memo.retained_span_capacity() == reserved);
        CHECK(memo.retained_days() <= SessionDayMemo::kDayCapacity);
    }

    // The cap was reached and used: the walk is longer than the table, which
    // is full, and the memo resolved more days than it holds.
    CHECK(days > static_cast<int>(SessionDayMemo::kDayCapacity));
    CHECK(memo.retained_days() == SessionDayMemo::kDayCapacity);
    CHECK(memo.day_resolutions() >= static_cast<std::uint64_t>(days));

    // The look-back: instants behind the front. The recent ones the table may
    // still hold; the ones past a table's width it dropped, and a day dropped
    // is resolved again (the count rises) and answered as ever.
    const std::int64_t front = from + static_cast<std::int64_t>(days - 1) * kDay;
    for (const int back : {1, 2, 3, 40, 70, 200, 500, 1000}) {
        if (back >= days) continue;
        const std::int64_t at = front - static_cast<std::int64_t>(back) * kDay
            + rng.in(0, kDay);
        const std::uint64_t before = memo.day_resolutions();
        ask(cal, script_tf, input_tf, at, memo, true);
        if (back >= 200) CHECK(memo.day_resolutions() > before);
        // Asked again, the day is answered from what that resolved.
        const std::uint64_t resolved = memo.day_resolutions();
        CHECK(same(session_day_at(cal, at), session_day_at(cal, at, memo)));
        CHECK(memo.day_resolutions() == resolved);
        CHECK(memo.retained_span_capacity() == reserved);
        CHECK(memo.retained_days() <= SessionDayMemo::kDayCapacity);
    }
}

void the_memo_answers_what_the_calendar_answers_over_thousands_of_days() {
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    // Closed days (a weekday mask), a declared break, an overnight session, a
    // session of three windows and no declared session at all, as the other
    // calendar rows build them; an hour bucket and a day bucket.
    for (const char* session : {"24x7", "0930-1600:23456", "0900-1130,1230-1500",
                                "1800-1700:23456", "0000-0230,0330-0600,1300-2400"}) {
        sweep(session, "UTC", "5", "5", kUtcFrom, 1500, 1, 7, seed += 0x1234567);
        sweep(session, "UTC", "D", "60", kUtcFrom, 1500, 1, 7, seed += 0x1234567);
    }
    // Week and month cores read a whole period's days for one question.
    for (const char* session : {"0930-1600:23456", "1800-1700:23456"}) {
        sweep(session, "UTC", "W", "D", kUtcFrom, 400, 1, 0, seed += 0x1234567);
        sweep(session, "UTC", "M", "D", kUtcFrom, 400, 1, 0, seed += 0x1234567);
    }
}

void the_memo_answers_what_the_calendar_answers_on_exchange_zones() {
    std::uint64_t seed = 0xD1B54A32D192ED03ULL;
    // New York's 2024 clock changes (March 10 and November 3) fall inside the
    // walk, across an overnight session whose origin sits in the evening.
    sweep("1800-1700:23456", "America/New_York", "5", "5", kZonedFrom, 330, 3, 0, seed);
    sweep("0900-1130,1230-1500", "Asia/Tokyo", "60", "60", kZonedFrom, 120, 3, 0, seed += 7);
}

// A day the calendar cannot key is an answer too: the slot holds none, hands
// out nothing, and is rewritten by the next day that maps to it.
void the_memo_keeps_an_absent_answer_like_any_other() {
    scenario = "absent days";
    const SessionCalendar cal = calendar("0930-1600:23456", "UTC");
    const Timeframe five = tf("5");
    const std::vector<std::int64_t> instants = {
        std::numeric_limits<std::int64_t>::min(),
        -62135596800001LL,  // one ms before 0001-01-01
        kUtcFrom,
        253402300800000LL,  // 10000-01-01 00:00 UTC
        kUtcFrom + kDay,
        std::numeric_limits<std::int64_t>::max(),
        kUtcFrom + 2 * kDay,
        kUtcFrom,
    };
    SessionDayMemo memo;
    std::size_t reserved = 0;
    for (const std::int64_t ms : instants) {
        ask(cal, five, five, ms, memo, true);
        if (reserved == 0) reserved = memo.retained_span_capacity();
        CHECK(memo.retained_span_capacity() == reserved);
        CHECK(memo.retained_days() <= SessionDayMemo::kDayCapacity);
    }
    CHECK(reserved >= SessionDayMemo::kDayCapacity * session_day_span_bound(cal));
}

void the_footprint_follows_the_calendar_it_is_bound_to() {
    scenario = "lifecycle";
    const SessionCalendar narrow = calendar("0930-1600", "UTC");
    const SessionCalendar wide = calendar("0000-0100,0200-0300,0400-0500,0600-0700", "UTC");
    const SessionCalendar all_day = calendar("24x7", "UTC");
    const SessionCalendar invalid{};
    CHECK(session_day_span_bound(invalid) == 0);
    CHECK(session_day_span_bound(all_day) >= 1);
    CHECK(session_day_span_bound(narrow) >= 1);
    CHECK(session_day_span_bound(wide) > session_day_span_bound(narrow));

    // Nothing is held before a calendar is bound, and a calendar that is not
    // valid is bound to nothing: it resolves no day and reserves no buffer.
    SessionDayMemo memo;
    CHECK(memo.retained_days() == 0);
    CHECK(memo.retained_span_capacity() == 0);
    CHECK(memo.day_resolutions() == 0);
    CHECK(!session_day_at(invalid, kUtcFrom, memo).has_value());
    CHECK(memo.retained_days() == 0);
    CHECK(memo.retained_span_capacity() == 0);
    CHECK(memo.day_resolutions() == 0);

    // A calendar at another address is a new binding: reserved for what it has.
    CHECK(session_day_at(narrow, kUtcFrom, memo).has_value());
    const std::size_t narrow_capacity = memo.retained_span_capacity();
    CHECK(narrow_capacity >= SessionDayMemo::kDayCapacity * session_day_span_bound(narrow));
    CHECK(session_day_at(wide, kUtcFrom, memo).has_value());
    const std::size_t wide_capacity = memo.retained_span_capacity();
    CHECK(wide_capacity >= SessionDayMemo::kDayCapacity * session_day_span_bound(wide));
    CHECK(wide_capacity > narrow_capacity);

    // reset() forgets all of it, and the same calendar is reserved the same.
    memo.reset();
    CHECK(memo.retained_days() == 0);
    CHECK(memo.retained_span_capacity() == 0);
    CHECK(memo.day_resolutions() == 0);
    CHECK(session_day_at(wide, kUtcFrom, memo).has_value());
    CHECK(memo.retained_span_capacity() == wide_capacity);

    // A moved memo keeps what it reserved.
    SessionDayMemo moved = std::move(memo);
    CHECK(moved.retained_span_capacity() == wide_capacity);
    CHECK(moved.retained_days() >= 1);
    CHECK(moved.retained_days() <= SessionDayMemo::kDayCapacity);
}

}  // namespace

int main() {
    the_memo_answers_what_the_calendar_answers_over_thousands_of_days();
    the_memo_answers_what_the_calendar_answers_on_exchange_zones();
    the_memo_keeps_an_absent_answer_like_any_other();
    the_footprint_follows_the_calendar_it_is_bound_to();
    if (failures == 0) {
        std::printf("test_native_calendar_memo_bound: %lld instants against the calendar, "
                    "%lld against a cold memo, footprint fixed\n", compared, compared_cold);
    }
    return failures == 0 ? 0 : 1;
}
