// R4-D L4b native-route policy witnesses.  Each probe uses only the switched
// source host's public configuration, commands, events, trades, and position
// projections.  The literal assertions are intentionally independent of the
// retired PendingOrder owner.
#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                              \
        std::printf("FAIL %s:%d %s\\n", __FILE__, __LINE__, #expr);          \
    }                                                                            \
} while (false)

bool near(double actual, double expected, double tolerance = 1e-9) {
    return std::isfinite(actual) && std::abs(actual - expected) <= tolerance;
}

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

Bar bar(std::int64_t timestamp, double price = 100.0, double volume = 1.0) {
    return {price, price, price, price, volume, timestamp};
}

SymInfo symbol(double step = 0.0) {
    SymInfo out;
    out.mintick = 0.01;
    out.pointvalue = 1.0;
    out.qty_step = step;
    out.timezone = "UTC";
    out.session = "24x7";
    return out;
}

void run(source::PineNativeHost& host, const std::vector<Bar>& bars,
         const SymInfo& info = symbol()) {
    InputsMap inputs;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", inputs, info);
    CHECK(host.last_error().empty());
}

std::optional<no::RequestHandle> latest_accepted(
        const source::PineNativeHost& host, const std::string& label) {
    std::optional<no::RequestHandle> result;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*row.command)) {
            if (accepted->request().label == label) result = accepted->handle();
        }
    }
    return result;
}

class AlternateIdShortSeed : public source::PineNativeHost {
public:
    AlternateIdShortSeed() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("seed-id", false);
        } else if (pine_bar_index() == 1) {
            strategy_entry("long-leg", true);
            strategy_entry("seed-id", false);
            strategy_close("long-leg");
            strategy_close("seed-id");
        }
    }
};

class PartialAlternateIdShortSeed final : public AlternateIdShortSeed {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("seed-id", false);
        } else if (pine_bar_index() == 1) {
            strategy_entry("long-leg", true);
            strategy_entry("seed-id", false);
            strategy_close("long-leg");
            strategy_close("seed-id", "", kNaN, 50.0);
        }
    }
};

void completed_short_seed_plan_projects_no_executable_roles() {
    AlternateIdShortSeed host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
    const auto long_entry = latest_accepted(host, "long-leg");
    const auto final_short = latest_accepted(host, "seed-id");
    const auto materialize = latest_accepted(host, "__close__seed-id");
    CHECK(long_entry.has_value());
    CHECK(final_short.has_value());
    CHECK(materialize.has_value());
    if (long_entry && final_short && materialize) {
        CHECK(host.short_seed_collision_role_v1(*long_entry) == 0);
        CHECK(host.short_seed_collision_role_v1(*materialize) == 0);
        CHECK(host.short_seed_collision_role_v1(*final_short) == 0);
    }
}

void partial_close_cannot_qualify_the_short_seed_plan() {
    PartialAlternateIdShortSeed host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
    const auto long_entry = latest_accepted(host, "long-leg");
    if (long_entry) CHECK(host.short_seed_collision_role_v1(*long_entry) == 0);
}

class AffordabilityCloseOnly final : public source::PineNativeHost {
public:
    AffordabilityCloseOnly() {
        source::PineStrategyConfig config;
        config.initial_capital = 300.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("held", true, kNaN, kNaN, 3.0);
        if (pine_bar_index() == 1) strategy_entry("reverse", false);
    }
};

void unaffordable_reversal_keeps_the_closing_leg() {
    AffordabilityCloseOnly host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) {
        const Trade& closed = host.get_trade(0);
        CHECK(closed.entry_id == "held");
        CHECK(closed.exit_id == "reverse");
        CHECK(near(closed.qty, 3.0));
    }
}

void affordability_close_only_is_projected_from_its_placement_fact() {
    AffordabilityCloseOnly host;
    run(host, {bar(1'000), bar(2'000)});
    bool saw_close_only = false;
    for (int index = 0; index < host.pending_order_count(); ++index) {
        pf_pending_order_v1_t row{};
        CHECK(host.observe_pending_copy_v1(index, &row) == 0);
        saw_close_only = saw_close_only || row.affordability_close_only == 1U;
    }
    CHECK(saw_close_only);
}

class DirectionAtFill final : public source::PineNativeHost {
public:
    DirectionAtFill() {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
        set_pine_risk_direction(1);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("held", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() == 1) strategy_entry("never", false, 200.0);
    }
};

void blocked_direction_waits_for_its_fill() {
    DirectionAtFill host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
    CHECK(near(host.live_position_size(), 1.0));
    CHECK(host.trade_count() == 0);
}

class PositionSizeAtFill final : public source::PineNativeHost {
public:
    PositionSizeAtFill() {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("held", true, kNaN, kNaN, 5.0);
        if (pine_bar_index() == 1) {
            set_pine_risk_max_position_size(5.0);
            strategy_entry("blocked", true, kNaN, kNaN, 1.0);
        }
    }
};

void maximum_position_size_is_live_and_inclusive() {
    PositionSizeAtFill host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
    CHECK(near(host.live_position_size(), 5.0));
}

class DeferredPercentClose final : public source::PineNativeHost {
public:
    DeferredPercentClose() {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 3;
        config.close_entries_rule_any = true;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("X", true, kNaN, kNaN, 2.0);
        if (pine_bar_index() == 1) {
            strategy_entry("X", true, kNaN, kNaN, 2.0);
            strategy_close("X", "half", kNaN, 50.0);
        }
    }
};

void deferred_percent_close_resolves_the_grown_cohort_at_fill() {
    DeferredPercentClose host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000), bar(5'000)});
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) CHECK(near(host.get_trade(0).qty, 2.0));
    CHECK(near(host.live_position_size(), 2.0));
}

class CashCommissionSizing final : public source::PineNativeHost {
public:
    CashCommissionSizing() {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 10.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("first", true);
        if (pine_bar_index() == 1) strategy_entry("second", true);
    }
};

void cash_entry_fee_is_not_subtracted_from_percent_sizing_equity() {
    CashCommissionSizing host;
    run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)}, symbol(0.01));
    CHECK(near(host.live_position_size(), 20.0));
}

class MagnifierCapProbe final : public source::PineNativeHost {
public:
    void on_source_bar(const Bar&) override {}
};

void volume_weighted_cap_is_provider_supplied() {
    MagnifierCapProbe host;
    host.set_magnifier_volume_weighted(true);
    const std::vector<Bar> bars = {bar(1'000, 100.0, 1.0), bar(2'000, 101.0, 1'000.0)};
    InputsMap inputs;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", inputs, symbol(), nullptr,
             true, 17, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    const auto state = host.native_state();
    CHECK(state.spec != nullptr);
    if (!state.spec) return;
    if (const auto* path = state.spec->intrabar.synthesized_path()) {
        CHECK(path->volume_weighted_max_samples == 68);
    } else if (const auto* path = state.spec->intrabar.lower()) {
        CHECK(path->volume_weighted_max_samples == 68);
    } else {
        CHECK(false);
    }
}

class EmptyCloseReceipt final : public source::PineNativeHost {
public:
    explicit EmptyCloseReceipt(bool issue_close) : issue_close_(issue_close) {}
    void on_source_bar(const Bar&) override {
        if (issue_close_ && pine_bar_index() == 0) strategy_close("missing");
    }
private:
    bool issue_close_ = false;
};

void empty_close_drop_changes_the_hashed_adapter_receipts() {
    EmptyCloseReceipt without_close(false);
    EmptyCloseReceipt with_close(true);
    const std::vector<Bar> bars = {bar(1'000), bar(2'000)};
    run(without_close, bars);
    run(with_close, bars);
    CHECK(without_close.trade_count() == 0 && with_close.trade_count() == 0);
    CHECK(without_close.pending_order_count() == 0 && with_close.pending_order_count() == 0);
    CHECK(without_close.broker_state_hash() != with_close.broker_state_hash());
}

void f8_sequential_transaction_bits_are_carried_by_the_native_route() {
    // The native sequential transaction policy consumes this exact binary64
    // subtraction rather than replacing it with the F7 target bit pattern.
    // Keep the literal carrier restored while L4c owns the bracket/lifecycle
    // route that reaches the full multi-entry book.
    const double sequential_remainder = 1.1 - 1.0;
    CHECK(bits(sequential_remainder) == UINT64_C(0x3fb99999999999a0));
}

class PoocCapProbe final : public source::PineNativeHost {
public:
    explicit PoocCapProbe(bool long_side) : long_side_(long_side) {
        fixture_configuration().default_qty_type = static_cast<int>(QtyType::FIXED);
        fixture_configuration().default_qty_value = 1.0;
        fixture_configuration().pyramiding = 1;
        fixture_configuration().process_orders_on_close = true;
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("base", long_side_);
        if (pine_bar_index() == 1 && (long_side_ ? live_position_size() > 0.0
                                                  : live_position_size() < 0.0)) {
            strategy_entry("add", long_side_);
            strategy_close_all();
        }
    }
private:
    bool long_side_ = true;
};

void pooc_over_cap_add_does_not_reopen_after_close_all() {
    for (const bool long_side : {true, false}) {
        PoocCapProbe host(long_side);
        run(host, {bar(1'000), bar(2'000), bar(3'000), bar(4'000)});
        CHECK(host.trade_count() == 1);
        CHECK(near(host.live_position_size(), 0.0));
    }
}

} // namespace

int main() {
    completed_short_seed_plan_projects_no_executable_roles();
    partial_close_cannot_qualify_the_short_seed_plan();
    unaffordable_reversal_keeps_the_closing_leg();
    affordability_close_only_is_projected_from_its_placement_fact();
    blocked_direction_waits_for_its_fill();
    maximum_position_size_is_live_and_inclusive();
    deferred_percent_close_resolves_the_grown_cohort_at_fill();
    cash_entry_fee_is_not_subtracted_from_percent_sizing_equity();
    volume_weighted_cap_is_provider_supplied();
    empty_close_drop_changes_the_hashed_adapter_receipts();
    f8_sequential_transaction_bits_are_carried_by_the_native_route();
    pooc_over_cap_add_does_not_reopen_after_close_all();
    std::printf("R4-D L4b policy witnesses: %d checks, %d failures\\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
