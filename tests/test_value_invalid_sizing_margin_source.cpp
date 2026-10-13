// EN-132 D6 rows 1/3/7. Authored before the adapter producers.
// Source-bound companion to the source-free native TU. The public stream
// warmup ABI runs real source callbacks; no generated/private fixture is used.
#define PINEFORGE_VALUE_INVALID_SIZING_MARGIN_FIXTURES 1
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pineforge.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::source;
int checks = 0;
int failures = 0;
const char* row = "setup";
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", row, __LINE__, #condition); } } while (0)
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double largest = std::numeric_limits<double>::max();

class Probe final : public PineStrategyHost {
public:
    explicit Probe(QtyType type = QtyType::PERCENT_OF_EQUITY,
                   double value = 100.0, CommissionType fee = CommissionType::PERCENT,
                   double fee_value = 0.0, double margin = 0.0) {
        PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_type = static_cast<int>(type);
        config.default_qty_value = value;
        config.commission_type = static_cast<int>(fee);
        config.commission_value = fee_value;
        config.margin_long = config.margin_short = margin;
        configure_pine_strategy(config);
    }
    std::function<void(Probe&, int)> script;
    int calls = 0;
    void on_source_bar(const Bar&) override {
        const int index = calls++;
        if (script) script(*this, index);
    }
    double position_units() const { return signed_position_size(); }
    double fixture_units(const PineSizingSnapshot& sizing) const {
        return fixture_default_sizing_units(sizing);
    }
};
PineSizingSnapshot snapshot() {
    PineSizingSnapshot out;
    out.equity = out.strategy_equity = 1000.0;
    out.mark = out.price = 100.0;
    out.fx = 1.0;
    return out;
}
struct Result {
    int rc = 0;
    int status = 0;
    std::string code, args;
    std::vector<pf_trade_t> trades;
};
Result run(Probe& probe) {
    pf_strategy_t handle = static_cast<BacktestEngine*>(&probe);
    pf_bar_t bars[5]{};
    for (int i = 0; i < 5; ++i) {
        bars[i].timestamp = 1736121600000LL + i * 60000LL;
        bars[i].open = bars[i].high = bars[i].low = bars[i].close = 100.0;
        bars[i].volume = 1.0;
    }
    Result out;
    out.rc = strategy_stream_begin(handle, bars, 5, "1", "1");
    out.status = strategy_last_run_status(handle);
    const char* code = strategy_get_last_error_code(handle);
    const char* args = strategy_get_last_error_args(handle);
    CHECK(code && args);
    out.code = code ? code : "<null>";
    out.args = args ? args : "<null>";
    if (out.rc == 0) CHECK(strategy_stream_end(handle, 0) == 0);
    pf_report_t report{};
    (void)strategy_stream_fill_report(handle, &report);
    if (report.total_trades > 0 && report.trades)
        out.trades.assign(report.trades, report.trades + report.total_trades);
    CHECK(report.total_trades == static_cast<int>(out.trades.size()));
    strategy_native_report_free_v1(&report);
    return out;
}
void failed(const Result& result, const char* field, const char* stage, const char* reason) {
    CHECK(result.rc == -1);
    CHECK(result.status == 1);
    CHECK(result.code == "engine_command_value_invalid");
    const std::string expected = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"" + reason + "\",\"stage\":\"" + stage + "\"}";
    CHECK(result.args == expected);
    CHECK(result.trades.empty());
}
void completed(const Result& result) {
    CHECK(result.rc == 0);
    CHECK(result.status == 0);
    CHECK(result.code.empty());
    CHECK(result.args.empty());
}
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

void row1_cash_arithmetic_overflow() {
    row = "row1_percent_cash_overflow";
    Probe percent(QtyType::PERCENT_OF_EQUITY, largest);
    percent.script = [](Probe& p, int index) {
        if (index) return;
        auto sizing = snapshot();
        sizing.equity = largest;
        (void)p.fixture_units(sizing);
    };
    failed(run(percent), "sizing.quantity", "sizing", "nonfinite");

    row = "row1_cash_fee_unit_cost_overflow";
    Probe unit_cost(QtyType::PERCENT_OF_EQUITY, 100, CommissionType::CASH_PER_CONTRACT, 1);
    unit_cost.script = [](Probe& p, int index) {
        if (index) return;
        auto sizing = snapshot();
        sizing.price = largest;
        sizing.fx = 2.0;
        (void)p.fixture_units(sizing);
    };
    failed(run(unit_cost), "sizing.quantity", "sizing", "nonfinite");

    row = "row1_cash_fee_output_overflow";
    Probe fee_output(QtyType::PERCENT_OF_EQUITY, 100, CommissionType::CASH_PER_CONTRACT, 1);
    fee_output.script = [](Probe& p, int index) {
        if (index) return;
        auto sizing = snapshot();
        sizing.price = largest / 2.0;
        (void)p.fixture_units(sizing);
    };
    failed(run(fee_output), "sizing.quantity", "sizing", "nonfinite");

    row = "row1_cash_fee_divisor_overflow";
    Probe divisor(QtyType::PERCENT_OF_EQUITY, 100, CommissionType::CASH_PER_CONTRACT, largest);
    divisor.script = [](Probe& p, int index) {
        if (index) return;
        auto sizing = snapshot();
        sizing.strategy_equity = 1.0;
        sizing.price = largest / 2.0;
        (void)p.fixture_units(sizing);
    };
    failed(run(divisor), "sizing.quantity", "sizing", "nonfinite");

    row = "row1_cash_order_fee_subtraction_overflow";
    Probe subtraction(QtyType::PERCENT_OF_EQUITY, -100, CommissionType::CASH_PER_ORDER, largest);
    subtraction.script = [](Probe& p, int index) {
        if (index) return;
        auto sizing = snapshot();
        sizing.strategy_equity = largest / 2.0;
        (void)p.fixture_units(sizing);
    };
    failed(run(subtraction), "sizing.quantity", "sizing", "nonfinite");
}
void row1_nonfinite_conversion_input() {
    for (const bool price : {false, true}) {
        for (const double value : {inf, nan}) {
            row = price ? "row1_price_nonfinite" : "row1_fx_nonfinite";
            Probe probe;
            probe.script = [price, value](Probe& p, int index) {
                if (index) return;
                auto sizing = snapshot();
                if (price) sizing.price = value;
                else sizing.fx = value;
                (void)p.fixture_units(sizing);
            };
            failed(run(probe), "sizing.quantity", "sizing", "nonfinite");
        }
    }
}
void row3_nonfinite_equity() {
    for (const bool cash_fee : {false, true}) {
        for (const double equity : {inf, -inf, nan}) {
            row = cash_fee ? "row3_strategy_equity_nonfinite" : "row3_equity_nonfinite";
            Probe probe(QtyType::PERCENT_OF_EQUITY, 100,
                        cash_fee ? CommissionType::CASH_PER_CONTRACT : CommissionType::PERCENT,
                        cash_fee ? 1.0 : 0.0);
            probe.script = [cash_fee, equity](Probe& p, int index) {
                if (index) return;
                auto sizing = snapshot();
                if (cash_fee) sizing.strategy_equity = equity;
                else sizing.equity = equity;
                (void)p.fixture_units(sizing);
            };
            failed(run(probe), "sizing.basis", "sizing", "nonfinite");
        }
    }
}
void row7_placement_requirement_overflow() {
    // L6: finite explicit overflow is KEEP; declaration-derived requirements
    // and nonfinite placement inputs remain G. Preserve the exact G payload.
    for (const bool is_long : {false, true}) {
        row = is_long ? "row7_default_long_placement_overflow"
                      : "row7_default_short_placement_overflow";
        Probe default_units(QtyType::FIXED, 1.0e308, CommissionType::PERCENT, 0.0, 100.0);
        default_units.script = [is_long](Probe& p, int index) {
            if (!index) p.strategy_entry("L", is_long);
        };
        failed(run(default_units), "margin.required", "margin", "nonfinite");
    }
    for (const bool default_quantity : {false, true}) {
        for (const double value : {inf, -inf, nan}) {
            row = default_quantity ? "row7_default_pointvalue_nonfinite"
                                   : "row7_explicit_pointvalue_nonfinite";
            Probe probe(QtyType::FIXED, 1.0, CommissionType::PERCENT, 0.0, 100.0);
            probe.script = [default_quantity, value](Probe& p, int index) {
                if (index) return;
                // Inject the derived placement tuple on an adapter bound to
                // the running host. The host's admitted run spec stays valid;
                // a declaration refusal must not stand in for the G producer.
                PineExecutionAdapter placement(p);
                PineStrategyConfig config;
                config.initial_capital = 1000.0;
                config.margin_long = config.margin_short = 100.0;
                placement.set_configuration(config);
                StagedConfiguration staged;
                staged.syminfo.mintick = 0.01;
                staged.syminfo.pointvalue = value;
                placement.set_staged_configuration(staged);
                placement.entry("G", true, nan, nan, default_quantity ? nan : 1.0);
            };
            failed(run(probe), "margin.required", "margin", "nonfinite");
        }
    }
}
void keep_finite_explicit_placement_overflow() {
    for (const double margin : {100.0, 200.0}) {
        for (const bool is_long : {false, true}) {
            for (const double units : {1.0e307, 1.0e308}) {
                row = margin == 100.0 ? "keep_explicit_signal_overflow_continues"
                                     : "keep_explicit_affordability_overflow_continues";
                Probe probe(QtyType::FIXED, 1.0, CommissionType::PERCENT, 0.0, margin);
                bool continued = false;
                probe.script = [is_long, units, &continued](Probe& p, int index) {
                    if (!index) {
                        p.strategy_entry("overflow", is_long, nan, nan, units);
                        continued = true;
                        p.strategy_entry("L", is_long, nan, nan, 1.0);
                    }
                    if (index == 2) {
                        CHECK(p.position_units() == (is_long ? 1.0 : -1.0));
                        p.strategy_close("L");
                    }
                };
                const auto result = run(probe);
                completed(result);
                CHECK(continued);
                CHECK(result.trades.size() == 1);
                CHECK(probe.position_units() == 0.0);
                if (result.trades.size() == 1) {
                    const auto& trade = result.trades[0];
                    CHECK(same_bits(trade.qty, 1.0));
                    CHECK(same_bits(trade.entry_price, 100.0));
                    CHECK(same_bits(trade.exit_price, 100.0));
                    CHECK(trade.pnl == 0.0);
                    CHECK(trade.commission == 0.0);
                }
            }
        }
    }
}
void keep_defined_zero_equity() {
    for (const bool cash_fee : {false, true}) {
        for (const double equity : {0.0, -1.0}) {
            row = cash_fee ? "row2_keep_cash_fee_equity" : "row2_keep_percent_equity";
            Probe probe(QtyType::PERCENT_OF_EQUITY, 100,
                        cash_fee ? CommissionType::CASH_PER_CONTRACT : CommissionType::PERCENT,
                        cash_fee ? 1.0 : 0.0);
            probe.script = [cash_fee, equity](Probe& p, int index) {
                if (index) return;
                auto sizing = snapshot();
                if (cash_fee) sizing.strategy_equity = equity;
                else sizing.equity = equity;
                const double units = p.fixture_units(sizing);
                CHECK(same_bits(units, 0.0));
                if (units > 0.0) p.strategy_entry("unexpected", true, nan, nan, units);
            };
            const auto result = run(probe);
            completed(result);
            CHECK(result.trades.empty());
            CHECK(probe.position_units() == 0.0);
        }
    }
    row = "row1_keep_zero_percent_cash";
    Probe zero(QtyType::PERCENT_OF_EQUITY, 0.0);
    zero.script = [](Probe& p, int index) {
        if (!index) p.strategy_entry("zero", true);
    };
    const auto result = run(zero);
    completed(result);
    CHECK(result.trades.empty());
}
void keep_disabled_margin() {
    for (const double margin : {0.0, -1.0}) {
        row = "row8_keep_nonpositive_margin_model";
        Probe probe(QtyType::FIXED, 1.0, CommissionType::PERCENT, 0.0, margin);
        probe.script = [](Probe& p, int index) {
            if (!index) p.strategy_entry("L", true, nan, nan, 1.0);
            if (index == 2) p.strategy_close("L");
        };
        const auto result = run(probe);
        completed(result);
        CHECK(result.trades.size() == 1);
        const auto state = probe.native_state();
        CHECK(state.spec && !state.spec->margin);
    }
}
void finite_boundary_is_bit_identical() {
    row = "source_finite_boundary_sized_vs_explicit";
    const double cash = std::nextafter(300.0, 0.0);
    const double expected = cash / (100.0 * 1.0 * 1.0);
    Probe sized(QtyType::CASH, cash, CommissionType::PERCENT, 0.0, 100.0);
    sized.script = [expected](Probe& p, int index) {
        if (!index) {
            CHECK(same_bits(p.fixture_units(snapshot()), expected));
            p.strategy_entry("L", true);
        }
        if (index == 2) p.strategy_close("L");
    };
    Probe explicit_units(QtyType::FIXED, 1.0, CommissionType::PERCENT, 0.0, 100.0);
    explicit_units.script = [expected](Probe& p, int index) {
        if (!index) p.strategy_entry("L", true, nan, nan, expected);
        if (index == 2) p.strategy_close("L");
    };
    const auto actual = run(sized);
    const auto reference = run(explicit_units);
    completed(actual);
    completed(reference);
    CHECK(actual.trades.size() == 1 && reference.trades.size() == 1);
    if (actual.trades.size() != 1 || reference.trades.size() != 1) return;
    const auto& a = actual.trades[0];
    const auto& b = reference.trades[0];
    CHECK(same_bits(a.qty, expected));
    CHECK(same_bits(a.qty, b.qty));
    CHECK(same_bits(a.entry_price, b.entry_price));
    CHECK(same_bits(a.exit_price, b.exit_price));
    CHECK(same_bits(a.pnl, b.pnl));
    CHECK(same_bits(a.commission, b.commission));
    CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
}
void finite_cash_fee_boundary_is_bit_identical() {
    row = "source_finite_cash_fee_boundary";
    const double money = std::nextafter(303.0, 0.0);
    // The frozen pre-change grouping: first convert the cash-fee budget,
    // then divide by the same 100 * point-value * FX denominator.
    const double expected = (money * 100.0 / (100.0 + 1.0)) / (100.0 * 1.0 * 1.0);
    Probe derived(QtyType::PERCENT_OF_EQUITY, 100.0,
                  CommissionType::CASH_PER_CONTRACT, 1.0, 100.0);
    derived.script = [money, expected](Probe& p, int index) {
        if (!index) {
            auto sizing = snapshot();
            sizing.strategy_equity = money;
            const double units = p.fixture_units(sizing);
            CHECK(same_bits(units, expected));
            p.strategy_entry("L", true, nan, nan, units);
        }
        if (index == 2) p.strategy_close("L");
    };
    Probe reference(QtyType::PERCENT_OF_EQUITY, 100.0,
                    CommissionType::CASH_PER_CONTRACT, 1.0, 100.0);
    reference.script = [expected](Probe& p, int index) {
        if (!index) p.strategy_entry("L", true, nan, nan, expected);
        if (index == 2) p.strategy_close("L");
    };
    const auto actual = run(derived);
    const auto old_grouping = run(reference);
    completed(actual);
    completed(old_grouping);
    CHECK(actual.trades.size() == 1 && old_grouping.trades.size() == 1);
    if (actual.trades.size() != 1 || old_grouping.trades.size() != 1) return;
    const auto& a = actual.trades[0];
    const auto& b = old_grouping.trades[0];
    CHECK(same_bits(a.qty, expected));
    CHECK(same_bits(a.qty, b.qty));
    CHECK(same_bits(a.entry_price, b.entry_price));
    CHECK(same_bits(a.exit_price, b.exit_price));
    CHECK(same_bits(a.pnl, b.pnl));
    CHECK(same_bits(a.commission, b.commission));
    CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
}
} // namespace
int main() {
    row1_cash_arithmetic_overflow();
    row1_nonfinite_conversion_input();
    row3_nonfinite_equity();
    row7_placement_requirement_overflow();
    keep_finite_explicit_placement_overflow();
    keep_defined_zero_equity();
    keep_disabled_margin();
    finite_boundary_is_bit_identical();
    finite_cash_fee_boundary_is_bit_identical();
    std::printf("EN132 sizing/margin source: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
