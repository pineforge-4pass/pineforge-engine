// A later same-id entry on a flat book under calc_on_order_fills replaces the
// earlier priced entry of that id issued in the same script evaluation.
//
// The probe: pyramiding = 1, calc_on_order_fills = true, close_entries_rule
// "ANY", fixed default quantity 1, initial capital 1,000,000, no commission,
// no slippage. Bar 10 enters "reused" short with quantity 1; bar 14 closes
// "reused"; bar 20 enters "reused" short twice, first with a sell limit of
// quantity 7 at 191 that the feed never reaches, then at market with quantity
// 2; bar 30 closes the whole position. The probe acts only in each bar's own
// calculation, never in a recalculation after a fill. On bar 20 the book is
// flat, so the evaluation holds the priced call back until it ends, while the
// later market call reaches the order core first. When the evaluation ends,
// the held call must not replace the later one.
//
// Rows and where their expectations come from:
//
// 1. The old id closed (close_old = true), process_orders_on_close false and
//    true. Both rows are matched by TradingView captures of the equivalent
//    Pine script: sha256
//    cd650d6c915c97e77aa820274538ecc49ee4a11b88126ff3ec2dc0ab28462e5f
//    for process_orders_on_close = false and
//    9b1c1c3227292f3f0318e8b59198370055b4f0978d5f6ff689018fd2065ae4ed
//    for process_orders_on_close = true. TradingView shows two short
//    "reused" trades, quantity 1 and then quantity 2, the second entering at
//    the bar-21 open (at the bar-20 close when orders process on close). The
//    Pine script's bar-20 limit is close * 10, which its chart never reaches;
//    the quantity-7 limit at 191 in this probe stands for that never-reached
//    limit. The Pine script ends with strategy.close_all(); the probe's
//    empty-id strategy_close is the same full-position close. The prices
//    checked here are the constructed feed's, derived by hand below.
//    That no order is pending at bar 25 is derived, not read from the
//    captures: Pine keeps one order per id, so the bar-20 market call
//    replaced the limit, and that market order filled at the bar-21 open
//    (bar-20 close).
//
// 2. The old id still open (close_old = false), process_orders_on_close false
//    and true: hand-derived controls with no TradingView capture. The bar-10
//    short holds the only pyramiding slot, so neither bar-20 entry adds to
//    it: one trade, the short of 1 held from bar 10 to the bar-30 close.
//
// 3. Metamorphic controls that need no TradingView claim. Each changes row 1
//    in a way that must leave its trades as they are:
//    (a) bar 20 issues only the market call. Row 1's limit is replaced by
//        that call, so without the limit the orders that can fill are the
//        same: the same two trades, nothing pending at bar 25.
//    (b) calc_on_order_fills off. Switching it off only removes the
//        recalculations after fills, and the probe places nothing in those,
//        so the script places the same orders: the same two trades, nothing
//        pending at bar 25.
//    (c) the two bar-20 calls use different ids, a limit "A" of 7 at 191 and
//        a market "B" of 2. Orders with different ids never replace each
//        other: "B" fills where row 1's successor fills, and "A" is never
//        reached, so at bar 25 it is the one pending order, a limit at 191.
//
// Hand derivation from the constructed feed. Bar i (0 to 39) opens at
// 100 + 2i, has high = open + 3, low = open - 2 and closes at open + 1; bar
// times are kStart + i hours. The highest high is bar 39's 178 + 3 = 181, so a
// sell limit at 191 (it fills only at 191 or higher) is never reached.
//
//   process_orders_on_close = false: an order placed on bar k fills at the
//   open of bar k + 1, 100 + 2 (k + 1).
//     bar-10 entry -> bar 11 open, 100 + 22 = 122
//     bar-14 close -> bar 15 open, 100 + 30 = 130
//     bar-20 entry -> bar 21 open, 100 + 42 = 142
//     bar-30 close -> bar 31 open, 100 + 62 = 162
//   process_orders_on_close = true: it fills at the close of bar k, 101 + 2k.
//     bar-10 entry -> 121, bar-14 close -> 129,
//     bar-20 entry -> 141, bar-30 close -> 161
//
//   With no commission, a short's profit is (entry - exit) x quantity:
//     old short of 1:        (122 - 130) x 1 = -8   or (121 - 129) x 1 = -8
//     successor short of 2:  (142 - 162) x 2 = -40  or (141 - 161) x 2 = -40
//     control short of 1:    (122 - 162) x 1 = -40  or (121 - 161) x 1 = -40
//
//   At the bar-20 calculation the old short is closed in rows 1 and 3
//   (size 0, no open lot) and still open in the controls (size -1, one lot).
//   At bar 25 the successor short of 2 is one open lot (size -2) in rows 1
//   and 3, and the control's short of 1 is one lot (size -1). A short of
//   more than one lot never exists, and every row is flat at the end.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1'704'067'200'000LL;
constexpr std::int64_t kHour = 3'600'000;
int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);      \
        }                                                                      \
    } while (false)

std::vector<Bar> rising_feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 40; ++i) {
        const double open = 100.0 + 2.0 * i;
        bars.push_back({open, open + 3.0, open - 2.0, open + 1.0, 1000.0,
                        kStart + i * kHour});
    }
    return bars;
}

class SlotProbe : public source::PineStrategyHost {
public:
    SlotProbe(bool on_close, int cap, bool calc_on_fills = true) {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = cap;
        cfg.calc_on_order_fills = calc_on_fills;
        cfg.process_orders_on_close = on_close;
        cfg.close_entries_rule_any = true;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 0.001);
    }

    bool ordinary_calculation() const {
        const auto point = current_execution_point();
        return point
            && point->decision.coordinate.provenance == NativePriceProvenance::Calculation;
    }

    void observe_slots() {
        if (position_side_ == PositionSide::SHORT)
            max_short_lots = std::max(max_short_lots, pyramid_entries_.size());
        if (pine_bar_index() == 25 && ordinary_calculation()) {
            saw_bar25 = true;
            bar25_size = signed_position_size();
            bar25_lots = pyramid_entries_.size();
            bar25_pending = pending_order_count();
            bar25_pending_limit = kNaN;
            bar25_pending_stop = kNaN;
            if (bar25_pending == 1) {
                double trail_activation = kNaN;
                if (pending_order_effective_levels(0, &bar25_pending_stop,
                                                   &bar25_pending_limit,
                                                   &trail_activation) != 0) {
                    bar25_pending_limit = kNaN;
                    bar25_pending_stop = kNaN;
                }
            }
        }
    }

    double final_size() const { return signed_position_size(); }
    std::size_t max_short_lots = 0;
    bool saw_bar25 = false;
    double bar25_size = kNaN;
    std::size_t bar25_lots = 0;
    int bar25_pending = -1;
    double bar25_pending_limit = kNaN;
    double bar25_pending_stop = kNaN;
};

// Rows 1 and 2, and metamorphic control (b) with calc_on_fills = false. The
// bar callback is the call sequence the TradingView-matched rows describe.
class ReusedIdSlotProbe final : public SlotProbe {
public:
    ReusedIdSlotProbe(bool on_close, bool close_old, bool calc_on_fills = true)
        : SlotProbe(on_close, 1, calc_on_fills), close_old_(close_old) {}

    void on_source_bar(const Bar&) override {
        observe_slots();
        if (!ordinary_calculation()) return;
        const int i = pine_bar_index();
        if (i == 10)
            strategy_entry("reused", false, kNaN, kNaN, 1.0);
        if (i == 14 && close_old_)
            strategy_close("reused", "", kNaN, kNaN, false, 1);
        if (i == 20) {
            before_reentry_size = signed_position_size();
            before_reentry_lots = pyramid_entries_.size();
            strategy_entry("reused", false, 191.0, kNaN, 7.0);
            strategy_entry("reused", false, kNaN, kNaN, 2.0);
        }
        if (i == 30)
            strategy_close("", "", kNaN, kNaN, false);
        observe_slots();
    }

    double before_reentry_size = kNaN;
    std::size_t before_reentry_lots = 0;

private:
    bool close_old_;
};

// Metamorphic control (a): row 1 without the bar-20 limit call.
class MarketOnlyProbe final : public SlotProbe {
public:
    explicit MarketOnlyProbe(bool on_close) : SlotProbe(on_close, 1) {}

    void on_source_bar(const Bar&) override {
        observe_slots();
        if (!ordinary_calculation()) return;
        const int i = pine_bar_index();
        if (i == 10)
            strategy_entry("reused", false, kNaN, kNaN, 1.0);
        if (i == 14)
            strategy_close("reused", "", kNaN, kNaN, false, 1);
        if (i == 20)
            strategy_entry("reused", false, kNaN, kNaN, 2.0);
        if (i == 30)
            strategy_close("", "", kNaN, kNaN, false);
        observe_slots();
    }
};

// Metamorphic control (c): row 1 with the two bar-20 calls under two new ids.
class DistinctIdProbe final : public SlotProbe {
public:
    explicit DistinctIdProbe(bool on_close) : SlotProbe(on_close, 1) {}

    void on_source_bar(const Bar&) override {
        observe_slots();
        if (!ordinary_calculation()) return;
        const int i = pine_bar_index();
        if (i == 10)
            strategy_entry("reused", false, kNaN, kNaN, 1.0);
        if (i == 14)
            strategy_close("reused", "", kNaN, kNaN, false, 1);
        if (i == 20) {
            strategy_entry("A", false, 191.0, kNaN, 7.0);
            strategy_entry("B", false, kNaN, kNaN, 2.0);
        }
        if (i == 30)
            strategy_close("", "", kNaN, kNaN, false);
        observe_slots();
    }
};

struct ExpectedTrade {
    const char* id;
    double qty;
    int entry_bar;
    double entry_price;
    int exit_bar;
    double exit_price;
    double profit;
};

// The old "reused" short of 1, opened on bar 10 and closed on bar 14.
ExpectedTrade old_short(bool on_close) {
    return on_close ? ExpectedTrade{"reused", 1.0, 10, 121.0, 14, 129.0, -8.0}
                    : ExpectedTrade{"reused", 1.0, 11, 122.0, 15, 130.0, -8.0};
}

// The bar-20 market short of 2, closed by the bar-30 full close.
ExpectedTrade successor_short(bool on_close, const char* id) {
    return on_close ? ExpectedTrade{id, 2.0, 20, 141.0, 30, 161.0, -40.0}
                    : ExpectedTrade{id, 2.0, 21, 142.0, 31, 162.0, -40.0};
}

// The controls' bar-10 short of 1, held to the bar-30 full close.
ExpectedTrade held_short(bool on_close) {
    return on_close ? ExpectedTrade{"reused", 1.0, 10, 121.0, 30, 161.0, -40.0}
                    : ExpectedTrade{"reused", 1.0, 11, 122.0, 31, 162.0, -40.0};
}

void check_trade(const SlotProbe& p, int index, const ExpectedTrade& want) {
    CHECK(index < p.trade_count());
    if (index >= p.trade_count()) return;
    const Trade trade = p.get_trade(index);
    std::printf("  trade %d: %s %s qty %.17g, entry bar %d at %.17g, "
                "exit bar %d at %.17g, profit %.17g\n",
                index, trade.entry_id.c_str(), trade.is_long ? "long" : "short",
                trade.qty, trade.entry_bar_index, trade.entry_price,
                trade.exit_bar_index, trade.exit_price, trade.pnl);
    CHECK(trade.entry_id == want.id);
    CHECK(!trade.is_long);
    CHECK(trade.qty == want.qty);
    CHECK(trade.entry_bar_index == want.entry_bar);
    CHECK(trade.entry_price == want.entry_price);
    CHECK(trade.exit_bar_index == want.exit_bar);
    CHECK(trade.exit_price == want.exit_price);
    CHECK(trade.pnl == want.profit);
    CHECK(!trade.open_at_end);
}

void print_book(const SlotProbe& p) {
    std::printf("  trades %d; bar 25 size %.17g, lots %zu, pending %d; "
                "max short lots %zu; final size %.17g\n",
                p.trade_count(), p.bar25_size, p.bar25_lots, p.bar25_pending,
                p.max_short_lots, p.final_size());
}

// Metamorphic controls (a), (b) and (c): row 1's trades, the old short of 1
// and then the bar-20 market short of 2 (id "reused", or "B" in (c)).
void check_reentry_trades(const SlotProbe& p, bool on_close, const char* successor_id) {
    CHECK(p.trade_count() == 2);
    check_trade(p, 0, old_short(on_close));
    check_trade(p, 1, successor_short(on_close, successor_id));
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == -2.0);
    CHECK(p.bar25_lots == 1U);
    CHECK(p.max_short_lots == 1U);
    CHECK(p.final_size() == 0.0);
}

// Rows 1 and 2.
void check_closed_incarnation(bool on_close, bool close_old) {
    std::printf("reused id, close processing %d, explicit old close %d\n",
                on_close, close_old);
    ReusedIdSlotProbe p(on_close, close_old);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    const int expected_trades = close_old ? 2 : 1;
    CHECK(p.trade_count() == expected_trades);
    std::printf("  trades %d (hand-derived %d); before reentry %.17g / %zu lots; "
                "bar 25 size %.17g, lots %zu\n",
                p.trade_count(), expected_trades,
                p.before_reentry_size, p.before_reentry_lots,
                p.bar25_size, p.bar25_lots);
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == (close_old ? -2.0 : -1.0));
    CHECK(p.bar25_lots == 1U);
    CHECK(p.max_short_lots == 1U);
    CHECK(p.final_size() == 0.0);
    CHECK(p.before_reentry_size == (close_old ? 0.0 : -1.0));
    CHECK(p.before_reentry_lots == (close_old ? 0U : 1U));
    if (close_old) {
        check_trade(p, 0, old_short(on_close));
        check_trade(p, 1, successor_short(on_close, "reused"));
        CHECK(p.bar25_pending == 0);
    } else {
        check_trade(p, 0, held_short(on_close));
    }
    print_book(p);
}

// Metamorphic control (a).
void check_market_only(bool on_close) {
    std::printf("market call only, close processing %d\n", on_close);
    MarketOnlyProbe p(on_close);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    check_reentry_trades(p, on_close, "reused");
    CHECK(p.bar25_pending == 0);
    print_book(p);
}

// Metamorphic control (b).
void check_without_fill_recalculation(bool on_close) {
    std::printf("reused id without fill recalculation, close processing %d\n",
                on_close);
    ReusedIdSlotProbe p(on_close, true, false);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    CHECK(p.before_reentry_size == 0.0);
    CHECK(p.before_reentry_lots == 0U);
    check_reentry_trades(p, on_close, "reused");
    CHECK(p.bar25_pending == 0);
    print_book(p);
}

// Metamorphic control (c).
void check_distinct_ids(bool on_close) {
    std::printf("distinct ids, close processing %d\n", on_close);
    DistinctIdProbe p(on_close);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    check_reentry_trades(p, on_close, "B");
    CHECK(p.bar25_pending == 1);
    CHECK(p.bar25_pending_limit == 191.0);
    CHECK(std::isnan(p.bar25_pending_stop));
    print_book(p);
}

}  // namespace

int main() {
    for (bool on_close : {false, true}) {
        check_closed_incarnation(on_close, true);
        check_closed_incarnation(on_close, false);
        check_market_only(on_close);
        check_without_fill_recalculation(on_close);
        check_distinct_ids(on_close);
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
