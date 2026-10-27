// CR-400 / CR-368b HEAVY refusal regression.
// Expected wall time: under 60 s for both cases together (lab must measure).
// Edit-only lane: no compile, fail-before, execution or timing is claimed.
//
// 20,000 bars / 4 = 5,000 rounds, each with two fresh ids and one closed
// trade: 10,000 ids, 5,000 trades. Only one position and one stop can be live;
// never accumulate unfilled entries (the quadratic workload in issue #452).
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kBars = 20'000;
constexpr std::size_t kRounds = 5'000;
constexpr std::size_t kDistinctIds = 10'000;
constexpr std::int64_t kStart = 1'736'121'600'000LL;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Explicit checks also work when this file is compiled with NDEBUG.
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

pf_scratch_tables_v1 descriptor(const pineforge::detail::ScratchManifest& manifest) {
    pf_scratch_tables_v1 out{};
    out.struct_size = static_cast<std::uint32_t>(sizeof out);
    out.version = 1;
    out.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    out.K_q = manifest.K_q; out.B_q = manifest.B_q;
    out.K_e = manifest.K_e; out.B_e = manifest.B_e;
    out.K_c = manifest.K_c; out.B_c = manifest.B_c;
    out.U = manifest.U; out.B_U = manifest.B_U;
    out.V = manifest.V; out.B_V = manifest.B_V;
    out.R = manifest.R;
    return out;
}

class DistinctIdsHost final : public pineforge::source::PineStrategyHost {
public:
    explicit DistinctIdsHost(const pf_scratch_tables_v1& tables) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 1;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        require(set_scratch_tables_v1(&tables) == 0,
                "bounded scratch descriptor must be accepted before running");
    }

    void on_source_bar(const pineforge::Bar&) override {
        const auto i = static_cast<std::size_t>(pine_bar_index());
        require(i == callbacks, "one source callback per consecutive chart bar");
        ++callbacks;
        const auto round = i / 4;
        const auto suffix = std::to_string(round);
        const auto entry_id = "Long " + suffix;
        switch (i % 4) {
        case 0:
            require(live_position_size() == 0.0 && pending_order_count() == 0,
                    "each round starts flat without a carried stop or entry");
            require(trade_count() == static_cast<int>(round),
                    "every earlier round must have exactly one closed trade");
            // The first two price arguments are limit and stop, NOT quantity.
            strategy_entry(entry_id, true, kNaN, kNaN, 1.0);
            ++entries;
            break;
        case 1:
            require(live_position_size() == 1.0 && pending_order_count() == 0,
                    "market entry must fill at the next open");
            strategy_exit("Long stop " + suffix, entry_id, kNaN, 1.0);
            ++exits;
            break;
        case 2:
            require(live_position_size() == 1.0 && pending_order_count() == 1,
                    "stop below every chart low must remain pending for one bar");
            strategy_close_all();
            ++closes;
            break;
        case 3:
            require(live_position_size() == 0.0 && pending_order_count() == 0,
                    "market close must fill promptly and remove its pending stop");
            require(trade_count() == static_cast<int>(round + 1),
                    "idle bar must observe exactly one newly closed trade");
            ++completed_rounds; // No strategy commands on this bar.
            break;
        }
    }

    std::size_t entries = 0, exits = 0, closes = 0;
    std::size_t callbacks = 0, completed_rounds = 0;
};

std::vector<pineforge::Bar> chart() {
    std::vector<pineforge::Bar> bars(kBars);
    for (std::size_t i = 0; i < kBars; ++i)
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   kStart + static_cast<std::int64_t>(i) * 60'000};
    return bars;
}

void default_manifest_admits(const std::vector<pineforge::Bar>& bars) {
    // Deliberately use this tree's default, without raising individual fields.
    // On feae33694 this same run is expected to fail at the old fixed limits.
    const auto tables = descriptor(pineforge::detail::default_scratch_manifest(kBars));
    DistinctIdsHost host(tables);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!host.last_error().empty())
        throw std::runtime_error("case A default manifest refused: " + host.last_error());
    require(std::string(pineforge::run_failure_code_of(host)).empty(),
            "case A must have no recorded failure code");
    require(host.callbacks == kBars && host.completed_rounds == kRounds,
            "case A must execute all 20,000 bars and all 5,000 rounds");
    require(host.entries == 5'000 && host.exits == 5'000 && host.closes == 5'000
                && host.entries + host.exits == kDistinctIds,
            "case A must submit exactly 10,000 distinct entry/stop ids");
    require(host.trade_count() == 5'000,
            "case A must record exactly 5,000 closed trades");
    require(host.live_position_size() == 0.0 && host.pending_order_count() == 0,
            "case A must finish flat without pending orders");
    // Check every entry spelling and fill, not just the command counters.
    for (int n = 0; n < 5'000; ++n) {
        const auto& trade = host.get_trade(n);
        require(trade.entry_id == "Long " + std::to_string(n) && trade.qty == 1.0,
                "each closed trade must belong to its own fresh one-unit entry");
        require(trade.entry_price == 100.0 && trade.exit_price == 100.0,
                "trades must close at the market, never at the distant stop");
        require(trade.entry_time == kStart + (4LL * n + 1) * 60'000
                    && trade.exit_time == kStart + (4LL * n + 3) * 60'000,
                "every entry and market close must fill on its next bar");
    }
}

void old_manifest_refuses(const std::vector<pineforge::Bar>& bars) {
    // Historical fixed maxima, independent of default_scratch_manifest().
    const pineforge::detail::ScratchManifest old{
        0, 0, 8192, 3145728, 4096, 2101248,
        4096, 1048576, 27, 6912, 2 * kBars};
    DistinctIdsHost host(descriptor(old));
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(!host.last_error().empty(), "case B must reproduce the historical refusal");
    // run() captures ScratchCapacityError; assert its carried code/resource
    // through the same host error channel as test_source_resident_cohort_ceiling.
    require(std::string(pineforge::run_failure_code_of(host)) == "scratch_capacity_exceeded"
                && std::string(pineforge::run_failure_args_of(host))
                    == "{\"resource\":\"erased_index\"}",
            "case B must be a typed retired-key refusal, not layout or input failure");
    require(host.last_error().find("more than 8,192 retired order-id keys")
                != std::string::npos,
            "case B must name the historical K_e count limit");
    // Each round retires two keys for its entry and two for its stop.
    // The recurring __close__ id adds two more ever-keys: 4*2047+2=8190,
    // while 4*2048+2=8194 exceeds K_e=8192. The final stop stays pinned
    // until the next position cycle opens, so entry 2049 is already filled
    // when retirement refuses, before that round can submit its stop.
    require(host.completed_rounds == 2'048 && host.trade_count() == 2'048,
            "case B must close 2,048 rounds before the retired-key refusal");
    require(host.entries == 2'049 && host.exits == 2'048 && host.closes == 2'048
                && host.entries + host.exits == 4'097,
            (std::string("case B must refuse after 4,096 completed-round ids plus the next entry; observed entries ")
                + std::to_string(host.entries) + ", exits " + std::to_string(host.exits)
                + ", closes " + std::to_string(host.closes)).c_str());
    require(host.trade_count() < 5'000, "case B must stop short of case A's trade count");
}
} // namespace

int main() {
    try {
        const auto bars = chart();
        default_manifest_admits(bars);
        old_manifest_refuses(bars);
        std::puts("CR-400 distinct ids admitted: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR-400 distinct ids admitted: %s\n", error.what());
        return 1;
    }
}
