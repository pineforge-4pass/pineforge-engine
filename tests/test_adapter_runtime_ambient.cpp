// R5 lane D2-C: a Pine host's publications write the pump's runtime block,
// and every value a script reads is the value it read through thread-local
// storage.
//
// PineStrategyHost::scheduler_publish_source_bar sets the chart day
// partition, the EMA seeding default and the TA bar context around every
// script bar it publishes; the probe's suppressed tail sets the partition; the
// request.security evaluators set the seeding default (the Pine sites) and
// the bar context (the kernel's dispatch) around every evaluation. Each now
// lands on the consumer's runtime block (NativeExecutionConsumer::
// pump_ambient, src/runtime_ambient.hpp) instead of thread-local storage.
//
// Every scenario runs on two fresh hosts -- with the block (shipped) and with
// set_runtime_ambient(false) (thread-local storage at every write, the path
// before the lane) -- and the two transcripts must be the same line for line:
// at every source callback the three states as the script reads them and the
// values of TA members that read them (bar-addressed extremum rings every
// bar and on a cadence, EMAs latching their seeding at their first compute,
// ta.vwap's session anchor, time("D"), timeframe.change("1D") and the session
// day under the chart's native daily partition), the same inside every
// request.security evaluation, and the trades, the broker-state hash and the
// continuation at the end. The block runs install the block and the
// reference runs never do; after every run the thread holds its own state.
//
// Scenarios: the magnifier off and on, an aggregated chart with and without
// it, calc_on_order_fills re-entries, both warm-up switches, a kernel-routed
// and a host-driven request.security site, a lower-timeframe array over the
// auxiliary feed, the chart's native daily partition (a CME holiday merged
// into one trade date), the probe's suppressed tail and a stream.
// test_native_runtime_ambient.cpp holds the mechanism itself on bare hosts.
//
// Fail-before: at the lane's base the consumer has no set_runtime_ambient,
// so this TU does not compile there (the lane report records the first
// diagnostic).
#include <pineforge/pineforge.h>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/runtime_ambient.hpp"
#include "test_o_close_pct_day_anchor_data.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the runtime-ambient witness drives the auxiliary request.security feed"
#endif

namespace {
using namespace pineforge;

int failures = 0;
long checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t kT0 = 1736121600000LL;  // a UTC midnight
constexpr std::int64_t kMinute = 60000;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

std::string bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    char text[20];
    std::snprintf(text, sizeof text, "%016" PRIx64, out);
    return text;
}

// A wave in quarter ticks, one bar per `step` minutes.
std::vector<Bar> wave(int count, int step, double base = 100.0) {
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

source::PineStrategyConfig base_config() {
    source::PineStrategyConfig config;
    config.initial_capital = 100000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.pyramiding = 2;
    config.commission_type = static_cast<int>(CommissionType::PERCENT);
    config.commission_value = 0.05;
    return config;
}

// A generated-strategy-shaped host whose body reads the runtime state.
class AmbientPineHost final : public source::PineStrategyHost {
public:
    explicit AmbientPineHost(const Scenario& scenario) : scenario_(scenario) {
        attach_pine_execution_adapter();
        set_syminfo_timezone(scenario.timezone);
        set_syminfo_session(scenario.session);
        if (!scenario.type.empty()) set_syminfo_type(scenario.type);
        set_syminfo_mintick(0.25);
        configure_pine_strategy(scenario.config);
        for (const auto& [key, value] : scenario.metadata) set_syminfo_metadata(key, value);
        if (scenario.suppress_tail) set_probe_suppress_tail_logic(true);
    }

    std::vector<std::string> lines;

    NativeExecutionConsumer& consumer() { return NativeExecutionConsumer::bound(*this); }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (const Site& site : scenario_.sites) {
            if (site.lower_array) register_security_lower_tf_eval(site.id, site.tf, input_tf_);
            else register_security_eval(site.id, site.tf, input_tf_, site.lookahead, false);
        }
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        const std::int64_t ts = current_bar_.timestamp;
        const bool fresh = history_advances_new_bar();
        std::string line = "bar " + std::to_string(index) + " " + std::to_string(ts)
            + (fresh ? " new" : " again") + state();
        const auto ta = [&](auto& member, double value) {
            line += " " + bits(fresh ? member.compute(value) : member.recompute(value));
        };
        ta(hi_, bar.high);
        if (index % 3 == 1) ta(lo_, bar.low);
        ta(highest_bars_, bar.close);
        ta(ema_first_, bar.close);
        if (index >= 9) ta(ema_late_, bar.close);
        const double hlc3 = (bar.high + bar.low + bar.close) / 3.0;
        const double vwap = fresh
            ? vwap_.compute(hlc3, bar.volume, ts, syminfo_.timezone, syminfo_.session)
            : vwap_.recompute(hlc3, bar.volume, ts, syminfo_.timezone, syminfo_.session);
        line += " vwap=" + bits(vwap);
        line += " timeD=" + std::to_string(pine_time(ts, "D", "", "", script_tf_,
                                                     syminfo_.timezone, syminfo_.session));
        line += " tfc=" + std::to_string(tf_change(prev_bar_timestamp_, ts, "1D",
                                                   syminfo_.timezone, syminfo_.session));
        line += " day=" + std::to_string(session_day_index(ts, syminfo_.timezone,
                                                           syminfo_.session));
        for (const Site& site : scenario_.sites) {
            line += " sec" + std::to_string(site.id) + "=" + bits(security_value_[site.id]);
        }
        lines.push_back(line);
        if (!fresh) return;
        // Orders, so fills, recalculations and applied callbacks happen.
        const int phase = index % 12;
        if (phase == 1) strategy_entry("L", true);
        if (phase == 3) strategy_exit("XL", "L", bar.close + 1.0, bar.close - 1.0);
        if (phase == 6) strategy_entry("S", false, kNa, bar.low - 0.25);
        if (phase == 9) strategy_close_all();
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        const bool fresh = security_series_slot_is_new(sec_id);
        std::string line = "sec " + std::to_string(sec_id) + " " + std::to_string(bar.timestamp)
            + (is_complete ? " complete" : " partial") + (fresh ? " new" : " again") + state();
        const double hi = fresh ? security_hi_.compute(bar.high) : security_hi_.recompute(bar.high);
        const double ema = fresh ? security_ema_.compute(bar.close)
                                 : security_ema_.recompute(bar.close);
        line += " " + bits(hi) + " " + bits(ema)
            + " sub=" + std::to_string(security_lower_tf_sub_bar_index(sec_id));
        lines.push_back(line);
        if (sec_id >= 0 && sec_id < 4) security_value_[sec_id] = ema;
    }

    void clear_security(int sec_id) override {
        if (sec_id >= 0 && sec_id < 4) security_value_[sec_id] = kNa;
    }

private:
    std::string state() const {
        const ta::BarContext context = ta::bar_context();
        const NativeDayPartition* partition = active_native_day_partition();
        const int which = partition == nullptr ? 0 : (partition == &chart_day_partition_ ? 1 : 2);
        return " ctx=" + std::to_string(context.installed) + "/"
            + std::to_string(context.bar_index) + "/" + std::to_string(context.origin)
            + " ema=" + std::to_string(ta::ema_na_warmup_flag())
            + " part=" + std::to_string(which);
    }

    const Scenario& scenario_;
    ta::Highest hi_{5};
    ta::Lowest lo_{4};
    ta::HighestBars highest_bars_{6};
    ta::EMA ema_first_{5};
    ta::EMA ema_late_{5};
    ta::VWAP vwap_;
    ta::Highest security_hi_{3};
    ta::EMA security_ema_{4};
    double security_value_[4] = {kNa, kNa, kNa, kNa};
};

struct Result {
    std::vector<std::string> lines;
    std::uint64_t installs = 0;
    bool thread_owns = false;
    bool thread_default = false;
    std::string error;
};

Result run(const Scenario& scenario, bool block) {
    AmbientPineHost host(scenario);
    host.consumer().set_runtime_ambient(block);
    if (!scenario.daily.empty()) {
        CHECK(host.set_native_security_feed("D", scenario.daily.data(),
                                            static_cast<int>(scenario.daily.size())));
    }
    if (!scenario.aux.empty()) {
        CHECK(host.set_aux_security_feed(scenario.aux.data(),
                                         static_cast<int>(scenario.aux.size()),
                                         scenario.aux_tf));
    }
    const int n = static_cast<int>(scenario.bars.size());
    if (scenario.stream_warmup > 0) {
        CHECK(host.stream_begin(scenario.bars.data(), scenario.stream_warmup,
                                scenario.input_tf, scenario.script_tf));
        for (int i = scenario.stream_warmup; i < n; ++i)
            CHECK(host.stream_push_bar(scenario.bars[static_cast<std::size_t>(i)]));
        CHECK(host.stream_end());
    } else {
        host.run(scenario.bars.data(), n, scenario.input_tf, scenario.script_tf,
                 scenario.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    }
    Result result;
    result.error = host.last_error();
    result.lines = std::move(host.lines);
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& trade = host.get_trade(i);
        result.lines.push_back("trade " + std::to_string(trade.entry_time) + " "
            + std::to_string(trade.exit_time) + " " + bits(trade.entry_price) + " "
            + bits(trade.exit_price) + " " + bits(trade.qty) + " " + bits(trade.pnl) + " "
            + trade.entry_id + " " + trade.exit_id);
    }
    result.lines.push_back("broker " + std::to_string(host.broker_state_hash()));
    result.lines.push_back("continuation " + std::to_string(host.consumer().continuation_hash()));
    result.installs = host.consumer().runtime_ambient_installs();
    result.thread_owns = internal::tl_runtime_ambient.installed == nullptr;
    const ta::BarContext context = ta::bar_context();
    result.thread_default = !context.installed && context.bar_index == 0 && context.origin == 0
        && !ta::ema_na_warmup_flag() && active_native_day_partition() == nullptr;
    return result;
}

std::vector<Scenario> scenarios() {
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

void block_matches_thread_local_storage() {
    long lines = 0;
    int runs = 0;
    for (const Scenario& scenario : scenarios()) {
        const Result block = run(scenario, true);
        const Result reference = run(scenario, false);
        CHECK(block.error.empty());
        CHECK(reference.error.empty());
        if (!block.error.empty()) std::fprintf(stderr, "  %s: %s\n", scenario.name.c_str(),
                                               block.error.c_str());
        CHECK(block.lines.size() > 50);
        CHECK(block.lines.size() == reference.lines.size());
        const std::size_t common = std::min(block.lines.size(), reference.lines.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (block.lines[i] != reference.lines[i]) {
                std::fprintf(stderr, "  %s: first divergence at line %zu\n    block     %s\n"
                             "    reference %s\n", scenario.name.c_str(), i,
                             block.lines[i].c_str(), reference.lines[i].c_str());
                ++failures;
                break;
            }
        }
        CHECK(block.installs > 0);
        CHECK(reference.installs == 0);
        CHECK(block.thread_owns);
        CHECK(reference.thread_owns);
        CHECK(block.thread_default);
        CHECK(reference.thread_default);
        lines += static_cast<long>(block.lines.size());
        ++runs;
        std::printf("  %-20s %5zu lines, %3" PRIu64 " installs\n", scenario.name.c_str(),
                    block.lines.size(), block.installs);
    }
    std::printf("pine hosts: %d scenarios, %ld transcript lines identical with and without "
                "the block\n", runs, lines);
}

// The scenarios' transcripts do read what the witness says they read: the
// chart partition inside publications, a raised seeding default, the kernel
// and host security dispatches, and the magnifier's and aggregation's bars.
void the_scenarios_reach_every_scope() {
    bool partition = false, raised = false, security_ctx = false, suppressed = false;
    for (const Scenario& scenario : scenarios()) {
        const Result block = run(scenario, true);
        for (const std::string& line : block.lines) {
            if (line.rfind("bar ", 0) == 0 && line.find(" part=1") != std::string::npos)
                partition = true;
            if (line.find(" ema=1") != std::string::npos) raised = true;
            if (line.rfind("sec ", 0) == 0 && line.find(" ctx=1/") != std::string::npos)
                security_ctx = true;
        }
        if (scenario.suppress_tail) {
            // The suppressed tail publishes no script bar: one fewer bar line.
            Scenario plain = scenario;
            plain.suppress_tail = false;
            const Result unsuppressed = run(plain, true);
            std::size_t bars = 0, plain_bars = 0;
            for (const auto& line : block.lines) bars += line.rfind("bar ", 0) == 0;
            for (const auto& line : unsuppressed.lines) plain_bars += line.rfind("bar ", 0) == 0;
            suppressed = bars + 1 == plain_bars;
        }
    }
    CHECK(partition);
    CHECK(raised);
    CHECK(security_ctx);
    CHECK(suppressed);
}

}  // namespace

int main() {
    block_matches_thread_local_storage();
    the_scenarios_reach_every_scope();
    CHECK(internal::tl_runtime_ambient.installed == nullptr);
    if (failures != 0) {
        std::fprintf(stderr, "test_adapter_runtime_ambient: %d failure(s) in %ld checks\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_adapter_runtime_ambient: ok (%ld checks)\n", checks);
    return 0;
}
