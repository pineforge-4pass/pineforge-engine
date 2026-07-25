/*
 * Pending-aware gross admission for a default-sized MARKET/MARKET pair queued
 * while a LIVE position is held.
 *
 * The KI-65 pending-MARKET oracle pins TradingView's rule as: the later of two
 * opposite same-source-bar entries is costed as its OWN requested position plus
 * the movement the earlier pending opposite call will make. At
 * percent_of_equity=100 / margin=100 that gross movement is ~200% of equity and
 * the later call is silently declined.
 *
 * The shipped rule only ran when the pair was queued from true flat. These tests
 * pin the two live-position cases, which differ ONLY in whether the earlier call
 * was already over the pyramiding cap when it was placed:
 *
 *   live SHORT: earlier "Long" reverses  -> counts -> later "Short"  DECLINED
 *   live LONG:  earlier "Long" over cap  -> zero   -> later "Short"  ADMITTED
 *
 * plus the specimen idiom's co-queued unpriced close legs, and the book-shape
 * controls that must still abandon the adjudication.
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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

// Which side the account holds when the pair is queued.
enum class Seed { LiveShort, LiveLong };

// What the dual-signal bar queues besides the two entries.
enum class Shape {
    // if bull: entry Long; close Short   /   if bear: entry Short; close Long
    // The chartprime / market-logic-india idiom.
    ClosePairIdiom,
    // if bull: entry Long   /   if bear: entry Short  (fluxchart idiom)
    BareEntryPair,
    // Same-direction pair: never this rule.
    SameDirection,
    // A priced third order in the book abandons the adjudication.
    PricedThird,
    // A raw strategy.order in the book abandons the adjudication.
    RawThird,
    // A bracket armed on an EARLIER bar is still in the book: abandon.
    CarriedBracket,
};

struct Probe : public BacktestEngine {
    Probe(Seed seed, Shape shape) : seed_(seed), shape_(shape) {
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

    Seed seed_;
    Shape shape_;

    size_t book_after_signal = 0;
    int candidates_after_signal = 0;
    // over_pyramiding_cap_at_placement of the EARLIER entry call.
    bool earlier_over_cap = false;
    double signed_position_after_fill = 0.0;
    int trades_after_fill = 0;
    std::string entry_ids_after_fill;

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            if (shape_ == Shape::CarriedBracket) {
                // A resting priced bracket armed a bar before the pair.
                strategy_entry(seed_ == Seed::LiveShort ? "Short" : "Long",
                               seed_ == Seed::LiveShort ? false : true);
                return;
            }
            strategy_entry(seed_ == Seed::LiveShort ? "Short" : "Long",
                           seed_ == Seed::LiveShort ? false : true);
            return;
        }
        if (bar_index_ == 1 && shape_ == Shape::CarriedBracket) {
            // Arm a resting long stop entry far above the market so it survives
            // into the pair's bar. Argument order is (id, is_long, limit, stop).
            strategy_entry("Rest", true, kNaN, 500.0);
            return;
        }
        const int pair_bar = (shape_ == Shape::CarriedBracket) ? 2 : 1;
        if (bar_index_ == pair_bar) {
            switch (shape_) {
                case Shape::ClosePairIdiom:
                    strategy_entry("Long", true);
                    strategy_close("Short");
                    strategy_entry("Short", false);
                    strategy_close("Long");
                    break;
                case Shape::BareEntryPair:
                case Shape::CarriedBracket:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    break;
                case Shape::SameDirection:
                    strategy_entry("Long-1", true);
                    strategy_entry("Long-2", true);
                    break;
                case Shape::PricedThird:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_entry("Priced", true, kNaN, 500.0);
                    break;
                case Shape::RawThird:
                    strategy_entry("Long", true);
                    strategy_entry("Short", false);
                    strategy_order("Raw", true, 1.0);
                    break;
            }
            book_after_signal = pending_orders_.size();
            uint64_t earliest = 0;
            for (const PendingOrder& order : pending_orders_) {
                if (order.default_flat_market_gross_candidate) {
                    ++candidates_after_signal;
                    if (earliest == 0 || order.incarnation < earliest) {
                        earliest = order.incarnation;
                        earlier_over_cap =
                            order.over_pyramiding_cap_at_placement;
                    }
                }
            }
            return;
        }
        if (bar_index_ == pair_bar + 1) {
            signed_position_after_fill = signed_position_size();
            trades_after_fill = trade_count();
            std::ostringstream ids;
            ids << "[";
            for (size_t i = 0; i < trades_.size(); ++i) {
                if (i != 0) ids << ",";
                ids << (trades_[i].is_long ? "L" : "S") << ":"
                    << trades_[i].entry_id;
            }
            ids << "]";
            entry_ids_after_fill = ids.str();
        }
    }
};

static void run_probe(Probe& probe) {
    const Bar bars[] = {
        flat_bar(100.0, 600'000),  flat_bar(100.0, 1'200'000),
        flat_bar(100.0, 1'800'000), flat_bar(100.0, 2'400'000),
    };
    probe.run(bars, 4);
}

// ---------------------------------------------------------------------------
// The two live-position cases the widening exists for.
// ---------------------------------------------------------------------------

static void test_live_short_declines_the_later_call() {
    std::printf("-- live SHORT + close-pair idiom: later call DECLINED --\n");
    Probe probe(Seed::LiveShort, Shape::ClosePairIdiom);
    run_probe(probe);
    // Long entry + __close__Short exit leg + Short entry.
    CHECK(probe.book_after_signal == 3);
    CHECK(probe.candidates_after_signal == 2);
    // The earlier "Long" opposes the live short, so it moves the broker and
    // must be charged against the later "Short".
    CHECK(probe.earlier_over_cap == false);
    // "Long" reverses the short and is the sole fill; the account ends LONG.
    CHECK(near(probe.signed_position_after_fill, 10.0));
    CHECK(probe.trades_after_fill == 1);
    CHECK(probe.entry_ids_after_fill == "[S:Short]");
}

static void test_live_short_bare_pair_declines() {
    std::printf("-- live SHORT + bare entry pair: later call DECLINED --\n");
    Probe probe(Seed::LiveShort, Shape::BareEntryPair);
    run_probe(probe);
    CHECK(probe.book_after_signal == 2);
    CHECK(probe.candidates_after_signal == 2);
    CHECK(probe.earlier_over_cap == false);
    CHECK(near(probe.signed_position_after_fill, 10.0));
    CHECK(probe.trades_after_fill == 1);
    CHECK(probe.entry_ids_after_fill == "[S:Short]");
}

static void test_live_long_admits_the_later_call() {
    std::printf("-- live LONG: earlier call is over cap, later ADMITTED --\n");
    Probe probe(Seed::LiveLong, Shape::ClosePairIdiom);
    run_probe(probe);
    // Long entry + Short entry + __close__Long exit leg.
    CHECK(probe.book_after_signal == 3);
    CHECK(probe.candidates_after_signal == 2);
    // The earlier "Long" duplicates the live long at pyramiding=0: it moves
    // nothing, so it contributes zero and the later "Short" fits on its own.
    CHECK(probe.earlier_over_cap == true);
    CHECK(near(probe.signed_position_after_fill, -10.0));
    CHECK(probe.trades_after_fill == 1);
    CHECK(probe.entry_ids_after_fill == "[L:Long]");
}

static void test_live_long_bare_pair_admits() {
    std::printf("-- live LONG + bare entry pair: later ADMITTED --\n");
    Probe probe(Seed::LiveLong, Shape::BareEntryPair);
    run_probe(probe);
    CHECK(probe.earlier_over_cap == true);
    CHECK(near(probe.signed_position_after_fill, -10.0));
    CHECK(probe.trades_after_fill == 1);
    CHECK(probe.entry_ids_after_fill == "[L:Long]");
}

// ---------------------------------------------------------------------------
// Controls: shapes that must NOT be adjudicated by this rule.
// ---------------------------------------------------------------------------

static void test_same_direction_pair_is_not_this_rule() {
    std::printf("-- control: same-direction pair while live --\n");
    Probe probe(Seed::LiveShort, Shape::SameDirection);
    run_probe(probe);
    CHECK(probe.candidates_after_signal == 2);
    // First reverses the short, second duplicates it at the cap: no decline.
    CHECK(near(probe.signed_position_after_fill, 10.0));
}

static void test_priced_third_abandons_adjudication() {
    std::printf("-- control: a priced third order abandons the book --\n");
    Probe probe(Seed::LiveShort, Shape::PricedThird);
    run_probe(probe);
    // Both entries fill: Long reverses the short, Short reverses back.
    CHECK(near(probe.signed_position_after_fill, -10.0));
    CHECK(probe.trades_after_fill == 2);
}

static void test_raw_third_abandons_adjudication() {
    std::printf("-- control: a raw strategy.order abandons the book --\n");
    Probe probe(Seed::LiveShort, Shape::RawThird);
    run_probe(probe);
    CHECK(near(probe.signed_position_after_fill, -10.0));
}

static void test_carried_bracket_abandons_adjudication() {
    std::printf("-- control: an order carried in from an earlier bar --\n");
    Probe probe(Seed::LiveShort, Shape::CarriedBracket);
    run_probe(probe);
    // The resting stop entry is still in the book on the pair's bar, so the
    // pinned shape is gone and both entries keep their ordinary fill rules.
    CHECK(probe.book_after_signal == 3);
    CHECK(near(probe.signed_position_after_fill, -10.0));
}

int main() {
    std::printf("=== live-position default MARKET/MARKET gross admission ===\n");
    test_live_short_declines_the_later_call();
    test_live_short_bare_pair_declines();
    test_live_long_admits_the_later_call();
    test_live_long_bare_pair_admits();
    test_same_direction_pair_is_not_this_rule();
    test_priced_third_abandons_adjudication();
    test_raw_third_abandons_adjudication();
    test_carried_bracket_abandons_adjudication();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
