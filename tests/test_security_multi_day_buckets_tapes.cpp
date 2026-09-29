/*
 * test_security_multi_day_buckets_tapes.cpp -- lane TAIL-A.
 *
 * request.security(syminfo.tickerid, "<N>D", expr) on TradingView groups the
 * symbol's trading dates N at a time, counting from the first trading date of
 * every calendar year; the year's last bar ends at the year's end. The
 * trading dates are the symbol's session calendar: every day on a 24x7
 * symbol, every weekday on OANDA's (a holiday too -- EURUSD's 8D bar opens on
 * Thu 2025-12-25, a date its chart has no bar of), the exchange's sessions on
 * a stock or an index (NYSE:F's 8D bar 2025-04-17 runs through 04-29 over
 * Good Friday). The engine's calendar aggregator split an N-day bucket at
 * every day boundary: an "8D" request read one day.
 *
 * Each row replays one TradingView read-out tape of a synthetic probe
 * (tests/fixtures/multi_day_buckets, lab tv exports on the 1D chart,
 * 2024-01-01 .. 2026-05-01) through a port of the probe that registers its
 * six request.security sites as the generated constructor does. The tape's
 * close comments spell, per chart bar, the open time (epoch days) of the
 * "2D", "3D", "5D", "8D" and "13D" bars the chart bar reads with
 * lookahead_on, and the "8D" context's bar_index. The chart bars are the
 * tape's own. The run starts the requested contexts at the export's first
 * bar (security_range_start_na_warmup), as TradingView does: a bucket opened
 * before it reads na.
 *
 * A stock chart holds its sessions from its first bar on: a year it starts
 * after a weekday holiday (NYSE's 2024-01-01) counts that weekday, so the
 * stock rows compare from 2025 on, the first year the chart holds whole, and
 * the 8D context's bar_index from there on.
 */

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
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

#ifndef PINEFORGE_MULTI_DAY_BUCKETS_FIXTURE_DIR
#error "PINEFORGE_MULTI_DAY_BUCKETS_FIXTURE_DIR must name tests/fixtures/multi_day_buckets"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 24 * 60 * kMinute;
constexpr std::int64_t kUtcPlus8 = 8 * 60 * kMinute;
constexpr int kSites = 5;
const char* const kTimeframes[kSites] = {"2D", "3D", "5D", "8D", "13D"};

// The read-out probe (fixtures/.../strategy.pine), its request.security
// sites as the generated strategy registers and evaluates them.
class MultiDayProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t time;
        double open[kSites];
        double index8;
    };
    std::vector<Row> rows;

    MultiDayProbe() { attach_pine_execution_adapter(); }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (int i = 0; i < kSites; ++i)
            register_security_eval(i, kTimeframes[i], input_tf_, true, false);  // time
        register_security_eval(kSites, "8D", input_tf_, true, false);          // bar_index
    }

    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        if (sec_id < kSites) {
            open_[sec_id] = static_cast<double>(bar.timestamp);
            return;
        }
        if (bar.timestamp != label8_) {
            label8_ = bar.timestamp;
            ++index8_;
        }
    }

    void clear_security(int sec_id) override {
        if (sec_id < kSites) open_[sec_id] = kNaN;
    }

    void on_source_bar(const Bar&) override {
        Row row{current_bar_.timestamp, {}, index8_ < 0 ? kNaN : static_cast<double>(index8_)};
        for (int i = 0; i < kSites; ++i) row.open[i] = open_[i];
        rows.push_back(row);
    }

private:
    double open_[kSites] = {kNaN, kNaN, kNaN, kNaN, kNaN};
    std::int64_t label8_ = std::numeric_limits<std::int64_t>::min();
    int index8_ = -1;
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

struct Tape {
    std::vector<std::int64_t> bars;                           // every chart bar, ascending
    std::map<std::int64_t, std::vector<std::string>> readout;  // chart bar -> its fields
};

// A close fills at its own chart bar's close, so an exit row's time is the
// chart bar whose read-out its comment spells: t2|t3|t5|t8|t13|i8|bar_index.
Tape load_tape(const std::string& dir) {
    Tape tape;
    std::ifstream in(std::string(PINEFORGE_MULTI_DAY_BUCKETS_FIXTURE_DIR) + "/" + dir
                     + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);  // header
    std::map<std::int64_t, bool> seen;
    while (std::getline(in, line)) {
        const auto cols = split(line, ',');
        if (cols.size() < 4) continue;
        const std::int64_t t = parse_utc8(cols[2]);
        seen[t] = true;
        if (cols[1].rfind("Exit", 0) != 0) continue;
        const auto f = split(cols[3], '|');
        if (f.size() == 7) tape.readout[t] = f;
    }
    for (const auto& entry : seen) tape.bars.push_back(entry.first);
    return tape;
}

// TradingView's "#.####" read-out of an open time in epoch days against the
// engine's label; "na" against no bar.
bool same_open(const std::string& tv, double engine_ms) {
    if (tv == "na") return std::isnan(engine_ms);
    if (std::isnan(engine_ms)) return false;
    return std::fabs(std::stod(tv) - engine_ms / static_cast<double>(kDay)) <= 0.00005 + 1e-9;
}

bool same_index(const std::string& tv, double engine) {
    if (tv == "na") return std::isnan(engine);
    return !std::isnan(engine) && std::stod(tv) == engine;
}

std::string engine_days(double ms) {
    if (std::isnan(ms)) return "na";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", ms / static_cast<double>(kDay));
    return buf;
}

struct Lane {
    const char* tape;
    const char* timezone;
    const char* session;
    const char* type;
    std::int64_t range_start_ms;  // metrics.json wsProvenance.returnedRange.from
    std::int64_t compare_from_ms;
};

// Returns the number of chart bars compared against the tape.
int replay(const Lane& lane) {
    const Tape tape = load_tape(lane.tape);
    CHECK(tape.bars.size() > 200);
    std::vector<Bar> chart;
    chart.reserve(tape.bars.size());
    for (std::size_t i = 0; i < tape.bars.size(); ++i) {
        const double v = 100.0 + static_cast<double>(i % 17);
        chart.push_back({v, v + 1.0, v - 1.0, v, 1.0, tape.bars[i]});
    }

    MultiDayProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, lane.timezone);
    strategy_set_syminfo_session(handle, lane.session);
    strategy_set_syminfo_type(handle, lane.type);
    strategy_set_syminfo_metadata(handle, "security_range_start_na_warmup",
                                  static_cast<double>(lane.range_start_ms));
    probe.run(chart.data(), static_cast<int>(chart.size()), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty());
    CHECK(probe.rows.size() == chart.size());
    if (probe.rows.size() != chart.size()) return 0;

    int compared = 0;
    int reported = 0;
    // From a later year on, the 8D context's bar_index counts the earlier
    // years' bars too: compared from the first compared row's.
    double engine_base = 0.0, tv_base = 0.0;
    bool based = lane.compare_from_ms == 0;
    for (const auto& row : probe.rows) {
        if (row.time < lane.compare_from_ms) continue;
        const auto it = tape.readout.find(row.time);
        if (it == tape.readout.end()) continue;
        const auto& tv = it->second;
        if (!based && tv[5] != "na" && !std::isnan(row.index8)) {
            engine_base = row.index8;
            tv_base = std::stod(tv[5]);
            based = true;
        }
        bool ok = tv[5] == "na"
            ? std::isnan(row.index8)
            : same_index(std::to_string(static_cast<long long>(std::stod(tv[5]) - tv_base)),
                         row.index8 - engine_base);
        for (int i = 0; i < kSites; ++i) ok = ok && same_open(tv[i], row.open[i]);
        if (!ok && reported++ < 8) {
            std::printf("  %s bar %lld: engine %s|%s|%s|%s|%s|%s  tape %s|%s|%s|%s|%s|%s\n",
                        lane.tape, static_cast<long long>(row.time),
                        engine_days(row.open[0]).c_str(), engine_days(row.open[1]).c_str(),
                        engine_days(row.open[2]).c_str(), engine_days(row.open[3]).c_str(),
                        engine_days(row.open[4]).c_str(),
                        std::isnan(row.index8) ? "na" : std::to_string(
                            static_cast<long long>(row.index8)).c_str(),
                        tv[0].c_str(), tv[1].c_str(), tv[2].c_str(), tv[3].c_str(),
                        tv[4].c_str(), tv[5].c_str());
        }
        CHECK(ok);
        ++compared;
    }
    std::printf("  %s: %d chart bars compared\n", lane.tape, compared);
    return compared;
}

constexpr std::int64_t k2025 = 1735689600000;  // 2025-01-01 00:00 UTC

void test_every_day_on_a_24x7_symbol() {
    // 2024-01-01 .. 2026-05-01: the 8D bars open on day 0, 8, 16 of each year
    // (2024-01-01, 2025-01-01 + 8k, 2026-01-01 + 8k); 2025's last 8D bar runs
    // Dec 27 .. 31.
    const Lane eth{"pf-taila-ndgrid-eth-d", "UTC", "24x7", "crypto",
                   1704067200000, 0};
    CHECK(replay(eth) == 851);
}

void test_every_weekday_on_oanda() {
    // Weekdays, holidays too: the Jan 1 2024 bucket (no bar) opened before the
    // export's first bar, so "2D" first reads the 01-03 bucket.
    const Lane eur{"pf-taila-ndgrid-eurusd-d", "America/New_York", "1700-1700", "forex",
                   1704146400000, 0};
    CHECK(replay(eur) == 604);
    const Lane xau{"pf-taila-ndgrid-xauusd-d", "America/New_York", "1800-1700", "cfd",
                   1704146400000, 0};
    CHECK(replay(xau) == 601);
}

void test_exchange_sessions_on_a_stock_or_an_index() {
    const Lane aapl{"pf-taila-ndgrid-aapl-d", "America/New_York", "0930-1600", "stock",
                    1704205800000, k2025};
    CHECK(replay(aapl) == 332);
    const Lane f{"pf-taila-ndgrid-f-d", "America/New_York", "0930-1600", "stock",
                 1704205800000, k2025};
    CHECK(replay(f) == 332);
    // NSE traded on 2024-01-01, the chart's first bar: every year whole.
    const Lane nifty{"pf-taila-ndgrid-nifty-d", "Asia/Kolkata", "0915-1530", "index",
                     1704080700000, 0};
    CHECK(replay(nifty) == 577);
}

}  // namespace

int main() {
    std::printf("[tail-a] N-day request.security bars, on TradingView's tapes\n");
    test_every_day_on_a_24x7_symbol();
    test_every_weekday_on_oanda();
    test_exchange_sessions_on_a_stock_or_an_index();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
