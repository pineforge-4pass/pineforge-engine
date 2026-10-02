#include <pineforge/source/pine_strategy_host.hpp>

#include "px-f1-samebar-tapes.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();

class PxF1Host : public pineforge::source::PineStrategyHost {
public:
    explicit PxF1Host(const PxF1CaseTape& tape) : tape_(tape) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = tape.pyramiding;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_metadata("ETHUSDT.P", 0.01);
    }

    void on_source_bar(const pineforge::Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0 && tape_.seed != 0) {
            strategy_entry(tape_.prefix + "_seed", tape_.seed > 0, missing, missing,
                           std::abs(tape_.seed));
        }
        if (bar == 2) {
            for (std::size_t ordinal = 0; ordinal < tape_.sequence.size(); ++ordinal) {
                const bool is_long = tape_.sequence[ordinal] == 'L';
                const auto id = tape_.prefix + "_" + std::to_string(ordinal)
                    + "_" + tape_.sequence[ordinal];
                const double quantity = tape_.default_quantity ? missing : 1.0;
                strategy_entry(id, is_long, missing, missing, quantity);
            }
        }
        if (bar == 5) strategy_close_all();
    }

private:
    const PxF1CaseTape& tape_;
};

bool run_case(const PxF1CaseTape& tape) {
    PxF1Host host(tape);
    std::vector<pineforge::Bar> bars;
    for (int bar = 0; bar < 7; ++bar) {
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        tape.start_time + bar * 900000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    bool passed = host.last_error().empty() && host.live_position_size() == 0.0
        && host.trade_count() == static_cast<int>(tape.trades.size());
    for (std::size_t index = 0; index < tape.trades.size()
         && index < static_cast<std::size_t>(host.trade_count()); ++index) {
        const auto& expected = tape.trades[index];
        const auto& actual = host.get_trade(static_cast<int>(index));
        const bool closes_at_end = expected.exit_id == tape.prefix + "_end";
        passed = passed && actual.entry_id == expected.entry_id
            && actual.exit_id == (closes_at_end ? "__close__" : expected.exit_id)
            && actual.is_long == expected.is_long
            && std::abs(actual.qty - expected.quantity) < 1e-9
            && actual.entry_price == 100.0 && actual.exit_price == 100.0
            && actual.pnl == 0.0
            && actual.entry_time == expected.entry_time
            && actual.exit_time == expected.exit_time;
    }
    if (!passed) {
        std::printf("FAIL %s: expected %zu trades, got %d, error=%s\n",
                    tape.name.c_str(), tape.trades.size(), host.trade_count(),
                    host.last_error().c_str());
        for (int index = 0; index < host.trade_count(); ++index) {
            const auto& trade = host.get_trade(index);
            std::printf("  %s %s %.8f -> %s %lld/%lld\n", trade.entry_id.c_str(),
                        trade.is_long ? "long" : "short", trade.qty,
                        trade.exit_id.c_str(), static_cast<long long>(trade.entry_time),
                        static_cast<long long>(trade.exit_time));
        }
    }
    return passed;
}

}

int main(int argc, char** argv) {
    int passed = 0;
    int failed = 0;
    for (const auto& tape : px_f1_tapes) {
        if (argc > 1 && tape.name.find(argv[1]) == std::string::npos) continue;
        if (run_case(tape)) ++passed;
        else ++failed;
    }
    std::printf("px-f1-samebar-entries: %d passed, %d failed\n", passed, failed);
    return failed == 0 && passed > 0 ? 0 : 1;
}
