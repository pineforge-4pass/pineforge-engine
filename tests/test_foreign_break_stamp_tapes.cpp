// Requests of other symbols on a daily chart stamped in its session's break
// (lane TAIL-G), through the Pine host's foreign request sites.
//
// OANDA stamps its XAUUSD daily bars at 17:00 ET, inside the break of the
// 1800-1700 session, and the kernel reads such a label as the session that
// closes at it (test_daily_stamp_in_the_break, tests/test_native_instrument_
// feed.cpp). TradingView merges another symbol's bars by the chart bar's own
// period instead (read_ahead_foreign_security_sites, src/source/
// pine_security_eval.cpp): a lookahead-off request reads the last requested
// bar closed by the chart bar's close, the next 17:00 ET, and a lookahead-on
// request of an intraday timeframe the last one opened by the chart bar's
// time.
//
// Tapes (tests/fixtures/daily_break_close, `lab tv --no-note`, see its
// README): tg-xau1d-requests (OANDA:XAUUSD 1D, 2025-04-01 .. 2026-05-01:
// lookahead-off D of CBOE:VIX, TVC:US10Y, CRYPTOCAP:USDT.D and TVC:DXY,
// lookahead-on 60 and 240 of TVC:VIX and CRYPTOCAP:USDT.D, 240 of TVC:DXY),
// w11-tclose-xau1d (lookahead-off D of TVC:DXY) and w11-tclose-la-xau1d
// (lookahead-off and lookahead-on 60 of TVC:DXY; its lookahead-on D column,
// which TradingView aligns by trading date, is not this rule's). A reading
// spells, in minutes, the chart bar's time minus the requested bar's time and
// time_close. Per shape, the requested bars TradingView read are rebuilt from
// the readings -- the tape's times, synthetic prices -- and installed as a
// feed under a synthetic key; a Pine host on the tape's chart bars reads each
// bar's requested time and time_close through its foreign site, and every
// read must be TradingView's. The kernel's reading of the label alone was a
// requested bar late (lookahead off) or a session early (lookahead on).

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible (engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS).
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "exit_comment_tape.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_DAILY_BREAK_CLOSE_FIXTURE_DIR
#error "PINEFORGE_DAILY_BREAK_CLOSE_FIXTURE_DIR must name tests/fixtures/daily_break_close"
#endif

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                          \
        }                                                                      \
    } while (false)

constexpr std::int64_t kMinute = exit_comment_tape::kMinute;

// One request site of a tape: the reading's '|' field that spells it, and
// the synthetic key and timeframe its rebuilt feed is installed under.
struct Shape {
    std::size_t field;
    const char* key;
    const char* tf;
    bool lookahead;
};

// The requested bar a reading names, from the chart bar's time: {time,
// time_close}; {na, na} for TradingView's "n,n".
std::pair<std::int64_t, std::int64_t> requested_bar(std::int64_t chart_ms,
                                                    const std::string& field, bool& ok) {
    const auto minutes = exit_comment_tape::split(field, ',');
    if (minutes.size() != 2) {
        ok = false;
        return {na<std::int64_t>(), na<std::int64_t>()};
    }
    if (minutes[0] == "n" || minutes[1] == "n") return {na<std::int64_t>(), na<std::int64_t>()};
    return {chart_ms - std::atoll(minutes[0].c_str()) * kMinute,
            chart_ms - std::atoll(minutes[1].c_str()) * kMinute};
}

pf_strategy_t handle(BacktestEngine& engine) { return static_cast<void*>(&engine); }

// Each foreign site's requested time and time_close, as the chart bar reads
// them once its script runs.
class Reader final : public source::PineStrategyHost {
public:
    std::vector<Shape> shapes;
    std::vector<std::int64_t> open_now, close_now;
    std::vector<std::vector<std::int64_t>> opens, closes;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        open_now.assign(shapes.size(), na<std::int64_t>());
        close_now.assign(shapes.size(), na<std::int64_t>());
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            register_security_eval(static_cast<int>(s), shapes[s].key, shapes[s].tf, input_tf_,
                                   shapes[s].lookahead, false, false);
        }
    }
    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        open_now[static_cast<std::size_t>(sec_id)] = bar.timestamp;
        close_now[static_cast<std::size_t>(sec_id)] = time_close();
    }
    void clear_security(int) override {}
    void on_source_bar(const Bar&) override {
        opens.push_back(open_now);
        closes.push_back(close_now);
    }
};

// Replays one tape's shapes, each read checked against TradingView's.
void replay(const char* slug, const std::vector<Shape>& shapes) {
    scenario = slug;
    bool ok = true;
    const auto readings = exit_comment_tape::read(PINEFORGE_DAILY_BREAK_CLOSE_FIXTURE_DIR, slug, ok);
    CHECK(ok);
    // The chart: one synthetic bar at each reading's chart bar.
    std::vector<Bar> chart;
    for (const auto& reading : readings) {
        if (!chart.empty() && chart.back().timestamp == reading.bar_ms) continue;
        Bar bar{};
        bar.open = bar.high = bar.low = bar.close = 3000.0 + static_cast<double>(chart.size());
        bar.volume = 1.0;
        bar.timestamp = reading.bar_ms;
        chart.push_back(bar);
    }
    // Per shape: TradingView's read on each chart bar, and the bars it read.
    std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>> expected(shapes.size());
    Reader reader;
    reader.shapes = shapes;
    reader.set_syminfo_timezone("America/New_York");
    reader.set_syminfo_session("1800-1700");
    reader.set_syminfo_type("cfd");
    reader.set_syminfo_string("tickerid", "TEST:XAUUSD");
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        std::map<std::int64_t, std::int64_t> read_bars;
        for (const auto& reading : readings) {
            const auto fields = exit_comment_tape::split(reading.signal, '|');
            if (fields.size() <= shapes[s].field) {
                ok = false;
                continue;
            }
            const auto bar = requested_bar(reading.bar_ms, fields[shapes[s].field], ok);
            expected[s].push_back(bar);
            if (is_na(bar.first)) continue;
            const auto known = read_bars.emplace(bar.first, bar.second);
            CHECK(known.first->second == bar.second);
        }
        std::vector<pf_bar_t> bars;
        std::vector<std::int64_t> close_ms;
        for (const auto& [open, close] : read_bars) {
            pf_bar_t bar{};
            bar.open = bar.high = bar.low = bar.close = 100.0 + static_cast<double>(bars.size());
            bar.volume = 1.0;
            bar.timestamp = open;
            bars.push_back(bar);
            close_ms.push_back(close);
        }
        CHECK(!bars.empty());
        CHECK(strategy_set_symbol_feed(handle(reader), shapes[s].key, shapes[s].tf, bars.data(),
                                       close_ms.data(), static_cast<int>(bars.size()))
              == 0);
    }
    CHECK(ok);
    reader.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(reader.last_error().empty());
    CHECK(reader.opens.size() == chart.size());
    if (reader.opens.size() != chart.size()) return;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        int compared = 0;
        int differ = 0;
        std::size_t k = 0;
        for (std::size_t j = 0; j < chart.size(); ++j) {
            // A chart bar with several readings reads one bar: the same pair.
            while (k < readings.size() && readings[k].bar_ms == chart[j].timestamp) {
                const auto& want = expected[s][k++];
                ++compared;
                const bool same = reader.opens[j][s] == want.first
                                  && reader.closes[j][s] == want.second;
                if (!same && differ++ == 0) {
                    std::printf("  %s field %zu at %lld: read [%lld, %lld], TradingView [%lld, %lld]\n",
                                slug, shapes[s].field, static_cast<long long>(chart[j].timestamp),
                                static_cast<long long>(reader.opens[j][s]),
                                static_cast<long long>(reader.closes[j][s]),
                                static_cast<long long>(want.first),
                                static_cast<long long>(want.second));
                }
            }
        }
        std::printf("  %s field %zu (%s %s, lookahead %s): %d of %d reads differ\n", slug,
                    shapes[s].field, shapes[s].key, shapes[s].tf,
                    shapes[s].lookahead ? "on" : "off", differ, compared);
        CHECK(compared > 0);
        CHECK(differ == 0);
    }
}

}  // namespace

int main() {
    replay("tg-xau1d-requests", {{1, "SYN:VIXD", "D", false},
                                 {2, "SYN:US10Y", "D", false},
                                 {3, "SYN:USDTD", "D", false},
                                 {4, "SYN:DXY", "D", false},
                                 {5, "SYN:VIX", "60", true},
                                 {6, "SYN:VIX", "240", true},
                                 {7, "SYN:DXY", "240", true},
                                 {8, "SYN:USDTD", "60", true},
                                 {9, "SYN:USDTD", "240", true}});
    replay("w11-tclose-xau1d", {{1, "SYN:DXY", "D", false}});
    replay("w11-tclose-la-xau1d", {{1, "SYN:DXYOFF", "60", false}, {2, "SYN:DXYON", "60", true}});
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
