/*
 * Production placement-frozen STOP admission semantics.
 *
 * One tightly scoped true-flat all-in pure STOP snapshots its quantity at
 * placement. A next-bar open-marketable fill uses that quantity for admission
 * and dispatch; intrabar touches and all non-target controls keep ordinary
 * fill-time sizing.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        const double _a = (a), _b = (b);                                       \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.12f, expected %.12f\n",       \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    b.volume = 1.0;
    b.timestamp = ts;
    return b;
}

namespace {

enum class Shape { STOP, LIMIT, STOP_LIMIT };
enum class PostPlacementMutation {
    NONE,
    REALIZED_EQUITY_GAIN,
    COMMISSION,
    SLIPPAGE,
    MARGIN,
    DEFAULT_SIZING,
};

class Probe : public BacktestEngine {
public:
    Probe(QtyType qty_type = QtyType::PERCENT_OF_EQUITY,
          double qty_value = 100.0, double margin = 100.0,
          double capital = 10000.0) {
        initial_capital_ = capital;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_value_ = 0.0;
        margin_long_ = margin;
        margin_short_ = margin;
        qty_step_ = 0.0001;
        pyramiding_ = 1;
        set_margin_call_enabled(false);
    }

    Shape shape = Shape::STOP;
    bool is_long = false;
    bool explicit_qty = false;
    int reissue_bar = -1;
    PostPlacementMutation post_placement_mutation =
        PostPlacementMutation::NONE;
    double stop = 120.0;
    double limit = 80.0;
    double placement_snapshot_qty = kNaN;
    double reissue_snapshot_qty = kNaN;

    void on_bar(const Bar&) override {
        if (bar_index_ != 0 && bar_index_ != reissue_bar) return;
        const double qty = explicit_qty ? 7.0 : kNaN;
        switch (shape) {
            case Shape::STOP:
                strategy_entry("E", is_long, kNaN, stop, qty);
                break;
            case Shape::LIMIT:
                strategy_entry("E", is_long, limit, kNaN, qty);
                break;
            case Shape::STOP_LIMIT:
                strategy_entry("E", is_long, limit, stop, qty);
                break;
        }
        const PendingOrder* order = pending();
        if (order != nullptr) {
            if (bar_index_ == 0) {
                placement_snapshot_qty =
                    order->stop_placement_open_qty;
            } else if (bar_index_ == reissue_bar) {
                reissue_snapshot_qty = order->stop_placement_open_qty;
            }
        }
        if (bar_index_ != 0) return;
        // Mutate only after the placement snapshot has been captured. Each
        // case therefore proves that consumption re-validates current broker
        // state and falls back to the ordinary fill-time path.
        switch (post_placement_mutation) {
            case PostPlacementMutation::NONE:
                break;
            case PostPlacementMutation::REALIZED_EQUITY_GAIN:
                // Model an independent intervening round trip that realizes a
                // gain and returns broker state to flat before this STOP's
                // next-open adjudication. Flatness alone must not authorize a
                // stale placement-equity snapshot.
                net_profit_sum_ = 1000.0;
                break;
            case PostPlacementMutation::COMMISSION:
                commission_value_ = 0.1;
                break;
            case PostPlacementMutation::SLIPPAGE:
                slippage_ = 1;
                syminfo_mintick_ = 0.01;
                break;
            case PostPlacementMutation::MARGIN:
                margin_short_ = 50.0;
                margin_long_ = 50.0;
                break;
            case PostPlacementMutation::DEFAULT_SIZING:
                default_qty_type_ = QtyType::FIXED;
                default_qty_value_ = 7.0;
                break;
        }
    }

    const PendingOrder* pending() const {
        for (const PendingOrder& order : pending_orders_) {
            if (order.id == "E") return &order;
        }
        return nullptr;
    }

    PositionSide side() const { return position_side_; }
    double position_qty() const { return position_qty_; }
    double entry_price() const { return position_entry_price_; }
    double ledger_qty(const std::string& id = "E") const {
        const auto it = id_unclosed_qty_.find(id);
        return it == id_unclosed_qty_.end() ? 0.0 : it->second;
    }
};

static void run(Probe& probe, const std::vector<Bar>& bars) {
    probe.run(bars.data(), static_cast<int>(bars.size()));
}

void test_default_placement_snapshot() {
    std::printf("-- production default captures the narrow STOP snapshot --\n");

    Probe probe;
    run(probe, {bar(1000, 100, 100, 100, 100)});
    CHECK(probe.pending() != nullptr);
    if (probe.pending() != nullptr) {
        CHECK(std::isnan(probe.pending()->qty));
        CHECK(std::isnan(probe.pending()->frozen_default_qty));
        CHECK_NEAR(probe.pending()->stop_placement_open_qty,
                   100.0, 1e-12);
        CHECK_NEAR(probe.pending()->stop_placement_open_equity,
                   10000.0, 1e-12);
        CHECK_NEAR(probe.pending()->stop_placement_signal_close,
                   100.0, 1e-12);
    }
}

void test_positive_gap_declines_both_directions() {
    std::printf("-- higher-notional gap-open declines long and short --\n");

    Probe short_probe;
    short_probe.is_long = false;
    short_probe.stop = 120.0;  // short STOP is already marketable at open 110
    run(short_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(short_probe.side() == PositionSide::FLAT);
    CHECK(short_probe.trade_count() == 0);

    Probe long_probe;
    long_probe.is_long = true;
    long_probe.stop = 105.0;   // long STOP is marketable at open 110
    run(long_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(long_probe.side() == PositionSide::FLAT);
    CHECK(long_probe.trade_count() == 0);
}

void test_accepted_gap_dispatches_placement_quantity() {
    std::printf("-- admitted gap dispatches placement-frozen qty --\n");

    Probe short_probe;
    short_probe.is_long = false;
    short_probe.stop = 95.0;   // open 90 is through the short STOP
    run(short_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK(short_probe.side() == PositionSide::SHORT);
    CHECK_NEAR(short_probe.entry_price(), 90.0, 1e-12);
    CHECK_NEAR(short_probe.position_qty(), 100.0, 1e-9);

    Probe long_probe;
    long_probe.is_long = true;
    long_probe.stop = 85.0;    // deliberately wrong-side stop, open-marketable
    run(long_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK(long_probe.side() == PositionSide::LONG);
    CHECK_NEAR(long_probe.entry_price(), 90.0, 1e-12);
    CHECK_NEAR(long_probe.position_qty(), 100.0, 1e-9);
}

void test_one_step_favorable_gap_keeps_signal_close_lot() {
    std::printf("-- one-step favorable gap keeps signal-close lot --\n");

    // Placement equity 13118.817086 and signal close 3988.94 floor to 3.2887
    // contracts, while
    // re-dividing at the next open 3988.93 produces 3.2888. TV exports 3.2887.
    Probe probe(QtyType::PERCENT_OF_EQUITY, 100.0, 100.0,
                13118.817086);
    probe.is_long = false;
    probe.stop = 4000.0;
    run(probe, {
        bar(1000, 3988.94, 3988.94, 3988.94, 3988.94),
        bar(2000, 3988.93, 3989.00, 3988.00, 3988.50),
    });
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK_NEAR(probe.entry_price(), 3988.93, 1e-12);
    CHECK_NEAR(probe.position_qty(), 3.2887, 1e-12);
}

void test_prequantized_dispatch_preserves_exact_binary_lot() {
    std::printf("-- placement lot is not quantized a second time --\n");

    // floor((10000 / close) / 0.0001) * 0.0001 is the binary double
    // represented by literal 0.3. Applying the same floor a second time is
    // 0.2999 on this boundary, so exact position and id-ledger equality pin
    // the independent prequantized dispatch provenance.
    Probe probe(QtyType::PERCENT_OF_EQUITY, 100.0, 100.0, 10000.0);
    probe.is_long = false;
    probe.stop = 40000.0;
    run(probe, {
        bar(1000, 33327.77870354941, 33327.77870354941,
            33327.77870354941, 33327.77870354941),
        bar(2000, 33327.77, 33328.0, 33327.0, 33327.5),
    });
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK(probe.placement_snapshot_qty == 0.3);
    CHECK(probe.position_qty() == 0.3);
    CHECK(probe.ledger_qty() == 0.3);
}

void test_zero_open_falls_back_without_frozen_qty() {
    std::printf("-- zero open stays on the baseline dispatch path --\n");

    Probe baseline;
    baseline.is_long = false;
    baseline.stop = 40000.0;
    run(baseline, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 0, 1, 0, 1),
    });

    Probe enabled;
    enabled.is_long = false;
    enabled.stop = 40000.0;
    run(enabled, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 0, 1, 0, 1),
    });

    CHECK(enabled.side() == baseline.side());
    CHECK(enabled.placement_snapshot_qty == 100.0);
    CHECK(enabled.position_qty() == baseline.position_qty());
    CHECK(enabled.ledger_qty() == baseline.ledger_qty());
    CHECK(enabled.position_qty() == 0.0);
    CHECK(enabled.ledger_qty() == 0.0);
}

void test_replacement_reissues_the_snapshot() {
    std::printf("-- same-id reissue replaces, rather than reuses, snapshot --\n");

    Probe probe;
    probe.is_long = false;
    probe.stop = 50.0;
    probe.reissue_bar = 1;
    run(probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 80, 81, 79, 80),
        bar(3000, 40, 41, 39, 40),
    });
    CHECK_NEAR(probe.placement_snapshot_qty, 100.0, 1e-12);
    CHECK_NEAR(probe.reissue_snapshot_qty, 125.0, 1e-12);
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK_NEAR(probe.entry_price(), 40.0, 1e-12);
    CHECK_NEAR(probe.position_qty(), 125.0, 1e-12);
    CHECK_NEAR(probe.ledger_qty(), 125.0, 1e-12);
    CHECK(probe.pending() == nullptr);
}

void test_intervening_equity_change_while_flat_falls_back() {
    std::printf("-- intervening realized-equity change invalidates snapshot --\n");

    Probe probe;
    probe.is_long = false;
    probe.stop = 95.0;
    probe.post_placement_mutation =
        PostPlacementMutation::REALIZED_EQUITY_GAIN;
    run(probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK_NEAR(probe.placement_snapshot_qty, 100.0, 1e-12);
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK_NEAR(probe.entry_price(), 90.0, 1e-12);
    // Current realized equity is 11000, so ordinary fill-time sizing is
    // floor((11000 / 90) / 0.0001) * 0.0001 = 122.2222, not stale 100.
    CHECK_NEAR(probe.position_qty(), 122.2222, 1e-9);
    CHECK_NEAR(probe.ledger_qty(), 122.2222, 1e-9);
}

void test_placement_to_fill_config_changes_fall_back() {
    std::printf("-- placement-to-fill config changes use ordinary fallback --\n");

    struct Expected {
        PostPlacementMutation mutation;
        PositionSide side;
        double qty;
        double price;
    };
    const Expected expected[] = {
        {PostPlacementMutation::COMMISSION,
         PositionSide::SHORT, 111.0001, 90.0},
        // Slippage moves the short fill to 89.99 and ordinary fill-time sizing
        // becomes 111.1234; consuming the placement lot would instead open 100.
        {PostPlacementMutation::SLIPPAGE,
         PositionSide::SHORT, 111.1234, 89.99},
        {PostPlacementMutation::MARGIN,
         PositionSide::SHORT, 111.1111, 90.0},
        {PostPlacementMutation::DEFAULT_SIZING,
         PositionSide::SHORT, 7.0, 90.0},
    };
    for (const Expected& value : expected) {
        Probe probe;
        probe.is_long = false;
        probe.stop = 95.0;
        probe.post_placement_mutation = value.mutation;
        run(probe, {
            bar(1000, 100, 100, 100, 100),
            bar(2000, 90, 91, 89, 90),
        });
        CHECK_NEAR(probe.placement_snapshot_qty, 100.0, 1e-12);
        CHECK(probe.side() == value.side);
        CHECK_NEAR(probe.position_qty(), value.qty, 1e-9);
        CHECK_NEAR(probe.ledger_qty(), value.qty, 1e-9);
        if (value.side != PositionSide::FLAT) {
            CHECK_NEAR(probe.entry_price(), value.price, 1e-12);
        }
        CHECK(probe.pending() == nullptr);
    }
}

void test_scope_controls_remain_ordinary() {
    std::printf("-- intrabar, delayed, fractional, explicit and shape controls --\n");

    // Intrabar touch is not an open-marketable gap. Existing admission uses
    // fill-time qty at stop=120, costed at open=110, so it admits.
    Probe intrabar;
    intrabar.is_long = true;
    intrabar.stop = 120.0;
    run(intrabar, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 121, 109, 120),
    });
    CHECK(intrabar.side() == PositionSide::LONG);
    CHECK_NEAR(intrabar.entry_price(), 120.0, 1e-12);
    CHECK_NEAR(intrabar.position_qty(), 83.3333, 1e-9);

    // A STOP first becoming marketable two bars after placement is outside the
    // exact next-bar replay cell and remains fill-time-sized.
    Probe delayed;
    delayed.is_long = false;
    delayed.stop = 80.0;
    run(delayed, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 100, 110, 90, 100),
        bar(3000, 70, 71, 69, 70),
    });
    CHECK(delayed.side() == PositionSide::SHORT);
    CHECK_NEAR(delayed.position_qty(), 142.8571, 1e-9);

    Probe fractional(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    fractional.is_long = false;
    fractional.stop = 120.0;
    run(fractional, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(fractional.side() == PositionSide::SHORT);
    CHECK_NEAR(fractional.position_qty(), 45.4545, 1e-9);

    Probe explicit_stop;
    explicit_stop.is_long = false;
    explicit_stop.stop = 120.0;
    explicit_stop.explicit_qty = true;
    run(explicit_stop, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(explicit_stop.side() == PositionSide::SHORT);
    CHECK_NEAR(explicit_stop.position_qty(), 7.0, 1e-12);

    Probe limit_only;
    limit_only.shape = Shape::LIMIT;
    run(limit_only, {bar(1000, 100, 100, 100, 100)});
    CHECK(limit_only.pending() != nullptr);
    if (limit_only.pending() != nullptr) {
        CHECK(std::isnan(
            limit_only.pending()->stop_placement_open_qty));
    }

    Probe stop_limit;
    stop_limit.shape = Shape::STOP_LIMIT;
    run(stop_limit, {bar(1000, 100, 100, 100, 100)});
    CHECK(stop_limit.pending() != nullptr);
    if (stop_limit.pending() != nullptr) {
        CHECK(std::isnan(
            stop_limit.pending()->stop_placement_open_qty));
    }
}

}  // namespace

int main() {
    std::printf("--- production STOP placement open qty ---\n");
    test_default_placement_snapshot();
    test_positive_gap_declines_both_directions();
    test_accepted_gap_dispatches_placement_quantity();
    test_one_step_favorable_gap_keeps_signal_close_lot();
    test_prequantized_dispatch_preserves_exact_binary_lot();
    test_zero_open_falls_back_without_frozen_qty();
    test_replacement_reissues_the_snapshot();
    test_intervening_equity_change_while_flat_falls_back();
    test_placement_to_fill_config_changes_fall_back();
    test_scope_controls_remain_ordinary();

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
