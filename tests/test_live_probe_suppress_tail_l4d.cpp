// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
Bar bar(double o, double h, double l, double c, int64_t ts) { return Bar{o, h, l, c, 1.0, ts}; }
// Re-issues a stop exit every bar (the dominant Pine idiom) at 1% below the
// current close, entering long on bar 1. Snapshots the pending book as seen
// at on_bar entry (= the book in force during that bar).
class ReissueStop final : public pineforge::source::PineStrategyHost {
public:
    using BacktestEngine::position_side_;  // expose the protected member for CHECKs below
    std::vector<std::vector<double>> book_at_on_bar_entry;  // stop prices per bar
    int on_bar_calls = 0;
    void on_source_bar(const Bar& b) override {
        ++on_bar_calls;
        std::vector<double> stops;
        for (const auto& o : pending_orders_) stops.push_back(o.legs.prices().stop_price);
        book_at_on_bar_entry.push_back(stops);
        if (bar_index_ == 1) strategy_entry("L", true);
        if (bar_index_ >= 1) strategy_exit("x", "L", na<double>(), b.close * 0.99);
    }
    std::vector<double> book_now() const {
        std::vector<double> stops;
        for (const auto& o : pending_orders_) stops.push_back(o.legs.prices().stop_price);
        return stops;
    }
};
}
int main() {
    const std::vector<Bar> bars = {
        bar(100, 101, 99, 100, 0), bar(100, 102, 99, 101, 60'000),
        bar(101, 103, 100, 102, 120'000), bar(102, 104, 101, 103, 180'000),
    };
    ReissueStop plain;
    plain.run(bars.data(), 4);
    CHECK(plain.on_bar_calls == 4);
    const std::vector<double> in_force_on_last = plain.book_at_on_bar_entry[3];  // stop from bar 2's close
    CHECK(in_force_on_last.size() == 1);

    ReissueStop probe;
    probe.set_probe_suppress_tail_logic(true);
    probe.run(bars.data(), 4);
    CHECK(probe.on_bar_calls == 3);                       // tail on_bar skipped
    CHECK(probe.book_now() == in_force_on_last);          // post-run book == in-force book
    CHECK(probe.trade_count() == plain.trade_count());    // no tail fill differs (stop not touched)

    // Discriminating case: the tail bar's low CROSSES the in-force stop
    // (bar 2's close 102 * 0.99 = 100.98; tail low here is 100.5). A bare
    // `return;` in the suppressed branch would leave the stop resting and
    // the position open -- only actually running process_pending_orders
    // against the forming bar produces the settled fill spec S3.2 requires.
    const std::vector<Bar> crossing_bars = {
        bar(100, 101, 99, 100, 0), bar(100, 102, 99, 101, 60'000),
        bar(101, 103, 100, 102, 120'000), bar(102, 104, 100.5, 103, 180'000),
    };
    ReissueStop plain2;
    plain2.run(crossing_bars.data(), 4);
    CHECK(plain2.trade_count() == 1);
    CHECK(near(plain2.get_trade(0).exit_price, 100.98));  // stop fill, step 1 of dispatch_bar

    ReissueStop probe2;
    probe2.set_probe_suppress_tail_logic(true);
    probe2.run(crossing_bars.data(), 4);
    CHECK(probe2.trade_count() == 1);                              // tail fill happened
    CHECK(near(probe2.get_trade(0).exit_price, 100.98));           // same fill as plain2
    CHECK(near(probe2.get_trade(0).exit_price, plain2.get_trade(0).exit_price));
    CHECK(probe2.book_now().empty());                              // stop consumed, no re-issue (on_bar skipped)
    CHECK(probe2.position_side_ == PositionSide::FLAT);            // settled book: position closed

    return failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
