/*
 * test_adapter_dual_entry_tie.cpp -- R5 INT24, the supervisor's ruling on
 * R5 lane B-ADAPTER's finding 1: strategy_last_bar_dual_entry_path reports
 * the leg order the run walks.
 *
 * A flat script with a long stop entry above the open and a short stop entry
 * below it, both reached on one bar: the run fills the entry on the leg it
 * walks first. Under Auto that leg is the kernel's open-proximity rule,
 * path_uses_high_first in src/native_execution_consumer.cpp -- the high leg
 * first only when |H - O| < |O - L|, so a tie walks the low leg first, as
 * the legacy engine did (ab9714be engine_path_resolve.cpp,
 * bar_path_uses_high_first). The bar-open observation restated the rule
 * with `<=` (since 73817c1d), so on a tie bar it reported 1 (long first)
 * while the run filled the short entry first. It now asks the kernel
 * (PineExecutionAdapter::source_path_uses_high_first), as the source layer's
 * other path-order readers do. Fills read the observation only as "an
 * arbitration happened" (non-zero), so no trade moved; the value is folded
 * into the Pine state hash.
 *
 * Each case replays the scratch measurement B-ADAPTER took
 * (exec/B-ADAPTER-scratch/h5/tie.cpp): long stop 101, short stop 99, the
 * second bar opening at 100. The observation, read on that bar both through
 * the host and through the C surface's seam, must name the entry the run
 * filled first -- the first closed row's entry side -- and the leg the
 * declared path order walks first.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class Host : public pineforge::source::PineStrategyHost {
public:
    Host() {
        initial_capital_ = 100000.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
    }
    int seen = -1;
    int seen_c = -1;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, 101.0);
            strategy_entry("S", false, kNaN, 99.0);
        }
        if (bar_index_ == 1) {
            seen = last_bar_dual_entry_path();
            seen_c = static_cast<const BacktestEngine&>(*this)
                         .observe_last_bar_dual_entry_path_v1();
        }
    }
};

// path_order: 0 Auto, 1 HighFirst, 2 LowFirst (BacktestEngine::set_path_order).
// want: 1 the long entry fills first, 2 the short entry fills first.
void one(const char* name, int path_order, double high, double low, int want) {
    std::vector<Bar> bars(3);
    for (int i = 0; i < 3; ++i) {
        bars[i].timestamp = 1700000000000LL + i * 3600000LL;
        bars[i].open = 100.0;
        bars[i].high = 100.2;
        bars[i].low = 99.8;
        bars[i].close = 100.0;
        bars[i].volume = 1.0;
    }
    bars[1].high = high;
    bars[1].low = low;
    Host h;
    h.set_path_order(path_order);
    h.run(bars.data(), 3);
    std::printf("%s path_order=%d H=%.2f L=%.2f |H-O|=%.17g |O-L|=%.17g observation=%d/%d trades=%d\n",
                name, path_order, high, low, std::abs(high - 100.0), std::abs(100.0 - low), h.seen,
                h.seen_c, h.trade_count());
    CHECK(h.last_error().empty());
    CHECK(h.trade_count() == 1);
    if (h.trade_count() < 1) return;
    const Trade& first = h.get_trade(0);
    std::printf("   row 0 %s entry %s @%.2f exit %s @%.2f\n", first.is_long ? "long" : "short",
                first.entry_id.c_str(), first.entry_price, first.exit_id.c_str(), first.exit_price);
    const int filled_first = first.is_long ? 1 : 2;
    // The run: the declared order's first leg fills its own side's entry.
    CHECK(filled_first == want);
    CHECK(first.entry_id == (want == 1 ? "L" : "S"));
    CHECK(first.entry_price == (want == 1 ? 101.0 : 99.0));
    CHECK(first.exit_price == (want == 1 ? 99.0 : 101.0));
    // The observation names that same entry, on both readers.
    CHECK(h.seen == filled_first);
    CHECK(h.seen_c == h.seen);
}

}  // namespace

int main() {
    // Auto: the open-proximity rule, strict -- a tie walks the low leg first.
    one("tie       ", 0, 101.5, 98.5, 2);
    one("high-first", 0, 101.5, 98.4, 1);
    one("low-first ", 0, 101.6, 98.5, 2);
    // A declared order decides the tie bar outright.
    one("tie/high  ", 1, 101.5, 98.5, 1);
    one("tie/low   ", 2, 101.5, 98.5, 2);
    std::printf("test_adapter_dual_entry_tie: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
