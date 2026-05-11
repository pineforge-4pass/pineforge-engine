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

int main() {
    test_reduce_two_sibling_partial();
    test_reduce_three_sibling();
    test_reduce_full_fill_cascade();
    test_cancel_unchanged();
    test_none_unchanged();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
