// Raw-argument overflow through real source command doors.
// Written before the checked header / adapter changes; expectations come
// from revision 3 section 4. The supervisor builds and runs these rows.
#include <pineforge/compat/pine/trail_ticks.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_checked_arith.hpp"
#include "../src/compat/pine/callback_lifecycle_rules.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <variant>

using namespace pineforge;

namespace {
int failures = 0;
const char* row = "";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kMax = std::numeric_limits<double>::max();
constexpr std::int64_t kTime = 1700000040000LL;

#define CHECK(expr) do { if (!(expr)) {                                      \
    std::fprintf(stderr, "FAIL [%s] %s:%d %s\n", row, __FILE__, __LINE__, #expr); \
    ++failures;                                                             \
} } while (0)

bool same_bits(double a, double b) {
    std::uint64_t left = 0, right = 0;
    std::memcpy(&left, &a, sizeof left);
    std::memcpy(&right, &b, sizeof right);
    return left == right;
}

Bar bar(int index, double price = 100.0) {
    return Bar{price, price, price, price, 1.0,
               kTime + static_cast<std::int64_t>(index) * 60000};
}

// Frozen pre-change arithmetic, including the original operation order.
// These calculate an oracle; none reads a product result or substitutes a
// fixture's expected numeric value for an input.
double old_level(double level, double tick) {
    if (!std::isfinite(level)) return level;
    int digits = -1;
    if (std::isfinite(tick) && tick > 0.0) {
        double scaled = tick;
        for (int d = 0; d <= 10; ++d) {
            const double k = std::floor(scaled + 0.5);
            if (k >= 1.0 && std::abs(scaled - k) <= 1e-6 * k) { digits = d; break; }
            scaled *= 10.0;
        }
    }
    if (digits < 0) return level;
    double scale = 1.0;
    for (int d = 0; d < digits; ++d) scale *= 10.0;
    const double point = 1.0 / scale;
    const double points = level / point;
    const double grid = std::floor(points + 0.5);
    return std::abs(points - grid) <= 0.01 / scale ? grid / scale : level;
}

double old_decimal_tick(double value, double tick) {
    const double k = std::floor(value / tick + 0.5);
    const double inverse = 1.0 / tick;
    const double n = std::floor(inverse + 0.5);
    if (n > 0.0 && std::abs(inverse - n) <= 1e-6 * n) return k / n;
    return k * tick;
}

double old_bar_tick(double value, double tick) {
    const double k = std::floor(value / tick + 0.5);
    if (k * tick == value) return value;
    return old_decimal_tick(value, tick);
}

double old_directional(double value, double tick, bool upward) {
    const double scaled = value / tick;
    return (upward ? std::ceil(scaled - 1e-9) : std::floor(scaled + 1e-9)) * tick;
}

double old_threshold(double level, double tick, bool buy, bool limit) {
    if (!std::isfinite(level) || !std::isfinite(tick) || tick <= 0.0) return level;
    if (level <= 0.0) return 0.0;
    const bool upward = limit ? !buy : buy;
    double scaled = level / tick;
    const double inverse = 1.0 / tick;
    const double n = std::floor(inverse + 0.5);
    if (n > 0.0 && std::abs(inverse - n) <= 1e-6 * n) {
        scaled = level * n;
        const double nearest = std::floor(scaled + 0.5);
        if (old_bar_tick(level, tick) == level) scaled = nearest;
    }
    const double target = upward ? std::ceil(scaled - 1e-12) : std::floor(scaled + 1e-12);
    const double grid = target * tick;
    double threshold = grid + (upward ? -0.5 : 0.5) * tick;
    const auto reaches = [&](double price) {
        const double rounded = std::floor(price / tick + 0.5);
        return upward ? rounded >= target : rounded <= target;
    };
    for (int i = 0; i < 16 && !reaches(threshold); ++i)
        threshold = std::nextafter(threshold, upward ? kInf : -kInf);
    for (int i = 0; i < 16; ++i) {
        const double candidate = std::nextafter(threshold, upward ? -kInf : kInf);
        if (!reaches(candidate)) break;
        threshold = candidate;
    }
    return threshold;
}

void expect_overflow(const RunFailureValue& failure, const char* field) {
    CHECK(failure.code == RunFailureCode::strategy_value_invalid);
    CHECK(run_failure_code_class(failure.code) == RunFailureClass::strategy);
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"numeric_overflow\"}";
    CHECK(failure.args && *failure.args == expected);
}

enum class Door { EntryLimit, EntryStop, OrderLimit, OrderStop };
bool is_limit(Door d) { return d == Door::EntryLimit || d == Door::OrderLimit; }
bool is_entry(Door d) { return d == Door::EntryLimit || d == Door::EntryStop; }
const char* field(Door d) {
    switch (d) {
        case Door::EntryLimit: return "entry.limit_price";
        case Door::EntryStop: return "entry.stop_price";
        case Door::OrderLimit: return "order.limit_price";
        case Door::OrderStop: return "order.stop_price";
    }
    return "";
}

class PriceHost final : public source::PineStrategyHost {
public:
    PriceHost(Door door, double level, double tick, bool percent_default = false,
              bool other_level = false)
        : door_(door), level_(level), percent_default_(percent_default), other_level_(other_level) {
        source::PineStrategyConfig config;
        config.initial_capital = 1e6;
        config.default_qty_type = static_cast<int>(percent_default
            ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        config.default_qty_value = 1.0;
        // Isolate the directional producer: this branch leaves the native
        // stop raw and never calls the later threshold helper.
        config.calc_on_order_fills = percent_default;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        syminfo_mintick_ = tick;
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        called = true;
        const double absent_or_other = other_level_ ? 100.0 : kNaN;
        const double limit = is_limit(door_) ? level_ : absent_or_other;
        const double stop = is_limit(door_) ? absent_or_other : level_;
        try {
            if (is_entry(door_))
                strategy_entry("raw", true, limit, stop, percent_default_ ? kNaN : 1.0);
            else
                strategy_order("raw", true, 1.0, limit, stop);
            pf_pending_order_v1_t pending{};
            if (pending_intent_view().copy_v1(0, &pending) == 0) {
                projected = true;
                projected_level = is_limit(door_) ? pending.limit_price : pending.stop_price;
            }
        } catch (const std::exception& error) {
            failure = classify_run_failure(error);
        }
    }

    bool called = false, projected = false;
    double projected_level = kNaN;
    RunFailureValue failure{};
private:
    Door door_;
    double level_;
    bool percent_default_, other_level_;
};

void run_price(PriceHost& host) {
    const Bar input = bar(0);
    host.run(&input, 1, "1", "1", false);
    CHECK(host.called);
    CHECK(host.last_error().empty());
}

void price_failure(const char* name, Door d, double level, double tick, bool percent = false,
                   bool other_level = false) {
    row = name;
    PriceHost host(d, level, tick, percent, other_level);
    run_price(host);
    expect_overflow(host.failure, field(d));
    CHECK(!host.projected);
}

void price_bits(const char* name, Door d, double level, double tick, bool percent = false) {
    row = name;
    PriceHost host(d, level, tick, percent);
    run_price(host);
    CHECK(host.failure.code == RunFailureCode::none);
    double expected = old_level(level, tick);
    if (percent) expected = old_directional(expected, tick, true);
    CHECK(host.projected);
    CHECK(same_bits(host.projected_level, expected));
    const double trigger = percent ? expected : old_threshold(expected, tick, true, is_limit(d));
    CHECK(std::isfinite(trigger));
    bool accepted = false;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* admission = std::get_if<native_order::AcceptedEvent>(&*event.command);
        if (!admission || admission->request().label != "raw") continue;
        accepted = true;
        if (is_limit(d)) {
            const auto* level_event = std::get_if<native_order::Limit>(&admission->request().trigger);
            CHECK(level_event && same_bits(level_event->price, trigger));
        } else {
            const auto* level_event = std::get_if<native_order::Stop>(&admission->request().trigger);
            CHECK(level_event && same_bits(level_event->price, trigger));
        }
    }
    CHECK(accepted);
}

void price_rows() {
    const Door doors[] = {Door::EntryLimit, Door::EntryStop, Door::OrderLimit, Door::OrderStop};
    const char* decimal[] = {"entry_limit_decimal_overflow", "entry_stop_decimal_overflow",
                            "order_limit_decimal_overflow", "order_stop_decimal_overflow"};
    const char* quotient[] = {"entry_limit_trigger_overflow", "entry_stop_trigger_overflow",
                             "order_limit_trigger_overflow", "order_stop_trigger_overflow"};
    const char* finite[] = {"entry_limit_near_boundary_bits", "entry_stop_near_boundary_bits",
                           "order_limit_near_boundary_bits", "order_stop_near_boundary_bits"};
    const char* small_tick[] = {"entry_limit_small_tick_bits", "entry_stop_small_tick_bits",
                               "order_limit_small_tick_bits", "order_stop_small_tick_bits"};
    for (int i = 0; i < 4; ++i) {
        // Both levels select StopLimit, bypassing threshold construction.
        // A removed decimal check cannot be masked by a later S producer.
        price_failure(decimal[i], doors[i], 1e308, 0.01, false, true);
        price_failure(quotient[i], doors[i], 1e308, 1e-11);
        price_bits(finite[i], doors[i], 1e306, 0.01);
        price_bits(small_tick[i], doors[i], 1e296, 1e-11);
    }
    price_failure("entry_stop_grid_product_overflow", Door::EntryStop, 1.5e308, 1e308);
    price_failure("order_stop_grid_product_overflow", Door::OrderStop, 1.5e308, 1e308);
    price_failure("entry_limit_boundary_add_overflow", Door::EntryLimit, 1.5e308, 1.2e308);
    price_failure("order_limit_boundary_add_overflow", Door::OrderLimit, 1.5e308, 1.2e308);
    price_bits("entry_stop_grid_product_bits", Door::EntryStop, 1.3e308, 7e307);
    price_bits("order_stop_grid_product_bits", Door::OrderStop, 1.3e308, 7e307);
    price_bits("entry_limit_boundary_add_bits", Door::EntryLimit, 1.3e308, 1e308);
    price_bits("order_limit_boundary_add_bits", Door::OrderLimit, 1.3e308, 1e308);
    price_failure("entry_stop_directional_overflow", Door::EntryStop, 1e308, 1e-11, true);
    price_bits("entry_stop_directional_bits", Door::EntryStop, 1e296, 1e-11, true);
    // The decimal division spelling differs from multiplication by 0.01.
    price_bits("entry_limit_decimal_spelling_bits", Door::EntryLimit, 2409.49, 0.01);
    price_bits("order_stop_decimal_spelling_bits", Door::OrderStop, 2409.49, 0.01);
    price_bits("entry_limit_half_tick_11_805_bits", Door::EntryLimit, 11.805, 0.01);
    price_bits("order_stop_half_tick_13_775_bits", Door::OrderStop, 13.775, 0.01);
    price_bits("entry_stop_zero_level_bits", Door::EntryStop, 0.0, 0.01);
    price_bits("order_limit_zero_level_bits", Door::OrderLimit, 0.0, 0.01);
}

class TrailHost final : public source::PineStrategyHost {
public:
    TrailHost(double points, double tick, double entry) : points_(points) {
        source::PineStrategyConfig config;
        config.initial_capital = 1e6;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        syminfo_mintick_ = tick;
        entry_ = entry;
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, kNaN, kNaN, 1.0);
        if (bar_index_ != 1) return;
        called = true;
        CHECK(physical_position().signed_units == 1.0);
        try { strategy_exit("trail", "E", kNaN, kNaN, points_, 1.0); }
        catch (const std::exception& error) { failure = classify_run_failure(error); }
    }
    void run_probe() {
        const Bar input[] = {bar(0, entry_), bar(1, entry_)};
        run(input, 2, "1", "1", false);
        CHECK(called);
        CHECK(last_error().empty());
    }
    bool called = false;
    RunFailureValue failure{};
private:
    double points_, entry_;
};

void trail_rows() {
    row = "exit_trail_tolerance_overflow";
    TrailHost overflow(-kMax, 1e308, 1e308);
    overflow.run_probe();
    expect_overflow(overflow.failure, "exit.trail_points");

    row = "exit_trail_near_boundary_bits";
    const double tolerance = compat::pine::kTrailPointsCeilMintickScale * 1e308;
    const double points = -0.99 * kMax;
    const auto checked = source::checked_add(points, -tolerance);
    CHECK(checked && same_bits(*checked, points - tolerance));
    CHECK(checked && same_bits(std::ceil(*checked), std::ceil(points - tolerance)));

    row = "exit_trail_large_finite_bits";
    TrailHost finite(kMax, 0.01, 100.0);
    finite.run_probe();
    CHECK(finite.failure.code == RunFailureCode::none);
    const double ticks = std::ceil(kMax - compat::pine::kTrailPointsCeilMintickScale * 0.01);
    const double activation = old_directional(100.0 + 1.0 * ticks * 0.01, 0.01, true);
    bool found = false;
    for (const auto& event : finite.native_events(0)) {
        if (!event.command) continue;
        const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command);
        if (!accepted || accepted->request().label != "trail") continue;
        const auto* trail = std::get_if<native_order::Trail>(&accepted->request().trigger);
        CHECK(trail && trail->best_seed && same_bits(*trail->best_seed, activation));
        found = true;
    }
    CHECK(found);

    row = "trail_constant_rule_fallback";
    const auto rule = source::detail::PineCallbackLifecycleRule::TrailPointsMintickTolerance;
    const bool previous = source::detail::pine_callback_lifecycle_rule(rule);
    source::detail::set_pine_callback_lifecycle_rule(rule, false);
    TrailHost fallback(-kMax, 1e308, 1e308);
    fallback.run_probe();
    source::detail::set_pine_callback_lifecycle_rule(rule, previous);
    // The constant fallback keeps ticks finite; G3 rejects the derived level.
    CHECK(fallback.failure.code == RunFailureCode::engine_command_value_invalid);
    CHECK(fallback.failure.args && *fallback.failure.args ==
        "{\"field\":\"relative_price.level\",\"reason\":\"numeric_overflow\","
        "\"stage\":\"relative_price\"}");

    row = "trail_invalid_mintick_fallback_bits";
    for (double tick : {0.0, -1.0, kNaN, kInf}) {
        const double p = 14.00001;
        CHECK(same_bits(compat::pine::trail_points_to_ticks(p, tick),
                        std::ceil(p - compat::pine::kTrailPointsCeilEps)));
    }
    CHECK(std::isnan(compat::pine::trail_points_to_ticks(kNaN, 0.01)));
    CHECK(same_bits(compat::pine::trail_offset_to_ticks(0.9), std::floor(0.9)));
}

// The constructor-time source projection is a defined public host behavior.
// It exercises the real explicit quantity floor without a huge quantity ever
// entering fill/margin arithmetic (which has different ownership).
class QuantityHost final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    void project(double qty, double grid) {
        source::StagedConfiguration staged;
        staged.quantity_grid = grid;
        adapter_.set_staged_configuration(staged);
        adapter_.entry("fallback", true, kNaN, kNaN, qty);
    }
};

// A cash-typed entry reaches the grid floor through typed_quantity_units,
// without explicit_entry_quantity_floor's noexcept boundary. One signal bar
// admits the order; its public fill-quantity probe observes the fallback in
// contracts without involving later fill/margin arithmetic.
class QuantityFallbackHost final : public source::PineStrategyHost {
public:
    QuantityFallbackHost(double cash, double grid) : cash_(cash) {
        source::PineStrategyConfig config;
        config.initial_capital = 1e6;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        qty_step_ = grid;
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        called = true;
        try {
            strategy_entry("fallback", true, kNaN, kNaN, cash_, "", "", 0,
                           static_cast<int>(QtyType::CASH));
        } catch (const std::exception& error) {
            failure = classify_run_failure(error);
            CHECK(false &&
                "the quantity grid arithmetic overflowed: expected the defined fallback (units unchanged), got a thrown value failure");
        }
    }
    bool called = false;
    RunFailureValue failure{};
private:
    double cash_;
};

void quantity_rows() {
    row = "quantity_infinite_intermediate_fallback";
    const double cash = 1e308;
    const double price = 2.0;
    const double quantity_grid = 0.001;
    // These four CHECKs document setup arithmetic and do not cover the product.
    // Point value and account FX are both 1; no fee is reserved for CASH.
    const double unquantized_units = cash / price;
    CHECK(std::isfinite(unquantized_units) && unquantized_units > 0.0);
    CHECK(std::isfinite(quantity_grid) && quantity_grid > 0.0);
    const double grid_quotient = unquantized_units / quantity_grid + 1e-6;
    CHECK(!std::isfinite(grid_quotient));
    CHECK(!std::isfinite(std::floor(grid_quotient) * quantity_grid));

    QuantityFallbackHost host(cash, quantity_grid);
    const Bar input = bar(0, price);
    host.run(&input, 1, "1", "1", false);
    CHECK(host.called);
    CHECK(host.failure.code == RunFailureCode::none);
    pf_pending_order_v1_t pending{};
    const bool projected = host.pending_intent_view().copy_v1(0, &pending) == 0;
    CHECK(projected);
    bool accepted = false;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* admission = std::get_if<native_order::AcceptedEvent>(&*event.command);
        if (admission && admission->request().label == "fallback") accepted = true;
    }
    CHECK(accepted);
    if (host.failure.code == RunFailureCode::none && projected && accepted) {
        // pending.qty retains the cash argument. The probe converts that
        // admitted order to contracts using the real quantity-grid helper.
        // Its underlying view is noexcept; skip it after a placement failure.
        // The guard only helps when the failure was raised at placement; a throw
        // first reached inside the noexcept probe would terminate the process.
        double contracts = kNaN;
        int close_only = -1, partition = -1;
        CHECK(host.probe_fill_qty(0, price, &contracts, &close_only, &partition) == 0);
        CHECK(same_bits(contracts, unquantized_units));
        CHECK(close_only == 0);
        CHECK(partition == 0);
    }
    CHECK(host.last_error().empty());
    CHECK(run_failure_value_of(host).code == RunFailureCode::none);

    row = "quantity_explicit_overflow_fallback";
    // On head the CASH host has no failure, so this row runs; after the mutant's
    // CHECK failure, skip the explicit noexcept door to avoid terminating the process.
    if (host.failure.code == RunFailureCode::none) {
        QuantityHost explicit_host;
        try { explicit_host.project(1e308, 0.001); }
        catch (const std::exception&) { CHECK(false); }
        pf_pending_order_v1_t explicit_pending{};
        CHECK(explicit_host.pending_intent_view().copy_v1(0, &explicit_pending) == 0);
        CHECK(same_bits(explicit_pending.qty, 1e308));
    }

    row = "quantity_decimal_tolerance_control";
    // A real decimal refinement: +epsilon lifts floor(q), while the exact
    // shortest-decimal floor stays below the requested quantity.
    const double units = 3124.9999995;
    const double grid = 1.0;
    QuantityHost decimal;
    try { decimal.project(units, grid); }
    catch (const std::exception&) { CHECK(false); }
    pf_pending_order_v1_t second{};
    CHECK(decimal.pending_intent_view().copy_v1(0, &second) == 0);
    CHECK(same_bits(second.qty, std::floor(units)));
}

void checked_helper_rows() {
    row = "checked_finite_overflow";
    CHECK(!source::checked_mul(kMax, 2.0));
    CHECK(!source::checked_div(kMax, 0.5));
    CHECK(!source::checked_add(kMax, kMax));
    CHECK(!source::checked_add(-kMax, -kMax));

    row = "checked_near_boundary_bits";
    const double below = std::nextafter(kMax / 2.0, 0.0);
    const auto mul = source::checked_mul(below, 2.0);
    const auto div = source::checked_div(below, 0.5);
    const auto add = source::checked_add(below, below);
    CHECK(mul && same_bits(*mul, below * 2.0));
    CHECK(div && same_bits(*div, below / 0.5));
    CHECK(add && same_bits(*add, below + below));
    const auto zero = source::checked_mul(-0.0, 2.0);
    CHECK(zero && same_bits(*zero, -0.0 * 2.0));

    row = "nonfinite_input_is_not_overflow";
    const auto inf = source::checked_mul(kInf, 2.0);
    const auto nan = source::checked_div(kNaN, 0.5);
    const auto opposite = source::checked_add(kInf, -kInf);
    CHECK(inf && *inf == kInf);
    CHECK(nan && std::isnan(*nan));
    CHECK(opposite && std::isnan(*opposite));

    row = "na_price_absence";
    PriceHost absence(Door::EntryLimit, kNaN, 0.01);
    run_price(absence);
    CHECK(absence.failure.code == RunFailureCode::none);
}
} // namespace

int main() {
    price_rows();
    trail_rows();
    quantity_rows();
    checked_helper_rows();
    return failures == 0 ? 0 : 1;
}
