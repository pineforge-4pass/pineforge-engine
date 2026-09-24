// R5 lane D2-C (stage once): the Pine host's fills settled from the one-lot
// stage their precommit preview took, against the same fills staged again.
//
// The Pine adapter implements validate_execution_precommit, so every fill it
// books is previewed first, and its hook refreshes the lot excursions its
// closed_lot_excursion answers: the close row must still be built at the
// settlement (tests/test_native_settlement_carry.cpp holds the kernel's side
// of the carry). Every D2-C scenario (tests/pine_d2c_scenarios_fixture.hpp:
// magnifier off and on, aggregated charts, calc_on_order_fills, warm-ups,
// request.security sites, the daily partition, the suppressed tail, a
// stream), its script's orders with a partial close added, and a margin
// variant whose calls liquidate, runs with the consumer's settlement carry on
// and off (NativeExecutionConsumer::set_settlement_carry): every line -- the
// book at every script bar, every trade with its commission and excursions,
// the event count, the broker-state and continuation hashes -- is identical,
// and the carry took settlements in the first run and none in the second.
//
// Fail-before: at the lane's base NativeExecutionConsumer has no
// set_settlement_carry, so this TU does not compile there (the lane report
// records the first diagnostic).
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
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

class CarryHost final : public source::PineStrategyHost {
public:
    explicit CarryHost(const Scenario& scenario) : scenario_(scenario) {
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
        const NativePhysicalPosition position = physical_position();
        lines.push_back("bar " + std::to_string(index) + " " + std::to_string(current_bar_.timestamp)
                        + (history_advances_new_bar() ? " new " : " again ")
                        + bits(position.signed_units) + "/" + bits(position.average_price) + "/"
                        + std::to_string(position.lot_count) + " trades="
                        + std::to_string(trade_count()));
        if (!history_advances_new_bar()) return;
        place_orders(*this, index, bar);
        // A partial close of the long entry: the book keeps a survivor.
        if (index % 12 == 7) strategy_close("L", "", kNa, 50.0);
    }

    void evaluate_security(int, const Bar&, bool) override {}
    void clear_security(int) override {}

private:
    const Scenario& scenario_;
};

// One-minute bars at 100 that gap down three points for three bars of every
// twelve: the long the script enters at phase 1 and the short it stops into
// at the gap both meet a three-point move against them.
std::vector<Bar> gap_tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 12;
        const double p = (phase >= 3 && phase <= 5 ? 97.0 : 100.0) + 0.25 * (i % 3);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0 + (i % 5),
                        kT0 + static_cast<std::int64_t>(i) * kMinute});
    }
    return bars;
}

std::vector<Scenario> carry_scenarios() {
    std::vector<Scenario> out = scenarios();
    Scenario margin{"margin-calls", gap_tape(420), "1", "1"};
    margin.config = base_config();
    // Nine times the equity on a tenth's margin: every gap calls the position,
    // and the next entry is sized to what the call left.
    margin.config.initial_capital = 1000.0;
    margin.config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    margin.config.default_qty_value = 900.0;
    margin.config.margin_long = 10.0;
    margin.config.margin_short = 10.0;
    out.push_back(margin);
    margin.name = "margin-calls-magnifier";
    margin.magnifier = true;
    out.push_back(margin);
    return out;
}

struct Result {
    std::vector<std::string> lines;
    std::uint64_t carried = 0;
    int liquidations = 0;
    std::string error;
};

Result run(const Scenario& scenario, bool carry) {
    CarryHost host(scenario);
    host.consumer().set_settlement_carry(carry);
    CHECK(drive(host, scenario));
    Result result;
    result.error = host.last_error();
    result.carried = host.consumer().carried_settlements();
    result.lines = std::move(host.lines);
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& trade = host.get_trade(i);
        if (trade.close_cause == execution::CloseCause::Liquidation) ++result.liquidations;
        result.lines.push_back("trade " + std::to_string(trade.entry_time) + " "
            + std::to_string(trade.exit_time) + " " + bits(trade.entry_price) + " "
            + bits(trade.exit_price) + " " + bits(trade.qty) + " " + bits(trade.pnl) + " "
            + bits(trade.commission) + " " + bits(trade.max_runup) + " "
            + bits(trade.max_drawdown) + " " + trade.entry_id + " " + trade.exit_id + " "
            + std::to_string(static_cast<int>(trade.close_cause)));
    }
    result.lines.push_back("events " + std::to_string(host.native_events(0).size()));
    result.lines.push_back("broker " + std::to_string(host.broker_state_hash()));
    result.lines.push_back("continuation " + std::to_string(host.consumer().continuation_hash()));
    return result;
}

void carried_and_staged_again_agree() {
    long lines = 0;
    std::uint64_t carried = 0;
    int runs = 0;
    for (const Scenario& scenario : carry_scenarios()) {
        const Result on = run(scenario, true);
        const Result off = run(scenario, false);
        CHECK(on.error.empty());
        CHECK(off.error.empty());
        CHECK(on.lines.size() > 50);
        CHECK(on.lines.size() == off.lines.size());
        const std::size_t common = std::min(on.lines.size(), off.lines.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (on.lines[i] != off.lines[i]) {
                std::fprintf(stderr, "  %s: first divergence at line %zu\n    carried %s\n"
                             "    staged  %s\n", scenario.name.c_str(), i,
                             on.lines[i].c_str(), off.lines[i].c_str());
                ++failures;
                break;
            }
        }
        CHECK(on.carried > 0);
        CHECK(off.carried == 0);
        // The margin variants' calls liquidate.
        if (scenario.config.margin_long < 100.0) CHECK(on.liquidations > 0);
        std::printf("  %-24s %5zu lines, %4" PRIu64 " settlements carried, %d liquidations\n",
                    scenario.name.c_str(), on.lines.size(), on.carried, on.liquidations);
        lines += static_cast<long>(on.lines.size());
        carried += on.carried;
        ++runs;
    }
    std::printf("pine hosts: %d scenarios, %ld transcript lines identical with and without the "
                "carry, %" PRIu64 " settlements carried\n", runs, lines, carried);
}

}  // namespace

int main() {
    carried_and_staged_again_agree();
    if (failures != 0) {
        std::fprintf(stderr, "test_adapter_settlement_carry: %d failure(s) in %ld checks\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_adapter_settlement_carry: ok (%ld checks)\n", checks);
    return 0;
}
