/*
 * test_ltf_lookahead_calling_open_tapes.cpp — lane W8C-SECURITY.
 *
 * request.security(<a timeframe finer than the chart>, expr, lookahead_on)
 * merges the requested series at the calling chart bar's OPEN: TradingView
 * reads the requested bar that opens at or before that instant, the latest
 * such bar. On a chart whose bars open where the requested timeframe has a
 * bar, that is the chart bar's first intrabar (the rule round 7 pinned on
 * BINANCE:BTCUSDT 1D). OANDA stamps its XAUUSD daily bars at 17:00 ET, inside
 * the 17:00-18:00 break, one hour before the session they carry opens: no
 * requested bar opens at the stamp, and TradingView reads the previous chart
 * bar's last requested bar (16:45 ET, or Friday 16:45 on the Sunday-stamped
 * Monday bar). The engine read the first bucket of the chart bar's own slice
 * (18:00 ET), one requested bar late.
 *
 * Each row replays TradingView's own read-out tape of one synthetic probe
 * (tests/fixtures/ltf_lookahead_calling_open, lab tv exports on the 1D chart,
 * 2025-04-01 .. 2026-05-01) through a port of the probe that registers its
 * five request.security sites as the generated constructor does, over the
 * lanes' own TradingView daily bars and, as the auxiliary feed, their own
 * TradingView 15m bars (bars.inc). The tape encodes, per chart bar, the
 * minutes from the chart bar's time to the requested bar each site merged
 * (time with lookahead_on, time[1] with lookahead_on, time with
 * lookahead_off, time with gaps_on + lookahead_on) and close[1] with
 * lookahead_on, in the entry order's id. XAUUSD is the rule; ETHUSDT.P (24x7),
 * EURUSD (1700-1700, a 17:00 ET bar exists) and AAPL (09:30 open) are its
 * controls, whose first intrabar opens at the chart bar's time. A second
 * XAUUSD tape reads "60" (the 16:00 ET bar) and "240", whose 4h grid opens
 * at the 17:00 ET stamp and so keeps the first intrabar.
 *
 * A last case pins the same rule on an aggregating evaluator: 1m auxiliary
 * bars rolled up into "5" buckets under the OANDA break stamp.
 */

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
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

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the calling-open tapes need the auxiliary security feed"
#endif
#ifndef PINEFORGE_LTF_CALLING_OPEN_FIXTURE_DIR
#error "PINEFORGE_LTF_CALLING_OPEN_FIXTURE_DIR must name tests/fixtures/ltf_lookahead_calling_open"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/ltf_lookahead_calling_open/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kUtcPlus8 = 8 * 60 * kMinute;

std::vector<Bar> to_bars(const FeedBar* rows, std::size_t n) {
    std::vector<Bar> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close,
                       rows[i].volume, rows[i].ts});
    }
    return out;
}

// The read-out probe (fixtures/.../strategy.pine), its request.security
// sites as the generated strategy registers and evaluates them.
class CallingOpenProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t time;
        double t_on, t_on1, t_off, t_gaps_on, close1_on;
    };
    std::vector<Row> rows;

    CallingOpenProbe() { attach_pine_execution_adapter(); }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "15", input_tf_, true, false);   // time
        register_security_eval(1, "15", input_tf_, true, false);   // time[1]
        register_security_eval(2, "15", input_tf_, false, false);  // time, lookahead_off
        register_security_eval(3, "15", input_tf_, true, true);    // time, gaps_on
        register_security_eval(4, "15", input_tf_, true, false);   // close[1]
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        switch (sec_id) {
            case 0: t_on_ = static_cast<double>(bar.timestamp); break;
            case 1:
                t_on1_ = time_hist_.empty() ? kNaN : time_hist_.back();
                if (is_complete) time_hist_.push_back(static_cast<double>(bar.timestamp));
                break;
            case 2: t_off_ = static_cast<double>(bar.timestamp); break;
            case 3: t_gaps_on_ = static_cast<double>(bar.timestamp); break;
            case 4:
                close1_on_ = close_hist_.empty() ? kNaN : close_hist_.back();
                if (is_complete) close_hist_.push_back(bar.close);
                break;
            default: break;
        }
    }

    void clear_security(int sec_id) override {
        switch (sec_id) {
            case 0: t_on_ = kNaN; break;
            case 1: t_on1_ = kNaN; time_hist_.clear(); break;
            case 2: t_off_ = kNaN; break;
            case 3: t_gaps_on_ = kNaN; break;
            case 4: close1_on_ = kNaN; close_hist_.clear(); break;
            default: break;
        }
    }

    void on_source_bar(const Bar&) override {
        rows.push_back({current_bar_.timestamp, t_on_, t_on1_, t_off_,
                        t_gaps_on_, close1_on_});
    }

private:
    double t_on_ = kNaN, t_on1_ = kNaN, t_off_ = kNaN, t_gaps_on_ = kNaN,
           close1_on_ = kNaN;
    std::vector<double> time_hist_, close_hist_;
};

// One TradingView read-out: the chart bar the entry was placed on is the one
// before its fill, the id is "on|on1|off|gaps_on|close1" (minutes or "na").
struct Readout {
    std::string on, on1, off, gaps_on;
    double close1 = kNaN;
};

std::int64_t parse_utc8(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    std::sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
    // days_from_civil (Howard Hinnant), proleptic Gregorian, UTC.
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u
        + static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097
        + static_cast<std::int64_t>(doe) - 719468;
    return (days * 86400 + h * 3600 + mi * 60) * 1000 - kUtcPlus8;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    return out;
}

// fill instant -> the read-out of the chart bar that placed the order.
std::map<std::int64_t, Readout> load_tape(const std::string& dir) {
    std::map<std::int64_t, Readout> out;
    std::ifstream in(std::string(PINEFORGE_LTF_CALLING_OPEN_FIXTURE_DIR) + "/" + dir
                     + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        const auto cols = split(line, ',');
        if (cols.size() < 4 || cols[1].rfind("Entry", 0) != 0) continue;
        const auto f = split(cols[3], '|');
        if (f.size() != 5) continue;
        Readout r{f[0], f[1], f[2], f[3], kNaN};
        if (f[4] != "NaN") r.close1 = std::stod(f[4]);
        out[parse_utc8(cols[2])] = r;
    }
    return out;
}

std::string minutes_from(double requested_time, std::int64_t chart_time) {
    if (std::isnan(requested_time)) return "na";
    const auto delta = static_cast<std::int64_t>(requested_time) - chart_time;
    return std::to_string(delta / kMinute);
}

struct Lane {
    const char* tape;
    const char* timezone;
    const char* session;
    const FeedBar* daily;
    std::size_t n_daily;
    const FeedBar* aux;
    std::size_t n_aux;
};

// Returns the number of chart bars compared against the tape.
int replay(const Lane& lane) {
    const auto tape = load_tape(lane.tape);
    CHECK(!tape.empty());
    const auto chart = to_bars(lane.daily, lane.n_daily);
    const auto aux = to_bars(lane.aux, lane.n_aux);

    CallingOpenProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, lane.timezone);
    strategy_set_syminfo_session(handle, lane.session);
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "15") == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.rows.size() == chart.size());
    if (probe.rows.size() != chart.size()) return 0;

    int compared = 0;
    // The first chart bar is warmup: the auxiliary routing feeds no bar
    // before the chart's first bar, so nothing precedes its open.
    for (std::size_t i = 1; i + 1 < chart.size(); ++i) {
        const auto it = tape.find(chart[i + 1].timestamp);
        if (it == tape.end()) continue;
        const auto& row = probe.rows[i];
        const auto& tv = it->second;
        const bool ok = minutes_from(row.t_on, row.time) == tv.on
            && minutes_from(row.t_on1, row.time) == tv.on1
            && minutes_from(row.t_off, row.time) == tv.off
            && minutes_from(row.t_gaps_on, row.time) == tv.gaps_on
            && row.close1_on == tv.close1;
        if (!ok) {
            std::printf("  %s bar %lld: engine %s|%s|%s|%s|%.6g  tape %s|%s|%s|%s|%.6g\n",
                        lane.tape, static_cast<long long>(row.time),
                        minutes_from(row.t_on, row.time).c_str(),
                        minutes_from(row.t_on1, row.time).c_str(),
                        minutes_from(row.t_off, row.time).c_str(),
                        minutes_from(row.t_gaps_on, row.time).c_str(), row.close1_on,
                        tv.on.c_str(), tv.on1.c_str(), tv.off.c_str(),
                        tv.gaps_on.c_str(), tv.close1);
        }
        CHECK(ok);
        ++compared;
    }
    return compared;
}

void test_oanda_break_stamp_reads_the_previous_requested_bar() {
    // Tue, Wed, Sun (Monday's session, whose previous requested bar is
    // Friday's 16:45 ET) and Mon of 2025-04 compared.
    const Lane xau{"w8c-ltfon-xau1d", "America/New_York", "1800-1700",
                   kXauDaily, std::size(kXauDaily), kXauAux15, std::size(kXauAux15)};
    CHECK(replay(xau) == 4);
}

void test_controls_read_the_first_intrabar() {
    const Lane eth{"w8c-ltfon-eth1d", "UTC", "24x7",
                   kEthDaily, std::size(kEthDaily), kEthAux15, std::size(kEthAux15)};
    CHECK(replay(eth) == 7);
    const Lane eur{"w8c-ltfon-eur1d", "America/New_York", "1700-1700",
                   kEurDaily, std::size(kEurDaily), kEurAux15, std::size(kEurAux15)};
    CHECK(replay(eur) == 4);
    const Lane aapl{"w8c-ltfon-aapl1d", "America/New_York", "0930-1600",
                    kAaplDaily, std::size(kAaplDaily), kAaplAux15, std::size(kAaplAux15)};
    CHECK(replay(aapl) == 4);
}

// The second XAUUSD tape (w8c-ltfon-tfs-xau1d) reads "240" and "60" (and "5")
// at the same stamps: "t240|t60|t5|t240off|c240". The 60m bar at or before
// 17:00 ET is 16:00's (-60, Friday's -2940 on the Sunday stamp). TradingView's
// 4h grid opens at the 17:00 ET stamp itself (+1200, 13:00 ET, is the bar's
// last 4h bar), so "240" keeps the first intrabar: 0. The "5" field is left
// out here: it is finer than these 15m auxiliary bars.
class HourMultiplesProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t time;
        double t240, t60, t240_off, close1_240;
    };
    std::vector<Row> rows;
    HourMultiplesProbe() { attach_pine_execution_adapter(); }
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "240", input_tf_, true, false);   // time
        register_security_eval(1, "60", input_tf_, true, false);    // time
        register_security_eval(2, "240", input_tf_, true, false);   // close[1]
        register_security_eval(3, "240", input_tf_, false, false);  // time, lookahead_off
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        switch (sec_id) {
            case 0: t240_ = static_cast<double>(bar.timestamp); break;
            case 1: t60_ = static_cast<double>(bar.timestamp); break;
            case 2:
                close1_240_ = hist_.empty() ? kNaN : hist_.back();
                if (is_complete) hist_.push_back(bar.close);
                break;
            case 3: t240_off_ = static_cast<double>(bar.timestamp); break;
            default: break;
        }
    }
    void clear_security(int sec_id) override {
        switch (sec_id) {
            case 0: t240_ = kNaN; break;
            case 1: t60_ = kNaN; break;
            case 2: close1_240_ = kNaN; hist_.clear(); break;
            case 3: t240_off_ = kNaN; break;
            default: break;
        }
    }
    void on_source_bar(const Bar&) override {
        rows.push_back({current_bar_.timestamp, t240_, t60_, t240_off_, close1_240_});
    }

private:
    double t240_ = kNaN, t60_ = kNaN, t240_off_ = kNaN, close1_240_ = kNaN;
    std::vector<double> hist_;
};

void test_hour_multiples_under_the_break_stamp() {
    // The tape's fields in Readout order: on = t240, on1 = t60, off = t5,
    // gaps_on = t240off, close1 = c240.
    const auto tape = load_tape("w8c-ltfon-tfs-xau1d");
    CHECK(!tape.empty());
    const auto chart = to_bars(kXauDaily, std::size(kXauDaily));
    const auto aux = to_bars(kXauAux15, std::size(kXauAux15));
    HourMultiplesProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "America/New_York");
    strategy_set_syminfo_session(handle, "1800-1700");
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "15") == 0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.rows.size() == chart.size());
    if (probe.rows.size() != chart.size()) return;
    int compared = 0;
    for (std::size_t i = 1; i + 1 < chart.size(); ++i) {
        const auto it = tape.find(chart[i + 1].timestamp);
        if (it == tape.end()) continue;
        const auto& row = probe.rows[i];
        const auto& tv = it->second;
        const bool ok = minutes_from(row.t240, row.time) == tv.on
            && minutes_from(row.t60, row.time) == tv.on1
            && minutes_from(row.t240_off, row.time) == tv.gaps_on
            && row.close1_240 == tv.close1;
        if (!ok) {
            std::printf("  w8c-ltfon-tfs-xau1d bar %lld: engine %s|%s|%s|%.6g  tape %s|%s|%s|%.6g\n",
                        static_cast<long long>(row.time),
                        minutes_from(row.t240, row.time).c_str(),
                        minutes_from(row.t60, row.time).c_str(),
                        minutes_from(row.t240_off, row.time).c_str(), row.close1_240,
                        tv.on.c_str(), tv.on1.c_str(), tv.gaps_on.c_str(), tv.close1);
        }
        CHECK(ok);
        ++compared;
    }
    CHECK(compared == 4);
}

// The same rule on an aggregating evaluator (the lanes' 1m auxiliary slice
// rolled up into the requested buckets): 1D bars stamped at the 17:00 ET
// break, 1m bars from the 18:00 ET session open, "5" buckets.
class FiveMinuteProbe final : public source::PineStrategyHost {
public:
    std::vector<double> close1_at_body;
    FiveMinuteProbe() { attach_pine_execution_adapter(); }
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "5", input_tf_, true, false);  // close[1]
    }
    void evaluate_security(int, const Bar& bar, bool is_complete) override {
        req_ = hist_.empty() ? kNaN : hist_.back();
        if (is_complete) hist_.push_back(bar.close);
    }
    void clear_security(int) override { req_ = kNaN; hist_.clear(); }
    void on_source_bar(const Bar&) override { close1_at_body.push_back(req_); }

private:
    double req_ = kNaN;
    std::vector<double> hist_;
};

void test_aggregated_buckets_under_the_break_stamp() {
    constexpr std::int64_t stamp_a = 1704751200000;  // Mon 2024-01-08 17:00 EST
    constexpr std::int64_t stamp_b = 1704837600000;  // Tue 17:00 EST
    constexpr std::int64_t stamp_c = 1704924000000;  // Wed 17:00 EST
    constexpr std::int64_t hour = 60 * kMinute;
    const Bar chart[] = {
        {100.0, 101.0, 99.0, 100.5, 10.0, stamp_a},
        {200.0, 201.0, 199.0, 200.5, 10.0, stamp_b},
        {300.0, 301.0, 299.0, 300.5, 10.0, stamp_c},
    };
    // Each session: two 1m bars in its first "5" bucket (18:00 ET), two in
    // its last (16:55 ET, closing at 17:00).
    std::vector<Bar> aux;
    const std::int64_t stamps[] = {stamp_a, stamp_b, stamp_c};
    for (int d = 0; d < 3; ++d) {
        const std::int64_t open = stamps[d] + hour;          // 18:00 ET
        const std::int64_t last = stamps[d] + 24 * hour - 5 * kMinute;  // 16:55 ET
        const double base = 10.0 * (d + 1);
        aux.push_back({base, base, base, base + 0.1, 1.0, open});
        aux.push_back({base, base, base, base + 0.2, 1.0, open + kMinute});
        aux.push_back({base, base, base, base + 0.3, 1.0, last});
        aux.push_back({base, base, base, base + 0.4, 1.0, last + kMinute});
    }

    FiveMinuteProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "America/New_York");
    strategy_set_syminfo_session(handle, "1800-1700");
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
    probe.run(chart, 3, "1D", "1D", false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.close1_at_body.size() == 3);
    if (probe.close1_at_body.size() != 3) return;
    // Bar a is warmup (nothing precedes it). At bar b's open the requested
    // bar at or before it is a's 16:55 bucket (close 10.4), whose close[1]
    // is a's 18:00 bucket: 10.2. At bar c's open: b's 18:00 bucket, 20.2.
    // The engine read the slice's own first bucket before: 10.4, 20.4.
    CHECK(std::isnan(probe.close1_at_body[0]));
    CHECK(probe.close1_at_body[1] == 10.2);
    CHECK(probe.close1_at_body[2] == 20.2);
}

}  // namespace

int main() {
    test_oanda_break_stamp_reads_the_previous_requested_bar();
    test_controls_read_the_first_intrabar();
    test_hour_multiples_under_the_break_stamp();
    test_aggregated_buckets_under_the_break_stamp();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
