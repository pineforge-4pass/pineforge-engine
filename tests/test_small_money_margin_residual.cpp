// A margin-100 long can have a real money-rounding deficit smaller than
// the runtime's former absolute 1e-7 representation guard. This compact
// broker fixture uses a four-bar ordinary market entry and next-open close.
// Oracle controls and source/CSV hashes live in the campaign discovery state.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0;
int failed = 0;
#define CHECK(value) do { \
    if (value) ++passed; \
    else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } \
} while (false)

constexpr double kQuantity = 891538.56;
Bar bar(int i, double open, double high, double low, double close) {
    Bar out;
    out.timestamp = 1749754800000LL + i * 900000LL;
    out.open = open; out.high = high; out.low = low; out.close = close;
    out.volume = 1.0;
    return out;
}
std::vector<Bar> bars() {
    return {
        bar(0, 1.15776, 1.15798, 1.15754, 1.15798),
        bar(1, 1.15798, 1.15808, 1.15760, 1.15761),
        bar(2, 1.15762, 1.15798, 1.15748, 1.15788),
        bar(3, 1.15788, 1.15804, 1.15762, 1.15762),
    };
}
class ResidualProbe : public pineforge::source::PineStrategyHost {
public:
    ResidualProbe(double capital, bool enabled = true, double realized = 0.0,
                  double quantity = kQuantity, bool unbounded = false)
        : realized_(realized), quantity_(quantity), unbounded_(unbounded) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin_short_ = 100.0;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
        slippage_ = 0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(0.00001);
        qty_step_ = 0.01;
        set_margin_call_enabled(enabled);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            // Initialize equivalent closed ledgers before any opening order.
            // The synthetic prior PnL isolates numerical representation from
            // the broker decisions that produced it.
            net_profit_sum_ = realized_;
            if (unbounded_)
                net_profit_roundoff_bound_ = std::numeric_limits<double>::infinity();
            const double na = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("L", true, na, na, quantity_);
        }
        if (bar_index_ == 1) strategy_close("L", "survivor");
    }
    const std::vector<Trade>& closed() const { return trades_; }
private:
    double realized_;
    double quantity_;
    bool unbounded_;
};

void check_capital(double capital, bool expects_call, bool enabled = true,
                   double realized = 0.0, bool unbounded = false) {
    ResidualProbe engine(capital, enabled, realized, kQuantity, unbounded);
    const auto input = bars();
    engine.run(input.data(), static_cast<int>(input.size()));
    const auto& closed = engine.closed();
    std::printf("capital %.10f enabled %d: %zu trades\n", capital, enabled, closed.size());
    CHECK(closed.size() == (expects_call ? 2U : 1U));
    if (closed.size() != (expects_call ? 2U : 1U)) return;
    if (expects_call) {
        const auto& call = closed[0];
        CHECK(call.exit_comment == "Margin call");
        CHECK(std::abs(call.qty - 1.0) < 1e-9);
        CHECK(call.entry_time == input[1].timestamp);
        CHECK(call.exit_time == input[1].timestamp);
        CHECK(std::abs(call.entry_price - 1.15798) < 1e-12);
        CHECK(std::abs(call.exit_price - 1.15808) < 1e-12);
    }
    const auto& survivor = closed.back();
    CHECK(survivor.exit_comment == "survivor");
    CHECK(survivor.exit_time == input[2].timestamp);
    CHECK(std::abs(survivor.qty - (kQuantity - (expects_call ? 1.0 : 0.0))) < 1e-6);
    CHECK(std::abs(survivor.exit_price - 1.15762) < 1e-12);
}

// Exercise the actual realized-PnL writer with three exact binary64 trade
// profits. The small middle term is lost by the existing naive accumulator;
// its uncertainty must still protect a later exact-money tie. The live
// position is initialized after those trades to isolate the margin checkpoint
// from entry admission, which is a different broker contract.
class HistoryProbe : public pineforge::source::PineStrategyHost {
public:
    bool with_history = true;
    HistoryProbe() {
        initial_capital_ = 1024.0 - std::ldexp(1.0, -24);
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 0;
        margin_long_ = margin_short_ = 100.0;
        syminfo_.pointvalue = 1.0;
        set_syminfo_mintick(std::ldexp(1.0, -24));
        qty_step_ = std::ldexp(1.0, -16);
    }
    void on_source_bar(const Bar& current) override {
        if (bar_index_ == 0) set_margin_call_enabled(false);
        if (with_history) {
            const double na = std::numeric_limits<double>::quiet_NaN();
            if (bar_index_ == 0 || bar_index_ == 2)
                strategy_entry("H", true, na, na, 1.0);
            if (bar_index_ == 4)
                strategy_entry("H", false, na, na, 1.0);
            if (bar_index_ == 1 || bar_index_ == 3 || bar_index_ == 5)
                strategy_close("H", "history");
        }
        if (bar_index_ == 6) {
            const double price = 1024.0 - std::ldexp(1.0, -21);
            position_side_ = PositionSide::LONG;
            position_qty_ = 1.0;
            position_entry_count_ = 1;
            position_entry_price_ = price;
            position_entry_time_ = current.timestamp;
            position_open_bar_ = bar_index_;
            PyramidEntry entry{};
            entry.price = price;
            entry.qty = 1.0;
            entry.time = current.timestamp;
            entry.entry_id = "current";
            entry.entry_bar_index = bar_index_;
            entry.entry_commission_account = 0.0;
            pyramid_entries_.push_back(entry);
            set_margin_call_enabled(true);
        }
    }
    int margin_calls() const {
        int count = 0;
        for (const auto& trade : trades_)
            count += trade.exit_comment == "Margin call";
        return count;
    }
    double position() const { return signed_position_size(); }
    using BacktestEngine::net_profit;
};
void check_history_tie_and_reset() {
    const double big = std::ldexp(1.0, 30);
    const double small = std::ldexp(1.0, -24);
    const double price = 1024.0 - std::ldexp(1.0, -21);
    std::vector<Bar> input;
    for (double p : {1.0, 1.0, 1.0 + big, 1.0, 1.0 + small,
                     1.0, 1.0 + big, price, price})
        input.push_back(bar(static_cast<int>(input.size()), p, p, p, p));
    HistoryProbe engine;
    engine.run(input.data(), static_cast<int>(input.size()));
    CHECK(engine.net_profit() == 0.0);  // preserve the existing financial sum
    CHECK(engine.trade_count() == 3);
    CHECK(engine.margin_calls() == 0);
    CHECK(engine.position() == 1.0);

    // A new run has no vanished positive realized term, hence this same
    // current-capital value has a real deficit. A stale error bound would
    // falsely hide the call; reset must clear numerical provenance too.
    engine.with_history = false;
    engine.run(input.data(), static_cast<int>(input.size()));
    CHECK(engine.trade_count() == 1);
    CHECK(engine.margin_calls() == 1);
    CHECK(engine.position() == 0.0);
}
void check_high_money_preserves_previous_boundary() {
    const auto input = bars();
    ResidualProbe deficit(103238382.21439985, true, 0.0, kQuantity * 100.0);
    deficit.run(input.data(), static_cast<int>(input.size()));
    CHECK(deficit.closed().size() == 2);
    if (deficit.closed().size() == 2) {
        CHECK(deficit.closed()[0].exit_comment == "Margin call");
        CHECK(deficit.closed()[0].qty == 1.0);
        CHECK(std::abs(deficit.closed()[0].exit_price - 1.15808) < 1e-12);
    }
    ResidualProbe funded(103238382.2144001, true, 0.0, kQuantity * 100.0);
    funded.run(input.data(), static_cast<int>(input.size()));
    CHECK(funded.closed().size() == 1);
    if (funded.closed().size() == 1)
        CHECK(funded.closed()[0].exit_comment == "survivor");
}

class OrdinaryHistoryProbe : public ResidualProbe {
    bool injected_;
public:
    explicit OrdinaryHistoryProbe(bool injected)
        : ResidualProbe(1032383.8221439 - 0.25 - (injected ? 100.0 : 0.0)),
          injected_(injected) {}
    void on_source_bar(const Bar&) override {
        const double na = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) {
            if (injected_) net_profit_sum_ = 100.0;
            strategy_entry("H", true, na, na, 1.0);
        }
        if (bar_index_ == 1) strategy_close("H", "ordinary history");
        if (bar_index_ == 2) strategy_entry("L", true, na, na, kQuantity);
        if (bar_index_ == 3) strategy_close("L", "survivor");
    }
};
void check_ordinary_and_untracked_history() {
    const std::vector<Bar> input = {
        bar(0, 1.0, 1.0, 1.0, 1.0),
        bar(1, 1.0, 1.0, 1.0, 1.0),
        bar(2, 1.25, 1.25, 1.15798, 1.15798),
        bar(3, 1.15798, 1.15808, 1.15760, 1.15761),
        bar(4, 1.15762, 1.15798, 1.15748, 1.15788),
    };
    for (bool injected : {false, true}) {
        OrdinaryHistoryProbe engine(injected);
        engine.run(input.data(), static_cast<int>(input.size()));
        const auto& closed = engine.closed();
        CHECK(closed.size() == (injected ? 2U : 3U));
        if (closed.size() != (injected ? 2U : 3U)) continue;
        CHECK(closed.front().pnl == 0.25);
        CHECK(closed.front().exit_comment == "ordinary history");
        if (!injected) {
            CHECK(closed[1].exit_comment == "Margin call");
            CHECK(closed[1].qty == 1.0);
            CHECK(std::abs(closed[1].exit_price - 1.15808) < 1e-12);
        }
        CHECK(closed.back().exit_comment == "survivor");
    }
}
}
int main() {
    check_capital(1032383.8221439, true);   // real 1e-7 deficit at the high
    check_capital(1032383.8221438, true);   // 2e-7 deficit
    check_capital(1032383.8221440, false);  // exact mathematical tie
    check_capital(1032383.8221441, false);  // positive coverage
    check_capital(1032383.8221449, false);  // wider positive coverage
    check_capital(1032383.8221439, false, false);

    // Equal current equity must make the same decision whether it is the
    // initial balance or follows a large realized loss. Scaling roundoff by
    // raw historical capital incorrectly suppressed the split-ledger call.
    const double initial = 1e9;
    const double loss = 1032383.8221436 - initial;
    check_capital(initial + loss, true);
    check_capital(initial, true, true, loss);
    const double funded_loss = 1032383.8221445 - initial;
    check_capital(initial + funded_loss, false);
    check_capital(initial, false, true, funded_loss);
    check_history_tie_and_reset();
    check_high_money_preserves_previous_boundary();
    check_ordinary_and_untracked_history();
    check_capital(1032383.8221439 - 1.0, false, true, 1.0);
    check_capital(1032383.8221439, false, true, 0.0, true);
    std::printf("small money residual: %d passed / %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
