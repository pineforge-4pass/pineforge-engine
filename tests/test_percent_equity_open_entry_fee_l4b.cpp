// CHECK-parity native-route port of test_percent_equity_open_entry_fee.cpp.
// All setup is issued from on_source_bar; no source lot, fee ledger or margin
// checkpoint is fabricated in this fixture.
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
int passed = 0;
int failed = 0;

#define CHECK(expr) do {                                                        \
    if (!(expr)) { ++failed;                                                    \
        std::printf("FAIL %s:%d %s\\n", __FILE__, __LINE__, #expr); }         \
    else ++passed;                                                              \
} while (0)

bool near(double lhs, double rhs, double tolerance = 1e-9) {
    return std::abs(lhs - rhs) <= tolerance;
}

Bar bar(double p, std::int64_t t) { return {p, p, p, p, 1.0, t}; }

class FeeHost final : public source::PineStrategyHost {
public:
    enum class Mode { Partial, Reversal, Holding, Margin, KiHolding, KiMargin };

    explicit FeeHost(Mode mode, CommissionType commission = CommissionType::PERCENT)
        : mode_(mode) {
        source::PineStrategyConfig config;
        const bool ki56 = mode == Mode::KiHolding || mode == Mode::KiMargin;
        config.initial_capital = mode == Mode::Reversal || ki56 ? 10000.0 : 1000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = ki56 ? 50.0 : 100.0;
        config.commission_type = static_cast<int>(commission);
        config.commission_value = commission == CommissionType::PERCENT
            ? (mode == Mode::Reversal || ki56 ? 10.0 : 1.0) : 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.process_orders_on_close = true;
        config.pyramiding = mode == Mode::Partial || mode == Mode::KiHolding ? 2 : 1;
        configure_pine_strategy(config);
        qty_step_ = mode == Mode::Reversal || ki56 ? 0.0001 : 0.0;
        syminfo_mintick_ = 0.01;
        if (mode == Mode::Partial || mode == Mode::Holding || mode == Mode::KiHolding)
            margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {
        if (mode_ == Mode::Partial) {
            if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 10.0);
            if (pine_bar_index() == 1) strategy_close("S", "", kNaN, 40.0);
            if (pine_bar_index() == 2) {
                // Public placement probe for the base calc_qty read: its
                // unreachable sell limit exposes the frozen sizing tuple
                // without changing the surviving six-unit position.
                strategy_entry("NEXT", false, 1000.0);
                for (const auto& row : source_pending_view())
                    if (row.id == "NEXT") next_default_qty = row.frozen_default_qty;
            }
        } else if (mode_ == Mode::Reversal) {
            if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 1.0);
            if (pine_bar_index() == 1)
                strategy_entry("S", false, kNaN, kNaN, 50.0, "", "", 0,
                               static_cast<int>(QtyType::PERCENT_OF_EQUITY));
        } else if (mode_ == Mode::Holding || mode_ == Mode::Margin) {
            if (pine_bar_index() == 0) strategy_entry("S", false, kNaN, kNaN, 10.0);
        } else if (mode_ == Mode::KiHolding) {
            if (pine_bar_index() == 0) strategy_entry("seed", true, kNaN, kNaN, 1.0);
            if (pine_bar_index() == 1) {
                strategy_entry("PROBE", true, 1.0);
                for (const auto& row : source_pending_view())
                    if (row.id == "PROBE") next_default_qty = row.frozen_default_qty;
            }
        } else if (mode_ == Mode::KiMargin) {
            if (pine_bar_index() == 0)
                strategy_entry("seed", false, kNaN, kNaN, 4.7745);
        }
    }

    double position() const {
        return mode_ == Mode::KiHolding ? next_default_qty : live_position_size();
    }
    int margin_count() const {
        int result = 0;
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") ++result;
        return result;
    }
    double first_margin_qty() const {
        for (int i = 0; i < trade_count(); ++i)
            if (get_trade(i).exit_comment == "Margin call") return get_trade(i).qty;
        return kNaN;
    }
    std::string first_exit_comment() const {
        return trade_count() == 0 ? std::string() : get_trade(0).exit_comment;
    }

    double next_default_qty = kNaN;

private:
    Mode mode_;
};

void test_sizing_debits_surviving_snapshot() {
    FeeHost probe(FeeHost::Mode::Partial);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000), bar(100.0, 122000),
                        bar(100.0, 182000)};
    probe.run(tape, 4);
    CHECK(near(probe.next_default_qty, 986.0 / 1.01 / 100.0));
    CHECK(std::isfinite(probe.next_default_qty));
}

void test_flat_sizing_has_no_open_fee_debit() {
    FeeHost probe(FeeHost::Mode::Holding);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000)};
    probe.run(tape, 2);
    CHECK(near(std::abs(probe.position()), 10.0));
}

void test_adverse_margin_ledger_debits_entry_fee() {
    FeeHost probe(FeeHost::Mode::Margin);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000)};
    probe.run(tape, 2);
    CHECK(probe.margin_count() == 1);
    CHECK(probe.first_exit_comment() == "Margin call");
    CHECK(near(probe.first_margin_qty(), 0.4));
    CHECK(near(std::abs(probe.position()), 9.6));
}

void test_non_percent_scope_is_unchanged() {
    FeeHost cash(FeeHost::Mode::Margin, CommissionType::CASH_PER_ORDER);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000)};
    cash.run(tape, 2);
    CHECK(cash.margin_count() == 0);
    CHECK(near(std::abs(cash.position()), 10.0));
}

void test_margin_ledger_is_independent_of_default_percent() {
    FeeHost ninety_nine(FeeHost::Mode::Margin);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000)};
    ninety_nine.run(tape, 2);
    CHECK(ninety_nine.margin_count() == 1);
    CHECK(ninety_nine.first_exit_comment() == "Margin call");
    CHECK(near(ninety_nine.first_margin_qty(), 0.4));
    CHECK(near(std::abs(ninety_nine.position()), 9.6));
}

void test_fifo_partial_scales_surviving_paid_fee_snapshot() {
    FeeHost probe(FeeHost::Mode::Partial);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000), bar(100.0, 122000),
                        bar(100.0, 182000)};
    probe.run(tape, 4);
    CHECK(near(std::abs(probe.position()), 6.0));
    CHECK(near(10.0 - std::abs(probe.position()), 4.0));
    CHECK(near(probe.next_default_qty, 986.0 / 1.01 / 100.0));
}

void test_percent_typed_reversal_does_not_double_debit_old_fee() {
    FeeHost probe(FeeHost::Mode::Reversal);
    const Bar tape[] = {bar(100.0, 2000), bar(100.0, 62000), bar(100.0, 122000)};
    probe.run(tape, 3);
    CHECK(probe.trade_count() == 1);
    CHECK(near(probe.position(), -45.3636, 1e-9));
}

void test_ki56_clean_room_tv_quantities() {
    FeeHost holding(FeeHost::Mode::KiHolding);
    const Bar holding_tape[] = {bar(1900.21, 2000), bar(1896.99, 62000)};
    holding.run(holding_tape, 2);
    CHECK(near(std::abs(holding.position()), 2.3498, 1e-9));

    FeeHost margin(FeeHost::Mode::KiMargin);
    const Bar margin_tape[] = {bar(1900.21, 2000), {1900.21, 1904.46, 1900.21, 1900.21, 1.0, 62000}};
    margin.run(margin_tape, 2);
    CHECK(margin.margin_count() == 1);
    CHECK(margin.first_exit_comment() == "Margin call");
    CHECK(near(margin.first_margin_qty(), 0.0428, 1e-9));
    CHECK(near(std::abs(margin.position()), 4.7317, 1e-9));
}

}  // namespace

int main() {
    test_sizing_debits_surviving_snapshot();
    test_flat_sizing_has_no_open_fee_debit();
    test_adverse_margin_ledger_debits_entry_fee();
    test_non_percent_scope_is_unchanged();
    test_margin_ledger_is_independent_of_default_percent();
    test_fifo_partial_scales_surviving_paid_fee_snapshot();
    test_percent_typed_reversal_does_not_double_debit_old_fee();
    test_ki56_clean_room_tv_quantities();
    std::printf("%d passed, %d failed\\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
