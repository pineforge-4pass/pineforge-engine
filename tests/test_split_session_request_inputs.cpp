// Every lower-timeframe bar of a day reaches a daily request.security on a
// chart whose bars run through a session break.
//
// Target (one): the input bars a daily request holds
// (request.security(syminfo.tickerid, "D", volume), lookahead off, gaps off)
// when the chart is built from a lower timeframe and one chart bar holds
// trading on both sides of a break in the session.
//
// The run is the source host on a 240-minute chart built from 15-minute bars,
// with the one request site such a script has, registered the way generated
// code registers it. On the session 0900-1130,1230-1530 the chart bar that
// opens at 09:00 ends at 13:00: it holds the morning and the first two bars
// after the 12:30 reopen.
//
// The feed is synthetic. The k-th bar of a day has volume 2^k, and the day's
// first bar also carries 2^(kDayBit + day), so a daily value is a set of bits
// that says which bars of which day it sums. The expected value of a day is
// the sum over all of its bars; nothing is read back from the engine to form
// it.
//
// Two cases over the same dates:
//   break    0900-1130,1230-1530, 22 bars a day, none inside the break;
//   control  0900-1530, one window, 26 bars a day: no chart bar meets a break.
//
// Every bar a daily value lacks is printed by day, index and time of day,
//   hole case=<name> day=<n> input=<k> open=<HH:MM>
// and each case ends with
//   summary case=<name> days=.. delivered=.. whole=.. holes=..

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

// Five trading dates, with a weekend between the third and the fourth.
struct Date {
    int year;
    int month;
    int day;
};
constexpr Date kDates[] = {{2025, 6, 4}, {2025, 6, 5}, {2025, 6, 6}, {2025, 6, 9}, {2025, 6, 10}};
constexpr int kDays = static_cast<int>(sizeof(kDates) / sizeof(kDates[0]));

// The bit of day 0's marker. A day has fewer bars than this, and the highest
// marker is far below 2^53, so every sum is exact in a double.
constexpr int kDayBit = 32;

struct Case {
    const char* name;
    const char* session;
    // The opens of one day's 15-minute bars, in minutes from midnight.
    std::vector<int> opens;
};

std::vector<int> quarter_hours(int from, int to) {
    std::vector<int> out;
    for (int minute = from; minute < to; minute += 15) out.push_back(minute);
    return out;
}

// The one request site: a daily bar of the chart symbol, lookahead off, gaps
// off. Every value handed to the script is kept, in the order it arrived.
class DailyRequestHost final : public source::PineStrategyHost {
public:
    struct Delivery {
        bool complete = false;
        double volume = 0.0;
    };
    std::vector<Delivery> delivered;
    long long other_sites = 0;

private:
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", input_tf_, false, false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0) {
            ++other_sites;
            return;
        }
        Delivery delivery;
        delivery.complete = is_complete;
        delivery.volume = bar.volume;
        delivered.push_back(delivery);
    }

    void clear_security(int) override {}

    void on_source_bar(const Bar&) override {}
};

// A volume as the set of bars it sums; false when it is not such a sum.
bool as_bits(double volume, std::uint64_t& bits) {
    if (!(volume >= 0.0) || volume >= 9007199254740992.0 || volume != std::floor(volume)) {
        return false;
    }
    bits = static_cast<std::uint64_t>(volume);
    return true;
}

void run_case(const Case& c) {
    const int per_day = static_cast<int>(c.opens.size());
    CHECK(per_day > 0 && per_day < kDayBit);
    if (per_day <= 0 || per_day >= kDayBit) return;
    std::vector<Bar> bars;
    for (int day = 0; day < kDays; ++day) {
        for (int k = 0; k < per_day; ++k) {
            const auto at = nc::resolve_civil(kTimezone, kDates[day].year, kDates[day].month,
                                              kDates[day].day, c.opens[k] / 60, c.opens[k] % 60);
            CHECK(at.has_value());
            if (!at) return;
            double volume = std::ldexp(1.0, k);
            if (k == 0) volume += std::ldexp(1.0, kDayBit + day);
            bars.push_back(Bar{100.0, 100.0, 100.0, 100.0, volume, at->epoch_ms});
        }
    }

    DailyRequestHost host;
    host.set_syminfo_timezone(kTimezone);
    host.set_syminfo_session(c.session);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "240");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  run failed: %s\n", host.last_error().c_str());
    }
    CHECK(host.other_sites == 0);
    // One daily value a day: each day's last bar ends at the session's close.
    CHECK(host.delivered.size() == static_cast<std::size_t>(kDays));

    const std::uint64_t day_bars = (std::uint64_t{1} << per_day) - 1;
    int whole = 0;
    int holes = 0;
    for (int day = 0; day < kDays; ++day) {
        const std::uint64_t marker = std::uint64_t{1} << (kDayBit + day);
        const std::uint64_t want = day_bars | marker;
        // The day's value: the one completed value that carries this day's
        // marker and no other day's. None, or more than one, reads as empty.
        std::uint64_t got = 0;
        int carriers = 0;
        for (const DailyRequestHost::Delivery& delivery : host.delivered) {
            std::uint64_t bits = 0;
            if (!delivery.complete || !as_bits(delivery.volume, bits)) continue;
            if ((bits & ~day_bars) != marker) continue;
            ++carriers;
            got = bits;
        }
        if (carriers != 1) {
            got = 0;
            holes += per_day;
            std::printf("hole case=%s day=%d: %d completed values carry its marker\n", c.name,
                        day, carriers);
        } else {
            for (int k = 0; k < per_day; ++k) {
                if (((got >> k) & 1U) != 0) continue;
                ++holes;
                std::printf("hole case=%s day=%d input=%d open=%02d:%02d\n", c.name, day, k,
                            c.opens[k] / 60, c.opens[k] % 60);
            }
        }
        // Every bar of the day, and nothing else, is in the day's value.
        CHECK(got == want);
        whole += got == want ? 1 : 0;
    }
    std::printf("summary case=%s days=%d delivered=%zu whole=%d holes=%d\n", c.name, kDays,
                host.delivered.size(), whole, holes);
}

}  // namespace

int main() {
    std::vector<int> split = quarter_hours(9 * 60, 11 * 60 + 30);
    const std::vector<int> afternoon = quarter_hours(12 * 60 + 30, 15 * 60 + 30);
    split.insert(split.end(), afternoon.begin(), afternoon.end());
    std::vector<Case> cases;
    cases.push_back({"break", "0900-1130,1230-1530", split});
    cases.push_back({"control", "0900-1530", quarter_hours(9 * 60, 15 * 60 + 30)});
    // Not vacuous: 22 and 26 bars a day, and the break case's bar 10 is the
    // first one after the reopen.
    CHECK(cases[0].opens.size() == 22 && cases[1].opens.size() == 26);
    CHECK(cases[0].opens.size() > 10 && cases[0].opens[10] == 12 * 60 + 30);
    for (const Case& c : cases) run_case(c);
    std::printf("test_split_session_request_inputs: %zu cases, %lld checks, %d failed\n",
                cases.size(), checks, failures);
    return failures == 0 ? 0 : 1;
}
