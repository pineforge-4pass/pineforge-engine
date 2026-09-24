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
#include <pineforge/session_time.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/runtime_ambient.hpp"
#include "pine_d2c_scenarios_fixture.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using namespace d2c_scenarios;

int failures = 0;
long checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// A generated-strategy-shaped host whose body reads the runtime state.
class AmbientPineHost final : public source::PineStrategyHost {
public:
    explicit AmbientPineHost(const Scenario& scenario) : scenario_(scenario) {
        attach_pine_execution_adapter();
        configure_pine_strategy(scenario.config);
        d2c_scenarios::configure(*this, scenario);
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
        place_orders(*this, index, bar);
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
    CHECK(drive(host, scenario));
    Result result;
    result.error = host.last_error();
    result.lines = std::move(host.lines);
    trade_lines(host, result.lines);
    result.lines.push_back("broker " + std::to_string(host.broker_state_hash()));
    result.lines.push_back("continuation " + std::to_string(host.consumer().continuation_hash()));
    result.installs = host.consumer().runtime_ambient_installs();
    result.thread_owns = internal::tl_runtime_ambient.installed == nullptr;
    const ta::BarContext context = ta::bar_context();
    result.thread_default = !context.installed && context.bar_index == 0 && context.origin == 0
        && !ta::ema_na_warmup_flag() && active_native_day_partition() == nullptr;
    return result;
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
