#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Public native-route admission twin.  It covers the source command boundary
// that precedes a margin-policy slice: an exact-funded explicit opening is
// accepted, while an over-notional opening is absent before native matching.

#include <cmath>
#include <cstdio>
#include <limits>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
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

class AdmissionHost final : public source::PineStrategyHost {
public:
    AdmissionHost(bool is_long, double units, double capital)
        : is_long_(is_long), units_(units) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = units;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 1;
        set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("entry", is_long_, kNaN, kNaN, units_);
    }

    double position() const { return physical_position().signed_units; }
    std::size_t lots() const { return physical_position().lot_count; }

private:
    bool is_long_;
    double units_;
};

void test_exact_margin_tie_is_admitted() {
    const Bar tape[] = {
        {100.0, 100.0, 100.0, 100.0, 1.0, 1000},
        {100.0, 100.0, 100.0, 100.0, 1.0, 2000},
    };
    for (bool is_long : {false, true}) {
        AdmissionHost host(is_long, 10.0, 1000.0);
        host.run(tape, 2);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 0);
        CHECK(near(host.position(), is_long ? 10.0 : -10.0));
        CHECK(host.lots() == 1);
    }
}

void test_over_notional_opening_is_dropped_at_source_command_boundary() {
    const Bar tape[] = {
        {100.0, 100.0, 100.0, 100.0, 1.0, 1000},
        {100.0, 100.0, 100.0, 100.0, 1.0, 2000},
    };
    for (bool is_long : {false, true}) {
        AdmissionHost host(is_long, 10.01, 1000.0);
        host.run(tape, 2);
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 0);
        CHECK(near(host.position(), 0.0));
        CHECK(host.lots() == 0);
    }
}

}  // namespace

int main() {
    test_exact_margin_tie_is_admitted();
    test_over_notional_opening_is_dropped_at_source_command_boundary();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
