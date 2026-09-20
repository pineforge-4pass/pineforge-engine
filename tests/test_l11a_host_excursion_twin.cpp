// R4-D L11a (RULING A48), the source-host TWIN: the source host owns per-lot
// excursion accounting behind ONE generic kernel capability. The kernel no
// longer samples the lot at the matched trigger price and no longer folds
// bar-path extremes into the closing row; both magnitudes come from the
// host's own sampler. Pins the owner rows around the first divergence of
// order-stop-entry-reversal-grouping-01 (favorable 13.64, exact — the
// half-tick kernel overshoot 13.645 is gone) and the entry-bar mask rows of
// the composite-scalping shape (same-bar priced entry + priced exit).
// Bars are embedded; this test must never open corpus files. This half binds
// pineforge/source, so tests/CMakeLists.txt registers it only when
// PINEFORGE_BUILD_SOURCE_LAYER is ON; the kernel half (a bare host and an
// owning native host) is tests/test_l11a_host_excursion.cpp.
#include "l4a_native_route_guard.hpp"

#include "l11a_host_excursion_fixture.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {
using namespace l11a_fixture;

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

// One generic capability: the source host declares ownership, every other
// native host keeps the kernel's own generic excursion model.
class DeclaresOwnership : public source::PineStrategyHost {
public:
    DeclaresOwnership() { configure_pine_strategy(cfg(2)); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {}
};

// order-stop-entry-reversal-grouping-01 around trade #801/#802: a long lot
// opened at the 01:00 open and stopped out at the very high of that bar.
// Owner favorable == net pnl == 13.64 exactly (ab9714be engine_trades.csv).
class StopReversal : public source::PineStrategyHost {
public:
    StopReversal() { configure_pine_strategy(cfg(2)); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 1 && live_position_size() == 0.0)
            strategy_entry("L1", true, kNaN, kNaN, 1.0, "long lot 1");
        if (i == 2 && live_position_size() > 0.0)
            strategy_entry("L2", true, kNaN, kNaN, 1.0, "long lot 2");
        if (i == 3 && live_position_size() > 0.0)
            strategy_entry("SREV", false, kNaN, bar.high, 1.0, "short stop reversal");
        if (i == 5 && live_position_size() < 0.0)
            strategy_entry("LREV", true, kNaN, bar.low, 1.0, "long stop reversal");
        if (i == 7 && live_position_size() != 0.0)
            strategy_close_all();
    }
};

// composite-scalping shape: a priced short entry and a priced exit on the
// SAME bar. The entry-bar extreme the assumed path reaches before the entry
// fill is masked out of the lot (ab9714be pine_fills.cpp:42), so the owner
// reports mfe 0.00 / mae == the pre-exit path extreme only.
class SameBarScalp : public source::PineStrategyHost {
public:
    SameBarScalp() { configure_pine_strategy(cfg(1)); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (placed_) return;
        strategy_entry("S", false, kNaN, 1810.00, 1.0, "stop short");
        strategy_exit("X", "S", 1818.00, kNaN, kNaN, kNaN, kNaN, 100.0, "stop buy back");
        placed_ = true;
    }
private:
    bool placed_ = false;
};

}  // namespace

int main() {
    {
        DeclaresOwnership host;
        CHECK(host.owns_lot_excursions());
        ClosedLotExcursionFacts facts;
        facts.carried_favorable = 4.0;
        facts.carried_adverse = 2.0;
        facts.entry_price = 100.0;
        facts.fill_price = 103.0;
        facts.lot_qty = 1.0;
        facts.closed_qty = 1.0;
        facts.is_long = true;
        // The supplier is the host's own model: carried extremes scaled to the
        // closed slice, with the exit fill itself always inside the trade.
        const ClosedLotExcursion owned = host.closed_lot_excursion(facts);
        CHECK(near(owned.favorable, 4.0));
        CHECK(near(owned.adverse, 2.0));
        facts.closed_qty = 0.5;
        const ClosedLotExcursion half = host.closed_lot_excursion(facts);
        CHECK(near(half.favorable, 2.0));
        CHECK(near(half.adverse, 1.0));
    }
    {
        StopReversal host;
        const std::vector<Bar> bars = {
            mk(1760659200000LL, 3892.02, 3903.63, 3886.15, 3901.02),
            mk(1760660100000LL, 3901.01, 3913.4, 3896.69, 3912.13),
            mk(1760661000000LL, 3912.15, 3924.0, 3906.38, 3918.01),
            mk(1760661900000LL, 3918.01, 3925.79, 3912.0, 3925.79),
            mk(1760662800000LL, 3925.8, 3948.06, 3920.1, 3933.02),
            mk(1760663700000LL, 3933.02, 3940.74, 3924.57, 3928.91),
            mk(1760664600000LL, 3928.92, 3932.0, 3907.93, 3917.78),
            mk(1760665500000LL, 3917.79, 3921.7, 3904.0, 3918.53),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        bool found = false;
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& t = host.get_trade(i);
            if (t.is_long && near(t.entry_price, 3912.15) && near(t.exit_price, 3925.79)) {
                found = true;
                // Owner row: favorable == 13.64 exactly, not 13.645.
                expect("reversal#801", t, true, 3912.15, 3925.79, 13.64, 5.77);
                break;
            }
        }
        CHECK(found);
    }
    {
        SameBarScalp host;
        // Low-first bar (close < open): the path runs open -> low -> high ->
        // close, so the stop short at 1810.00 fills on the way down and the
        // 1806.40 low belongs to the lot, while the pre-fill high side is
        // masked. The stop buy back at 1818.00 fills on the way up.
        const std::vector<Bar> bars = {
            mk(1743397200000LL, 1804.00, 1813.00, 1803.33, 1811.96),
            mk(1743398100000LL, 1812.10, 1819.40, 1806.40, 1817.25),
            mk(1743399000000LL, 1817.25, 1820.00, 1815.00, 1819.10),
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            const auto& t = host.get_trade(0);
            // High-first bar: the 1819.40 high precedes the stop-short fill,
            // so it is masked out of the lot and out of the exit fold; the
            // post-fill low 1806.40 gives mfe 3.60 and the buy-back fill
            // itself gives mae 8.00 (ab9714be pine_fills.cpp:42 + :5741).
            expect("same-bar-scalp#1", t, false, 1810.00, 1818.00, 3.60, 8.00);
        }
    }
    std::printf("test_l11a_host_excursion_twin: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
