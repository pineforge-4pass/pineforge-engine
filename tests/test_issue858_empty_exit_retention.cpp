// Repeated absent-parent exits must not restore empty cohort payloads just
// to read live membership or an immutable native owner handle. Exercise the
// real run()/strategy_exit() path and existing production work counters.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t epoch = INT64_C(1736121600000);
constexpr int empty_bars = 192;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void configure_host(source::PineStrategyHost& host, bool recalc = false, bool any = false) {
    source::PineStrategyConfig config;
    config.initial_capital = 10000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 2.0;
    config.commission_value = 0.0;
    config.slippage = 0;
    config.margin_long = config.margin_short = 0.0;
    config.calc_on_order_fills = recalc;
    config.close_entries_rule_any = any;
    host.configure_pine_strategy(config);
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    host.set_syminfo_mintick(1.0);
    host.set_broker_state_hash_recording(false);
    host.fixture_retain_all_events();
}

std::vector<Bar> flat_feed(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0, epoch + i * INT64_C(60000)});
    return bars;
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

class EmptyExitHost final : public source::PineStrategyHost {
public:
    EmptyExitHost(int fanout, int parents) : fanout_(fanout), parents_(parents) {
        configure_host(*this);
    }

    void on_source_bar(const Bar& bar) override {
        ++callbacks;
        require(live_position_size() == 0.0 && trade_count() == 0,
                "absent-parent calls produced a position or trade");
        for (int slot = 0; slot < fanout_; ++slot) {
            const int parent = (pine_bar_index() * fanout_ + slot) % parents_;
            const auto suffix = std::to_string(parent);
            const auto before = adapter_.cohort_table_work().rows_restored;
            // The reporter's one/two absent brackets. The issuance assertion
            // below also requires every repeated call to reach native code.
            strategy_exit("guard-" + suffix, "parent-" + suffix,
                          bar.close * (slot == 0 ? 1.06 : 0.94),
                          bar.close * (slot == 0 ? 0.97 : 1.03));
            restored_in_calls += adapter_.cohort_table_work().rows_restored - before;
            ++calls;
        }
        spilled = adapter_.cohort_table_work().rows_spilled;
    }

    std::uint64_t restored_in_calls = 0, spilled = 0;
    int callbacks = 0, calls = 0;

private:
    int fanout_, parents_;
};

std::uint64_t empty_exit_work(int fanout, int parents) {
    EmptyExitHost host(fanout, parents);
    const auto bars = flat_feed(empty_bars);
    const auto run_once = [&] {
        host.callbacks = host.calls = 0;
        host.restored_in_calls = host.spilled = 0;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host.last_error().empty(), "empty-exit run failed: " + host.last_error());
        require(host.callbacks == empty_bars && host.calls == fanout * empty_bars,
                "empty-exit run did not execute its input calls");
        require(host.live_position_size() == 0.0 && host.trade_count() == 0,
                "empty-exit run changed trading state");
        std::size_t definitions = 0;
        for (const auto& row : host.native_events(0)) {
            if (!row.command) continue;
            if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*row.command)) {
                require(static_cast<bool>(accepted->definition),
                        "accepted exit lost its definition");
                ++definitions;
            } else if (const auto* replaced = std::get_if<native_order::ReplacedEvent>(&*row.command)) {
                require(static_cast<bool>(replaced->successor_definition),
                        "replacement exit lost its definition");
                ++definitions;
            }
        }
        require(definitions == static_cast<std::size_t>(2 * host.calls),
                "an absent exit lost or duplicated a native leg");
        require(host.spilled > 0, "production policy never spilled an empty cohort");
        std::printf("empty_exit_retention fanout=%d parents=%d calls=%d definitions=%zu "
                    "spilled=%llu restored_in_calls=%llu\n",
                    fanout, parents, host.calls, definitions,
                    static_cast<unsigned long long>(host.spilled),
                    static_cast<unsigned long long>(host.restored_in_calls));
        return host.restored_in_calls;
    };
    const auto first = run_once();
    // Reuse the host across native run reset. Cached handles from the first
    // run must not stand in for cohort admission in the second.
    return first + run_once();
}

enum class Calls { while_open, every_bar, extra_absent };

class TradingHost final : public source::PineStrategyHost {
public:
    using BacktestEngine::closed_trade_entry_id;
    using BacktestEngine::closed_trade_exit_id;
    using BacktestEngine::closed_trade_exit_comment;

    TradingHost(Calls calls, bool recalc, bool any, bool explicit_units)
        : calls_(calls), explicit_units_(explicit_units) {
        configure_host(*this, recalc, any);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        ++callbacks;
        const bool enter = i == 4 && !entered_;
        if (enter) {
            entered_ = true;
            // A real pending entry, after earlier absent exits of this id.
            strategy_entry("held", true, 99.0, na, 2.0, "opening");
        }
        const double limit = i < 6 ? 140.0 : 110.0;
        const double stop = i < 6 ? 60.0 : 90.0;
        if (calls_ == Calls::every_bar || enter || live_position_size() > 0.0) {
            strategy_exit("protect", "held", limit, stop, na, na, na,
                          explicit_units_ ? 100.0 : 50.0, "protective",
                          explicit_units_ ? 2.0 : na, "protect-oca");
            if (i == 6) {
                // Same-pass replacement of both legs, preserving the current
                // values. The ordinary adapter owns the replacement and OCA.
                strategy_exit("protect", "held", limit, stop, na, na, na,
                              explicit_units_ ? 100.0 : 50.0, "replacement",
                              explicit_units_ ? 2.0 : na, "protect-oca");
            }
        }
        if (calls_ == Calls::every_bar)
            strategy_exit("opposite-protect", "opposite", 60.0, 140.0);
        if (calls_ == Calls::extra_absent)
            strategy_exit("unrelated-protect", "unrelated", limit, stop);
        if (i == 9 && !closed_) {
            closed_ = true;
            strategy_close("held", "explicit-close");
        }
    }

    int callbacks = 0;

private:
    Calls calls_;
    bool explicit_units_, entered_ = false, closed_ = false;
};

struct Outcome {
    std::vector<TradeC> trades;
    std::vector<std::string> entry_ids, exit_ids, comments;
};

Outcome trading_case(Calls calls, bool recalc, bool any, bool explicit_units) {
    TradingHost host(calls, recalc, any, explicit_units);
    auto bars = flat_feed(14);
    bars[5].low = 98.0;   // Pending buy fills at its 99 limit.
    bars[7].high = 112.0; // Replaced limit closes its requested units at 110.
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), "trading control failed: " + host.last_error());
    require(host.callbacks >= static_cast<int>(bars.size()),
            "trading control did not visit every source bar");
    if (recalc)
        require(host.callbacks > static_cast<int>(bars.size()),
                "fill recalculation control had no recalculation");
    Report report;
    host.fill_report(&report.value);
    const int expected_trades = explicit_units ? 1 : 2;
    require(report.value.trades_len == expected_trades && host.trade_count() == expected_trades
            && host.live_position_size() == 0.0,
            "bracket quantity or explicit close changed the closed slices");
    Outcome outcome;
    for (int i = 0; i < report.value.trades_len; ++i) {
        const auto& trade = report.value.trades[i];
        const double units = explicit_units ? 2.0 : 1.0;
        require(!trade.open_at_end && trade.is_long && trade.qty == units
                && trade.entry_price == 99.0 && trade.commission == 0.0,
                "pending entry or partial quantity changed");
        require(trade.exit_price == (i == 0 ? 110.0 : 100.0)
                && trade.pnl == units * (i == 0 ? 11.0 : 1.0),
                "replacement/OCA or explicit close changed the expected fill");
        outcome.trades.push_back(trade);
        outcome.entry_ids.push_back(host.closed_trade_entry_id(i));
        outcome.exit_ids.push_back(host.closed_trade_exit_id(i));
        outcome.comments.push_back(host.closed_trade_exit_comment(i));
    }
    require(outcome.entry_ids == std::vector<std::string>(expected_trades, "held")
            && (explicit_units || outcome.comments[1] == "explicit-close"),
            "an earlier absent exit attached to the later entry");
    return outcome;
}

bool same_trade(const TradeC& a, const TradeC& b) {
    return std::tie(a.entry_time, a.exit_time, a.entry_price, a.exit_price, a.pnl, a.pnl_pct,
                    a.is_long, a.max_runup, a.max_drawdown, a.qty, a.commission,
                    a.entry_bar_index, a.exit_bar_index, a.open_at_end)
        == std::tie(b.entry_time, b.exit_time, b.entry_price, b.exit_price, b.pnl, b.pnl_pct,
                    b.is_long, b.max_runup, b.max_drawdown, b.qty, b.commission,
                    b.entry_bar_index, b.exit_bar_index, b.open_at_end);
}

void trading_controls() {
    for (bool recalc : {false, true}) {
        for (bool any : {false, true}) {
            for (bool explicit_units : {false, true}) {
                const auto control = trading_case(Calls::while_open, recalc, any, explicit_units);
                for (auto calls : {Calls::every_bar, Calls::extra_absent}) {
                    const auto observed = trading_case(calls, recalc, any, explicit_units);
                    require(observed.entry_ids == control.entry_ids
                            && observed.exit_ids == control.exit_ids
                            && observed.comments == control.comments,
                            "absent calls changed the source trade identities");
                    require(observed.trades.size() == control.trades.size(),
                            "absent calls changed the trade count");
                    for (std::size_t i = 0; i < control.trades.size(); ++i)
                        require(same_trade(observed.trades[i], control.trades[i]),
                                "absent calls changed a trade field");
                }
            }
        }
    }
}

void version_only_scratch_control() {
    namespace pd = pineforge::detail;
    namespace sd = pineforge::source::detail;
    constexpr std::size_t page_size = 512;
    std::vector<std::byte> bytes(128 * page_size);
    pd::FixedMemoryScratchBacking backing{{bytes.data(), bytes.size()}, page_size};
    pd::CommandScratchStore store{
        {2, page_size, 1, 1, sd::exit_scratch_key_order(), pd::ScratchOpenMode::create_empty},
        &backing};
    require(store.status() == pd::ScratchStatus::ok, "scratch control setup failed");
    sd::ExitScratchVersionsProduction versions{store};
    sd::ExitScratchContext context{store, {}};
    context.versions = &versions;
    std::uint64_t version = 0;
    {
        // A version can own payload without any selected row. Moving an
        // empty selection must transfer that independent cleanup obligation.
        sd::ExitScratchRows<std::uint64_t> rows(context);
        native_order::Request request;
        request.label = "frozen";
        request.comment = "original";
        request.trigger = native_order::Limit{137.0};
        version = versions.request(rows.scope(), request);
        request.comment = "changed";
        request.trigger = native_order::Stop{41.0};
        require(rows.empty() && store.record_count() > 0,
                "version-only scope has no frozen payload");
        auto moved = std::move(rows);
        {
            sd::ExitScratchRows<std::string> nested(context);
            require(!nested.contains("missing"), "empty nested selection found a row");
            nested.push_back("nested");
            const auto frozen = versions.request(version);
            const auto* limit = std::get_if<native_order::Limit>(&frozen.trigger);
            require(moved.empty() && nested.size() == 1
                    && frozen.label == "frozen" && frozen.comment == "original"
                    && limit && limit->price == 137.0,
                    "nested or moved scope changed an independently frozen request");
        }
        require(versions.request(version).comment == "original",
                "nested teardown released the outer version");
    }
    pd::ScratchRecordInfo info{};
    require(store.point(version, info) == pd::ScratchStatus::not_found
            && store.record_count() == 0, "moved version-only scope leaked its payload");
}
} // namespace

int main() {
    try {
        // Nonempty semantic controls run before the expected base work failure.
        trading_controls();
        version_only_scratch_control();
        // Exceed the bounded handle working set. Real restoration must remain
        // possible; a disabled or fabricated work counter cannot pass.
        require(empty_exit_work(1, 17) > 0, "churn control never restored a cohort");
        const auto one = empty_exit_work(1, 1);
        const auto two = empty_exit_work(2, 2);
        require(one == 0 && two == 0,
                "absent-parent strategy_exit restored an empty cohort payload");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
