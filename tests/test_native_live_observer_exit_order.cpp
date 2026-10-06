#include <pineforge/source/pine_strategy_host.hpp>

#include "json.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" int equivalence_export_actions(void*, const char*);

namespace {

namespace orders = pineforge::native_order;

class SiblingExits final : public pineforge::source::PineStrategyHost {
public:
    SiblingExits() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config{};
        config.initial_capital = 10000.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_chart_timezone("UTC");
        fixture_retain_all_events();
    }

    void on_source_bar(const pineforge::Bar&) override {
        const double absent = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 0) strategy_entry("SHORT", false, absent, absent, 2.0);
        if (pine_bar_index() == 1) {
            strategy_exit("TP", "SHORT", absent, 110.0, absent, absent, absent, 100.0, {}, 1.0);
            strategy_exit("RUNNER", "SHORT", absent, 105.0, absent, absent, absent, 100.0, {}, 1.0);
        }
    }
};

}

int main() {
    SiblingExits host;
    const std::vector<pineforge::Bar> bars{
        {100, 100, 100, 100, 1, 1704067200000LL},
        {100, 100, 100, 100, 1, 1704067260000LL},
        {100, 115, 99, 103, 1, 1704067320000LL},
        {103, 103, 103, 103, 1, 1704067380000LL}};
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             pineforge::MagnifierDistribution::ENDPOINTS);
    assert(host.last_error().empty());
    std::vector<const orders::ExecutionAppliedEvent*> receipts;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* applied = std::get_if<orders::ExecutionAppliedEvent>(&*event.command)) {
            if (applied->closed_trade_count) receipts.push_back(applied);
        }
    }
    assert(receipts.size() == 2);
    assert(receipts[0]->request().label == "RUNNER");
    assert(receipts[1]->request().label == "TP");
    assert(host.closed_trade(0).exit_id == "TP");
    assert(host.closed_trade(1).exit_id == "RUNNER");
    const auto path = std::filesystem::temp_directory_path()
        / ("pineforge-observer-exits-" + std::to_string(getpid()) + ".jsonl");
    assert(equivalence_export_actions(&host, path.c_str()) == 0);
    std::ifstream input(path);
    std::string line;
    std::size_t index = 0;
    while (std::getline(input, line)) {
        const auto record = pineforge::live::parse_json(line);
        const auto& order = record.at("order");
        if (order.at("leg").value != "exit") continue;
        assert(index < receipts.size());
        const auto& receipt = *receipts[index++];
        assert(order.at("id").value == receipt.request().label);
        assert(order.at("price").real() == receipt.resolved_price);
        assert(order.at("contracts").real() == receipt.closed_units);
        assert(order.at("action").value == "buy");
        assert(record.at("timestamp").integer<std::int64_t>() == receipt.effective_time_ms());
        assert(record.at("bar_index").integer<int>() == receipt.interval_index());
    }
    assert(index == 2);
    std::filesystem::remove(path);
    std::puts("PASS sibling exits retain RUNNER then TP receipt metadata after report sorting");
}
