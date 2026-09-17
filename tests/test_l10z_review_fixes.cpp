// R4-D L10z: focused checks for the review findings this lane fixes
// (tasks/r4-d/exact/REVIEW-QWENMAX-L10-REPORT.md).  Every check drives the
// switched source host through its public surface only.  Bars are embedded
// from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv — this test must never
// open corpus files or absolute paths (CI has no corpus checkout).
#include "l4a_native_route_guard.hpp"

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(int pyramiding, bool process_orders_on_close = false) {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyramiding;
    c.process_orders_on_close = process_orders_on_close;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

// Bars from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv:
// 2025-04-10 04:45 UTC to 2025-04-10 06:45 UTC.
std::vector<Bar> sample_bars() {
    return {
        mk(1744260300000LL, 1614.60, 1620.00, 1613.83, 1618.68, 51703.598),  // 0: 04:45 call bar
        mk(1744261200000LL, 1618.67, 1623.33, 1615.76, 1622.15, 47453.600),  // 1: 05:00
        mk(1744262100000LL, 1622.15, 1625.48, 1619.30, 1623.48, 56619.446),  // 2: 05:15
        mk(1744263000000LL, 1623.47, 1624.91, 1617.55, 1619.02, 38925.067),  // 3: 05:30
        mk(1744263900000LL, 1619.02, 1619.99, 1613.58, 1616.39, 47160.450),  // 4: 05:45
        mk(1744264800000LL, 1616.40, 1622.18, 1615.13, 1619.10, 39319.511),  // 5: 06:00
        mk(1744265700000LL, 1619.09, 1619.99, 1610.05, 1611.73, 55503.545),  // 6: 06:15
        mk(1744266600000LL, 1611.73, 1612.50, 1605.61, 1608.91, 60878.670),  // 7: 06:30
        mk(1744267500000LL, 1608.90, 1616.50, 1607.44, 1615.54, 41165.902),  // 8: 06:45
    };
}

// Review fix 2 (P1, pine_adapter.cpp same_side_pending): a live row whose
// cancellation is already recorded must not compete for the pyramiding cap.
// Bar 0 places a resting limit entry "A", re-issues the same id (the successor
// row carries the recorded Replacement cancellation) and then a market entry
// "B".  With pyramiding=1 the cancelled row must not push "B" over the cap.
class CancelledRowCapHost : public source::PineStrategyHost {
public:
    CancelledRowCapHost() {
        configure_pine_strategy(cfg(1));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            strategy_entry("A", true, 1500.00, kNaN, 1.0, "resting limit");
            strategy_entry("A", true, 1510.00, kNaN, 1.0, "same-id reissue");
            strategy_entry("B", true, kNaN, kNaN, 1.0, "market entry");
        }
        if (i == 5 && live_position_size() != 0.0) strategy_close_all();
    }
};

void test_cancelled_live_row_is_not_counted_against_the_cap() {
    CancelledRowCapHost host;
    const auto bars = sample_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const auto& t0 = host.get_trade(0);
        CHECK(t0.entry_id == "B");
        CHECK(t0.is_long);
        CHECK(t0.entry_time == 1744261200000LL);
        CHECK(near(t0.entry_price, 1618.67));
        CHECK(near(t0.qty, 1.0));
    }
    CHECK(near(host.live_position_size(), 0.0));
}

// Review fix 3 (P1, pine_adapter.cpp close()): the resting-live-entry scan was
// added to has_pending_entry, but no unit check is pinned here: through the
// public host surface a same-bar strategy.close(id) against a resting limit
// entry still reaches close() with the row staged in pending_entries_ (or, on
// a later bar, with a placement_script_open_ms older than the current script
// bar), so the retirement branch is not exercised by any configuration this
// test could build.  Reported as an open gap rather than pinned by a check that
// would assert the un-retired behaviour.

// Review fix 4 (P1, native_execution_consumer.cpp): the interval lookup cache
// is per-consumer, not per-thread.  Two streaming consumers on one thread with
// different script timeframes see the same input timestamps; a shared
// timestamp-keyed cache would let the 5m consumer read the 1m consumer's
// script interval and seal its script buckets at the wrong boundary.
class IntervalDispatchHost : public source::PineStrategyHost {
public:
    std::vector<int> dispatch_bar_index;

    IntervalDispatchHost() {
        configure_pine_strategy(cfg(1));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar&) override {
        dispatch_bar_index.push_back(pine_bar_index());
    }
};

std::vector<Bar> minute_bars(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double price = 1600.0 + static_cast<double>(i % 5);
        bars.push_back(mk(1744260300000LL + static_cast<std::int64_t>(i) * 60000LL,
                          price, price + 1.0, price - 1.0, price + 0.5, 10.0));
    }
    return bars;
}

std::vector<int> solo_stream_dispatches(const std::string& script_tf,
                                       const std::vector<Bar>& warmup,
                                       const std::vector<Bar>& live) {
    IntervalDispatchHost host;
    if (!host.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "1", script_tf))
        return {};
    for (const auto& bar : live) {
        if (!host.stream_push_bar(bar)) break;
    }
    (void)host.stream_end(false);
    return host.dispatch_bar_index;
}

std::vector<int> shared_thread_dispatches(const std::string& script_tf,
                                         const std::vector<Bar>& warmup,
                                         const std::vector<Bar>& live,
                                         IntervalDispatchHost& peer) {
    IntervalDispatchHost host;
    if (!host.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "1", script_tf))
        return {};
    for (const auto& bar : live) {
        // Interleave the two consumers on this thread: the peer queries the
        // same timestamps between this consumer's bars.
        (void)peer.stream_push_bar(bar);
        if (!host.stream_push_bar(bar)) break;
    }
    (void)host.stream_end(false);
    return host.dispatch_bar_index;
}

void test_two_consumers_on_one_thread_keep_independent_intervals() {
    const auto bars = minute_bars(14);
    const std::vector<Bar> warmup(bars.begin(), bars.begin() + 2);
    const std::vector<Bar> live(bars.begin() + 2, bars.end());

    const auto solo_1m = solo_stream_dispatches("1", warmup, live);
    const auto solo_5m = solo_stream_dispatches("5", warmup, live);
    CHECK(!solo_1m.empty());
    CHECK(!solo_5m.empty());
    CHECK(solo_5m.size() < solo_1m.size());

    IntervalDispatchHost peer_1m;
    CHECK(peer_1m.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "1", "1"));
    const auto shared_5m = shared_thread_dispatches("5", warmup, live, peer_1m);
    (void)peer_1m.stream_end(false);

    IntervalDispatchHost peer_5m;
    CHECK(peer_5m.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "1", "5"));
    const auto shared_1m = shared_thread_dispatches("1", warmup, live, peer_5m);
    (void)peer_5m.stream_end(false);

    CHECK(shared_5m == solo_5m);
    CHECK(shared_1m == solo_1m);
    CHECK(peer_1m.dispatch_bar_index == solo_1m);
    CHECK(peer_5m.dispatch_bar_index == solo_5m);
}

}  // namespace

int main() {
    test_cancelled_live_row_is_not_counted_against_the_cap();
    test_two_consumers_on_one_thread_keep_independent_intervals();
    std::printf("test_l10z_review_fixes: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
