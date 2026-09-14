/*
 * Production placement-frozen STOP sizing semantics (round 7, family K).
 *
 * A DEFAULT percent_of_equity <= 100 pure STOP is sized when strategy.entry
 * is called — at the tick-snapped level, or at tick(close) when the level is
 * already at/beyond the close (TV's market-at-next-open order) — and is
 * placement-checked on that quantity at tick(close) (family E). The frozen
 * quantity is what admission costs and dispatch opens on every fill shape
 * (gap-through, intrabar touch, delayed touch); it is never re-sized while
 * resting. Non-default sizing, limit and stop-limit shapes carry no snapshot.
 *
 * Rule, tapes (scratchpad/r7/pins/f15-stopsize-*) and the ahtisham decode:
 * PendingOrder::default_stop_placement_qty (engine.hpp) and
 * tests/test_default_pct_stop_sizing.cpp. Before this round the snapshot
 * was a next-open-only, all-in, margin-100 special case sized at the CLOSE;
 * the expectations re-pinned here are listed in the commit.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
using pineforge::source::PendingOrder;

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

class Probe : public pineforge::source::PineStrategyHost {
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
    double reissue_stop = kNaN;   // level of the re-issue (default: same)
    PostPlacementMutation post_placement_mutation =
        PostPlacementMutation::NONE;
    double stop = 120.0;
    double limit = 80.0;
    double placement_snapshot_qty = kNaN;
    double placement_snapshot_basis = kNaN;
    double reissue_snapshot_qty = kNaN;
    bool placed_at_0 = false;
    bool placed_at_reissue = false;

    void on_source_bar(const Bar&) override {
        if (bar_index_ != 0 && bar_index_ != reissue_bar) return;
        const double qty = explicit_qty ? 7.0 : kNaN;
        const double level = (bar_index_ == reissue_bar && !std::isnan(reissue_stop))
            ? reissue_stop : stop;
        switch (shape) {
            case Shape::STOP:
                strategy_entry("E", is_long, kNaN, level, qty);
                break;
            case Shape::LIMIT:
                strategy_entry("E", is_long, limit, kNaN, qty);
                break;
            case Shape::STOP_LIMIT:
                strategy_entry("E", is_long, limit, level, qty);
                break;
        }
        const PendingOrder* order = pending();
        if (bar_index_ == 0) {
            placed_at_0 = order != nullptr;
            if (order != nullptr) {
                placement_snapshot_qty = order->default_stop_placement_qty;
                placement_snapshot_basis = order->default_stop_sizing_price;
            }
        } else if (bar_index_ == reissue_bar) {
            placed_at_reissue = order != nullptr;
            if (order != nullptr) {
                reissue_snapshot_qty = order->default_stop_placement_qty;
            }
        }
        if (bar_index_ != 0) return;
        // Mutate only after the placement snapshot has been captured. Each
        // case therefore proves what consumption does with a snapshot whose
        // broker state moved underneath it.
        switch (post_placement_mutation) {
            case PostPlacementMutation::NONE:
                break;
            case PostPlacementMutation::REALIZED_EQUITY_GAIN:
                // Model an independent intervening round trip that realizes a
                // gain and returns broker state to flat before this STOP's
                // next-open adjudication.
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
    std::printf("-- production default captures the STOP snapshot --\n");

    // A short stop ABOVE the close (120 > 100) is already beyond the level:
    // TV's market-at-next-open order, sized at tick(close) = 100 -> 100.
    Probe beyond;
    run(beyond, {bar(1000, 100, 100, 100, 100)});
    CHECK(beyond.pending() != nullptr);
    if (beyond.pending() != nullptr) {
        CHECK(std::isnan(beyond.pending()->qty));
        CHECK(std::isnan(beyond.pending()->frozen_default_qty));
        CHECK_NEAR(beyond.pending()->default_stop_placement_qty,
                   100.0, 1e-12);
        CHECK_NEAR(beyond.pending()->default_stop_sizing_price,
                   100.0, 1e-12);
        CHECK_NEAR(beyond.pending()->default_stop_placement_equity,
                   10000.0, 1e-12);
        CHECK_NEAR(beyond.pending()->default_stop_placement_signal_close,
                   100.0, 1e-12);
    }

    // A buy stop above the close is sized at the LEVEL: floor(10000 / 120,
    // 0.0001) = 83.3333, placement cost 83.3333 x 100 <= 10000.
    Probe at_level;
    at_level.is_long = true;
    at_level.stop = 120.0;
    run(at_level, {bar(1000, 100, 100, 100, 100)});
    CHECK(at_level.pending() != nullptr);
    if (at_level.pending() != nullptr) {
        CHECK_NEAR(at_level.pending()->default_stop_placement_qty,
                   83.3333, 1e-12);
        CHECK_NEAR(at_level.pending()->default_stop_sizing_price,
                   120.0, 1e-12);
    }
}

void test_positive_gap_declines_both_directions() {
    std::printf("-- higher-notional gap-open declines long and short --\n");

    // Beyond-level short (market-sized 100 at the close 100): the 110 open
    // costs 11000 > 10000 -> declined, dropped.
    Probe short_probe;
    short_probe.is_long = false;
    short_probe.stop = 120.0;
    run(short_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(short_probe.side() == PositionSide::FLAT);
    CHECK(short_probe.trade_count() == 0);

    // Buy stop 105 sized at the level (95.2380): the gap-through at 110
    // costs 10476.18 > 10000 -> declined, dropped.
    Probe long_probe;
    long_probe.is_long = true;
    long_probe.stop = 105.0;
    run(long_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(long_probe.placed_at_0);
    CHECK_NEAR(long_probe.placement_snapshot_qty, 95.238, 1e-9);
    CHECK(long_probe.side() == PositionSide::FLAT);
    CHECK(long_probe.trade_count() == 0);
}

void test_accepted_gap_dispatches_placement_quantity() {
    std::printf("-- admitted gap dispatches placement-frozen qty; the all-in sell stop below the close is never placed --\n");

    // Re-pinned (tapes pct100 / short-only): an all-in sell stop BELOW the
    // close is rejected at placement — floor(10000 / 95) = 105.2631 x 100 =
    // 10526.3 > 10000 — so the 90 gap-open through it fills nothing (the
    // pre-round-7 snapshot filled 100 @90 here).
    Probe short_probe;
    short_probe.is_long = false;
    short_probe.stop = 95.0;
    run(short_probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK(!short_probe.placed_at_0);
    CHECK(short_probe.side() == PositionSide::FLAT);
    CHECK(short_probe.trade_count() == 0);

    // At pct 50 the same sell stop places (52.6315 x 100 <= 10000) and the
    // gap-open fills the placement quantity at the rounded open 90 —
    // 52.6315, not the 55.5555 a fill-time re-size at 90 would open.
    Probe half(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    half.is_long = false;
    half.stop = 95.0;
    run(half, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK(half.placed_at_0);
    CHECK_NEAR(half.placement_snapshot_qty, 52.6315, 1e-9);
    CHECK(half.side() == PositionSide::SHORT);
    CHECK_NEAR(half.entry_price(), 90.0, 1e-12);
    CHECK_NEAR(half.position_qty(), 52.6315, 1e-9);

    // Beyond-level long (85 <= 100): market-sized 100 at the close, fills the
    // 90 open (9000 <= 10000) with that quantity.
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

    // Beyond-level short stop (4000 > 3988.94): market-sized at the close.
    // Placement equity 13118.817086 and signal close 3988.94 floor to 3.2887
    // contracts, while re-dividing at the next open 3988.93 produces 3.2888.
    // TV exports 3.2887.
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

    // pct 50 (an all-in sell stop below the close would never place). Bar 0:
    // sell stop 50 -> floor(5000 / 50) = 100, 100 x 100 <= 10000 placed. Bar
    // 1 (no touch) re-issues at 40 -> 125 = floor(5000 / 40), 125 x 80 =
    // 10000 <= 10000 placed, replacing the 100. Bar 2 gaps through 40: the
    // re-issued 125 fills at the open 40 (125 x 40 = 5000).
    Probe probe(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    probe.is_long = false;
    probe.stop = 50.0;
    probe.reissue_bar = 1;
    probe.reissue_stop = 40.0;
    run(probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 80, 81, 79, 80),
        bar(3000, 40, 41, 39, 40),
    });
    CHECK(probe.placed_at_0);
    CHECK_NEAR(probe.placement_snapshot_qty, 100.0, 1e-12);
    CHECK(probe.placed_at_reissue);
    CHECK_NEAR(probe.reissue_snapshot_qty, 125.0, 1e-12);
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK_NEAR(probe.entry_price(), 40.0, 1e-12);
    CHECK_NEAR(probe.position_qty(), 125.0, 1e-12);
    CHECK_NEAR(probe.ledger_qty(), 125.0, 1e-12);
    CHECK(probe.pending() == nullptr);
}

void test_rejected_reissue_cancels_resting_snapshot() {
    std::printf("-- a rejected same-id reissue cancels the resting default stop --\n");

    // pct 50: bar 0 sell stop 50 places 100 (100 x 100 <= 10000). Bar 1
    // closes 210 and re-issues the same level: 100 x 210 = 21000 > 10000 ->
    // rejected, and the resting 100 is cancelled (family E rule 2,
    // xau-flatten-replace-c10983). Bar 2 gaps through 50 and fills nothing.
    Probe probe(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    probe.is_long = false;
    probe.stop = 50.0;
    probe.reissue_bar = 1;
    run(probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 200, 211, 199, 210),
        bar(3000, 40, 41, 39, 40),
    });
    CHECK(probe.placed_at_0);
    CHECK(!probe.placed_at_reissue);
    CHECK(probe.pending() == nullptr);
    CHECK(probe.side() == PositionSide::FLAT);
    CHECK(probe.trade_count() == 0);
}

void test_intervening_equity_change_while_flat_keeps_snapshot() {
    std::printf("-- intervening realized-equity change does not re-size the resting stop --\n");

    // Re-pinned: the quantity is fixed at the call (K pin: qty = floor(equity
    // x pct / tick(level)) at the call; rule 2: only the script's next call
    // re-issues it). pct 50 sell stop 95 -> 52.6315 placed; realized equity
    // then moves to 11000 with nothing re-issued; the 90 gap-open fills the
    // placement lot 52.6315 (a fill-time re-size would open 61.1111), and the
    // fill is admitted against the equity at the fill (4736.8 <= 11000).
    Probe probe(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    probe.is_long = false;
    probe.stop = 95.0;
    probe.post_placement_mutation =
        PostPlacementMutation::REALIZED_EQUITY_GAIN;
    run(probe, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 90, 91, 89, 90),
    });
    CHECK_NEAR(probe.placement_snapshot_qty, 52.6315, 1e-9);
    CHECK(probe.side() == PositionSide::SHORT);
    CHECK_NEAR(probe.entry_price(), 90.0, 1e-12);
    CHECK_NEAR(probe.position_qty(), 52.6315, 1e-9);
    CHECK_NEAR(probe.ledger_qty(), 52.6315, 1e-9);
}

void test_placement_to_fill_config_changes() {
    std::printf("-- placement-to-fill config changes: the snapshot holds while the partition holds --\n");

    // Buy stop 105 above the close 100: 95.238 = floor(10000 / 105) placed
    // (9523.8 <= 10000). Bar 1 opens 104 and touches 105: the placement lot
    // fills at the level (95.238 x 105 = 9999.99 <= 10000) whatever moved
    // in the commission / slippage / margin settings since the call — the
    // order's quantity is the order's. Slippage moves the booked price one
    // tick. A declaration change to FIXED sizing leaves the default-percent
    // partition, so the snapshot is not consumed and the fill-time FIXED
    // quantity (7) opens.
    struct Expected {
        PostPlacementMutation mutation;
        PositionSide side;
        double qty;
        double price;
    };
    const Expected expected[] = {
        {PostPlacementMutation::COMMISSION,
         PositionSide::LONG, 95.238, 105.0},
        {PostPlacementMutation::SLIPPAGE,
         PositionSide::LONG, 95.238, 105.01},
        {PostPlacementMutation::MARGIN,
         PositionSide::LONG, 95.238, 105.0},
        {PostPlacementMutation::DEFAULT_SIZING,
         PositionSide::LONG, 7.0, 105.0},
    };
    for (const Expected& value : expected) {
        Probe probe;
        probe.is_long = true;
        probe.stop = 105.0;
        probe.post_placement_mutation = value.mutation;
        run(probe, {
            bar(1000, 100, 100, 100, 100),
            bar(2000, 104, 106, 103, 105),
        });
        CHECK_NEAR(probe.placement_snapshot_qty, 95.238, 1e-9);
        CHECK(probe.side() == value.side);
        CHECK_NEAR(probe.position_qty(), value.qty, 1e-9);
        CHECK_NEAR(probe.ledger_qty(), value.qty, 1e-9);
        if (value.side != PositionSide::FLAT) {
            CHECK_NEAR(probe.entry_price(), value.price, 1e-9);
        }
        CHECK(probe.pending() == nullptr);
    }
}

void test_scope_controls_remain_ordinary() {
    std::printf("-- intrabar, delayed, fractional, explicit and shape controls --\n");

    // Intrabar touch: the buy stop 120 was sized at the level (83.3333) and
    // fills there (83.3333 x 120 = 9999.996 <= 10000).
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

    // A STOP first becoming marketable two bars after placement still carries
    // its placement quantity: pct 50 sell stop 80 -> 62.5 (an all-in sell
    // stop below the close would not place), the bar-2 gap-open 70 fills
    // 62.5 (a fill-time re-size at 70 would open 71.4285).
    Probe delayed(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    delayed.is_long = false;
    delayed.stop = 80.0;
    run(delayed, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 100, 110, 90, 100),
        bar(3000, 70, 71, 69, 70),
    });
    CHECK(delayed.side() == PositionSide::SHORT);
    CHECK_NEAR(delayed.entry_price(), 70.0, 1e-12);
    CHECK_NEAR(delayed.position_qty(), 62.5, 1e-9);

    // Beyond-level short at pct 50: market-sized at the close, 50 =
    // floor(5000 / 100), fills the 110 open with 50 (5500 <= 10000) — not the
    // 45.4545 a fill-time re-size at the open would open (the ahtisham 04-04
    // 13:45Z 1,043 = floor(eq / tick(close)) shape).
    Probe fractional(QtyType::PERCENT_OF_EQUITY, 50.0, 100.0);
    fractional.is_long = false;
    fractional.stop = 120.0;
    run(fractional, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(fractional.side() == PositionSide::SHORT);
    CHECK_NEAR(fractional.position_qty(), 50.0, 1e-9);

    // Explicit qty: family E, no snapshot; 7 x 100 placed, 7 x 110 admitted.
    Probe explicit_stop;
    explicit_stop.is_long = false;
    explicit_stop.stop = 120.0;
    explicit_stop.explicit_qty = true;
    run(explicit_stop, {
        bar(1000, 100, 100, 100, 100),
        bar(2000, 110, 111, 109, 110),
    });
    CHECK(std::isnan(explicit_stop.placement_snapshot_qty));
    CHECK(explicit_stop.side() == PositionSide::SHORT);
    CHECK_NEAR(explicit_stop.position_qty(), 7.0, 1e-12);

    Probe limit_only;
    limit_only.shape = Shape::LIMIT;
    run(limit_only, {bar(1000, 100, 100, 100, 100)});
    CHECK(limit_only.pending() != nullptr);
    if (limit_only.pending() != nullptr) {
        CHECK(std::isnan(
            limit_only.pending()->default_stop_placement_qty));
    }

    Probe stop_limit;
    stop_limit.shape = Shape::STOP_LIMIT;
    run(stop_limit, {bar(1000, 100, 100, 100, 100)});
    CHECK(stop_limit.pending() != nullptr);
    if (stop_limit.pending() != nullptr) {
        CHECK(std::isnan(
            stop_limit.pending()->default_stop_placement_qty));
    }
}

}  // namespace

int main() {
    std::printf("--- production STOP placement qty (round 7 family K) ---\n");
    test_default_placement_snapshot();
    test_positive_gap_declines_both_directions();
    test_accepted_gap_dispatches_placement_quantity();
    test_one_step_favorable_gap_keeps_signal_close_lot();
    test_prequantized_dispatch_preserves_exact_binary_lot();
    test_zero_open_falls_back_without_frozen_qty();
    test_replacement_reissues_the_snapshot();
    test_rejected_reissue_cancels_resting_snapshot();
    test_intervening_equity_change_while_flat_keeps_snapshot();
    test_placement_to_fill_config_changes();
    test_scope_controls_remain_ordinary();

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
