/*
 * test_exit_reissue_na_tapes.cpp -- R5 lane TAIL-D.
 *
 * A strategy.exit call that names a level replaces the whole exit order: a
 * price leg the new call leaves na is withdrawn on TradingView, a stop and a
 * limit alike. Pinned against TradingView behaviour (recorded tapes are kept
 * outside this repository). The adapter must replace every leg of the id.
 *
 * Invented fifteen-minute bars first cross a withdrawn long stop, then its
 * retained limit. The paired control retains its stop and closes earlier.
 * Shorts then cross a withdrawn limit; their paired control retains that
 * limit, while the survivor closes at its replacement stop. Both omissions
 * use runtime na. Every trade's times, side, prices and quantity are checked
 * against independently specified synthetic rows.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.001;

// (entry ms, long, entry ticks, quantity, exit ms, exit ticks)
using Row = std::tuple<std::int64_t, bool, long long, long long, std::int64_t, long long>;

long long ticks(double price) { return std::llround(price / kTick); }

// Arbitrary epoch, independent of any market fixture.
std::int64_t at(int index) {
    return 947545200000LL + index * 900000LL;
}

std::vector<Row> tape_trades() {
    // One unit per entry; ticks = price / 0.001. Rows follow exit order:
    // retained stop 96, retained limit 108, retained limit 92, new stop 106.
    return {{at(1), true, 100000, 1, at(2), 96000},
            {at(1), true, 100000, 1, at(3), 108000},
            {at(5), false, 104000, 1, at(7), 92000},
            {at(5), false, 104000, 1, at(8), 106000}};
}

// Two paired controls isolate withdrawal of each exit leg.
class ReissueNaHost final : public source::PineStrategyHost {
public:
    ReissueNaHost() {
        attach_pine_execution_adapter();
        set_syminfo_session("1800-1700");
        set_syminfo_timezone("America/New_York");
        source::PineStrategyConfig c;
        c.pyramiding = 2;
        c.initial_capital = 10000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        configure_pine_strategy(c);
        set_syminfo_metadata("qty_step", 0.01);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double na_level = kNaN;  // var float naLevel = na
        if (t == at(0)) {
            strategy_entry("C", true);
            strategy_entry("D", true);
            strategy_exit("CX", "C", 108.0, 96.0);
            strategy_exit("DX", "D", 108.0, 96.0);
        }
        if (t == at(1)) {
            strategy_exit("CX", "C", 108.0, na_level);
            strategy_exit("DX", "D", 108.0, 96.0);
        }
        if (t == at(4) && signed_position_size() == 0.0) {
            strategy_entry("A", false);
            strategy_entry("B", false);
            strategy_exit("AX", "A", 92.0, 112.0);
            strategy_exit("BX", "B", 92.0, 112.0);
        }
        if (t == at(6)) {
            strategy_exit("AX", "A", na_level, 106.0);
            strategy_exit("BX", "B", 92.0, 106.0);
        }
        if (t == at(9)) strategy_close_all();
    }
};

std::vector<Bar> feed() {
    return {{100.0, 101.0, 99.0, 100.0, 1.0, at(0)},
            {100.0, 101.0, 99.0, 100.0, 1.0, at(1)},
            {100.0, 101.0, 95.0, 100.0, 1.0, at(2)},
            {100.0, 109.0, 99.0, 108.0, 1.0, at(3)},
            {104.0, 105.0, 103.0, 104.0, 1.0, at(4)},
            {104.0, 105.0, 103.0, 104.0, 1.0, at(5)},
            {104.0, 105.0, 103.0, 104.0, 1.0, at(6)},
            {104.0, 105.0, 91.0, 94.0, 1.0, at(7)},
            {94.0, 107.0, 93.0, 104.0, 1.0, at(8)},
            {104.0, 105.0, 103.0, 104.0, 1.0, at(9)},
            {104.0, 105.0, 103.0, 104.0, 1.0, at(10)}};
}

void show(const char* tag, const std::vector<Row>& rows) {
    for (const Row& r : rows)
        std::printf("    %-6s %lld %s @%lld q=%lld -> %lld @%lld\n", tag,
                    static_cast<long long>(std::get<0>(r)), std::get<1>(r) ? "long" : "short",
                    std::get<2>(r), std::get<3>(r), static_cast<long long>(std::get<4>(r)),
                    std::get<5>(r));
}

}  // namespace

int main() {
    std::printf("-- exit-reissue-na-limit\n");
    const std::vector<Bar> bars = feed();
    const std::vector<Row> tape = tape_trades();
    ReissueNaHost host;
    host.set_trade_start_time(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    std::vector<Row> engine;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        engine.emplace_back(t.entry_time, t.is_long, ticks(t.entry_price),
                            std::llround(t.qty), t.exit_time, ticks(t.exit_price));
    }
    CHECK(engine.size() == 4);
    CHECK(engine == tape);
    if (engine != tape) {
        show("tape", tape);
        show("engine", engine);
    }
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
