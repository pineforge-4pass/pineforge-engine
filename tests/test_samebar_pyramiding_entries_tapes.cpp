#include <pineforge/source/pine_strategy_host.hpp>

#include "samebar_pyramiding_entries_tapes.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();

class SamebarPyramidingHost : public pineforge::source::PineStrategyHost {
public:
    explicit SamebarPyramidingHost(const SamebarPyramidingCaseTape& tape) : tape_(tape) {
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
                if (tape_.sequence[ordinal] == 'C') {
                    strategy_close(tape_.prefix + "_seed");
                    continue;
                }
                const bool is_long = tape_.sequence[ordinal] == 'L';
                const auto id = tape_.prefix + "_" + std::to_string(ordinal)
                    + "_" + tape_.sequence[ordinal];
                const double quantity = tape_.default_quantity ? missing : 1.0;
                strategy_entry(id, is_long, missing, missing, quantity);
            }
        }
        if (bar == 3) position_after_batch_ = live_position_size();
        if (bar == 5) strategy_close_all();
    }

    double position_after_batch() const { return position_after_batch_; }

private:
    const SamebarPyramidingCaseTape& tape_;
    double position_after_batch_ = missing;
};

bool run_case(const SamebarPyramidingCaseTape& tape, bool flat_after_batch_only = false) {
    SamebarPyramidingHost host(tape);
    std::vector<pineforge::Bar> bars;
    for (int bar = 0; bar < 7; ++bar) {
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        tape.start_time + bar * 900000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    bool passed = host.last_error().empty() && host.live_position_size() == 0.0;
    if (flat_after_batch_only) {
        passed = passed && host.position_after_batch() == 0.0;
        for (int index = 0; index < host.trade_count(); ++index) {
            passed = passed && host.get_trade(index).exit_id != "__close__";
        }
    } else {
        passed = passed && host.trade_count() == static_cast<int>(tape.trades.size());
    }
    for (std::size_t index = 0; !flat_after_batch_only && index < tape.trades.size()
         && index < static_cast<std::size_t>(host.trade_count()); ++index) {
        const auto& expected = tape.trades[index];
        const auto& actual = host.get_trade(static_cast<int>(index));
        const bool closes_at_end = expected.exit_id == tape.prefix + "_end";
        const std::string close_label = "Close entry(s) order ";
        const auto expected_exit = closes_at_end ? std::string("__close__")
            : expected.exit_id.compare(0, close_label.size(), close_label) == 0
                ? "__close__" + expected.exit_id.substr(close_label.size())
                : expected.exit_id;
        passed = passed && actual.entry_id == expected.entry_id
            && actual.exit_id == expected_exit
            && actual.is_long == expected.is_long
            && std::abs(actual.qty - expected.quantity) < 1e-9
            && actual.entry_price == 100.0 && actual.exit_price == 100.0
            && actual.pnl == 0.0
            && actual.entry_time == expected.entry_time
            && actual.exit_time == expected.exit_time;
    }
    if (!passed) {
        if (flat_after_batch_only) {
            std::printf("FAIL %s: expected flat after batch and no final close_all trade, "
                        "got position %.8f, error=%s\n", tape.name.c_str(),
                        host.position_after_batch(), host.last_error().c_str());
        } else {
            std::printf("FAIL %s: expected %zu trades, got %d, error=%s\n",
                        tape.name.c_str(), tape.trades.size(), host.trade_count(),
                        host.last_error().c_str());
        }
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

class EntryExitFallbackHost : public pineforge::source::PineStrategyHost {
public:
    explicit EntryExitFallbackHost(bool two_entries) : two_entries_(two_entries) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("E", true, missing, missing, 2.0);
        if (two_entries_) strategy_entry("S", false, missing, missing, 1.0);
        strategy_exit("T1", "E", 105.0, 95.0, missing, missing, missing,
                      100.0, {}, 1.0);
        if (!two_entries_) {
            strategy_exit("T2", "E", 105.0, 95.0, missing, missing, missing,
                          100.0, {}, 1.0);
        }
    }

private:
    bool two_entries_;
};

bool run_entry_exit_fallback(bool two_entries) {
    EntryExitFallbackHost host(two_entries);
    const std::int64_t start_time = 1743465600000LL;
    std::vector<pineforge::Bar> bars;
    for (int ordinal = 0; ordinal < 7; ++ordinal) {
        bars.push_back({100.0, ordinal == 1 ? 106.0 : 101.0,
                        ordinal == 1 ? 94.0 : 99.0, 100.0, 1.0,
                        start_time + ordinal * 900000LL});
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    const std::string name = two_entries ? "entry-exit-batch" : "entry-exit-single";
    const int expected_count = two_entries ? 1 : 2;
    const double expected_position = two_entries ? -1.0 : 0.0;
    bool passed = host.last_error().empty() && host.trade_count() == expected_count
        && host.live_position_size() == expected_position;
    for (int index = 0; index < host.trade_count(); ++index) {
        const auto& trade = host.get_trade(index);
        const bool market_exit = two_entries && index == 0;
        const std::string exit_id = market_exit ? "S"
            : two_entries || index == 0 ? "T1" : "T2";
        const double exit_price = market_exit ? 100.0 : 95.0;
        const double quantity = market_exit ? 2.0 : 1.0;
        passed = passed && trade.entry_id == "E" && trade.exit_id == exit_id
            && trade.is_long && trade.qty == quantity && trade.entry_price == 100.0
            && trade.exit_price == exit_price
            && trade.pnl == quantity * (exit_price - 100.0)
            && trade.entry_time == start_time + 900000LL
            && trade.exit_time == start_time + 900000LL;
        std::printf("%s row %d %s/%s long=%d qty=%.17g entry=%.17g exit=%.17g "
                    "pnl=%.17g times=%lld/%lld\n", name.c_str(), index,
                    trade.entry_id.c_str(), trade.exit_id.c_str(), trade.is_long,
                    trade.qty, trade.entry_price, trade.exit_price, trade.pnl,
                    static_cast<long long>(trade.entry_time),
                    static_cast<long long>(trade.exit_time));
    }
    if (!passed) std::printf("FAIL %s: expected %d trades and position=%.17g; "
                            "got %d trades, position=%.17g, error=%s\n", name.c_str(),
                            expected_count, expected_position, host.trade_count(), host.live_position_size(),
                            host.last_error().c_str());
    return passed;
}

}

int main(int argc, char** argv) {
    int passed = 0;
    int failed = 0;
    int known_open = 0;
    for (const auto& tape : samebar_pyramiding_tapes) {
        if (argc > 1 && tape.name.find(argv[1]) == std::string::npos) continue;
        const bool flat_after_batch_only = tape.name == "entry-close-X04";
        if (tape.known_open) {
            ++known_open;
            if (!flat_after_batch_only) continue;
        }
        if (run_case(tape, flat_after_batch_only)) ++passed;
        else ++failed;
    }
    for (const bool two_entries : {false, true}) {
        const std::string name = two_entries ? "entry-exit-batch" : "entry-exit-single";
        if (argc > 1 && name.find(argv[1]) == std::string::npos) continue;
        if (run_entry_exit_fallback(two_entries)) ++passed;
        else ++failed;
    }
    std::printf("test_samebar_pyramiding_entries_tapes: %d passed, %d failed\n", passed, failed);
    std::printf("Recorded %d known-open cases without asserting their divergent tapes\n", known_open);
    return failed == 0 && passed > 0 ? 0 : 1;
}
