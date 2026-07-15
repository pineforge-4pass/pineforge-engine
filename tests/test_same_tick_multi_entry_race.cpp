/*
 * test_same_tick_multi_entry_race.cpp — TradingView same-tick multi-entry
 * fill semantics (audit rule R*, jevondijefferson-big-breakout-strategy).
 *
 * Shape under test: TWO strategy.entry blocks with DISTINCT ids sharing one
 * gate (3 BOS blocks entering "Long"/"Short" + a Wyckoff block entering
 * "Wyckoff Swing Long"/"Wyckoff Swing Short"), pyramiding=0 (engine
 * pyramiding_=1), percent-of-equity sizing, each entry paired with a
 * strategy.exit(from_entry=<its id>) bracket. When both blocks fire on the
 * SAME bar, both market entries fill at the SAME tick (next bar's open).
 *
 * TV's behaviour — validated 26/26 against every in-window race in the
 * jevondijefferson tv_trades.csv export (qty arithmetic to 1e-4, operative
 * bracket prices to the cent; audit artifacts scorecard.py / qtytest.py /
 * reversals.py, 2026-07-02 tv-ceiling audit):
 *
 *   R*: same-tick entries fill SEQUENTIALLY in script-call order, each at
 *   plain percent-of-equity qty; reversal augmentation (close-opposite-
 *   then-enter extra qty) attaches ONLY to the LAST same-direction entry
 *   of the tick; pyramiding=0 rejects an entry executing while the
 *   position is already in that direction (evaluated at execution time,
 *   against the sequentially-updated position); the fill that crosses
 *   zero / opens from flat owns the entry ID; strategy.exit(from_entry=X)
 *   brackets bind to id X even when X's paired entry call was rejected.
 *
 * Observable TV trade-list rows per race class (this is what the engine
 * must reproduce — TV reports the old lot's close as ONE row at the shared
 * fill price, attributed to the FIRST closing order's signal):
 *
 *   A flat at fill:      first entry opens at q_plain and owns its id;
 *                        the later entry is pyramiding-rejected.
 *   B opposite |pos| > q: old lot exits in ONE row with exit signal =
 *                        FIRST entry id; the new lot opens at q_plain
 *                        under the LAST entry id (total traded |pos|+q).
 *   C opposite |pos| < q: the FIRST entry's single plain fill crosses
 *                        zero: old lot exits (signal = first id), the
 *                        REMAINDER (q - |pos|) opens under the FIRST id;
 *                        the later entry is pyramiding-rejected.
 *   D same direction:    no entry executes; the live lot's bracket is
 *                        refreshed via from_entry binding.
 *
 * Pre-fix engine behaviour (dual-lot desync seeds): the first entry fill
 * took flip_market_position_to — closing the whole opposite position and
 * opening a FULL q_plain lot under the FIRST id. Class B then bound the
 * WRONG bracket (first id instead of last id); class C opened q_plain
 * instead of the remainder (TV: 0.2262 / engine: 7.915 at the 2025-06-17
 * 15:15 race), seeding multi-day tiny-qty stale-remainder chains.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>

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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Mirrors the BOS+Wyckoff structure: on the race bar, TWO market entries
// with distinct ids fire back-to-back in script-call order, each paired
// with its own from_entry bracket. Percent-of-equity 2% default sizing,
// pyramiding=0 (engine pyramiding_=1).
//
// Feed convention: o=h=l=c=100 (±0.5 wick) so market fills land at 100
// and q_plain = 2% * 1,000,000 / 100 = 200 exactly (PnL-neutral closes
// keep equity at 1,000,000 through the race).
class RaceProbe : public BacktestEngine {
public:
    struct TradeRow {
        std::string entry_id, exit_id;
        double qty, entry_price, exit_price;
    };

    // Scenario knobs (set before run()).
    int seed_bar = -1;            // bar issuing the seed entry (-1 = none)
    bool seed_is_long = false;
    double seed_qty = 0.0;        // explicit FIXED qty for the seed
    int race_bar = 2;             // bar issuing both entry blocks
    bool race_is_long = true;
    int second_race_bar = -1;     // optional class-D repeat (-1 = none)
    bool first_has_bracket = true;
    bool last_has_bracket = true;
    double first_bracket_qty_percent = 100.0;
    double last_bracket_qty_percent = 100.0;

    // Brackets (long-side values; short-side mirrors around 100).
    // First entry's bracket fires strictly EARLIER on the path than the
    // last entry's bracket, so the operative bracket is observable.
    double first_limit_long = 105.0, first_stop_long = 90.0;
    double last_limit_long = 110.0, last_stop_long = 88.0;

    RaceProbe() {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 2.0;
        pyramiding_ = 1;   // TV pyramiding=0: one entry per direction
        slippage_ = 0;
        commission_value_ = 0.0;
    }

    void on_bar(const Bar& bar) override {
        if (bar_index_ == seed_bar && seed_qty > 0.0) {
            strategy_entry("Seed", seed_is_long, kNaN, kNaN, seed_qty, "");
        }
        if (bar_index_ == race_bar || bar_index_ == second_race_bar) {
            issue_race_blocks();
        }
        snapshot();
    }

    void issue_race_blocks() {
        if (race_is_long) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN, "");
            if (first_has_bracket) {
                strategy_exit("Long Exit", "Long", first_limit_long,
                              first_stop_long, kNaN, kNaN, kNaN,
                              first_bracket_qty_percent, "", kNaN, "");
            }
            strategy_entry("Wyckoff Long", true, kNaN, kNaN, kNaN, "");
            if (last_has_bracket) {
                strategy_exit("Wyckoff Long Exit", "Wyckoff Long", last_limit_long,
                              last_stop_long, kNaN, kNaN, kNaN,
                              last_bracket_qty_percent, "", kNaN, "");
            }
        } else {
            strategy_entry("Short", false, kNaN, kNaN, kNaN, "");
            if (first_has_bracket) {
                strategy_exit("Short Exit", "Short", 200.0 - first_limit_long,
                              200.0 - first_stop_long, kNaN, kNaN, kNaN,
                              first_bracket_qty_percent, "", kNaN, "");
            }
            strategy_entry("Wyckoff Short", false, kNaN, kNaN, kNaN, "");
            if (last_has_bracket) {
                strategy_exit("Wyckoff Short Exit", "Wyckoff Short", 200.0 - last_limit_long,
                              200.0 - last_stop_long, kNaN, kNaN, kNaN,
                              last_bracket_qty_percent, "", kNaN, "");
            }
        }
    }

    void snapshot() {
        final_side = position_side_;
        final_qty = position_qty_;
        open_lots.clear();
        for (const auto& pe : pyramid_entries_) {
            open_lots.push_back({pe.entry_id, pe.qty});
        }
        closed.clear();
        for (const auto& t : trades_) {
            closed.push_back({t.entry_id, t.exit_id, t.qty, t.entry_price, t.exit_price});
        }
    }

    PositionSide final_side = PositionSide::FLAT;
    double final_qty = 0.0;
    std::vector<std::pair<std::string, double>> open_lots;
    std::vector<TradeRow> closed;
};

static std::vector<Bar> flat_feed(int n, double extra_high_bar = -1,
                                  double extra_high = 0.0) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        bars[i].open = 100.0;
        bars[i].high = 100.5;
        bars[i].low = 99.5;
        bars[i].close = 100.0;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 900'000;
        if (i == (int)extra_high_bar) bars[i].high = extra_high;
    }
    return bars;
}

}  // namespace

// Class A — flat at fill: first entry opens q_plain and owns the id, the
// later same-direction entry is pyramiding-rejected at execution time.
static void test_flat_race_first_id_wins_plain_qty() {
    std::printf("test_flat_race_first_id_wins_plain_qty\n");
    RaceProbe p;
    p.race_bar = 2;
    p.race_is_long = true;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
    CHECK(p.closed.empty());
}

// Class B — opposite position larger than q_plain: the old lot exits in ONE
// row attributed to the FIRST entry id; the new lot opens at q_plain under
// the LAST entry id; total traded = |pos| + q_plain.
static void test_reversal_race_last_id_owns_entry() {
    std::printf("test_reversal_race_last_id_owns_entry\n");
    RaceProbe p;
    p.seed_bar = 0;
    p.seed_is_long = false;
    p.seed_qty = 300.0;      // seed short 300 > q_plain 200
    p.race_bar = 2;
    p.race_is_long = true;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    // Old short (300) closed in one row, exit signal = FIRST entry id.
    double closed_seed = 0.0;
    bool exit_sig_first = true;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Seed") {
            closed_seed += t.qty;
            if (t.exit_id != "Long") exit_sig_first = false;
        }
    }
    CHECK(near(closed_seed, 300.0, 1e-9));
    CHECK(exit_sig_first);

    // New long lot: q_plain under the LAST id (audit rule R*: the fill that
    // crosses zero owns the entry ID; augmentation attaches to the LAST
    // same-direction entry of the tick).
    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Wyckoff Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
}

// Class B bracket binding: the operative bracket must be the LAST entry's
// from_entry bracket (limit 110), not the first's (limit 105). A bar
// touching 106 must NOT exit; the 111 bar exits at 110 via the last id's
// bracket.
static void test_reversal_race_operative_bracket_is_last() {
    std::printf("test_reversal_race_operative_bracket_is_last\n");
    RaceProbe p;
    p.seed_bar = 0;
    p.seed_is_long = false;
    p.seed_qty = 300.0;
    p.race_bar = 2;
    p.race_is_long = true;
    auto bars = flat_feed(7);
    bars[4].high = 106.0;    // would fire the WRONG bracket (limit 105)
    bars[5].high = 111.0;    // fires the correct bracket (limit 110)
    p.run(bars.data(), (int)bars.size());

    // The long lot must survive bar 4 untouched and exit at 110 on bar 5.
    bool found_bracket_exit = false;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Wyckoff Long") {
            found_bracket_exit = true;
            CHECK(t.exit_id == "Wyckoff Long Exit");
            CHECK(near(t.exit_price, 110.0, 1e-9));
        }
        // No trade may exit through the first entry's bracket at 105.
        CHECK(!(t.exit_id == "Long Exit"));
    }
    CHECK(found_bracket_exit);
    CHECK(p.final_side == PositionSide::FLAT);
}

// Class C — opposite position smaller than q_plain: the FIRST entry's plain
// fill crosses zero; the old lot exits with the first id's signal and the
// REMAINDER (q_plain - |pos|) opens under the FIRST id; the later entry is
// pyramiding-rejected against the sequentially-updated position.
static void test_reversal_race_remainder_crosses_zero_first_id() {
    std::printf("test_reversal_race_remainder_crosses_zero_first_id\n");
    RaceProbe p;
    p.seed_bar = 0;
    p.seed_is_long = true;
    p.seed_qty = 50.0;       // seed long 50 < q_plain 200
    p.race_bar = 2;
    p.race_is_long = false;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    double closed_seed = 0.0;
    bool exit_sig_first = true;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Seed") {
            closed_seed += t.qty;
            if (t.exit_id != "Short") exit_sig_first = false;
        }
    }
    CHECK(near(closed_seed, 50.0, 1e-9));
    CHECK(exit_sig_first);

    // Remainder short: q_plain(200) - 50 = 150 under the FIRST id, and the
    // later entry must NOT have added a second lot (TV total traded on the
    // tick = q_plain, not q_plain + q_plain).
    CHECK(p.final_side == PositionSide::SHORT);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Short");
        CHECK(near(p.open_lots[0].second, 150.0, 1e-9));
    }
}

// Rsantana discriminator — a duplicate later MARKET entry does not activate
// Jevond's sequential plain-transaction rule unless both entry blocks own
// their own full from_entry brackets. The primary entry here is unbracketed;
// TV therefore performs the ordinary full reversal under the primary id, and
// the bracketed duplicate is rejected by the pyramiding gate.
static void test_unbracketed_primary_reversal_keeps_full_qty() {
    std::printf("test_unbracketed_primary_reversal_keeps_full_qty\n");
    RaceProbe p;
    p.seed_bar = 0;
    p.seed_is_long = false;
    p.seed_qty = 50.0;       // old short < q_plain, the old broad R* made 150
    p.race_bar = 2;
    p.race_is_long = true;
    p.first_has_bracket = false;
    p.last_has_bracket = true;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    double closed_seed = 0.0;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Seed") closed_seed += t.qty;
    }
    CHECK(near(closed_seed, 50.0, 1e-9));
    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
}

static void check_nonpaired_reversal_keeps_full_qty(
        const char* label, bool first_has_bracket, bool last_has_bracket,
        double first_qty_percent, double last_qty_percent) {
    std::printf("%s\n", label);
    RaceProbe p;
    p.seed_bar = 0;
    p.seed_is_long = false;
    p.seed_qty = 50.0;
    p.race_bar = 2;
    p.race_is_long = true;
    p.first_has_bracket = first_has_bracket;
    p.last_has_bracket = last_has_bracket;
    p.first_bracket_qty_percent = first_qty_percent;
    p.last_bracket_qty_percent = last_qty_percent;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
}

// Both sides of the paired-bracket predicate are load-bearing. Neither a
// missing later child nor a partial child may activate Jevond R*.
static void test_unbracketed_later_reversal_keeps_full_qty() {
    check_nonpaired_reversal_keeps_full_qty(
        "test_unbracketed_later_reversal_keeps_full_qty",
        true, false, 100.0, 100.0);
}

static void test_partial_primary_bracket_keeps_full_qty() {
    check_nonpaired_reversal_keeps_full_qty(
        "test_partial_primary_bracket_keeps_full_qty",
        true, true, 50.0, 100.0);
}

static void test_partial_later_bracket_keeps_full_qty() {
    check_nonpaired_reversal_keeps_full_qty(
        "test_partial_later_bracket_keeps_full_qty",
        true, true, 100.0, 50.0);
}

// Class D — same-direction position: both entries are rejected at execution
// time; the position is unchanged and the live lot's bracket is refreshed
// via from_entry binding.
static void test_same_direction_race_rejected() {
    std::printf("test_same_direction_race_rejected\n");
    RaceProbe p;
    p.race_bar = 2;           // opens "Long" 200 from flat (class A)
    p.second_race_bar = 4;    // fires again while long — class D
    p.race_is_long = true;
    auto bars = flat_feed(8);
    p.run(bars.data(), (int)bars.size());

    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
    CHECK(p.closed.empty());
}

// Composition guard — same-bar strategy.close batch (672c59b) + sequential
// same-tick entries under process_orders_on_close: the surviving batched
// close fills at the bar close BEFORE the entry orders fill (dispatch step
// 3b before step 4), so the entries execute from FLAT: first id opens
// q_plain, later id is pyramiding-rejected.
static void test_poc_close_batch_then_sequential_entries() {
    std::printf("test_poc_close_batch_then_sequential_entries\n");
    class PocProbe : public RaceProbe {
    public:
        PocProbe() { process_orders_on_close_ = true; }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Seed", true, kNaN, kNaN, 50.0, "");
            }
            if (bar_index_ == 2) {
                strategy_close("Seed", "flip", kNaN, kNaN, false);
                race_is_long = false;
                issue_race_blocks();
            }
            snapshot();
        }
    };
    PocProbe p;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    // Seed long closed by the batched close (not by the entry fills).
    double closed_seed = 0.0;
    bool closed_by_close = true;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Seed") {
            closed_seed += t.qty;
            if (t.exit_id.rfind("__close__", 0) != 0) closed_by_close = false;
        }
    }
    CHECK(near(closed_seed, 50.0, 1e-9));
    CHECK(closed_by_close);

    // Entries then fill from flat at the same bar's close: first id owns
    // the position at q_plain; the later entry is pyramiding-rejected.
    CHECK(p.final_side == PositionSide::SHORT);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Short");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
}

// Single-entry reversal (no same-tick sibling) keeps the classic augmented
// flip: whole opposite position closes and a FULL q_plain lot opens under
// the single entry's id — the fix must not disturb the everyday path.
static void test_single_entry_reversal_unchanged() {
    std::printf("test_single_entry_reversal_unchanged\n");
    class SingleProbe : public RaceProbe {
    public:
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("Seed", false, kNaN, kNaN, 300.0, "");
            }
            if (bar_index_ == 2) {
                strategy_entry("Long", true, kNaN, kNaN, kNaN, "");
            }
            snapshot();
        }
    };
    SingleProbe p;
    auto bars = flat_feed(6);
    p.run(bars.data(), (int)bars.size());

    double closed_seed = 0.0;
    for (const auto& t : p.closed) {
        if (t.entry_id == "Seed") closed_seed += t.qty;
    }
    CHECK(near(closed_seed, 300.0, 1e-9));
    CHECK(p.final_side == PositionSide::LONG);
    CHECK(p.open_lots.size() == 1);
    if (p.open_lots.size() == 1) {
        CHECK(p.open_lots[0].first == "Long");
        CHECK(near(p.open_lots[0].second, 200.0, 1e-9));
    }
}

int main() {
    test_flat_race_first_id_wins_plain_qty();
    test_reversal_race_last_id_owns_entry();
    test_reversal_race_operative_bracket_is_last();
    test_reversal_race_remainder_crosses_zero_first_id();
    test_unbracketed_primary_reversal_keeps_full_qty();
    test_unbracketed_later_reversal_keeps_full_qty();
    test_partial_primary_bracket_keeps_full_qty();
    test_partial_later_bracket_keeps_full_qty();
    test_same_direction_race_rejected();
    test_poc_close_batch_then_sequential_entries();
    test_single_entry_reversal_unchanged();

    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
