#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Literal order admission only: no Engine::run, feed or reference engine.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace pineforge;
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

class Account final : public pineforge::source::PineStrategyHost {
public:
    explicit Account(double capital = 10000) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        margin_long_ = margin_short_ = 100;
        commission_value_ = 0;
        slippage_ = 0;
        qty_step_ = 0;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (!issued_) {
            issued_ = true;
            strategy_entry("order", buy_, std::numeric_limits<double>::quiet_NaN(),
                           stop_ ? 101.0 : std::numeric_limits<double>::quiet_NaN(),
                           units_);
        }
    }
    void request(bool buy, double units, bool stop) {
        buy_ = buy;
        units_ = units;
        stop_ = stop;
        const Bar one{100, 100, 100, 100, 1, 60000};
        run(&one, 1);
    }
    size_t pending() const { return static_cast<size_t>(pending_order_count()); }
    bool physical_book_empty() const {
        return physical_position().lot_count == 0
            && physical_position().signed_units == 0.0
            && trade_count() == 0
            && live_current_equity() == fixture_configuration().initial_capital;
    }
private:
    bool issued_ = false;
    bool buy_ = true;
    bool stop_ = false;
    double units_ = std::numeric_limits<double>::quiet_NaN();
};
}

int main() {
    for (bool buy : {false, true}) for (bool stop : {false, true}) {
        // Both explicit infinity and a finite quantity whose notional
        // overflows exceed finite account resources.
        for (double units : {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::max(), 101.0}) {
            Account a;
            a.request(buy, units, stop);
            CHECK(a.pending() == 0);
            CHECK(a.physical_book_empty());
        }
        Account equality;
        equality.request(buy, 100, stop); // 100 units * price100 == capital10000
        CHECK(equality.pending() == 1);
        CHECK(equality.physical_book_empty());

        Account omitted;
        omitted.request(buy, std::numeric_limits<double>::quiet_NaN(), stop);
        CHECK(omitted.pending() == 1); // NaN remains the default-quantity sentinel
        CHECK(omitted.physical_book_empty());

        // Even the largest finite equity cannot fund an infinite cost.
        // Adding the comparison tolerance to this balance overflows, so a
        // plain required > (balance + epsilon) would otherwise miss it.
        Account largest_balance(std::numeric_limits<double>::max());
        largest_balance.request(buy, std::numeric_limits<double>::max(), stop);
        CHECK(largest_balance.pending() == 0);
        CHECK(largest_balance.physical_book_empty());
    }
    std::printf("unbounded margin admission: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
