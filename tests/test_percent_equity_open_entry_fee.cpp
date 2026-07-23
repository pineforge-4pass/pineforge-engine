/*
 * Paid percent entry fees debit the live broker ledger used by subsequent
 * percent-of-equity sizing and finite adverse-margin checks.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);     \
            ++failed;                                                        \
        } else {                                                             \
            ++passed;                                                        \
        }                                                                    \
    } while (0)

bool near(double lhs, double rhs, double tolerance = 1e-9) {
    return std::abs(lhs - rhs) <= tolerance;
}

Bar bar(double price) {
    Bar result;
    result.open = price;
    result.high = price;
    result.low = price;
    result.close = price;
    result.volume = 1.0;
    result.timestamp = 2000;
    return result;
}

class FeeEquityProbe : public BacktestEngine {
public:
    FeeEquityProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 1.0;
        margin_short_ = 100.0;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {}

    void set_commission_type(CommissionType type) { commission_type_ = type; }
    void set_default_percent(double value) { default_qty_value_ = value; }
    void set_default_type(QtyType type) { default_qty_type_ = type; }

    double flat_qty(double price) {
        position_side_ = PositionSide::FLAT;
        position_qty_ = 0.0;
        pyramid_entries_.clear();
        current_bar_ = bar(price);
        return calc_qty(price);
    }

    double seeded_qty(double price) {
        seed_short();
        current_bar_ = bar(price);
        return calc_qty(price);
    }

    double seeded_typed_percent_qty(double price, double percent) {
        seed_short();
        current_bar_ = bar(price);
        return calc_qty_for_type(
            price, percent,
            static_cast<int>(QtyType::PERCENT_OF_EQUITY));
    }

    void process_seeded_adverse(double price) {
        seed_short();
        current_bar_ = bar(price);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }

    double paid_open_fee() const { return surviving_open_percent_commission_account(); }

    double live_position_qty() const { return position_qty_; }
    int closed_count() const { return static_cast<int>(trades_.size()); }
    double first_closed_qty() const {
        return trades_.empty() ? std::numeric_limits<double>::quiet_NaN()
                               : trades_.front().qty;
    }
    std::string first_exit_comment() const {
        return trades_.empty() ? std::string() : trades_.front().exit_comment;
    }

private:
    void seed_short() {
        trades_.clear();
        net_profit_sum_ = 0.0;
        position_side_ = PositionSide::SHORT;
        position_qty_ = 10.0;
        position_entry_price_ = 100.0;
        position_entry_time_ = 1000;
        position_open_bar_ = 0;
        position_entry_count_ = 1;
        position_cycle_seq_ = 1;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back({100.0, 1000, 10.0, "S", 0});
        pyramid_entries_.back().entry_commission_account = 10.0;
        pyramid_entries_.back().entry_incarnation = 1;
        id_unclosed_qty_["S"] = 10.0;
    }
};

class PartialFeeSnapshotProbe : public BacktestEngine {
public:
    PartialFeeSnapshotProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 1.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        set_margin_call_enabled(false);
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/10.0);
        } else if (bar_index_ == 1) {
            strategy_close(
                "S", "", kNaN, /*qty_percent=*/40.0, false);
        } else if (bar_index_ == 2) {
            surviving_paid_fee = surviving_open_percent_commission_account();
            next_default_qty = calc_qty(current_bar_.close);
            remaining_qty = position_qty_;
        }
    }

    double surviving_paid_fee = kNaN;
    double next_default_qty = kNaN;
    double remaining_qty = kNaN;

private:
    static constexpr double kNaN =
        std::numeric_limits<double>::quiet_NaN();
};

// A percent-typed reversal sizes only after the old position has been fully
// realized. Its closed entry/exit commissions are already in net profit; the
// old lot's open-PnL and entry-fee snapshot must not be counted a second time.
class PercentTypedReversalProbe : public BacktestEngine {
public:
    PercentTypedReversalProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        process_orders_on_close_ = true;
        qty_step_ = 0.0001;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        set_margin_call_enabled(false);
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            strategy_entry(
                "S", false, kNaN, kNaN, /*qty=*/50.0,
                /*comment=*/"", /*oca_name=*/"", /*oca_type=*/0,
                static_cast<int>(QtyType::PERCENT_OF_EQUITY));
        }
    }

    double signed_position() const {
        if (position_side_ == PositionSide::LONG) return position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    }

private:
    static constexpr double kNaN =
        std::numeric_limits<double>::quiet_NaN();
};

// Exact quantities exported by the clean-room Pine v6 KI-56 holding and
// adverse-margin discriminators on ETHUSDT.P, 15m, 2025-04-02.
class Ki56TvOracleProbe : public BacktestEngine {
public:
    Ki56TvOracleProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 50.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {}

    double holding_probe_qty() {
        seed_position(PositionSide::LONG, 1900.21, 1.0);
        current_bar_ = bar(1896.99);
        return calc_qty(1896.99);
    }

    void run_adverse_margin_probe() {
        seed_position(PositionSide::SHORT, 1900.21, 4.7745);
        current_bar_ = bar(1900.21);
        current_bar_.high = 1904.46;
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }

    int closed_count() const { return static_cast<int>(trades_.size()); }
    double first_closed_qty() const {
        return trades_.empty() ? kNaN : trades_.front().qty;
    }
    double remaining_qty() const { return position_qty_; }
    std::string first_exit_comment() const {
        return trades_.empty() ? std::string() : trades_.front().exit_comment;
    }

private:
    static constexpr double kNaN =
        std::numeric_limits<double>::quiet_NaN();

    void seed_position(PositionSide side, double price, double qty) {
        trades_.clear();
        net_profit_sum_ = 0.0;
        position_side_ = side;
        position_qty_ = qty;
        position_entry_price_ = price;
        position_entry_time_ = 1000;
        position_open_bar_ = 0;
        position_entry_count_ = 1;
        position_cycle_seq_ = 1;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back({price, 1000, qty, "seed", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["seed"] = qty;
    }
};

void test_sizing_debits_surviving_snapshot() {
    FeeEquityProbe probe;
    probe.set_default_percent(50.0);
    CHECK(near(probe.seeded_qty(100.0), 990.0 * 0.50 / 1.01 / 100.0));

    // Explicit qty_type=PERCENT uses the same fee-net equity even when the
    // configured default is FIXED. This prevents the two sizing APIs from
    // drifting apart.
    FeeEquityProbe typed;
    typed.set_default_type(QtyType::FIXED);
    CHECK(near(
        typed.seeded_typed_percent_qty(100.0, 50.0),
        990.0 * 0.50 / 1.01 / 100.0));
}

void test_flat_sizing_has_no_open_fee_debit() {
    FeeEquityProbe probe;
    CHECK(near(probe.flat_qty(100.0), 1000.0 / 1.01 / 100.0));
}

void test_adverse_margin_ledger_debits_entry_fee() {
    FeeEquityProbe probe;
    probe.process_seeded_adverse(100.0);
    CHECK(probe.closed_count() == 1);
    CHECK(probe.first_exit_comment() == "Margin call");
    CHECK(near(probe.first_closed_qty(), 0.4));
    CHECK(near(probe.live_position_qty(), 9.6));
}

void test_non_percent_scope_is_unchanged() {
    FeeEquityProbe cash;
    cash.set_commission_type(CommissionType::CASH_PER_ORDER);
    cash.process_seeded_adverse(100.0);
    CHECK(cash.closed_count() == 0);
    CHECK(near(cash.live_position_qty(), 10.0));

}

void test_margin_ledger_is_independent_of_default_percent() {
    FeeEquityProbe ninety_nine;
    ninety_nine.set_default_percent(99.0);
    ninety_nine.process_seeded_adverse(100.0);
    CHECK(ninety_nine.closed_count() == 1);
    CHECK(ninety_nine.first_exit_comment() == "Margin call");
    CHECK(near(ninety_nine.first_closed_qty(), 0.4));
    CHECK(near(ninety_nine.live_position_qty(), 9.6));
}

void test_fifo_partial_scales_surviving_paid_fee_snapshot() {
    PartialFeeSnapshotProbe probe;
    const Bar bars[] = {bar(100.0), bar(100.0), bar(100.0)};
    probe.run(bars, 3);

    CHECK(near(probe.remaining_qty, 6.0));
    CHECK(near(probe.surviving_paid_fee, 6.0));
    // The closed four-contract slice realizes its $4 entry fee and $4 exit
    // fee in net profit; only the surviving $6 entry-fee snapshot is debited
    // from the next live-equity budget.
    CHECK(near(probe.next_default_qty, 986.0 / 1.01 / 100.0));
}

void test_percent_typed_reversal_does_not_double_debit_old_fee() {
    PercentTypedReversalProbe probe;
    const Bar bars[] = {bar(100.0), bar(100.0)};
    probe.run(bars, 2);

    CHECK(probe.trade_count() == 1);
    // Closing 1 @ 100 realizes its $10 entry and $10 exit commission, leaving
    // $9980 equity. The new 50%-of-equity short reserves its own 10% fee:
    // floor((9980 * .5 / 1.1 / 100) / .0001) * .0001 = 45.3636.
    CHECK(near(probe.signed_position(), -45.3636, 1e-9));
}

void test_ki56_clean_room_tv_quantities() {
    Ki56TvOracleProbe holding;
    CHECK(near(holding.holding_probe_qty(), 2.3498, 1e-9));

    Ki56TvOracleProbe margin;
    margin.run_adverse_margin_probe();
    CHECK(margin.closed_count() == 1);
    CHECK(margin.first_exit_comment() == "Margin call");
    CHECK(near(margin.first_closed_qty(), 0.0428, 1e-9));
    CHECK(near(margin.remaining_qty(), 4.7317, 1e-9));
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
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
