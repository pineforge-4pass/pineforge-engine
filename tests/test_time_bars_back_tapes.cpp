/*
 * test_time_bars_back_tapes.cpp -- R5 lane TAIL-E, item 2.
 *
 * Pine v6 time() / time_close() reading another bar (bars_back,
 * timeframe_bars_back) against TradingView (tests/fixtures/time_bars_back,
 * README.md there), through the Pine host as generated code calls it
 * (PineStrategyHost::pine_time_offset, inside on_source_bar).
 *
 * Every bar of each probe spells the times it reads, "MMdd-HHmm" in UTC: the
 * chart probe its own bar's open, the next bar's with and without the session
 * argument, two and five bars ahead, the bar before, the next bar's close and
 * its own, and the weekday of the next bar; the timeframe probe the hour, the
 * day, the week and the month holding a bar, the one after it and the one
 * before it, on BINANCE:ETHUSDT.P 15, NASDAQ:AAPL 15 and 1D, OANDA:XAUUSD 15,
 * OANDA:EURUSD 1D and BINANCE:BTCUSDT 1D. TradingView reads the bars its
 * history holds: AAPL's next bar after Thursday 2025-04-17 15:45 ET is Monday
 * 04-21 09:30 (Good Friday), after the 01-08 daily bar the 01-10 one (the
 * 01-09 closure), after Friday 05-23 Tuesday 05-27 (Memorial Day).
 *
 * Each run feeds the tape's own bar times, and the last bar of TradingView's
 * range, which the tape only names as its predecessor's next bar. A value
 * TradingView read from a bar outside the range, where the run holds no bar,
 * is the host's session calendar, which holds no exchange holidays: the cells
 * listed in kCalendarEdges are those where the two part, each named with its
 * reason.
 */

#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR
#error "PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR must name tests/fixtures/time_bars_back"
#endif

namespace {

constexpr std::int64_t kMinute = 60'000;

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc_ms(int y, int mo, int d, int h, int mi) {
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 24 + h)
            * 60 + mi) * kMinute;
}

// "MMdd-HHmm" of a UTC instant, "na" for na: the probes' f().
std::string spell(std::int64_t ms) {
    if (is_na(ms)) return "na";
    std::int64_t minutes = ms / kMinute;
    const int mi = static_cast<int>(minutes % 60);
    minutes /= 60;
    const int h = static_cast<int>(minutes % 24);
    std::int64_t z = minutes / 24 + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char out[16];
    std::snprintf(out, sizeof out, "%02u%02u-%02d%02d", m, d, h, mi);
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, sep)) parts.push_back(part);
    return parts;
}

// "MMdd-HHmm" in the year of `near_ms` (the export's own date), as UTC ms.
std::int64_t unspell(const std::string& word, std::int64_t near_ms, bool& ok) {
    int mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(word.c_str(), "%2d%2d-%2d%2d", &mo, &d, &h, &mi) != 4) {
        ok = false;
        return 0;
    }
    const std::int64_t year_ms = 365LL * 24 * 60 * kMinute;
    const int year = 1970 + static_cast<int>(near_ms / (year_ms + 6 * 60 * kMinute));
    std::int64_t best = 0;
    std::int64_t best_gap = -1;
    for (int y = year - 1; y <= year + 1; ++y) {
        const std::int64_t t = utc_ms(y, mo, d, h, mi);
        const std::int64_t gap = t > near_ms ? t - near_ms : near_ms - t;
        if (best_gap < 0 || gap < best_gap) { best = t; best_gap = gap; }
    }
    return best;
}

// Every bar's spelled values in a tape: the Signal of each row (an entry's
// name, a close's comment), keyed by the bar's own open, its first field.
struct Tape {
    std::map<std::int64_t, std::vector<std::string>> bars;
    bool ok = true;
};

Tape read_tape(const std::string& slug) {
    Tape tape;
    std::ifstream in(std::string(PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR) + "/" + slug
                     + "/tv_trades.csv");
    if (!in) { tape.ok = false; return tape; }
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        const auto cell = split(line, ',');
        // The run-end close of the position left open carries no Signal.
        if (cell.size() < 4 || cell[3].empty()) continue;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) {
            tape.ok = false;
            continue;
        }
        const std::int64_t row_ms = utc_ms(y, mo, d, h, mi) - 8 * 60 * kMinute;  // UTC+8
        const auto fields = split(cell[3], '|');
        if (fields.empty()) { tape.ok = false; continue; }
        const std::int64_t bar = unspell(fields[0], row_ms, tape.ok);
        tape.bars[bar] = fields;
    }
    if (tape.bars.empty()) tape.ok = false;
    return tape;
}

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

enum class Probe { Chart, Timeframe };

// The fields each probe spells, as its generated code reads them.
class OffsetHost final : public source::PineStrategyHost {
public:
    OffsetHost(Probe probe, const std::string& session, const std::string& timezone)
        : probe_(probe) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }
    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const std::string& tz = syminfo_.timezone;
        const std::string& session = syminfo_.session;
        std::vector<std::string> v;
        if (probe_ == Probe::Chart) {
            const std::int64_t next = pine_time_offset(t, -1, "", "", "", 0, false);
            v = {spell(t),
                 spell(pine_time_offset(t, -1, "", "", "", 0, false)),
                 spell(next),
                 spell(pine_time_offset(t, -2, "", "", "", 0, false)),
                 spell(pine_time_offset(t, -5, "", "", "", 0, false)),
                 spell(pine_time_offset(t, 1, "", "", "", 0, false)),
                 spell(pine_time_offset(t, -1, "", "", "", 0, true)),
                 spell(pine_time_close(t, "", "", "", script_tf(), tz, session)),
                 std::to_string(pine_dayofweek(next, tz))};
        } else {
            v = {spell(t),
                 spell(pine_time(t, "60", "", "", script_tf(), tz, session)),
                 spell(pine_time_offset(t, -1, "60", "", "", 0, false)),
                 spell(pine_time_offset(t, 0, "60", "", "", -1, false)),
                 spell(pine_time_offset(t, 0, "60", "", "", 1, false)),
                 spell(pine_time(t, "D", "", "", script_tf(), tz, session)),
                 spell(pine_time_offset(t, -1, "D", "", "", 0, false)),
                 spell(pine_time_offset(t, 0, "D", "", "", -1, false)),
                 spell(pine_time_offset(t, 0, "D", "", "", 1, false)),
                 spell(pine_time_offset(t, 0, "D", "", "", -1, true)),
                 spell(pine_time_offset(t, 0, "W", "", "", -1, false)),
                 spell(pine_time_offset(t, 0, "M", "", "", -1, false)),
                 spell(pine_time_offset(t, 1, "D", "", "", -1, false))};
        }
        seen[t] = v;
    }
    const std::string& script_tf() const { return script_tf_; }
    std::map<std::int64_t, std::vector<std::string>> seen;

private:
    Probe probe_;
};

struct Case {
    const char* slug;
    Probe probe;
    const char* session;
    const char* timezone;
    const char* tf;
    const char* chart_slug;  // the chart probe's tape of the same chart and range
    bool xau_slots = false;  // the cells xau_no_bar_slot names part
};

// The cells where TradingView read a bar outside its range that the calendar
// steps to otherwise: (slug, bar "MMdd-HHmm", field index) and why.
struct Edge {
    const char* slug;
    const char* bar;
    int field;
    const char* reason;
};
const Edge kCalendarEdges[] = {
    // TradingView's range opens on 2025-01-02; the run holds no bar before
    // it, and the session calendar reads New Year's Day, a closure.
    {"te-time-bb-tf-aapl1d", "0102-1430", 4,
     "time(\"60\", 0, 1): TradingView 12-31, the calendar 01-01"},
    {"te-time-bb-tf-aapl1d", "0102-1430", 8,
     "time(\"D\", 0, 1): TradingView 12-30, the calendar 01-01"},
};

// OANDA:XAUUSD's own history holds no bar in the 17:00-18:00 ET slot, nor in
// Good Friday 2025-04-18's session, where TradingView's calendar holds both:
// its next bar after 16:45 ET is 17:00 ET and its next day after Thursday
// 04-17's is Friday's, while the host reads the next bar the history holds
// (18:00 ET, Sunday 04-20). A cell of this class reads TradingView's value in
// the 17:00 ET hour or Good Friday's session, or is one hour off it.
std::int64_t minute_of(const std::string& word) {
    int mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(word.c_str(), "%2d%2d-%2d%2d", &mo, &d, &h, &mi) != 4) return -1;
    return ((mo * 31 + d) * 24 + h) * 60 + mi;
}

bool xau_no_bar_slot(const std::string& tv, const std::string& host) {
    if (tv == "5" && host == "1") return true;  // dayofweek of the next bar
    const std::int64_t t = minute_of(tv);
    const std::int64_t h = minute_of(host);
    if (t < 0 || h < 0) return false;
    const std::int64_t tod = t % (24 * 60);
    const bool break_hour = tod >= 21 * 60 && tod < 22 * 60;
    const bool good_friday = t >= minute_of("0417-2100") && t <= minute_of("0418-2100");
    return break_hour || good_friday || h - t == 60 || t - h == 60;
}

bool is_edge(const Case& c, const std::string& bar, int field) {
    for (const Edge& e : kCalendarEdges) {
        if (e.slug != nullptr && c.slug == std::string(e.slug) && bar == e.bar
            && field == e.field) {
            return true;
        }
    }
    return false;
}

void replay(const Case& c) {
    const Tape tape = read_tape(c.slug);
    CHECK(tape.ok);
    if (!tape.ok) return;
    std::vector<Bar> bars;
    for (const auto& [ts, fields] : tape.bars) bars.push_back(flat_bar(ts));
    // The range's last bar: its predecessor's next bar in the chart probe's
    // tape of the same range, the one bar a tape spells no values for.
    const Tape chart = std::string(c.chart_slug) == c.slug ? tape : read_tape(c.chart_slug);
    CHECK(chart.ok);
    bool ok = chart.ok;
    const std::int64_t final_bar =
        ok ? unspell(chart.bars.rbegin()->second[1], chart.bars.rbegin()->first, ok) : 0;
    CHECK(ok);
    if (ok && final_bar > bars.back().timestamp) bars.push_back(flat_bar(final_bar));

    OffsetHost host(c.probe, c.session, c.timezone);
    host.run(bars.data(), static_cast<int>(bars.size()), c.tf, c.tf);
    CHECK(host.last_error().empty());
    int cells = 0, misses = 0, edges = 0, slots = 0;
    for (const auto& [ts, want] : tape.bars) {
        const auto got = host.seen.find(ts);
        CHECK(got != host.seen.end());
        if (got == host.seen.end()) continue;
        CHECK(got->second.size() == want.size());
        for (std::size_t k = 0; k < want.size() && k < got->second.size(); ++k) {
            ++cells;
            if (got->second[k] == want[k]) continue;
            if (is_edge(c, want[0], static_cast<int>(k))) {
                ++edges;
                continue;
            }
            if (c.xau_slots && xau_no_bar_slot(want[k], got->second[k])) {
                ++slots;
                continue;
            }
            ++misses;
            if (misses <= 8)
                std::printf("    %s bar %s field %zu: TradingView %s, host %s\n", c.slug,
                            want[0].c_str(), k, want[k].c_str(), got->second[k].c_str());
        }
    }
    std::printf("  %-26s %-4s %4zu bars %5d cells: %d differ, %d calendar edges,"
                " %d no-bar slots\n", c.slug, c.tf, tape.bars.size(), cells, misses, edges,
                slots);
    CHECK(misses == 0);
    CHECK(c.xau_slots || slots == 0);
}

}  // namespace

int main() {
    std::printf("time() / time_close() with bars_back and timeframe_bars_back, against TradingView\n");
    const Case cases[] = {
        {"te-time-bb-chart-eth15", Probe::Chart, "24x7", "UTC", "15", "te-time-bb-chart-eth15"},
        {"te-time-bb-chart-btc1d", Probe::Chart, "24x7", "UTC", "1D", "te-time-bb-chart-btc1d"},
        {"te-time-bb-chart-aapl15", Probe::Chart, "0930-1600", "America/New_York", "15", "te-time-bb-chart-aapl15"},
        {"te-time-bb-chart-aapl1d", Probe::Chart, "0930-1600", "America/New_York", "1D", "te-time-bb-chart-aapl1d"},
        {"te-time-bb-chart-xau15", Probe::Chart, "1800-1700", "America/New_York", "15", "te-time-bb-chart-xau15", true},
        {"te-time-bb-chart-eurusd1d", Probe::Chart, "1700-1700", "America/New_York", "1D", "te-time-bb-chart-eurusd1d"},
        {"te-time-bb-tf-eth15", Probe::Timeframe, "24x7", "UTC", "15", "te-time-bb-chart-eth15"},
        {"te-time-bb-tf-btc1d", Probe::Timeframe, "24x7", "UTC", "1D", "te-time-bb-chart-btc1d"},
        {"te-time-bb-tf-aapl15", Probe::Timeframe, "0930-1600", "America/New_York", "15", "te-time-bb-chart-aapl15"},
        {"te-time-bb-tf-aapl1d", Probe::Timeframe, "0930-1600", "America/New_York", "1D", "te-time-bb-chart-aapl1d"},
        {"te-time-bb-tf-xau15", Probe::Timeframe, "1800-1700", "America/New_York", "15", "te-time-bb-chart-xau15", true},
        {"te-time-bb-tf-eurusd1d", Probe::Timeframe, "1700-1700", "America/New_York", "1D", "te-time-bb-chart-eurusd1d"},
    };
    for (const Case& c : cases) replay(c);

    // A stream retains its warmup; its live bars have no bar after them, and
    // no history in the retained input: the calendar steps both ways. On a
    // 24x7 chart the calendar's bars are the history's, so every value past
    // the warmup is TradingView's too.
    for (const Probe probe : {Probe::Chart, Probe::Timeframe}) {
        const char* slug = probe == Probe::Chart ? "te-time-bb-chart-eth15" : "te-time-bb-tf-eth15";
        const Tape tape = read_tape(slug);
        CHECK(tape.ok);
        if (!tape.ok) continue;
        std::vector<Bar> bars;
        for (const auto& [ts, fields] : tape.bars) bars.push_back(flat_bar(ts));
        bars.push_back(flat_bar(bars.back().timestamp + 15 * kMinute));
        const std::size_t warmup = 48;  // half a day
        OffsetHost host(probe, "24x7", "UTC");
        const bool began = host.stream_begin(bars.data(), static_cast<int>(warmup), "15", "15");
        CHECK(began);
        for (std::size_t i = warmup; began && i < bars.size(); ++i)
            CHECK(host.stream_push_bar(bars[i]));
        CHECK(host.stream_end(false));
        CHECK(host.last_error().empty());
        int cells = 0, misses = 0;
        for (const auto& [ts, want] : tape.bars) {
            const auto got = host.seen.find(ts);
            CHECK(got != host.seen.end());
            if (got == host.seen.end()) continue;
            for (std::size_t k = 0; k < want.size() && k < got->second.size(); ++k) {
                ++cells;
                if (got->second[k] != want[k]) {
                    ++misses;
                    if (misses <= 8)
                        std::printf("    streamed %s bar %s field %zu: TradingView %s, host %s\n",
                                    slug, want[0].c_str(), k, want[k].c_str(),
                                    got->second[k].c_str());
                }
            }
        }
        std::printf("  %-26s streamed after %zu bars: %d cells, %d differ\n", slug, warmup, cells,
                    misses);
        CHECK(misses == 0);
    }
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
