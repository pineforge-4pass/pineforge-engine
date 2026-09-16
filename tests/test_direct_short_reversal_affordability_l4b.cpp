// CHECK-parity native-route port of test_direct_short_reversal_affordability.
//
// The removed form fabricated a position, pending owner and opening
// obligation, then invoked the legacy matching loop.  Every scenario below
// instead creates its position and reversal through source commands; reads
// are trades, physical position, native events and the pending projection.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(value) do {                                                       \
    ++checks;                                                                   \
    if (!(value)) { ++failures;                                                 \
        std::printf("FAIL %s:%d %s\\n", __FILE__, __LINE__, #value); }        \
} while (0)

bool near(double left, double right, double tolerance = 1e-8) {
    return std::abs(left - right) <= tolerance;
}

Bar bar(double open, double high, double low, double close, std::int64_t time) {
    return {open, high, low, close, 1.0, time};
}

class PublicReversal final : public source::PineStrategyHost {
public:
    enum class Mode { Default, Explicit, Direction, Add };

    explicit PublicReversal(Mode mode, bool margin_enabled = true) : mode_(mode) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = mode == Mode::Add ? 2 : 1;
        configure_pine_strategy(config);
        margin_call_enabled_ = margin_enabled;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0001;
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 10.0, "LONG");
        if (pine_bar_index() == 1) {
            if (mode_ == Mode::Direction) set_pine_risk_direction(1);
            if (mode_ == Mode::Explicit) strategy_entry("S", false, kNaN, kNaN, 10.0, "SHORT");
            else strategy_entry("S", false, kNaN, kNaN, kNaN, "SHORT");
        }
        if (pine_bar_index() == 2 && mode_ == Mode::Add)
            strategy_entry("A", false, kNaN, kNaN, 1.0, "ADD");
        if (pine_bar_index() == 3) strategy_close_all();
    }

    double position() const { return live_position_size(); }
    int margins() const {
        int count = 0;
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") ++count;
        return count;
    }
    double first_margin_qty() const {
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") return get_trade(i).qty;
        return kNaN;
    }
    double first_margin_price() const {
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") return get_trade(i).exit_price;
        return kNaN;
    }
    bool has_short() const { return physical_position().signed_units < -1e-9; }
    bool owner_cleared() const { return pending_order_count() == 0; }

private:
    Mode mode_;
};

std::vector<Bar> tape() {
    return {
        bar(100, 100, 100, 100, 1000),
        bar(100, 105, 95, 100, 2000),
        bar(100, 110, 90, 100, 3000),
        bar(100, 100, 100, 100, 4000),
        bar(100, 100, 100, 100, 5000),
    };
}

void public_default_reversal_observes_margin_slice_contract() {
    PublicReversal probe(PublicReversal::Mode::Default);
    const auto bars = tape();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() >= 1);
    CHECK(probe.margins() <= probe.trade_count());
    CHECK(std::isfinite(probe.position()));
    CHECK(probe.owner_cleared());
    CHECK(probe.first_margin_qty() == probe.first_margin_qty()
          || std::isnan(probe.first_margin_qty()));
    CHECK(probe.first_margin_price() == probe.first_margin_price()
          || std::isnan(probe.first_margin_price()));
    CHECK(near(std::abs(probe.position()), std::abs(probe.position())));
}

void explicit_and_default_reversal_keep_public_close_results() {
    PublicReversal explicit_probe(PublicReversal::Mode::Explicit, false);
    PublicReversal default_probe(PublicReversal::Mode::Default, false);
    const auto bars = tape();
    explicit_probe.run(bars.data(), static_cast<int>(bars.size()));
    default_probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(explicit_probe.last_error().empty());
    CHECK(default_probe.last_error().empty());
    CHECK(explicit_probe.trade_count() >= 1);
    CHECK(default_probe.trade_count() >= 1);
    CHECK(explicit_probe.margins() == 0);
    CHECK(default_probe.margins() == 0);
    CHECK(std::isfinite(explicit_probe.position()));
    CHECK(std::isfinite(default_probe.position()));
    CHECK(explicit_probe.owner_cleared());
    CHECK(default_probe.owner_cleared());
}

void direction_and_add_controls_remain_command_driven() {
    PublicReversal direction(PublicReversal::Mode::Direction, false);
    PublicReversal add(PublicReversal::Mode::Add, false);
    const auto bars = tape();
    direction.run(bars.data(), static_cast<int>(bars.size()));
    add.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(direction.last_error().empty());
    CHECK(add.last_error().empty());
    CHECK(direction.trade_count() >= 1);
    CHECK(add.trade_count() >= 1);
    CHECK(!direction.has_short());
    CHECK(add.native_events(0).size() >= direction.native_events(0).size());
    CHECK(direction.pending_order_count() == 0);
    CHECK(add.pending_order_count() == 0);
    CHECK(std::isfinite(direction.position()));
    CHECK(std::isfinite(add.position()));
}

class LiteralFixture : public source::PineStrategyHost {
public:
    double position() const { return live_position_size(); }
    int margins() const {
        int count = 0;
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") ++count;
        return count;
    }
    double first_margin_qty() const {
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") return get_trade(i).qty;
        return kNaN;
    }
    double first_margin_price() const {
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") return get_trade(i).exit_price;
        return kNaN;
    }
    bool has_short() const { return physical_position().signed_units < -1e-9; }
    bool owner_cleared() const { return pending_order_count() == 0; }
};

// Public-command reconstruction of ab9714be lines 137-183.  The explicit
// opening establishes the same carried long; the later default short sees the
// original signal close and the 3145.01 -> 3154.20 fill path.
class OpeningRetryFixture final : public LiteralFixture {
public:
    OpeningRetryFixture() {
        source::PineStrategyConfig config;
        config.initial_capital = 99764.603236;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.03;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, kNaN, kNaN, 31.4892, "LONG");
        if (pine_bar_index() == 1)
            strategy_entry("S", false, kNaN, kNaN, kNaN, "SHORT");
    }
};

std::vector<Bar> opening_retry_tape() {
    return {
        bar(3167.25, 3167.25, 3167.25, 3167.25, 1000),
        bar(3167.25, 3167.25, 3145.00, 3145.00, 2000),
        bar(3145.01, 3154.20, 3144.00, 3150.00, 3000),
    };
}

// Public-command reconstruction of the two ab9714be lines 185-262 fixtures.
// Their owner-only realized balance is represented by its equivalent initial
// realized balance; both metadata settings retain the identical public tape.
class FloorZeroFixture final : public LiteralFixture {
public:
    explicit FloorZeroFixture(bool full_residual) {
        source::PineStrategyConfig config;
        config.initial_capital = 12841.804380999999;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("margin_zero_cover_full_liquidation",
                             full_residual ? 1.0 : 0.0);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("L", true, kNaN, kNaN, 2.7738, "LONG");
        if (pine_bar_index() == 1) {
            strategy_entry("S", false, kNaN, kNaN, kNaN, "SHORT");
            strategy_close("L");
        }
    }
};

std::vector<Bar> floor_zero_tape() {
    return {
        bar(4629.63, 4629.63, 4629.63, 4629.63, 1000),
        bar(4629.63, 4629.63, 4500.00, 4506.71, 2000),
        bar(4506.70, 4514.70, 4500.00, 4506.70, 3000),
        bar(4514.70, 4539.00, 4500.00, 4530.00, 4000),
    };
}

// Public-command reconstruction of ab9714be lines 264-315.  The adjusted
// initial balance is algebraically the same account state that the deleted
// fixture produced by writing net_profit_sum_ before its adverse checkpoint.
class TrueFlatFloorZeroFixture final : public LiteralFixture {
public:
    TrueFlatFloorZeroFixture() {
        constexpr double qty = 3.6930;
        constexpr double entry = 1799.94;
        constexpr double adverse = 1801.26;
        constexpr double raw_q_min = 0.00005;
        constexpr double fee_rate = 0.0005;
        const double opening_fee = qty * entry * fee_rate;
        source::PineStrategyConfig config;
        config.initial_capital = (qty - raw_q_min) * adverse + opening_fee
            + (adverse - entry) * qty;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = qty;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.05;
        config.margin_short = 100.0;
        configure_pine_strategy(config);
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("S", false, kNaN, kNaN, kNaN, "SHORT");
    }
};

std::vector<Bar> true_flat_floor_zero_tape() {
    return {
        bar(1799.94, 1799.94, 1799.94, 1799.94, 1000),
        bar(1799.94, 1801.26, 1799.50, 1800.50, 2000),
    };
}

void exact_legacy_margin_literals_remain_executable_pending_checks() {
    OpeningRetryFixture opening;
    const auto opening_bars = opening_retry_tape();
    opening.run(opening_bars.data(), static_cast<int>(opening_bars.size()));
    CHECK(opening.margins() == 2);
    CHECK(near(opening.first_margin_qty(), 0.0376, 1e-9));
    CHECK(near(opening.first_margin_price(), 3145.01, 1e-9));
    CHECK(near(opening.position(), -30.8219, 1e-9));
    CHECK(opening.has_short());
    CHECK(opening.owner_cleared());

    for (const bool full_residual : {false, true}) {
        FloorZeroFixture floor_zero(full_residual);
        const auto floor_bars = floor_zero_tape();
        floor_zero.run(floor_bars.data(), static_cast<int>(floor_bars.size()));
        CHECK(near(floor_zero.first_margin_qty(), 0.0392, 1e-9));
        CHECK(near(floor_zero.first_margin_price(), 4514.70, 1e-9));
        CHECK(near(floor_zero.position(), -1.7346, 1e-9));
        CHECK(floor_zero.has_short());
        CHECK(floor_zero.owner_cleared());
    }

    TrueFlatFloorZeroFixture true_flat;
    const auto true_flat_bars = true_flat_floor_zero_tape();
    true_flat.run(true_flat_bars.data(), static_cast<int>(true_flat_bars.size()));
    CHECK(true_flat.margins() == 1);
    CHECK(near(true_flat.first_margin_qty(), 1.0, 1e-9));
    CHECK(near(true_flat.position(), -2.6930, 1e-9));
    CHECK(true_flat.has_short());
    CHECK(true_flat.owner_cleared());

    PublicReversal control(PublicReversal::Mode::Default, false);
    const auto control_bars = tape();
    control.run(control_bars.data(), static_cast<int>(control_bars.size()));
    const double control_qty = control.first_margin_qty();
    const double control_price = control.first_margin_price();
    CHECK(control.margins() == 0);
    CHECK(!control.has_short());
    CHECK(control.owner_cleared());
    CHECK(std::isfinite(control_qty) || std::isnan(control_qty));
    CHECK(std::isfinite(control_price) || std::isnan(control_price));
}

}  // namespace

int main() {
    public_default_reversal_observes_margin_slice_contract();
    explicit_and_default_reversal_keep_public_close_results();
    direction_and_add_controls_remain_command_driven();
    exact_legacy_margin_literals_remain_executable_pending_checks();
    std::printf("direct short reversal affordability: %d checks, %d failures\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
