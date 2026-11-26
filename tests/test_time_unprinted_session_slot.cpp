// time() / time_close() reading a later bar over a session slot the input
// holds no bar for, with no symbol calendar installed (the default).
//
// TradingView reads the bar after a chart bar from its calendar for the
// symbol, not from the chart's bars (tests/test_symbol_calendar_tapes.cpp, its
// opening note). A slot of the symbol's session that lies between two bars the
// input holds in one session day is such a calendar slot, so the host reads it
// forward without a calendar too (PineStrategyHost::pine_time_offset,
// unprinted_slot). A session that never closes ("24x7") has no day that ends
// early and none to skip, so there the slot is read between two bars on any
// days: across the trading-day roll and at a day's edge. Every bar here is
// invented: UTC charts, five-minute on a 0900-1000 session unless a scenario
// names another.
//
// 1. One slot without a bar (09:10): each overload reads it as the next bar,
//    through the calls generated code makes.
// 2. The same bars with the session installed as the symbol calendar
//    (set_symbol_calendar): the same readings, the control.
// 3. Two slots without a bar (09:10, 09:15), a whole "10" bar: the next "10"
//    bar is the one no bar was printed in.
// 4. The input's next bar on another session day stands: only a calendar says
//    whether the day went on past its last bar. The control for what does not
//    move.
// 5. A 24x7 session, one minute, bars at 23:53, 23:58 and 00:01: time("", -1)
//    reads 23:54, 23:59 and 00:02. The 23:59 slot lies across the trading-day
//    roll from the input's next bar, and is read as the 23:54 slot inside the
//    day is. These three readings were observed on TradingView with a
//    synthetic script; nothing else is asserted on these bars.
// 6. A session with an overnight break (0900-1000), slots without a bar at
//    the day's edges on both sides of the break: the input's next bar stands,
//    as in 4. Not compared with TradingView: these rows pin the answer the
//    24x7 reading leaves alone.
//
// Source-bound (includes pineforge/source): release profile only.

#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "init";

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL [%s] line %d: %s\n", scenario, __LINE__, #expr); \
        }                                                                      \
    } while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kDay = 24 * 60 * kMinute;
const char* const kSession = "0900-1000";
// The session that never closes.
const char* const kAllDay = "24x7";
using Sessions = std::vector<std::pair<std::int64_t, std::int64_t>>;

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// An instant of Tuesday 2030-01-08 (`day` 0) or of the Wednesday after it
// (`day` 1), UTC.
std::int64_t at(int day, int hour, int minute) {
    return (days_from_civil(2030, 1, 8) + day) * kDay + (hour * 60 + minute) * kMinute;
}

// "d<day> HH:MM" of an instant, "na" for na.
std::string spell(std::int64_t ms) {
    if (is_na(ms)) return "na";
    const std::int64_t minutes = (ms - at(0, 0, 0)) / kMinute;
    char out[72];
    std::snprintf(out, sizeof out, "d%lld %02lld:%02lld",
                  static_cast<long long>(minutes / (24 * 60)),
                  static_cast<long long>(minutes % (24 * 60) / 60),
                  static_cast<long long>(minutes % 60));
    return out;
}

void expect(const char* what, std::int64_t bar, std::int64_t got, std::int64_t want) {
    ++checks;
    if (got == want) return;
    ++failures;
    std::printf("  FAIL [%s] bar %s %s: host %s, expected %s\n", scenario, spell(bar).c_str(),
                what, spell(got).c_str(), spell(want).c_str());
}

// What one bar reads, each field the call generated code makes for the Pine
// expression beside it.
struct Reading {
    std::int64_t next_open = 0;      // time("", bars_back = -1)
    std::int64_t next_close = 0;     // time_close("", bars_back = -1)
    std::int64_t session_open = 0;   // time("", "0900-1000", -1)
    std::int64_t session_close = 0;  // time_close("", "0900-1000", -1)
    std::int64_t zone_open = 0;      // time("", "0900-1000", "UTC", -1)
    std::int64_t zone_close = 0;     // time_close("", "0900-1000", "UTC", -1)
    std::int64_t step_open = 0;      // time("", 0, -1)
    std::int64_t step_close = 0;     // time_close("", 0, -1)
    std::int64_t second_open = 0;    // time("", bars_back = -2)
    std::int64_t prior_open = 0;     // time("", bars_back = 1)
    std::int64_t ten_open = 0;       // time("10", 0, -1)
    std::int64_t ten_close = 0;      // time_close("10", 0, -1)
};

class SlotHost final : public source::PineStrategyHost {
public:
    // A UTC symbol whose session is `symbol_session`.
    explicit SlotHost(const char* symbol_session) {
        set_syminfo_session(symbol_session);
        set_syminfo_timezone("UTC");
    }
    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const std::string none;
        const std::string session = kSession;
        const std::string utc = "UTC";
        const std::string ten = "10";
        Reading r;
        r.next_open = pine_time_offset(t, -1, none, none, none, 0, false);
        r.next_close = pine_time_offset(t, -1, none, none, none, 0, true);
        r.session_open = pine_time_offset(t, -1, none, session, none, 0, false);
        r.session_close = pine_time_offset(t, -1, none, session, none, 0, true);
        r.zone_open = pine_time_offset(t, -1, none, session, utc, 0, false);
        r.zone_close = pine_time_offset(t, -1, none, session, utc, 0, true);
        r.step_open = pine_time_offset(t, 0, none, none, none, -1, false);
        r.step_close = pine_time_offset(t, 0, none, none, none, -1, true);
        r.second_open = pine_time_offset(t, -2, none, none, none, 0, false);
        r.prior_open = pine_time_offset(t, 1, none, none, none, 0, false);
        r.ten_open = pine_time_offset(t, 0, ten, none, none, -1, false);
        r.ten_close = pine_time_offset(t, 0, ten, none, none, -1, true);
        seen[t] = r;
    }
    std::map<std::int64_t, Reading> seen;
};

// Runs a chart of `timeframe` (five minutes unless named) holding one bar at
// each of `opens`, on a symbol whose session is `symbol_session` (0900-1000
// unless named); `calendar`, when given, is installed as the symbol calendar.
std::map<std::int64_t, Reading> read(const std::vector<std::int64_t>& opens,
                                     const Sessions* calendar = nullptr,
                                     const char* timeframe = "5",
                                     const char* symbol_session = kSession) {
    std::vector<Bar> bars;
    double close = 4.0;
    for (const std::int64_t ts : opens) {
        Bar b{};
        b.timestamp = ts;
        b.open = close; b.high = close + 1.0; b.low = close - 1.0; b.close = close;
        b.volume = 1.0;
        bars.push_back(b);
        close += 1.0;
    }
    SlotHost host(symbol_session);
    if (calendar != nullptr) CHECK(host.set_symbol_calendar(*calendar));
    host.run(bars.data(), static_cast<int>(bars.size()), timeframe, timeframe);
    CHECK(host.last_error().empty());
    CHECK(host.seen.size() == opens.size());
    return host.seen;
}

const Reading* reading_at(const std::map<std::int64_t, Reading>& seen, std::int64_t bar) {
    const auto it = seen.find(bar);
    CHECK(it != seen.end());
    return it == seen.end() ? nullptr : &it->second;
}

// The six bars of the 09:00 hour's first seven slots: 09:10 holds none.
std::vector<std::int64_t> gapped_bars() {
    return {at(0, 9, 0), at(0, 9, 5), at(0, 9, 15), at(0, 9, 20), at(0, 9, 25), at(0, 9, 30)};
}

// Every bar's readings on gapped_bars(): the bar after 09:05 is the 09:10
// slot, and the bar before 09:15 the 09:05 bar the input holds.
void check_gapped(const std::map<std::int64_t, Reading>& seen) {
    struct Row {
        std::int64_t bar, next_open, next_close, second_open, prior_open;
    };
    const Row rows[] = {
        {at(0, 9, 0), at(0, 9, 5), at(0, 9, 10), at(0, 9, 10), na<std::int64_t>()},
        {at(0, 9, 5), at(0, 9, 10), at(0, 9, 15), at(0, 9, 15), at(0, 9, 0)},
        {at(0, 9, 15), at(0, 9, 20), at(0, 9, 25), at(0, 9, 25), at(0, 9, 5)},
        {at(0, 9, 20), at(0, 9, 25), at(0, 9, 30), at(0, 9, 30), at(0, 9, 15)},
        {at(0, 9, 25), at(0, 9, 30), at(0, 9, 35), at(0, 9, 35), at(0, 9, 20)},
        {at(0, 9, 30), at(0, 9, 35), at(0, 9, 40), at(0, 9, 40), at(0, 9, 25)},
    };
    for (const Row& row : rows) {
        const Reading* r = reading_at(seen, row.bar);
        if (r == nullptr) continue;
        expect("time(\"\", -1)", row.bar, r->next_open, row.next_open);
        expect("time_close(\"\", -1)", row.bar, r->next_close, row.next_close);
        expect("time(\"\", session, -1)", row.bar, r->session_open, row.next_open);
        expect("time_close(\"\", session, -1)", row.bar, r->session_close, row.next_close);
        expect("time(\"\", session, tz, -1)", row.bar, r->zone_open, row.next_open);
        expect("time_close(\"\", session, tz, -1)", row.bar, r->zone_close, row.next_close);
        expect("time(\"\", 0, -1)", row.bar, r->step_open, row.next_open);
        expect("time_close(\"\", 0, -1)", row.bar, r->step_close, row.next_close);
        expect("time(\"\", -2)", row.bar, r->second_open, row.second_open);
        expect("time(\"\", 1)", row.bar, r->prior_open, row.prior_open);
    }
}

void test_slot_between_two_bars() {
    scenario = "a slot between two bars, no calendar";
    check_gapped(read(gapped_bars()));
}

void test_calendar_control() {
    scenario = "the same bars, the session as the symbol calendar";
    const Sessions calendar = {{at(0, 9, 0), at(0, 10, 0)}};
    check_gapped(read(gapped_bars(), &calendar));
}

void test_whole_timeframe_bar_without_bars() {
    scenario = "a whole \"10\" bar without bars, no calendar";
    const auto seen = read({at(0, 9, 0), at(0, 9, 5), at(0, 9, 20), at(0, 9, 25)});
    struct Row {
        std::int64_t bar, ten_open, ten_close;
    };
    const Row rows[] = {
        {at(0, 9, 0), at(0, 9, 10), at(0, 9, 20)},
        {at(0, 9, 5), at(0, 9, 10), at(0, 9, 20)},
        {at(0, 9, 20), at(0, 9, 30), at(0, 9, 40)},
        {at(0, 9, 25), at(0, 9, 30), at(0, 9, 40)},
    };
    for (const Row& row : rows) {
        const Reading* r = reading_at(seen, row.bar);
        if (r == nullptr) continue;
        expect("time(\"10\", 0, -1)", row.bar, r->ten_open, row.ten_open);
        expect("time_close(\"10\", 0, -1)", row.bar, r->ten_close, row.ten_close);
    }
    // Both slots are read in turn: 09:10 after 09:05, then 09:15.
    if (const Reading* r = reading_at(seen, at(0, 9, 5))) {
        expect("time(\"\", -1)", at(0, 9, 5), r->next_open, at(0, 9, 10));
        expect("time(\"\", -2)", at(0, 9, 5), r->second_open, at(0, 9, 15));
    }
}

void test_next_session_day_stands() {
    scenario = "the input's next bar on another session day";
    const auto seen = read({at(0, 9, 0), at(0, 9, 5), at(1, 9, 0), at(1, 9, 5)});
    if (const Reading* r = reading_at(seen, at(0, 9, 5))) {
        expect("time(\"\", -1)", at(0, 9, 5), r->next_open, at(1, 9, 0));
        expect("time_close(\"\", -1)", at(0, 9, 5), r->next_close, at(1, 9, 5));
        expect("time(\"\", 0, -1)", at(0, 9, 5), r->step_open, at(1, 9, 0));
        expect("time(\"\", -2)", at(0, 9, 5), r->second_open, at(1, 9, 5));
        expect("time(\"10\", 0, -1)", at(0, 9, 5), r->ten_open, at(1, 9, 0));
        expect("time_close(\"10\", 0, -1)", at(0, 9, 5), r->ten_close, at(1, 9, 10));
    }
    if (const Reading* r = reading_at(seen, at(1, 9, 0)))
        expect("time(\"\", 1)", at(1, 9, 0), r->prior_open, at(0, 9, 5));
    // Past the input the session clock answers, as before.
    if (const Reading* r = reading_at(seen, at(1, 9, 5)))
        expect("time(\"\", -1)", at(1, 9, 5), r->next_open, at(1, 9, 10));
}

void test_slot_across_the_day_roll() {
    scenario = "24x7, one minute: the slot across the trading-day roll";
    // No bar at 23:54-23:57, at 23:59 or at the next day's 00:00.
    const auto seen = read({at(0, 23, 53), at(0, 23, 58), at(1, 0, 1)}, nullptr, "1", kAllDay);
    struct Row {
        std::int64_t bar, next_open;
    };
    const Row rows[] = {
        // Inside the day: the next minute, which holds no bar.
        {at(0, 23, 53), at(0, 23, 54)},
        // Across the roll: the day's last minute, which holds no bar.
        {at(0, 23, 58), at(0, 23, 59)},
        // The input's last bar: the session clock answers.
        {at(1, 0, 1), at(1, 0, 2)},
    };
    for (const Row& row : rows) {
        if (const Reading* r = reading_at(seen, row.bar))
            expect("time(\"\", -1)", row.bar, r->next_open, row.next_open);
    }
}

// One bar's readings on the chart's own timeframe, with no session argument:
// the bar after it and that bar's close, the second bar after it, and the bar
// before it.
struct ChartRow {
    std::int64_t bar, next_open, next_close, second_open, prior_open;
};

void check_chart_rows(const std::map<std::int64_t, Reading>& seen,
                      const std::vector<ChartRow>& rows) {
    for (const ChartRow& row : rows) {
        const Reading* r = reading_at(seen, row.bar);
        if (r == nullptr) continue;
        expect("time(\"\", -1)", row.bar, r->next_open, row.next_open);
        expect("time_close(\"\", -1)", row.bar, r->next_close, row.next_close);
        expect("time(\"\", 0, -1)", row.bar, r->step_open, row.next_open);
        expect("time_close(\"\", 0, -1)", row.bar, r->step_close, row.next_close);
        expect("time(\"\", -2)", row.bar, r->second_open, row.second_open);
        expect("time(\"\", 1)", row.bar, r->prior_open, row.prior_open);
    }
}

void test_overnight_break_stands() {
    scenario = "an overnight break: slots at the day's edges on both sides";
    // No bar at 09:50 or 09:55 before the break, nor at 09:00 after it. The
    // input's next bar stands on both counts, with no calendar to say that
    // the first day went on to its close or that the second opened on time.
    // Pinned rows: the same readings with and without the 24x7 rule.
    const auto seen = read({at(0, 9, 45), at(1, 9, 5)});
    check_chart_rows(seen, {
        {at(0, 9, 45), at(1, 9, 5), at(1, 9, 10), at(1, 9, 10), na<std::int64_t>()},
        {at(1, 9, 5), at(1, 9, 10), at(1, 9, 15), at(1, 9, 15), at(0, 9, 45)},
    });
}

}  // namespace

int main() {
    std::printf("time() / time_close(): a session slot the input holds no bar for\n");
    test_slot_between_two_bars();
    test_calendar_control();
    test_whole_timeframe_bar_without_bars();
    test_next_session_day_stands();
    test_slot_across_the_day_roll();
    test_overnight_break_stands();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
