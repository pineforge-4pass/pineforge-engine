// The chart bar on which a daily request.security hands over a half day, on a
// session with a break.
//
// Target (one): on a chart built from a lower timeframe, the chart bar at
// which a daily request (request.security(syminfo.tickerid, "D", volume),
// lookahead off, gaps off) first returns the value of a day that stopped at
// the session's break.
//
// The session is 0900-1130,1230-1530 on a 240-minute chart built from
// 15-minute bars: three whole days, then a day that trades the morning only
// (its ten bars, 09:00 to 11:15), then three whole days. The half day is told
// by its missing bars alone; the session text does not know it. The symbol
// kind is the engine's default one, "crypto": for it, as for "forex" and
// "cfd", the engine does not end a day at an early close, so the half day's
// daily bar completes when the next day's first bar arrives.
//
// The half day has one chart bar, the one that opens at 09:00. What the
// engine does, and this test pins:
//   - at that chart bar the request still returns the day before;
//   - at the next day's first chart bar it returns the half day, whole.
// This is the engine's behaviour as released, not a reading of TradingView:
// none is on record for a half day on such a session. Move the pin only with
// one.
//
// The feed is synthetic: the k-th bar of a day has volume 2^k and the day's
// first bar also carries 2^(kDayBit + day), so a daily value says which day
// it is and which bars it sums. The whole days are not compared: only whose
// value a chart bar reads, and the half day's own bars.

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace pineforge;
namespace nc = pineforge::native_calendar;

int failures = 0;
long long checks = 0;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);        \
    }                                                                          \
} while (0)

const char* const kTimezone = "Asia/Tokyo";
const char* const kSession = "0900-1130,1230-1530";

// Seven trading dates, with a weekend between the third and the fourth.
struct Date {
    int year;
    int month;
    int day;
};
constexpr Date kDates[] = {{2025, 6, 4},  {2025, 6, 5},  {2025, 6, 6}, {2025, 6, 9},
                           {2025, 6, 10}, {2025, 6, 11}, {2025, 6, 12}};
constexpr int kDays = static_cast<int>(sizeof(kDates) / sizeof(kDates[0]));
// The day that trades the morning only, and the bars of a morning.
constexpr int kHalfDay = 3;
constexpr int kMorningBars = 10;

// The bit of day 0's marker. A day has fewer bars than this, and the highest
// marker is far below 2^53, so every sum is exact in a double.
constexpr int kDayBit = 32;

std::vector<int> quarter_hours(int from, int to) {
    std::vector<int> out;
    for (int minute = from; minute < to; minute += 15) out.push_back(minute);
    return out;
}

// The one request site: a daily bar of the chart symbol, lookahead off, gaps
// off. Each chart bar keeps what the request returned when its body ran.
class DailyRequestHost final : public source::PineStrategyHost {
public:
    struct ChartBar {
        std::int64_t open_ms = 0;
        // The request's last completed daily volume; -1 before the first.
        double value = -1.0;
    };
    std::vector<ChartBar> chart_bars;
    long long other_sites = 0;
    const std::string& symbol_kind() const { return syminfo_.type; }

private:
    double latest_ = -1.0;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", input_tf_, false, false);
    }

    // A request's value arrives before the body of the chart bar that first
    // reads it.
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0) {
            ++other_sites;
            return;
        }
        if (is_complete) latest_ = bar.volume;
    }

    void clear_security(int) override {}

    void on_source_bar(const Bar&) override {
        ChartBar seen;
        seen.open_ms = current_bar_.timestamp;
        seen.value = latest_;
        chart_bars.push_back(seen);
    }
};

// A volume as the set of bars it sums; false when it is not such a sum.
bool as_bits(double volume, std::uint64_t& bits) {
    if (!(volume >= 0.0) || volume >= 9007199254740992.0 || volume != std::floor(volume)) {
        return false;
    }
    bits = static_cast<std::uint64_t>(volume);
    return true;
}

}  // namespace

int main() {
    const std::vector<int> morning = quarter_hours(9 * 60, 11 * 60 + 30);
    const std::vector<int> afternoon = quarter_hours(12 * 60 + 30, 15 * 60 + 30);
    // Not vacuous: ten morning bars and twelve afternoon bars.
    CHECK(static_cast<int>(morning.size()) == kMorningBars && afternoon.size() == 12);
    CHECK(kHalfDay > 0 && kHalfDay + 1 < kDays);

    std::vector<Bar> bars;
    std::int64_t day_open[kDays] = {};
    for (int day = 0; day < kDays; ++day) {
        std::vector<int> opens = morning;
        if (day != kHalfDay) opens.insert(opens.end(), afternoon.begin(), afternoon.end());
        for (std::size_t k = 0; k < opens.size(); ++k) {
            const auto at = nc::resolve_civil(kTimezone, kDates[day].year, kDates[day].month,
                                              kDates[day].day, opens[k] / 60, opens[k] % 60);
            CHECK(at.has_value());
            if (!at) return 1;
            if (k == 0) day_open[day] = at->epoch_ms;
            double volume = std::ldexp(1.0, static_cast<int>(k));
            if (k == 0) volume += std::ldexp(1.0, kDayBit + day);
            bars.push_back(Bar{100.0, 100.0, 100.0, 100.0, volume, at->epoch_ms});
        }
    }

    DailyRequestHost host;
    host.set_syminfo_timezone(kTimezone);
    host.set_syminfo_session(kSession);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "240");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  run failed: %s\n", host.last_error().c_str());
    }
    CHECK(host.other_sites == 0);
    // The premise: a symbol kind whose day the engine never closes early.
    CHECK(host.symbol_kind() == "crypto");

    // The half day's one chart bar and the next day's first, by their opens.
    int half_bars = 0;
    int next_bars = 0;
    double at_half = -1.0;
    double at_next = -1.0;
    for (const DailyRequestHost::ChartBar& bar : host.chart_bars) {
        if (bar.open_ms == day_open[kHalfDay]) {
            ++half_bars;
            at_half = bar.value;
        }
        if (bar.open_ms == day_open[kHalfDay + 1]) {
            ++next_bars;
            at_next = bar.value;
        }
    }
    CHECK(half_bars == 1);
    CHECK(next_bars == 1);

    const std::uint64_t morning_bits = (std::uint64_t{1} << kMorningBars) - 1;
    const std::uint64_t half_value = morning_bits | (std::uint64_t{1} << (kDayBit + kHalfDay));
    std::uint64_t half_bits = 0;
    std::uint64_t next_bits = 0;
    const bool half_read = as_bits(at_half, half_bits);
    const bool next_read = as_bits(at_next, next_bits);
    std::printf("half day's chart bar: value %s, days 0x%llx, bars 0x%llx\n",
                half_read ? "read" : "none",
                static_cast<unsigned long long>(half_bits >> kDayBit),
                static_cast<unsigned long long>(half_bits & ((std::uint64_t{1} << kDayBit) - 1)));
    std::printf("next day's first chart bar: value %s, days 0x%llx, bars 0x%llx\n",
                next_read ? "read" : "none",
                static_cast<unsigned long long>(next_bits >> kDayBit),
                static_cast<unsigned long long>(next_bits & ((std::uint64_t{1} << kDayBit) - 1)));

    // At the half day's own chart bar the request still returns the day before.
    CHECK(half_read && (half_bits >> kDayBit) == (std::uint64_t{1} << (kHalfDay - 1)));
    // At the next day's first chart bar it returns the half day: its ten
    // morning bars and no other.
    CHECK(next_read && next_bits == half_value);

    std::printf("test_split_session_half_day_request: %zu chart bars, %lld checks, %d failed\n",
                host.chart_bars.size(), checks, failures);
    return failures == 0 ? 0 : 1;
}
