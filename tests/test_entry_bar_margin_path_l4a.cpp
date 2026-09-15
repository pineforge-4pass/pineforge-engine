#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// Public-command entry-path margin twin.  The three retained registry tapes
// distinguish a stop filled after the pre-fill extreme, an opening fill, and
// an unleveraged entry whose pre-fill high must never manufacture a slice.

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

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
bool near(double a, double b, double tolerance = 1e-7) {
    return std::abs(a - b) <= tolerance;
}

Bar make_bar(std::int64_t timestamp, double open, double high, double low, double close) {
    return {open, high, low, close, 1.0, timestamp};
}

class PathHost final : public source::PineStrategyHost {
public:
    enum class Entry { Stop, Market };

    PathHost(double capital, double stop, double quantity, Entry entry)
        : stop_(stop), quantity_(quantity), entry_(entry) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        pyramiding_ = 0;
        qty_step_ = 0.01;
        set_syminfo_mintick(0.005);
        set_margin_call_enabled(true);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (entry_ == Entry::Stop)
            strategy_entry("S", false, kNaN, stop_, quantity_);
        else
            strategy_entry("S", false, kNaN, kNaN, quantity_);
    }

    int margin_rows() const {
        int result = 0;
        for (int index = 0; index < trade_count(); ++index)
            result += get_trade(index).exit_comment == "Margin call";
        return result;
    }
    double position() const { return physical_position().signed_units; }

private:
    double stop_;
    double quantity_;
    Entry entry_;
};

void test_bearish_stop_sees_only_post_fill_path() {
    // OANDA:XAUUSD 15, xau15-mcpath-a.  The high 2975.73 occurs before
    // the sell-stop fill, so the first eligible adverse mark is on bar 2.
    const Bar tape[] = {
        make_bar(1744043400000LL, 2977.895, 2984.98, 2975.098, 2975.22),
        make_bar(1744044300000LL, 2975.185, 2975.73, 2969.975, 2970.925),
        make_bar(1744045200000LL, 2970.945, 2975.345, 2959.6, 2966.36),
    };
    PathHost host(10000.0, 2970.215, 3.36, PathHost::Entry::Stop);
    host.run(tape, 3);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(host.margin_rows() == 1);
    CHECK(host.get_trade(0).entry_bar_index == 1);
    CHECK(host.get_trade(0).exit_bar_index == 2);
    CHECK(near(host.get_trade(0).entry_price, 2970.215));
    CHECK(near(host.get_trade(0).exit_price, 2975.345));
    CHECK(near(host.get_trade(0).qty, 1.0));
    CHECK(host.get_trade(0).exit_comment == "Margin call");
    CHECK(near(host.position(), -2.36));
}

void test_open_fill_sees_whole_remaining_path() {
    // OANDA:XAUUSD 15, xau15-mcpath-b.  The stop is met at the opening
    // print, so the high 2980 is after the entry and is eligible.
    const Bar tape[] = {
        make_bar(1744155000000LL, 2981.015, 2981.275, 2969.97, 2973.78),
        make_bar(1744155900000LL, 2973.84, 2980.0, 2970.48, 2978.56),
    };
    PathHost host(10020.0, 2973.84, 3.36, PathHost::Entry::Stop);
    host.run(tape, 2);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(host.margin_rows() == 1);
    CHECK(host.get_trade(0).entry_bar_index == 1);
    CHECK(host.get_trade(0).exit_bar_index == 1);
    CHECK(near(host.get_trade(0).entry_price, 2973.84));
    CHECK(near(host.get_trade(0).exit_price, 2980.0));
    CHECK(near(host.get_trade(0).qty, 1.0));
    CHECK(host.get_trade(0).exit_comment == "Margin call");
    CHECK(near(host.position(), -2.36));
}

void test_pre_fill_high_cannot_create_a_phantom_margin_row() {
    // OANDA:XAUUSD 15, asian-box.  The historical high precedes the stop
    // fill; its post-fill low and close are solvent, so no margin row exists.
    const Bar tape[] = {
        make_bar(1743521400000LL, 3126.63, 3127.345, 3119.33, 3121.325),
        make_bar(1743522300000LL, 3121.33, 3124.295, 3113.44, 3113.79),
        make_bar(1743523200000LL, 3113.755, 3116.855, 3106.715, 3107.08),
    };
    PathHost host(10000.0, 3120.335, 3.2, PathHost::Entry::Stop);
    host.run(tape, 3);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 0);
    CHECK(host.margin_rows() == 0);
    CHECK(near(host.position(), -3.2));
}

}  // namespace

int main() {
    test_bearish_stop_sees_only_post_fill_path();
    test_open_fill_sees_whole_remaining_path();
    test_pre_fill_high_cannot_create_a_phantom_margin_row();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
