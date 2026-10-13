// EN-132 D6 rows 15/16/18. Written first; each G row pins code/args from
// MAP.md, never from an executed output. Uses the existing module C ABI
// wrapper over scripted PineStrategyHost callbacks, not a copied run loop.
#include <pineforge/native_module.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/compat/pine/callback_lifecycle_rules.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;
const char* row = "";
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", row, line, expression);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
std::string text(const char* value) { return value ? value : "<null>"; }

enum class Shape {
    Profit, Loss, Trail, PendingProfit, PendingLoss, Absolute,
    Placeholder, NoExit, FilledCounter, Drawdown, Intraday, Preclose, InfiniteConfigured
};

class SourceProbe : public source::PineStrategyHost {
public:
    Shape shape = Shape::Profit;
    double operand = 0x1p1023;
    double capital = 0x1p1023;
    bool short_side = false;
    int calls = 0;
    void setup(Shape chosen, double ticks, double initial, bool is_short = false,
               bool calc_on_fills = false) {
        shape = chosen;
        operand = ticks;
        capital = initial;
        short_side = is_short;
        source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 1;
        config.calc_on_order_fills = calc_on_fills;
        configure_pine_strategy(config);
        syminfo_mintick_ = 1.0;
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        const int index = calls++;
        if (shape == Shape::Drawdown) {
            if (index == 0) set_pine_risk_max_drawdown(operand, true);
            return;
        }
        if (shape == Shape::InfiniteConfigured) {
            // Test-only injection bypasses the S setter: configured infinity
            // must not be counted as engine-derived at the threshold use.
            if (index == 0) fixture_value_invalid_drawdown_limit(operand, true);
            return;
        }
        if (index == 0) {
            if (shape == Shape::FilledCounter)
                set_pine_risk_max_intraday_filled_orders(std::numeric_limits<int>::max());
            strategy_entry("E", !short_side, na, na, 1.0);
            if (shape == Shape::PendingProfit || shape == Shape::PendingLoss) {
                // Explicit qty deliberately makes this definition unanchorable
                // (anchorable_relative_exit); exercise fill-point arithmetic,
                // not the row17 refusal that already has a typed code.
                strategy_exit("X", "E", na, na, na, na, na, 100.0, "", 1.0, "",
                              shape == Shape::PendingProfit ? operand : na,
                              shape == Shape::PendingLoss ? operand : na);
            } else if (shape == Shape::Placeholder) {
                strategy_exit("X", "E", na, na, na, na, na, 100.0, "", na, "",
                              operand, na);
            }
        }
        if (index == 2) {
            if (shape == Shape::Profit || shape == Shape::Loss) {
                strategy_exit("X", "E", na, na, na, na, na, 100.0, "", na, "",
                              shape == Shape::Profit ? operand : na,
                              shape == Shape::Loss ? operand : na);
            } else if (shape == Shape::Trail) {
                strategy_exit("X", "E", na, na, operand, 0.0);
            } else if (shape == Shape::Absolute) {
                strategy_exit("X", "E", operand, na);
            } else if (shape == Shape::Intraday) {
                set_pine_risk_max_intraday_loss(operand, true);
            } else if (shape == Shape::Preclose) {
                set_pine_risk_max_intraday_loss(operand, true);
                strategy_close("E", "", na, na, true);
                // Settlement queues the applied notification until this
                // callback returns. Keep the limit active through that drain;
                // this is the final input bar, so no later marked-price check
                // can rescue a removed preclose producer.
            }
        }
        if (index == 4 && (shape == Shape::Placeholder || shape == Shape::NoExit
                          || shape == Shape::FilledCounter))
            strategy_close("E", "", na, na, true);
    }
};
} // namespace

PINEFORGE_EXPORT_NATIVE_STRATEGY(SourceProbe);

namespace {
struct Run {
    pf_strategy_t handle = nullptr;
    SourceProbe* host = nullptr;
    pf_report_t report{};
    std::string code;
    std::string args;
    int status = -1;
    Run(Shape shape, double operand, double capital, double price, bool short_side = false,
        bool calc_on_fills = false) {
        handle = strategy_create(nullptr);
        CHECK(handle != nullptr);
        if (!handle) return;
        host = dynamic_cast<SourceProbe*>(static_cast<BacktestEngine*>(handle));
        CHECK(host != nullptr);
        if (!host) return;
        host->setup(shape, operand, capital, short_side, calc_on_fills);
        std::vector<pf_bar_t> bars;
        const int bar_count = shape == Shape::Preclose ? 3 : 40;
        for (int i = 0; i < bar_count; ++i) {
            const double p = (shape == Shape::Intraday || shape == Shape::Preclose) && i >= 2
                ? price - 1.0 : price;
            bars.push_back({p, p, p, p, 1.0, 1700000040000LL
                + static_cast<std::int64_t>(i) * 60000});
        }
        run_backtest_full(handle, bars.data(), static_cast<int>(bars.size()), "1", "1", 0, 4,
                          PF_MAGNIFIER_ENDPOINTS, &report);
        code = text(strategy_get_last_error_code(handle));
        args = text(strategy_get_last_error_args(handle));
        status = strategy_last_run_status(handle);
    }
    ~Run() { report_free(&report); strategy_free(handle); }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
};

void expect_g(const Run& run, const char* field, const char* stage) {
    CHECK(run.code == "engine_command_value_invalid");
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"numeric_overflow\",\"stage\":\"" + stage + "\"}";
    if (run.code != "engine_command_value_invalid" || run.args != expected) {
        std::fprintf(stderr, "[%s] got code=%s args=%s; expected engine_command_value_invalid %s\n",
                     row, run.code.c_str(), run.args.c_str(), expected.c_str());
    }
    CHECK(run.args == expected);
    CHECK(run.status == 1);
}

void row15_finite_relative_sum_overflow() {
    for (const Shape shape : {Shape::Profit, Shape::Loss, Shape::Trail,
                              Shape::PendingProfit, Shape::PendingLoss}) {
        row = shape == Shape::Profit ? "row15_known_profit_sum"
            : shape == Shape::Loss ? "row15_known_loss_sum"
            : shape == Shape::Trail ? "row15_known_trail_sum"
            : shape == Shape::PendingProfit ? "row15_fill_profit_sum" : "row15_fill_loss_sum";
        Run run(shape, 0x1p1023, 0x1p1023, 0x1p1023,
                shape == Shape::Loss || shape == Shape::PendingLoss);
        expect_g(run, "relative_price.level", "relative_price");
        CHECK(run.report.total_trades == 0);
    }
}

void row16_placeholder_withdrawal_keep(bool raw_projection = false) {
    row = raw_projection ? "row16_invalid_installed_level_placeholder_keep"
                         : "row16_placeholder_withdrawal_keep";
    Run placeholder(Shape::Placeholder, 200.0, 10000.0, 100.0, true, raw_projection);
    Run control(Shape::NoExit, 0.0, 10000.0, 100.0, true, raw_projection);
    CHECK(placeholder.code.empty() && placeholder.args.empty() && placeholder.status == 0);
    CHECK(control.code.empty() && control.status == 0);
    CHECK(placeholder.report.trades_len == 1 && control.report.trades_len == 1);
    if (placeholder.report.trades_len == 1 && control.report.trades_len == 1) {
        const auto& a = placeholder.report.trades[0];
        const auto& b = control.report.trades[0];
        CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
        CHECK(same_bits(a.entry_price, b.entry_price));
        CHECK(same_bits(a.exit_price, b.exit_price));
        CHECK(same_bits(a.pnl, b.pnl));
        CHECK(same_bits(a.pnl_pct, b.pnl_pct));
        CHECK(same_bits(a.max_runup, b.max_runup));
        CHECK(same_bits(a.max_drawdown, b.max_drawdown));
        CHECK(same_bits(a.qty, b.qty) && same_bits(a.commission, b.commission));
        CHECK(a.is_long == b.is_long && a.open_at_end == b.open_at_end);
        CHECK(a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index);
    }
    int armed = 0;
    int withdrawn = 0;
    native_order::RequestHandle child{};
    std::uint64_t armed_ordinal = 0;
    if (placeholder.host) {
        for (const auto& event : placeholder.host->native_events(0)) {
            if (!event.command) continue;
            if (const auto* arm = std::get_if<native_order::ArmedEvent>(&*event.command)) {
                if (!arm->definition || arm->definition->request.label != "X") continue;
                const auto* limit = std::get_if<native_order::Limit>(
                    &arm->definition->request.trigger);
                // The ordinary negative profit level is clamped to zero by
                // source_trigger_threshold. The raw on-grid COOF fixture
                // instead retains -100, so resolve_anchored_level returns the
                // documented tick=1 placeholder and marks it unadoptable.
                CHECK(limit && same_bits(limit->price, raw_projection ? 1.0 : 0.0));
                CHECK(std::holds_alternative<native_order::Absolute>(
                    arm->definition->request.anchor));
                child = arm->definition->handle;
                armed_ordinal = arm->ordinal;
                ++armed;
            }
            if (const auto* cancel = std::get_if<native_order::CancelledEvent>(&*event.command)) {
                if (armed && cancel->definition && cancel->handle() == child) {
                    CHECK(cancel->ordinal > armed_ordinal);
                    ++withdrawn;
                }
            }
        }
    }
    CHECK(armed == 1 && withdrawn == 1);
}

void row16_invalid_installed_level_placeholder_keep() {
    // Existing internal fixture: without RestingLimitTickReach an on-grid
    // COOF limit keeps its raw level. Scope it to these two single-threaded
    // runs so the actual negative-installed-level KEEP branch is exercised.
    struct RawLimitScope {
        using Rule = source::detail::PineCallbackLifecycleRule;
        bool previous = source::detail::pine_callback_lifecycle_rule(Rule::RestingLimitTickReach);
        RawLimitScope() {
            source::detail::set_pine_callback_lifecycle_rule(Rule::RestingLimitTickReach, false);
        }
        ~RawLimitScope() {
            source::detail::set_pine_callback_lifecycle_rule(Rule::RestingLimitTickReach, previous);
        }
    } scope;
    row16_placeholder_withdrawal_keep(true);
}

void row18_threshold_overflow() {
    for (const Shape shape : {Shape::Drawdown, Shape::Intraday, Shape::Preclose}) {
        row = shape == Shape::Drawdown ? "row18_drawdown_threshold"
            : shape == Shape::Intraday ? "row18_marked_intraday_threshold"
                                       : "row18_preclose_intraday_threshold";
        Run run(shape, 0x1p1023, 10000.0, 100.0);
        expect_g(run, "risk.threshold", "risk");
        if (shape != Shape::Preclose) CHECK(run.report.total_trades == 0);
        else {
            CHECK(run.host && run.host->calls == 3);
            int closed = 0;
            if (run.host) {
                for (const auto& event : run.host->native_events(0)) {
                    if (!event.command) continue;
                    const auto* applied = std::get_if<native_order::ExecutionAppliedEvent>(
                        &*event.command);
                    if (!applied || applied->closed_trade_count == 0) continue;
                    CHECK(applied->closed_trade_count == 1);
                    CHECK(same_bits(applied->closed_units, 1.0));
                    CHECK(same_bits(applied->resolved_price, 99.0));
                    ++closed;
                }
            }
            CHECK(closed == 1);
        }
    }
}

void row18_infinite_configured_limit_is_not_g() {
    row = "row18_infinite_configured_limit_is_not_g";
    Run run(Shape::InfiniteConfigured, std::numeric_limits<double>::infinity(), 10000.0, 100.0);
    CHECK(run.code.empty() && run.args.empty());
    CHECK(run.status == 0);
    CHECK(run.host && run.host->calls == 40);
    CHECK(run.report.total_trades == 0);
}

void row20_filled_counter_keep() {
    row = "row20_filled_counter_keep";
    Run run(Shape::FilledCounter, 0.0, 10000.0, 100.0);
    Run control(Shape::NoExit, 0.0, 10000.0, 100.0);
    CHECK(run.code.empty() && run.args.empty() && run.status == 0);
    CHECK(control.code.empty() && control.status == 0);
    CHECK(run.report.trades_len == 1 && control.report.trades_len == 1);
    CHECK(run.host && run.host->calls == 40);
    if (run.report.trades_len == 1 && control.report.trades_len == 1) {
        CHECK(same_bits(run.report.trades[0].entry_price, control.report.trades[0].entry_price));
        CHECK(same_bits(run.report.trades[0].exit_price, control.report.trades[0].exit_price));
        CHECK(same_bits(run.report.trades[0].pnl, control.report.trades[0].pnl));
        CHECK(same_bits(run.report.trades[0].qty, control.report.trades[0].qty));
        CHECK(same_bits(run.report.trades[0].qty, 1.0));
    }
}

void finite_relative_bits() {
    row = "row15_finite_relative_near_boundary_bits";
    Run relative(Shape::Profit, 0x1p1022, 0x1p1023, 0x1p1022);
    Run absolute(Shape::Absolute, 0x1p1023, 0x1p1023, 0x1p1022);
    CHECK(relative.code.empty() && absolute.code.empty());
    CHECK(relative.status == 0 && absolute.status == 0);
    CHECK(relative.report.trades_len == 1 && absolute.report.trades_len == 1);
    if (relative.report.trades_len == 1 && absolute.report.trades_len == 1) {
        const auto& a = relative.report.trades[0];
        const auto& b = absolute.report.trades[0];
        CHECK(same_bits(a.entry_price, 0x1p1022));
        CHECK(same_bits(a.entry_price, b.entry_price));
        CHECK(same_bits(a.exit_price, b.exit_price));
        CHECK(same_bits(a.pnl, b.pnl));
        CHECK(same_bits(a.pnl_pct, b.pnl_pct));
        CHECK(same_bits(a.qty, b.qty));
        CHECK(same_bits(a.max_runup, b.max_runup));
        CHECK(same_bits(a.max_drawdown, b.max_drawdown));
        CHECK(same_bits(a.commission, b.commission));
    }
}
} // namespace

int main() {
    row15_finite_relative_sum_overflow();
    row16_placeholder_withdrawal_keep();
    row16_invalid_installed_level_placeholder_keep();
    row18_threshold_overflow();
    row18_infinite_configured_limit_is_not_g();
    row20_filled_counter_keep();
    finite_relative_bits();
    std::printf("value-invalid relative/risk source: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
