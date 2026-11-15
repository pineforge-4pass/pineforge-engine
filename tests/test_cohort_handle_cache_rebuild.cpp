// The handle cache is derived from the hashed cohort table. Reconstruct that
// component at a callback boundary, retaining the other live host components,
// and resume the same orders. This is a cohort checkpoint, not a process image.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
constexpr auto na = std::numeric_limits<double>::quiet_NaN();

// Existing explicit-instantiation seam: no production friend or layout change.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
struct Cache { friend auto access(Cache); };
struct Cursor { friend auto access(Cursor); };
struct Lookup { friend auto access(Lookup); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<Cache, &Adapter::cohort_handle_cache_>;
template struct Access<Cursor, &Adapter::cohort_handle_cache_next_>;
template struct Access<Lookup, &Adapter::find_cohort_handle>;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::uint64_t fold(const Adapter& adapter) {
    BrokerStateHashSink sink;
    adapter.hash_state(sink);
    return sink.h;
}

void clear_cache(Adapter& adapter, std::size_t cursor = 0) {
    adapter.*access(Cache{}) = {};
    adapter.*access(Cursor{}) = cursor;
}

using Handles = std::vector<std::pair<std::string, native_order::CohortHandle>>;

Handles authoritative_handles(Adapter& adapter) {
    auto& table = adapter.*access(Cohorts{});
    Handles result;
    table.for_each_id([&](const std::string& id) {
        const auto row = table.find(id);
        require(row != table.end(), "cohort checkpoint lost a logical id");
        result.emplace_back(id, row->second.handle);
    });
    return result;
}

void lookup_rebuild_control(Adapter& adapter) {
    auto& table = adapter.*access(Cohorts{});
    auto expected = authoritative_handles(adapter);
    const auto before = fold(adapter);
    require(expected.size() > (adapter.*access(Cache{})).size(),
            "lookup control did not exceed cache capacity");
    // All replacement positions, both visiting orders, exact-spelling misses,
    // a real disk restore on cold lookup, and a real cache hit after spill.
    for (std::size_t cursor = 0; cursor < (adapter.*access(Cache{})).size(); ++cursor) {
        clear_cache(adapter, cursor);
        for (const auto& item : expected) {
            table.spill();
            const auto cold = (adapter.*access(Lookup{}))(item.first);
            require(cold == item.second, "cold lookup differs from hashed table");
            table.spill();
            const auto restored = table.work_counters().rows_restored;
            require((adapter.*access(Lookup{}))(item.first) == cold,
                    "warm lookup differs from cold lookup");
            require(table.work_counters().rows_restored == restored,
                    "warm lookup bypassed neither the disk nor the cache");
            require((adapter.*access(Lookup{}))(item.first + "-missing").value == 0,
                    "cache confused a distinct full id with a cached id");
        }
        std::reverse(expected.begin(), expected.end());
        table.spill();
        for (const auto& item : expected)
            require((adapter.*access(Lookup{}))(item.first) == item.second,
                    "eviction or lookup order changed the handle");
        require(fold(adapter) == before, "lookup/cache geometry changed broker hash");
    }
    require(table.work_counters().rows_restored > 0,
            "cold lookup control never restored authoritative disk data");
}

void restore_cohort_checkpoint(Adapter& adapter) {
    auto& table = adapter.*access(Cohorts{});
    using Facts = std::decay_t<decltype(table.begin()->second)>;
    std::vector<std::pair<std::string, typename Facts::Record>> checkpoint;
    const auto before = fold(adapter);
    // This boundary precedes the first live entry. Record is a complete
    // representation here: no opened vector or live-origin units are omitted.
    table.for_each_id([&](const std::string& id) {
        const auto row = table.find(id);
        require(row != table.end() && row->second.spillable(),
                "cohort checkpoint would omit live facts");
        checkpoint.emplace_back(id, row->second.retired_record());
    });
    require(checkpoint.size() > 8, "checkpoint did not contain an eviction witness");
    table.clear();
    clear_cache(adapter);
    for (const auto& item : checkpoint)
        table.emplace(item.first, Facts::restore_record(item.second));
    require(fold(adapter) == before, "reconstructed cohort checkpoint changed hashed state");
    for (const auto& entry : adapter.*access(Cache{}))
        require(entry.handle.value == 0, "restore did not start with a cold cache");
    require(adapter.*access(Cursor{}) == 0, "restore retained replacement history");
}

using Fill = std::tuple<std::uint64_t, std::uint64_t, std::string,
                        std::int64_t, double, double, double, double, double,
                        std::size_t, std::size_t, std::uint64_t, bool>;

class Host final : public source::PineStrategyHost {
public:
    Host(bool restore, bool recalc) : restore_(restore) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.margin_long = config.margin_short = 0.0;
        config.calc_on_order_fills = recalc;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(true);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == previous_) return;
        previous_ = bar;
        if (bar == 0) {
            for (int i = 0; i < 17; ++i) {
                const auto id = "cold-" + std::to_string(i);
                strategy_exit("exit-" + id, id, 150.0, 50.0);
            }
        }
        if (bar == 4) {
            require(live_position_size() == 0.0, "checkpoint boundary is not flat");
            if (restore_) {
                restore_cohort_checkpoint(adapter_);
                lookup_rebuild_control(adapter_);
                clear_cache(adapter_); // Resume orders with the rebuilt cache cold.
            }
            checkpoint_visited = true;
            strategy_entry("held", true, 99.0, na, 2.0, "opening");
        }
        if (bar >= 4 && bar <= 8)
            strategy_exit("protect", "held", bar < 6 ? 140.0 : 110.0,
                          60.0, na, na, na, 50.0, "protective");
        if (bar == 9) strategy_close("held", "remainder");
        if (bar == 11) strategy_entry("short", false, na, na, 1.0, "short-opening");
        if (bar == 14) strategy_close("short", "short-close");
    }

    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        fills.emplace_back(event.ordinal, event.handle().incarnation, event.request().label,
                           event.effective_time_ms(), event.raw_price, event.resolved_price,
                           event.closed_units, event.opened_units, event.filled_working,
                           event.first_trade_index, event.closed_trade_count,
                           event.opened_lot_incarnation, event.terminal);
    }

    bool checkpoint_visited = false;
    std::vector<Fill> fills;
private:
    bool restore_;
    int previous_ = -1;
};

bool same_trade(const TradeC& a, const TradeC& b) {
    return std::tie(a.entry_time, a.exit_time, a.entry_price, a.exit_price, a.pnl, a.pnl_pct,
                    a.is_long, a.max_runup, a.max_drawdown, a.qty, a.commission,
                    a.entry_bar_index, a.exit_bar_index, a.open_at_end)
        == std::tie(b.entry_time, b.exit_time, b.entry_price, b.exit_price, b.pnl, b.pnl_pct,
                    b.is_long, b.max_runup, b.max_drawdown, b.qty, b.commission,
                    b.entry_bar_index, b.exit_bar_index, b.open_at_end);
}

void resumed_run_control(bool recalc) {
    Host uninterrupted(false, recalc), restored(true, recalc);
    std::vector<Bar> bars;
    for (int i = 0; i < 18; ++i)
        bars.push_back({100.0, i == 7 ? 112.0 : 100.0, i == 5 ? 98.0 : 100.0,
                        100.0, 1.0, INT64_C(1736121600000) + i * INT64_C(60000)});
    for (auto* host : {&uninterrupted, &restored}) {
        host->run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host->last_error().empty(), host->last_error().c_str());
        require(host->checkpoint_visited && host->trade_count() >= 3,
                "resume witness did not checkpoint and close partial/long/short trades");
        require(host->live_position_size() == 0.0 && host->fills.size() >= 5,
                "resume witness did not execute its actual orders");
    }
    require(uninterrupted.fills == restored.fills, "cache-cleared resume changed fills");
    require(uninterrupted.broker_state_hash() == restored.broker_state_hash(),
            "cache-cleared resume changed final broker hash");
    ReportC a{}, b{};
    struct Release {
        ReportC& report;
        ~Release() { BacktestEngine::free_report(&report); }
    } release_a{a}, release_b{b};
    uninterrupted.fill_report(&a);
    restored.fill_report(&b);
    require(a.trades_len == b.trades_len && a.broker_state_hash_len == b.broker_state_hash_len
            && a.broker_state_hash_len > 0 && a.net_profit == b.net_profit,
            "cache-cleared resume changed report totals");
    for (int i = 0; i < a.trades_len; ++i)
        require(same_trade(a.trades[i], b.trades[i]), "cache-cleared resume changed a trade");
    for (std::int64_t i = 0; i < a.broker_state_hash_len; ++i)
        require(a.broker_state_hash[i] == b.broker_state_hash[i],
                "cache-cleared resume changed a per-bar broker hash");
}
} // namespace

int main() {
    try {
        resumed_run_control(false);
        resumed_run_control(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
