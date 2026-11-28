// test_requested_ema_first_chart_bar -- a ta.ema inside request.security warms
// up from the first bar of the requested series.
//
// On a run with a request feed attached, the adapter starts a requested series
// whose timeframe is the chart's or coarser at the run's first chart bar: the
// feed's earlier bars are never fed, and a requested bar already in progress
// there is absent. Such a series has no bar before its first one, so a
// ta.ema(x, length) inside the request follows TradingView's rule for the
// first bars of a series: na for length - 1 requested bars, then the simple
// average of the first length values, then the recursion. Seeding it with the
// first requested bar's value answers a number where TradingView answers na.
//
// The script each row stands for, on a 15-minute chart with a 1-minute
// request feed:
//   tf = input.timeframe("60")            // "240" on the 24x7 row
//   c  = request.security(syminfo.tickerid, tf, close,
//                         lookahead = barmerge.lookahead_off)
//   e  = request.security(syminfo.tickerid, tf, ta.ema(close, 3),
//                         lookahead = barmerge.lookahead_off)
//
// Invented bars. Every minute of one requested bar carries one price, so the
// requested close is that price; the request feed also holds an earlier
// session (or day) at another price, which the requested series never sees.
//
// Rows, one per market type:
//   session-bound   0930-1600 America/New_York, type stock, "60"
//   CFD             1800-1700 America/New_York, type cfd, "60"
//   24x7            UTC, type crypto, "240"
// The CFD session trades 23 whole hours a day, so its daily break and its
// weekend close move no requested bar: they only leave hours without one.
// That row therefore reads 30 requested bars, across one daily break and the
// weekend close, where the other two read seven.
// The rule counts requested bars, so it is the same with lookahead on and has
// no row of its own.

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/na.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond, row, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s: %s (line %d)\n", (row), (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

constexpr std::int64_t kMinute = 60'000;

// Unix ms of a UTC civil date-time (days_from_civil).
std::int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

bool same_value(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return a == b;
}

// One lookahead_off evaluator on the requested timeframe, registered at run
// time as a generated strategy registers an input-bound timeframe. It reads
// every completed requested bar once: its open, its close and
// ta.ema(close, 3).
class RequestedEmaHarness : public pineforge::source::PineStrategyHost {
public:
    explicit RequestedEmaHarness(const char* tf) : tf_(tf) {}

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, tf_, input_tf_, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) return;
        if (!opens.empty() && opens.back() == bar.timestamp) return;
        opens.push_back(bar.timestamp);
        closes.push_back(bar.close);
        emas.push_back(ema_.compute(bar.close));
    }
    void on_source_bar(const Bar&) override {}

    std::vector<std::int64_t> opens;
    std::vector<double> closes;
    std::vector<double> emas;

private:
    std::string tf_;
    ta::EMA ema_{3};
};

void run_row(RequestedEmaHarness& h, const char* row, const std::vector<Bar>& chart,
             const std::vector<Bar>& request_feed, const char* tz, const char* session,
             const char* type) {
    strategy_set_syminfo_timezone(static_cast<pf_strategy_t>(&h), tz);
    strategy_set_syminfo_session(static_cast<pf_strategy_t>(&h), session);
    strategy_set_syminfo_type(static_cast<pf_strategy_t>(&h), type);
    CHECK(strategy_set_aux_security_feed(
              static_cast<pf_strategy_t>(&h),
              reinterpret_cast<const pf_bar_t*>(request_feed.data()),
              static_cast<int>(request_feed.size()), "1") == 0,
          row, "the 1-minute request feed is attached");
    h.run(chart.data(), static_cast<int>(chart.size()), "15", "15", false, 4,
          MagnifierDistribution::ENDPOINTS);
    CHECK(h.last_error().empty(), row, "the run succeeds");
    if (!h.last_error().empty()) std::printf("   engine error: %s\n", h.last_error().c_str());
}

// Requested closes 10, 20, 30, ... give ta.ema(close, 3):
//   bar 1, bar 2   na
//   bar 3          (10 + 20 + 30) / 3 = 20
//   bar 4 ..       alpha = 2 / (3 + 1) = 0.5: 30, 40, 50, 60
// The first-value seed gives 10, 15, 22.5, 31.25, ... instead.
void check_warm_up(const RequestedEmaHarness& h, const char* row) {
    const double na_ = na<double>();
    const double want_close[7] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
    const double want_ema[7] = {na_, na_, 20.0, 30.0, 40.0, 50.0, 60.0};
    CHECK(h.closes.size() >= 7, row, "seven requested bars complete");
    for (std::size_t i = 0; i < 7 && i < h.closes.size(); ++i) {
        // The first requested close is the run's own, never the 999 of the
        // feed bars before the run's first chart bar.
        CHECK(same_value(h.closes[i], want_close[i]), row,
              "the requested series starts at the run's first chart bar");
        if (!same_value(h.emas[i], want_ema[i])) {
            std::printf("FAIL: %s: requested bar %zu: ta.ema(close, 3) reads %g, want %g "
                        "(na for 2 requested bars, then their simple average)\n",
                        row, i + 1, h.emas[i], want_ema[i]);
            ++failures;
        }
    }
}

// The 0930-1600 New York session is 13:30Z-20:00Z in June. One price per hour
// counted from the session open (the session's seventh hour is 30 minutes
// long): 10 .. 70 on Mon 2025-06-02, 80 .. 140 on Tue 06-03. The request feed
// also holds Fri 05-30, before the run's first chart bar, at 999.
std::vector<Bar> session_bars(std::int64_t step_ms, bool with_earlier_session) {
    const int month[3] = {5, 6, 6};
    const int day[3] = {30, 2, 3};
    std::vector<Bar> bars;
    for (int s = with_earlier_session ? 0 : 1; s < 3; ++s) {
        const std::int64_t open = utc_ms(2025, month[s], day[s], 13, 30);
        for (std::int64_t t = open; t < open + 390 * kMinute; t += step_ms) {
            const int hour = static_cast<int>((t - open) / (60 * kMinute));
            const double px = s == 0 ? 999.0 : 10.0 * (1 + (s - 1) * 7 + hour);
            bars.push_back(Bar{px, px, px, px, 1.0, t});
        }
    }
    return bars;
}

void test_session_bound_60() {
    const char* row = "session-bound 60";
    const int before = failures;
    const auto chart = session_bars(15 * kMinute, false);
    const auto request_feed = session_bars(kMinute, true);
    RequestedEmaHarness h("60");
    run_row(h, row, chart, request_feed, "America/New_York", "0930-1600", "stock");
    check_warm_up(h, row);
    std::printf("test_session_bound_60: %s\n", failures > before ? "FAIL" : "ok");
}

// The same expectation over `count` requested bars. Bar n closes at 10 * n.
// ta.ema(close, 3) reads na on bars 1 and 2 and 20 on bar 3; from there, with
// alpha = 0.5 and closes that rise by 10, each bar reads
//   10 * (n - 2) + 0.5 * (10 * n - 10 * (n - 2)) = 10 * (n - 1),
// every value an exact double. The first-value seed reads
// 10 * (n - 1) + 10 / 2^(n - 1) instead: 10, 15, 22.5, 31.25, ...
void check_warm_up_run(const RequestedEmaHarness& h, const char* row, std::size_t count) {
    CHECK(h.closes.size() >= count, row, "the requested bars complete");
    for (std::size_t i = 0; i < count && i < h.closes.size(); ++i) {
        const double want_close = 10.0 * static_cast<double>(i + 1);
        const double want_ema = i < 2 ? na<double>() : 10.0 * static_cast<double>(i);
        // One requested bar per traded hour, none for a closed hour, and the
        // first one is the run's own, never the 999 of the earlier feed bars.
        CHECK(same_value(h.closes[i], want_close), row,
              "one requested bar per traded hour from the run's first chart bar");
        if (!same_value(h.emas[i], want_ema)) {
            std::printf("FAIL: %s: requested bar %zu: ta.ema(close, 3) reads %.17g, want %g "
                        "(na for 2 requested bars, then their simple average)\n",
                        row, i + 1, h.emas[i], want_ema);
            ++failures;
        }
    }
}

// The 1800-1700 New York session in June: a trading day runs 22:00Z to
// 21:00Z, the hour from 21:00Z is the daily break, and nothing trades from
// Fri 2025-06-06 21:00Z to Sun 06-08 22:00Z.
bool cfd_trades(std::int64_t hour_open) {
    if ((hour_open / (60 * kMinute)) % 24 == 21) return false;
    return hour_open < utc_ms(2025, 6, 6, 21) || hour_open >= utc_ms(2025, 6, 8, 22);
}

// One price per traded hour counted from the run's first chart bar, Thu
// 2025-06-05 14:00 New York (18:00Z), three hours before a daily break:
//   bars  1 ..  3   Thu 18:00Z .. 20:00Z          10 ..  30
//                   daily break, 21:00Z
//   bars  4 .. 26   Thu 22:00Z .. Fri 20:00Z       40 .. 260
//                   weekend close
//   bars 27 .. 32   Sun 22:00Z .. Mon 03:00Z      270 .. 320
// The request feed also holds the 23 traded hours before the first chart bar
// (Wed 18:00Z .. Thu 17:00Z, with their own daily break) at 999.
std::vector<Bar> cfd_bars(std::int64_t step_ms, bool with_earlier_day) {
    const std::int64_t hour = 60 * kMinute;
    const std::int64_t first = utc_ms(2025, 6, 5, 18);
    const std::int64_t begin = with_earlier_day ? first - 24 * hour : first;
    std::vector<Bar> bars;
    int traded_hours = 0;
    for (std::int64_t open = begin; open < utc_ms(2025, 6, 9, 4); open += hour) {
        if (!cfd_trades(open)) continue;
        const double px = open < first ? 999.0 : 10.0 * ++traded_hours;
        for (std::int64_t t = open; t < open + hour; t += step_ms) {
            bars.push_back(Bar{px, px, px, px, 1.0, t});
        }
    }
    return bars;
}

// The session is 23 whole hours, so every requested "60" bar is a full hour
// on the hour and the break shortens none. What the break and the weekend do
// is leave hours with no requested bar: bar 4 is the first hour after the
// daily break and bar 27 the first after the weekend, and the EMA runs on
// across both as if the bars were consecutive. Thirty bars cover both gaps.
void test_cfd_60() {
    const char* row = "cfd 60";
    const int before = failures;
    const auto chart = cfd_bars(15 * kMinute, false);
    const auto request_feed = cfd_bars(kMinute, true);
    RequestedEmaHarness h("60");
    run_row(h, row, chart, request_feed, "America/New_York", "1800-1700", "cfd");
    check_warm_up_run(h, row, 30);
    std::printf("test_cfd_60: %s\n", failures > before ? "FAIL" : "ok");
}

// 24x7, UTC: one price per four hours counted from Mon 2025-06-02 00:00Z, the
// run's first chart bar: 10 .. 60 that day, 70 .. 120 the next. The request
// feed also holds the day before at 999.
std::vector<Bar> all_day_bars(std::int64_t step_ms, bool with_earlier_day) {
    const std::int64_t first = utc_ms(2025, 6, 2);
    const std::int64_t begin = with_earlier_day ? first - 1440 * kMinute : first;
    std::vector<Bar> bars;
    for (std::int64_t t = begin; t < first + 2 * 1440 * kMinute; t += step_ms) {
        const double px = t < first
            ? 999.0 : 10.0 * (1 + static_cast<int>((t - first) / (240 * kMinute)));
        bars.push_back(Bar{px, px, px, px, 1.0, t});
    }
    return bars;
}

void test_all_day_240() {
    const char* row = "24x7 240";
    const int before = failures;
    const auto chart = all_day_bars(15 * kMinute, false);
    const auto request_feed = all_day_bars(kMinute, true);
    RequestedEmaHarness h("240");
    run_row(h, row, chart, request_feed, "UTC", "24x7", "crypto");
    check_warm_up(h, row);
    std::printf("test_all_day_240: %s\n", failures > before ? "FAIL" : "ok");
}

}  // namespace

int main() {
    test_session_bound_60();
    test_cfd_60();
    test_all_day_240();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_requested_ema_first_chart_bar passed.\n");
    return 0;
}
