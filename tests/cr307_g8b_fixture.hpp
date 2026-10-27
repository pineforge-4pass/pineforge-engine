#pragma once

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cr307_g8b {
using Adapter = pineforge::source::PineExecutionAdapter;
constexpr double na = std::numeric_limits<double>::quiet_NaN();

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct CloseUnits { friend auto access(CloseUnits); };
struct Tombstones { friend auto access(Tombstones); };
struct Tokens { friend auto access(Tokens); };
template struct Access<CloseUnits, &Adapter::close_logical_units_>;
template struct Access<Tombstones, &Adapter::exit_tombstones_>;
template struct Access<Tokens, &Adapter::named_entry_cancel_tokens_>;

inline void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

// Compile on the frozen head too: its ordinary maps hold every row in RAM.
template<class Table>
auto resident_size(const Table& table, int) -> decltype(table.resident_size()) {
    return table.resident_size();
}
template<class Table>
std::size_t resident_size(const Table& table, long) { return table.size(); }
template<class Table>
auto sorted_rows(const Table& table, int) -> decltype(table.all_sorted()) {
    return table.all_sorted();
}
template<class Table>
auto sorted_rows(const Table& table, long) {
    return std::map<typename Table::key_type, typename Table::mapped_type>(
        table.begin(), table.end());
}
template<class Table>
auto spill(const Table& table, int) -> decltype(table.spill(), void()) { table.spill(); }
template<class Table>
void spill(const Table&, long) {}
template<class Value>
auto scalar(const Value& value, int) -> decltype(value.value) { return value.value; }
template<class Value>
Value scalar(const Value& value, long) { return value; }
template<class Table>
auto value_for(const Table& table, const std::string& key) {
    const auto value = [&] {
        const auto row = table.find(key);
        require(row != table.end(), "expected historical key exists");
        return scalar(row->second, 0);
    }();
    spill(table, 0);
    return value;
}

inline std::vector<pineforge::Bar> chart(std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   1736121600000LL + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

class HistoryHost final : public pineforge::source::PineStrategyHost {
public:
    HistoryHost(std::size_t bars, bool partial)
        : rounds_((bars - 1) / 4), partial_(partial) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e9;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 2;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.close_entries_rule_any = true;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        qty_step_ = 1.0; // Tombstone policy requires an explicit decimal lot grid.
    }

    void on_source_bar(const pineforge::Bar&) override {
        ++callbacks;
        peak_close = std::max(peak_close, resident_size(adapter_.*access(CloseUnits{}), 0));
        peak_tombstones = std::max(peak_tombstones,
                                  resident_size(adapter_.*access(Tombstones{}), 0));
        require(native_open_lots(100.0).size() <= 2,
                "keeper and current fresh id are the only live lots");
        const auto bar = static_cast<std::size_t>(pine_bar_index());
        if (bar == 0) {
            strategy_entry("keeper", true, na, na, 1.0);
            return;
        }
        const auto round = (bar - 1) / 4;
        if (round >= rounds_) return; // Leave incomplete trailing rounds idle.
        const auto id = "fresh-" + std::to_string(round);
        const auto& exit_id = id; // Only keeper and this fresh textual id are live.
        switch ((bar - 1) % 4) {
        case 0:
            require(live_position_size() == 1.0, "previous fresh id fully retired");
            strategy_entry(id, true, na, na, partial_ ? 2.0 : 1.0);
            break;
        case 1:
            require(live_position_size() == (partial_ ? 3.0 : 2.0),
                    "fresh market entry really filled");
            if (partial_) {
                const auto lots = native_open_lots(100.0);
                const auto fresh = std::find_if(lots.begin(), lots.end(),
                    [&](const pineforge::NativeOpenLot& lot) { return lot.entry_label == id; });
                require(fresh != lots.end(), "observe real fresh entry incarnation");
                origins.push_back(fresh->entry_incarnation);
                strategy_exit(exit_id, id, 100.5, na, na, na, na, 50.0);
            } else
                strategy_close(id);
            break;
        case 2:
            if (partial_) {
                require(live_position_size() == 2.0, "one-unit partial exit really filled");
                const auto& stones = adapter_.*access(Tombstones{});
                require(stones.size() == round + 1, "every fresh partial creates history");
                const auto before = native_working_requests().size();
                strategy_exit(exit_id, id, 100.5, na, na, na, na, 50.0, {}, 1.0);
                require(native_working_requests().size() == before,
                        "same-incarnation partial reissue must stay tombstoned");
                strategy_close(id);
            }
            break;
        case 3:
            require(live_position_size() == 1.0, "fresh lot closed while keeper remains");
            ++completed;
            require(trade_count() == static_cast<int>(completed * (partial_ ? 2 : 1)),
                    "every round produced its real close trades");
            require(adapter_.fixture_close_logical_count() == completed + 1,
                    "ANY close history includes keeper and all retired ids");
            // Check old keys after retirement; zeroing/erasing history is wrong.
            require(adapter_.fixture_close_logical_units("fresh-0") == (partial_ ? 2.0 : 1.0),
                    "retired first id keeps its original ANY credit");
            if (completed == rounds_) check_history();
            break;
        }
    }

    void check_history() {
        const auto& units = adapter_.*access(CloseUnits{});
        std::map<std::string, double> expected_units{{"keeper", 1.0}};
        for (std::size_t i = 0; i < completed; ++i)
            expected_units.emplace("fresh-" + std::to_string(i), partial_ ? 2.0 : 1.0);
        pineforge::BrokerStateHashSink actual, expected;
        actual.u(units.size());
        for (const auto& row : sorted_rows(units, 0)) {
            actual.s(row.first); actual.d(scalar(row.second, 0));
        }
        spill(units, 0);
        expected.u(expected_units.size());
        for (const auto& row : expected_units) { expected.s(row.first); expected.d(row.second); }
        require(actual.h == expected.h, "close history keeps original sorted count/key/double fold");
        if (partial_) {
            const auto& stones = adapter_.*access(Tombstones{});
            std::map<std::string, std::uint64_t> expected_stones;
            // Opening incarnations are observed from real filled lots below,
            // never inferred from a fixed kernel request-number sequence.
            require(origins.size() == completed, "one observed incarnation per fresh entry");
            for (std::size_t i = 0; i < completed; ++i)
                expected_stones.emplace("fresh-" + std::to_string(i) + "\x1f"
                                         + "fresh-" + std::to_string(i), origins[i]);
            pineforge::BrokerStateHashSink a, e;
            a.s("pineforge-exit-tombstones/v1"); a.u(stones.size());
            for (const auto& row : sorted_rows(stones, 0)) {
                a.s(row.first); a.u(scalar(row.second, 0));
            }
            spill(stones, 0);
            e.s("pineforge-exit-tombstones/v1"); e.u(expected_stones.size());
            for (const auto& row : expected_stones) { e.s(row.first); e.u(row.second); }
            require(a.h == e.h, "tombstone fold preserves joined keys, incarnations and order");
        }
        checked = true;
    }

    // Observations only; none of these counters controls a source command.
    std::size_t callbacks = 0, completed = 0, peak_close = 0, peak_tombstones = 0;
    bool checked = false;
    std::vector<std::uint64_t> origins;

private:
    const std::size_t rounds_;
    const bool partial_;
};

inline std::size_t history_run(std::size_t bars, bool partial) {
    const auto data = chart(bars);
    HistoryHost host(bars, partial);
    host.run(data.data(), static_cast<int>(data.size()), "1", "1");
    if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
    require(host.callbacks == bars && host.completed == (bars - 1) / 4 && host.checked,
            "complete host workload and independent history oracle executed");
    const auto peak = partial ? host.peak_tombstones : host.peak_close;
    std::printf("B=%zu completed=%zu resident_peak=%zu\n", bars, host.completed, peak);
    return peak;
}

inline int history_main(bool partial) {
    try {
        const auto small = history_run(2000, partial);
        const auto large = history_run(8000, partial);
        require(small <= 64 && large <= 64,
                "scalar history resident rows must stay within a fixed 64-row window");
        require(large <= small, "8,000 bars must not retain more history rows than 2,000");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
} // namespace cr307_g8b
