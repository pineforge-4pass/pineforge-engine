// R5 lane D2-C: the Pine run shapes the lane's source-bound witnesses sweep
// (tests/test_adapter_runtime_ambient.cpp, tests/test_adapter_in_place_reads.cpp).
//
// Each scenario is a tape, the run's timeframe arguments and switches, and
// the request.security sites and feeds a generated strategy would register:
// the magnifier off and on, an aggregated chart with and without it,
// calc_on_order_fills re-entries, both warm-up switches, a kernel-routed and
// a host-driven request.security site, a lower-timeframe array over the
// auxiliary feed, the chart's native daily partition (the NQ tape's Memorial
// Day merge), the probe's suppressed tail and a stream. A witness's host
// derives from source::PineStrategyHost as generated code does, reads what it
// checks in on_source_bar / evaluate_security, and places its orders with
// place_orders, so fills, fill recalculations and applied callbacks happen.
// Every price is a whole number of quarter ticks. Source-bound.
#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "test_o_close_pct_day_anchor_data.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the D2-C scenarios drive the auxiliary request.security feed"
#endif

namespace d2c_scenarios {

using namespace pineforge;

constexpr std::int64_t kT0 = 1736121600000LL;  // a UTC midnight
constexpr std::int64_t kMinute = 60000;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

inline std::string bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    char text[20];
    std::snprintf(text, sizeof text, "%016" PRIx64, out);
    return text;
}

// A wave in quarter ticks, one bar per `step` minutes.
inline std::vector<Bar> wave(int count, int step, double base = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 12;
        const int triangle = phase < 6 ? phase : 12 - phase;
        const double p = base + 0.75 * triangle + 0.25 * (i % 5) + 0.5 * ((i / 40) % 3);
        bars.push_back({p, p + 0.75, p - 0.5, p + 0.25, 1.0 + (i % 7),
                        kT0 + static_cast<std::int64_t>(i) * step * kMinute});
    }
    return bars;
}

template <std::size_t N>
std::vector<Bar> vec(const Bar (&bars)[N]) {
    return std::vector<Bar>(bars, bars + N);
}

struct Site {
    int id = 0;
    std::string tf;
    bool lookahead = false;
    bool lower_array = false;
};

struct Scenario {
    std::string name;
    std::vector<Bar> bars;
    std::string input_tf;
    std::string script_tf;
    bool magnifier = false;
    std::string timezone = "UTC";
    std::string session = "24x7";
    std::string type;
    source::PineStrategyConfig config{};
    std::vector<std::pair<std::string, double>> metadata;
    std::vector<Site> sites;
    std::vector<Bar> daily;   // native "D" feed: the chart's day partition
    std::vector<Bar> aux;     // auxiliary request.security feed
    std::string aux_tf;
    bool suppress_tail = false;
    int stream_warmup = 0;    // > 0: stream these bars' tail
};

inline source::PineStrategyConfig base_config() {
    source::PineStrategyConfig config;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 2;
    config.commission_type = static_cast<int>(CommissionType::PERCENT);
    config.commission_value = 0.05;
    return config;
}

inline std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;
    const std::vector<Bar> minutes = wave(420, 1);
    {
        Scenario s{"chart-kernel-site", minutes, "1", "1"};
        s.config = base_config();
        s.sites = {{0, "5"}};
        out.push_back(s);
        s.name = "chart-magnifier";
        s.magnifier = true;
        out.push_back(s);
    }
    {
        Scenario s{"aggregated", minutes, "1", "5"};
        s.config = base_config();
        s.sites = {{0, "15"}};
        out.push_back(s);
        s.name = "aggregated-magnifier";
        s.magnifier = true;
        out.push_back(s);
    }
    {
        Scenario s{"coof", minutes, "1", "1"};
        s.config = base_config();
        s.config.calc_on_order_fills = true;
        s.sites = {{0, "5"}};
        out.push_back(s);
    }
    {
        Scenario s{"warmups", minutes, "1", "1"};
        s.config = base_config();
        s.metadata = {{"chart_ema_na_warmup", 1.0},
                      {"security_range_start_na_warmup",
                       static_cast<double>(kT0 + 30 * kMinute)}};
        s.sites = {{0, "5"}};
        out.push_back(s);
    }
    {
        Scenario s{"host-driven-site", minutes, "1", "1"};
        s.config = base_config();
        s.sites = {{0, "5", true}, {1, "10"}};
        out.push_back(s);
    }
    {
        Scenario s{"aux-lower-array", wave(90, 5), "5", "5"};
        s.config = base_config();
        s.aux = wave(460, 1, 100.25);
        s.aux_tf = "1";
        s.sites = {{0, "1", false, true}, {1, "1"}, {2, "1", true}};
        out.push_back(s);
    }
    {
        Scenario s{"daily-partition", vec(o_data::kNq15May), "15", "15"};
        s.config = base_config();
        s.timezone = "America/Chicago";
        s.session = "1700-1600";
        s.type = "futures";
        s.daily = vec(o_data::kNq1DMay);
        out.push_back(s);
    }
    {
        Scenario s{"suppressed-tail", minutes, "1", "1"};
        s.config = base_config();
        s.suppress_tail = true;
        s.sites = {{0, "5"}};
        out.push_back(s);
    }
    {
        Scenario s{"stream", minutes, "1", "1"};
        s.config = base_config();
        s.stream_warmup = 150;
        out.push_back(s);
    }
    return out;
}

// The host's configuration as a generated strategy's constructor and a
// runner's setters leave it.
inline void configure(source::PineStrategyHost& host, const Scenario& scenario) {
    host.set_syminfo_timezone(scenario.timezone);
    host.set_syminfo_session(scenario.session);
    if (!scenario.type.empty()) host.set_syminfo_type(scenario.type);
    host.set_syminfo_mintick(0.25);
    for (const auto& [key, value] : scenario.metadata) host.set_syminfo_metadata(key, value);
    if (scenario.suppress_tail) host.set_probe_suppress_tail_logic(true);
}

// Installs the scenario's feeds and runs it, batch or stream. False when a
// feed or a stream step was refused.
inline bool drive(source::PineStrategyHost& host, const Scenario& scenario) {
    bool ok = true;
    if (!scenario.daily.empty()) {
        ok = host.set_native_security_feed("D", scenario.daily.data(),
                                           static_cast<int>(scenario.daily.size())) && ok;
    }
    if (!scenario.aux.empty()) {
        ok = host.set_aux_security_feed(scenario.aux.data(),
                                        static_cast<int>(scenario.aux.size()),
                                        scenario.aux_tf) && ok;
    }
    const int n = static_cast<int>(scenario.bars.size());
    if (scenario.stream_warmup > 0) {
        ok = host.stream_begin(scenario.bars.data(), scenario.stream_warmup,
                               scenario.input_tf, scenario.script_tf) && ok;
        for (int i = scenario.stream_warmup; ok && i < n; ++i)
            ok = host.stream_push_bar(scenario.bars[static_cast<std::size_t>(i)]);
        ok = ok && host.stream_end();
    } else {
        host.run(scenario.bars.data(), n, scenario.input_tf, scenario.script_tf,
                 scenario.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    }
    return ok;
}

// The script's orders on source bar `index`: an entry, its bracket, a stop
// entry the other way and a flatten, every twelve bars.
inline void place_orders(source::PineStrategyHost& host, int index, const Bar& bar) {
    const int phase = index % 12;
    if (phase == 1) host.strategy_entry("L", true);
    if (phase == 3) host.strategy_exit("XL", "L", bar.close + 1.0, bar.close - 1.0);
    if (phase == 6) host.strategy_entry("S", false, kNa, bar.low - 0.25);
    if (phase == 9) host.strategy_close_all();
}

// Every closed trade as a transcript line.
inline void trade_lines(const BacktestEngine& engine, std::vector<std::string>& lines) {
    for (int i = 0; i < engine.trade_count(); ++i) {
        const Trade& trade = engine.get_trade(i);
        lines.push_back("trade " + std::to_string(trade.entry_time) + " "
            + std::to_string(trade.exit_time) + " " + bits(trade.entry_price) + " "
            + bits(trade.exit_price) + " " + bits(trade.qty) + " " + bits(trade.pnl) + " "
            + trade.entry_id + " " + trade.exit_id);
    }
}

}  // namespace d2c_scenarios
