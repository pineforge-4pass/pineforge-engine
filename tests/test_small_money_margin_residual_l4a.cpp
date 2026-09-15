#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Public residual-money twin.  It preserves the adjacent-binary64 boundary
// that distinguishes a genuine rounded-money deficit from an exact funded
// long, without seeding a legacy trade/PendingOrder ledger.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kQuantity = 891538.56;
int passed = 0;
int failed = 0;
#define CHECK(expr) do {                                                       \
    if (expr) ++passed; else {                                                 \
        ++failed; std::printf("FAIL %d %s\n", __LINE__, #expr);               \
    }                                                                          \
} while (0)
bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

class ResidualHost final : public source::PineStrategyHost {
public:
    explicit ResidualHost(double capital, bool enabled = true) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 0;
        qty_step_ = 0.01;
        set_syminfo_mintick(0.00001);
        set_margin_call_enabled(enabled);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, kNaN, kNaN, kQuantity);
        if (pine_bar_index() == 1)
            strategy_close("L", "survivor");
    }

    int margin_rows() const {
        int count = 0;
        for (int index = 0; index < trade_count(); ++index)
            count += get_trade(index).exit_comment == "Margin call";
        return count;
    }
    double position() const { return physical_position().signed_units; }
};

const Bar kTape[] = {
    {1.15776, 1.15798, 1.15754, 1.15798, 1.0, 1749754800000LL},
    {1.15798, 1.15808, 1.15760, 1.15761, 1.0, 1749755700000LL},
    {1.15762, 1.15798, 1.15748, 1.15788, 1.0, 1749756600000LL},
    {1.15788, 1.15804, 1.15762, 1.15762, 1.0, 1749757500000LL},
};

void test_real_one_unit_rounding_deficit_closes_before_the_script_close() {
    ResidualHost host(1032383.8221438);
    host.run(kTape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    CHECK(host.margin_rows() == 1);
    CHECK(host.get_trade(0).exit_comment == "Margin call");
    CHECK(near(host.get_trade(0).qty, 1.0));
    CHECK(near(host.get_trade(0).entry_price, 1.15798, 1e-12));
    CHECK(near(host.get_trade(0).exit_price, 1.15808, 1e-12));
    CHECK(host.get_trade(0).entry_time == kTape[1].timestamp);
    CHECK(host.get_trade(0).exit_time == kTape[1].timestamp);
    CHECK(host.get_trade(1).exit_comment == "survivor");
    CHECK(near(host.get_trade(1).qty, kQuantity - 1.0, 1e-6));
    CHECK(near(host.position(), 0.0));
}

void test_exact_funded_control_has_no_margin_row() {
    ResidualHost host(1032383.8221440);
    host.run(kTape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(host.margin_rows() == 0);
    CHECK(host.get_trade(0).exit_comment == "survivor");
    CHECK(near(host.get_trade(0).qty, kQuantity, 1e-6));
    CHECK(near(host.get_trade(0).exit_price, 1.15762, 1e-12));
    CHECK(near(host.position(), 0.0));
}

void test_emulator_switch_suppresses_the_same_residual() {
    ResidualHost host(1032383.8221438, false);
    host.run(kTape, 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(host.margin_rows() == 0);
    CHECK(host.get_trade(0).exit_comment == "survivor");
    CHECK(near(host.get_trade(0).qty, kQuantity, 1e-6));
    CHECK(near(host.position(), 0.0));
}

}  // namespace

int main() {
    test_real_one_unit_rounding_deficit_closes_before_the_script_close();
    test_exact_funded_control_has_no_margin_row();
    test_emulator_switch_suppresses_the_same_residual();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
