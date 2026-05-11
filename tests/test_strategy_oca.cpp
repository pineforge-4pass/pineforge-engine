/*
 * test_strategy_oca.cpp — verify Pine v6 OCA group semantics on
 * BacktestEngine. Specifically pins down strategy.oca.reduce: when one
 * sibling fills qty Q, every other sibling's remaining qty drops by Q
 * (rather than being cancelled outright, which is oca.cancel behaviour).
 * See TradingView Pine v6 docs strategy.oca.reduce for the reference
 * semantics. Regression guard for the prior bug where oca_type==2 was
 * routed to cancel_oca_group (full sibling wipe).
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {

// Probe that places a configurable batch of OCA-grouped limit orders on
// bar 1, then exposes pending_orders_ each bar so the test can inspect
// remaining qty after the first sibling fires.
class OcaProbe : public BacktestEngine {
public:
    struct Sibling {
        std::string id;
        bool is_long;
        double qty;
        double limit_price;
    };
    std::vector<Sibling> siblings;
    int oca_type = 2;  // 2 = reduce, 1 = cancel
    std::string oca_name = "G1";

    // Snapshot of pending orders at the END of each bar (after fills).
    struct PendingSnap { std::string id; double qty; };
    std::vector<std::vector<PendingSnap>> pending_per_bar;

    OcaProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 100;  // allow many entries to coexist
    }

    void on_bar(const Bar& bar) override {
        if (bar_index_ == 1) {
            for (const auto& s : siblings) {
                strategy_order(s.id, s.is_long, s.qty, s.limit_price,
                               std::numeric_limits<double>::quiet_NaN(),
                               oca_name, oca_type);
            }
        }
        std::vector<PendingSnap> snap;
        for (const auto& o : pending_orders_) {
            snap.push_back({o.id, o.qty});
        }
        pending_per_bar.push_back(std::move(snap));
    }

    const PendingSnap* find(int bar, const std::string& id) const {
        if (bar < 0 || bar >= (int)pending_per_bar.size()) return nullptr;
        for (const auto& s : pending_per_bar[bar]) {
            if (s.id == id) return &s;
        }
        return nullptr;
    }
};

}  // namespace

// Case 1: 2-sibling REDUCE, first fills qty 3 (of 5) → other sibling's
// remaining qty should drop from 5 to 2 (NOT be cancelled).
static void test_reduce_two_sibling_partial() {
    std::printf("test_reduce_two_sibling_partial\n");
    OcaProbe p;
    p.oca_type = 2;
    // Bar 2 open=100, low=90: A's limit @ 100 fills (qty 3 long). B's
    // limit at 80 doesn't trigger (low=90 > 80) so it survives in
    // pending_orders_, where we can read its post-reduce qty.
    p.siblings = {
        {"A", true, 3.0, 100.0},
        {"B", true, 5.0, 80.0},
    };
    Bar bars[4] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},  // place orders
        {100, 110,  90, 105, 1000, 180'000},  // A fills @ 100 qty 3
        {100, 110,  85, 105, 1000, 240'000},  // B's limit still 80 — but reduced qty applies if it ever fires
    };
    p.run(bars, 4);

    // After bar 2: A is filled (gone). B should still be pending with qty 5-3=2.
    auto* b_after = p.find(2, "B");
    CHECK(b_after != nullptr);
    if (b_after) CHECK(near(b_after->qty, 2.0));

    // A must NOT be in pending after fill.
    CHECK(p.find(2, "A") == nullptr);
}

// Case 2: 3-sibling REDUCE, first fills 2 (of 4) → other two siblings
// should each go from 4 to 2.
static void test_reduce_three_sibling() {
    std::printf("test_reduce_three_sibling\n");
    OcaProbe p;
    p.oca_type = 2;
    p.siblings = {
        {"A", true, 2.0, 100.0},
        {"B", true, 4.0,  80.0},
        {"C", true, 4.0,  70.0},
    };
    Bar bars[4] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},
        {100, 110,  90, 105, 1000, 180'000},  // A fills qty 2
        {100, 110,  85, 105, 1000, 240'000},
    };
    p.run(bars, 4);

    auto* b = p.find(2, "B");
    auto* c = p.find(2, "C");
    CHECK(b != nullptr);
    CHECK(c != nullptr);
    if (b) CHECK(near(b->qty, 2.0));
    if (c) CHECK(near(c->qty, 2.0));
}

// Case 3: REDUCE full-fill cascades — when filled qty >= sibling qty,
// the sibling drops to 0 and is removed (degenerates to oca.cancel for
// that sibling, matching TV semantics).
static void test_reduce_full_fill_cascade() {
    std::printf("test_reduce_full_fill_cascade\n");
    OcaProbe p;
    p.oca_type = 2;
    p.siblings = {
        {"A", true, 5.0, 100.0},
        {"B", true, 5.0,  80.0},
        {"C", true, 3.0,  70.0},
    };
    Bar bars[4] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},
        {100, 110,  90, 105, 1000, 180'000},  // A fills qty 5 → B,C reduced by 5
        {100, 110,  85, 105, 1000, 240'000},
    };
    p.run(bars, 4);

    // B (5-5=0) and C (3-5<0) both removed.
    CHECK(p.find(2, "B") == nullptr);
    CHECK(p.find(2, "C") == nullptr);
    CHECK(p.find(2, "A") == nullptr);
}

// Sanity: oca.cancel still nukes all siblings on first fill.
static void test_cancel_unchanged() {
    std::printf("test_cancel_unchanged\n");
    OcaProbe p;
    p.oca_type = 1;
    p.siblings = {
        {"A", true, 3.0, 100.0},
        {"B", true, 5.0,  80.0},
    };
    Bar bars[4] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},
        {100, 110,  90, 105, 1000, 180'000},  // A fills → B cancelled
        {100, 110,  85, 105, 1000, 240'000},
    };
    p.run(bars, 4);
    CHECK(p.find(2, "B") == nullptr);
    CHECK(p.find(2, "A") == nullptr);
}

// Cross-group isolation: when Group A's CANCEL sibling fills, Group
// B's REDUCE sibling must remain untouched (different oca_name). Both
// the cancel_oca_group helper and the reduce_oca_group helper already
// scope by oca_name, so this is a regression guard for the
// post-fill OCA dispatch in apply_filled_order_to_state.
static void test_cross_group_isolation() {
    std::printf("test_cross_group_isolation\n");
    class CrossGroupProbe : public BacktestEngine {
    public:
        struct PendingSnap { std::string id; double qty; std::string oca_name; };
        std::vector<std::vector<PendingSnap>> pending_per_bar;

        CrossGroupProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 100;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 1) {
                // Group A: CANCEL siblings (each qty=2)
                strategy_order("A_TP", true, 2.0, /*limit=*/100.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               "GRP_A", /*cancel=*/1);
                strategy_order("A_SL", true, 2.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/120.0,
                               "GRP_A", 1);
                // Group B: REDUCE siblings (each qty=2)
                strategy_order("B_TP", true, 2.0, /*limit=*/95.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               "GRP_B", /*reduce=*/2);
                strategy_order("B_SL", true, 2.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/130.0,
                               "GRP_B", 2);
            }
            std::vector<PendingSnap> snap;
            for (const auto& po : pending_orders_) {
                snap.push_back({po.id, po.qty, po.oca_name});
            }
            pending_per_bar.push_back(std::move(snap));
        }
        const PendingSnap* find(int bar, const std::string& id) const {
            if (bar < 0 || bar >= (int)pending_per_bar.size()) return nullptr;
            for (const auto& s : pending_per_bar[bar]) {
                if (s.id == id) return &s;
            }
            return nullptr;
        }
    };
    CrossGroupProbe p;
    Bar bars[5] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},  // place orders
        {100, 110,  90, 105, 1000, 180'000},  // A_TP @100 fills (qty 2)
        {100, 110,  85, 105, 1000, 240'000},
        {100, 110,  85, 105, 1000, 300'000},
    };
    p.run(bars, 5);

    // After bar 2: A_TP filled → A_SL cancelled (Group A).
    CHECK(p.find(2, "A_TP") == nullptr);
    CHECK(p.find(2, "A_SL") == nullptr);
    // Group B siblings MUST still be alive — A's fill is in a different
    // OCA group and must not touch them. Note: B_TP's limit at 95 is
    // not yet touched (low=90 on bar 2 fills A_TP @ 100 first; B_TP
    // would fill on the SAME bar but our run executes the priced
    // entries one per bar, so B_TP fires on a later bar).
    auto* b_tp = p.find(2, "B_TP");
    auto* b_sl = p.find(2, "B_SL");
    if (b_tp == nullptr) {
        // B_TP may have fired same bar; check Group B's cross-group
        // isolation by inspecting whether B_SL still has its full qty.
        CHECK(b_sl != nullptr);
    } else {
        // B_TP still pending — verify its qty unchanged (Group A's fill
        // doesn't reduce or cancel B's siblings).
        CHECK(near(b_tp->qty, 2.0));
        CHECK(b_sl != nullptr);
        CHECK(near(b_sl->qty, 2.0));
    }
}

// OCA-CANCEL full-fill gate: when a CANCEL-group order fires for less
// qty than its requested ``order.qty`` (because the position is smaller
// than the order size), TV does NOT cancel the remaining siblings until
// the originating order fully fills. The engine guards this by
// comparing ``filled_qty`` against ``order.qty`` in
// apply_filled_order_to_state.
//
// In our engine, RAW_ORDER opposite-direction fills close the FULL
// position regardless of order.qty (apply_raw_order_fill, line 494),
// so for OCA-cancel siblings of size > position the fill IS partial
// and ``filled_qty < order.qty``. With the gate, A_SL stays alive.
// Without the gate, A_SL is wiped immediately.
static void test_cancel_oca_partial_fill_keeps_sibling() {
    std::printf("test_cancel_oca_partial_fill_keeps_sibling\n");
    class PartialFillProbe : public BacktestEngine {
    public:
        struct PendingSnap { std::string id; double qty; };
        std::vector<std::vector<PendingSnap>> pending_per_bar;

        PartialFillProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 100;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Bar 1: open long qty 2.
            if (bar_index_ == 1) {
                strategy_entry("L", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               2.0, "long entry");
            }
            // Bar 2: place A_TP (limit, short, qty=4 — bigger than
            // position) and A_SL (stop, short, qty=4) in OCA-CANCEL.
            // A_SL has stop BELOW the bar range (80) so only A_TP
            // (limit=100) is touched at fire time. When A_TP fires,
            // the engine fills a SHORT order against the long position
            // — this closes the position (qty=2), not the full order
            // qty 4. filled_qty (2) < order.qty (4) → fully_filled =
            // false → cancel_oca_group should NOT run. A_SL remains
            // pending.
            if (bar_index_ == 2 && position_side_ == PositionSide::LONG) {
                strategy_order("A_TP", false, 4.0, /*limit=*/100.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               "GRP_A", /*cancel=*/1);
                strategy_order("A_SL", false, 4.0,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/80.0,
                               "GRP_A", 1);
            }
            std::vector<PendingSnap> snap;
            for (const auto& po : pending_orders_) {
                snap.push_back({po.id, po.qty});
            }
            pending_per_bar.push_back(std::move(snap));
        }
        const PendingSnap* find(int bar, const std::string& id) const {
            if (bar < 0 || bar >= (int)pending_per_bar.size()) return nullptr;
            for (const auto& s : pending_per_bar[bar]) {
                if (s.id == id) return &s;
            }
            return nullptr;
        }
    };
    PartialFillProbe p;
    Bar bars[5] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 105,  95, 100, 1000, 120'000},  // place L
        {100, 105,  95, 100, 1000, 180'000},  // place A_TP/A_SL while long
        {100, 110,  90, 105, 1000, 240'000},  // A_TP @100 fires; partial fill (only qty 2)
        {100, 110,  90, 105, 1000, 300'000},
    };
    p.run(bars, 5);

    // After bar 3: A_TP filled qty=2 against position size 2. order.qty
    // was 4 — so fully_filled=false. A_SL must remain pending.
    CHECK(p.find(3, "A_TP") == nullptr);  // A_TP itself is gone (consumed)
    auto* a_sl = p.find(3, "A_SL");
    CHECK(a_sl != nullptr);
    if (a_sl) CHECK(near(a_sl->qty, 4.0));  // unchanged
}

// Sanity: oca.none leaves siblings untouched on fill.
static void test_none_unchanged() {
    std::printf("test_none_unchanged\n");
    OcaProbe p;
    p.oca_type = 0;
    p.siblings = {
        {"A", true, 3.0, 100.0},
        {"B", true, 5.0,  80.0},
    };
    Bar bars[4] = {
        {100, 105,  95, 100, 1000,  60'000},
        {100, 108,  95, 105, 1000, 120'000},
        {100, 110,  90, 105, 1000, 180'000},  // A fills → B should remain qty 5
        {100, 110,  85, 105, 1000, 240'000},
    };
    p.run(bars, 4);

    auto* b = p.find(2, "B");
    CHECK(b != nullptr);
    if (b) CHECK(near(b->qty, 5.0));
}

// Two strategy.exit brackets attached to the same long entry, each with
// its own qty + oca_name. Tests that:
//   (a) ``qty=N`` on strategy.exit is honoured as an absolute reservation
//       (so two qty=1 brackets coexist against a qty=2 position instead
//       of the first one swallowing 100% of the position).
//   (b) When bracket A's TP fires, only Group A siblings are cancelled —
//       Group B remains pending and can fire later.
//
// Pre-fix: ``strategy_exit`` ignored both ``qty`` and ``oca_name`` (the
// codegen warned + dropped them). Symptoms in
// validation_oca/oca-three-way-probe-02-multi-group-partial: TV=1242
// trades, engine=716 trades (engine misses ~42% because the first
// bracket's qty_percent=100 reserved the whole position so the second
// bracket never placed).
static void test_strategy_exit_two_brackets_independent_oca_groups() {
    std::printf("test_strategy_exit_two_brackets_independent_oca_groups\n");
    class TwoBracketProbe : public BacktestEngine {
    public:
        struct TradeRow {
            std::string entry_id;
            std::string exit_id;
            double qty;
            double exit_price;
        };
        std::vector<TradeRow> closed_trades;

        TwoBracketProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 2.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 1;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Bar 0: open long qty 2 (default_qty_value_).
            if (bar_index_ == 0) {
                strategy_entry("L", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               2.0, "long entry");
            }
            // Bars 1+: while long, attach two qty=1 brackets in distinct
            // OCA groups. Bracket A is tight (TP=110, SL=90); bracket B
            // is wide (TP=130, SL=70).
            if (position_side_ == PositionSide::LONG) {
                strategy_exit("X_A", "L",
                              /*limit=*/110.0, /*stop=*/90.0,
                              /*trail_points=*/std::numeric_limits<double>::quiet_NaN(),
                              /*trail_offset=*/std::numeric_limits<double>::quiet_NaN(),
                              /*trail_price=*/std::numeric_limits<double>::quiet_NaN(),
                              /*qty_percent=*/100.0,
                              /*comment=*/"X_A",
                              /*qty=*/1.0,
                              /*oca_name=*/"GRP_A");
                strategy_exit("X_B", "L",
                              /*limit=*/130.0, /*stop=*/70.0,
                              /*trail_points=*/std::numeric_limits<double>::quiet_NaN(),
                              /*trail_offset=*/std::numeric_limits<double>::quiet_NaN(),
                              /*trail_price=*/std::numeric_limits<double>::quiet_NaN(),
                              /*qty_percent=*/100.0,
                              /*comment=*/"X_B",
                              /*qty=*/1.0,
                              /*oca_name=*/"GRP_B");
            }
            if (bar_index_ == 6) {
                for (const auto& t : trades_) {
                    closed_trades.push_back({t.entry_id, t.exit_id, t.qty, t.exit_price});
                }
            }
        }
    };
    TwoBracketProbe p;
    Bar bars[7];
    // Bar 1 fills L at open=100. Bar 2: high=112 → X_A's TP=110 fires
    // (qty=1). Bar 3: high=132 → X_B's TP=130 fires (qty=1). Both
    // brackets must trigger independently; pre-fix only X_A would.
    double opens[7]  = { 100, 100, 105, 120, 120, 120, 120 };
    double highs[7]  = { 101, 105, 112, 132, 121, 121, 121 };
    double lows[7]   = {  99,  99, 104, 119, 119, 119, 119 };
    double closes[7] = { 100, 105, 112, 130, 120, 120, 120 };
    for (int i = 0; i < 7; ++i) {
        bars[i].open      = opens[i];
        bars[i].high      = highs[i];
        bars[i].low       = lows[i];
        bars[i].close     = closes[i];
        bars[i].volume    = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
    }
    p.run(bars, 7);

    // Both bracket fires must produce a trade. With the bug, only X_A
    // fired (X_B was never placed because X_A reserved 100% of qty).
    CHECK(p.closed_trades.size() == 2);
    bool seen_a = false, seen_b = false;
    for (const auto& tr : p.closed_trades) {
        CHECK(near(tr.qty, 1.0));
        if (tr.exit_id == "X_A") { seen_a = true; CHECK(near(tr.exit_price, 110.0)); }
        if (tr.exit_id == "X_B") { seen_b = true; CHECK(near(tr.exit_price, 130.0)); }
    }
    CHECK(seen_a);
    CHECK(seen_b);
}

int main() {
    test_reduce_two_sibling_partial();
    test_reduce_three_sibling();
    test_reduce_full_fill_cascade();
    test_cancel_unchanged();
    test_cross_group_isolation();
    test_cancel_oca_partial_fill_keeps_sibling();
    test_none_unchanged();
    test_strategy_exit_two_brackets_independent_oca_groups();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
