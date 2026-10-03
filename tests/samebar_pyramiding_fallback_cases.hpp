#pragma once

#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace samebar_fallback {

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_time = 1743465600000LL;

class FallbackHost : public pineforge::source::PineStrategyHost {
public:
    using Script = std::function<void(FallbackHost&, int)>;
    FallbackHost(int pyramiding, Script script) : script_(std::move(script)) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = pyramiding;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const pineforge::Bar&) override { script_(*this, pine_bar_index()); }
    using PineStrategyHost::strategy_entry;
    using PineStrategyHost::strategy_order;
    using PineStrategyHost::strategy_exit;
    using PineStrategyHost::strategy_close;
    using PineStrategyHost::strategy_cancel_all;
    using PineStrategyHost::strategy_close_all;
private:
    Script script_;
};

struct ExpectedTrade {
    std::string entry_id;
    std::string exit_id;
    bool is_long;
    double quantity;
    double entry_price;
    double exit_price;
    double pnl;
    int entry_bar;
    int exit_bar;
};

struct FallbackCase {
    std::string name;
    int pyramiding;
    int cross_bar;
    double final_position;
    FallbackHost::Script script;
    std::vector<ExpectedTrade> trades;
};

inline std::pair<int, int> run_cases(const char* filter) {
    const std::vector<FallbackCase> cases{
#include "fixtures/samebar_pyramiding_entries/fallback-S2/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S29/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S30/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S22b/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S25/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S25b/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S22/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S42/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S43/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-S28/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-seeded-three-leg-exit/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-X8c/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-Y3/case.hpp"
#include "fixtures/samebar_pyramiding_entries/fallback-seeded-three-leg-exit-default/case.hpp"
#include "fixtures/samebar_pyramiding_entries/live-exit-two-sided-entry/case.hpp"
    };
    int passed = 0;
    int failed = 0;
    for (const auto& fixture : cases) {
        if (filter && fixture.name.find(filter) == std::string::npos) continue;
        FallbackHost host(fixture.pyramiding, fixture.script);
        std::vector<pineforge::Bar> bars;
        for (int bar = 0; bar < 9; ++bar) {
            bars.push_back({100.0, bar == fixture.cross_bar ? 106.0 : 101.0,
                           bar == fixture.cross_bar ? 94.0 : 99.0, 100.0, 1.0,
                           start_time + bar * 900000LL});
        }
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
        bool correct = host.last_error().empty()
            && host.trade_count() == static_cast<int>(fixture.trades.size())
            && host.live_position_size() == fixture.final_position;
        for (int index = 0; index < host.trade_count(); ++index) {
            const auto actual = host.get_trade(index);
            std::printf("%s row %d %s/%s side=%d qty=%.17g prices=%.17g/%.17g "
                        "pnl=%.17g bars=%lld/%lld\n", fixture.name.c_str(), index,
                        actual.entry_id.c_str(), actual.exit_id.c_str(), actual.is_long,
                        actual.qty, actual.entry_price, actual.exit_price, actual.pnl,
                        static_cast<long long>((actual.entry_time-start_time)/900000LL),
                        static_cast<long long>((actual.exit_time-start_time)/900000LL));
            if (index >= static_cast<int>(fixture.trades.size())) continue;
            const auto& expected = fixture.trades[static_cast<std::size_t>(index)];
            correct = correct && actual.entry_id == expected.entry_id
                && actual.exit_id == expected.exit_id && actual.is_long == expected.is_long
                && actual.qty == expected.quantity && actual.entry_price == expected.entry_price
                && actual.exit_price == expected.exit_price && actual.pnl == expected.pnl
                && actual.entry_time == start_time + expected.entry_bar * 900000LL
                && actual.exit_time == start_time + expected.exit_bar * 900000LL;
        }
        if (correct) ++passed;
        else {
            ++failed;
            std::printf("FAIL %s: expected %zu rows position %.17g, got %d rows position "
                        "%.17g error=%s\n", fixture.name.c_str(), fixture.trades.size(),
                        fixture.final_position, host.trade_count(), host.live_position_size(),
                        host.last_error().c_str());
        }
    }
    return {passed, failed};
}

}
