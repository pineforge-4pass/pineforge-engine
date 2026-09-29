// TradingView's session calendar for the chart's symbol (R5 lane XAU-CAL),
// against TradingView's own tapes (tests/fixtures/symbol_calendar, `lab tv
// --no-note` exports of the lane's synthetic scripts; the README names each).
//
// TradingView reads the bar after the current one from its calendar for the
// symbol, not from the chart's bars: time("", "", -1), time_close("", -1),
// time("", -2), time("D", 0, -1), session.islastbar and
// session.islastbar_regular. OANDA:XAUUSD's calendar is a session every
// weekday from 17:00 ET to 17:00 ET, holidays included, where its feed has no
// bar from 17:00 to 18:00 ET, none on Good Friday, Christmas or New Year's
// Day, and stops early on US holidays. NASDAQ:AAPL's calendar carries the
// exchange's holidays and early closes, as its feed does.
//
// 1. xc-flags-xau15-*: every bar of five OANDA:XAUUSD 15 windows -- Good
//    Friday, Memorial Day, Independence Day, Thanksgiving and the year end --
//    spells what TradingView read. With the symbol's calendar installed
//    (PineStrategyHost::set_symbol_calendar, xc-cal-xau15's) every cell is
//    TradingView's; without it the host reads the feed's next bar, and the
//    holiday and 17:00 ET cells part.
// 2. xc-flags-aapl15-thanksgiving: NASDAQ:AAPL's feed holds exactly its
//    calendar's sessions (xc-cal-aapl15 over 2025-04 .. 2026-05, as NYSE:F's
//    and NSE:NIFTY's do), so every cell is TradingView's with the calendar and
//    without it: a lane like it is given none and stays feed-based.
// 3. Lane TAIL-E's tapes of time() / time_close() with bars_back on
//    OANDA:XAUUSD 15 (tests/fixtures/time_bars_back, te-time-bb-*-xau15):
//    with the calendar none of their cells parts, where the feed parts on 484.
// 4. The C ABI carries the calendar as syminfo metadata ("symbol_calendar_*",
//    scripts/run_strategy.py): installed at the run's begin it answers as the
//    setter does, a day missing fails the run naming it, and days out of order
//    are refused. The days a run begins with are consumed, so a reused host's
//    next announcement starts from none; a day key whose index is no decimal
//    number is no day; a count that is no number >= 0 removes the calendar.
// 5. The calendar's slots are an intraday chart's bars and a 1D chart's days:
//    a W or M chart's bars are no calendar day, so it reads its own history,
//    with the calendar installed as without it.
//
// Source-bound (includes pineforge/source): release profile only.

#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR
#error "PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR must name tests/fixtures/symbol_calendar"
#endif
#ifndef PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR
#error "PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR must name tests/fixtures/time_bars_back"
#endif

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
using Sessions = std::vector<std::pair<std::int64_t, std::int64_t>>;

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

// "MMdd-HHmm" in the year nearest `near_ms`, as UTC ms.
std::int64_t unspell(const std::string& word, std::int64_t near_ms) {
    int mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(word.c_str(), "%2d%2d-%2d%2d", &mo, &d, &h, &mi) != 4) return -1;
    const int year = 1970 + static_cast<int>(near_ms / (365LL * 24 * 60 * kMinute + 6 * 60 * kMinute));
    std::int64_t best = -1, best_gap = -1;
    for (int y = year - 1; y <= year + 1; ++y) {
        const std::int64_t t = utc_ms(y, mo, d, h, mi);
        const std::int64_t gap = t > near_ms ? t - near_ms : near_ms - t;
        if (best_gap < 0 || gap < best_gap) { best = t; best_gap = gap; }
    }
    return best;
}

// A tape's readings: every bar's '|'-split Signal (an entry's name, a close's
// comment), keyed by the bar's own open, its first field.
std::map<std::int64_t, std::vector<std::string>> read_tape(const std::string& dir,
                                                           const std::string& slug) {
    std::map<std::int64_t, std::vector<std::string>> bars;
    std::ifstream in(dir + "/" + slug + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        const auto cell = split(line, ',');
        if (cell.size() < 4 || cell[3].empty()) continue;   // the range end's close
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        const std::int64_t row_ms = utc_ms(y, mo, d, h, mi) - 8 * 60 * kMinute;  // UTC+8
        const auto fields = split(cell[3], '|');
        if (fields.empty()) continue;
        bars[unspell(fields[0], row_ms)] = fields;
    }
    return bars;
}

// A pineforge-symbol-calendar/v1 document's sessions (scripts/symbol_calendar.py).
Sessions read_calendar(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR) + "/" + slug
                     + "/calendar.json");
    std::stringstream text;
    text << in.rdbuf();
    const std::string doc = text.str();
    Sessions out;
    std::size_t at = doc.find("\"sessions\":[");
    if (at == std::string::npos) return out;
    at += 12;
    while (true) {
        at = doc.find('[', at);
        if (at == std::string::npos) break;
        char* end = nullptr;
        const long long open = std::strtoll(doc.c_str() + at + 1, &end, 10);
        const long long close = std::strtoll(end + 1, &end, 10);
        out.emplace_back(open, close);
        at = static_cast<std::size_t>(end - doc.c_str());
    }
    return out;
}

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

enum class Probe { Flags, TimeChart, TimeTf };

// The fields each probe spells, as its generated code reads them.
class CalendarHost final : public source::PineStrategyHost {
public:
    CalendarHost(Probe probe, const std::string& session, const std::string& timezone)
        : probe_(probe) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }
    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const std::string& tz = syminfo_.timezone;
        const std::string& session = syminfo_.session;
        const auto b = [](bool x) { return std::string(x ? "1" : "0"); };
        std::vector<std::string> v;
        if (probe_ == Probe::Flags) {
            const std::int64_t next = pine_time_offset(t, -1, "", "", "", 0, false);
            v = {spell(t), spell(next), spell(pine_time_offset(t, -1, "", "", "", 0, true)),
                 std::to_string(pine_dayofweek(next, tz)),
                 spell(pine_time_offset(t, -2, "", "", "", 0, false)),
                 spell(pine_time_offset(t, 0, "D", "", "", -1, false)),
                 b(session_isfirstbar_) + b(session_islastbar_) + b(session_isfirstbar_regular_)
                     + b(session_islastbar_regular_)};
        } else if (probe_ == Probe::TimeChart) {
            const std::int64_t next = pine_time_offset(t, -1, "", "", "", 0, false);
            v = {spell(t), spell(next), spell(next),
                 spell(pine_time_offset(t, -2, "", "", "", 0, false)),
                 spell(pine_time_offset(t, -5, "", "", "", 0, false)),
                 spell(pine_time_offset(t, 1, "", "", "", 0, false)),
                 spell(pine_time_offset(t, -1, "", "", "", 0, true)),
                 spell(pine_time_close(t, "", "", "", script_tf_, tz, session)),
                 std::to_string(pine_dayofweek(next, tz))};
        } else {
            v = {spell(t),
                 spell(pine_time(t, "60", "", "", script_tf_, tz, session)),
                 spell(pine_time_offset(t, -1, "60", "", "", 0, false)),
                 spell(pine_time_offset(t, 0, "60", "", "", -1, false)),
                 spell(pine_time_offset(t, 0, "60", "", "", 1, false)),
                 spell(pine_time(t, "D", "", "", script_tf_, tz, session)),
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
    std::map<std::int64_t, std::vector<std::string>> seen;

private:
    Probe probe_;
};

struct Outcome {
    int cells = 0;
    int differ = 0;
    std::string error;
};

// Replays one tape's bars -- the bars it spells, and the range's last bar,
// which only its predecessor names (the next-bar field of the tape `last_of`)
// -- and counts the cells the host reads otherwise. `calendar`, when given,
// is installed through the setter, or with `metadata` as the C ABI carries it.
Outcome replay(const std::string& dir, const std::string& slug, const std::string& last_of,
               Probe probe, const char* session, const char* timezone, const Sessions* calendar,
               bool metadata = false, int show = 4) {
    Outcome out;
    const auto tape = read_tape(dir, slug);
    const auto chart = last_of == slug ? tape : read_tape(dir, last_of);
    CHECK(!tape.empty() && !chart.empty());
    if (tape.empty() || chart.empty()) return out;
    std::vector<Bar> bars;
    for (const auto& entry : tape) bars.push_back(flat_bar(entry.first));
    const std::int64_t final_bar =
        unspell(chart.rbegin()->second.at(1), chart.rbegin()->first);
    if (final_bar > bars.back().timestamp) bars.push_back(flat_bar(final_bar));

    CalendarHost host(probe, session, timezone);
    if (calendar != nullptr && !metadata) CHECK(host.set_symbol_calendar(*calendar));
    if (calendar != nullptr && metadata) {
        host.set_syminfo_metadata("symbol_calendar_days", static_cast<double>(calendar->size()));
        for (std::size_t i = 0; i < calendar->size(); ++i) {
            host.set_syminfo_metadata("symbol_calendar_open:" + std::to_string(i),
                                      static_cast<double>((*calendar)[i].first));
            host.set_syminfo_metadata("symbol_calendar_close:" + std::to_string(i),
                                      static_cast<double>((*calendar)[i].second));
        }
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    out.error = host.last_error();
    for (const auto& [ts, want] : tape) {
        const auto got = host.seen.find(ts);
        if (got == host.seen.end() || got->second.size() != want.size()) {
            ++out.differ;
            continue;
        }
        for (std::size_t k = 0; k < want.size(); ++k) {
            ++out.cells;
            if (got->second[k] == want[k]) continue;
            ++out.differ;
            if (out.differ <= show)
                std::printf("    %s bar %s field %zu: TradingView %s, host %s\n", slug.c_str(),
                            want[0].c_str(), k, want[k].c_str(), got->second[k].c_str());
        }
    }
    return out;
}

const char* const kXauWindows[] = {
    "xc-flags-xau15-goodfriday", "xc-flags-xau15-memorial", "xc-flags-xau15-jul4",
    "xc-flags-xau15-thanksgiving", "xc-flags-xau15-yearend",
};

void test_xau_flags() {
    scenario = "OANDA:XAUUSD flags";
    const Sessions calendar = read_calendar("xc-cal-xau15");
    CHECK(calendar.size() == 1308);
    const std::string dir = PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR;
    int feed_differ = 0;
    for (const char* slug : kXauWindows) {
        const Outcome with = replay(dir, slug, slug, Probe::Flags, "1800-1700",
                                    "America/New_York", &calendar);
        const Outcome without = replay(dir, slug, slug, Probe::Flags, "1800-1700",
                                       "America/New_York", nullptr, false, 0);
        std::printf("  %-30s %5d cells: calendar %d differ, feed %d differ\n", slug, with.cells,
                    with.differ, without.differ);
        CHECK(with.error.empty());
        CHECK(with.cells > 0);
        CHECK(with.differ == 0);
        CHECK(without.differ > 0);
        feed_differ += without.differ;
    }
    CHECK(feed_differ > 0);
}

void test_aapl_stays_feed_based() {
    scenario = "NASDAQ:AAPL flags";
    const Sessions calendar = read_calendar("xc-cal-aapl15");
    CHECK(calendar.size() == 275);
    const std::string dir = PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR;
    const char* slug = "xc-flags-aapl15-thanksgiving";
    const Outcome without = replay(dir, slug, slug, Probe::Flags, "0930-1600", "America/New_York",
                                   nullptr);
    const Outcome with = replay(dir, slug, slug, Probe::Flags, "0930-1600", "America/New_York",
                                &calendar);
    std::printf("  %-30s %5d cells: feed %d differ, calendar %d differ\n", slug, without.cells,
                without.differ, with.differ);
    CHECK(without.cells > 0);
    CHECK(without.differ == 0);
    CHECK(with.differ == 0);
}

void test_time_bars_back_tapes() {
    scenario = "TAIL-E time bars_back tapes";
    const Sessions calendar = read_calendar("xc-cal-xau15");
    const std::string dir = PINEFORGE_TIME_BARS_BACK_FIXTURE_DIR;
    for (const auto& [slug, probe] :
         {std::pair<const char*, Probe>{"te-time-bb-chart-xau15", Probe::TimeChart},
          {"te-time-bb-tf-xau15", Probe::TimeTf}}) {
        const Outcome with = replay(dir, slug, "te-time-bb-chart-xau15", probe, "1800-1700",
                                    "America/New_York", &calendar);
        const Outcome without = replay(dir, slug, "te-time-bb-chart-xau15", probe, "1800-1700",
                                       "America/New_York", nullptr, false, 0);
        std::printf("  %-30s %5d cells: calendar %d differ, feed %d differ\n", slug, with.cells,
                    with.differ, without.differ);
        CHECK(with.cells > 0);
        CHECK(with.differ == 0);
        CHECK(without.differ > 0);
    }
}

void test_metadata_transport() {
    scenario = "metadata transport";
    const Sessions calendar = read_calendar("xc-cal-xau15");
    const std::string dir = PINEFORGE_SYMBOL_CALENDAR_FIXTURE_DIR;
    const char* slug = "xc-flags-xau15-jul4";
    const Outcome via = replay(dir, slug, slug, Probe::Flags, "1800-1700", "America/New_York",
                               &calendar, true);
    CHECK(via.error.empty());
    CHECK(via.cells > 0);
    CHECK(via.differ == 0);
    // A day the count names but no key carries fails the run, naming it.
    Sessions gapped = calendar;
    CalendarHost host(Probe::Flags, "1800-1700", "America/New_York");
    host.set_syminfo_metadata("symbol_calendar_days", 3.0);
    host.set_syminfo_metadata("symbol_calendar_open:0", static_cast<double>(gapped[0].first));
    host.set_syminfo_metadata("symbol_calendar_close:0", static_cast<double>(gapped[0].second));
    host.set_syminfo_metadata("symbol_calendar_open:2", static_cast<double>(gapped[2].first));
    host.set_syminfo_metadata("symbol_calendar_close:2", static_cast<double>(gapped[2].second));
    std::vector<Bar> bars = {flat_bar(gapped[0].first + 60 * kMinute),
                             flat_bar(gapped[0].first + 75 * kMinute)};
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().find("symbol calendar: session day 1 of 3 is missing")
          != std::string::npos);
    // Days out of order are refused, and nothing is installed.
    CalendarHost refused(Probe::Flags, "1800-1700", "America/New_York");
    CHECK(!refused.set_symbol_calendar({{gapped[1].first, gapped[1].second},
                                        {gapped[0].first, gapped[0].second}}));
    CHECK(!refused.set_symbol_calendar({{gapped[0].second, gapped[0].first}}));
    CHECK(refused.symbol_calendar().empty());
    CHECK(refused.set_symbol_calendar({{gapped[0].first, gapped[0].second}}));
    CHECK(refused.symbol_calendar().size() == 1);
    CHECK(refused.set_symbol_calendar({}));
    CHECK(refused.symbol_calendar().empty());
}

void send_calendar(CalendarHost& host, const Sessions& days, long long count,
                   const std::vector<std::size_t>& indices, const char* suffix = "") {
    host.set_syminfo_metadata("symbol_calendar_days", static_cast<double>(count));
    for (std::size_t i : indices) {
        host.set_syminfo_metadata("symbol_calendar_open:" + std::to_string(i) + suffix,
                                  static_cast<double>(days[i].first));
        host.set_syminfo_metadata("symbol_calendar_close:" + std::to_string(i) + suffix,
                                  static_cast<double>(days[i].second));
    }
}

void test_metadata_per_run() {
    scenario = "metadata per run";
    const Sessions calendar = read_calendar("xc-cal-xau15");
    CHECK(calendar.size() > 3);
    if (calendar.size() <= 3) return;
    std::vector<Bar> bars = {flat_bar(calendar[0].first + 60 * kMinute),
                             flat_bar(calendar[0].first + 75 * kMinute)};
    // A reused host: the next run's announcement starts from none, so a day
    // it leaves out is missing, not the run before's.
    CalendarHost host(Probe::Flags, "1800-1700", "America/New_York");
    send_calendar(host, calendar, 3, {0, 1, 2});
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    CHECK(host.symbol_calendar().size() == 3);
    send_calendar(host, calendar, 3, {0, 2});
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().find("symbol calendar: session day 1 of 3 is missing")
          != std::string::npos);
    // The announcement replaced the calendar: none is left for a later run
    // that announces nothing.
    CHECK(host.symbol_calendar().empty());
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    CHECK(host.symbol_calendar().empty());
    // A day key whose index is no decimal number is no day.
    CalendarHost malformed(Probe::Flags, "1800-1700", "America/New_York");
    send_calendar(malformed, calendar, 2, {0});
    send_calendar(malformed, calendar, 2, {1}, "x");
    malformed.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(malformed.last_error().find("symbol calendar: session day 1 of 2 is missing")
          != std::string::npos);
    // A count that is no number >= 0 removes the calendar.
    for (const double count : {std::nan(""), -1.0}) {
        CalendarHost removed(Probe::Flags, "1800-1700", "America/New_York");
        send_calendar(removed, calendar, 2, {0, 1});
        removed.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
        CHECK(removed.last_error().empty());
        CHECK(removed.symbol_calendar().size() == 2);
        removed.set_syminfo_metadata("symbol_calendar_days", count);
        CHECK(removed.symbol_calendar().empty());
        removed.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
        CHECK(removed.last_error().empty());
        CHECK(removed.symbol_calendar().empty());
    }
}

// A W or M chart's bars, one per period's first calendar day, read with the
// calendar installed and without it.
void test_weekly_and_monthly_charts() {
    scenario = "weekly and monthly charts";
    const Sessions calendar = read_calendar("xc-cal-xau15");
    for (const char* tf : {"W", "M"}) {
        std::vector<Bar> bars;
        long long period = -1;
        for (const auto& day : calendar) {
            // The period of the day's trading date: its close's New York date.
            const std::int64_t close_day = (day.second - 5 * 3600000LL) / 86400000LL;
            long long key = 0;
            if (tf[0] == 'W') {
                key = (close_day + 3) / 7;   // weeks from Monday 1970-01-05
            } else {
                // spell() reads MMDD-HHMM: a new month is a new MM.
                key = std::stoll(spell(day.second - 5 * 3600000LL).substr(0, 2));
            }
            if (key != period) {
                period = key;
                bars.push_back(flat_bar(day.first));
            }
            if (bars.size() >= 40) break;
        }
        CalendarHost plain(Probe::TimeChart, "1800-1700", "America/New_York");
        CalendarHost with(Probe::TimeChart, "1800-1700", "America/New_York");
        CHECK(with.set_symbol_calendar(calendar));
        plain.run(bars.data(), static_cast<int>(bars.size()), tf, tf);
        with.run(bars.data(), static_cast<int>(bars.size()), tf, tf);
        CHECK(plain.last_error().empty());
        CHECK(with.last_error().empty());
        CHECK(plain.seen.size() == bars.size());
        CHECK(with.seen == plain.seen);
        int differ = 0;
        for (const auto& [ts, cells] : plain.seen) {
            const auto other = with.seen.find(ts);
            if (other == with.seen.end() || other->second != cells) ++differ;
        }
        std::printf("  %s chart: %zu bars, %d read otherwise with the calendar\n", tf,
                    plain.seen.size(), differ);
    }
}

}  // namespace

int main() {
    std::printf("TradingView's symbol calendar: the bar after the current one\n");
    test_xau_flags();
    test_aapl_stays_feed_based();
    test_time_bars_back_tapes();
    test_metadata_transport();
    test_metadata_per_run();
    test_weekly_and_monthly_charts();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
