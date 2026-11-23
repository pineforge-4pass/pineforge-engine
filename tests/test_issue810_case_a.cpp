// Public synthetic native proof for issue #810 Case A.
//
// The exact row submits a market entry, a 50% tick-profit exit and a full
// price-stop exit in one process_orders_on_close calculation.  The controls
// retain the absolute-price A counterpart and the two Case B reach controls.
// The two held-entry rows place the same three kinds of order without
// process_orders_on_close, so the entry is held to the end of its calculation
// and both exits are placed while flat.  One row reaches the full exit's stop
// first, the other the partial exit's limit first.  They assert only what the
// documented order rules give; check_held_entry cites each rule.
// Observations come from native applied events and closed trades.  The B
// pending control and the held-entry rows also read the pending rows of their
// signal bar, and the final position.

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEntryPrice = 43915.71;
constexpr double kHalfPrice = 44415.71;
constexpr double kRestPrice = 42915.71;
// The probe's price tick, and the B pending control's profit distance in ticks.
constexpr double kMinTick = 0.01;
constexpr double kBProfitTicks = 100.0;
// The held-entry rows: the entry's quantity, the partial exit's share of it in
// percent, and that exit's profit distance in ticks.  The quantity is four
// steps, so the share and what it leaves are different amounts.
constexpr double kHeldEntryQty = 0.04;
constexpr double kHeldPartialPercent = 25.0;
constexpr double kHeldProfitTicks = 50000.0;
// How far past a level the bar that reaches it travels, so that a fill booked
// at the bar's extreme is told from a fill booked at the level.
constexpr double kHeldOvershoot = 10.0;

enum class Mode {
    Exact,
    AbsoluteControl,
    BPendingEntryControl,
    BMarketAddControl,
    HeldEntryStopFirst,
    HeldEntryLimitFirst,
};

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Exact: return "exact";
        case Mode::AbsoluteControl: return "a_absolute_control";
        case Mode::BPendingEntryControl: return "b_pending_entry_control";
        case Mode::BMarketAddControl: return "b_market_add_control";
        case Mode::HeldEntryStopFirst: return "held_entry_stop_first";
        case Mode::HeldEntryLimitFirst: return "held_entry_limit_first";
    }
    return "unknown";
}

struct Applied {
    std::string label;
    int bar = -1;
    double opened = 0.0;
    double closed = 0.0;
    double price = kNaN;
    double position_after = 0.0;
};

Bar bar(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1000.0,
            1'704'067'200'000LL + static_cast<std::int64_t>(index) * 3'600'000LL};
}

class Probe final : public source::PineNativeHost {
public:
    explicit Probe(Mode mode) : mode_(mode) {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 0.02;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = mode == Mode::BMarketAddControl ? 2 : 1;
        config.process_orders_on_close =
            mode == Mode::Exact || mode == Mode::AbsoluteControl;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(kMinTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 0.01);
    }

    void on_source_bar(const Bar&) override {
        switch (mode_) {
            case Mode::Exact:
            case Mode::AbsoluteControl:
                on_case_a_bar();
                return;
            case Mode::BPendingEntryControl:
                on_case_b_pending_bar();
                return;
            case Mode::BMarketAddControl:
                on_case_b_market_add_bar();
                return;
            case Mode::HeldEntryStopFirst:
            case Mode::HeldEntryLimitFirst:
                on_held_entry_bar();
                return;
        }
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        if (event.opened_units == 0.0 && event.closed_units == 0.0) return;
        applied_.push_back({event.request().label,
                            static_cast<int>(context.coordinate.interval_index),
                            event.opened_units, event.closed_units,
                            event.resolved_price,
                            physical_position().signed_units});
    }

    double trade_size(int index) const { return closed_trade_size(index); }
    std::string trade_entry_id(int index) const { return closed_trade_entry_id(index); }
    std::string trade_exit_id(int index) const { return closed_trade_exit_id(index); }
    double trade_entry_price(int index) const {
        return closed_trade_entry_price(index);
    }
    double trade_exit_price(int index) const { return closed_trade_exit_price(index); }

    const std::vector<Applied>& applied() const { return applied_; }
    bool b_relative_shadow_seen() const { return b_relative_shadow_seen_; }
    std::uint64_t b_relative_shadow_rank() const { return b_relative_shadow_rank_; }
    bool held_flat_at_signal() const { return held_flat_at_signal_; }
    bool held_entry_staged() const { return held_entry_staged_; }
    bool held_partial_staged() const { return held_partial_staged_; }
    bool held_full_exit_placed() const { return held_full_exit_placed_; }

private:
    void exit(const char* id, const char* from, double limit, double stop,
              double qty_percent, double qty, double profit_ticks,
              const char* comment) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, qty_percent,
                      comment, qty, "", profit_ticks, kNaN);
    }

    void on_case_a_bar() {
        if (pine_bar_index() != 0) return;
        strategy_entry("Long", true, kNaN, kNaN, 0.02, "case-a-entry");
        if (mode_ == Mode::Exact) {
            exit("Half", "Long", kNaN, kNaN, 50.0, kNaN, 50000.0,
                 "case-a-tick-half");
        } else {
            exit("Half", "Long", kHalfPrice, kNaN, 50.0, kNaN, kNaN,
                 "case-a-price-half");
        }
        exit("Rest", "Long", kNaN, kRestPrice, 100.0, kNaN, kNaN,
             "case-a-price-rest");
    }

    void on_case_b_pending_bar() {
        if (pine_bar_index() == 0) {
            strategy_entry("B", true, kNaN, kNaN, 0.02, "case-b-entry");
            exit("BHalf", "B", kNaN, kNaN, 50.0, kNaN, kBProfitTicks,
                 "case-b-relative-pending-half");
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                if (row.snapshot.source_id == "BHalf"
                    && row.snapshot.from_entry == "B" && row.staged) {
                    b_relative_shadow_seen_ = true;
                    b_relative_shadow_rank_ = row.snapshot.chain_origin_sequence;
                }
            }
            return;
        }
    }

    void on_case_b_market_add_bar() {
        if (pine_bar_index() == 0) {
            strategy_entry("B", true, kNaN, kNaN, 0.02, "case-b-base");
            return;
        }
        if (pine_bar_index() == 1 && physical_position().signed_units > 0.0) {
            exit("BHalf", "B", 1000.0, kNaN, 100.0, 0.01, kNaN,
                 "case-b-control-half");
            exit("BRest", "B", 2000.0, kNaN, 100.0, kNaN, kNaN,
                 "case-b-control-rest");
            strategy_entry("Add", true, kNaN, kNaN, 0.02, "case-b-market-add");
            return;
        }
        if (pine_bar_index() == 4 && physical_position().signed_units > 0.0)
            strategy_close("Add", "case-b-add-close");
        if (pine_bar_index() == 5 && physical_position().signed_units > 0.0)
            strategy_close("B", "case-b-base-close");
    }

    // The exact row's three kinds of order, without process_orders_on_close.
    // The rows read back prove the form before any fill: the book is flat,
    // the entry is held and is not yet a working order, the partial exit
    // waits for the entry's fill, and the full exit has been placed.
    void on_held_entry_bar() {
        if (pine_bar_index() != 0) return;
        const bool flat_before = physical_position().signed_units == 0.0;
        strategy_entry("Long", true, kNaN, kNaN, kHeldEntryQty, "held-entry");
        exit("Half", "Long", kNaN, kNaN, kHeldPartialPercent, kNaN,
             kHeldProfitTicks, "held-tick-partial");
        exit("Rest", "Long", kNaN, kRestPrice, 100.0, kNaN, kNaN,
             "held-price-rest");
        held_flat_at_signal_ = flat_before
            && physical_position().signed_units == 0.0;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            const auto& snapshot = row.snapshot;
            if (snapshot.source_id == "Long" && snapshot.opening && row.staged)
                held_entry_staged_ = true;
            if (snapshot.source_id == "Half" && snapshot.from_entry == "Long"
                && row.staged) {
                held_partial_staged_ = true;
            }
            if (snapshot.source_id == "Rest" && snapshot.from_entry == "Long")
                held_full_exit_placed_ = true;
        }
    }

    Mode mode_;
    std::vector<Applied> applied_;
    bool b_relative_shadow_seen_ = false;
    std::uint64_t b_relative_shadow_rank_ = 0;
    bool held_flat_at_signal_ = false;
    bool held_entry_staged_ = false;
    bool held_partial_staged_ = false;
    bool held_full_exit_placed_ = false;
};

void print_applied(const Probe& probe) {
    for (const auto& row : probe.applied()) {
        std::printf("APPLIED label=%s bar=%d opened=%.17g closed=%.17g price=%.17g "
                    "position_after=%.17g\n",
                    row.label.c_str(), row.bar, row.opened, row.closed, row.price,
                    row.position_after);
    }
}

bool near(double left, double right) {
    return std::isfinite(left) && std::fabs(left - right) <= 1e-9;
}

bool has_applied(const Probe& probe, const char* label, double closed,
                 double opened, int* bar_out = nullptr) {
    for (const auto& row : probe.applied()) {
        if (row.label == label && near(row.closed, closed) && near(row.opened, opened)) {
            if (bar_out) *bar_out = row.bar;
            return true;
        }
    }
    return false;
}

const Applied* find_applied(const Probe& probe, const char* label, double closed,
                            double opened) {
    for (const auto& row : probe.applied()) {
        if (row.label == label && near(row.closed, closed) && near(row.opened, opened))
            return &row;
    }
    return nullptr;
}

// Every applied row of `label` that opened units, in the order they applied.
std::vector<const Applied*> opening_rows(const Probe& probe, const char* label) {
    std::vector<const Applied*> rows;
    for (const auto& row : probe.applied()) {
        if (row.label == label && row.opened > 0.0) rows.push_back(&row);
    }
    return rows;
}

// Every applied row of `label` that closed units, in the order they applied.
std::vector<const Applied*> closing_rows(const Probe& probe, const char* label) {
    std::vector<const Applied*> rows;
    for (const auto& row : probe.applied()) {
        if (row.label == label && row.closed > 0.0) rows.push_back(&row);
    }
    return rows;
}

bool check(bool condition, const char* mode, const char* name,
           double actual = kNaN, double expected = kNaN) {
    std::printf("CHECK mode=%s name=%s status=%s actual=%.17g expected=%.17g\n",
                mode, name, condition ? "PASS" : "FAIL", actual, expected);
    return condition;
}

bool check_case_a(Probe& probe, Mode mode) {
    const char* name = mode_name(mode);
    bool ok = true;
    int half_bar = -1;
    int rest_bar = -1;
    const auto* entry = find_applied(probe, "Long", 0.0, 0.02);
    const auto* half = find_applied(probe, "Half", 0.01, 0.0);
    const auto* rest = find_applied(probe, "Rest", 0.01, 0.0);
    ok = check(entry != nullptr, name, "pending_entry_reached") && ok;
    ok = check(half != nullptr, name, "partial_exit_reached") && ok;
    ok = check(rest != nullptr, name, "remainder_exit_reached") && ok;
    if (entry) {
        ok = check(near(entry->price, kEntryPrice), name,
                   "entry_fill_price", entry->price, kEntryPrice) && ok;
        ok = check(near(entry->position_after, 0.02), name,
                   "position_after_entry", entry->position_after, 0.02) && ok;
    }
    if (half) {
        half_bar = half->bar;
        ok = check(near(half->price, kHalfPrice), name,
                   "partial_exit_price", half->price, kHalfPrice) && ok;
        ok = check(near(half->position_after, 0.01), name,
                   "position_after_partial_exit", half->position_after, 0.01) && ok;
    }
    if (rest) {
        rest_bar = rest->bar;
        ok = check(near(rest->price, kRestPrice), name,
                   "remainder_exit_price", rest->price, kRestPrice) && ok;
        ok = check(near(rest->position_after, 0.0), name,
                   "position_after_remainder_exit", rest->position_after, 0.0) && ok;
    }
    ok = check(half_bar == 1, name, "partial_exit_bar", half_bar, 1) && ok;
    ok = check(rest_bar == 2, name, "remainder_exit_bar", rest_bar, 2) && ok;
    ok = check(probe.trade_count() == 2, name, "two_closed_trade_rows",
               probe.trade_count(), 2) && ok;
    if (probe.trade_count() == 2) {
        ok = check(near(probe.trade_size(0), 0.01), name,
                   "first_trade_partial_quantity", probe.trade_size(0), 0.01) && ok;
        ok = check(near(probe.trade_size(1), 0.01), name,
                   "second_trade_remainder_quantity", probe.trade_size(1), 0.01) && ok;
        ok = check(probe.trade_exit_id(0) == "Half", name,
                   "first_trade_is_half") && ok;
        ok = check(probe.trade_exit_id(1) == "Rest", name,
                   "second_trade_is_rest") && ok;
    }
    return ok;
}

bool check_b_pending(Probe& probe, const std::vector<Bar>& bars) {
    const char* name = mode_name(Mode::BPendingEntryControl);
    bool ok = true;
    int entry_bar = -1;
    int half_bar = -1;
    ok = check(probe.b_relative_shadow_seen(), name,
               "relative_pending_shadow_seen") && ok;
    ok = check(probe.b_relative_shadow_rank() == 0, name,
               "relative_pending_shadow_rank",
               static_cast<double>(probe.b_relative_shadow_rank()), 0) && ok;
    const bool entry_reached = has_applied(probe, "B", 0.0, 0.02, &entry_bar);
    ok = check(entry_reached, name,
               "pending_entry_reached", entry_bar, 1) && ok;
    ok = check(entry_bar == 1, name, "pending_entry_bar",
               static_cast<double>(entry_bar), 1) && ok;
    const Applied* half = find_applied(probe, "BHalf", 0.01, 0.0);
    if (half) half_bar = half->bar;
    ok = check(half != nullptr, name,
               "relative_pending_partial_reached") && ok;
    // First eligible bar, reasoned from the fixture rather than from the fill:
    // B is submitted on bar 0 without process_orders_on_close, so it fills at
    // bar 1's open (100) and the 100-tick profit at mintick 0.01 is a sell
    // limit at 101.  Bar 1's high is 100; bar 2's high is the first to touch
    // 101, and a limit order fills on touch by default.
    ok = check(half_bar == 2, name, "relative_pending_partial_bar",
               static_cast<double>(half_bar), 2) && ok;
    // The price by the same reasoning, computed and not typed: the level is
    // bar 1's open plus the profit distance in ticks, and a limit that a
    // crossing reaches fills at its level.
    if (half) {
        const double level = bars[1].open + kBProfitTicks * kMinTick;
        ok = check(near(half->price, level), name,
                   "relative_pending_partial_price", half->price, level) && ok;
    }
    ok = check(probe.trade_count() == 1, name, "one_closed_trade_row",
               probe.trade_count(), 1) && ok;
    if (probe.trade_count() == 1) {
        ok = check(near(probe.trade_size(0), 0.01), name,
                   "relative_pending_trade_quantity",
                   probe.trade_size(0), 0.01) && ok;
        ok = check(probe.trade_exit_id(0) == "BHalf", name,
                   "relative_pending_trade_exit") && ok;
    }
    ok = check(near(probe.physical_position().signed_units, 0.01), name,
               "relative_pending_position_after_fill",
               probe.physical_position().signed_units, 0.01) && ok;
    return ok;
}

bool check_b_market_add(Probe& probe) {
    const char* name = mode_name(Mode::BMarketAddControl);
    bool ok = true;
    int add_bar = -1;
    ok = check(has_applied(probe, "B", 0.0, 0.02), name,
               "pending_entry_reached") && ok;
    const bool add_reached = has_applied(probe, "Add", 0.0, 0.02, &add_bar);
    ok = check(add_reached, name,
               "separate_market_add_reached", add_bar, 2) && ok;
    ok = check(add_bar == 2, name, "separate_market_add_bar",
               static_cast<double>(add_bar), 2) && ok;
    ok = check(has_applied(probe, "__close__Add", 0.02, 0.0), name,
               "market_add_close_reached") && ok;
    ok = check(has_applied(probe, "__close__B", 0.02, 0.0), name,
               "base_close_reached") && ok;
    return ok;
}

// The held-entry rows.  Every expected value is computed from the fixture by
// a documented order rule; none is read from a run.
//
// No single published page states these rules.  The fill bar, the fill price
// and the reserved quantity are spread over four documents:
//   docs/pine-adapter-kernel-notes.md   R1, R3, R5
//   docs/pages/native-engine.md         R1, R3
//   docs/pages/pine-to-native.md        R2
//   docs/pages/quantity-intent.md       R4, R5
// docs/pages/fill-model.md gives R6 and R7 only, and CHANGELOG.md gives R4
// and R5 in words.  src/source/pine_adapter.cpp states each rule where it
// applies it.  Every citation below is to the lines that state the rule.
//
//   R1  Without process_orders_on_close a market order fills at the next
//       bar's open.
//       - docs/pine-adapter-kernel-notes.md:237: a market fill at the next
//         bar is quoted at that bar's open; :239: under
//         process_orders_on_close, at the signal close.
//       - docs/pages/native-engine.md:3366-3367: an accepted order has no
//         fill yet, and the next bar's modeled open is the fill.
//       - src/source/pine_adapter.cpp:3135-3136: without
//         process_orders_on_close an order executes at the next eligible
//         point; :20065-20067: its quote is the next open.
//   R2  A level given in ticks is the entry's fill price moved by that many
//       ticks.
//       - docs/pages/pine-to-native.md:343-345: at the parent's fill the
//         level becomes the fill plus the offset, in ticks.
//       - src/source/pine_adapter.cpp:15558-15564
//         (materialize_relative_exits): the limit is the entry's fill price
//         plus the profit ticks, on the tick grid.
//   R3  A limit that a crossing reaches fills at its level, and a sell stop
//       at the lower of the bar's open and its level.
//       - docs/pages/native-engine.md:520-522: the path has to reach a
//         limit's level, and a crossing books the level itself.
//       - docs/pine-adapter-kernel-notes.md:238: a stop is quoted at
//         min(open, stop) for a sell.
//       - src/source/pine_adapter.cpp:20066: max/min(open, stop) for a stop;
//         :21842-21843: a touched level fills at its level.  The adapter
//         leaves a limit's price to the kernel's matcher.
//   R4  Exits reserve the entry's quantity in the order they were placed,
//       and a later full exit is left what the earlier ones have not
//       reserved.
//       - CHANGELOG.md:4064-4067 (1.0.0): the strategy.exit orders of one
//         entry queue in the order they were created, and a later exit gets
//         only what the earlier ones leave.
//       - CHANGELOG.md:29 (unreleased, #810): a partial exit placed before
//         its market entry fills keeps its place ahead of a full exit placed
//         after it, which closes only the remainder.
//       - docs/pages/quantity-intent.md:35-37: a full request clipped behind
//         another reservation holds less than its basis.
//       - src/source/pine_adapter.cpp:859-862 (exit_queue_rank), :5880-5884
//         (compute_exit_reservation), :6226-6227 and :6277-6303
//         (reconcile_deferred_exit_reservations): the entry's quantity goes
//         to its exits in queue order, each taking what the ones before it
//         leave; :12901-12924 (exit_with_reservation_sequence): a waiting
//         partial exit gets its place when a later exit of the entry is
//         placed.
//   R5  An exit placed while its entry was unfilled holds a fixed share of
//       what the entry opens, whichever exit fills first.
//       - docs/pine-adapter-kernel-notes.md:963-971: when the entry opens,
//         each such exit is bound to a fixed share of the opened lot, so
//         what it closes does not depend on which exit fills first.
//       - docs/pages/quantity-intent.md:22-23: a deferred exit acquires its
//         reservation when a live owner is bound.
//       - CHANGELOG.md:29 (unreleased, #810): the partial exit is sized from
//         the quantity the entry actually opens.
//       - src/source/pine_adapter.cpp:15545-15557
//         (materialize_relative_exits): the partial exit's quantity is its
//         percentage of what the entry opened; :6321-6327
//         (reconcile_deferred_exit_reservations): each exit's units are
//         written on its order rows; :17360-17375 (resolve_source_terms): a
//         fixed reservation executes as written.
//   R6  A closing order closes no more than is held and never opens a
//       position.
//       - docs/pages/fill-model.md:91-92: a reduction is capped at the held
//         exposure and never opens or flips; :214: it never creates new
//         exposure.
//       - src/source/pine_adapter.cpp:6277-6279: what an exit may take is
//         what is held less what the exits before it took, never below zero.
//   R7  An order that has filled is not filled again.
//       - docs/pages/fill-model.md:176: a retired object is not dispatched
//         again.
//       - src/source/pine_adapter.cpp:26683-26685 (on_applied): an exit that
//         has filled on an opening does not fill again on that opening.
bool check_held_entry(Probe& probe, Mode mode, const std::vector<Bar>& bars) {
    const char* name = mode_name(mode);
    const bool stop_first = mode == Mode::HeldEntryStopFirst;
    bool ok = true;

    // The form, read on the signal bar before any fill.
    ok = check(probe.held_flat_at_signal(), name, "flat_at_signal") && ok;
    ok = check(probe.held_entry_staged(), name, "entry_held_at_signal") && ok;
    ok = check(probe.held_partial_staged(), name,
               "partial_exit_waiting_at_signal") && ok;
    ok = check(probe.held_full_exit_placed(), name,
               "full_exit_placed_at_signal") && ok;

    const double entry_price = bars[1].open;                               // R1
    const double limit_level = entry_price + kHeldProfitTicks * kMinTick;  // R2
    const double stop_level = kRestPrice;
    const double share = kHeldEntryQty * kHeldPartialPercent / 100.0;      // R5
    const double remainder = kHeldEntryQty - share;                        // R4
    // Bars 2 and 3 each reach one level (bars_for).
    const int partial_bar = stop_first ? 3 : 2;
    const int full_bar = stop_first ? 2 : 3;

    // R1: the entry fills once, on bar 1, at that bar's open.
    const auto entries = opening_rows(probe, "Long");
    ok = check(entries.size() == 1, name, "entry_filled_once",
               static_cast<double>(entries.size()), 1) && ok;
    if (!entries.empty()) {
        const Applied& entry = *entries.front();
        ok = check(entry.bar == 1, name, "entry_fill_bar", entry.bar, 1) && ok;
        ok = check(near(entry.price, entry_price), name, "entry_fill_price",
                   entry.price, entry_price) && ok;
        ok = check(near(entry.opened, kHeldEntryQty), name, "entry_fill_quantity",
                   entry.opened, kHeldEntryQty) && ok;
        ok = check(near(entry.position_after, kHeldEntryQty), name,
                   "position_after_entry", entry.position_after,
                   kHeldEntryQty) && ok;
    }

    // R7: each exit fills once.  R5, R2 and R3: the partial exit closes its
    // share at its level, on the bar that reaches it.
    const auto partials = closing_rows(probe, "Half");
    ok = check(partials.size() == 1, name, "partial_exit_filled_once",
               static_cast<double>(partials.size()), 1) && ok;
    if (!partials.empty()) {
        const Applied& partial = *partials.front();
        const double left = stop_first ? 0.0 : remainder;
        ok = check(near(partial.closed, share), name, "partial_exit_quantity",
                   partial.closed, share) && ok;
        ok = check(partial.bar == partial_bar, name, "partial_exit_bar",
                   partial.bar, partial_bar) && ok;
        ok = check(near(partial.price, limit_level), name, "partial_exit_price",
                   partial.price, limit_level) && ok;
        ok = check(near(partial.position_after, left), name,
                   "position_after_partial_exit", partial.position_after,
                   left) && ok;
    }

    // R4 and R3: the full exit closes exactly what the partial exit has not
    // reserved, at its stop, whether it fills before the partial or after.
    const auto fulls = closing_rows(probe, "Rest");
    ok = check(fulls.size() == 1, name, "full_exit_filled_once",
               static_cast<double>(fulls.size()), 1) && ok;
    if (!fulls.empty()) {
        const Applied& full = *fulls.front();
        const double left = stop_first ? share : 0.0;
        ok = check(near(full.closed, remainder), name, "full_exit_quantity",
                   full.closed, remainder) && ok;
        ok = check(full.bar == full_bar, name, "full_exit_bar",
                   full.bar, full_bar) && ok;
        ok = check(near(full.price, stop_level), name, "full_exit_price",
                   full.price, stop_level) && ok;
        ok = check(near(full.position_after, left), name,
                   "position_after_full_exit", full.position_after,
                   left) && ok;
    }

    // R6: no fill takes more than was held before it, and the exits together
    // close what the entry opened, so nothing is left open and nothing is
    // closed twice.
    double opened = 0.0;
    double closed = 0.0;
    bool within_held = true;
    for (const auto& row : probe.applied()) {
        opened += row.opened;
        closed += row.closed;
        if (closed > opened + 1e-9 || row.position_after < -1e-9)
            within_held = false;
    }
    ok = check(within_held, name, "no_fill_exceeds_held_quantity") && ok;
    ok = check(opened > 0.0 && near(closed, opened), name,
               "closed_equals_opened", closed, opened) && ok;
    ok = check(near(probe.physical_position().signed_units, 0.0), name,
               "final_position_flat",
               probe.physical_position().signed_units, 0.0) && ok;

    // The closed trades say the same.
    ok = check(probe.trade_count() == 2, name, "two_closed_trade_rows",
               probe.trade_count(), 2) && ok;
    if (probe.trade_count() == 2) {
        const int partial_row = stop_first ? 1 : 0;
        const int full_row = stop_first ? 0 : 1;
        ok = check(probe.trade_exit_id(partial_row) == "Half", name,
                   "partial_trade_exit") && ok;
        ok = check(near(probe.trade_size(partial_row), share), name,
                   "partial_trade_quantity", probe.trade_size(partial_row),
                   share) && ok;
        ok = check(near(probe.trade_exit_price(partial_row), limit_level), name,
                   "partial_trade_exit_price",
                   probe.trade_exit_price(partial_row), limit_level) && ok;
        ok = check(probe.trade_exit_id(full_row) == "Rest", name,
                   "full_trade_exit") && ok;
        ok = check(near(probe.trade_size(full_row), remainder), name,
                   "full_trade_quantity", probe.trade_size(full_row),
                   remainder) && ok;
        ok = check(near(probe.trade_exit_price(full_row), stop_level), name,
                   "full_trade_exit_price",
                   probe.trade_exit_price(full_row), stop_level) && ok;
        for (const int row : {0, 1}) {
            ok = check(probe.trade_entry_id(row) == "Long", name,
                       "trade_entry") && ok;
            ok = check(near(probe.trade_entry_price(row), entry_price), name,
                       "trade_entry_price", probe.trade_entry_price(row),
                       entry_price) && ok;
        }
    }
    return ok;
}

std::vector<Bar> bars_for(Mode mode) {
    if (mode == Mode::Exact || mode == Mode::AbsoluteControl) {
        return {
            bar(0, kEntryPrice, kEntryPrice, kEntryPrice, kEntryPrice),
            bar(1, kEntryPrice, kHalfPrice, kEntryPrice, kHalfPrice),
            bar(2, kHalfPrice, kHalfPrice, kRestPrice, 43000.0),
        };
    }
    if (mode == Mode::BPendingEntryControl) {
        return {
            bar(0, 100.0, 100.0, 100.0, 100.0),
            bar(1, 100.0, 100.0, 100.0, 100.0),
            bar(2, 100.0, 101.0, 99.0, 100.5),
            bar(3, 101.0, 102.0, 100.0, 101.5),
            bar(4, 101.5, 101.5, 101.5, 101.5),
        };
    }
    if (mode == Mode::HeldEntryStopFirst || mode == Mode::HeldEntryLimitFirst) {
        // Bar 1 only fills the entry, at its open.  Bars 2 and 3 each reach
        // one exit level and travel past it: the stop first, or the limit
        // first.  Bar 4 crosses both levels again with nothing left to fill.
        const double above =
            kEntryPrice + kHeldProfitTicks * kMinTick + kHeldOvershoot;
        const double below = kRestPrice - kHeldOvershoot;
        const bool stop_first = mode == Mode::HeldEntryStopFirst;
        return {
            bar(0, kEntryPrice, kEntryPrice, kEntryPrice, kEntryPrice),
            bar(1, kEntryPrice, kEntryPrice, kEntryPrice, kEntryPrice),
            stop_first ? bar(2, kEntryPrice, kEntryPrice, below, kEntryPrice)
                       : bar(2, kEntryPrice, above, kEntryPrice, kEntryPrice),
            stop_first ? bar(3, kEntryPrice, above, kEntryPrice, kEntryPrice)
                       : bar(3, kEntryPrice, kEntryPrice, below, kEntryPrice),
            bar(4, kEntryPrice, above, below, kEntryPrice),
        };
    }
    return {
        bar(0, 100.0, 100.0, 100.0, 100.0),
        bar(1, 100.0, 100.0, 100.0, 100.0),
        bar(2, 100.0, 100.0, 100.0, 100.0),
        bar(3, 100.0, 100.0, 100.0, 100.0),
        bar(4, 100.0, 100.0, 100.0, 100.0),
        bar(5, 100.0, 100.0, 100.0, 100.0),
        bar(6, 100.0, 100.0, 100.0, 100.0),
    };
}

bool run_mode(Mode mode) {
    const char* name = mode_name(mode);
    Probe probe(mode);
    const auto bars = bars_for(mode);
    probe.fixture_retain_all_events();
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    std::printf("EXECUTION mode=%s status=%s last_error=%s\n", name,
                probe.last_error().empty() ? "completed" : "failed",
                probe.last_error().c_str());
    print_applied(probe);
    for (int index = 0; index < probe.trade_count(); ++index) {
        std::printf("TRADE mode=%s index=%d entry=%s exit=%s qty=%.17g "
                    "entry_price=%.17g exit_price=%.17g\n",
                    name, index, probe.trade_entry_id(index).c_str(),
                    probe.trade_exit_id(index).c_str(), probe.trade_size(index),
                    probe.trade_entry_price(index), probe.trade_exit_price(index));
    }
    const auto position = probe.physical_position();
    std::printf("POSITION mode=%s signed_units=%.17g lot_count=%zu trades=%d\n",
                name, position.signed_units, position.lot_count, probe.trade_count());
    std::printf("REPORT mode=%s broker_state_hash=%llu\n", name,
                static_cast<unsigned long long>(probe.broker_state_hash()));
    if (!probe.last_error().empty()) return false;
    if (mode == Mode::Exact || mode == Mode::AbsoluteControl)
        return check_case_a(probe, mode);
    if (mode == Mode::BPendingEntryControl) return check_b_pending(probe, bars);
    if (mode == Mode::HeldEntryStopFirst || mode == Mode::HeldEntryLimitFirst)
        return check_held_entry(probe, mode, bars);
    return check_b_market_add(probe);
}

bool parse_mode(const std::string& value, Mode& out) {
    if (value == "exact") out = Mode::Exact;
    else if (value == "a_absolute_control") out = Mode::AbsoluteControl;
    else if (value == "b_pending_entry_control") out = Mode::BPendingEntryControl;
    else if (value == "b_market_add_control") out = Mode::BMarketAddControl;
    else if (value == "held_entry_stop_first") out = Mode::HeldEntryStopFirst;
    else if (value == "held_entry_limit_first") out = Mode::HeldEntryLimitFirst;
    else return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        Mode mode;
        if (!parse_mode(argv[1], mode)) return 2;
        return run_mode(mode) ? 0 : 1;
    }
    bool ok = true;
    for (const Mode mode : {Mode::Exact, Mode::AbsoluteControl,
                            Mode::BPendingEntryControl, Mode::BMarketAddControl,
                            Mode::HeldEntryStopFirst, Mode::HeldEntryLimitFirst}) {
        ok = run_mode(mode) && ok;
    }
    return ok ? 0 : 1;
}
