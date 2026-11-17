// A weekly or monthly request.security on a daily chart closes on the
// period's last traded daily bar.
//
// Target (one): the chart bar on which a single-period calendar request (W,
// M) fed by daily bars hands its completed bucket to the script, and the daily
// bars that bucket holds, on a session that does not trade at the weekend.
//
// TradingView closes such a bucket on the last daily bar of the week or the
// month. On OANDA's forex and metals symbols the week's last daily bar is
// Friday's (stamped Thursday 17:00 New York): a script that compares the
// close with the weekly close signals on that bar and fills at Sunday's 17:00
// open. The session strings of those symbols ("1700-1700", "1800-1700") name
// no weekdays, so a calendar that reads the string alone takes Saturday and
// Sunday for trading days, keeps the week open after Friday and closes it
// only when Monday's bar arrives. The request then reaches the script one
// daily bar late, every week, and every month that ends on a weekend.
//
// The run is the source host on a 1D chart with the two request sites such a
// script has (request.security(syminfo.tickerid, "W" or "M", ...), lookahead
// off, gaps off), registered the way generated code registers them. Three
// daily tapes:
//   forex   a 17:00-to-17:00 New York day, type forex;
//   metals  an 18:00-to-17:00 New York day, type cfd;
//           both with one bar per trading date Monday to Friday, stamped
//           17:00 on the evening before it, over both clock changes and two
//           year ends, with two midweek holidays a year and one Monday
//           holiday missing;
//   allday  an all-day UTC tape that trades every day, where the week's last
//           daily bar is Sunday's.
//
// The expected values are computed here from each tape's trading dates by
// civil arithmetic alone (Monday-based weeks, calendar months): for every
// period one completed bucket, handed over on the chart bar of the period's
// last trading weekday, holding the period's first open, highest high, lowest
// low, last close and summed volume. Nothing is read back from the engine to
// form them. A period whose last trading weekday has no bar is outside the
// rule stated here; the tapes hold none, and the test checks that.
//
// Every bucket is printed with the chart bar it was expected on and the one it
// arrived on, and each site ends with a summary line:
//   summary tape=<name> site=<W|M> expected=.. delivered=.. on_time=..
//           one_bar_late=.. other=.. missing=.. extra=..
// one_bar_late counts buckets that hold the right bars and arrived on the
// next chart bar.

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
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

struct Tape {
    std::string name;
    std::string timezone;
    std::string session;
    std::string type;
    // The weekdays that trade: bit 0 Sunday .. bit 6 Saturday.
    unsigned weekdays = 0;
    std::vector<Bar> bars;
    // Each bar's trading date, in days from 1970-01-01.
    std::vector<std::int64_t> trading_days;
};

// 0 Sunday .. 6 Saturday; 1970-01-01 was a Thursday. Every date here is after it.
int weekday_of(std::int64_t day) {
    return static_cast<int>((day + 4) % 7);
}

// The Monday-based week a date falls in: Monday 1969-12-29 opens week 0.
std::int64_t week_of(std::int64_t day) {
    return (day + 3) / 7;
}

std::int64_t month_of(std::int64_t day) {
    const nc::NativeCivilDate date = nc::native_civil_date(day);
    return date.year * 12 + (date.month - 1);
}

// The first trading weekday after `day`.
std::int64_t next_trading_day(const Tape& tape, std::int64_t day) {
    std::int64_t next = day + 1;
    for (int guard = 0; guard < 7 && (tape.weekdays & (1u << weekday_of(next))) == 0; ++guard) {
        ++next;
    }
    return next;
}

struct Expected {
    long long chart_index = 0;
    Bar bar{};
};

// One completed bucket per period, on the bar of the period's last trading
// weekday. The period the tape leaves before that weekday closes nothing.
std::vector<Expected> expected_periods(const Tape& tape, std::int64_t (*period_of)(std::int64_t)) {
    std::vector<Expected> out;
    std::size_t first = 0;
    for (std::size_t i = 0; i < tape.bars.size(); ++i) {
        const std::int64_t day = tape.trading_days[i];
        const bool last_bar_of_period = i + 1 == tape.bars.size()
            || period_of(tape.trading_days[i + 1]) != period_of(day);
        if (!last_bar_of_period) continue;
        const bool period_over = period_of(next_trading_day(tape, day)) != period_of(day);
        // Inside the tape a period ends on its last trading weekday: no
        // holiday of these tapes removes one.
        if (i + 1 < tape.bars.size()) CHECK(period_over);
        if (!period_over) break;
        Expected expected;
        expected.chart_index = static_cast<long long>(i);
        expected.bar = tape.bars[first];
        for (std::size_t j = first + 1; j <= i; ++j) {
            expected.bar.high = std::max(expected.bar.high, tape.bars[j].high);
            expected.bar.low = std::min(expected.bar.low, tape.bars[j].low);
            expected.bar.close = tape.bars[j].close;
            expected.bar.volume += tape.bars[j].volume;
        }
        out.push_back(expected);
        first = i + 1;
    }
    return out;
}

// The two request sites of a script that reads the weekly and the monthly bar
// on its chart symbol, as generated code registers them. Site 0 is "W", site
// 1 is "M"; both lookahead off, gaps off.
class RequestHost final : public source::PineStrategyHost {
public:
    struct Delivery {
        long long chart_index = 0;
        bool complete = false;
        Bar bar{};
    };
    std::vector<Delivery> delivered[2];
    long long bars_seen = 0;
    long long other_sites = 0;
    long long cleared = 0;

private:
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "W", input_tf_, false, false);
        register_security_eval(1, "M", input_tf_, false, false);
    }

    // A request's bucket arrives before the body of the chart bar it belongs
    // to: bars_seen is that chart bar's index.
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 && sec_id != 1) {
            ++other_sites;
            return;
        }
        Delivery delivery;
        delivery.chart_index = bars_seen;
        delivery.complete = is_complete;
        delivery.bar = bar;
        delivered[sec_id].push_back(delivery);
    }

    void clear_security(int) override { ++cleared; }

    void on_source_bar(const Bar&) override { ++bars_seen; }
};

bool same_values(const Bar& a, const Bar& b) {
    return a.open == b.open && a.high == b.high && a.low == b.low && a.close == b.close
        && a.volume == b.volume;
}

void print_values(const Bar& bar) {
    std::printf("o=%.17g h=%.17g l=%.17g c=%.17g v=%.17g", bar.open, bar.high, bar.low,
                bar.close, bar.volume);
}

void compare(const Tape& tape, const char* site, const std::vector<Expected>& expected,
             const std::vector<RequestHost::Delivery>& delivered) {
    long long on_time = 0;
    long long one_bar_late = 0;
    long long other = 0;
    const std::size_t shared = std::min(expected.size(), delivered.size());
    for (std::size_t k = 0; k < shared; ++k) {
        const Expected& want = expected[k];
        const RequestHost::Delivery& got = delivered[k];
        const bool same = got.complete && same_values(got.bar, want.bar);
        const char* verdict = "other";
        if (same && got.chart_index == want.chart_index) {
            ++on_time;
            verdict = "on_time";
        } else if (same && got.chart_index == want.chart_index + 1) {
            ++one_bar_late;
            verdict = "one_bar_late";
        } else {
            ++other;
        }
        std::printf("bucket tape=%s site=%s n=%zu expected_bar=%lld delivered_bar=%lld "
                    "complete=%d label=%lld ", tape.name.c_str(), site, k, want.chart_index,
                    got.chart_index, got.complete ? 1 : 0,
                    static_cast<long long>(got.bar.timestamp));
        print_values(got.bar);
        std::printf(" %s\n", verdict);
        if (!same) {
            std::printf("  expected values: ");
            print_values(want.bar);
            std::printf("\n");
        }
    }
    const long long missing = static_cast<long long>(expected.size() - shared);
    const long long extra = static_cast<long long>(delivered.size() - shared);
    std::printf("summary tape=%s site=%s expected=%zu delivered=%zu on_time=%lld "
                "one_bar_late=%lld other=%lld missing=%lld extra=%lld\n", tape.name.c_str(), site,
                expected.size(), delivered.size(), on_time, one_bar_late, other, missing, extra);
    // Not vacuous: the tape holds whole periods.
    CHECK(expected.size() >= 12);
    CHECK(delivered.size() == expected.size());
    CHECK(on_time == static_cast<long long>(expected.size()));
}

void run_tape(const Tape& tape) {
    std::printf("tape %s: %zu daily bars, session %s, timezone %s, type %s\n", tape.name.c_str(),
                tape.bars.size(), tape.session.c_str(), tape.timezone.c_str(), tape.type.c_str());
    CHECK(tape.weekdays != 0);
    CHECK(tape.bars.size() >= 300);
    CHECK(tape.bars.size() == tape.trading_days.size());
    if (tape.weekdays == 0 || tape.bars.empty() || tape.bars.size() != tape.trading_days.size()) {
        return;
    }
    const std::vector<Expected> weeks = expected_periods(tape, week_of);
    const std::vector<Expected> months = expected_periods(tape, month_of);

    RequestHost host;
    host.set_syminfo_timezone(tape.timezone);
    host.set_syminfo_session(tape.session);
    host.set_syminfo_type(tape.type);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()), "1D", "1D");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  run failed: %s\n", host.last_error().c_str());
    }
    CHECK(host.bars_seen == static_cast<long long>(tape.bars.size()));
    CHECK(host.other_sites == 0);
    CHECK(host.cleared == 0);
    compare(tape, "W", weeks, host.delivered[0]);
    compare(tape, "M", months, host.delivered[1]);
}

// A deterministic price walk; next() is in [-1, 1].
struct Walk {
    std::uint64_t state;
    double next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state % 20001u) / 10000.0 - 1.0;
    }
};

std::int64_t civil(int year, int month, int day) {
    return nc::native_civil_days(year, month, day);
}

// One daily bar per trading date in [first, last] that is a trading weekday
// and no holiday, stamped at hour:00 local time on the civil date
// `stamp_offset` days from the trading date.
Tape make_tape(const char* name, const char* timezone, const char* session, const char* type,
               unsigned weekdays, std::int64_t first, std::int64_t last,
               const std::vector<std::int64_t>& holidays, int stamp_offset, int hour,
               double start_price, std::uint64_t seed) {
    Tape tape;
    tape.name = name;
    tape.timezone = timezone;
    tape.session = session;
    tape.type = type;
    tape.weekdays = weekdays;
    Walk walk{seed};
    double price = start_price;
    for (std::int64_t day = first; day <= last; ++day) {
        if ((weekdays & (1u << weekday_of(day))) == 0) continue;
        if (std::find(holidays.begin(), holidays.end(), day) != holidays.end()) continue;
        const nc::NativeCivilDate stamp = nc::native_civil_date(day + stamp_offset);
        const auto at = nc::resolve_civil(timezone, static_cast<int>(stamp.year), stamp.month,
                                          stamp.day, hour, 0);
        CHECK(at.has_value());
        if (!at) continue;
        const double open = price;
        const double close = open * (1.0 + 0.012 * walk.next());
        const double high = std::max(open, close) * (1.0 + 0.002 * std::fabs(walk.next()));
        const double low = std::min(open, close) * (1.0 - 0.002 * std::fabs(walk.next()));
        const double volume = 1000.0 + static_cast<double>(tape.bars.size() % 17);
        tape.bars.push_back(Bar{open, high, low, close, volume, at->epoch_ms});
        tape.trading_days.push_back(day);
        price = close;
    }
    return tape;
}

std::vector<Tape> make_tapes() {
    // Monday 2024-10-07 to Friday 2026-02-27: the last bar ends a week and,
    // February 2026 ending on a Saturday, a month.
    const std::int64_t first = civil(2024, 10, 7);
    const std::int64_t last = civil(2026, 2, 27);
    // No bar on these trading dates: four midweek days and one Monday. None is
    // a Friday or a month's last weekday.
    const std::vector<std::int64_t> holidays = {
        civil(2024, 12, 25), civil(2025, 1, 1), civil(2025, 5, 26), civil(2025, 12, 25),
        civil(2026, 1, 1)};
    const unsigned monday_to_friday = 0x3Eu;
    const unsigned every_day = 0x7Fu;
    std::vector<Tape> tapes;
    tapes.push_back(make_tape("forex", "America/New_York", "1700-1700", "forex",
                              monday_to_friday, first, last, holidays, -1, 17, 1.1,
                              0x9E3779B97F4A7C15ULL));
    tapes.push_back(make_tape("metals", "America/New_York", "1800-1700", "cfd",
                              monday_to_friday, first, last, holidays, -1, 17, 2000.0,
                              0xD1B54A32D192ED03ULL));
    // Monday 2024-10-07 to Sunday 2025-11-30, the last day of its week and month.
    tapes.push_back(make_tape("allday", "UTC", "24x7", "crypto", every_day, first,
                              civil(2025, 11, 30), {}, 0, 0, 100.0, 0x94D049BB133111EBULL));
    return tapes;
}

}  // namespace

int main() {
    const std::vector<Tape> tapes = make_tapes();
    CHECK(tapes.size() == 3);
    for (const Tape& tape : tapes) run_tape(tape);
    std::printf("test_calendar_request_last_traded_bar: %zu tapes, %lld checks, %d failed\n",
                tapes.size(), checks, failures);
    return failures == 0 ? 0 : 1;
}
