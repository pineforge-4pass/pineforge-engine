/*
 * Production default MARKET/MARKET gross admission.
 *
 * At the canonical range start, both stop expressions are na, so Long then
 * Short arrive as omitted-qty MARKET strategy.entry calls. With default
 * percent_of_equity=100, each freezes one account-equity lot. TradingView
 * keeps Long because the later Short's gross transaction is ~200% of equity.
 *
 * These tests pin the production behavior and its deliberate non-target scope.
 */

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN =
    std::numeric_limits<double>::quiet_NaN();

static bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}


static Bar flat_bar(double price, int64_t timestamp) {
    Bar bar;
    bar.open = price;
    bar.high = price;
    bar.low = price;
    bar.close = price;
    bar.volume = 1000.0;
    bar.timestamp = timestamp;
    return bar;
}

struct Snapshot {
    double signed_position = 0.0;
    int trades = 0;
    std::string pending_book;
    std::string trade_book;
};

static void check_snapshot(const Snapshot& actual, double signed_position,
                           int trades, const char* pending_book,
                           const char* trade_book) {
    if (!near(actual.signed_position, signed_position)) {
        std::printf("  snapshot position actual=%.17g expected=%.17g\n",
                    actual.signed_position, signed_position);
    }
    CHECK(near(actual.signed_position, signed_position));
    CHECK(actual.trades == trades);
    CHECK(actual.pending_book == pending_book);
    CHECK(actual.trade_book == trade_book);
}

enum class Shape {
    OPPOSITE,
    SAME_ID,
    SAME_DIRECTION,
    THREE_CALLS,
    REPLACEMENT,
    OCA,
    PRICED_THIRD,
    RAW_THIRD,
    CANCELED_THIRD,
};

struct Probe : public BacktestEngine {
    explicit Probe(Shape shape = Shape::OPPOSITE) : shape_(shape) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        pyramiding_ = 1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        qty_step_ = 0.0;
        set_margin_call_enabled(false);
    }

    Shape shape_;
    bool placed_ = false;
    size_t queued_after_signal = 0;
    int candidates_after_signal = 0;
    int replacements_after_signal = 0;
    Snapshot after_signal;
    Snapshot after_fill;

    static const char* order_type_name(OrderType type) {
        switch (type) {
            case OrderType::MARKET: return "M";
            case OrderType::ENTRY: return "E";
            case OrderType::EXIT: return "X";
            case OrderType::RAW_ORDER: return "R";
        }
        return "?";
    }

    static std::string number(double value) {
        if (std::isnan(value)) return "na";
        std::ostringstream out;
        out << std::fixed << std::setprecision(4) << value;
        return out.str();
    }

    Snapshot snapshot() const {
        Snapshot result;
        result.signed_position = signed_position_size();
        result.trades = trade_count();
        std::ostringstream orders;
        orders << "[";
        for (size_t i = 0; i < pending_orders_.size(); ++i) {
            if (i != 0) orders << ",";
            const PendingOrder& order = pending_orders_[i];
            orders << order.id << ":" << order_type_name(order.type)
                   << ":" << (order.is_long ? "L" : "S")
                   << ":q=" << number(order.qty)
                   << ":l=" << number(order.limit_price)
                   << ":s=" << number(order.stop_price)
                   << ":o=" << (order.oca_name.empty() ? "-" : order.oca_name)
                   << "/" << order.oca_type
                   << ":c=" << order.default_flat_market_gross_candidate
                   << ":r=" << order.created_by_same_id_replacement;
        }
        orders << "]";
        result.pending_book = orders.str();

        std::ostringstream trades;
        trades << "[";
        for (size_t i = 0; i < trades_.size(); ++i) {
            if (i != 0) trades << ",";
            const Trade& trade = trades_[i];
            trades << (trade.is_long ? "L" : "S")
                   << ":" << trade.entry_id << ">" << trade.exit_id
                   << ":" << number(trade.entry_price)
                   << ">" << number(trade.exit_price)
                   << ":q=" << number(trade.qty)
                   << ":p=" << number(trade.pnl);
        }
        trades << "]";
        result.trade_book = trades.str();
        return result;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && !placed_) {
            placed_ = true;
            switch (shape_) {
                case Shape::OPPOSITE:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    break;
                case Shape::SAME_ID:
                    strategy_entry("Same", true);
                    strategy_entry("Same", false);
                    break;
                case Shape::SAME_DIRECTION:
                    strategy_entry("Long-1", true);
                    strategy_entry("Long-2", true);
                    break;
                case Shape::THREE_CALLS:
                    strategy_entry("Long-1", true);
                    strategy_entry("Short-2", false);
                    strategy_entry("Long-3", true);
                    break;
                case Shape::REPLACEMENT:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_entry("Short", false);
                    break;
                case Shape::OCA:
                    strategy_entry("Long", true, kNaN, kNaN, kNaN,
                                   "", "G", 1);
                    strategy_entry("Short", false, kNaN, kNaN, kNaN,
                                   "", "G", 1);
                    break;
                case Shape::PRICED_THIRD:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_entry("Priced", true, kNaN, 200.0, 1.0);
                    break;
                case Shape::RAW_THIRD:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_order("Raw", true, 1.0);
                    break;
                case Shape::CANCELED_THIRD:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_entry("Third", true);
                    strategy_cancel("Third");
                    break;
            }
            queued_after_signal = pending_orders_.size();
            for (const PendingOrder& order : pending_orders_) {
                if (order.default_flat_market_gross_candidate) {
                    ++candidates_after_signal;
                }
                if (order.created_by_same_id_replacement) {
                    ++replacements_after_signal;
                }
            }
            after_signal = snapshot();
        }
        if (bar_index_ == 1) {
            after_fill = snapshot();
        }
    }
};

static Snapshot run_probe(Probe& probe) {
    const Bar bars[] = {
        flat_bar(100.0, 600'000),
        flat_bar(100.0, 1'200'000),
    };
    probe.run(bars, 2);
    return probe.after_fill;
}

static void test_default_exact_pair_admission() {
    std::printf("-- production default exact-pair gross admission --\n");

    Probe probe;
    const Snapshot result = run_probe(probe);
    CHECK(probe.queued_after_signal == 2);
    CHECK(probe.candidates_after_signal == 2);
    check_snapshot(
        probe.after_signal, 0.0, 0,
        "[Long:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
        "Short:M:S:q=na:l=na:s=na:o=-/0:c=1:r=0]",
        "[]");
    // Gross frozen cost = (10 + 10) * 100 = 2000 > 1000 equity. Only the
    // later Short is canceled; Long fills its unchanged frozen quantity.
    check_snapshot(result, 10.0, 0, "[]", "[]");
}

static void test_exact_book_controls_remain_ordinary() {
    std::printf("-- id/direction/third/replacement/OCA controls --\n");
    struct Expected {
        Shape shape;
        const char* signal_book;
        double fill_position;
        int fill_trades;
        const char* fill_book;
        const char* trade_book;
    };
    const Expected expected[] = {
        {Shape::SAME_ID,
         "[Same:M:S:q=na:l=na:s=na:o=-/0:c=1:r=1]",
         -10.0, 0, "[]", "[]"},
        {Shape::SAME_DIRECTION,
         "[Long-1:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Long-2:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0]",
         10.0, 0, "[]", "[]"},
        {Shape::THREE_CALLS,
         "[Long-1:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Short-2:M:S:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Long-3:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0]",
         10.0, 2, "[]",
         "[L:Long-1>Short-2:100.0000>100.0000:q=10.0000:p=0.0000,"
         "S:Short-2>Long-3:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {Shape::REPLACEMENT,
         "[Long:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Short:M:S:q=na:l=na:s=na:o=-/0:c=1:r=1]",
         -10.0, 1, "[]",
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {Shape::OCA,
         "[Long:M:L:q=na:l=na:s=na:o=G/1:c=0:r=0,"
         "Short:M:S:q=na:l=na:s=na:o=G/1:c=0:r=0]",
         10.0, 0, "[]", "[]"},
        {Shape::PRICED_THIRD,
         "[Long:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Short:M:S:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Priced:E:L:q=1.0000:l=na:s=200.0000:o=-/0:c=0:r=0]",
         -10.0, 1,
         "[Priced:E:L:q=1.0000:l=na:s=200.0000:o=-/0:c=0:r=0]",
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {Shape::RAW_THIRD,
         "[Long:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Short:M:S:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Raw:R:L:q=1.0000:l=na:s=na:o=-/0:c=0:r=0]",
         -10.0, 1,
         "[Raw:R:L:q=1.0000:l=na:s=na:o=-/0:c=0:r=0]",
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {Shape::CANCELED_THIRD,
         "[Long:M:L:q=na:l=na:s=na:o=-/0:c=1:r=0,"
         "Short:M:S:q=na:l=na:s=na:o=-/0:c=1:r=0]",
         -10.0, 1, "[]",
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
    };
    for (const Expected& value : expected) {
        Probe probe(value.shape);
        const Snapshot result = run_probe(probe);
        check_snapshot(probe.after_signal, 0.0, 0, value.signal_book, "[]");
        check_snapshot(result, value.fill_position, value.fill_trades,
                       value.fill_book, value.trade_book);
    }

    Probe same_id(Shape::SAME_ID);
    run_probe(same_id);
    CHECK(same_id.queued_after_signal == 1);
    CHECK(same_id.replacements_after_signal == 1);

    Probe three(Shape::THREE_CALLS);
    run_probe(three);
    CHECK(three.queued_after_signal == 3);
    CHECK(three.candidates_after_signal == 3);

    Probe replacement(Shape::REPLACEMENT);
    run_probe(replacement);
    CHECK(replacement.queued_after_signal == 2);
    CHECK(replacement.replacements_after_signal == 1);

    Probe oca(Shape::OCA);
    run_probe(oca);
    CHECK(oca.candidates_after_signal == 0);

    Probe canceled(Shape::CANCELED_THIRD);
    run_probe(canceled);
    CHECK(canceled.queued_after_signal == 2);
}

enum class ConfigControl {
    DEFAULT_FIXED_5_5,
    EXPLICIT_KI65_5_5,
    MARGIN_50,
    COMMISSION,
    SLIPPAGE,
    RISK_LONG_ONLY,
    POOC,
    COOF,
    MAGNIFIER,
};

struct ConfigProbe : public BacktestEngine {
    explicit ConfigProbe(ConfigControl control) : control_(control) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        pyramiding_ = 1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        qty_step_ = 0.0;
        set_margin_call_enabled(false);
        switch (control_) {
            case ConfigControl::DEFAULT_FIXED_5_5:
                default_qty_type_ = QtyType::FIXED;
                default_qty_value_ = 5.5;
                break;
            case ConfigControl::EXPLICIT_KI65_5_5:
                default_qty_type_ = QtyType::FIXED;
                default_qty_value_ = 1.0;
                pyramiding_ = 2;
                break;
            case ConfigControl::MARGIN_50:
                margin_long_ = 50.0;
                margin_short_ = 50.0;
                break;
            case ConfigControl::COMMISSION:
                commission_value_ = 0.1;
                break;
            case ConfigControl::SLIPPAGE:
                slippage_ = 1;
                syminfo_mintick_ = 0.01;
                break;
            case ConfigControl::RISK_LONG_ONLY:
                risk_direction_ = RiskDirection::LONG_ONLY;
                break;
            case ConfigControl::POOC:
                process_orders_on_close_ = true;
                break;
            case ConfigControl::COOF:
                calc_on_order_fills_ = true;
                break;
            case ConfigControl::MAGNIFIER:
                bar_magnifier_enabled_ = true;
                break;
        }
    }

    ConfigControl control_;
    bool placed_ = false;
    int candidates_after_signal = 0;
    Snapshot after_signal;
    Snapshot after_fill;

    Snapshot snapshot() const {
        Snapshot result;
        result.signed_position = signed_position_size();
        result.trades = trade_count();
        std::ostringstream orders;
        orders << "[";
        for (size_t i = 0; i < pending_orders_.size(); ++i) {
            if (i != 0) orders << ",";
            const PendingOrder& order = pending_orders_[i];
            orders << order.id << ":" << Probe::order_type_name(order.type)
                   << ":" << (order.is_long ? "L" : "S")
                   << ":q=" << Probe::number(order.qty)
                   << ":l=" << Probe::number(order.limit_price)
                   << ":s=" << Probe::number(order.stop_price)
                   << ":o=" << (order.oca_name.empty() ? "-" : order.oca_name)
                   << "/" << order.oca_type
                   << ":c=" << order.default_flat_market_gross_candidate
                   << ":r=" << order.created_by_same_id_replacement;
        }
        orders << "]";
        result.pending_book = orders.str();
        std::ostringstream trades;
        trades << "[";
        for (size_t i = 0; i < trades_.size(); ++i) {
            if (i != 0) trades << ",";
            const Trade& trade = trades_[i];
            trades << (trade.is_long ? "L" : "S")
                   << ":" << trade.entry_id << ">" << trade.exit_id
                   << ":" << Probe::number(trade.entry_price)
                   << ">" << Probe::number(trade.exit_price)
                   << ":q=" << Probe::number(trade.qty)
                   << ":p=" << Probe::number(trade.pnl);
        }
        trades << "]";
        result.trade_book = trades.str();
        return result;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && !placed_) {
            placed_ = true;
            if (control_ == ConfigControl::EXPLICIT_KI65_5_5) {
                strategy_entry("Long", true, kNaN, kNaN, 5.5);
                strategy_entry("Short", false, kNaN, kNaN, 5.5);
            } else {
                strategy_entry("Long", true);
                strategy_entry("Short", false);
            }
            for (const PendingOrder& order : pending_orders_) {
                if (order.default_flat_market_gross_candidate) {
                    ++candidates_after_signal;
                }
            }
            after_signal = snapshot();
        }
        if (bar_index_ == 1) {
            after_fill = snapshot();
        }
    }
};

static Snapshot run_config(ConfigProbe& probe) {
    const Bar bars[] = {
        flat_bar(100.0, 600'000),
        flat_bar(100.0, 1'200'000),
    };
    if (probe.control_ == ConfigControl::MAGNIFIER) {
        probe.run(bars, 2, "1", "1", /*bar_magnifier=*/true, 4,
                  MagnifierDistribution::ENDPOINTS);
    } else {
        probe.run(bars, 2);
    }
    return probe.after_fill;
}

static void test_configuration_controls_and_explicit_ki65_stay_inert() {
    std::printf("-- fixed/explicit/risk/scheduler configuration controls --\n");
    struct Expected {
        ConfigControl control;
        const char* signal_book;
        double fill_position;
        int fill_trades;
        const char* trade_book;
    };
    const char* default_signal =
        "[Long:M:L:q=na:l=na:s=na:o=-/0:c=0:r=0,"
        "Short:M:S:q=na:l=na:s=na:o=-/0:c=0:r=0]";
    const Expected expected[] = {
        {ConfigControl::DEFAULT_FIXED_5_5, default_signal, -5.5, 1,
         "[L:Long>Short:100.0000>100.0000:q=5.5000:p=0.0000]"},
        {ConfigControl::EXPLICIT_KI65_5_5,
         "[Long:M:L:q=5.5000:l=na:s=na:o=-/0:c=0:r=0,"
         "Short:M:S:q=5.5000:l=na:s=na:o=-/0:c=0:r=0]",
         5.5, 0, "[]"},
        {ConfigControl::MARGIN_50, default_signal, -10.0, 1,
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {ConfigControl::COMMISSION, default_signal, -9.990009990009991, 1,
         "[L:Long>Short:100.0000>100.0000:q=9.9900:p=-1.9980]"},
        {ConfigControl::SLIPPAGE, default_signal, -10.001000100010002, 1,
         "[L:Long>Short:100.0100>99.9900:q=9.9990:p=-0.2000]"},
        {ConfigControl::RISK_LONG_ONLY, default_signal, 0.0, 1,
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {ConfigControl::POOC, default_signal, -10.0, 1,
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {ConfigControl::COOF, default_signal, -10.0, 1,
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
        {ConfigControl::MAGNIFIER, default_signal, -10.0, 1,
         "[L:Long>Short:100.0000>100.0000:q=10.0000:p=0.0000]"},
    };
    for (const Expected& value : expected) {
        ConfigProbe probe(value.control);
        const Snapshot result = run_config(probe);
        CHECK(probe.candidates_after_signal == 0);
        check_snapshot(probe.after_signal, 0.0, 0, value.signal_book, "[]");
        check_snapshot(result, value.fill_position, value.fill_trades,
                       "[]", value.trade_book);
    }
}

int main() {
    std::printf("--- production default flat MARKET gross admission ---\n");
    test_default_exact_pair_admission();
    test_exact_book_controls_remain_ordinary();
    test_configuration_controls_and_explicit_ki65_stay_inert();

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
