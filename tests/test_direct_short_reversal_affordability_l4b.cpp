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
#define CHECK_NEAR(actual, expected, tolerance) do {                            \
    ++checks;                                                                   \
    if (!near((actual), (expected), (tolerance))) {                             \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s == %.12f, expected %.12f\\n",             \
                    __FILE__, __LINE__, #actual, (actual), (expected));          \
    }                                                                           \
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

class LiteralProbeBase : public source::PineStrategyHost {
public:
    double position() const { return live_position_size(); }
    bool has_short() const { return physical_position().signed_units < -1e-9; }
    bool owner_cleared() const { return pending_order_count() == 0; }
    std::vector<double> margin_quantities() const {
        std::vector<double> result;
        for (int index = 0; index < trade_count(); ++index) {
            if (get_trade(index).exit_comment == "Margin call")
                result.push_back(get_trade(index).qty);
        }
        return result;
    }
    std::vector<double> margin_prices() const {
        std::vector<double> result;
        for (int index = 0; index < trade_count(); ++index) {
            if (get_trade(index).exit_comment == "Margin call")
                result.push_back(get_trade(index).exit_price);
        }
        return result;
    }
};

class OpeningRetryPublic final : public LiteralProbeBase {
public:
    OpeningRetryPublic() {
        source::PineStrategyConfig config;
        config.initial_capital = 99764.603236;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.03;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        margin_call_enabled_ = true;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0001;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 31.4892);
        if (pine_bar_index() == 1 && live_position_size() > 0.0)
            strategy_entry("S", false);
    }
};

class FloorZeroPublic final : public LiteralProbeBase {
public:
    explicit FloorZeroPublic(bool full_residual) {
        source::PineStrategyConfig config;
        config.initial_capital = 12841.8043809999995;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        margin_call_enabled_ = true;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0001;
        set_syminfo_metadata("margin_zero_cover_full_liquidation",
                            full_residual ? 1.0 : 0.0);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 2.7738);
        if (pine_bar_index() == 1 && live_position_size() > 0.0) {
            strategy_entry("S", false);
            strategy_close("L");
        }
    }
};

class TrueFlatPublic final : public LiteralProbeBase {
public:
    TrueFlatPublic() {
        source::PineStrategyConfig config;
        config.initial_capital = 6660.16146621;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 3.6930;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.05;
        config.margin_short = 100.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        margin_call_enabled_ = true;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0001;
        set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry("S", false, kNaN, kNaN, 3.6930);
    }
};

void public_default_reversal_observes_margin_slice_contract() {
    PublicReversal probe(PublicReversal::Mode::Default);
    const auto bars = tape();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() >= 1);
    CHECK(probe.margins() <= probe.trade_count());
    CHECK(std::isfinite(probe.position()));
    CHECK(probe.owner_cleared());
    CHECK(probe.margins() == 0 || (std::isfinite(probe.first_margin_qty())
                                   && probe.first_margin_qty() > 0.0));
    CHECK(probe.margins() == 0 || (std::isfinite(probe.first_margin_price())
                                   && probe.first_margin_price() > 0.0));
    CHECK(std::abs(probe.position()) <= 10'000.0);
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

void exact_legacy_margin_literals_use_three_public_probes() {
    OpeningRetryPublic retry;
    const std::vector<Bar> retry_bars = {
        bar(3167.25, 3167.25, 3167.25, 3167.25, 1000),
        bar(3167.25, 3167.25, 3144.00, 3145.00, 2000),
        bar(3145.01, 3154.20, 3144.00, 3150.00, 3000),
        bar(3150.00, 3150.00, 3150.00, 3150.00, 4000),
    };
    retry.run(retry_bars.data(), static_cast<int>(retry_bars.size()));
    const auto retry_qty = retry.margin_quantities();
    const auto retry_price = retry.margin_prices();
    CHECK(retry.last_error().empty());
    CHECK(retry_qty.size() == 2U);
    CHECK(retry_price.size() == 2U);
    CHECK(retry_qty.size() == 2U && near(retry_qty[0], 0.0376, 1e-9));
    CHECK(retry_price.size() == 2U && near(retry_price[0], 3145.01, 1e-9));
    if (retry_qty.size() == 2U && retry_price.size() == 2U) {
        CHECK_NEAR(retry_qty[1], 0.6204, 1e-9);
        CHECK_NEAR(retry_price[1], 3154.20, 1e-9);
    }
    CHECK(near(retry.position(), -30.8219, 1e-9));
    CHECK(retry.has_short() && retry.owner_cleared());

    const std::vector<Bar> floor_bars = {
        bar(4629.63, 4629.63, 4629.63, 4629.63, 1000),
        bar(4629.63, 4629.63, 4506.71, 4506.71, 2000),
        bar(4506.70, 4514.70, 4500.00, 4506.70, 3000),
        bar(4514.70, 4539.00, 4500.00, 4530.00, 4000),
        bar(4530.00, 4530.00, 4530.00, 4530.00, 5000),
    };
    FloorZeroPublic one_contract(false);
    FloorZeroPublic full_residual(true);
    one_contract.run(floor_bars.data(), static_cast<int>(floor_bars.size()));
    full_residual.run(floor_bars.data(), static_cast<int>(floor_bars.size()));
    const auto floor_qty = one_contract.margin_quantities();
    const auto floor_price = one_contract.margin_prices();
    CHECK(one_contract.last_error().empty() && full_residual.last_error().empty());
    CHECK(floor_qty.size() == 2U && floor_price.size() == 2U);
    CHECK(floor_qty.size() == 2U && near(floor_qty[0], 0.0392, 1e-9));
    CHECK(floor_price.size() == 2U && near(floor_price[0], 4514.70, 1e-9));
    CHECK(floor_qty.size() == 2U && near(floor_qty[1], 1.0, 1e-9));
    if (floor_price.size() == 2U) CHECK_NEAR(floor_price[1], 4539.00, 1e-9);
    CHECK(near(one_contract.position(), -1.7346, 1e-9));
    CHECK(one_contract.has_short() && one_contract.owner_cleared());
    CHECK(near(full_residual.position(), -1.7346, 1e-9)
          && full_residual.has_short() && full_residual.owner_cleared());
    const auto full_residual_price = full_residual.margin_prices();
    CHECK(full_residual_price.size() == 2U);
    if (full_residual_price.size() == 2U)
        CHECK_NEAR(full_residual_price[1], 4539.00, 1e-9);

    TrueFlatPublic flat;
    const std::vector<Bar> flat_bars = {
        bar(1799.94, 1799.94, 1799.94, 1799.94, 1000),
        bar(1799.94, 1799.94, 1799.94, 1799.94, 2000),
        bar(1800.00, 1801.26, 1799.50, 1800.50, 3000),
        bar(1800.50, 1800.50, 1800.50, 1800.50, 4000),
    };
    flat.run(flat_bars.data(), static_cast<int>(flat_bars.size()));
    const auto flat_qty = flat.margin_quantities();
    CHECK(flat.last_error().empty());
    CHECK(flat_qty.size() == 1U);
    CHECK(flat_qty.size() == 1U && near(flat_qty[0], 1.0, 1e-9));
    CHECK(near(flat.position(), -2.6930, 1e-9));
    CHECK(flat.has_short() && flat.owner_cleared());
    CHECK(retry_qty.size() + floor_qty.size() + flat_qty.size() == 5U);
}

}  // namespace

int main() {
    public_default_reversal_observes_margin_slice_contract();
    explicit_and_default_reversal_keep_public_close_results();
    direction_and_add_controls_remain_command_driven();
    exact_legacy_margin_literals_use_three_public_probes();
    std::printf("direct short reversal affordability: %d checks, %d failures\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
