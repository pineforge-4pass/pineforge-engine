#pragma once

// Test strategy only. No replay state, declarations, records or arithmetic
// are supplied by this fixture: those must come from the linked runtime.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <native_execution_consumer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace replay_rq {
using namespace pineforge;
using HostBase = source::PineStrategyHost;
constexpr std::int64_t start = INT64_C(1736121600000);
constexpr std::int64_t minute = 60000;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Existing private-access test pattern (test_global_exit_recut_scratch).
// The only injected state is an OLD sort key (exit phase), never a marker.
template<class Tag, typename Tag::type Member> struct Access {
    friend typename Tag::type member(Tag) { return Member; }
};
struct Sort {
    using type = std::optional<std::size_t> (HostBase::*)(
        TradeLedger&, source::PineExecutionAdapter&);
    friend type member(Sort);
};
template struct Access<Sort, &HostBase::sort_same_bar_exit_trades>;
struct Phase {
    using type = void (source::PineExecutionAdapter::*)(std::size_t, std::uint8_t);
    friend type member(Phase);
};
template struct Access<Phase, &source::PineExecutionAdapter::set_exit_phase>;

class Host final : public HostBase {
public:
    std::string shape = "fifo";
    std::array<std::uint64_t, 2> parents{};
    std::array<std::size_t, 2> group_first{}, group_end{}, physical_count{};
    std::size_t groups = 0, sort_first = 0;

    Host() { configure(); }

    void configure() {
        source::PineStrategyConfig config;
        config.initial_capital = shape == "cancellation"
            ? static_cast<double>(UINT64_C(1) << 60) : 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 4;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        config.commission_value = shape == "fifo_no_fees" ? 0.0
            : shape == "cancellation" ? 0.5 : 1.0;
        config.close_entries_rule_any = shape == "any" || shape == "cancellation";
        config.calc_on_order_fills = false;
        config.process_orders_on_close = false;
        if (shape == "two_groups") {
            // Direct realtime commands have no callback's signal mark/equity.
            // Explicit units use Pine's supported no-margin mode; the actual
            // ticks still price and settle every entry and global close.
            config.margin_long = 0.0;
            config.margin_short = 0.0;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 1.0);
        set_trade_history_streamable(shape == "sealed");
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (shape == "zero" || shape == "two_groups") return;
        if (shape == "cancellation") {
            if (bar == 0) strategy_entry("B", true, absent, absent, 1.0);
            if (bar == 1) strategy_entry("Z", true, absent, absent, 1.0);
            if (bar == 2) strategy_entry("A", true, absent, absent, 1.0);
            if (bar == 3)
                strategy_exit("global", "", static_cast<double>(UINT64_C(1) << 54), absent);
            return;
        }
        if (shape == "ordinary" || shape == "quote") {
            if (bar == 0) strategy_entry("ordinary", true, absent, absent, 1.0);
            if (bar == 1) strategy_close("ordinary");
            if (bar == (shape == "quote" ? 2 : 3))
                strategy_entry("held", true, absent, absent, 1.0);
            return;
        }
        const int offset = shape == "sealed" ? 24 : 0;
        // Six earlier and three later distinct-time closes force the recut
        // group past the newest exit-time block AND its retained predecessor.
        if (shape == "sealed" && (bar < 24 || (bar >= 32 && bar < 44))) {
            if (bar % 4 == 0) strategy_entry("warmup", true, absent, absent, 1.0);
            if (bar % 4 == 1) strategy_close("warmup");
        }
        if (bar == offset)
            strategy_entry("Z", true, absent, absent, shape == "sort" ? 1.0 : 4.0);
        if (bar == offset + 1)
            strategy_entry("A", true, absent, absent, shape == "sort" ? 4.0 : 2.0);
        if (bar == offset + 2) strategy_exit("global", "", 136.0, absent);
        if (shape == "sort" && bar == 4) force_real_sort();
    }

    std::size_t ledger_base() const { return trades_.base(); }
    std::size_t prefix_base() const {
        return NativeExecutionConsumer::bound(*this).closed_trade_prefix().base;
    }
    Trade row(std::size_t index) const { return trades_[index]; }
    double raw_sum_before_extension() const { return BacktestEngine::net_profit(); }

    void force_real_sort() {
        require(shape == "sort" && groups == 1 && trades_.size() == 3,
                "sort fixture did not produce the three real FIFO slices");
        require(row(1).entry_id == "A" && row(2).entry_id == "A"
                && row(1).qty == 3.0 && row(2).qty == 1.0,
                "sort fixture lacks distinguishable same-entry tail slices");
        (adapter_.*member(Phase{}))(1, static_cast<std::uint8_t>(NativePathPhase::High));
        (adapter_.*member(Phase{}))(2, static_cast<std::uint8_t>(NativePathPhase::Open));
        const auto moved = (this->*member(Sort{}))(trades_, adapter_);
        require(moved && *moved == 1 && row(1).qty == 1.0 && row(2).qty == 3.0,
                "sort_same_bar_exit_trades did not execute its permutation cycles");
        sort_first = *moved;
        native_closed_rows_amended(*moved);
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.closed_trade_count < 2 || event.first_trade_index >= trades_.size()) return;
        if (trades_[event.first_trade_index].exit_id != "global") return;
        require(groups < parents.size(), "unexpected third global close");
        parents[groups] = event.ordinal;
        group_first[groups] = event.first_trade_index;
        group_end[groups] = trades_.size();
        physical_count[groups] = event.closed_trade_count;
        ++groups;
    }
};

inline std::vector<Bar> tape(const std::string& shape) {
    const int count = shape == "sealed" ? 46 : shape == "quote" ? 60 : 8;
    std::vector<Bar> result;
    for (int i = 0; i < count; ++i) {
        Bar bar{128.0, 128.0, 128.0, 128.0, 1.0, start + i * minute};
        if ((shape == "sealed" && i == 27)
            || ((shape == "fifo" || shape == "fifo_no_fees"
                 || shape == "any" || shape == "sort") && i == 3))
            bar.high = 136.0;
        if (shape == "ordinary" && i == 2)
            bar.open = bar.high = bar.low = bar.close = 136.0;
        if (shape == "ordinary" && i == count - 1)
            bar.open = bar.high = bar.low = bar.close = 144.0;
        if (shape == "cancellation") {
            const double large = static_cast<double>(UINT64_C(1) << 54);
            const double price = i < 2 ? 1.0 : i == 3 ? large * 2.0 : large;
            bar.open = bar.high = bar.low = bar.close = price;
        }
        result.push_back(bar);
    }
    return result;
}

// External source-command driving is test control, between public C streaming
// calls, as native stream submit tests do. No observer places commands. Every
// close and every marker is produced by the normal kernel/adapter pipeline.
// This shape alone declares zero margins: source placement admission otherwise
// requires a callback's signal mark/equity, absent between stream inputs
// (pine_adapter.cpp:7750-7810). Quantities are explicit, never default-sized.
inline void two_groups(Host& host, const std::vector<Bar>& bars) {
    require(strategy_stream_begin(&host, reinterpret_cast<const pf_bar_t*>(bars.data()),
                                  3, "1", "1") == 0, "stream begin failed");
    const auto tick = [&](int sequence, double price) {
        const pf_trade_tick_t value{start + 3 * minute + sequence * 1000,
                                    static_cast<std::uint64_t>(sequence), price, 1.0};
        require(strategy_stream_push_tick(&host, &value) == 0, "stream tick failed");
    };
    for (int group = 0; group < 2; ++group) {
        const double entry = group == 0 ? 128.0 : 64.0;
        host.strategy_entry("Z", true, absent, absent, 4.0);
        tick(group * 3 + 1, entry);
        host.strategy_entry("A", true, absent, absent, 2.0);
        tick(group * 3 + 2, entry);
        host.strategy_exit("global", "", entry + 8.0, absent);
        tick(group * 3 + 3, entry + 8.0);
    }
    require(strategy_stream_advance_time(&host, start + bars.size() * minute) == 0,
            "stream confirmation failed");
    require(strategy_stream_end(&host, 0) == 0, "stream end failed");
}

inline void run(Host& host, const std::vector<Bar>& bars, const char* input_tf,
                const char* script_tf, bool magnifier, int samples,
                MagnifierDistribution distribution) {
    host.groups = 0;
    host.parents = {};
    if (host.shape == "two_groups") {
        two_groups(host, bars);
    } else {
        if (host.shape == "quote")
            require(host.set_report_terminal_quote(bars.back().timestamp + minute, 144.0),
                    "terminal quote refused");
        host.run(bars.data(), static_cast<int>(bars.size()), input_tf, script_tf,
                 magnifier, samples, distribution);
    }
    require(host.last_run_status() == 0 && host.last_error().empty(), "real fixture run failed");
}
}  // namespace replay_rq
