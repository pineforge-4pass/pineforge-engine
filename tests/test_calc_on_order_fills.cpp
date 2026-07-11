/*
 * KI-60: calc_on_order_fills historical broker scheduling.
 *
 * These fixtures intentionally exercise the semantic seams that a broad
 * "run on_bar again after process_pending_orders" loop misses:
 *   - one broker fill per recalc, with a monotonic O -> near -> far -> C path;
 *   - the four historical fill-event budget (including exits, not just opens);
 *   - orders born in a recalc can only inspect the current/remaining path;
 *   - process_orders_on_close fills recalc at C without replaying the wick;
 *   - historical recalc executions expose barstate.isnew/isconfirmed together;
 *   - script state rolls back to the committed checkpoint, broker state does not;
 *   - the flag-off path and an explicit false override retain legacy behaviour.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/series.hpp>

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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

std::vector<Bar> standard_feed() {
    return {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0,  90.0, 105.0, 1000.0, 1'800'000},
        {105.0, 106.0, 104.0, 105.0, 1000.0, 2'700'000},
    };
}

class CoofBase : public BacktestEngine {
public:
    explicit CoofBase(bool enabled = true) {
        calc_on_order_fills_ = enabled;
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 10;
        slippage_ = 0;
        commission_value_ = 0.0;
    }

    double signed_size() const { return signed_position_size(); }
    int open_lot_count() const { return static_cast<int>(pyramid_entries_.size()); }
    std::vector<double> open_lot_prices() const {
        std::vector<double> out;
        for (const auto& lot : pyramid_entries_) out.push_back(lot.price);
        return out;
    }
    std::vector<std::string> open_lot_ids() const {
        std::vector<std::string> out;
        for (const auto& lot : pyramid_entries_) out.push_back(lot.entry_id);
        return out;
    }
    bool coof_enabled() const { return calc_on_order_fills_; }
};

// Q1 TV pin: a carried market entry fills at O, its post-fill strategy.close
// fills on that same historical bar at the same price.
class MarketCloseProbe final : public CoofBase {
public:
    using CoofBase::CoofBase;

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
        } else if (position_side_ == PositionSide::LONG) {
            strategy_close("L");
        }
    }
};

void test_market_close_fills_same_bar_at_entry_price() {
    std::printf("test_market_close_fills_same_bar_at_entry_price\n");
    MarketCloseProbe p;
    auto bars = standard_feed();
    p.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 100.0));
    }
}

// Q3 TV pin: the bracket does not exist until the entry-fill recalc. Its stop
// must become live for the REMAINING path and fill at its level, not at the
// later endpoint and not on the following bar.
class BracketProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, 99.0);
        }
    }
};

void test_recalc_bracket_uses_remaining_path() {
    std::printf("test_recalc_bracket_uses_remaining_path\n");
    BracketProbe p;
    auto bars = standard_feed();
    p.run(bars.data(), 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 99.0));
    }

    // The same contract on real lower-TF magnifier data: endpoint count and
    // termination come from the supplied lower bars (4 OHLC ticks each), and
    // the recalc-created stop sees only endpoints after the entry fill.
    BracketProbe magnified;
    Bar lower[] = {
        {100.0, 101.0,  99.0, 100.0, 500.0,  60'000},
        {100.0, 101.0,  99.0, 100.0, 500.0, 120'000},
        {100.0, 102.0,  98.0, 101.0, 500.0, 180'000},
        {101.0, 103.0, 100.0, 102.0, 500.0, 240'000},
    };
    magnified.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
                  /*magnifier_samples=*/4,
                  MagnifierDistribution::ENDPOINTS);
    CHECK(magnified.last_error().empty());
    CHECK(magnified.trade_count() == 1);
    if (magnified.trade_count() == 1) {
        const Trade& t = magnified.get_trade(0);
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 99.0));
    }
}

// Q2 TV pin: a historical non-magnified bar supplies four broker fill events.
// A carried market order and the first recalc order both execute at O; later
// recalc orders advance monotonically to the near and far endpoints. For this
// tie-distance bar the standard path is O -> L -> H -> C, so use a high-near
// bar below to pin the exported O,O,H,L sequence exactly.
class RefillProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ <= 1 && position_qty_ < 6.0) {
            strategy_entry("L" + std::to_string(position_entry_count_), true);
        }
    }
};

void test_historical_refill_is_exact_o_o_near_far_and_capped_at_four() {
    std::printf("test_historical_refill_is_exact_o_o_near_far_and_capped_at_four\n");
    RefillProbe p;
    // |H-O|=1 < |O-L|=10 => O -> H -> L -> C.
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 101.0,  90.0,  95.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.open_lot_count() == 4);
    const std::vector<double> px = p.open_lot_prices();
    CHECK(px.size() == 4);
    if (px.size() == 4) {
        CHECK(near(px[0], 100.0));
        CHECK(near(px[1], 100.0));
        CHECK(near(px[2], 101.0));
        CHECK(near(px[3],  90.0));
    }

    // Real lower-TF magnifier data supplies 60 endpoint ticks for the second
    // script bar (15 lower bars x O/H/L/C), so the six-unit strategy cap—not a
    // hard-coded four/16-iteration loop—must become the binding limit.
    RefillProbe magnified;
    std::vector<Bar> lower;
    lower.reserve(30);
    for (int i = 0; i < 30; ++i) {
        const double o = (i < 15) ? 100.0 : 100.0 + (i - 15) * 0.1;
        lower.push_back({o, o + 1.0, o - 1.0, o + 0.25,
                         500.0, static_cast<int64_t>(i) * 60'000});
    }
    magnified.run(lower.data(), static_cast<int>(lower.size()),
                  "1", "15", /*bar_magnifier=*/true,
                  /*magnifier_samples=*/4,
                  MagnifierDistribution::ENDPOINTS);
    CHECK(magnified.last_error().empty());
    CHECK(magnified.open_lot_count() == 6);
}

// Only the historical bar's O has the documented same-point two-fill
// exception. When a resting priced entry lands exactly on H/L, that endpoint
// is consumed before its fill recalc runs; a recalc-born market add must wait
// for the NEXT waypoint/tick even when the fill price equals the endpoint.
class EndpointMarketAddProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("Stop", true, kNaN, 105.0);
        } else if (bar_index_ == 1 && coof_fill_recalc_active_
                   && position_entry_count_ == 1) {
            strategy_entry("Add", true);
        }
    }
};

void test_non_open_endpoint_fill_consumes_point_before_market_add() {
    std::printf(
        "test_non_open_endpoint_fill_consumes_point_before_market_add\n");
    EndpointMarketAddProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0, 900'000},
        {100.0, 105.0, 90.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    const std::vector<double> px = p.open_lot_prices();
    CHECK(px.size() == 2);
    if (px.size() == 2) {
        CHECK(near(px[0], 105.0));
        CHECK(near(px[1], 90.0));
    }
}

void test_magnifier_endpoint_fill_consumes_tick_before_market_add() {
    std::printf(
        "test_magnifier_endpoint_fill_consumes_tick_before_market_add\n");
    EndpointMarketAddProbe p;
    Bar lower[] = {
        {100.0, 101.0,  99.0, 100.0, 500.0,       0},
        {100.0, 101.0,  99.0, 100.0, 500.0,  60'000},
        {100.0, 105.0,  90.0, 100.0, 500.0, 120'000},
        {100.0, 101.0,  99.0, 100.0, 500.0, 180'000},
    };
    p.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
          /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(p.last_error().empty());
    const std::vector<double> px = p.open_lot_prices();
    CHECK(px.size() == 2);
    if (px.size() == 2) {
        CHECK(near(px[0], 105.0));
        CHECK(near(px[1], 90.0));
    }
}

// Real lower-timeframe bars are distinct broker epochs.  A gap from one
// sub-bar's close to the next sub-bar's open is not a traversed price segment:
// a resting limit crossed by that gap fills at the new open, never at an
// interpolated price inside the gap.  The non-COOF magnifier path already
// preserves this boundary; this fixture pins the COOF scheduler to the same
// contract.
class MagnifierGapBoundaryProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && pending_orders_.empty() && trades_.empty()) {
            strategy_entry("GapLimit", true, 95.0, kNaN, 1.0);
        }
    }
};

void test_real_magnifier_gap_fills_limit_at_fresh_subbar_open() {
    std::printf(
        "test_real_magnifier_gap_fills_limit_at_fresh_subbar_open\n");
    MagnifierGapBoundaryProbe p;
    Bar lower[] = {
        // Script bar 0: place the carried 95 limit at the completed close.
        {100.0, 101.0, 99.0, 100.0, 1000.0,       0},
        {100.0, 101.0, 99.0, 100.0, 1000.0,  60'000},
        // Script bar 1: first sub-bar stays above 95; the second gaps to 90.
        {100.0, 101.0, 99.0, 100.0, 1000.0, 120'000},
        { 90.0,  92.0, 88.0,  91.0, 1000.0, 180'000},
    };
    p.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
          /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(p.last_error().empty());
    CHECK(near(p.signed_size(), 1.0));
    CHECK(p.open_lot_count() == 1);
    if (p.open_lot_count() == 1) {
        CHECK(near(p.open_lot_prices().front(), 90.0));
    }
}

class MagnifierGapStopBoundaryProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && pending_orders_.empty() && trades_.empty()) {
            strategy_entry("GapStop", true, kNaN, 105.0, 1.0);
        }
    }
};

void test_real_magnifier_gap_fills_stop_at_fresh_subbar_open() {
    std::printf(
        "test_real_magnifier_gap_fills_stop_at_fresh_subbar_open\n");
    MagnifierGapStopBoundaryProbe p;
    Bar lower[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,       0},
        {100.0, 101.0, 99.0, 100.0, 1000.0,  60'000},
        {100.0, 104.0, 99.0, 100.0, 1000.0, 120'000},
        {110.0, 112.0,108.0, 111.0, 1000.0, 180'000},
    };
    p.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
          /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(p.last_error().empty());
    CHECK(near(p.signed_size(), 1.0));
    CHECK(p.open_lot_count() == 1);
    if (p.open_lot_count() == 1) {
        CHECK(near(p.open_lot_prices().front(), 110.0));
    }
}

// Mutation killer for termination counters that count only entries (or only
// newly-created trade rows). Entry and market-close fills must each consume an
// event. Four events produce exactly two round trips: O/O then H/L.
class AlternatingFillKindsProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && trades_.empty()
            && position_side_ == PositionSide::FLAT) {
            strategy_entry("L0", true);
            return;
        }
        if (bar_index_ != 1) return;
        if (position_side_ == PositionSide::LONG) {
            strategy_close(pyramid_entries_.front().entry_id);
        } else if (trades_.size() < 2) {
            strategy_entry("L" + std::to_string(trades_.size() + 1), true);
        }
    }
};

void test_exit_fills_consume_historical_event_budget() {
    std::printf("test_exit_fills_consume_historical_event_budget\n");
    AlternatingFillKindsProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 101.0,  90.0,  95.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 100.0));
        CHECK(near(p.get_trade(1).entry_price, 101.0));
        CHECK(near(p.get_trade(1).exit_price, 90.0));
    }
    CHECK(near(p.signed_size(), 0.0));
}

// A source-order scan is not a chronological scheduler. Both resting buy
// stops are touched on the same rising segment, but the farther stop was
// created first. TV fills Near@105 before Far@108; after the first fill the
// cursor must continue from 105 so the farther trigger remains reachable.
class RestingPricedChronologyProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Far", true, kNaN, 108.0);
            strategy_entry("Near", true, kNaN, 105.0);
        }
    }
};

void test_same_segment_priced_orders_fill_nearest_first() {
    std::printf("test_same_segment_priced_orders_fill_nearest_first\n");
    RestingPricedChronologyProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0, 99.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    const auto px = p.open_lot_prices();
    CHECK(ids.size() == 2);
    CHECK(px.size() == 2);
    if (ids.size() == 2 && px.size() == 2) {
        CHECK(ids[0] == "Near");
        CHECK(ids[1] == "Far");
        CHECK(near(px[0], 105.0));
        CHECK(near(px[1], 108.0));
    }
}

// Stop-limit activation is broker state, not a property that can be
// reconstructed from each shortened scheduler segment. A activates on O->H;
// B fills first on H->L; resuming from B@100 must retain A's activation so its
// limit can fill later at 95.
class StopLimitActivationProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("A", true, 95.0, 105.0);
            strategy_entry("B", true, 100.0, kNaN);
        }
    }
};

void test_stop_limit_activation_survives_segment_split() {
    std::printf("test_stop_limit_activation_survives_segment_split\n");
    StopLimitActivationProbe p;
    Bar bars[] = {
        {102.0, 103.0, 101.0, 102.0, 1000.0,   900'000},
        {102.0, 110.0,  90.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    const auto px = p.open_lot_prices();
    CHECK(ids.size() == 2);
    CHECK(px.size() == 2);
    if (ids.size() == 2 && px.size() == 2) {
        CHECK(ids[0] == "B");
        CHECK(ids[1] == "A");
        CHECK(near(px[0], 100.0));
        CHECK(near(px[1], 95.0));
    }
}

// KI-67: with the fixed 4-event budget removed the broker cursor traverses the
// WHOLE O->L->H->C path, so A's stop=108 IS genuinely reached on the L->H leg
// (the bar prints 110) and A activates; its limit=95 then fills on bar index 2
// when the low reaches 90. (The old budget stopped the cursor at 105 and A
// never armed — a truncation artifact, not TV behaviour.) A is a resting order,
// not a cascade order, so the cascade waypoint gate never applies to it.
class StopLimitSpeculationProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("M0", true);
            strategy_entry("M1", true);
            strategy_entry("A", true, 95.0, 108.0);
            strategy_entry("B103", true, kNaN, 103.0);
            strategy_entry("B105", true, kNaN, 105.0);
        }
    }
};

void test_stop_limit_activation_commits_only_through_consumed_cursor() {
    std::printf("test_stop_limit_activation_commits_only_through_consumed_cursor\n");
    StopLimitSpeculationProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0, 85.0, 100.0, 1000.0, 1'800'000},
        {100.0, 104.0, 90.0,  95.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    const auto lpx = p.open_lot_prices();
    CHECK(ids.size() == 5);
    if (ids.size() == 5) {
        CHECK(ids[0] == "M0");
        CHECK(ids[1] == "M1");
        CHECK(ids[2] == "B103");
        CHECK(ids[3] == "B105");
        CHECK(ids[4] == "A");        // KI-67: A's stop=108 is truly reached; it
        CHECK(near(lpx[4], 95.0));   // arms and its limit fills at 95 on bar 2.
    }
}

// The legacy one-priced-entry-per-bar throttle predates COOF. A priced entry
// born in a fill recalc belongs to the new broker epoch and may itself fill,
// recalc, and place another priced entry on the remaining same-bar segment.
// KI-67: L1 is placed by the bar-OPEN recalc (standard: exact fill at 105);
// L2 is placed by the MID-BAR recalc that L1's fill triggered, so it is a
// cascade order and GAP-fills at the next extreme waypoint (W2=110), not at its
// interpolated 108 level on the L->H segment.
class RecalcPricedEntryCascadeProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("L0", true);
        } else if (bar_index_ == 1 && position_entry_count_ == 1) {
            strategy_entry("L1", true, kNaN, 105.0);
        } else if (bar_index_ == 1 && position_entry_count_ == 2) {
            strategy_entry("L2", true, kNaN, 108.0);
        }
    }
};

void test_fill_recalc_priced_entries_bypass_legacy_bar_throttle() {
    std::printf("test_fill_recalc_priced_entries_bypass_legacy_bar_throttle\n");
    RecalcPricedEntryCascadeProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0, 90.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    const auto px = p.open_lot_prices();
    CHECK(ids.size() == 3);
    CHECK(px.size() == 3);
    if (ids.size() == 3 && px.size() == 3) {
        CHECK(ids[0] == "L0");
        CHECK(ids[1] == "L1");
        CHECK(ids[2] == "L2");
        CHECK(near(px[0], 100.0));
        CHECK(near(px[1], 105.0));
        // KI-67: cascade L2 gap-fills at the extreme waypoint W2=110, not at
        // its interpolated 108 level inside the L->H segment.
        CHECK(near(px[2], 110.0));
    }
}

// Recalc origin is an event epoch, not a permanent exemption. A stop emitted
// by a prior bar's fill recalc and carried overnight must re-enter the legacy
// one-priced-entry-per-bar arbitration on the later bar.
class RecalcPricedCarryThrottleProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("L0", true);
        } else if (bar_index_ == 1 && coof_fill_recalc_active_
                   && position_entry_count_ == 1) {
            strategy_entry("Carry", true, kNaN, 108.0);
        } else if (bar_index_ == 1 && !coof_fill_recalc_active_
                   && position_entry_count_ == 1) {
            strategy_entry("First", true, kNaN, 105.0);
        }
    }
};

void test_recalc_priced_entry_exemption_expires_after_creation_bar() {
    std::printf("test_recalc_priced_entry_exemption_expires_after_creation_bar\n");
    RecalcPricedCarryThrottleProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 106.0, 90.0, 100.0, 1000.0, 1'800'000},
        {100.0, 110.0, 85.0, 100.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    CHECK(ids.size() == 2);
    if (ids.size() == 2) {
        CHECK(ids[0] == "L0");
        CHECK(ids[1] == "First");
    }
}

// A full close's stale-order cancellation belongs to the position cycle it
// ended. Once New0 opens a fresh cycle, New1/New2 emitted by its recalcs must
// not be mistaken for adds attached to the old closed long merely because all
// events share one historical bar.
class CloseReopenCycleProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("Old", true);
            return;
        }
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG
            && !coof_fill_recalc_active_) {
            strategy_close("Old");
            return;
        }
        if (bar_index_ != 2) return;
        if (position_side_ == PositionSide::FLAT) {
            strategy_entry("New0", true);
        } else if (position_entry_count_ < 3) {
            strategy_entry("New" + std::to_string(position_entry_count_), true);
        }
    }
};

void test_close_cleanup_does_not_leak_into_new_position_cycle() {
    std::printf("test_close_cleanup_does_not_leak_into_new_position_cycle\n");
    CloseReopenCycleProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 101.0, 99.0, 100.0, 1000.0, 1'800'000},
        {100.0, 105.0, 90.0,  95.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);

    CHECK(p.last_error().empty());
    const auto ids = p.open_lot_ids();
    const auto px = p.open_lot_prices();
    CHECK(ids.size() == 3);
    CHECK(px.size() == 3);
    if (ids.size() == 3 && px.size() == 3) {
        CHECK(ids[0] == "New0");
        CHECK(ids[1] == "New1");
        CHECK(ids[2] == "New2");
        CHECK(near(px[0], 100.0));
        CHECK(near(px[1], 105.0));
        CHECK(near(px[2], 90.0));
    }
}

// A COOF-created bracket may contain one leg that is already marketable at the
// entry-fill cursor. TradingView suppresses only that wrong-side leg for the
// entry bar: it carries into the next bar, while a correctly-sided sibling
// remains eligible on the entry bar's remaining path.
class RecalcEntryBarBracketProbe final : public CoofBase {
public:
    enum class Shape {
        WRONG_STOP_ONLY,
        WRONG_LIMIT_ONLY,
        WRONG_STOP_VALID_LIMIT,
        VALID_STOP_WRONG_LIMIT,
    };

    explicit RecalcEntryBarBracketProbe(Shape shape) : shape_(shape) {}

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("L", true);
        } else if (bar_index_ == 1 && position_side_ == PositionSide::LONG
                   && coof_fill_recalc_active_) {
            switch (shape_) {
            case Shape::WRONG_STOP_ONLY:
                strategy_exit("X", "L", kNaN, 105.0);
                break;
            case Shape::WRONG_LIMIT_ONLY:
                strategy_exit("X", "L", 95.0, kNaN);
                break;
            case Shape::WRONG_STOP_VALID_LIMIT:
                strategy_exit("X", "L", 110.0, 105.0);
                break;
            case Shape::VALID_STOP_WRONG_LIMIT:
                strategy_exit("X", "L", 90.0, 95.0);
                break;
            }
        }
    }

private:
    Shape shape_;
};

void test_recalc_wrong_side_entry_bar_legs_carry_to_next_bar() {
    std::printf("test_recalc_wrong_side_entry_bar_legs_carry_to_next_bar\n");
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 105.0, 90.0,  95.0, 1000.0, 1'800'000},
        {104.0, 106.0, 103.0, 105.0, 1000.0, 2'700'000},
    };

    for (auto shape : {
             RecalcEntryBarBracketProbe::Shape::WRONG_STOP_ONLY,
             RecalcEntryBarBracketProbe::Shape::WRONG_LIMIT_ONLY,
         }) {
        RecalcEntryBarBracketProbe p(shape);
        p.run(bars, 3);
        CHECK(p.last_error().empty());
        CHECK(p.trade_count() == 1);
        if (p.trade_count() == 1) {
            const Trade& t = p.get_trade(0);
            CHECK(near(t.entry_price, 100.0));
            CHECK(near(t.exit_price, 104.0));
            CHECK(t.entry_bar_index == 1);
            CHECK(t.exit_bar_index == 2);
        }
    }
}

void test_recalc_wrong_stop_does_not_hide_valid_limit_leg() {
    std::printf("test_recalc_wrong_stop_does_not_hide_valid_limit_leg\n");
    RecalcEntryBarBracketProbe p(
        RecalcEntryBarBracketProbe::Shape::WRONG_STOP_VALID_LIMIT);
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 112.0, 90.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 110.0));
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);
    }
}

void test_recalc_wrong_limit_does_not_hide_valid_stop_leg() {
    std::printf("test_recalc_wrong_limit_does_not_hide_valid_stop_leg\n");
    RecalcEntryBarBracketProbe p(
        RecalcEntryBarBracketProbe::Shape::VALID_STOP_WRONG_LIMIT);
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 106.0, 90.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 95.0));
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);
    }
}

// A first fill that occurs inside an OHLC path segment is not a second broker
// tick at that price. A market entry created by its COOF recalc fills at the
// segment's next waypoint. This is distinct from the bar-open exception where
// a carried market fill and the first order it creates may both consume O.
//
// The second short deliberately inherits an already-marketable buy-limit. Its
// entry must be L=90, the limit must remain dormant for that entry bar, and the
// carried limit must exit at 95 on the next bar.
class InteriorExitReentryCarryProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("S0", false);
            return;
        }

        if (position_side_ == PositionSide::SHORT) {
            if (position_open_bar_ == 1) {
                strategy_exit("X0", "S0", 95.0, kNaN);
            } else if (position_open_bar_ == 2) {
                strategy_exit("X1", "S1", 95.0, kNaN);
            }
            return;
        }

        if (bar_index_ == 2 && coof_fill_recalc_active_) {
            strategy_entry("S1", false);
        }
    }
};

void test_interior_fill_recalc_market_entry_waits_for_next_waypoint() {
    std::printf("test_interior_fill_recalc_market_entry_waits_for_next_waypoint\n");
    InteriorExitReentryCarryProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 102.0,  98.0, 101.0, 1000.0, 1'800'000},
        {100.0, 105.0,  90.0,  92.0, 1000.0, 2'700'000},
        {100.0, 101.0,  90.0,  96.0, 1000.0, 3'600'000},
    };
    p.run(bars, 4);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& first = p.get_trade(0);
        CHECK(near(first.entry_price, 100.0));
        CHECK(near(first.exit_price, 95.0));
        CHECK(first.entry_bar_index == 1);
        CHECK(first.exit_bar_index == 2);

        const Trade& carried = p.get_trade(1);
        CHECK(near(carried.entry_price, 90.0));
        CHECK(near(carried.exit_price, 95.0));
        CHECK(carried.entry_bar_index == 2);
        CHECK(carried.exit_bar_index == 3);
    }
}

// process_orders_on_close grants the same-tick close shortcut only at the
// bar's actual C execution. At an intrabar fill-recalc cursor, an ordinary
// close waits for the next waypoint; immediately=true remains selective and
// executes at the current cursor.
class PoocCursorTimingProbe final : public CoofBase {
public:
    explicit PoocCursorTimingProbe(bool immediate) : immediate_(immediate) {
        process_orders_on_close_ = true;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT) {
            strategy_entry("A", true, kNaN, 100.0);
            return;
        }
        if (bar_index_ != 1) return;
        if (position_entry_count_ == 1) {
            strategy_entry("B", true);
        } else if (position_entry_count_ == 2) {
            strategy_close("", "", kNaN, kNaN, immediate_);
        }
    }

private:
    bool immediate_;
};

void test_pooc_same_tick_requires_close_cursor_or_immediately() {
    std::printf("test_pooc_same_tick_requires_close_cursor_or_immediately\n");
    Bar bars[] = {
        {90.0, 95.0, 85.0, 90.0, 1000.0,   900'000},
        {100.0, 105.0, 90.0, 95.0, 1000.0, 1'800'000},
    };

    PoocCursorTimingProbe ordinary(false);
    ordinary.run(bars, 2);
    CHECK(ordinary.last_error().empty());
    CHECK(ordinary.trade_count() == 2);
    if (ordinary.trade_count() == 2) {
        CHECK(near(ordinary.get_trade(0).exit_price, 105.0));
        CHECK(near(ordinary.get_trade(1).exit_price, 105.0));
    }

    PoocCursorTimingProbe immediate(true);
    immediate.run(bars, 2);
    CHECK(immediate.last_error().empty());
    CHECK(immediate.trade_count() == 2);
    if (immediate.trade_count() == 2) {
        CHECK(near(immediate.get_trade(0).exit_price, 100.0));
        CHECK(near(immediate.get_trade(1).exit_price, 100.0));
    }
}

// A priced bracket born in an INTRABAR fill recalc is a KI-67 cascade EXIT and
// follows Model S ("R-cascade-gapjump"): held on its in-flight leg, then it
// gap-fills at that leg-end waypoint if its level is in the in-flight remainder,
// and EXACT-level fills on any subsequent leg. Entry stop L=105 fills mid-bar
// (path tie -> O=100,L=90,H=110,C=100 => O->L->H->C), so the exit's in-flight
// leg is L->H (90->110) and the subsequent leg is H->C (110->100).
//   sl=102: below the rising in-flight leg, but the reversed subsequent leg
//           110->100 crosses it — EXACT fill at 102, SAME bar (KI-67 residual
//           fix; pre-fix this rolled because only the W2=110 extreme was eligible).
//   tp=112: not in the in-flight remainder (105,110] and never reached on the
//           down subsequent leg — it rolls to the next bar (rises to 113 there).
class PoocIntrabarBracketProbe final : public CoofBase {
public:
    enum class Leg { STOP, LIMIT };
    explicit PoocIntrabarBracketProbe(Leg leg) : leg_(leg) {
        process_orders_on_close_ = true;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true, kNaN, 105.0);
        } else if (position_side_ == PositionSide::LONG) {
            if (leg_ == Leg::STOP) {
                strategy_exit("X", "L", kNaN, 102.0);
            } else {
                strategy_exit("X", "L", 112.0, kNaN);
            }
        }
    }

private:
    Leg leg_;
};

void test_pooc_intrabar_recalc_priced_order_uses_remaining_path() {
    std::printf("test_pooc_intrabar_recalc_priced_order_uses_remaining_path\n");
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0, 90.0, 100.0, 1000.0, 1'800'000},
        {103.0, 113.0, 95.0,  98.0, 1000.0, 2'700'000},
    };

    // Cascade sl=102 (KI-67 Model S): the subsequent leg H->C (110->100) crosses
    // it, so it EXACT-level fills at 102 on the SAME bar (bar 1), not at the
    // W2=110 extreme and not rolled to the next bar.
    PoocIntrabarBracketProbe stop(PoocIntrabarBracketProbe::Leg::STOP);
    stop.run(bars, 3);
    CHECK(stop.last_error().empty());
    CHECK(stop.trade_count() == 1);
    if (stop.trade_count() == 1) {
        CHECK(near(stop.get_trade(0).entry_price, 105.0));
        CHECK(near(stop.get_trade(0).exit_price, 102.0));
        CHECK(stop.get_trade(0).entry_bar_index == 1);
        CHECK(stop.get_trade(0).exit_bar_index == 1);   // KI-67 residual: was 2
    }

    // Cascade tp=112 is likewise unreachable at W2=110 on bar 1; it converts to
    // a resting limit and fills at 112 on bar 2 (which rises to 113), NOT at an
    // interpolated level on the bar-1 105->110 segment.
    PoocIntrabarBracketProbe limit(PoocIntrabarBracketProbe::Leg::LIMIT);
    limit.run(bars, 3);
    CHECK(limit.last_error().empty());
    CHECK(limit.trade_count() == 1);
    if (limit.trade_count() == 1) {
        CHECK(near(limit.get_trade(0).entry_price, 105.0));
        CHECK(near(limit.get_trade(0).exit_price, 112.0));
        CHECK(limit.get_trade(0).entry_bar_index == 1);
        CHECK(limit.get_trade(0).exit_bar_index == 2);
    }
}

// Generated classes own the concrete deep-copy representation. This manual
// analogue pins the engine's lifecycle: snapshot once; restore before every
// historical execution; commit only the last execution. Script state rolls
// back, while position/trades/orders remain live across recalc executions.
class RollbackProbe final : public CoofBase {
public:
    int script_scalar = 0;
    Series<int> script_series{32};
    std::vector<int> script_collection;

    int snapshot_calls = 0;
    int restore_calls = 0;
    int commit_calls = 0;
    std::vector<int> scalar_before_body;
    std::vector<int> body_bar;
    std::vector<bool> body_isnew;
    std::vector<bool> body_isconfirmed;

    void on_bar(const Bar&) override {
        scalar_before_body.push_back(script_scalar);
        body_bar.push_back(bar_index_);
        body_isnew.push_back(is_first_tick_);
        body_isconfirmed.push_back(is_last_tick_);

        ++script_scalar;
        if (history_advances_new_bar()) script_series.push(script_scalar);
        else script_series.update(script_scalar);
        script_collection.push_back(bar_index_);

        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
        } else if (position_side_ == PositionSide::LONG) {
            strategy_close("L");
        }
    }

protected:
    void snapshot_script_state() override {
        ++snapshot_calls;
        checkpoint_scalar_ = script_scalar;
        checkpoint_series_ = script_series;
        checkpoint_collection_ = script_collection;
    }

    void restore_script_state() override {
        ++restore_calls;
        script_scalar = checkpoint_scalar_;
        script_series = checkpoint_series_;
        script_collection = checkpoint_collection_;
    }

    void commit_script_state() override {
        ++commit_calls;
        checkpoint_scalar_ = script_scalar;
        checkpoint_series_ = script_series;
        checkpoint_collection_ = script_collection;
    }

private:
    int checkpoint_scalar_ = 0;
    Series<int> checkpoint_series_{32};
    std::vector<int> checkpoint_collection_;
};

void test_historical_barstate_and_committed_state_rollback_hooks() {
    std::printf("test_historical_barstate_and_committed_state_rollback_hooks\n");
    RollbackProbe p;
    auto bars = standard_feed();
    p.run(bars.data(), 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);  // broker state persisted through rollback
    CHECK(p.snapshot_calls == 2);
    CHECK(p.commit_calls == 2);
    // Every script execution restores its starting checkpoint.  The repaired
    // scheduler additionally restores the completed ordinary-close checkpoint
    // after post-C recalcs so speculative C state cannot become live state.
    CHECK(p.restore_calls == static_cast<int>(p.body_bar.size()) + 2);

    // Only one committed mutation per historical bar survives.
    CHECK(p.script_scalar == 2);
    CHECK(p.script_series.size() == 2);
    CHECK(p.script_series[0] == 2);
    CHECK(p.script_series[1] == 1);
    CHECK(p.script_collection.size() == 2);
    if (p.script_collection.size() == 2) {
        CHECK(p.script_collection[0] == 0);
        CHECK(p.script_collection[1] == 1);
    }

    int bar1_executions = 0;
    for (std::size_t i = 0; i < p.body_bar.size(); ++i) {
        CHECK(p.body_isnew[i]);
        CHECK(p.body_isconfirmed[i]);
        if (p.body_bar[i] == 1) {
            ++bar1_executions;
            CHECK(p.scalar_before_body[i] == 1);
        }
    }
    CHECK(bar1_executions == 3);  // entry fill, close fill, final close calc
}

// A fill produced by the ordinary process_orders_on_close pass occurs at the
// historical bar's terminal C tick. There is no later broker tick on which to
// run a fill-triggered body for that bar. In particular, such a body must not
// create a priced order that wakes over the next bar before its ordinary close
// execution can issue the durable order. This is the Fran470 production shape.
class PoocTerminalBracketProbe final : public CoofBase {
public:
    explicit PoocTerminalBracketProbe(bool is_long) : is_long_(is_long) {
        process_orders_on_close_ = true;
        pyramiding_ = 0;
    }

    void on_bar(const Bar&) override {
        const std::string entry_id = is_long_ ? "L" : "S";
        if (coof_fill_recalc_active_ && coof_cursor_is_bar_close_) {
            ++terminal_recalc_calls;
            if (position_side_ != PositionSide::FLAT) {
                strategy_exit("X", entry_id,
                              is_long_ ? 105.0 : 95.0, kNaN);
            }
            return;
        }
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry(entry_id, is_long_);
        } else if (bar_index_ == 1
                   && position_side_ != PositionSide::FLAT) {
            strategy_exit("X", entry_id, kNaN,
                          is_long_ ? 95.0 : 105.0);
        }
    }

    int terminal_recalc_calls = 0;

private:
    bool is_long_;
};

void test_pooc_terminal_close_fill_has_no_recalc_or_c_born_order() {
    std::printf(
        "test_pooc_terminal_close_fill_has_no_recalc_or_c_born_order\n");
    for (bool is_long : {true, false}) {
        PoocTerminalBracketProbe p(is_long);
        Bar bars[] = {
            {100.0, 110.0, 90.0, 100.0, 1000.0,   900'000},
            {100.0, 106.0, 94.0, 100.0, 1000.0, 1'800'000},
            {100.0, 106.0, 94.0, 100.0, 1000.0, 2'700'000},
        };
        p.run(bars, 3);

        CHECK(p.last_error().empty());
        CHECK(p.terminal_recalc_calls == 0);
        CHECK(p.trade_count() == 1);
        if (p.trade_count() == 1) {
            const Trade& t = p.get_trade(0);
            CHECK(t.entry_bar_index == 0);
            CHECK(t.exit_bar_index == 2);
            CHECK(near(t.entry_price, 100.0));
            CHECK(near(t.exit_price, is_long ? 95.0 : 105.0));
        }
        CHECK(near(p.signed_size(), 0.0));
    }
}

void test_magnifier_pooc_terminal_close_fill_has_no_recalc_or_c_born_order() {
    std::printf(
        "test_magnifier_pooc_terminal_close_fill_has_no_recalc_or_c_born_order\n");
    for (bool is_long : {true, false}) {
        PoocTerminalBracketProbe p(is_long);
        Bar lower[] = {
            {100.0, 105.0, 95.0, 102.0, 500.0,       0},
            {102.0, 110.0, 90.0, 100.0, 500.0,  60'000},
            {100.0, 103.0, 97.0, 101.0, 500.0, 120'000},
            {101.0, 106.0, 94.0, 100.0, 500.0, 180'000},
            {100.0, 103.0, 97.0, 101.0, 500.0, 240'000},
            {101.0, 106.0, 94.0, 100.0, 500.0, 300'000},
        };
        p.run(lower, 6, "1", "2", /*bar_magnifier=*/true,
              /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

        CHECK(p.last_error().empty());
        CHECK(p.terminal_recalc_calls == 0);
        CHECK(p.trade_count() == 1);
        if (p.trade_count() == 1) {
            const Trade& t = p.get_trade(0);
            CHECK(t.entry_bar_index == 0);
            CHECK(t.exit_bar_index == 2);
            CHECK(near(t.entry_price, 100.0));
            CHECK(near(t.exit_price, is_long ? 95.0 : 105.0));
        }
        CHECK(near(p.signed_size(), 0.0));
    }
}

// Per-trade excursion begins at a POOC entry's C fill. A fill-triggered body
// after that terminal tick would call update_per_trade_extremes() with the
// completed entry bar and retroactively count its pre-entry high/low.
class PoocTerminalExcursionProbe final : public CoofBase {
public:
    explicit PoocTerminalExcursionProbe(bool is_long) : is_long_(is_long) {
        process_orders_on_close_ = true;
        pyramiding_ = 0;
    }

    void on_bar(const Bar&) override {
        if (coof_fill_recalc_active_ && coof_cursor_is_bar_close_) {
            ++terminal_recalc_calls;
            return;
        }
        const std::string entry_id = is_long_ ? "L" : "S";
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry(entry_id, is_long_);
        } else if (bar_index_ == 1
                   && position_side_ != PositionSide::FLAT) {
            strategy_close(entry_id);
        }
    }

    int terminal_recalc_calls = 0;

private:
    bool is_long_;
};

void test_pooc_terminal_fill_does_not_backfill_entry_bar_excursion() {
    std::printf(
        "test_pooc_terminal_fill_does_not_backfill_entry_bar_excursion\n");
    for (bool is_long : {true, false}) {
        PoocTerminalExcursionProbe p(is_long);
        Bar bars[] = {
            {100.0, 120.0, 80.0, 100.0, 1000.0,   900'000},
            {100.0, 101.0, 99.0, 100.0, 1000.0, 1'800'000},
        };
        p.run(bars, 2);

        CHECK(p.last_error().empty());
        CHECK(p.terminal_recalc_calls == 0);
        CHECK(p.trade_count() == 1);
        if (p.trade_count() == 1) {
            const Trade& t = p.get_trade(0);
            CHECK(t.entry_bar_index == 0);
            CHECK(t.exit_bar_index == 1);
            CHECK(near(t.entry_price, 100.0));
            CHECK(near(t.exit_price, 100.0));
            CHECK(near(t.max_runup, 1.0));
            CHECK(near(t.max_drawdown, 1.0));
        }
    }
}

// Delta control: the ordinary close pass enters at C. Its next ordinary close
// pass closes at the next C; neither terminal fill triggers another body.
class PoocCloseCursorSingleUseProbe final : public CoofBase {
public:
    PoocCloseCursorSingleUseProbe() {
        process_orders_on_close_ = true;
        pyramiding_ = 0;
    }

    void on_bar(const Bar&) override {
        if (coof_fill_recalc_active_ && coof_cursor_is_bar_close_) {
            ++terminal_recalc_calls;
        }
        if (position_side_ == PositionSide::FLAT) {
            strategy_entry("L", true);
        } else {
            strategy_close("L");
        }
    }

    int terminal_recalc_calls = 0;
};

void test_delta_pooc_close_fills_are_terminal() {
    std::printf("test_delta_pooc_close_fills_are_terminal\n");
    PoocCloseCursorSingleUseProbe p;
    Bar bars[] = {
        {100.0, 110.0, 90.0, 104.0, 1000.0, 900'000},
        {104.0, 112.0, 98.0, 106.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.terminal_recalc_calls == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 104.0));
        CHECK(near(t.exit_price, 106.0));
    }
}

void test_delta_magnifier_pooc_close_fills_are_terminal() {
    std::printf(
        "test_delta_magnifier_pooc_close_fills_are_terminal\n");
    PoocCloseCursorSingleUseProbe p;
    Bar lower[] = {
        {100.0, 103.0,  99.0, 101.0, 500.0,       0},
        {101.0, 105.0, 100.0, 104.0, 500.0,  60'000},
        {104.0, 109.0, 103.0, 105.0, 500.0, 120'000},
        {105.0, 110.0, 102.0, 106.0, 500.0, 180'000},
    };
    p.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
          /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(p.last_error().empty());
    CHECK(p.terminal_recalc_calls == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 104.0));
        CHECK(near(t.exit_price, 106.0));
    }
}

// MrWick control: breakout/daily mutations and its bracket are issued by the
// ordinary C execution. They remain committed without a terminal fill body.
class PoocBreakoutStateScheduleProbe final : public CoofBase {
public:
    PoocBreakoutStateScheduleProbe() {
        process_orders_on_close_ = true;
        pyramiding_ = 0;
    }

    void on_bar(const Bar&) override {
        if (coof_fill_recalc_active_ && coof_cursor_is_bar_close_) {
            ++terminal_recalc_calls;
        }
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && !first_breakout_seen) {
            first_breakout_seen = true;
            continuation_taken = true;
            breakout_direction = 1;
            strategy_entry("L", true);
        }
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG
            && first_breakout_seen && continuation_taken
            && breakout_direction == 1) {
            strategy_close("L");
        }
    }

    bool first_breakout_seen = false;
    bool continuation_taken = false;
    int breakout_direction = 0;
    int terminal_recalc_calls = 0;

protected:
    void snapshot_script_state() override {
        checkpoint_first_breakout_seen_ = first_breakout_seen;
        checkpoint_continuation_taken_ = continuation_taken;
        checkpoint_breakout_direction_ = breakout_direction;
    }
    void restore_script_state() override {
        first_breakout_seen = checkpoint_first_breakout_seen_;
        continuation_taken = checkpoint_continuation_taken_;
        breakout_direction = checkpoint_breakout_direction_;
    }
    void commit_script_state() override {
        snapshot_script_state();
    }

private:
    bool checkpoint_first_breakout_seen_ = false;
    bool checkpoint_continuation_taken_ = false;
    int checkpoint_breakout_direction_ = 0;
};

void test_mrwick_ordinary_close_state_survives_without_terminal_recalc() {
    std::printf(
        "test_mrwick_ordinary_close_state_survives_without_terminal_recalc\n");
    PoocBreakoutStateScheduleProbe p;
    Bar bars[] = {
        {100.0, 110.0, 90.0, 104.0, 1000.0, 900'000},
        {104.0, 112.0, 98.0, 106.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.terminal_recalc_calls == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 104.0));
        CHECK(near(t.exit_price, 106.0));
    }
    CHECK(p.first_breakout_seen);
    CHECK(p.continuation_taken);
    CHECK(p.breakout_direction == 1);
}

// Wayward control: a close and opposite entry emitted by the one ordinary C
// execution are siblings at the same live broker epoch and both fill there.
class PoocOrdinaryCloseReversalSiblingProbe final : public CoofBase {
public:
    PoocOrdinaryCloseReversalSiblingProbe() {
        process_orders_on_close_ = true;
        pyramiding_ = 0;
    }

    void on_bar(const Bar&) override {
        if (coof_fill_recalc_active_ && coof_cursor_is_bar_close_) {
            ++terminal_recalc_calls;
        }
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
            return;
        }
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG
            && !coof_fill_recalc_active_) {
            strategy_close("L");
            strategy_entry("S", false);
        }
    }

    int terminal_recalc_calls = 0;
};

void test_pooc_ordinary_close_reversal_siblings_share_live_c() {
    std::printf("test_pooc_ordinary_close_reversal_siblings_share_live_c\n");
    PoocOrdinaryCloseReversalSiblingProbe p;
    Bar bars[] = {
        {100.0, 110.0, 90.0, 104.0, 1000.0, 900'000},
        {104.0, 112.0, 98.0, 106.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.terminal_recalc_calls == 0);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.entry_price, 104.0));
        CHECK(near(t.exit_price, 106.0));
    }
    CHECK(near(p.signed_size(), -1.0));
}

// KI-67: TradingView applies NO per-bar fill-event budget. A carried five-unit
// entry fills at O and each fill recalc closes one more unit immediately; with
// the fixed 4-event cap removed, all five one-unit closes execute and the
// position ends flat (the old budget stopped after three, leaving 2 units).
// This control is deliberately non-POOC: its carried entry fills at O, so all
// recalculations occur before the terminal close phase.
class RecalcChainBudgetProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (position_side_ == PositionSide::FLAT && trades_.empty()) {
            strategy_entry("L", true, kNaN, kNaN, 5.0);
        } else if (position_side_ == PositionSide::LONG) {
            strategy_close("L", "", 1.0, kNaN, /*immediately=*/true);
        }
    }
};

void test_intrabar_direct_fill_from_last_recalc_respects_event_budget() {
    std::printf(
        "test_intrabar_direct_fill_from_last_recalc_respects_event_budget\n");
    RecalcChainBudgetProbe p;
    Bar bars[] = {
        {100.0, 101.0, 99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0, 90.0, 104.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    // KI-67: no fill-event budget — all five one-unit closes execute (was 3).
    CHECK(p.trade_count() == 5);
    CHECK(near(p.signed_size(), 0.0));
}

struct IdentitySnapshot {
    std::vector<Trade> trades;
    double signed_size = 0.0;
    int body_calls = 0;
    int snapshot_calls = 0;
    int restore_calls = 0;
    int commit_calls = 0;
};

class FalsePathProbe final : public CoofBase {
public:
    explicit FalsePathProbe(bool enabled) : CoofBase(enabled) {}

    void on_bar(const Bar&) override {
        ++body_calls;
        if (bar_index_ == 0) strategy_entry("L", true);
        if (position_side_ == PositionSide::LONG && bar_index_ >= 1) {
            strategy_close("L");
        }
    }

    IdentitySnapshot result() const {
        IdentitySnapshot out;
        for (int i = 0; i < trade_count(); ++i) out.trades.push_back(get_trade(i));
        out.signed_size = signed_position_size();
        out.body_calls = body_calls;
        out.snapshot_calls = snapshot_calls;
        out.restore_calls = restore_calls;
        out.commit_calls = commit_calls;
        return out;
    }

    int body_calls = 0;
    int snapshot_calls = 0;
    int restore_calls = 0;
    int commit_calls = 0;

protected:
    void snapshot_script_state() override { ++snapshot_calls; }
    void restore_script_state() override { ++restore_calls; }
    void commit_script_state() override { ++commit_calls; }
};

bool identical_trade(const Trade& a, const Trade& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && a.entry_bar_index == b.entry_bar_index
        && a.exit_bar_index == b.exit_bar_index
        && a.is_long == b.is_long && a.entry_id == b.entry_id
        && a.exit_id == b.exit_id && a.entry_comment == b.entry_comment
        && a.exit_comment == b.exit_comment && a.entry_price == b.entry_price
        && a.exit_price == b.exit_price && a.qty == b.qty && a.pnl == b.pnl
        && a.pnl_pct == b.pnl_pct && a.max_runup == b.max_runup
        && a.max_drawdown == b.max_drawdown && a.commission == b.commission;
}

void test_false_flag_path_is_legacy_identical_and_never_calls_hooks() {
    std::printf("test_false_flag_path_is_legacy_identical_and_never_calls_hooks\n");
    FalsePathProbe default_false(false);
    FalsePathProbe explicit_false(false);
    auto bars = standard_feed();
    default_false.run(bars.data(), static_cast<int>(bars.size()));

    StrategyOverrides ov;
    ov.calc_on_order_fills = 0;
    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;
    explicit_false.run(bars.data(), static_cast<int>(bars.size()),
                       "15", "15", inputs, sym, &ov);

    const IdentitySnapshot a = default_false.result();
    const IdentitySnapshot b = explicit_false.result();
    CHECK(a.trades.size() == b.trades.size());
    for (std::size_t i = 0; i < a.trades.size() && i < b.trades.size(); ++i) {
        CHECK(identical_trade(a.trades[i], b.trades[i]));
    }
    CHECK(a.signed_size == b.signed_size);
    CHECK(a.body_calls == static_cast<int>(bars.size()));
    CHECK(b.body_calls == static_cast<int>(bars.size()));
    CHECK(a.snapshot_calls == 0 && a.restore_calls == 0 && a.commit_calls == 0);
    CHECK(b.snapshot_calls == 0 && b.restore_calls == 0 && b.commit_calls == 0);
}

void test_strategy_override_can_enable_and_disable_coof() {
    std::printf("test_strategy_override_can_enable_and_disable_coof\n");
    auto bars = standard_feed();
    std::unordered_map<std::string, std::string> inputs;
    SymInfo sym;

    MarketCloseProbe enabled_by_override(false);
    StrategyOverrides on;
    on.calc_on_order_fills = 1;
    enabled_by_override.run(bars.data(), static_cast<int>(bars.size()),
                            "15", "15", inputs, sym, &on);
    CHECK(enabled_by_override.coof_enabled());
    CHECK(enabled_by_override.trade_count() == 1);
    if (enabled_by_override.trade_count() == 1) {
        CHECK(enabled_by_override.get_trade(0).entry_bar_index
              == enabled_by_override.get_trade(0).exit_bar_index);
    }

    MarketCloseProbe disabled_by_override(true);
    StrategyOverrides off;
    off.calc_on_order_fills = 0;
    disabled_by_override.run(bars.data(), static_cast<int>(bars.size()),
                             "15", "15", inputs, sym, &off);
    CHECK(!disabled_by_override.coof_enabled());
    CHECK(disabled_by_override.trade_count() == 1);
    if (disabled_by_override.trade_count() == 1) {
        CHECK(disabled_by_override.get_trade(0).exit_bar_index
              > disabled_by_override.get_trade(0).entry_bar_index);
    }
}

}  // namespace

int main() {
    test_market_close_fills_same_bar_at_entry_price();
    test_recalc_bracket_uses_remaining_path();
    test_historical_refill_is_exact_o_o_near_far_and_capped_at_four();
    test_non_open_endpoint_fill_consumes_point_before_market_add();
    test_magnifier_endpoint_fill_consumes_tick_before_market_add();
    test_real_magnifier_gap_fills_limit_at_fresh_subbar_open();
    test_real_magnifier_gap_fills_stop_at_fresh_subbar_open();
    test_exit_fills_consume_historical_event_budget();
    test_same_segment_priced_orders_fill_nearest_first();
    test_stop_limit_activation_survives_segment_split();
    test_stop_limit_activation_commits_only_through_consumed_cursor();
    test_fill_recalc_priced_entries_bypass_legacy_bar_throttle();
    test_recalc_priced_entry_exemption_expires_after_creation_bar();
    test_close_cleanup_does_not_leak_into_new_position_cycle();
    test_recalc_wrong_side_entry_bar_legs_carry_to_next_bar();
    test_recalc_wrong_stop_does_not_hide_valid_limit_leg();
    test_recalc_wrong_limit_does_not_hide_valid_stop_leg();
    test_interior_fill_recalc_market_entry_waits_for_next_waypoint();
    test_pooc_same_tick_requires_close_cursor_or_immediately();
    test_pooc_intrabar_recalc_priced_order_uses_remaining_path();
    test_historical_barstate_and_committed_state_rollback_hooks();
    test_pooc_terminal_close_fill_has_no_recalc_or_c_born_order();
    test_magnifier_pooc_terminal_close_fill_has_no_recalc_or_c_born_order();
    test_pooc_terminal_fill_does_not_backfill_entry_bar_excursion();
    test_delta_pooc_close_fills_are_terminal();
    test_delta_magnifier_pooc_close_fills_are_terminal();
    test_mrwick_ordinary_close_state_survives_without_terminal_recalc();
    test_pooc_ordinary_close_reversal_siblings_share_live_c();
    test_intrabar_direct_fill_from_last_recalc_respects_event_budget();
    test_false_flag_path_is_legacy_identical_and_never_calls_hooks();
    test_strategy_override_can_enable_and_disable_coof();

    if (tests_failed == 0) {
        std::printf("test_calc_on_order_fills PASSED (%d checks)\n", tests_passed);
        return 0;
    }
    std::printf("test_calc_on_order_fills FAILED (%d failed, %d passed)\n",
                tests_failed, tests_passed);
    return 1;
}
