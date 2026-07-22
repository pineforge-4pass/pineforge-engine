/*
 * Regression coverage for the raw-TV SHORT-seed default-FIFO close collision.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                           \
        } else {                                                                \
            ++g_pass;                                                           \
        }                                                                       \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Bar flat_bar(int64_t timestamp) {
    return {100.0, 101.0, 99.0, 100.0, 1'000.0, timestamp};
}

class SourceOrderChain final : public BacktestEngine {
public:
    explicit SourceOrderChain(bool source_long) : source_long_(source_long) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        slippage_ = 0;
        commission_value_ = 0.0;
    }

    void on_bar(const Bar&) override {
        const std::string held = source_long_ ? "Long" : "Short";
        const std::string opposite = source_long_ ? "Short" : "Long";
        if (bar_index_ == 0) {
            strategy_entry(held, source_long_);
        } else if (bar_index_ == 1) {
            CHECK(position_side_ ==
                  (source_long_ ? PositionSide::LONG : PositionSide::SHORT));
            CHECK(pyramid_entries_.size() == 1);
            CHECK(pyramid_entries_[0].entry_id == held);

            // The first close has no live default-FIFO id ledger and therefore
            // queues no broker object. The surviving book is exactly:
            // opposite entry -> held-side entry -> close(held).
            strategy_entry(opposite, !source_long_);
            strategy_entry(held, source_long_);
            strategy_close(opposite);
            strategy_close(held);

            queued_ids_.clear();
            queued_types_.clear();
            for (const PendingOrder& order : pending_orders_) {
                queued_ids_.push_back(order.id);
                queued_types_.push_back(order.type);
            }
        }
    }

    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
    const std::vector<std::string>& queued_ids() const { return queued_ids_; }
    const std::vector<OrderType>& queued_types() const { return queued_types_; }
    uint64_t reported_entry_incarnation(int index) const {
        return closed_trade_entry_incarnation(index);
    }

private:
    bool source_long_;
    std::vector<std::string> queued_ids_;
    std::vector<OrderType> queued_types_;
};

class SameDirectionCloseControl final : public BacktestEngine {
public:
    explicit SameDirectionCloseControl(bool source_long)
        : source_long_(source_long) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        // One live lot leaves admission headroom for the co-queued add.
        pyramiding_ = 2;
        slippage_ = 0;
        commission_value_ = 0.0;
    }

    void on_bar(const Bar&) override {
        const std::string held = source_long_ ? "Long" : "Short";
        if (bar_index_ == 0) {
            strategy_entry(held, source_long_);
        } else if (bar_index_ == 1) {
            strategy_entry(held, source_long_);
            strategy_close(held);
            queued_count_ = pending_orders_.size();
        }
    }

    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
    std::size_t queued_count() const { return queued_count_; }

private:
    bool source_long_;
    std::size_t queued_count_ = 0;
};

enum class RejectedLeg { FirstOpposite, SecondHeld };

class RejectionControl final : public BacktestEngine {
public:
    explicit RejectionControl(RejectedLeg rejected_leg)
        : rejected_leg_(rejected_leg) {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        pyramiding_ = 1;
        margin_call_enabled_ = false;
        syminfo_mintick_ = 0.01;
        commission_value_ = 0.0;
        slippage_ = 0;
        // At the +1 fill gap, 100% margin declines the all-in reversal.
        // Giving the first SHORT leg 50% margin admits only that leg, so the
        // second LONG leg faces the intended decline independently.
        if (rejected_leg_ == RejectedLeg::SecondHeld) {
            margin_short_ = 50.0;
        }
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true);
        } else if (bar_index_ == 1) {
            strategy_entry("Short", false);
            strategy_entry("Long", true);
            strategy_close("Short");  // no live default-FIFO ledger
            strategy_close("Long");
            queued_count_ = pending_orders_.size();
        }
    }

    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
    std::size_t queued_count() const { return queued_count_; }

private:
    RejectedLeg rejected_leg_;
    std::size_t queued_count_ = 0;
};

void run_source_order_chain(bool source_long) {
    SourceOrderChain probe(source_long);
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    const std::string held = source_long ? "Long" : "Short";
    const std::string opposite = source_long ? "Short" : "Long";
    const std::vector<std::string> expected_ids = {
        opposite, held, "__close__" + held};
    const std::vector<OrderType> expected_types = {
        OrderType::MARKET, OrderType::MARKET, OrderType::EXIT};
    CHECK(probe.queued_ids() == expected_ids);
    CHECK(probe.queued_types() == expected_types);
    if (!source_long) {
        // Authoritative SHORT-seed tape: the ordinary broker order is
        // Long -> __close__Short -> Short. The middle object materializes a
        // second LONG lot; final Short closes both LONG lots and leaves flat.
        CHECK(probe.final_side() == PositionSide::FLAT);
        CHECK(std::fabs(probe.final_qty()) < 1e-9);
        CHECK(probe.trade_count() == 3);
        if (probe.trade_count() == 3) {
            const Trade& seed = probe.get_trade(0);
            const Trade& first_long = probe.get_trade(1);
            const Trade& close_short_long = probe.get_trade(2);
            CHECK(!seed.is_long);
            CHECK(seed.entry_id == "Short");
            CHECK(seed.exit_id == "Long");
            CHECK(seed.entry_time == 1'200'000);
            CHECK(seed.exit_time == 1'800'000);
            CHECK(std::fabs(seed.entry_price - 100.0) < 1e-9);
            CHECK(std::fabs(seed.exit_price - 100.0) < 1e-9);
            CHECK(std::fabs(seed.pnl) < 1e-9);
            CHECK(std::fabs(seed.commission) < 1e-9);
            CHECK(seed.entry_incarnation != 0);
            CHECK(first_long.is_long);
            CHECK(first_long.entry_id == "Long");
            CHECK(first_long.exit_id == "Short");
            CHECK(first_long.entry_time == 1'800'000);
            CHECK(first_long.exit_time == 1'800'000);
            CHECK(std::fabs(first_long.entry_price - 100.0) < 1e-9);
            CHECK(std::fabs(first_long.exit_price - 100.0) < 1e-9);
            CHECK(std::fabs(first_long.pnl) < 1e-9);
            CHECK(std::fabs(first_long.commission) < 1e-9);
            CHECK(first_long.entry_incarnation != 0);
            CHECK(close_short_long.is_long);
            CHECK(close_short_long.entry_id == "__close__Short");
            CHECK(close_short_long.exit_id == "Short");
            CHECK(close_short_long.entry_time == 1'800'000);
            CHECK(close_short_long.exit_time == 1'800'000);
            CHECK(std::fabs(close_short_long.entry_price - 100.0) < 1e-9);
            CHECK(std::fabs(close_short_long.exit_price - 100.0) < 1e-9);
            CHECK(std::fabs(close_short_long.pnl) < 1e-9);
            CHECK(std::fabs(close_short_long.commission) < 1e-9);
            CHECK(close_short_long.entry_incarnation != 0);
            CHECK(first_long.entry_incarnation
                  != close_short_long.entry_incarnation);
            CHECK(probe.reported_entry_incarnation(1)
                  == first_long.entry_incarnation);
            CHECK(probe.reported_entry_incarnation(2)
                  == close_short_long.entry_incarnation);
            CHECK(first_long.entry_bar_index == first_long.exit_bar_index);
            CHECK(close_short_long.entry_bar_index
                  == close_short_long.exit_bar_index);
            CHECK(std::fabs(first_long.qty - 1.0) < 1e-9);
            CHECK(std::fabs(close_short_long.qty - 1.0) < 1e-9);
        }
    } else {
        // No long-seed mirror is established. Keep the baseline source-order
        // lifecycle: Short reversal, stale close removal, Long reversal.
        CHECK(probe.final_side() == PositionSide::LONG);
        CHECK(std::fabs(probe.final_qty() - 1.0) < 1e-9);
        CHECK(probe.trade_count() == 2);
        if (probe.trade_count() == 2) {
            const Trade& seed = probe.get_trade(0);
            const Trade& short_lot = probe.get_trade(1);
            CHECK(seed.is_long);
            CHECK(seed.entry_id == "Long");
            CHECK(seed.exit_id == "Short");
            CHECK(!short_lot.is_long);
            CHECK(short_lot.entry_id == "Short");
            CHECK(short_lot.exit_id == "Long");
        }
    }
}

void run_same_direction_close_control(bool source_long) {
    SameDirectionCloseControl probe(source_long);
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.queued_count() == 2);
    CHECK(probe.final_side() ==
          (source_long ? PositionSide::LONG : PositionSide::SHORT));
    CHECK(std::fabs(std::fabs(probe.final_qty()) - 1.0) < 1e-9);
    CHECK(probe.trade_count() == 1);
}

void run_rejection_control(RejectedLeg rejected_leg) {
    RejectionControl probe(rejected_leg);
    Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1'000.0, 600'000},
        {100.0, 112.0, 99.0, 110.0, 1'000.0, 1'200'000},
        {111.0, 112.0, 110.0, 111.0, 1'000.0, 1'800'000},
        {111.0, 111.0, 111.0, 111.0, 1'000.0, 2'400'000},
    };
    probe.run(bars, 4);

    CHECK(probe.queued_count() == 3);
    if (rejected_leg == RejectedLeg::FirstOpposite) {
        // The first reversal decline leaves the seed LONG in place. The
        // second same-side attempt cannot add an all-in lot, and the paired
        // close is atomically suppressed by the existing decline rule.
        CHECK(probe.final_side() == PositionSide::LONG);
        CHECK(std::fabs(probe.final_qty() - 100.0) < 1e-9);
        CHECK(probe.trade_count() == 0);
    } else {
        // The 50%-margin SHORT reversal fills, but the second 100%-margin LONG
        // reversal declines at the same +1 gap. Since the side never returns
        // to the close's creation side, the exact-close bypass must stay off.
        CHECK(probe.final_side() == PositionSide::SHORT);
        CHECK(std::fabs(probe.final_qty() + 100.0) < 1e-9);
        CHECK(probe.trade_count() == 1);
    }
}

void run_empty_held_id_fail_closed(bool source_long) {
    class Probe final : public BacktestEngine {
    public:
        explicit Probe(bool source_long) : source_long_(source_long) {
            initial_capital_ = 1'000'000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 1;
            commission_value_ = 0.0;
            slippage_ = 0;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("", source_long_);
            } else if (bar_index_ == 1) {
                strategy_entry("Opposite", !source_long_);
                strategy_entry("", source_long_);
                strategy_close("Opposite");
                strategy_close("");  // close_all, never close(held-id)
                queued_count_ = pending_orders_.size();
            }
        }
        PositionSide final_side() const { return position_side_; }
        std::size_t queued_count() const { return queued_count_; }
    private:
        bool source_long_;
        std::size_t queued_count_ = 0;
    };

    Probe probe(source_long);
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.queued_count() == 3);
    CHECK(probe.final_side() ==
          (source_long ? PositionSide::LONG : PositionSide::SHORT));
    CHECK(probe.trade_count() == 2);
}

void run_mismatched_reentry_qty_fail_closed(bool source_long) {
    class Probe final : public BacktestEngine {
    public:
        explicit Probe(bool source_long) : source_long_(source_long) {
            initial_capital_ = 1'000'000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 1;
            commission_value_ = 0.0;
            slippage_ = 0;
        }
        void on_bar(const Bar&) override {
            const std::string held = source_long_ ? "Long" : "Short";
            const std::string opposite = source_long_ ? "Short" : "Long";
            if (bar_index_ == 0) {
                strategy_entry(held, source_long_, kNaN, kNaN, 1.0);
            } else if (bar_index_ == 1) {
                strategy_entry(opposite, !source_long_, kNaN, kNaN, 1.0);
                strategy_entry(held, source_long_, kNaN, kNaN, 2.0);
                strategy_close(opposite);
                strategy_close(held);
                queued_count_ = pending_orders_.size();
            }
        }
        PositionSide final_side() const { return position_side_; }
        double final_qty() const { return signed_position_size(); }
        std::size_t queued_count() const { return queued_count_; }
    private:
        bool source_long_;
        std::size_t queued_count_ = 0;
    };

    Probe probe(source_long);
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.queued_count() == 3);
    CHECK(probe.final_side() ==
          (source_long ? PositionSide::LONG : PositionSide::SHORT));
    CHECK(std::fabs(std::fabs(probe.final_qty()) - 2.0) < 1e-9);
    CHECK(probe.trade_count() == 2);
}

class StructuralIdProbe final : public BacktestEngine {
public:
    StructuralIdProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false);
        } else if (bar_index_ == 1) {
            strategy_entry("L", true);
            strategy_entry("S", false);
            strategy_close("L");
            strategy_close("S");
        }
    }

    PositionSide final_side() const { return position_side_; }
};

void run_structural_id_control() {
    StructuralIdProbe probe;
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.final_side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 3);
    if (probe.trade_count() == 3) {
        CHECK(!probe.get_trade(0).is_long);
        CHECK(probe.get_trade(0).entry_id == "S");
        CHECK(probe.get_trade(0).exit_id == "L");
        CHECK(probe.get_trade(1).is_long);
        CHECK(probe.get_trade(1).entry_id == "L");
        CHECK(probe.get_trade(1).exit_id == "S");
        CHECK(probe.get_trade(2).is_long);
        CHECK(probe.get_trade(2).entry_id == "__close__S");
        CHECK(probe.get_trade(2).exit_id == "S");
    }
}

void run_projected_final_admission_fail_closed() {
    class Probe final : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 3.0;
            pyramiding_ = 1;
            margin_long_ = 100.0;
            margin_short_ = 100.0;
            commission_value_ = 0.0;
            slippage_ = 0;
        }

        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("Short", false);
            } else if (bar_index_ == 1) {
                strategy_entry("Long", true);
                strategy_entry("Short", false);
                strategy_close("Long");  // no live default-FIFO ledger
                strategy_close("Short");
            }
        }

        PositionSide final_side() const { return position_side_; }
        double final_qty() const { return signed_position_size(); }
        bool has_open_materialized_lot() const {
            for (const PyramidEntry& entry : pyramid_entries_) {
                if (entry.entry_id == "__close__Short") return true;
            }
            return false;
        }
    };

    Probe probe;
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    // Materializing Long3 would leave Long6 before the final Short. Its normal
    // admission requires $900 against only $400 of projected free funds, so
    // the special transaction must fail closed to the ordinary Short3 result.
    CHECK(probe.final_side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.final_qty() + 3.0) < 1e-9);
    CHECK(probe.trade_count() == 2);
    CHECK(!probe.has_open_materialized_lot());
    for (int i = 0; i < probe.trade_count(); ++i) {
        CHECK(probe.get_trade(i).entry_id != "__close__Short");
    }
}

void run_partial_close_fragments_share_entry_incarnation() {
    class Probe final : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 2.0;
            pyramiding_ = 1;
            commission_value_ = 0.0;
            slippage_ = 0;
        }

        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("L", true);
            } else if (bar_index_ == 1) {
                strategy_close("L", "half", 1.0);
            } else if (bar_index_ == 2) {
                strategy_close("L");
            }
        }
    };

    Probe probe;
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() == 2) {
        const Trade& first = probe.get_trade(0);
        const Trade& second = probe.get_trade(1);
        CHECK(first.entry_incarnation != 0);
        CHECK(second.entry_incarnation == first.entry_incarnation);
        CHECK(std::fabs(first.qty - 1.0) < 1e-9);
        CHECK(std::fabs(second.qty - 1.0) < 1e-9);
    }
}

void run_internal_close_id_collision_fail_closed() {
    class Probe final : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            pyramiding_ = 1;
            commission_value_ = 0.0;
            slippage_ = 0;
        }

        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("Short", false);
            } else if (bar_index_ == 1) {
                // A user entry may legally occupy the engine's synthesized
                // close-id namespace. It must not become indistinguishable
                // from the physical close transaction.
                strategy_entry("__close__Short", true);
                strategy_entry("Short", false);
                strategy_close("__close__Short");
                strategy_close("Short");
            }
        }

        PositionSide final_side() const { return position_side_; }
        double final_qty() const { return signed_position_size(); }
    };

    Probe probe;
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.final_side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.final_qty() + 1.0) < 1e-9);
    CHECK(probe.trade_count() == 2);
}

enum class GateControl {
    AnyCloseRule,
    ProcessOnClose,
    CalcOnFills,
    Magnifier,
    ExtraObject,
    RejectedExtraCall,
    PartialClose,
    PricedEntry,
    SameIdReplacement,
    NonconsecutiveSequence,
    NonzeroSlippage,
    NonzeroCommission,
};

class GateControlProbe final : public BacktestEngine {
public:
    explicit GateControlProbe(GateControl control) : control_(control) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
        switch (control_) {
            case GateControl::AnyCloseRule:
                close_entries_rule_any_ = true;
                break;
            case GateControl::ProcessOnClose:
                process_orders_on_close_ = true;
                break;
            case GateControl::CalcOnFills:
                calc_on_order_fills_ = true;
                break;
            case GateControl::Magnifier:
                bar_magnifier_enabled_ = true;
                break;
            case GateControl::NonzeroSlippage:
                slippage_ = 1;
                syminfo_mintick_ = 0.01;
                break;
            case GateControl::NonzeroCommission:
                commission_type_ = CommissionType::PERCENT;
                commission_value_ = 0.1;
                break;
            default:
                break;
        }
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && !seed_issued_) {
            seed_issued_ = true;
            strategy_entry("Short", false);
            return;
        }
        if (bar_index_ != 1 || signal_issued_) return;
        signal_issued_ = true;

        if (control_ == GateControl::RejectedExtraCall) {
            // Signal-time margin rejection: no PendingOrder/incarnation remains,
            // so the source-bar rejection tombstone is the only proof this was
            // not the exact three-call book.
            strategy_entry("Rejected", true, kNaN, kNaN, 1'000'000.0);
        }

        strategy_entry("Long", true,
                       kNaN,
                       control_ == GateControl::PricedEntry ? 100.0 : kNaN);

        if (control_ == GateControl::SameIdReplacement) {
            strategy_entry("Long", true);
        } else if (control_ == GateControl::NonconsecutiveSequence) {
            strategy_entry("Gap", true);
            strategy_cancel("Gap");
        }

        strategy_entry("Short", false);
        if (control_ == GateControl::ExtraObject) {
            strategy_entry("ExtraLong", true);
        }
        strategy_close("Long");
        if (control_ == GateControl::PartialClose) {
            strategy_close("Short", "", kNaN, 50.0);
        } else {
            strategy_close("Short");
        }
        queued_count_ = pending_orders_.size();
    }

    std::size_t queued_count() const { return queued_count_; }
    bool has_materialized_close_lot() const {
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).entry_id == "__close__Short") return true;
        }
        return false;
    }

private:
    GateControl control_;
    bool seed_issued_ = false;
    bool signal_issued_ = false;
    std::size_t queued_count_ = 0;
};

void run_gate_control(GateControl control) {
    GateControlProbe probe(control);
    Bar bars[] = {
        flat_bar(600'000),
        flat_bar(1'200'000),
        flat_bar(1'800'000),
        flat_bar(2'400'000),
    };
    if (control == GateControl::Magnifier) {
        probe.run(bars, 4, "1", "1", /*bar_magnifier=*/true, 4,
                  MagnifierDistribution::ENDPOINTS);
    } else {
        probe.run(bars, 4);
    }

    const std::size_t expected_queued = control == GateControl::ExtraObject
        ? 4U
        : (control == GateControl::ProcessOnClose ? 2U : 3U);
    if (probe.queued_count() != expected_queued
        || probe.has_materialized_close_lot()) {
        std::fprintf(stderr,
                     "gate control %d: queued=%zu expected=%zu materialized=%d\n",
                     static_cast<int>(control), probe.queued_count(),
                     expected_queued,
                     probe.has_materialized_close_lot() ? 1 : 0);
    }
    CHECK(probe.queued_count() == expected_queued);
    CHECK(!probe.has_materialized_close_lot());
}

}  // namespace

int main() {
    run_source_order_chain(false);
    run_source_order_chain(true);
    run_same_direction_close_control(false);
    run_same_direction_close_control(true);
    run_rejection_control(RejectedLeg::FirstOpposite);
    run_rejection_control(RejectedLeg::SecondHeld);
    run_empty_held_id_fail_closed(false);
    run_empty_held_id_fail_closed(true);
    run_mismatched_reentry_qty_fail_closed(false);
    run_mismatched_reentry_qty_fail_closed(true);
    run_structural_id_control();
    run_projected_final_admission_fail_closed();
    run_partial_close_fragments_share_entry_incarnation();
    run_internal_close_id_collision_fail_closed();
    run_gate_control(GateControl::AnyCloseRule);
    run_gate_control(GateControl::ProcessOnClose);
    run_gate_control(GateControl::CalcOnFills);
    run_gate_control(GateControl::Magnifier);
    run_gate_control(GateControl::ExtraObject);
    run_gate_control(GateControl::RejectedExtraCall);
    run_gate_control(GateControl::PartialClose);
    run_gate_control(GateControl::PricedEntry);
    run_gate_control(GateControl::SameIdReplacement);
    run_gate_control(GateControl::NonconsecutiveSequence);
    run_gate_control(GateControl::NonzeroSlippage);
    run_gate_control(GateControl::NonzeroCommission);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
