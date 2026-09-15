// Native-route literal twin for the L0 stop-snapshot placement rule:
// percentage sizing freezes at the directionally snapped stop, not the close
// or a later gap-through quote.  The 858 quantity is from the L0 F@15 cells.
#include <pineforge/source/pine_native_host.hpp>

#include "oracle_fixture_config_shim.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::printf("FAIL %d: %s\n", __LINE__, #expr); } } while (false)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class StopSnapshotProbe final : public source::PineNativeHost {
public:
    StopSnapshotProbe() {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        margin_long_ = margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        qty_step_ = 1.0;
        set_syminfo_mintick(0.01);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true, kNaN, 11.65, kNaN, "EXPANSION UP");
            double qty = kNaN;
            int close_only = -1;
            int partition = -1;
            placement_result_ = probe_fill_qty(0, 11.65, &qty, &close_only, &partition);
            placement_qty_ = qty;
            placement_partition_ = partition;
        } else if (bar_index_ == 2 && live_position_size() > 0.0) {
            strategy_close_all();
        }
    }

    int placement_result() const noexcept { return placement_result_; }
    double placement_qty() const noexcept { return placement_qty_; }
    int placement_partition() const noexcept { return placement_partition_; }

private:
    int placement_result_ = -1;
    double placement_qty_ = kNaN;
    int placement_partition_ = -1;
};
} // namespace

int main() {
    StopSnapshotProbe host;
    const std::vector<Bar> bars = {
        {11.50, 11.55, 11.45, 11.50, 1000.0, 1'000},
        {11.52, 11.70, 11.50, 11.66, 1000.0, 2'000},
        {11.66, 11.68, 11.60, 11.62, 1000.0, 3'000},
        {11.62, 11.64, 11.58, 11.60, 1000.0, 4'000},
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    // floor(10,000 / ceil_tick(11.65)) = 858, the L0 frozen stop literal.
    CHECK(host.placement_result() == 0);
    CHECK(std::abs(host.placement_qty() - 858.0) < 1e-12);
    CHECK(host.placement_partition() == 1);
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& trade = host.get_trade(0);
        CHECK(trade.is_long);
        CHECK(std::abs(trade.entry_price - 11.65) < 1e-12);
        CHECK(std::abs(trade.qty - 858.0) < 1e-12);
    }
    std::printf("R4-D native stop-snapshot twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
