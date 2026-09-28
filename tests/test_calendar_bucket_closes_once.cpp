// A D/W/M bucket closes exactly once, and not while the next known input bar
// still belongs to its period (R5 lane W15-KERNEL-CAL, item C).
//
// An exchange session template knows Monday to Friday. NSE traded two
// sessions its template does not declare inside this campaign's windows: the
// Budget-day sessions of Sat 2025-02-01 and Sun 2026-02-01, whole 09:15-15:30
// days. TradingView keeps each in its week -- Tue 2026-01-27 .. Sun 02-01 is
// one weekly bar whose bar_index advances once, on Sunday's 15:15 bar (lab tv
// w14-htf-w-nifty15, NSE:NIFTY 15, 2025-04-01 .. 2026-05-01). The kernel's
// nominal W/M rule ends a week at the close of its last WEEKDAY session, so
// it completed the week on Friday's 15:15 bar, and every Sunday bar merged
// back into the same bucket completed it again: the requested weekly series
// advanced 26 times for one week.
//
// Kernel-only (timeframe.hpp and native_host.hpp): the aggregator's own
// completions, fed with and without the next input bar, then the same week
// delivered through a bare host's "W" subscription. Synthetic prices on the
// real NSE clock (Asia/Kolkata, 0915-1530); the controls are a regular week,
// the Friday month-end before the Sunday, a Saturday session and the daily
// bucket, each completing exactly where it did before the lane.

#include <pineforge/bar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
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

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kQuarter = 15 * kMinute;
const std::string kTz = "Asia/Kolkata";
const std::string kSession = "0915-1530";

// Unix ms of an IST civil date-time (Hinnant's days_from_civil; IST is a
// fixed UTC+05:30).
std::int64_t ist_ms(int y, int m, int d, int h, int mi) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000
        - (5 * 3600 + 30 * 60) * 1000LL;
}

struct Day { int y, m, d; };

// A full NSE session (09:15 .. 15:15, 25 quarter-hour bars) per day, with
// distinct prices so every aggregate is checkable.
std::vector<Bar> sessions(const std::vector<Day>& days) {
    std::vector<Bar> bars;
    int k = 0;
    for (const Day& day : days) {
        const std::int64_t open = ist_ms(day.y, day.m, day.d, 9, 15);
        for (int i = 0; i < 25; ++i, ++k) {
            Bar bar{};
            bar.timestamp = open + i * kQuarter;
            bar.open = 1000.0 + k;
            bar.high = bar.open + 3.0 + (k % 7);
            bar.low = bar.open - 2.0 - (k % 5);
            bar.close = bar.open + 0.5;
            bar.volume = 1.0 + k;
            bars.push_back(bar);
        }
    }
    return bars;
}

std::vector<Bar> dailies(const std::vector<Day>& days) {
    std::vector<Bar> bars;
    int k = 0;
    for (const Day& day : days) {
        Bar bar{};
        bar.timestamp = ist_ms(day.y, day.m, day.d, 9, 15);
        bar.open = 2000.0 + k;
        bar.high = bar.open + 11.0;
        bar.low = bar.open - 9.0;
        bar.close = bar.open + 1.0;
        bar.volume = 10.0 + k;
        bars.push_back(bar);
        ++k;
    }
    return bars;
}

struct Completion {
    std::size_t at = 0;   // index of the input bar that completed the bucket
    Bar bar{};
    int subs = 0;
};

// Feeds every bar, handing the next input bar's timestamp when `with_next`
// (a historical run holds its whole feed; 0 = unknown, the realtime form).
std::vector<Completion> feed_all(TimeframeAggregator& agg, const std::vector<Bar>& bars,
                                 bool with_next) {
    std::vector<Completion> out;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const std::int64_t next =
            with_next && i + 1 < bars.size() ? bars[i + 1].timestamp : 0;
        const AggregatedBar ab = agg.feed(bars[i], next);
        if (ab.is_complete) out.push_back({i, ab.bar, ab.sub_bar_count});
    }
    return out;
}

bool same(double a, double b) { return std::abs(a - b) <= 1e-9; }

// The hand aggregate of bars[from, to): the oracle.
Bar aggregate(const std::vector<Bar>& bars, std::size_t from, std::size_t to,
              std::int64_t label) {
    Bar out = bars[from];
    out.timestamp = label;
    for (std::size_t i = from + 1; i < to; ++i) {
        out.high = std::max(out.high, bars[i].high);
        out.low = std::min(out.low, bars[i].low);
        out.close = bars[i].close;
        out.volume += bars[i].volume;
    }
    return out;
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && same(a.open, b.open) && same(a.high, b.high)
        && same(a.low, b.low) && same(a.close, b.close) && same(a.volume, b.volume);
}

// Fri 01-23 (the week before), the Budget week -- Mon 01-26 is Republic Day,
// so Tue 01-27 .. Fri 01-30 and Sun 02-01 -- then Mon 02-02 and Tue 02-03.
const std::vector<Day> kBudgetWeek2026 = {
    {2026, 1, 23}, {2026, 1, 27}, {2026, 1, 28}, {2026, 1, 29}, {2026, 1, 30},
    {2026, 2, 1}, {2026, 2, 2}, {2026, 2, 3}};

// ---- 1. the Budget week, the next input bar known -------------------------

void test_weekend_session_week_with_next_bar() {
    scenario = "W, Sunday session, next bar known";
    const std::vector<Bar> bars = sessions(kBudgetWeek2026);
    TimeframeAggregator week("W", "15", kTz, kSession);
    const std::vector<Completion> done = feed_all(week, bars, true);
    // The week before completes on its Friday's last bar; the Budget week once,
    // on Sunday's 15:15 bar, holding its 125 bars; the running week is open
    // when the feed ends on Tuesday.
    CHECK(done.size() == 2);
    if (done.size() != 2) {
        std::printf("  completions: %zu\n", done.size());
        return;
    }
    CHECK(done[0].at == 24);
    const std::size_t sunday_last = 5 * 25 + 24;
    CHECK(done[1].at == sunday_last);
    CHECK(bars[done[1].at].timestamp == ist_ms(2026, 2, 1, 15, 15));
    CHECK(done[1].subs == 125);
    CHECK(same_bar(done[1].bar, aggregate(bars, 25, 150, ist_ms(2026, 1, 27, 9, 15))));
}

// ---- 2. the same week with no next bar (a realtime feed) ------------------

void test_weekend_session_week_without_next_bar() {
    scenario = "W, Sunday session, next bar unknown";
    const std::vector<Bar> bars = sessions(kBudgetWeek2026);
    TimeframeAggregator week("W", "15", kTz, kSession);
    const std::vector<Completion> done = feed_all(week, bars, false);
    // Without the next bar the template's rule decides, once: the week
    // completes on Friday's 15:15 bar (100 bars), and the Sunday session
    // merges into the closed bucket without completing it again. The next
    // week's first bar opens a new bucket and re-emits nothing.
    CHECK(done.size() == 2);
    if (done.size() != 2) {
        std::printf("  completions: %zu\n", done.size());
        return;
    }
    CHECK(done[1].at == 4 * 25 + 24);
    CHECK(bars[done[1].at].timestamp == ist_ms(2026, 1, 30, 15, 15));
    CHECK(done[1].subs == 100);
    CHECK(same_bar(done[1].bar, aggregate(bars, 25, 125, ist_ms(2026, 1, 27, 9, 15))));
    CHECK(same_bar(week.last_completed(), done[1].bar));
    // The running bucket is the next week's (Mon 02-02 opened it).
    CHECK(week.current().timestamp == ist_ms(2026, 2, 2, 9, 15));
}

// ---- 3. a weekday-only week is unchanged ------------------------------------

void test_regular_week_control() {
    scenario = "W, regular week";
    const std::vector<Bar> bars = sessions({{2025, 6, 13}, {2025, 6, 16}, {2025, 6, 17},
                                            {2025, 6, 18}, {2025, 6, 19}, {2025, 6, 20},
                                            {2025, 6, 23}});
    for (bool with_next : {true, false}) {
        TimeframeAggregator week("W", "15", kTz, kSession);
        const std::vector<Completion> done = feed_all(week, bars, with_next);
        CHECK(done.size() == 2);
        if (done.size() != 2) continue;
        CHECK(done[0].at == 24);
        CHECK(done[1].at == 5 * 25 + 24);
        CHECK(done[1].subs == 125);
    }
}

// ---- 4. the month that ends on the Friday before the Sunday ----------------

void test_month_ends_on_friday_before_sunday() {
    scenario = "M, January 2026";
    const std::vector<Bar> bars = sessions(kBudgetWeek2026);
    TimeframeAggregator month("M", "15", kTz, kSession);
    const std::vector<Completion> done = feed_all(month, bars, true);
    // January's last session is Fri 01-30: the next known bar (Sun 02-01) is
    // February's, so January completes there, exactly once.
    CHECK(done.size() == 1);
    if (done.size() != 1) return;
    CHECK(done[0].at == 4 * 25 + 24);
    CHECK(bars[done[0].at].timestamp == ist_ms(2026, 1, 30, 15, 15));
}

// ---- 5. the daily bucket of a weekend session ------------------------------

void test_daily_buckets_of_weekend_session() {
    scenario = "D, Sunday session";
    const std::vector<Bar> bars = sessions(kBudgetWeek2026);
    for (bool with_next : {true, false}) {
        TimeframeAggregator day("D", "15", kTz, kSession);
        const std::vector<Completion> done = feed_all(day, bars, with_next);
        // One completion per session, on its 15:15 bar, Sunday's included.
        CHECK(done.size() == kBudgetWeek2026.size());
        for (std::size_t k = 0; k < done.size(); ++k) {
            CHECK(done[k].at == k * 25 + 24);
            CHECK(done[k].subs == 25);
        }
    }
}

// ---- 6. a Saturday session ---------------------------------------------------

void test_saturday_session_week() {
    scenario = "W, Saturday session";
    // Budget day 2025: Sat 02-01 traded a full session.
    const std::vector<Bar> bars = sessions({{2025, 1, 27}, {2025, 1, 28}, {2025, 1, 29},
                                            {2025, 1, 30}, {2025, 1, 31}, {2025, 2, 1},
                                            {2025, 2, 3}});
    TimeframeAggregator week("W", "15", kTz, kSession);
    const std::vector<Completion> done = feed_all(week, bars, true);
    CHECK(done.size() == 1);
    if (done.size() != 1) return;
    CHECK(bars[done[0].at].timestamp == ist_ms(2025, 2, 1, 15, 15));
    CHECK(done[0].subs == 150);
}

// ---- 7. a daily input with a Sunday bar ------------------------------------

void test_daily_input_week() {
    scenario = "W from daily bars";
    const std::vector<Bar> days = dailies(kBudgetWeek2026);
    TimeframeAggregator week("W", "D", kTz, kSession);
    const std::vector<Completion> done = feed_all(week, days, true);
    CHECK(done.size() == 2);
    if (done.size() != 2) return;
    // The feed's first bar only opens its week, which the next week's first
    // bar then closes; the Budget week completes on the Sunday daily bar.
    CHECK(done[0].at == 1);
    CHECK(done[1].at == 5);
    CHECK(done[1].subs == 5);
    CHECK(same_bar(done[1].bar, aggregate(days, 1, 6, ist_ms(2026, 1, 27, 9, 15))));
}

// ---- 8. a bare host's "W" series delivers the week once --------------------

class WeekHost final : public NativeStrategyHost {
public:
    std::vector<Bar> weeks;
    std::vector<int> delivered_before;   // script bars calculated before each
    int bars_seen = 0;
    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext&) override {
        weeks.push_back(bar);
        delivered_before.push_back(bars_seen);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars_seen; }
};

void test_bare_host_week_series() {
    scenario = "bare host W series";
    const std::vector<Bar> bars = sessions(kBudgetWeek2026);
    NativeRunSpec spec;
    spec.identity = {"w15-close-once", 1};
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "NIFTY";
    spec.tickerid = "TEST:NIFTY";
    spec.type = "index";
    spec.currency = "INR";
    spec.basecurrency = "INR";
    spec.description = "weekend session week";
    spec.volumetype = "base";
    spec.timezone = kTz;
    spec.session = kSession;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.05;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    NativeTimeframeSubscription weekly;
    weekly.tf = "W";
    spec.subscriptions.push_back(weekly);

    WeekHost host;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    // Two weeks delivered: the one before, and the Budget week on Sunday's
    // last bar -- the 150th script bar -- with every bar of the week in it.
    CHECK(host.weeks.size() == 2);
    if (host.weeks.size() != 2) {
        std::printf("  deliveries: %zu\n", host.weeks.size());
        return;
    }
    CHECK(same_bar(host.weeks[1], aggregate(bars, 25, 150, ist_ms(2026, 1, 27, 9, 15))));
    CHECK(host.delivered_before[1] == 149);
}

}  // namespace

int main() {
    test_weekend_session_week_with_next_bar();
    test_weekend_session_week_without_next_bar();
    test_regular_week_control();
    test_month_ends_on_friday_before_sunday();
    test_daily_buckets_of_weekend_session();
    test_saturday_session_week();
    test_daily_input_week();
    test_bare_host_week_series();
    std::printf("calendar bucket closes once: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
