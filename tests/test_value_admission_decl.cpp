// EN-132 D3: declaration admission through the real source host.
// Written before the producer; expectations pinned from D3 and .lane-out/MAP.md.
// No new admission header is needed to compile this TU against the base tree.
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>

namespace {
using namespace pineforge;
using namespace pineforge::source;

int failures = 0;
int checks = 0;
const char* row = "setup";

#define CHECK(expression) do {                                                 \
    ++checks;                                                                  \
    if (!(expression)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d [%s] %s\n",                            \
                     __FILE__, __LINE__, row, #expression);                    \
        ++failures;                                                            \
    }                                                                          \
} while (false)

struct DeclarationHost final : PineStrategyHost {
    void on_source_bar(const Bar&) override {}
    const PineStrategyConfig& observed() const { return fixture_configuration(); }
};

struct DeclarationField {
    double PineStrategyConfig::* member;
    const char* field;
    const char* pos_inf_row;
    const char* neg_inf_row;
    const char* nan_row;
    const char* finite_row;
};

constexpr DeclarationField fields[] = {
    {&PineStrategyConfig::default_qty_value, "strategy.default_qty_value",
     "decl_default_qty_value_pos_inf", "decl_default_qty_value_neg_inf",
     "decl_default_qty_value_nan_preserved", "decl_default_qty_value_finite"},
    {&PineStrategyConfig::margin_long, "strategy.margin_long",
     "decl_margin_long_pos_inf", "decl_margin_long_neg_inf",
     "decl_margin_long_nan_preserved", "decl_margin_long_finite"},
    {&PineStrategyConfig::margin_short, "strategy.margin_short",
     "decl_margin_short_pos_inf", "decl_margin_short_neg_inf",
     "decl_margin_short_nan_preserved", "decl_margin_short_finite"},
};

bool same_number(double left, double right) {
    if (std::isnan(left) || std::isnan(right))
        return std::isnan(left) && std::isnan(right);
    return left == right && (left != 0.0 || std::signbit(left) == std::signbit(right));
}

void check_config(const PineStrategyConfig& actual, const PineStrategyConfig& expected) {
    CHECK(same_number(actual.default_qty_value, expected.default_qty_value));
    CHECK(same_number(actual.margin_long, expected.margin_long));
    CHECK(same_number(actual.margin_short, expected.margin_short));
    CHECK(same_number(actual.initial_capital, expected.initial_capital));
    CHECK(same_number(actual.commission_value, expected.commission_value));
    CHECK(actual.default_qty_type == expected.default_qty_type);
    CHECK(actual.commission_type == expected.commission_type);
    CHECK(actual.pyramiding == expected.pyramiding);
    CHECK(actual.slippage == expected.slippage);
    CHECK(actual.process_orders_on_close == expected.process_orders_on_close);
    CHECK(actual.calc_on_order_fills == expected.calc_on_order_fills);
    CHECK(actual.close_entries_rule_any == expected.close_entries_rule_any);
    CHECK(actual.src_series_active == expected.src_series_active);
}

void check_failure(const std::exception& error, RunFailureCode code,
                   const std::string& expected_args) {
    // A plain invalid_argument/logic_error cannot satisfy the typed producer row.
    CHECK(dynamic_cast<const RunFailureInfo*>(&error) != nullptr);
    const auto value = classify_run_failure(error);
    CHECK(value.code == code);
    CHECK(value.args != nullptr);
    if (value.args) CHECK(*value.args == expected_args);
}

bool configure_ok(DeclarationHost& host, const PineStrategyConfig& config) {
    try {
        host.configure_pine_strategy(config);
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected declaration rejection [%s]: %s\n", row, error.what());
        CHECK(false);
    }
    return false;
}

NativeRunSpec project(const DeclarationHost& host) {
    static const Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1.0, 1736121600000LL},
        {100.0, 101.0, 99.0, 100.0, 1.0, 1736121660000LL},
    };
    NativeBeginArgs args{};
    args.bars = bars;
    args.n = 2;
    args.input_tf = "1";
    args.script_tf = "1";
    PineExecutionAdapter adapter;
    return adapter.project(host.observed(), StagedConfiguration{}, args);
}

void test_infinity_fields() {
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto& field : fields) {
        for (const double value : {inf, -inf}) {
            row = value > 0 ? field.pos_inf_row : field.neg_inf_row;
            DeclarationHost host;
            PineStrategyConfig before{};
            before.default_qty_value = 7.0;
            before.margin_long = 25.0;
            before.margin_short = 50.0;
            if (!configure_ok(host, before)) continue;
            auto invalid = before;
            invalid.*field.member = value;
            bool rejected = false;
            try {
                host.configure_pine_strategy(invalid);
            } catch (const std::exception& error) {
                rejected = true;
                check_failure(error, RunFailureCode::strategy_value_invalid,
                              std::string("{\"field\":\"") + field.field
                                  + "\",\"reason\":\"nonfinite\"}");
            }
            CHECK(rejected);
            check_config(host.observed(), before);
        }
    }
}

void test_declaration_precedes_override() {
    const double inf = std::numeric_limits<double>::infinity();
    for (const double value : {inf, -inf}) {
        row = value > 0 ? "decl_pos_inf_before_finite_override"
                        : "decl_neg_inf_before_finite_override";
        DeclarationHost host;
        const PineStrategyConfig before{};
        if (!configure_ok(host, before)) continue;
        auto invalid = before;
        invalid.default_qty_value = value;
        StrategyOverrides supplied{};
        supplied.default_qty_value = 3.0;
        bool reached_override = false;
        bool rejected = false;
        try {
            host.configure_pine_strategy(invalid);
            reached_override = true;
            host.set_strategy_override(supplied);
        } catch (const std::exception& error) {
            rejected = true;
            check_failure(error, RunFailureCode::strategy_value_invalid,
                          "{\"field\":\"strategy.default_qty_value\",\"reason\":\"nonfinite\"}");
        }
        CHECK(rejected);
        CHECK(!reached_override);
        check_config(host.observed(), before);
    }
}

void test_nan_declarations_stay() {
    // MAP: source na/computed declarations do not produce NaN at this door.
    // D3-a remains pending and kDeclarationNanIsInvalid is pinned false.
    for (const auto& field : fields) {
        row = field.nan_row;
        DeclarationHost host;
        PineStrategyConfig config{};
        config.*field.member = std::numeric_limits<double>::quiet_NaN();
        if (configure_ok(host, config)) check_config(host.observed(), config);
    }
}

void test_finite_values_stay() {
    const double values[] = {
        1.0, -1.0, 0.0, -0.0,
        std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
    };
    for (const auto& field : fields) {
        row = field.finite_row;
        for (const double value : values) {
            DeclarationHost host;
            PineStrategyConfig config{};
            config.*field.member = value;
            if (configure_ok(host, config)) check_config(host.observed(), config);
        }
    }
}

void test_nonpositive_margin_suppression_stays() {
    const double values[] = {0.0, -0.0, -1.0, std::numeric_limits<double>::lowest()};
    for (const auto member : {&PineStrategyConfig::margin_long, &PineStrategyConfig::margin_short}) {
        row = member == &PineStrategyConfig::margin_long
            ? "decl_nonpositive_margin_long_no_model" : "decl_nonpositive_margin_short_no_model";
        for (const double value : values) {
            DeclarationHost host;
            PineStrategyConfig config{};
            config.*member = value;
            if (!configure_ok(host, config)) continue;
            check_config(host.observed(), config);
            CHECK(!project(host).margin.has_value());
        }
    }
    row = "decl_positive_margins_model_present";
    DeclarationHost host;
    PineStrategyConfig config{};
    config.margin_long = 25.0;
    config.margin_short = 50.0;
    if (!configure_ok(host, config)) return;
    const auto spec = project(host);
    CHECK(spec.margin.has_value());
    if (spec.margin) {
        CHECK(spec.margin->maintenance_long == 0.25);
        CHECK(spec.margin->maintenance_short == 0.5);
    }
}

void test_override_sentinels_and_caller_infinity_stay() {
    row = "override_nan_leaves_all_double_members";
    DeclarationHost host;
    PineStrategyConfig config{};
    config.initial_capital = 123456.0;
    config.commission_value = 0.25;
    config.default_qty_value = 7.0;
    config.margin_long = 25.0;
    config.margin_short = 50.0;
    if (!configure_ok(host, config)) return;
    StrategyOverrides leave{};
    CHECK(std::isnan(leave.initial_capital));
    CHECK(std::isnan(leave.commission_value));
    CHECK(std::isnan(leave.default_qty_value));
    leave.pyramiding = 4;  // Witness that the merge executed despite absent doubles.
    host.set_strategy_override(leave);
    config.pyramiding = 4;
    check_config(host.observed(), config);

    const double inf = std::numeric_limits<double>::infinity();
    for (const double value : {inf, -inf}) {
        row = value > 0 ? "override_default_qty_pos_inf_deferred_D3b"
                        : "override_default_qty_neg_inf_deferred_D3b";
        DeclarationHost caller;
        PineStrategyConfig original{};
        if (!configure_ok(caller, original)) continue;
        StrategyOverrides supplied{};
        supplied.default_qty_value = value;
        caller.set_strategy_override(supplied);
        original.default_qty_value = value;
        check_config(caller.observed(), original);
        caller.set_strategy_override(StrategyOverrides{});
        check_config(caller.observed(), original);  // NaN leaves even a caller infinity.
    }
}

void test_existing_financial_refusals_stay() {
    const double inf = std::numeric_limits<double>::infinity();
    const double values[] = {inf, -inf, std::numeric_limits<double>::quiet_NaN(), -1.0};
    for (const auto member : {&PineStrategyConfig::initial_capital, &PineStrategyConfig::commission_value}) {
        row = member == &PineStrategyConfig::initial_capital
            ? "initial_capital_existing_typed_refusal" : "commission_value_existing_typed_refusal";
        const std::string expected_args = member == &PineStrategyConfig::initial_capital
            ? "{\"field\":\"initial_capital\"}" : "{\"field\":\"fee_value\"}";
        for (const double value : values) {
            DeclarationHost host;
            PineStrategyConfig config{};
            config.*member = value;
            if (!configure_ok(host, config)) continue;
            bool rejected = false;
            try {
                (void)project(host);
            } catch (const std::exception& error) {
                rejected = true;
                check_failure(error, RunFailureCode::strategy_settings_rejected, expected_args);
            }
            CHECK(rejected);
        }
    }
}

}  // namespace

int main() {
    try {
        test_infinity_fields();
        test_declaration_precedes_override();
        test_nan_declarations_stay();
        test_finite_values_stay();
        test_nonpositive_margin_suppression_stays();
        test_override_sentinels_and_caller_infinity_stay();
        test_existing_financial_refusals_stay();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected exception [%s]: %s\n", row, error.what());
        ++failures;
    }
    std::printf("declaration value admission: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
