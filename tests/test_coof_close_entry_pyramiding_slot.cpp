// A settled entry occupies its pyramiding slot before its fill recalculation.
//
// Derivation from the constructed feed and next-open execution:
// The long signalled at 10 opens at 10 + 1 = 11 with quantity 1.
// The close and short signalled at 20 execute at 20 + 1 = 21.
// One occupied short slot exhausts pyramiding = 1, so the repeated entry
// requesting 3 cannot add a lot. The close-all at 30 exits at 30 + 1 = 31.
// Thus there are 1 long + 1 short = 2 closed trades, short quantity 2,
// maximum simultaneous short lots 1, and final position 0.
// All opens are 100; entry and exit prices are 100, so gross PnL is
// (100 - 100) * quantity * pointvalue(1) * FX(1) = 0, with zero costs.
// Turning fill recalculation off or omitting the explicit close preserves
// those trades (the latter reverses the long with the short entry).
// Input mutant: pyramiding = 2 admits the second short of 3 at 21,
// giving 1 + 2 = 3 trades, maximum short lots 2, and position -(2 + 3)
// before the close-all. This must differ from the one-slot result.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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

std::vector<Bar> constructed_feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 40; ++i)
        bars.push_back({100.0, 102.0, 99.0, 101.0, 1000.0,
                        kStart + i * kHour});
    return bars;
}

class CloseEntryProbe final : public source::PineStrategyHost {
public:
    CloseEntryProbe(bool coof, bool explicit_close, int pyramiding)
        : explicit_close_(explicit_close) {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = pyramiding;
        cfg.calc_on_order_fills = coof;
        cfg.process_orders_on_close = false;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 0.001);
    }

    void observe() {
        if (position_side_ != PositionSide::SHORT) return;
        max_short_lots = std::max(max_short_lots, pyramid_entries_.size());
        for (const auto& lot : pyramid_entries_)
            if (lot.qty == 3.0) saw_three_unit_short = true;
    }

    void on_source_bar(const Bar&) override {
        observe();
        const int i = pine_bar_index();
        const auto point = current_execution_point();
        if (i == 21 && point
            && point->decision.coordinate.provenance == NativePriceProvenance::Calculation) {
            saw_bar21_close = true;
            bar21_size = signed_position_size();
            bar21_lots = pyramid_entries_.size();
        }
        if (i == 10)
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        if ((i == 20 || i == 21) && is_last_tick_) {
            if (explicit_close_)
                strategy_close("L", "", kNaN, kNaN, false, 1);
            strategy_entry("S", false, kNaN, kNaN, i == 20 ? 2.0 : 3.0);
        }
        if (i == 30)
            strategy_close("", "", kNaN, kNaN, false);
        observe();
    }

    double final_size() const { return signed_position_size(); }
    std::size_t max_short_lots = 0;
    bool saw_three_unit_short = false;
    bool saw_bar21_close = false;
    double bar21_size = kNaN;
    std::size_t bar21_lots = 0;

private:
    bool explicit_close_;
};

void check_trade(const Trade& trade, const char* id, bool is_long, double qty,
                 int entry_bar) {
    CHECK(trade.entry_id == id);
    CHECK(trade.is_long == is_long);
    CHECK(trade.qty == qty);
    CHECK(trade.entry_bar_index == entry_bar);
    CHECK(trade.exit_bar_index == (is_long ? 21 : 31));
    CHECK(trade.entry_time == kStart + entry_bar * kHour);
    CHECK(trade.exit_time == kStart + (is_long ? 21 : 31) * kHour);
    CHECK(trade.entry_price == 100.0);
    CHECK(trade.exit_price == 100.0);
    CHECK(trade.pnl == 0.0);
}

void check_case(const char* name, bool coof, bool explicit_close, int cap) {
    std::printf("%s\n", name);
    CloseEntryProbe p(coof, explicit_close, cap);
    const auto bars = constructed_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");

    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) {
        std::printf("  run error: %s\n", p.last_error().c_str());
        return;
    }
    // One long plus one short per available slot; cap is the mutated input.
    const int expected_trades = 1 + cap;
    CHECK(p.trade_count() == expected_trades);
    if (p.trade_count() != expected_trades) {
        std::printf("  actual trades %d, expected %d; bar 21 size %.17g, lots %zu\n",
                    p.trade_count(), expected_trades, p.bar21_size, p.bar21_lots);
        for (int i = 0; i < p.trade_count(); ++i) {
            const Trade& t = p.get_trade(i);
            std::printf("  %s %s qty %.17g entry %d @%.17g exit %d @%.17g\n",
                        t.entry_id.c_str(), t.is_long ? "long" : "short", t.qty,
                        t.entry_bar_index, t.entry_price, t.exit_bar_index, t.exit_price);
        }
    }
    if (p.trade_count() == expected_trades) {
        check_trade(p.get_trade(0), "L", true, 1.0, 11);
        check_trade(p.get_trade(1), "S", false, 2.0, 21);
        if (cap == 2) check_trade(p.get_trade(2), "S", false, 3.0, 21);
    }
    CHECK(p.saw_bar21_close);
    CHECK(p.bar21_size == (cap == 1 ? -2.0 : -(2.0 + 3.0)));
    CHECK(p.bar21_lots == static_cast<std::size_t>(cap));
    CHECK(p.max_short_lots == static_cast<std::size_t>(cap));
    CHECK(p.saw_three_unit_short == (cap == 2));
    for (int i = 0; i < p.trade_count(); ++i)
        if (!p.get_trade(i).is_long && cap == 1) CHECK(p.get_trade(i).qty != 3.0);
    CHECK(p.final_size() == 0.0);
}

// Unlike the original feed, every bar here has O=100+2*i, H=O+3,
// L=O-2 and C=O+1. Entry and exit prices therefore differ.
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
    SlotProbe(bool on_close, int cap) {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = cap;
        cfg.calc_on_order_fills = true;
        cfg.process_orders_on_close = on_close;
        // Keep admission on cohort/origin counting: the FIFO ledger's
        // separate record count would mask a wrongly counted pending entry.
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
        }
    }

    double final_size() const { return signed_position_size(); }
    std::size_t max_short_lots = 0;
    bool saw_bar25 = false;
    double bar25_size = kNaN;
    std::size_t bar25_lots = 0;
};

void check_priced_trade(const Trade& trade, const char* id, bool is_long,
                        double qty, int entry_bar, double entry_price,
                        int exit_bar, double exit_price) {
    CHECK(trade.entry_id == id);
    CHECK(trade.is_long == is_long);
    CHECK(trade.qty == qty);
    CHECK(trade.entry_bar_index == entry_bar);
    CHECK(trade.exit_bar_index == exit_bar);
    CHECK(trade.entry_time == kStart + entry_bar * kHour);
    CHECK(trade.exit_time == kStart + exit_bar * kHour);
    CHECK(trade.entry_price == entry_price);
    CHECK(trade.exit_price == exit_price);
    // Point value and FX are one, with zero commission and slippage.
    CHECK(trade.pnl == (is_long ? exit_price - entry_price
                               : entry_price - exit_price) * qty);
}

class SettledSlotProbe final : public SlotProbe {
public:
    SettledSlotProbe(bool on_close, bool limit, bool same_id, int cap,
                     double second_limit, bool first_limit)
        : SlotProbe(on_close, cap), on_close_(on_close), limit_(limit),
          same_id_(same_id), second_limit_(second_limit), first_limit_(first_limit) {}

    void on_source_bar(const Bar& bar) override {
        observe_slots();
        const int i = pine_bar_index();
        if (i == (on_close_ ? 20 : 21))
            ++reversal_source_callbacks;
        if (ordinary_calculation()) {
            if (i == 10)
                strategy_entry("initial", true, kNaN, kNaN, 1.0);
            if (i == 20) {
                strategy_close(first_limit_ ? "" : "initial", "", kNaN, kNaN, false, 1);
                // C20-1=140: a sell limit already executable at either
                // C20=141 (close processing) or O21=142 (next-open).
                // The first-limit companion uses a full close with
                // next-open processing: that close releases its paired
                // limit synchronously, just as it releases a market entry.
                strategy_entry("short", false, first_limit_ ? bar.close - 1.0 : kNaN,
                               kNaN, 2.0);
            }
            if (i == 30)
                strategy_close("", "", kNaN, kNaN, false);
        } else if (!submitted_second && i == (on_close_ ? 20 : 21)
                   && signed_position_size() == -2.0) {
            // The close's fill callback sees the short physically settled,
            // before the short's own queued applied notification is drained.
            // Terminal close processing schedules no such source callback.
            submitted_second = true;
            submission_bar = i;
            submission_size = signed_position_size();
            submission_lots = pyramid_entries_.size();
            strategy_entry(same_id_ ? "short" : "additional", false,
                           limit_ ? second_limit_ : kNaN, kNaN, 3.0);
        }
        observe_slots();
    }

    bool submitted_second = false;
    int reversal_source_callbacks = 0;
    int submission_bar = -1;
    double submission_size = kNaN;
    std::size_t submission_lots = 0;

private:
    bool on_close_;
    bool limit_;
    bool same_id_;
    double second_limit_;
    bool first_limit_;
};

void check_settled_slot(const char* name, bool on_close, bool limit,
                        bool same_id, int cap, int limit_bar = 24,
                        bool first_limit = false) {
    std::printf("%s (close processing %d, same id %d, cap %d, limit bar %d, first limit %d)\n",
                name, on_close, same_id, cap, limit_bar, first_limit);
    const auto bars = rising_feed();
    // A sell limit at O[k]+2 lies strictly between H[k-1]=O[k]+1
    // and H[k]=O[k]+3. If submitted, it first fills at its level on bar k.
    // Mutating k from 24 to 26 must move the extra trade by two bars
    // and raise its entry price by 2*(26-24)=4.
    const double second_limit = bars[limit_bar].open + 2.0;
    SettledSlotProbe p(on_close, limit, same_id, cap, second_limit, first_limit);
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) {
        std::printf("  run error: %s\n", p.last_error().c_str());
        return;
    }
    // A terminal process-on-close fill schedules no source recalculation:
    // C10 opens the long, C20 closes it and opens the short, and C30 closes
    // that short. There is no callback to submit quantity 3, even at cap 2.
    // Next-open processing instead reaches that callback at O21, so its
    // cap-2 input must admit quantity 3. Keep the POOC close-price checks.
    const int expected_short_lots = on_close ? 1 : cap;
    const int expected_trades = 1 + expected_short_lots;
    CHECK(p.trade_count() == expected_trades);
    std::printf("  actual trades %d, expected %d; bar 25 size %.17g, lots %zu\n",
                p.trade_count(), expected_trades, p.bar25_size, p.bar25_lots);
    CHECK(p.submitted_second == !on_close);
    CHECK(p.submission_bar == (on_close ? -1 : 21));
    if (on_close) {
        // Exactly one ordinary evaluation; the terminal fills add none.
        CHECK(p.reversal_source_callbacks == 1);
        CHECK(std::isnan(p.submission_size));
        CHECK(p.submission_lots == 0U);
    } else {
        CHECK(p.reversal_source_callbacks > 1);
        CHECK(p.submission_size == -2.0);
        CHECK(p.submission_lots == 1U);
    }
    const int initial_bar = on_close ? 10 : 11;
    const int short_bar = on_close ? 20 : 21;
    const int exit_bar = on_close ? 30 : 31;
    const double initial_price = on_close ? bars[initial_bar].close : bars[initial_bar].open;
    const double short_price = on_close ? bars[short_bar].close : bars[short_bar].open;
    const double exit_price = on_close ? bars[exit_bar].close : bars[exit_bar].open;
    if (p.trade_count() == expected_trades) {
        check_priced_trade(p.get_trade(0), "initial", true, 1.0,
                           initial_bar, initial_price, short_bar, short_price);
        check_priced_trade(p.get_trade(1), "short", false, 2.0,
                           short_bar, short_price, exit_bar, exit_price);
        if (!on_close && cap == 2) {
            // The next-open callback's market add fills at O21.
            // A resting limit instead waits for k.
            const int added_bar = limit ? limit_bar : 21;
            const double added_price = limit ? second_limit : bars[added_bar].open;
            check_priced_trade(p.get_trade(2), same_id ? "short" : "additional",
                               false, 3.0, added_bar, added_price, exit_bar, exit_price);
        }
    }
    // With next-open processing, at bar 25 the shifted limit is waiting,
    // whereas the ordinary limit and the market add have already filled.
    const bool added_by_25 = !on_close && cap == 2 && (!limit || limit_bar <= 25);
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == -(2.0 + (added_by_25 ? 3.0 : 0.0)));
    CHECK(p.bar25_lots == (added_by_25 ? 2U : 1U));
    CHECK(p.max_short_lots == static_cast<std::size_t>(expected_short_lots));
    CHECK(p.final_size() == 0.0);
}

class ReplacementSlotProbe final : public SlotProbe {
public:
    ReplacementSlotProbe(bool pending_limit, int cap, double admitted_qty,
                         bool closed_predecessor)
        : SlotProbe(false, cap), pending_limit_(pending_limit),
          admitted_qty_(admitted_qty), closed_predecessor_(closed_predecessor) {}

    void on_source_bar(const Bar&) override {
        observe_slots();
        if (!ordinary_calculation()) return;
        const int i = pine_bar_index();
        if (i == 5 && closed_predecessor_)
            strategy_entry("replacement", false, kNaN, kNaN, 1.0);
        if (i == 10)
            strategy_entry("seed", false, kNaN, kNaN, 2.0);
        if (i == 14 && closed_predecessor_)
            strategy_close("replacement", "", kNaN, kNaN, false, 1);
        if (i == 20) {
            before_replacement_size = signed_position_size();
            before_replacement_lots = pyramid_entries_.size();
            // max(H)=H39=181. Sell limits 181+10 and 181+20 cannot
            // fill. Market orders also cannot fill inside this calculation.
            // All three requests reuse one id before any can settle:
            // neither replaced quantity (7 or 5) occupies a slot.
            strategy_entry("replacement", false, pending_limit_ ? 191.0 : kNaN,
                           kNaN, 7.0);
            strategy_entry("replacement", false, pending_limit_ ? 201.0 : kNaN,
                           kNaN, 5.0);
            strategy_entry("replacement", false, kNaN, kNaN, admitted_qty_);
        }
        if (i == 30)
            strategy_close("", "", kNaN, kNaN, false);
        observe_slots();
    }

    double before_replacement_size = kNaN;
    std::size_t before_replacement_lots = 0;

private:
    bool pending_limit_;
    double admitted_qty_;
    bool closed_predecessor_;
};

void check_unfilled_replacement(bool pending_limit, int cap, double admitted_qty,
                                bool closed_predecessor = false) {
    std::printf("unfilled %s replacement, cap %d, admitted quantity %.17g, closed predecessor %d\n",
                pending_limit ? "limit" : "market", cap, admitted_qty, closed_predecessor);
    ReplacementSlotProbe p(pending_limit, cap, admitted_qty, closed_predecessor);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    // The seed fills at O11; with cap 2 exactly one slot is free at 20,
    // so only the last replacement fills at O21. Cap 1 has no free slot.
    // The companion starts an older replacement-id lot at O6=112 and
    // explicitly closes it at O15=130. ANY closes that id's one unit,
    // preserving the seed's two units. At 20 the origin check is reached
    // with a nonflat book, unlike the one-slot flat reentry case below.
    const int expected_trades = cap + (closed_predecessor ? 1 : 0);
    CHECK(p.trade_count() == expected_trades);
    CHECK(p.before_replacement_size == -2.0);
    CHECK(p.before_replacement_lots == 1U);
    if (p.trade_count() == expected_trades) {
        const int seed_index = closed_predecessor ? 1 : 0;
        if (closed_predecessor)
            check_priced_trade(p.get_trade(0), "replacement", false, 1.0,
                               6, bars[6].open, 15, bars[15].open);
        check_priced_trade(p.get_trade(seed_index), "seed", false, 2.0,
                           11, bars[11].open, 31, bars[31].open);
        if (cap == 2)
            check_priced_trade(p.get_trade(seed_index + 1), "replacement", false, admitted_qty,
                               21, bars[21].open, 31, bars[31].open);
    }
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == -(2.0 + (cap == 2 ? admitted_qty : 0.0)));
    CHECK(p.bar25_lots == static_cast<std::size_t>(cap));
    CHECK(p.max_short_lots == static_cast<std::size_t>(cap));
    CHECK(p.final_size() == 0.0);
}

class ReusedIdSlotProbe final : public SlotProbe {
public:
    ReusedIdSlotProbe(bool on_close, bool close_old)
        : SlotProbe(on_close, 1), close_old_(close_old) {}

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
            // Reuse the closed id first for a never-reached sell limit,
            // then for the market successor. No old physical origin
            // survives the explicit full close to occupy this new slot.
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

void check_closed_incarnation(bool on_close, bool close_old) {
    std::printf("reused id, close processing %d, explicit old close %d\n",
                on_close, close_old);
    ReusedIdSlotProbe p(on_close, close_old);
    const auto bars = rising_feed();
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    // With the close: one old short plus one new short = two trades,
    // never simultaneous. Without it the old lot still fills cap 1,
    // and the quantity-2 successor must be refused.
    const int expected_trades = close_old ? 2 : 1;
    CHECK(p.trade_count() == expected_trades);
    if (p.trade_count() != expected_trades) {
        std::printf("  actual trades %d, expected %d; before reentry %.17g / %zu lots; "
                    "bar 25 size %.17g, lots %zu\n",
                    p.trade_count(), expected_trades,
                    p.before_reentry_size, p.before_reentry_lots,
                    p.bar25_size, p.bar25_lots);
        for (int i = 0; i < p.trade_count(); ++i) {
            const Trade& t = p.get_trade(i);
            std::printf("  %s %s qty %.17g entry %d @%.17g exit %d @%.17g\n",
                        t.entry_id.c_str(), t.is_long ? "long" : "short", t.qty,
                        t.entry_bar_index, t.entry_price, t.exit_bar_index, t.exit_price);
        }
    }
    CHECK(p.before_reentry_size == (close_old ? 0.0 : -1.0));
    CHECK(p.before_reentry_lots == (close_old ? 0U : 1U));
    const int initial_bar = on_close ? 10 : 11;
    const int old_exit_bar = close_old ? (on_close ? 14 : 15) : (on_close ? 30 : 31);
    const int new_bar = on_close ? 20 : 21;
    const int exit_bar = on_close ? 30 : 31;
    if (p.trade_count() == expected_trades) {
        check_priced_trade(p.get_trade(0), "reused", false, 1.0, initial_bar,
                           on_close ? bars[initial_bar].close : bars[initial_bar].open,
                           old_exit_bar,
                           on_close ? bars[old_exit_bar].close : bars[old_exit_bar].open);
        if (close_old)
            check_priced_trade(p.get_trade(1), "reused", false, 2.0, new_bar,
                               on_close ? bars[new_bar].close : bars[new_bar].open,
                               exit_bar, on_close ? bars[exit_bar].close : bars[exit_bar].open);
    }
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == (close_old ? -2.0 : -1.0));
    CHECK(p.bar25_lots == 1U);
    CHECK(p.max_short_lots == 1U);
    CHECK(p.final_size() == 0.0);
}

// POOC only suppresses terminal-close fill callbacks. A resting buy stop
// at O11=122 waits through C10=121, then fills at the next open.
// Its first-open callback submits a close and its paired short of 2.
// First-open requests retain native market triggers, so the close's applied
// handler can settle its child before recalculating the close. A mid-leg
// initial fill instead converts the child to a waypoint stop; synchronous
// execution accepts only market triggers, so that child's own notification
// would arrive before a callback sees the short. It cannot test this interval.
// That callback submits the second sell limit, so this is an admission test.
// At cap 1 only the short of 2 may survive; cap 2 must also admit short 3.
// Sell limit O[k]+2 first touches at k: H[k-1]=O[k]+1 < O[k]+2 < H[k].
// The final close is C30=161. Long PnL=(122-122)*1=0;
// first short PnL=(122-161)*2=-78. For k=24/26, added short PnL
// is (150-161)*3=-33 / (154-161)*3=-21, respectively.
class PoocIntrabarSlotProbe final : public SlotProbe {
public:
    PoocIntrabarSlotProbe(bool same_id, int cap, double first_price, double second_limit,
                          bool first_is_stop = true)
        : SlotProbe(true, cap), same_id_(same_id),
          first_price_(first_price), second_limit_(second_limit),
          first_is_stop_(first_is_stop) {}

    void on_source_bar(const Bar&) override {
        observe_slots();
        const int i = pine_bar_index();
        const bool ordinary = ordinary_calculation();
        if (i == 11) {
            if (ordinary) {
                ++ordinary_callbacks;
                short_notifications_at_ordinary = short_open_notifications;
            } else {
                ++fill_callbacks;
            }
        }
        if (ordinary) {
            if (i == 10)
                strategy_entry("initial", true,
                               first_is_stop_ ? kNaN : first_price_,
                               first_is_stop_ ? first_price_ : kNaN, 1.0);
            if (i == 30)
                strategy_close("", "", kNaN, kNaN, false);
        } else if (i == 11) {
            if (!requested_pair && signed_position_size() == 1.0) {
                requested_pair = true;
                pair_request_callback = fill_callbacks;
                pair_request_size = signed_position_size();
                pair_request_lots = pyramid_entries_.size();
                const auto point = current_execution_point();
                if (point) pair_request_price = point->price;
                strategy_close("initial", "", kNaN, kNaN, false, 1);
                strategy_entry("short", false, kNaN, kNaN, 2.0);
            } else if (requested_pair && !submitted_second
                       && signed_position_size() == -2.0) {
                submitted_second = true;
                submission_bar = i;
                submission_size = signed_position_size();
                submission_lots = pyramid_entries_.size();
                short_notifications_at_submission = short_open_notifications;
                close_notifications_at_submission = close_notifications;
                submission_callback = fill_callbacks;
                const auto point = current_execution_point();
                if (point) submission_price = point->price;
                strategy_entry(same_id_ ? "short" : "additional", false,
                               second_limit_, kNaN, 3.0);
            }
        }
        observe_slots();
    }

    bool requested_pair = false;
    int pair_request_callback = -1;
    double pair_request_size = kNaN;
    double pair_request_price = kNaN;
    std::size_t pair_request_lots = 0;
    bool submitted_second = false;
    int submission_bar = -1;
    double submission_size = kNaN;
    double submission_price = kNaN;
    std::size_t submission_lots = 0;
    int ordinary_callbacks = 0;
    int fill_callbacks = 0;
    int submission_callback = -1;
    int short_open_notifications = 0;
    int close_notifications = 0;
    int short_notifications_at_submission = -1;
    int close_notifications_at_submission = -1;
    int short_notifications_at_ordinary = -1;

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        // This observer runs after adapter credit, before the source recalc.
        // The first-open rows submit before the short observer runs; the
        // mid-leg control and ordinary evaluation on 11 see it after credit.
        if (event.definition && event.opened_units != 0.0
            && event.definition->request.label == "short")
            ++short_open_notifications;
        if (event.closed_units > 0.0 && event.opened_units == 0.0)
            ++close_notifications;
    }

private:
    bool same_id_;
    double first_price_;
    double second_limit_;
    bool first_is_stop_;
};

void check_pooc_intrabar_slot(bool same_id, int cap, int limit_bar = 24) {
    std::printf("close processing with intrabar paired fill, same id %d, cap %d, limit bar %d\n",
                same_id, cap, limit_bar);
    const auto bars = rising_feed();
    const double first_stop = bars[11].open;
    const double second_limit = bars[limit_bar].open + 2.0;
    PoocIntrabarSlotProbe p(same_id, cap, first_stop, second_limit);
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) {
        std::printf("  run error: %s\n", p.last_error().c_str());
        return;
    }
    // Three nonterminal fills on 11: the initial stop, its close, and the
    // paired short. Plus exactly one ordinary evaluation. The second entry
    // is sent by callback 2, before short notification 1, never at the close.
    CHECK(p.requested_pair);
    CHECK(p.pair_request_callback == 1);
    CHECK(p.pair_request_size == 1.0);
    CHECK(p.pair_request_price == bars[11].open);
    CHECK(p.pair_request_lots == 1U);
    CHECK(p.fill_callbacks == 3);
    CHECK(p.ordinary_callbacks == 1);
    CHECK(p.submitted_second);
    CHECK(p.submission_callback == 2);
    CHECK(p.submission_bar == 11);
    CHECK(p.submission_price == bars[11].open);
    CHECK(p.submission_size == -2.0);
    CHECK(p.submission_lots == 1U);
    CHECK(p.short_notifications_at_submission == 0);
    CHECK(p.close_notifications_at_submission == 1);
    CHECK(p.short_notifications_at_ordinary == 1);

    // One long and cap short openings: changing only cap 1 -> 2 must add
    // one trade and three units. A wrongly admitted cap-1 limit must fill
    // on k, remain visible at 25 (k=24), and close as a third trade at 30.
    const int expected_trades = 1 + cap;
    CHECK(p.trade_count() == expected_trades);
    std::printf("  actual trades %d, expected %d; bar 25 size %.17g, lots %zu; "
                "fill callbacks %d, submission callback %d, prior short notifications %d\n",
                p.trade_count(), expected_trades, p.bar25_size, p.bar25_lots,
                p.fill_callbacks, p.submission_callback, p.short_notifications_at_submission);
    if (p.trade_count() != expected_trades) {
        for (int i = 0; i < p.trade_count(); ++i) {
            const Trade& t = p.get_trade(i);
            std::printf("  %s %s qty %.17g entry %d @%.17g exit %d @%.17g\n",
                        t.entry_id.c_str(), t.is_long ? "long" : "short", t.qty,
                        t.entry_bar_index, t.entry_price, t.exit_bar_index, t.exit_price);
        }
    }
    if (p.trade_count() == expected_trades) {
        check_priced_trade(p.get_trade(0), "initial", true, 1.0,
                           11, first_stop, 11, bars[11].open);
        check_priced_trade(p.get_trade(1), "short", false, 2.0,
                           11, bars[11].open, 30, bars[30].close);
        if (cap == 2)
            check_priced_trade(p.get_trade(2), same_id ? "short" : "additional",
                               false, 3.0, limit_bar, second_limit, 30, bars[30].close);
    }
    const bool added_by_25 = cap == 2 && limit_bar <= 25;
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == -(2.0 + (added_by_25 ? 3.0 : 0.0)));
    CHECK(p.bar25_lots == (added_by_25 ? 2U : 1U));
    CHECK(p.max_short_lots == static_cast<std::size_t>(cap));
    CHECK(p.final_size() == 0.0);
}

void check_pooc_midleg_control() {
    std::printf("close processing with a mid-leg initial fill timing control\n");
    const auto bars = rising_feed();
    const double first_limit = bars[10].close - 0.5;
    const double second_limit = bars[24].open + 2.0;
    PoocIntrabarSlotProbe p(true, 2, first_limit, second_limit, false);
    p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    CHECK(p.last_error().empty());
    if (!p.last_error().empty()) return;
    // Mutate only the initial trigger: buy limit 120.5 fills mid-leg.
    // The native waypoint stop cannot execute synchronously in the close
    // handler, so callback 2 sees flat and callback 3 sees the notified short.
    // This control must differ from the queued-child rows' 2 / 0 assertions.
    CHECK(p.requested_pair);
    CHECK(p.pair_request_callback == 1);
    CHECK(p.pair_request_price == first_limit);
    CHECK(p.fill_callbacks == 3);
    CHECK(p.ordinary_callbacks == 1);
    CHECK(p.submitted_second);
    CHECK(p.submission_callback == 3);
    CHECK(p.submission_price == bars[11].low);
    CHECK(p.submission_size == -2.0);
    CHECK(p.submission_lots == 1U);
    CHECK(p.short_notifications_at_submission == 1);
    CHECK(p.close_notifications_at_submission == 1);
    CHECK(p.short_notifications_at_ordinary == 1);
    // Cap 2 admits both short lots: 1 long + 2 shorts = 3 trades.
    CHECK(p.trade_count() == 3);
    if (p.trade_count() == 3) {
        check_priced_trade(p.get_trade(0), "initial", true, 1.0,
                           11, first_limit, 11, bars[11].low);
        check_priced_trade(p.get_trade(1), "short", false, 2.0,
                           11, bars[11].low, 30, bars[30].close);
        check_priced_trade(p.get_trade(2), "short", false, 3.0,
                           24, second_limit, 30, bars[30].close);
    }
    CHECK(p.saw_bar25);
    CHECK(p.bar25_size == -(2.0 + 3.0));
    CHECK(p.bar25_lots == 2U);
    CHECK(p.max_short_lots == 2U);
    CHECK(p.final_size() == 0.0);
}

}  // namespace

int main() {
    check_case("close and entry with fill recalculation", true, true, 1);
    check_case("control without fill recalculation", false, true, 1);
    check_case("control with entry reversal", true, false, 1);
    check_case("two-slot input mutant", true, true, 2);
    for (bool same_id : {true, false}) {
        for (bool on_close : {false, true}) {
            check_settled_slot(on_close ? "terminal close callback control"
                                       : "settled limit occupies the only slot",
                               on_close, true, same_id, 1);
            check_settled_slot(on_close ? "terminal close callback control with two slots"
                                       : "settled limit leaves a second slot",
                               on_close, true, same_id, 2);
            check_settled_slot(on_close ? "terminal close callback control with later limit"
                                       : "later-limit input mutant",
                               on_close, true, same_id, 2, 26);
        }
        check_settled_slot("terminal close callback control with market request",
                           true, false, same_id, 1);
        check_settled_slot("terminal close callback control with market request and two slots",
                           true, false, same_id, 2);
        check_settled_slot("settled pure-limit origin occupies the only slot",
                           false, true, same_id, 1, 24, true);
        check_settled_slot("settled pure-limit origin leaves a second slot",
                           false, true, same_id, 2, 24, true);
        check_settled_slot("settled pure-limit origin with a later limit",
                           false, true, same_id, 2, 26, true);
    }
    for (bool pending_limit : {true, false}) {
        check_unfilled_replacement(pending_limit, 2, 3.0);
        check_unfilled_replacement(pending_limit, 2, 4.0);
        check_unfilled_replacement(pending_limit, 1, 3.0);
        check_unfilled_replacement(pending_limit, 2, 3.0, true);
    }
    for (bool on_close : {false, true}) {
        check_closed_incarnation(on_close, true);
        check_closed_incarnation(on_close, false);
    }
    for (bool same_id : {true, false}) {
        check_pooc_intrabar_slot(same_id, 1);
        check_pooc_intrabar_slot(same_id, 2);
        check_pooc_intrabar_slot(same_id, 2, 26);
    }
    check_pooc_midleg_control();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
