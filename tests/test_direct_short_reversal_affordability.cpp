/*
 * Focused coverage for direct default-sized short-reversal affordability.
 *
 * An omitted-qty, percent-of-equity=100 MARKET LONG-to-SHORT reversal at
 * margin_short=100 receives a fill-price affordability pass followed by one
 * bounded adverse-high retry. The established one-contract finite-price
 * floor-zero fallback depends on the budget and lot grid. The optional
 * full-residual interpretation yields to that settled slice whenever the
 * one-contract fallback is expressible (finding 279: TV slices and holds at
 * eps-scale deficits, it never full-liquidates there).
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
            ++tests_failed;                                                \
        } else {                                                           \
            ++tests_passed;                                                \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                             \
    do {                                                                    \
        const double _actual = (actual);                                    \
        const double _expected = (expected);                                \
        if (!(std::fabs(_actual - _expected) <= (tolerance))) {             \
            std::printf("  FAIL  %s:%d  %s == %.12f, expected %.12f\n",    \
                        __FILE__, __LINE__, #actual, _actual, _expected);    \
            ++tests_failed;                                                 \
        } else {                                                            \
            ++tests_passed;                                                 \
        }                                                                   \
    } while (0)

Bar bar(int64_t timestamp, double open, double high, double low, double close) {
    Bar out;
    out.timestamp = timestamp;
    out.open = open;
    out.high = high;
    out.low = low;
    out.close = close;
    out.volume = 1.0;
    return out;
}

class DirectShortReversalProbe : public pineforge::source::PineStrategyHost {
public:
    double position_size() const { return signed_position_size(); }
    bool has_live_short_position() const {
        return position_side_ == PositionSide::SHORT
            && position_qty_ > 1e-9
            && position_cycle_seq_ != 0
            && !pyramid_entries_.empty()
            && pyramid_entries_.front().qty > 1e-9
            && pyramid_entries_.front().entry_incarnation != 0;
    }
    bool opening_event_cleared() const {
        return !opening_obligations_.pending()
            && !opening_obligations_.actionable()
            && !opening_obligations_.requires_adverse_pass()
            && std::isnan(opening_obligations_.raw_fill_base());
    }

    std::vector<double> margin_quantities() const {
        std::vector<double> out;
        for (int index = 0; index < trade_count(); ++index) {
            if (closed_trade_exit_comment(index) == "Margin call") {
                out.push_back(closed_trade_size(index));
            }
        }
        return out;
    }

    std::vector<double> margin_prices() const {
        std::vector<double> out;
        for (int index = 0; index < trade_count(); ++index) {
            if (closed_trade_exit_comment(index) == "Margin call") {
                out.push_back(closed_trade_exit_price(index));
            }
        }
        return out;
    }

protected:
    bool opening_owner_matches_position() const {
        const auto& receipt = opening_obligations_.peek();
        return receipt && position_cycle_seq_ != 0
            && receipt->owner().positionCycle == position_cycle_seq_;
    }

    void seed_position(PositionSide side,
                       double entry,
                       double qty,
                       const std::string& entry_key,
                       double realized_net_profit) {
        position_side_ = side;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = current_bar_.timestamp;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = bar_index_;
        trail_best_price_ = entry;
        net_profit_sum_ = realized_net_profit;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, entry_key, bar_index_});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_[entry_key] = qty;
    }
};

class OpeningRetryProbe final : public DirectShortReversalProbe {
public:
    OpeningRetryProbe() {
        initial_capital_ = 99764.603236;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.03;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        seed_position(
            PositionSide::LONG, 3167.25, 31.4892, "L",
            /*realized_net_profit=*/0.0);
        strategy_entry("S", false, kNaN, kNaN, kNaN);
    }
};

void test_direct_reversal_runs_opening_check_and_one_adverse_retry() {
    std::printf(
        "test_direct_reversal_runs_opening_check_and_one_adverse_retry\n");
    const std::vector<Bar> bars = {
        bar(1000, 3145.00, 3145.00, 3145.00, 3145.00),
        bar(2000, 3145.01, 3154.20, 3144.00, 3150.00),
    };
    OpeningRetryProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));
    const std::vector<double> qty = probe.margin_quantities();
    const std::vector<double> price = probe.margin_prices();

    CHECK(qty.size() == 2U);
    CHECK(price.size() == 2U);
    if (qty.size() == 2U && price.size() == 2U) {
        CHECK_NEAR(qty[0], 0.0376, 1e-9);
        CHECK_NEAR(price[0], 3145.01, 1e-9);
        CHECK_NEAR(qty[1], 0.6204, 1e-9);
        CHECK_NEAR(price[1], 3154.20, 1e-9);
    }
    CHECK_NEAR(probe.position_size(), -30.8219, 1e-9);
    CHECK(probe.has_live_short_position());
    CHECK(probe.opening_event_cleared());
}

class FloorZeroPrecedenceProbe final : public DirectShortReversalProbe {
public:
    explicit FloorZeroPrecedenceProbe(bool full_residual) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata(
            "margin_zero_cover_full_liquidation",
            full_residual ? 1.0 : 0.0);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        seed_position(
            PositionSide::LONG, 4629.63, 2.7738, "L",
            /*realized_net_profit=*/2841.8043809999995);
        strategy_entry("S", false, kNaN, kNaN, kNaN);
        strategy_close("L");
    }
};

void test_direct_reversal_floor_zero_full_residual_yields_to_slice() {
    std::printf(
        "test_direct_reversal_floor_zero_full_residual_yields_to_slice\n");
    const std::vector<Bar> bars = {
        bar(1000, 4506.71, 4506.71, 4506.71, 4506.71),
        bar(2000, 4506.70, 4514.70, 4500.00, 4506.70),
        bar(3000, 4514.70, 4539.00, 4500.00, 4530.00),
    };

    FloorZeroPrecedenceProbe one_contract(/*full_residual=*/false);
    one_contract.run(bars.data(), static_cast<int>(bars.size()));
    const std::vector<double> one_contract_qty =
        one_contract.margin_quantities();
    const std::vector<double> one_contract_price =
        one_contract.margin_prices();
    CHECK(one_contract_qty.size() == 2U);
    CHECK(one_contract_price.size() == 2U);
    if (one_contract_qty.size() == 2U
        && one_contract_price.size() == 2U) {
        CHECK_NEAR(one_contract_qty[0], 0.0392, 1e-9);
        CHECK_NEAR(one_contract_price[0], 4514.70, 1e-9);
        CHECK_NEAR(one_contract_qty[1], 1.0, 1e-9);
        CHECK_NEAR(one_contract_price[1], 4539.00, 1e-9);
    }
    CHECK_NEAR(one_contract.position_size(), -1.7346, 1e-9);
    CHECK(one_contract.has_live_short_position());
    CHECK(one_contract.opening_event_cleared());

    // The full-residual opt-in yields to the settled one-contract slice at
    // the floor-zero discontinuity: both cells now take the identical lots
    // and HOLD the remainder (TV never full-liquidates at these eps-scale
    // deficits — finding 279, serhan ADX).
    FloorZeroPrecedenceProbe full_residual(/*full_residual=*/true);
    full_residual.run(bars.data(), static_cast<int>(bars.size()));
    const std::vector<double> full_residual_qty =
        full_residual.margin_quantities();
    const std::vector<double> full_residual_price =
        full_residual.margin_prices();
    CHECK(full_residual_qty.size() == 2U);
    CHECK(full_residual_price.size() == 2U);
    if (full_residual_qty.size() == 2U
        && full_residual_price.size() == 2U) {
        CHECK_NEAR(full_residual_qty[0], 0.0392, 1e-9);
        CHECK_NEAR(full_residual_price[0], 4514.70, 1e-9);
        CHECK_NEAR(full_residual_qty[1], 1.0, 1e-9);
        CHECK_NEAR(full_residual_price[1], 4539.00, 1e-9);
    }
    CHECK_NEAR(full_residual.position_size(), -1.7346, 1e-9);
    CHECK(full_residual.has_live_short_position());
    CHECK(full_residual.opening_event_cleared());
}

class TrueFlatFullResidualControlProbe final
    : public DirectShortReversalProbe {
public:
    TrueFlatFullResidualControlProbe() {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata(
            "margin_zero_cover_full_liquidation", 1.0);

        constexpr double qty = 3.6930;
        constexpr double entry = 1799.94;
        constexpr double adverse = 1801.26;
        constexpr double raw_q_min = 0.00005;
        current_bar_ = bar(1000, entry, entry, entry, entry);
        bar_index_ = 0;
        seed_position(
            PositionSide::SHORT, entry, qty, "S",
            /*realized_net_profit=*/0.0);
        const double open_fee = surviving_open_percent_commission_account();
        net_profit_sum_ =
            (qty - raw_q_min) * adverse - initial_capital_ + open_fee
            + (adverse - entry) * qty;
    }

    void on_source_bar(const Bar&) override {}

    void trigger() {
        current_bar_ =
            bar(2000, 1800.00, 1801.26, 1799.50, 1800.50);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

// Control: a commissioned short with the full-residual opt-in still takes
// the settled one-contract slice and holds its remaining physical lot.
void test_true_flat_floor_zero_control_slices_one_contract() {
    std::printf("test_true_flat_floor_zero_control_slices_one_contract\n");
    TrueFlatFullResidualControlProbe probe;
    probe.trigger();
    const std::vector<double> qty = probe.margin_quantities();
    CHECK(qty.size() == 1U);
    if (qty.size() == 1U) {
        CHECK_NEAR(qty[0], 1.0, 1e-9);
    }
    CHECK_NEAR(probe.position_size(), -2.6930, 1e-9);
    CHECK(probe.has_live_short_position());
    CHECK(probe.opening_event_cleared());
}

class DirectPathExclusionProbe final : public DirectShortReversalProbe {
public:
    enum class Mechanism {
        ExplicitQuantity,
        FixedSizing,
        PartialPercentSizing,
        LeveragedShortMargin,
        PricedOrder,
        Slippage,
        ProcessOnClose,
        CalcOnFill,
        BarMagnifier,
        StreamWarmup,
        StreamRealtime,
        OcaMembership,
    };

    explicit DirectPathExclusionProbe(Mechanism mechanism)
        : mechanism_(mechanism) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        margin_call_enabled_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        switch (mechanism_) {
        case Mechanism::FixedSizing:
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 100.0;
            break;
        case Mechanism::PartialPercentSizing:
            default_qty_value_ = 50.0;
            break;
        case Mechanism::LeveragedShortMargin:
            margin_short_ = 50.0;
            break;
        case Mechanism::Slippage:
            slippage_ = 1;
            break;
        case Mechanism::ProcessOnClose:
            process_orders_on_close_ = true;
            break;
        case Mechanism::CalcOnFill:
            calc_on_order_fills_ = true;
            break;
        case Mechanism::BarMagnifier:
            bar_magnifier_enabled_ = true;
            break;
        case Mechanism::StreamWarmup:
            stream_warmup_mode_ = true;
            break;
        case Mechanism::StreamRealtime:
            stream_phase_ = StreamPhase::REALTIME;
            break;
        default:
            break;
        }
    }

    void on_source_bar(const Bar&) override {}

    void exercise() {
        current_bar_ = bar(1000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 0;
        seed_position(
            PositionSide::LONG, 100.0, 100.0, "L",
            /*realized_net_profit=*/0.0);

        if (mechanism_ == Mechanism::ExplicitQuantity) {
            strategy_entry("S", false, kNaN, kNaN, 100.0);
        } else if (mechanism_ == Mechanism::PricedOrder) {
            strategy_entry("S", false, /*limit_price=*/100.0);
        } else if (mechanism_ == Mechanism::OcaMembership) {
            strategy_entry(
                "S", false, kNaN, kNaN, kNaN, "",
                /*oca_name=*/"G", /*oca_type=*/1);
        } else {
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }

        current_bar_ = bar(2000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 1;
        process_pending_orders(current_bar_);

        reversal_filled =
            position_side_ == PositionSide::SHORT
            && position_qty_ > 1e-9;
        direct_opening_path_armed =
            opening_obligations_.pending()
            && opening_obligations_.actionable()
            && opening_obligations_.requires_adverse_pass()
            && std::isfinite(opening_obligations_.raw_fill_base());
    }

    bool reversal_filled = false;
    bool direct_opening_path_armed = false;

private:
    Mechanism mechanism_;
};

void test_direct_path_exclusion_matrix() {
    std::printf("test_direct_path_exclusion_matrix\n");
    using Mechanism = DirectPathExclusionProbe::Mechanism;
    struct Case {
        const char* label;
        Mechanism mechanism;
    };
    const std::array<Case, 12> cases = {{
        {"explicit quantity", Mechanism::ExplicitQuantity},
        {"fixed sizing", Mechanism::FixedSizing},
        {"partial percent sizing", Mechanism::PartialPercentSizing},
        {"leveraged short margin", Mechanism::LeveragedShortMargin},
        {"priced order", Mechanism::PricedOrder},
        {"slippage", Mechanism::Slippage},
        {"process on close", Mechanism::ProcessOnClose},
        {"calc on fill", Mechanism::CalcOnFill},
        {"bar magnifier", Mechanism::BarMagnifier},
        {"stream warmup", Mechanism::StreamWarmup},
        {"stream realtime", Mechanism::StreamRealtime},
        {"OCA membership", Mechanism::OcaMembership},
    }};

    for (const Case& test_case : cases) {
        std::printf("  %s\n", test_case.label);
        DirectPathExclusionProbe probe(test_case.mechanism);
        probe.exercise();
        CHECK(probe.reversal_filled);
        CHECK(!probe.direct_opening_path_armed);
    }
}

class NoEffectAddProbe final : public DirectShortReversalProbe {
public:
    enum class Attempt {
        RejectedByPyramiding,
        QuantizedToZero,
    };

    explicit NoEffectAddProbe(Attempt attempt) : attempt_(attempt) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        margin_call_enabled_ = false;
        pyramiding_ =
            attempt_ == Attempt::RejectedByPyramiding ? 1 : 2;
        qty_step_ =
            attempt_ == Attempt::QuantizedToZero ? 1.0 : 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {}

    void exercise() {
        current_bar_ = bar(1000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 0;
        seed_position(
            PositionSide::LONG, 100.0, 100.0, "L",
            /*realized_net_profit=*/0.0);
        strategy_entry("S", false, kNaN, kNaN, kNaN);

        current_bar_ = bar(2000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 1;
        process_pending_orders(current_bar_);
        captured_before_attempt = opening_obligations_.actionable()
            && opening_obligations_.requires_adverse_pass()
            && opening_owner_matches_position();
        // A disabled margin pass still consumes the opening checkpoint.
        // A later rejected/no-op order must not create another receipt.
        process_margin_call(current_bar_);
        consumed_before_attempt = opening_event_cleared();
        qty_before_attempt = position_qty_;
        const int64_t cycle_before_attempt = position_cycle_seq_;
        const uint64_t fills_before_attempt = broker_fill_event_seq_;
        const uint64_t incarnation_before_attempt = pyramid_entries_.empty()
            ? 0 : pyramid_entries_.front().entry_incarnation;
        const double lot_qty_before_attempt = pyramid_entries_.empty()
            ? 0.0 : pyramid_entries_.front().qty;

        const double requested_qty =
            attempt_ == Attempt::QuantizedToZero ? 0.5 : 1.0;
        strategy_entry("A", false, kNaN, kNaN, requested_qty);
        current_bar_ = bar(3000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 2;
        process_pending_orders(current_bar_);

        quantity_unchanged =
            std::abs(position_qty_ - qty_before_attempt) <= 1e-9;
        owner_preserved = has_live_short_position()
            && position_cycle_seq_ == cycle_before_attempt
            && pyramid_entries_.front().entry_incarnation
                == incarnation_before_attempt
            && std::abs(pyramid_entries_.front().qty - lot_qty_before_attempt)
                <= 1e-9;
        no_fill_committed = broker_fill_event_seq_ == fills_before_attempt;
        no_new_obligation = opening_event_cleared();
    }

    bool captured_before_attempt = false;
    bool consumed_before_attempt = false;
    bool quantity_unchanged = false;
    bool owner_preserved = false;
    bool no_fill_committed = false;
    bool no_new_obligation = false;
    double qty_before_attempt = 0.0;

private:
    Attempt attempt_;
};

void test_no_effect_same_side_add_keeps_owner_without_new_obligation() {
    std::printf(
        "test_no_effect_same_side_add_keeps_owner_without_new_obligation\n");
    for (const NoEffectAddProbe::Attempt attempt : {
             NoEffectAddProbe::Attempt::RejectedByPyramiding,
             NoEffectAddProbe::Attempt::QuantizedToZero,
         }) {
        NoEffectAddProbe probe(attempt);
        probe.exercise();
        CHECK(probe.captured_before_attempt);
        CHECK(probe.consumed_before_attempt);
        CHECK(probe.quantity_unchanged);
        CHECK(probe.owner_preserved);
        CHECK(probe.no_fill_committed);
        CHECK(probe.no_new_obligation);
    }
}

class FreshOpeningResetProbe final : public pineforge::source::PineStrategyHost {
public:
    enum class Opening {
        HighLevelEntry,
        RawOrder,
    };

    explicit FreshOpeningResetProbe(Opening opening) : opening_(opening) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        margin_long_ = 100.0;
        margin_call_enabled_ = false;
    }

    void on_source_bar(const Bar&) override {}

    void exercise() {
        current_bar_ = bar(1000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 0;
        const broker::OpeningOwner previous_owner{
            position_cycle_seq_, broker_fill_event_seq_, 0,
            bar_index_, current_bar_.timestamp};
        opening_obligations_.replace(broker::OpeningReceipt::check(
            previous_owner, 90.0,
            broker::OpeningContinuation::RemainingAdversePath));
        stale_obligation_seeded = opening_obligations_.actionable()
            && opening_obligations_.requires_adverse_pass();
        if (opening_ == Opening::HighLevelEntry) {
            strategy_entry("N", true, kNaN, kNaN, 1.0);
        } else {
            strategy_order("N", true, 1.0);
        }

        current_bar_ = bar(2000, 100.0, 100.0, 100.0, 100.0);
        bar_index_ = 1;
        process_pending_orders(current_bar_);
        fresh_open_filled =
            position_side_ == PositionSide::LONG
            && position_qty_ > 1e-9
            && position_cycle_seq_ != 0;
        const auto& receipt = opening_obligations_.peek();
        obligation_replaced = receipt
            && receipt->decision() == broker::OpeningDecision::Check
            && receipt->owner().positionCycle == position_cycle_seq_
            && receipt->owner().positionCycle != previous_owner.positionCycle
            && receipt->owner().producerFill > previous_owner.producerFill
            && !pyramid_entries_.empty()
            && receipt->owner().orderIncarnation
                == pyramid_entries_.back().entry_incarnation
            && receipt->owner().barIndex == bar_index_
            && receipt->owner().timestamp == current_bar_.timestamp
            && std::abs(receipt->raw_fill_base() - 100.0) <= 1e-9
            && !receipt->requires_adverse_pass();
    }

    bool stale_obligation_seeded = false;
    bool fresh_open_filled = false;
    bool obligation_replaced = false;

private:
    Opening opening_;
};

void test_fresh_entry_and_raw_order_replace_stale_obligation() {
    std::printf(
        "test_fresh_entry_and_raw_order_replace_stale_obligation\n");
    for (const FreshOpeningResetProbe::Opening opening : {
             FreshOpeningResetProbe::Opening::HighLevelEntry,
             FreshOpeningResetProbe::Opening::RawOrder,
         }) {
        FreshOpeningResetProbe probe(opening);
        probe.exercise();
        CHECK(probe.stale_obligation_seeded);
        CHECK(probe.fresh_open_filled);
        CHECK(probe.obligation_replaced);
    }
}

class OpeningMutationProbe final : public DirectShortReversalProbe {
public:
    enum class Mutation {
        AcceptedAdd,
        ScriptPartialReduction,
        FullClose,
    };

    explicit OpeningMutationProbe(Mutation mutation)
        : mutation_(mutation) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        margin_call_enabled_ = false;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            seed_position(
                PositionSide::LONG, 100.0, 100.0, "L",
                /*realized_net_profit=*/0.0);
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            captured_after_reversal = opening_obligations_.actionable()
                && opening_obligations_.requires_adverse_pass()
                && opening_owner_matches_position();
            original_cycle_ = position_cycle_seq_;
            original_qty_ = position_qty_;
            original_incarnation_ = pyramid_entries_.empty()
                ? 0 : pyramid_entries_.front().entry_incarnation;
            if (opening_obligations_.peek()) {
                original_fill_ = opening_obligations_.peek()->owner().producerFill;
            }
            if (mutation_ == Mutation::AcceptedAdd) {
                // Placed on bar 2 instead (see below): the all-in short has no
                // free equity at its own fill price, so an add costed as
                // held + add (design-market-entry-affordability) is only
                // affordable once the short is in profit (close 90).
            } else if (mutation_ == Mutation::ScriptPartialReduction) {
                strategy_close(
                    "S", "", 1.0, kNaN, /*immediately=*/true);
                mutation_applied =
                    position_side_ == PositionSide::SHORT
                    && position_qty_ > 1.0
                    && std::abs(position_qty_ - (original_qty_ - 1.0)) <= 1e-9;
                valid_after_mutation = position_cycle_seq_ == original_cycle_
                    && pyramid_entries_.size() == 1
                    && pyramid_entries_.front().entry_incarnation
                        == original_incarnation_
                    && opening_owner_matches_position()
                    && opening_obligations_.peek()->owner().producerFill
                        == original_fill_;
            } else {
                strategy_close(
                    "S", "", kNaN, kNaN, /*immediately=*/true);
                mutation_applied =
                    position_side_ == PositionSide::FLAT;
                valid_after_mutation = position_cycle_seq_ == 0
                    && opening_event_cleared();
            }
        } else if (bar_index_ == 2
                   && mutation_ == Mutation::AcceptedAdd) {
            // MTM 11,000 at close 90 vs (100 + 1) * 90 = 9,090: admitted.
            strategy_entry("A", false, kNaN, kNaN, 1.0);
        } else if (bar_index_ == 3
                   && mutation_ == Mutation::AcceptedAdd) {
            mutation_applied =
                position_side_ == PositionSide::SHORT
                && position_entry_count_ == 2
                && std::abs(position_qty_ - (original_qty_ + 1.0)) <= 1e-9;
            valid_after_mutation = position_cycle_seq_ == original_cycle_
                && opening_obligations_.actionable()
                && opening_owner_matches_position()
                && opening_obligations_.peek()->owner().producerFill > original_fill_
                && pyramid_entries_.size() == 2
                && pyramid_entries_.front().entry_incarnation == original_incarnation_
                && opening_obligations_.peek()->owner().orderIncarnation
                    == pyramid_entries_.back().entry_incarnation;
        }
    }

    bool captured_after_reversal = false;
    bool mutation_applied = false;
    bool valid_after_mutation = false;

private:
    Mutation mutation_;
    int64_t original_cycle_ = 0;
    uint64_t original_incarnation_ = 0;
    uint64_t original_fill_ = 0;
    double original_qty_ = 0.0;
};

void test_direct_reversal_owner_and_obligation_follow_script_mutations() {
    std::printf("test_direct_reversal_owner_and_obligation_follow_script_mutations\n");
    const std::vector<Bar> bars = {
        bar(1000, 100.0, 100.0, 100.0, 100.0),
        bar(2000, 100.0, 100.0, 100.0, 100.0),
        bar(3000, 90.0, 90.0, 90.0, 90.0),
        bar(4000, 90.0, 90.0, 90.0, 90.0),
    };

    for (const OpeningMutationProbe::Mutation mutation : {
             OpeningMutationProbe::Mutation::AcceptedAdd,
             OpeningMutationProbe::Mutation::ScriptPartialReduction,
             OpeningMutationProbe::Mutation::FullClose,
         }) {
        OpeningMutationProbe probe(mutation);
        probe.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(probe.captured_after_reversal);
        CHECK(probe.mutation_applied);
        CHECK(probe.valid_after_mutation);
    }
}

class RunResetControlProbe final : public pineforge::source::PineStrategyHost {
public:
    RunResetControlProbe() { margin_call_enabled_ = false; }

    void on_source_bar(const Bar&) override {}

    void prime_opening_obligation() {
        opening_obligations_.replace(broker::OpeningReceipt::check(
            {position_cycle_seq_, broker_fill_event_seq_, 0,
             bar_index_, current_bar_.timestamp}, 100.0,
            broker::OpeningContinuation::RemainingAdversePath));
    }

    bool opening_pending() const {
        return opening_obligations_.pending();
    }
};

void test_run_reset_clears_opening_obligation() {
    std::printf("test_run_reset_clears_opening_obligation\n");
    const std::vector<Bar> bars = {
        bar(1000, 100.0, 100.0, 100.0, 100.0),
    };
    RunResetControlProbe probe;
    probe.prime_opening_obligation();
    CHECK(probe.opening_pending());
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(!probe.opening_pending());
}

}  // namespace

int main() {
    std::printf("--- direct short reversal affordability ---\n");
    test_direct_reversal_runs_opening_check_and_one_adverse_retry();
    test_direct_reversal_floor_zero_full_residual_yields_to_slice();
    test_true_flat_floor_zero_control_slices_one_contract();
    test_direct_path_exclusion_matrix();
    test_no_effect_same_side_add_keeps_owner_without_new_obligation();
    test_fresh_entry_and_raw_order_replace_stale_obligation();
    test_direct_reversal_owner_and_obligation_follow_script_mutations();
    test_run_reset_clears_opening_obligation();
    std::printf(
        "=== Results: %d passed, %d failed ===\n",
        tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
