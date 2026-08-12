/*
 * Regression coverage for the SHORT-seed default-FIFO close collision under
 * frozen PERCENT_OF_EQUITY / CASH default sizing (finding 272).
 *
 * TV rule (25/25 exact on the alpha-forge-liquidity-matrix-v2 tape): with a
 * SHORT seed of qty S entered on an earlier bar and the exact same-bar book
 *   entry(Long); entry(Short); close(Long)[no-op]; close(Short)[frozen S]
 * all filling at the next open P, TV emits: (1) the old short S exits via
 * order 'Long'; (2) a zero-PnL dur-0 LONG round trip qty L (the frozen
 * default qty), 'Long' -> 'Short'; (3) a second zero-PnL dur-0 LONG round
 * trip qty min(S, L), '__close__Short' -> 'Short'; (4) the end-of-bar
 * position is SHORT max(0, L - S) under id 'Short' (flat when L <= S), and
 * the real opposite entry is NOT queued — the strategy resumes ordinary
 * signal processing from that position.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

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

Bar make_bar(double open, double high, double low, double close,
             int64_t timestamp) {
    return {open, high, low, close, 1'000.0, timestamp};
}

// Percent-of-equity remnant case (L > S): the seed short profits before the
// collision bar, so the frozen default qty L exceeds the seed S and the final
// Short must re-open exactly the surplus L - S. A later strategy.close on the
// remnant proves the ledger / id / incarnation provenance of the re-opened
// lot.
class PercentRemnantProbe final : public BacktestEngine {
public:
    PercentRemnantProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            CHECK(position_side_ == PositionSide::SHORT);
            CHECK(pyramid_entries_.size() == 1);
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");  // no live default-FIFO ledger -> no-op
            strategy_close("Short");
        } else if (bar_index_ == 2) {
            // The deferred real short is NOT a queued order: the episode
            // consumed the whole book and left the remnant as an ordinary
            // open position.
            pending_after_collision_ = pending_orders_.size();
            side_after_collision_ = position_side_;
            qty_after_collision_ = signed_position_size();
            remnant_entry_id_ = pyramid_entries_.size() == 1
                ? pyramid_entries_[0].entry_id
                : std::string();
            strategy_close("Short");
        }
    }

    std::size_t pending_after_collision_ = 999;
    PositionSide side_after_collision_ = PositionSide::FLAT;
    double qty_after_collision_ = kNaN;
    std::string remnant_entry_id_;
    PositionSide final_side() const { return position_side_; }
};

void run_percent_remnant_case() {
    PercentRemnantProbe probe;
    Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 89.5, 90.0, 1'200'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 1'800'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 2'400'000),
        make_bar(90.0, 90.0, 90.0, 90.0, 3'000'000),
    };
    probe.run(bars, 5);

    // Frozen sizing, mirrored with the engine's operation order:
    // S at bar0 close (flat): (1e6 * 10%) / 100 = 1000 exactly.
    // L at bar1 close: equity = 1e6 + 1000*(100-90) = 1'010'000,
    // L = (1'010'000 * 10%) / 90.
    const double kSeedQty = 1'000.0;
    const double kL = (1'010'000.0 * (10.0 / 100.0)) / 90.0;
    const double kResidual = kL - kSeedQty;
    CHECK(kL > kSeedQty);  // test-shape sanity

    // Same-bar outcome: SHORT remnant of exactly L - S, no pending orders.
    CHECK(probe.pending_after_collision_ == 0);
    CHECK(probe.side_after_collision_ == PositionSide::SHORT);
    CHECK(std::fabs(probe.qty_after_collision_ + kResidual) < 1e-6);
    CHECK(probe.remnant_entry_id_ == "Short");
    CHECK(probe.final_side() == PositionSide::FLAT);

    CHECK(probe.trade_count() == 4);
    if (probe.trade_count() == 4) {
        const Trade& seed = probe.get_trade(0);
        const Trade& zero1 = probe.get_trade(1);
        const Trade& zero2 = probe.get_trade(2);
        const Trade& remnant = probe.get_trade(3);

        // (1) Old short S exits at P via order 'Long'.
        CHECK(!seed.is_long);
        CHECK(seed.entry_id == "Short");
        CHECK(seed.exit_id == "Long");
        CHECK(seed.entry_time == 1'200'000);
        CHECK(seed.exit_time == 1'800'000);
        CHECK(std::fabs(seed.qty - kSeedQty) < 1e-6);
        CHECK(std::fabs(seed.entry_price - 100.0) < 1e-9);
        CHECK(std::fabs(seed.exit_price - 90.0) < 1e-9);
        CHECK(std::fabs(seed.pnl - 10'000.0) < 1e-6);

        // (2) Zero-PnL dur-0 LONG round trip qty L, 'Long' -> 'Short'.
        CHECK(zero1.is_long);
        CHECK(zero1.entry_id == "Long");
        CHECK(zero1.exit_id == "Short");
        CHECK(zero1.entry_time == 1'800'000);
        CHECK(zero1.exit_time == 1'800'000);
        CHECK(zero1.entry_bar_index == zero1.exit_bar_index);
        CHECK(std::fabs(zero1.qty - kL) < 1e-6);
        CHECK(std::fabs(zero1.entry_price - 90.0) < 1e-9);
        CHECK(std::fabs(zero1.exit_price - 90.0) < 1e-9);
        CHECK(std::fabs(zero1.pnl) < 1e-9);

        // (3) Second zero-PnL dur-0 LONG round trip qty min(S, L),
        //     '__close__Short' -> 'Short'.
        CHECK(zero2.is_long);
        CHECK(zero2.entry_id == "__close__Short");
        CHECK(zero2.exit_id == "Short");
        CHECK(zero2.entry_time == 1'800'000);
        CHECK(zero2.exit_time == 1'800'000);
        CHECK(std::fabs(zero2.qty - kSeedQty) < 1e-6);  // min(S, L) == S here
        CHECK(std::fabs(zero2.pnl) < 1e-9);

        // (4) The remnant lot carries the final Short's id/incarnation and
        //     entered at the collision fill; the later close resolves it via
        //     the ordinary ledger.
        CHECK(!remnant.is_long);
        CHECK(remnant.entry_id == "Short");
        CHECK(remnant.exit_id == "__close__Short");
        CHECK(remnant.entry_time == 1'800'000);
        CHECK(remnant.exit_time == 2'400'000);
        CHECK(std::fabs(remnant.qty - kResidual) < 1e-6);
        CHECK(std::fabs(remnant.entry_price - 90.0) < 1e-9);
        CHECK(std::fabs(remnant.pnl) < 1e-9);

        // Physical provenance: the three collision objects carry consecutive
        // incarnations Long -> Short -> __close__Short; the remnant lot is
        // the final Short order's own incarnation.
        CHECK(zero1.entry_incarnation != 0);
        CHECK(remnant.entry_incarnation == zero1.entry_incarnation + 1);
        CHECK(zero2.entry_incarnation == zero1.entry_incarnation + 2);
    }
}

// Percent-of-equity flat case (L <= S): the seed short is underwater on the
// collision bar, the frozen default qty L is below the seed S, the second
// zero trade is min(S, L) == L, and the episode ends FLAT with no same-bar
// short.
class PercentFlatProbe final : public BacktestEngine {
public:
    PercentFlatProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        } else if (bar_index_ == 2) {
            pending_after_collision_ = pending_orders_.size();
            side_after_collision_ = position_side_;
        }
    }

    std::size_t pending_after_collision_ = 999;
    PositionSide side_after_collision_ = PositionSide::SHORT;
    PositionSide final_side() const { return position_side_; }
};

void run_percent_flat_case() {
    PercentFlatProbe probe;
    Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 110.5, 99.5, 110.0, 1'200'000),
        make_bar(110.0, 110.5, 109.5, 110.0, 1'800'000),
        make_bar(110.0, 110.0, 110.0, 110.0, 2'400'000),
    };
    probe.run(bars, 4);

    // S = 1000; equity at bar1 close = 1e6 + 1000*(100-110) = 990'000;
    // L = (990'000 * 10%) / 110 = 900 exactly. L < S -> flat episode.
    const double kSeedQty = 1'000.0;
    const double kL = (990'000.0 * (10.0 / 100.0)) / 110.0;
    CHECK(kL < kSeedQty);  // test-shape sanity

    CHECK(probe.pending_after_collision_ == 0);
    CHECK(probe.side_after_collision_ == PositionSide::FLAT);
    CHECK(probe.final_side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 3);
    if (probe.trade_count() == 3) {
        const Trade& seed = probe.get_trade(0);
        const Trade& zero1 = probe.get_trade(1);
        const Trade& zero2 = probe.get_trade(2);
        CHECK(!seed.is_long);
        CHECK(seed.entry_id == "Short");
        CHECK(seed.exit_id == "Long");
        CHECK(std::fabs(seed.qty - kSeedQty) < 1e-6);
        CHECK(std::fabs(seed.pnl + 10'000.0) < 1e-6);
        CHECK(zero1.is_long);
        CHECK(zero1.entry_id == "Long");
        CHECK(zero1.exit_id == "Short");
        CHECK(std::fabs(zero1.qty - kL) < 1e-6);
        CHECK(std::fabs(zero1.pnl) < 1e-9);
        CHECK(zero2.is_long);
        CHECK(zero2.entry_id == "__close__Short");
        CHECK(zero2.exit_id == "Short");
        // min(S, L) == L in the flat regime.
        CHECK(std::fabs(zero2.qty - kL) < 1e-6);
        CHECK(std::fabs(zero2.pnl) < 1e-9);
    }
}

// CASH default sizing follows the same frozen-snapshot collision shape.
class CashRemnantProbe final : public BacktestEngine {
public:
    CashRemnantProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::CASH;
        default_qty_value_ = 100'000.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
    }

    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
};

void run_cash_remnant_case() {
    CashRemnantProbe probe;
    Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 89.5, 90.0, 1'200'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 1'800'000),
        make_bar(90.0, 90.0, 90.0, 90.0, 2'400'000),
    };
    probe.run(bars, 4);

    // S = 100'000/100 = 1000; L = 100'000/90; residual = L - S.
    const double kSeedQty = 1'000.0;
    const double kL = 100'000.0 / 90.0;
    CHECK(probe.final_side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.final_qty() + (kL - kSeedQty)) < 1e-6);
    CHECK(probe.trade_count() == 3);
    if (probe.trade_count() == 3) {
        CHECK(probe.get_trade(1).is_long);
        CHECK(probe.get_trade(1).entry_id == "Long");
        CHECK(std::fabs(probe.get_trade(1).qty - kL) < 1e-6);
        CHECK(std::fabs(probe.get_trade(1).pnl) < 1e-9);
        CHECK(probe.get_trade(2).is_long);
        CHECK(probe.get_trade(2).entry_id == "__close__Short");
        CHECK(std::fabs(probe.get_trade(2).qty - kSeedQty) < 1e-6);
        CHECK(std::fabs(probe.get_trade(2).pnl) < 1e-9);
    }
}

// Non-trigger control: an all-in (100%) book whose reversal legs face a
// gap-up decline must NOT be tagged — the projection mirrors the KI-54
// frozen reversal re-check, and the ordinary path's atomic decline
// (entry declined, co-queued close suppressed, same-direction re-add
// declined) is preserved byte-for-byte.
class PercentGapDeclineControl final : public BacktestEngine {
public:
    PercentGapDeclineControl() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
        margin_call_enabled_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        } else if (bar_index_ == 2) {
            pending_after_collision_ = pending_orders_.size();
        }
    }

    std::size_t pending_after_collision_ = 999;
    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
    bool has_materialized_close_trade() const {
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).entry_id == "__close__Short") return true;
        }
        return false;
    }
};

void run_percent_gap_decline_control() {
    PercentGapDeclineControl probe;
    Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 99.5, 100.0, 1'200'000),
        // Gap-up fill bar: frozen L*open = 1e6*101/100 > sizing equity 1e6.
        make_bar(101.0, 101.0, 100.5, 101.0, 1'800'000),
        make_bar(101.0, 101.0, 101.0, 101.0, 2'400'000),
    };
    probe.run(bars, 4);

    CHECK(probe.pending_after_collision_ == 0);
    CHECK(probe.final_side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.final_qty() + 10'000.0) < 1e-6);
    CHECK(probe.trade_count() == 0);
    CHECK(!probe.has_materialized_close_trade());
}

// Non-trigger control: a PARTIAL close(held) breaks the exact three-object
// book under percent sizing exactly as it does for the FIXED cohort — the
// stale close is removed and the engine keeps its ordinary two-reversal
// outcome with a full-size short.
class PercentPartialCloseControl final : public BacktestEngine {
public:
    PercentPartialCloseControl() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;
        pyramiding_ = 1;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false);
        } else if (bar_index_ == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short", "", kNaN, 50.0);
        }
    }

    PositionSide final_side() const { return position_side_; }
    double final_qty() const { return signed_position_size(); }
    bool has_materialized_close_trade() const {
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).entry_id == "__close__Short") return true;
        }
        return false;
    }
};

void run_percent_partial_close_control() {
    PercentPartialCloseControl probe;
    Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 89.5, 90.0, 1'200'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 1'800'000),
        make_bar(90.0, 90.0, 90.0, 90.0, 2'400'000),
    };
    probe.run(bars, 4);

    const double kL = (1'010'000.0 * (10.0 / 100.0)) / 90.0;
    CHECK(probe.final_side() == PositionSide::SHORT);
    CHECK(std::fabs(probe.final_qty() + kL) < 1e-6);
    CHECK(probe.trade_count() == 2);
    CHECK(!probe.has_materialized_close_trade());
}

}  // namespace

int main() {
    run_percent_remnant_case();
    run_percent_flat_case();
    run_cash_remnant_case();
    run_percent_gap_decline_control();
    run_percent_partial_close_control();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
