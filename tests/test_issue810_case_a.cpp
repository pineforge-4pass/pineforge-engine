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
// The step rows place the exact row's three kinds of order in whole steps of
// the quantity step: three steps with a 50% partial exit, whose share is one
// and a half steps, and a four-step control.  After both exits nothing may be
// left for the end close. Two more rows compare a real positive with an empty
// or stale observation; independent controls isolate bar, price, phase and
// end-close evidence.
// The step rows come in two forms.  Under process_orders_on_close the entry
// is an order at once.  The live-entry rows run without it and with a
// commission, which keeps the entry from being held: it is a working order
// when the exits are placed, and fills at the next bar's open.

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
constexpr int kCaseASignalBar = 0;
constexpr int kBBaseSignalBar = 0;
constexpr int kBAddSignalBar = 1;
constexpr int kBAddCloseSignalBar = 4;
constexpr int kBBaseCloseSignalBar = 5;
// The held-entry rows: the entry's quantity, the partial exit's share of it in
// percent, and that exit's profit distance in ticks.  The quantity is four
// steps, so the share and what it leaves are different amounts.
constexpr double kHeldEntryQty = 0.04;
constexpr double kHeldPartialPercent = 25.0;
constexpr double kHeldProfitTicks = 50000.0;
// How far past a level the bar that reaches it travels, so that a fill booked
// at the bar's extreme is told from a fill booked at the level.
constexpr double kHeldOvershoot = 10.0;
// The quantity step of every row: the smallest quantity an order can trade.
constexpr double kQtyStep = 0.01;
// The step rows, in synthetic prices.  The entry is three steps of kQtyStep,
// or four in the control; the partial exit is 50% with a tick profit and the
// full exit a price stop.  Bars after the first open kStepGap higher, and the
// bar that reaches a level travels kStepOvershoot past it.
constexpr double kStepOddEntryQty = 0.03;
constexpr double kStepWholeEntryQty = 0.04;
constexpr double kStepPartialPercent = 50.0;
constexpr double kStepProfitTicks = 100.0;
constexpr double kStepBasePrice = 100.0;
constexpr double kStepStopPrice = 99.0;
constexpr double kStepGap = 0.25;
constexpr double kStepOvershoot = 0.5;
// The script offers an end close after both crossing bars.
constexpr int kStepEndCloseBar = 3;
// The live-entry step rows: a commission in percent, the one setting that
// keeps their entry from being held, and the bar of their end close.  Their
// entry fills one bar later than under process_orders_on_close.
constexpr double kStepCommissionPercent = 0.1;
constexpr int kLiveEndCloseBar = 4;

enum class Mode {
    Exact,
    AbsoluteControl,
    BPendingEntryControl,
    BMarketAddControl,
    HeldEntryStopFirst,
    HeldEntryLimitFirst,
    PoocOddStepsLimitFirst,
    PoocOddStepsStopFirst,
    PoocWholeStepsLimitFirst,
    PoocWholeStepsStopFirst,
    LiveEntryOddStepsLimitFirst,
    LiveEntryOddStepsStopFirst,
    LiveEntryWholeStepsLimitFirst,
    LiveEntryWholeStepsStopFirst,
    StepChecksEmptyInput,
    StepChecksStaleInput,
};

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Exact: return "exact";
        case Mode::AbsoluteControl: return "a_absolute_control";
        case Mode::BPendingEntryControl: return "b_pending_entry_control";
        case Mode::BMarketAddControl: return "b_market_add_control";
        case Mode::HeldEntryStopFirst: return "held_entry_stop_first";
        case Mode::HeldEntryLimitFirst: return "held_entry_limit_first";
        case Mode::PoocOddStepsLimitFirst: return "pooc_odd_steps_limit_first";
        case Mode::PoocOddStepsStopFirst: return "pooc_odd_steps_stop_first";
        case Mode::PoocWholeStepsLimitFirst: return "pooc_whole_steps_limit_first";
        case Mode::PoocWholeStepsStopFirst: return "pooc_whole_steps_stop_first";
        case Mode::LiveEntryOddStepsLimitFirst:
            return "live_entry_odd_steps_limit_first";
        case Mode::LiveEntryOddStepsStopFirst:
            return "live_entry_odd_steps_stop_first";
        case Mode::LiveEntryWholeStepsLimitFirst:
            return "live_entry_whole_steps_limit_first";
        case Mode::LiveEntryWholeStepsStopFirst:
            return "live_entry_whole_steps_stop_first";
        case Mode::StepChecksEmptyInput: return "step_checks_empty_input";
        case Mode::StepChecksStaleInput: return "step_checks_stale_input";
    }
    return "unknown";
}

// The step rows that run the engine, in their two forms.  Under
// process_orders_on_close the entry is an order at once.  In the live-entry
// rows a commission keeps it from being held to the end of its calculation:
// In src/source/pine_adapter.cpp, same_bar_market_tx_scope requires zero
// commission. The other two cases in entry (the flat pair and
// fixed_unmargined_market_batch_scope) require pyramiding of at least two.
bool is_pooc_step_mode(Mode mode) {
    return mode == Mode::PoocOddStepsLimitFirst
        || mode == Mode::PoocOddStepsStopFirst
        || mode == Mode::PoocWholeStepsLimitFirst
        || mode == Mode::PoocWholeStepsStopFirst;
}

bool is_live_step_mode(Mode mode) {
    return mode == Mode::LiveEntryOddStepsLimitFirst
        || mode == Mode::LiveEntryOddStepsStopFirst
        || mode == Mode::LiveEntryWholeStepsLimitFirst
        || mode == Mode::LiveEntryWholeStepsStopFirst;
}

bool is_step_mode(Mode mode) {
    return is_pooc_step_mode(mode) || is_live_step_mode(mode);
}

double step_entry_qty(Mode mode) {
    const bool whole = mode == Mode::PoocWholeStepsLimitFirst
        || mode == Mode::PoocWholeStepsStopFirst
        || mode == Mode::LiveEntryWholeStepsLimitFirst
        || mode == Mode::LiveEntryWholeStepsStopFirst;
    return whole ? kStepWholeEntryQty : kStepOddEntryQty;
}

bool step_stop_first(Mode mode) {
    return mode == Mode::PoocOddStepsStopFirst
        || mode == Mode::PoocWholeStepsStopFirst
        || mode == Mode::LiveEntryOddStepsStopFirst
        || mode == Mode::LiveEntryWholeStepsStopFirst;
}

int step_end_close_bar(Mode mode) {
    return is_live_step_mode(mode) ? kLiveEndCloseBar : kStepEndCloseBar;
}

struct Applied {
    std::string label;
    int bar = -1;
    double opened = 0.0;
    double closed = 0.0;
    double price = kNaN;
    double position_after = 0.0;
    NativePathPhase phase = NativePathPhase::None;
    bool after_calculation_close = false;
};

struct EndClose {
    int bar = -1;
    int visits = 0;
    double position = kNaN;
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
        config.commission_value =
            is_live_step_mode(mode) ? kStepCommissionPercent : 0.0;
        config.slippage = 0;
        config.pyramiding = mode == Mode::BMarketAddControl ? 2 : 1;
        config.process_orders_on_close =
            mode == Mode::Exact || mode == Mode::AbsoluteControl
            || is_pooc_step_mode(mode);
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(kMinTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kQtyStep);
    }

    void on_source_bar(const Bar& current) override {
        last_source_bar_ = pine_bar_index();
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
            case Mode::PoocOddStepsLimitFirst:
            case Mode::PoocOddStepsStopFirst:
            case Mode::PoocWholeStepsLimitFirst:
            case Mode::PoocWholeStepsStopFirst:
            case Mode::LiveEntryOddStepsLimitFirst:
            case Mode::LiveEntryOddStepsStopFirst:
            case Mode::LiveEntryWholeStepsLimitFirst:
            case Mode::LiveEntryWholeStepsStopFirst:
                on_step_bar(current);
                return;
            case Mode::StepChecksEmptyInput:
            case Mode::StepChecksStaleInput:
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
                            physical_position().signed_units,
                            context.coordinate.path_phase,
                            event.cursor.point.provenance == NativePriceProvenance::AfterCalculationClose});
    }

    double trade_size(int index) const { return closed_trade_size(index); }
    std::string trade_entry_id(int index) const { return closed_trade_entry_id(index); }
    std::string trade_exit_id(int index) const { return closed_trade_exit_id(index); }
    double trade_entry_price(int index) const {
        return closed_trade_entry_price(index);
    }
    double trade_exit_price(int index) const { return closed_trade_exit_price(index); }

    const std::vector<Applied>& applied() const { return applied_; }
    Mode mode() const { return mode_; }
    int last_source_bar() const { return last_source_bar_; }
    const EndClose& end_close() const { return end_close_; }
    bool b_relative_shadow_seen() const { return b_relative_shadow_seen_; }
    std::uint64_t b_relative_shadow_rank() const { return b_relative_shadow_rank_; }
    bool held_flat_at_signal() const { return held_flat_at_signal_; }
    bool held_entry_staged() const { return held_entry_staged_; }
    bool held_partial_staged() const { return held_partial_staged_; }
    bool held_full_exit_placed() const { return held_full_exit_placed_; }
    bool step_entry_order_at_signal() const { return step_entry_order_at_signal_; }
    // The delivered path is independent of whether either order filled.
    int stop_first_paths = 0;

private:
    void exit(const char* id, const char* from, double limit, double stop,
              double qty_percent, double qty, double profit_ticks,
              const char* comment) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, qty_percent,
                      comment, qty, "", profit_ticks, kNaN);
    }

    void on_case_a_bar() {
        if (pine_bar_index() != kCaseASignalBar) return;
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
        if (pine_bar_index() == kBBaseSignalBar) {
            strategy_entry("B", true, kNaN, kNaN, 0.02, "case-b-base");
            return;
        }
        if (pine_bar_index() == kBAddSignalBar && physical_position().signed_units > 0.0) {
            exit("BHalf", "B", 1000.0, kNaN, 100.0, 0.01, kNaN,
                 "case-b-control-half");
            exit("BRest", "B", 2000.0, kNaN, 100.0, kNaN, kNaN,
                 "case-b-control-rest");
            strategy_entry("Add", true, kNaN, kNaN, 0.02, "case-b-market-add");
            return;
        }
        if (pine_bar_index() == kBAddCloseSignalBar && physical_position().signed_units > 0.0)
            strategy_close("Add", "case-b-add-close");
        if (pine_bar_index() == kBBaseCloseSignalBar && physical_position().signed_units > 0.0)
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

    // The exact row's three calls in quantity steps, then an end close as a
    // script makes it on a last bar.  It is called only while something is
    // still held, so whatever the two exits leave shows up as a third closing
    // fill.  Rounding noise far below a step is not a holding.
    void on_step_bar(const Bar& current) {
        if (pine_bar_index() == 0) {
            strategy_entry("Long", true, kNaN, kNaN, step_entry_qty(mode_),
                           "step-entry");
            exit("Half", "Long", kNaN, kNaN, kStepPartialPercent, kNaN,
                 kStepProfitTicks, "step-tick-partial");
            exit("Rest", "Long", kNaN, kStepStopPrice, 100.0, kNaN, kNaN,
                 "step-price-rest");
            // In a live-entry row the entry must be a working order here, not
            // one held to the end of the calculation: the fixture lists a
            // working order unstaged.
            if (is_live_step_mode(mode_)) {
                for (const auto& row : adapter_.fixture_pending_snapshots()) {
                    if (row.snapshot.source_id == "Long" && row.snapshot.opening
                        && !row.staged) {
                        step_entry_order_at_signal_ = true;
                    }
                }
            }
            return;
        }
        const bool pooc = is_pooc_step_mode(mode_);
        if (pooc && pine_bar_index() == 1 && current.open > kStepStopPrice
            && current.low < kStepStopPrice && current.close > kStepStopPrice
            && current.high < kStepBasePrice + kStepProfitTicks * kMinTick) {
            ++stop_first_paths;
        }
        if (pine_bar_index() == step_end_close_bar(mode_)) {
            // Observe the guarded action even when it has nothing to close.
            const double position = physical_position().signed_units;
            end_close_ = {pine_bar_index(), end_close_.visits + 1, position};
            if (std::fabs(position) > 1e-9) strategy_close("Long", "step-end-close");
        }
    }

    Mode mode_;
    std::vector<Applied> applied_;
    int last_source_bar_ = -1;
    EndClose end_close_;
    bool b_relative_shadow_seen_ = false;
    std::uint64_t b_relative_shadow_rank_ = 0;
    bool held_flat_at_signal_ = false;
    bool held_entry_staged_ = false;
    bool held_partial_staged_ = false;
    bool held_full_exit_placed_ = false;
    bool step_entry_order_at_signal_ = false;
};

const char* phase_name(NativePathPhase phase) {
    switch (phase) {
        case NativePathPhase::None: return "none";
        case NativePathPhase::Open: return "open";
        case NativePathPhase::High: return "high";
        case NativePathPhase::Low: return "low";
        case NativePathPhase::Close: return "close";
    }
    return "unknown";
}

void print_applied(const Probe& probe, const char* mode) {
    for (const auto& row : probe.applied()) {
        std::printf("APPLIED mode=%s label=%s bar=%d opened=%.17g closed=%.17g price=%.17g "
                    "position_after=%.17g phase=%s\n",
                    mode, row.label.c_str(), row.bar, row.opened, row.closed, row.price,
                    row.position_after, phase_name(row.phase));
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

struct ClosedTrade {
    std::string exit_id;
    double size = 0.0;
};

struct Observed {
    std::vector<Applied> applied;
    std::vector<ClosedTrade> trades;
    bool entry_order_at_signal = false;
    std::optional<Mode> source;
    std::optional<NativeLifecycleKind> lifecycle;
    std::string error;
    int last_source_bar = -1;
    EndClose end_close;
    double final_position = kNaN;
    int stop_first_paths = 0;
};

Observed observe(Probe& probe) {
    Observed seen;
    seen.applied = probe.applied();
    for (int index = 0; index < probe.trade_count(); ++index)
        seen.trades.push_back({probe.trade_exit_id(index), probe.trade_size(index)});
    seen.entry_order_at_signal = probe.step_entry_order_at_signal();
    seen.source = probe.mode();
    seen.lifecycle = probe.native_state().kind;
    seen.error = probe.last_error();
    seen.last_source_bar = probe.last_source_bar();
    seen.end_close = probe.end_close();
    seen.final_position = probe.physical_position().signed_units;
    seen.stop_first_paths = probe.stop_first_paths;
    return seen;
}

struct Comparison {
    const char* name = "";
    bool ok = false;
    double actual = kNaN;
    double expected = kNaN;
    const char* missing = nullptr;
    std::string actual_ids{}, expected_ids{};
    const char* reason = "value comparison";

    void require(bool met, const char* why, double got, double want) {
        if (met || missing) return;
        ok = false;
        missing = why;
        actual = got;
        expected = want;
    }
    void one(std::size_t count, const char* what) {
        require(count == 1, what, static_cast<double>(count), 1.0);
    }
    void expect(bool met, const char* what, double got, double want) {
        if (!ok || missing) return;
        ok = met;
        reason = what;
        actual = got;
        expected = want;
    }
};

bool report_comparison(const Comparison& r, const char* mode,
                       const char* input = "ROW") {
    const char* status = r.missing ? "NOT COMPARED" : (r.ok ? "PASS" : "FAIL");
    const char* reason = r.missing ? r.missing : r.reason;
    std::printf("CHECK mode=%s name=%s input=%s status=%s reason=%s ",
                mode, r.name, input, status, reason);
    if (!r.expected_ids.empty())
        std::printf("actual=%s expected=%s\n", r.actual_ids.c_str(), r.expected_ids.c_str());
    else
        std::printf("actual=%.17g expected=%.17g\n", r.actual, r.expected);
    return r.ok && !r.missing;
}

void require_completed(Comparison& r, const Observed& seen, Mode mode) {
    if (seen.source != mode) {
        if (r.missing) return;
        r.ok = false;
        r.missing = "observation source row";
        r.actual_ids = seen.source ? mode_name(*seen.source) : "absent";
        r.expected_ids = mode_name(mode);
        return;
    }
    if (!seen.error.empty() || seen.lifecycle != NativeLifecycleKind::Completed) {
        if (r.missing) return;
        r.ok = false;
        r.missing = "incomplete run";
        r.actual_ids = "state:" + std::to_string(seen.lifecycle
            ? static_cast<int>(*seen.lifecycle) : -1) + ",error:" + seen.error;
        r.expected_ids = "state:" + std::to_string(
            static_cast<int>(NativeLifecycleKind::Completed)) + ",error:empty";
    }
}

void require_run(Comparison& r, const Observed& seen, Mode mode, int last_bar) {
    require_completed(r, seen, mode);
    r.require(seen.last_source_bar == last_bar, "last source bar",
              seen.last_source_bar, last_bar);
}

struct NamedFill {
    const Applied* only = nullptr;
    std::size_t count = 0;
};

NamedFill named_fill(const Observed& seen, const char* label) {
    NamedFill result;
    for (const auto& row : seen.applied) {
        if (row.label != label) continue;
        ++result.count;
        result.only = result.count == 1 ? &row : nullptr;
    }
    return result;
}

enum class Field { Bar, Price, Phase };

const char* field_reason(Field field) {
    switch (field) {
        case Field::Bar: return "fill bar";
        case Field::Price: return "fill price";
        case Field::Phase: return "fill phase";
    }
    return "unknown field";
}

struct FillRule {
    const char* label;
    double closed;
    double opened;
    const char* reached;
    const char* bar_check;
    int bar;
    const char* price_check;
    double price;
    const char* phase_check = nullptr;
    NativePathPhase phase = NativePathPhase::None;
};

// The expectations come from the call sites and feed, not an applied event.
// docs/pine-adapter-kernel-notes.md:237-239 and docs/pages/pine-to-native.md:130:
// POOC fills a market entry at its signal close; ordinary markets (including
// the B row's two non-immediate strategy_close calls) fill at the next open.
std::vector<FillRule> authored_fills(Mode mode, const std::vector<Bar>& bars) {
    if (mode == Mode::BMarketAddControl) {
        const int base = kBBaseSignalBar + 1;
        const int add = kBAddSignalBar + 1;
        const int close_add = kBAddCloseSignalBar + 1;
        const int close_base = kBBaseCloseSignalBar + 1;
        return {
            {"B", 0.0, 0.02, "pending_entry_reached",
             "base_entry_bar", base, "base_entry_price", bars[base].open},
            {"Add", 0.0, 0.02, "separate_market_add_reached",
             "separate_market_add_bar", add, "separate_market_add_price", bars[add].open},
            {"__close__Add", 0.02, 0.0, "market_add_close_reached",
             "market_add_close_bar", close_add, "market_add_close_price", bars[close_add].open},
            {"__close__B", 0.02, 0.0, "base_close_reached",
             "base_close_bar", close_base, "base_close_price", bars[close_base].open},
        };
    }
    return {
        {"Long", 0.0, 0.02, "pending_entry_reached",
         "entry_fill_bar", kCaseASignalBar, "entry_fill_price", kEntryPrice,
         "entry_fill_phase", NativePathPhase::Close},
        {"Half", 0.01, 0.0, "partial_exit_reached",
         "partial_exit_bar", 1, "partial_exit_price", kHalfPrice},
        {"Rest", 0.01, 0.0, "remainder_exit_reached",
         "remainder_exit_bar", 2, "remainder_exit_price", kRestPrice},
    };
}

void require_fill_shape(Comparison& r, const NamedFill& fill, const FillRule& rule) {
    r.one(fill.count, "named applied fill count");
    if (!fill.only) return;
    r.require(near(fill.only->opened, rule.opened), "named fill opened quantity",
              fill.only->opened, rule.opened);
    r.require(near(fill.only->closed, rule.closed), "named fill closed quantity",
              fill.only->closed, rule.closed);
}

void compare_field(Comparison& r, const Applied& fill, const FillRule& rule,
                   Field field, int last_bar) {
    const char* why = field_reason(field);
    switch (field) {
        case Field::Bar:
            r.require(fill.bar >= 0 && fill.bar <= last_bar, "fill bar outside observation",
                      fill.bar, rule.bar);
            r.expect(fill.bar == rule.bar, why, fill.bar, rule.bar);
            return;
        case Field::Price:
            r.require(std::isfinite(fill.price), "fill price unavailable", fill.price, rule.price);
            r.expect(near(fill.price, rule.price), why, fill.price, rule.price);
            return;
        case Field::Phase:
            const auto actual = static_cast<int>(fill.phase);
            const auto expected = static_cast<int>(rule.phase);
            r.require(fill.phase >= NativePathPhase::Open && fill.phase <= NativePathPhase::Close,
                      "fill phase unavailable", actual, expected);
            r.expect(fill.phase == rule.phase, why, actual, expected);
            return;
    }
}

Comparison authored_coordinate(const Observed& seen, Mode mode, int last_bar,
                               const FillRule& rule, Field field) {
    const char* name = field == Field::Bar ? rule.bar_check
        : (field == Field::Price ? rule.price_check : rule.phase_check);
    Comparison r{name, true};
    require_run(r, seen, mode, last_bar);
    const auto fill = named_fill(seen, rule.label);
    require_fill_shape(r, fill, rule);
    if (fill.only) compare_field(r, *fill.only, rule, field, last_bar);
    return r;
}

bool check_authored_fills(Probe& probe, const Observed& seen, Mode mode,
                          const std::vector<Bar>& bars) {
    const char* name = mode_name(mode);
    const int last_bar = static_cast<int>(bars.size()) - 1;
    const auto rules = authored_fills(mode, bars);
    const bool case_a = mode != Mode::BMarketAddControl;
    bool ok = true;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        const auto fill = named_fill(seen, rule.label);
        Comparison reached{rule.reached, true};
        require_run(reached, seen, mode, last_bar);
        reached.expect(fill.count == 1, "named applied fill count", fill.count, 1);
        if (fill.only) {
            reached.expect(near(fill.only->opened, rule.opened), "opened quantity",
                           fill.only->opened, rule.opened);
            reached.expect(near(fill.only->closed, rule.closed), "closed quantity",
                           fill.only->closed, rule.closed);
            if (reached.ok) {
                reached.reason = "named fill observed";
                reached.actual = fill.only->bar;
                reached.expected = rule.bar;
            }
        }
        ok = report_comparison(reached, name) && ok;
        ok = report_comparison(authored_coordinate(seen, mode, last_bar, rule, Field::Bar),
                               name) && ok;
        ok = report_comparison(authored_coordinate(seen, mode, last_bar, rule, Field::Price),
                               name) && ok;
        if (rule.phase_check)
            ok = report_comparison(authored_coordinate(seen, mode, last_bar, rule, Field::Phase),
                                   name) && ok;
        if (case_a) {
            constexpr const char* positions[] = {"position_after_entry",
                "position_after_partial_exit", "position_after_remainder_exit"};
            constexpr double expected[] = {0.02, 0.01, 0.0};
            Comparison position{positions[i], true};
            require_run(position, seen, mode, last_bar);
            require_fill_shape(position, fill, rule);
            if (fill.only) position.expect(near(fill.only->position_after, expected[i]),
                                            "held position", fill.only->position_after, expected[i]);
            ok = report_comparison(position, name) && ok;
        }
    }
    if (!case_a) return ok;
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
    ok = check(probe.b_relative_shadow_seen()
               && probe.b_relative_shadow_rank() == 0, name,
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
// and R5 in words. The adapter functions cited below are in
// src/source/pine_adapter.cpp.
//
//   R1  Without process_orders_on_close a market order fills at the next
//       bar's open.
//       - docs/pine-adapter-kernel-notes.md:237: a market fill at the next
//         bar is quoted at that bar's open; :239: under
//         process_orders_on_close, at the signal close.
//       - docs/pages/native-engine.md, the NextEligiblePoint example:
//         acceptance has no fill yet; the next bar's modeled open is the fill.
//       - project selects NextEligiblePoint without process_orders_on_close;
//         unified_fill_admits describes the market fill quote as the next open.
//   R2  A level given in ticks is the entry's fill price moved by that many
//       ticks.
//       - docs/pages/pine-to-native.md:343-345: at the parent's fill the
//         level becomes the fill plus the offset, in ticks.
//       - materialize_relative_exits sets the long entry's limit to the fill
//         price plus the profit ticks, rounded up to the tick grid.
//   R3  A limit that a crossing reaches fills at its level, and a sell stop
//       at the lower of the bar's open and its level.
//       - docs/pages/native-engine.md, "A limit is two facts": the path has
//         to reach the level, and a crossing books the level itself.
//       - docs/pine-adapter-kernel-notes.md:238: a stop is quoted at
//         min(open, stop) for a sell.
//       - unified_fill_admits describes the sell stop quote as min(open, stop);
//         pending_veto_point_first describes a touched level filling at its
//         level. The kernel's matcher prices the limit.
//   R4  Exits reserve the entry's quantity in the order they were placed,
//       and a later full exit is left what the earlier ones have not
//       reserved.
//       - CHANGELOG.md (1.0.0, "The `strategy.exit` orders of one entry queue
//         in the order they were created"): a later exit gets only what the
//         earlier ones leave.
//       - CHANGELOG.md (unreleased, "When a partial percentage exit waits for
//         a named market entry's fill"): on a three-contract entry two
//         waiting 50% exits reserve one contract each, leaving one for a
//         later full stop.
//       - docs/pages/quantity-intent.md:35-37: a full request clipped behind
//         another reservation holds less than its basis.
//       - exit_queue_rank retains an exit's place; compute_exit_reservation
//         caps a new exit at the unreserved quantity. At the entry's fill,
//         reconcile_deferred_exit_reservations allocates units in queue order.
//         exit_with_reservation_sequence gives a waiting partial its place
//         before sizing a later exit for the same entry.
//   R5  An exit placed while its entry was unfilled holds a fixed share of
//       what the entry opens, whichever exit fills first.
//       - docs/pine-adapter-kernel-notes.md:963-971: when the entry opens,
//         each such exit is bound to a fixed share of the opened lot, so
//         what it closes does not depend on which exit fills first.
//       - docs/pages/quantity-intent.md:22-23: a deferred exit acquires its
//         reservation when a live owner is bound.
//       - CHANGELOG.md (the same unreleased entry): the partial exit is sized
//         from the quantity the entry actually opens.
//       - materialize_relative_exits sizes the waiting partial from the units
//         the entry actually opened. reconcile_deferred_exit_reservations
//         records each share on its orders; resolve_source_terms executes
//         the fixed reservation without sizing it again.
//   R6  A closing order closes no more than is held and never opens a
//       position.
//       - docs/pages/fill-model.md:91-92: a reduction is capped at the held
//         exposure and never opens or flips; :214: it never creates new
//         exposure.
//       - reconcile_deferred_exit_reservations caps an exit at what is held
//         less earlier reservations, with zero as the lower bound.
//   R7  An order that has filled is not filled again.
//       - docs/pages/fill-model.md:176: a retired object is not dispatched
//         again.
//       - on_applied removes a waiting reissue when its exit has filled on
//         that opening, so it cannot fill there again.
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
    // closed twice.  The first is said of fills that close something, so it
    // is credited only when one was seen: a stream with the entry alone has
    // no fill to hold against what was held.
    double opened = 0.0;
    double closed = 0.0;
    int closing_fills = 0;
    bool within_held = true;
    for (const auto& row : probe.applied()) {
        opened += row.opened;
        closed += row.closed;
        if (row.closed > 0.0) ++closing_fills;
        if (closed > opened + 1e-9 || row.position_after < -1e-9)
            within_held = false;
    }
    ok = check(opened > 0.0 && closing_fills > 0 && within_held, name,
               "no_fill_exceeds_held_quantity") && ok;
    ok = check(opened > 0.0 && near(closed, opened), name,
               "closed_equals_opened", closed, opened) && ok;
    ok = check(opened > 0.0
               && near(probe.physical_position().signed_units, 0.0), name,
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
    if (is_live_step_mode(mode)) {
        // Without process_orders_on_close the entry fills at bar 1's open,
        // which is higher than bar 0's close.  Bar 1 does nothing else.
        // Bars 2 and 3 each reach one exit level and travel past it.  Bar 4
        // is the end close's bar, and bar 5 is where that close would fill.
        static_assert(kLiveEndCloseBar == 4, "bar 4 is the end close's bar");
        const double base = kStepBasePrice;
        const double later = base + kStepGap;
        const double above = later + kStepProfitTicks * kMinTick + kStepOvershoot;
        const double below = kStepStopPrice - kStepOvershoot;
        const bool stop_first = step_stop_first(mode);
        return {
            bar(0, base, base, base, base),
            bar(1, later, later, later, later),
            stop_first ? bar(2, later, later, below, later)
                       : bar(2, later, above, later, later),
            stop_first ? bar(3, later, above, later, later)
                       : bar(3, later, later, below, later),
            bar(4, later, later, later, later),
            bar(5, later, later, later, later),
        };
    }
    if (is_step_mode(mode)) {
        // Bar 0 places the three orders and closes at the base price, where
        // the entry fills.  Later bars open a little higher, so that price
        // tells a fill at the signal close from one at the next open.
        const double base = kStepBasePrice;
        const double later = base + kStepGap;
        const double above = base + kStepProfitTicks * kMinTick + kStepOvershoot;
        const double below = kStepStopPrice - kStepOvershoot;
        const bool stop_first = step_stop_first(mode);
        return {
            bar(0, base, base, base, base),
            stop_first ? bar(1, later, later, below, later)
                       : bar(1, later, above, later, later),
            stop_first ? bar(2, later, above, later, later)
                       : bar(2, later, later, below, later),
            bar(3, later, later, later, later),
            bar(4, later, later, later, later),
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

// The step rows.  The exact row's three kinds of order under
// process_orders_on_close, in whole steps of the quantity step.  The rules are
// R2 to R7 of check_held_entry, and two more:
//
//   R1' Under process_orders_on_close a market order fills at the close of
//       the bar that placed it: docs/pine-adapter-kernel-notes.md:239.
//   R8  On a quantity step a share below the whole is floored onto the step:
//       docs/pages/pine-to-native.md:291 says it of a fractional close, and
//       quantize_percent_exit_units in src/source/pine_adapter.cpp floors
//       the requested share, keeping one step for a smaller positive share
//       when a whole step is available.
//
// A 50% partial exit of three steps has a share of one and a half steps.  By
// R8 it closes one, and by R4 and R5 the full exit closes what that leaves,
// two, so after both exits nothing is held (R6).  The script then makes an
// end close, which must find nothing to take.
//
// The live-entry rows place the same orders without process_orders_on_close
// and with a commission.  Their entry is a working order when the exits are
// placed and fills at the next bar's open (R1). In either POOC stop-first
// row the sole stop crossing is on the bar immediately after the entry.
//
// The applied position after the second exit, the position seen by the
// end-close calculation, and the final physical position are separate
// observations. An end-close fill can flatten the final position.
struct StepCase {
    double entry_qty = 0.0;
    bool stop_first = false;
    // Under process_orders_on_close, or with a live entry and without it.
    bool on_close = true;
};

StepCase step_case(Mode mode) {
    return {step_entry_qty(mode), step_stop_first(mode), is_pooc_step_mode(mode)};
}

// The bar on which a step row's script places its three orders (on_step_bar),
// and the bar on which its entry then fills: the signal bar under
// process_orders_on_close, at that bar's close (R1'), and the bar after it
// without, at that bar's open (R1).
constexpr int kStepSignalBar = 0;

int step_entry_bar(const StepCase& step) {
    return step.on_close ? kStepSignalBar : kStepSignalBar + 1;
}

FillRule live_entry_rule(Mode mode) {
    const auto step = step_case(mode);
    return {"Long", 0.0, step.entry_qty, nullptr, nullptr,
            step_entry_bar(step), nullptr, kStepBasePrice + kStepGap,
            nullptr, NativePathPhase::Open};
}

Comparison live_entry_at_next_open(const Observed& seen, Mode mode) {
    const auto rule = live_entry_rule(mode);
    Comparison r{"live_entry_filled_at_next_open", true};
    r.require(!seen.applied.empty(), "minimum applied-fill count", seen.applied.size(), 1);
    require_run(r, seen, mode, step_end_close_bar(mode) + 1);
    const auto fill = named_fill(seen, rule.label);
    require_fill_shape(r, fill, rule);
    r.expect(seen.entry_order_at_signal, "working entry at signal",
             seen.entry_order_at_signal, true);
    if (fill.only) {
        const int last_bar = step_end_close_bar(mode) + 1;
        // Check every field's availability even if another field disagrees.
        for (const Field field : {Field::Bar, Field::Price, Field::Phase})
            compare_field(r, *fill.only, rule, field, last_bar);
    }
    return r;
}

std::size_t closing_fill_count(const Observed& seen) {
    std::size_t count = 0;
    for (const auto& fill : seen.applied)
        if (fill.closed > 0.0) ++count;
    return count;
}

Comparison end_close_check(const Observed& seen, Mode mode) {
    Comparison r{"nothing_left_for_end_close", true};
    require_completed(r, seen, mode);
    if (is_pooc_step_mode(mode) && step_stop_first(mode)) {
        r.one(seen.stop_first_paths, "N+1 stop-only crossing count");
    }
    const auto entry = named_fill(seen, "Long");
    const auto half = named_fill(seen, "Half");
    const auto rest = named_fill(seen, "Rest");
    const int end_bar = step_end_close_bar(mode);
    r.one(entry.count, "Long fill count");
    r.one(half.count, "Half fill count");
    r.one(rest.count, "Rest fill count");
    if (entry.only) {
        r.require(entry.only->opened > 0.0, "Long opening quantity",
                  entry.only->opened, step_entry_qty(mode));
        r.require(near(entry.only->closed, 0.0), "Long closed units",
                  entry.only->closed, 0.0);
    }
    for (const auto* exit : {half.only, rest.only}) {
        if (exit) {
            r.require(exit->closed > 0.0, "named exit closed units", exit->closed, kQtyStep);
            r.require(near(exit->opened, 0.0), "named exit opened units", exit->opened, 0.0);
            r.require(exit->bar >= 0 && exit->bar < end_bar,
                      "named exit before end-close action", exit->bar, end_bar - 1);
        }
    }
    r.one(seen.end_close.visits, "end-close action visits");
    r.require(seen.end_close.bar == end_bar, "end-close action bar",
              seen.end_close.bar, end_bar);
    r.require(seen.last_source_bar == end_bar + 1, "last source bar",
              seen.last_source_bar, end_bar + 1);
    r.require(std::isfinite(seen.end_close.position), "end-close position unavailable",
              seen.end_close.position, 0.0);
    r.require(std::isfinite(seen.final_position), "final position unavailable",
              seen.final_position, 0.0);
    const auto closing = closing_fill_count(seen);
    r.expect(closing == 2, "closing fill count", closing, 2);
    r.expect(near(seen.end_close.position, 0.0), "position at end-close action",
             seen.end_close.position, 0.0);
    r.expect(near(seen.final_position, 0.0), "final position",
             seen.final_position, 0.0);
    return r;
}

void report_end_close(const Observed& seen, Mode row, const char* input) {
    const int end_bar = step_end_close_bar(row);
    std::printf("END_CLOSE mode=%s name=nothing_left_for_end_close input=%s "
                "last_bar=%d expected_last_bar=%d action_bar=%d expected_action_bar=%d "
                "visits=%d expected_visits=1 position_at_action=%.17g "
                "expected_position_at_action=0 final_position=%.17g expected_final_position=0 "
                "closing_fills=%zu expected_closing_fills=2\n", mode_name(row), input,
                seen.last_source_bar, end_bar + 1, seen.end_close.bar, end_bar,
                seen.end_close.visits, seen.end_close.position, seen.final_position,
                closing_fill_count(seen));
}

Comparison stop_first_alignment(const Observed& seen, const StepCase& step) {
    Comparison aligned{"aligned_signal_and_first_stop_path", true};
    const bool completed = seen.error.empty() && seen.lifecycle == NativeLifecycleKind::Completed;
    aligned.require(completed, "completed run", completed, true);
    aligned.require(seen.end_close.visits == 1, "end-close calculation count",
                    seen.end_close.visits, 1);
    const Applied* entry = nullptr;
    std::size_t entries = 0;
    for (const auto& fill : seen.applied) {
        if (fill.label == "Long" && fill.opened > 0.0) {
            entry = &fill;
            ++entries;
        }
    }
    aligned.one(entries, "Long fill count");
    if (entry) {
        aligned.require(entry->bar == step_entry_bar(step), "signal bar",
                        entry->bar, step_entry_bar(step));
        aligned.require(entry->after_calculation_close, "POOC close point",
                        entry->after_calculation_close, true);
        aligned.require(near(entry->opened, step.entry_qty), "row entry quantity",
                        entry->opened, step.entry_qty);
        aligned.require(near(entry->price, kStepBasePrice), "signal close price",
                        entry->price, kStepBasePrice);
    }
    aligned.require(seen.stop_first_paths == 1, "N+1 stop-only crossing count",
                    seen.stop_first_paths, 1);
    if (!aligned.missing) {
        aligned.actual = seen.stop_first_paths;
        aligned.expected = 1;
    }
    return aligned;
}

std::vector<Comparison> step_checks(const Observed& seen, Mode mode) {
    const auto step = step_case(mode);
    std::vector<const Applied*> entries;
    std::vector<const Applied*> partials;
    std::vector<const Applied*> fulls;
    for (const auto& row : seen.applied) {
        if (row.opened > 0.0 && row.label == "Long") entries.push_back(&row);
        if (!(row.closed > 0.0)) continue;
        if (row.label == "Half") partials.push_back(&row);
        if (row.label == "Rest") fulls.push_back(&row);
    }
    // A fill is read only when it is the one fill of its order.  With none,
    // or with a second one, every check that needs it fails; none is skipped.
    const Applied* entry = entries.size() == 1 ? entries.front() : nullptr;
    const Applied* partial = partials.size() == 1 ? partials.front() : nullptr;
    const Applied* full = fulls.size() == 1 ? fulls.front() : nullptr;
    const bool all = entry != nullptr && partial != nullptr && full != nullptr;
    // The two exits in the order they filled.
    const Applied* first = nullptr;
    const Applied* second = nullptr;
    if (partial != nullptr && full != nullptr) {
        first = partial < full ? partial : full;
        second = partial < full ? full : partial;
    }
    const double share = entry != nullptr
        ? entry->opened * kStepPartialPercent / 100.0 : kNaN;
    const double floored = entry != nullptr
        ? std::floor(share / kQtyStep + 1e-9) * kQtyStep : kNaN;      // R8
    const double level = entry != nullptr
        ? entry->price + kStepProfitTicks * kMinTick : kNaN;           // R2
    const double rest = entry != nullptr && partial != nullptr
        ? entry->opened - partial->closed : kNaN;                      // R4
    // The bar with the first crossing in this row's feed (bars_for).
    const int first_bar = step_entry_bar(step) + 1;
    const int partial_bar = step.stop_first ? first_bar + 1 : first_bar;
    const int full_bar = step.stop_first ? first_bar : first_bar + 1;

    std::vector<Comparison> out;
    const auto require_events = [&](Comparison& c) {
        c.require(!seen.applied.empty(), "minimum applied-fill count", seen.applied.size(), 1);
    };
    if (step.on_close) {
        const auto at_signal_close = [&](const Applied* fill) {
            Comparison r{"entry_filled_at_signal_close",
                         fill && near(fill->price, kStepBasePrice)
                             && near(fill->opened, step.entry_qty),
                         fill ? fill->opened : kNaN, step.entry_qty};
            require_events(r);
            r.one(entries.size(), "Long fill count");
            require_run(r, seen, mode, step_end_close_bar(mode) + 1);
            if (fill) {
                r.require(fill->bar >= 0 && fill->bar <= seen.last_source_bar,
                          "fill bar outside observation", fill->bar, kStepSignalBar);
                const auto phase = static_cast<int>(fill->phase);
                const auto close = static_cast<int>(NativePathPhase::Close);
                r.require(fill->phase >= NativePathPhase::Open && fill->phase <= NativePathPhase::Close,
                          "fill phase unavailable", phase, close);
                r.expect(fill->bar == kStepSignalBar, "fill bar", fill->bar, kStepSignalBar);
                r.expect(phase == close, "fill phase", phase, close);
                r.expect(fill->after_calculation_close, "after-calculation close",
                         fill->after_calculation_close, true);
            }
            return r;
        };
        const auto positive = at_signal_close(entry);
        out.push_back(positive);
        if (!step.stop_first) {
            const auto reject_copy = [&](const char* name, const char* reason, auto change) {
                Comparison control = positive;
                control.name = name;
                control.require(positive.ok && entry && entry->bar == kStepSignalBar
                                    && entry->phase == NativePathPhase::Close
                                    && entry->after_calculation_close,
                                "real signal-close entry required", 0, 1);
                if (!control.missing) {
                    Applied copy = *entry;
                    change(copy);
                    const auto result = at_signal_close(&copy);
                    const bool rejected = !result.ok && !result.missing
                        && std::string_view(result.reason) == reason;
                    control.expect(rejected, "copy rejected for changed field", rejected, true);
                }
                out.push_back(control);
            };
            reject_copy("reject_open_entry_phase", "fill phase",
                        [](Applied& fill) { fill.phase = NativePathPhase::Open; });
            reject_copy("reject_late_entry_bar", "fill bar",
                        [](Applied& fill) { ++fill.bar; });
            reject_copy("reject_entry_without_close_provenance", "after-calculation close",
                        [](Applied& fill) { fill.after_calculation_close = false; });
        }
    } else {
        out.push_back(live_entry_at_next_open(seen, mode));
    }
    // R7: each exit fills once.
    out.push_back({"partial_exit_filled_once", partial != nullptr,
                   static_cast<double>(partials.size()), 1.0});
    require_events(out.back());
    out.push_back({"full_exit_filled_once", full != nullptr,
                   static_cast<double>(fulls.size()), 1.0});
    require_events(out.back());
    // R5 and R8: the partial exit closes its share of what the entry opened,
    // floored onto the quantity step, and that is more than nothing.
    out.push_back({"partial_exit_floored_to_step",
                   entry != nullptr && partial != nullptr && floored > 0.0
                       && near(partial->closed, floored),
                   partial != nullptr ? partial->closed : kNaN, floored});
    const auto require_partial = [&](Comparison& c) {
        c.one(entries.size(), "Long fill count");
        c.one(partials.size(), "Half fill count");
    };
    const auto require_all = [&](Comparison& c) {
        require_partial(c);
        c.one(fulls.size(), "Rest fill count");
    };
    require_partial(out.back());
    // R2 and R3: it fills at the entry's fill price moved by its ticks, on
    // the bar that reaches that level.
    out.push_back({"partial_exit_at_its_level",
                   entry != nullptr && partial != nullptr
                       && partial->bar == partial_bar && near(partial->price, level),
                   partial != nullptr ? partial->price : kNaN, level});
    require_partial(out.back());
    // R3: the full exit fills at its stop, on the bar that reaches it.
    out.push_back({"full_exit_at_its_stop",
                   full != nullptr && full->bar == full_bar
                       && near(full->price, kStepStopPrice),
                   full != nullptr ? full->price : kNaN, kStepStopPrice});
    out.back().one(fulls.size(), "Rest fill count");
    // R4 and R5: the full exit closes exactly what the partial exit's own
    // fill leaves of the entry.
    out.push_back({"full_exit_takes_what_partial_leaves",
                   all && near(full->closed, rest),
                   full != nullptr ? full->closed : kNaN, rest});
    require_all(out.back());
    // R6: after the second of the two exits nothing is held.  The position is
    // the one that exit's own fill reports.
    out.push_back({"flat_after_both_exits",
                   all && near(first->position_after, entry->opened - first->closed)
                       && near(second->position_after, 0.0),
                   second != nullptr ? second->position_after : kNaN, 0.0});
    require_all(out.back());
    out.push_back(end_close_check(seen, mode));
    // The closed trades say the same: one row for each exit, in fill order.
    bool trades_match = all && seen.trades.size() == 2;
    if (trades_match) {
        trades_match = seen.trades[0].exit_id == first->label
            && near(seen.trades[0].size, first->closed)
            && seen.trades[1].exit_id == second->label
            && near(seen.trades[1].size, second->closed);
    }
    out.push_back({"closed_trades_are_the_two_exits", trades_match,
                   static_cast<double>(seen.trades.size()), 2.0});
    auto& ledger = out.back();
    require_all(ledger);
    ledger.require(seen.trades.size() == 2, "closed-trade count for pairing",
                   seen.trades.size(), 2);
    if (!ledger.missing && (seen.trades[0].exit_id != first->label
                            || seen.trades[1].exit_id != second->label)) {
        ledger.missing = "trade/fill id alignment";
        ledger.actual_ids = seen.trades[0].exit_id + "," + seen.trades[1].exit_id;
        ledger.expected_ids = first->label + "," + second->label;
    }
    if (!seen.applied.empty())
        for (auto& r : out) require_completed(r, seen, mode);
    if (step.on_close && step.stop_first) {
        const auto aligned = stop_first_alignment(seen, step);
        if (aligned.missing) {
            for (auto& c : out) {
                const auto name = c.name;
                c = aligned;
                c.name = name;
            }
        }
        out.insert(out.begin(), aligned);
    }
    return out;
}

bool report_step_checks(const std::vector<Comparison>& results, const char* mode,
                        const char* input = "ROW") {
    bool ok = true;
    for (const auto& result : results) ok = report_comparison(result, mode, input) && ok;
    return ok;
}

Observed observe_run(Mode mode, const std::vector<Bar>& bars, int count) {
    Probe probe(mode);
    probe.fixture_retain_all_events();
    probe.run(bars.data(), count, "1", "1");
    return observe(probe);
}

bool control_verdict(const char* selector, Mode row, const char* input,
                     const Comparison& raw, bool attributable) {
    std::printf("CONTROL selector=%s mode=%s name=%s input=%s status=%s\n",
                selector, mode_name(row), raw.name, input, attributable ? "PASS" : "FAIL");
    return attributable;
}

bool expect_rejection(const char* selector, Mode row, const char* input,
                      const Comparison& raw, const char* reason, bool unavailable,
                      bool source_verified = true) {
    report_comparison(raw, mode_name(row), input);
    const bool rejected = unavailable
        ? raw.missing && std::string_view(raw.missing) == reason
        : !raw.missing && !raw.ok && std::string_view(raw.reason) == reason;
    return control_verdict(selector, row, input, raw, source_verified && rejected);
}

void report_fill_observation(const Observed& seen, Mode row, const FillRule& rule,
                             const char* input, int last_bar) {
    const auto fill = named_fill(seen, rule.label);
    std::printf("FILL_OBSERVATION mode=%s input=%s label=%s count=%zu expected_count=1 "
                "last_bar=%d expected_last_bar=%d", mode_name(row), input, rule.label,
                fill.count, seen.last_source_bar, last_bar);
    if (fill.only) {
        std::printf(" bar=%d expected_bar=%d price=%.17g expected_price=%.17g "
                    "phase=%s expected_phase=%s opened=%.17g expected_opened=%.17g "
                    "closed=%.17g expected_closed=%.17g", fill.only->bar, rule.bar,
                    fill.only->price, rule.price, phase_name(fill.only->phase),
                    rule.phase == NativePathPhase::None ? "not checked" : phase_name(rule.phase),
                    fill.only->opened, rule.opened, fill.only->closed, rule.closed);
    }
    std::printf("\n");
}

bool verify_prefix(const Observed& seen, Mode mode, int last_bar, const char* input) {
    Comparison r{"prefix_source", true, static_cast<double>(seen.last_source_bar),
                 static_cast<double>(last_bar)};
    require_run(r, seen, mode, last_bar);
    return report_comparison(r, mode_name(mode), input);
}

template <typename Predicate>
bool run_coordinate_control(const char* selector, Mode mode, const std::vector<Bar>& bars,
                            const FillRule& rule, Field field, bool live, Predicate predicate) {
    const int last_bar = static_cast<int>(bars.size()) - 1;
    const Observed positive = observe_run(mode, bars, last_bar + 1);
    report_fill_observation(positive, mode, rule, "POSITIVE", last_bar);
    const auto raw_positive = predicate(positive);
    const auto fill = named_fill(positive, rule.label);
    const bool positive_ok = report_comparison(raw_positive, mode_name(mode), "POSITIVE");
    if (!control_verdict(selector, mode, "POSITIVE", raw_positive, positive_ok && fill.only))
        return false;

    // For F3, confirm the other authored coordinates of this same fill. No
    // quantity failure or different order can be mistaken for the target.
    if (!live) {
        for (const auto sibling : {Field::Bar, Field::Price, Field::Phase}) {
            if (sibling == field || (sibling == Field::Phase && !rule.phase_check)) continue;
            if (!report_comparison(authored_coordinate(positive, mode, last_bar, rule, sibling),
                                   mode_name(mode), "POSITIVE_COMPANION"))
                return control_verdict(selector, mode, "POSITIVE_COMPANION", raw_positive, false);
        }
    }

    Observed wrong = positive;
    const auto index = fill.only - positive.applied.data();
    auto& changed = wrong.applied[index];
    switch (field) {
        case Field::Bar: changed.bar += changed.bar < last_bar ? 1 : -1; break;
        case Field::Price: changed.price += kMinTick; break;
        case Field::Phase:
            changed.phase = changed.phase == NativePathPhase::Open
                ? NativePathPhase::Close : NativePathPhase::Open;
            break;
    }
    report_fill_observation(wrong, mode, rule, "NEGATIVE", last_bar);
    if (!expect_rejection(selector, mode, "NEGATIVE", predicate(wrong),
                          field_reason(field), false))
        return false;

    if (field == Field::Phase) {
        Observed no_phase = positive;
        no_phase.applied[index].phase = NativePathPhase::None;
        if (!expect_rejection(selector, mode, "MISSING_PHASE", predicate(no_phase),
                              "fill phase unavailable", true))
            return false;
    }

    const Observed empty;
    report_fill_observation(empty, mode, rule, "EMPTY", last_bar);
    if (!expect_rejection(selector, mode, "EMPTY", predicate(empty),
                          live ? "minimum applied-fill count" : "observation source row", true))
        return false;

    // Real prefix of this feed, missing its last bar. It may have the right
    // fill, but it does not supply this row's completed input horizon.
    const Observed stale = observe_run(mode, bars, last_bar);
    const bool prefix_ok = verify_prefix(stale, mode, last_bar - 1, "STALE");
    report_fill_observation(stale, mode, rule, "STALE", last_bar);
    return expect_rejection(selector, mode, "STALE", predicate(stale), "last source bar",
                            true, prefix_ok);
}

bool run_g2_control(const char* selector, bool final_position_only) {
    const Mode row = Mode::PoocWholeStepsStopFirst;
    const auto bars = bars_for(row);
    const int last = static_cast<int>(bars.size()) - 1;
    const int end = step_end_close_bar(row);
    const Observed positive = observe_run(row, bars, last + 1);
    report_end_close(positive, row, "POSITIVE");
    const auto raw_positive = end_close_check(positive, row);
    const bool positive_ok = report_comparison(raw_positive, mode_name(row), "POSITIVE");
    if (!control_verdict(selector, row, "POSITIVE", raw_positive, positive_ok))
        return false;

    if (final_position_only) {
        Observed nonflat = positive;
        // Reuse observed held units from the opening, while retaining the
        // action, both named exits, count and horizon of the real positive.
        nonflat.final_position = named_fill(positive, "Long").only->opened;
        report_end_close(nonflat, row, "NEGATIVE");
        if (!expect_rejection(selector, row, "NEGATIVE", end_close_check(nonflat, row),
                              "final position", false))
            return false;
    }

    const Observed empty;
    report_end_close(empty, row, "EMPTY");
    if (!expect_rejection(selector, row, "EMPTY", end_close_check(empty, row),
                          "observation source row", true))
        return false;

    // With the A7 feed, both exits are due before this prefix stops.
    // Credit staleness only if it saw the stop-only crossing, agrees
    // with the real positive about the fills, and is already flat.
    const Observed stale = observe_run(row, bars, end);
    const bool prefix_ok = verify_prefix(stale, row, end - 1, "STALE");
    Comparison snapshot{"two_exits_before_end_close", true};
    snapshot.expect(stale.stop_first_paths == 1, "N+1 stop-only crossing count",
                    stale.stop_first_paths, 1);
    snapshot.expect(closing_fill_count(stale) == 2, "closing fill count",
                    closing_fill_count(stale), 2);
    snapshot.expect(near(stale.final_position, positive.final_position), "prefix final position",
                    stale.final_position, positive.final_position);
    snapshot.expect(stale.end_close.visits == 0, "end-close action visits",
                    stale.end_close.visits, 0);
    for (const char* label : {"Long", "Half", "Rest"}) {
        const auto before = named_fill(stale, label);
        const auto after = named_fill(positive, label);
        Comparison identity{label, true};
        identity.one(before.count, "prefix named fill count");
        if (before.only && after.only) {
            identity.expect(before.only->bar == after.only->bar, "prefix fill bar",
                            before.only->bar, after.only->bar);
            identity.expect(near(before.only->opened, after.only->opened), "prefix opened units",
                            before.only->opened, after.only->opened);
            identity.expect(near(before.only->closed, after.only->closed), "prefix closed units",
                            before.only->closed, after.only->closed);
            identity.expect(near(before.only->price, after.only->price), "prefix price",
                            before.only->price, after.only->price);
        }
        if (!report_comparison(identity, mode_name(row), "STALE")) snapshot.ok = false;
    }
    const bool snapshot_ok = report_comparison(snapshot, mode_name(row), "STALE");
    report_end_close(stale, row, "STALE");
    return expect_rejection(selector, row, "STALE", end_close_check(stale, row),
                            "end-close action visits", true, prefix_ok && snapshot_ok);
}

bool retained_step_control(const char* selector, Mode row, Mode other, bool stale) {
    const auto bars = bars_for(row);
    const int last = static_cast<int>(bars.size()) - 1;
    const Observed positive = observe_run(row, bars, last + 1);
    const auto real = step_checks(positive, row);
    report_end_close(positive, row, "POSITIVE");
    const bool real_ok = report_step_checks(real, mode_name(row), "POSITIVE");
    Comparison source{"checks_were_run", true};
    require_run(source, positive, row, last);
    source.expect(!real.empty(), "comparison count", real.size(), 1);
    const bool source_ok = report_comparison(source, mode_name(row), "POSITIVE");
    if (!control_verdict(selector, row, "POSITIVE", source, real_ok && source_ok)) {
        for (const auto& result : real)
            std::printf("CONTROL selector=%s mode=%s name=%s input=%s status=SKIP "
                        "reason=own positive did not pass\n",
                        selector, mode_name(row), result.name, stale ? "STALE" : "EMPTY");
        return false;
    }

    Observed input;
    bool input_ok = true;
    if (stale) {
        const int first = step_entry_bar(step_case(row));
        input = observe_run(row, bars, first + 1);
        const auto entry = named_fill(input, "Long");
        const auto reference = named_fill(positive, "Long");
        Comparison earlier{"earlier_snapshot_is_the_entry_fill", true};
        require_run(earlier, input, row, first);
        earlier.one(input.applied.size(), "prefix applied-fill count");
        earlier.one(entry.count, "Long fill count");
        earlier.expect(input.trades.empty(), "prefix trade count", input.trades.size(), 0);
        if (entry.only && reference.only) {
            earlier.expect(entry.only->bar == first, "entry bar", entry.only->bar, first);
            earlier.expect(near(entry.only->opened, reference.only->opened), "entry opened units",
                           entry.only->opened, reference.only->opened);
            earlier.expect(near(entry.only->closed, reference.only->closed), "entry closed units",
                           entry.only->closed, reference.only->closed);
            earlier.expect(near(entry.only->price, reference.only->price), "entry price",
                           entry.only->price, reference.only->price);
        }
        input_ok = report_comparison(earlier, mode_name(row), "STALE");
        if (!input_ok) return control_verdict(selector, row, "STALE", earlier, false);
        const auto other_bars = bars_for(other);
        const int other_last = static_cast<int>(other_bars.size()) - 1;
        const Observed leftover = observe_run(other, other_bars, other_last + 1);
        auto finished = end_close_check(leftover, other);
        finished.name = "leftover_run_is_finished";
        finished.expect(leftover.trades.size() == 2, "closed-trade count",
                        leftover.trades.size(), 2);
        if (!report_comparison(finished, mode_name(other), "STALE_OTHER"))
            return control_verdict(selector, row, "STALE_OTHER", finished, false);
        // Retain the other-row entry challenge, but do not credit it as
        // evidence about the right row's earlier snapshot.
        const auto wrong_row = step_checks(leftover, row);
        if (wrong_row.empty()) return control_verdict(selector, row, "STALE_OTHER", source, false);
        if (!expect_rejection(selector, row, "STALE_OTHER", wrong_row.front(),
                              "observation source row", true)) return false;
    }
    const char* kind = stale ? "STALE" : "EMPTY";
    const auto raw = step_checks(input, row);
    report_end_close(input, row, kind);
    if (raw.empty() || raw.size() != real.size())
        return control_verdict(selector, row, kind, source, false);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto& result = raw[i];
        report_comparison(result, mode_name(row), kind);
        const bool rejected = std::string_view(result.name) == real[i].name && !result.ok;
        if (!control_verdict(selector, row, kind, result, input_ok && rejected)) return false;
    }
    return true;
}

bool run_retained_step_controls(Mode mode) {
    const char* selector = mode_name(mode);
    const bool stale = mode == Mode::StepChecksStaleInput;
    if (!retained_step_control(selector, Mode::PoocOddStepsLimitFirst,
                               Mode::PoocWholeStepsLimitFirst, stale)) {
        std::printf("CONTROL selector=%s mode=%s input=%s status=SKIP "
                    "reason=previous control failed\n", selector,
                    mode_name(Mode::LiveEntryOddStepsLimitFirst), stale ? "STALE" : "EMPTY");
        return false;
    }
    return retained_step_control(selector, Mode::LiveEntryOddStepsLimitFirst,
                                 Mode::LiveEntryWholeStepsLimitFirst, stale);
}

bool run_step_checks_on_empty_input() {
    const char* name = mode_name(Mode::StepChecksEmptyInput);
    bool ok = run_retained_step_controls(Mode::StepChecksEmptyInput);
    for (const Mode row : {Mode::PoocOddStepsStopFirst, Mode::PoocWholeStepsStopFirst}) {
        const std::string prefix = std::string(mode_name(row)) + ":";
        const auto results = step_checks(Observed{}, row);
        ok = check(!results.empty(), name, (prefix + "checks_were_run").c_str()) && ok;
        for (const auto& result : results)
            ok = check(!result.ok && result.missing, name,
                       (prefix + result.name).c_str()) && ok;
    }
    return ok;
}

// A real same-size limit-first run has the same order ids and entry point,
// but offers no stop-only window on N+1. None of its results may be paired
// with the stop-first row. First authenticate the donor itself.
bool reject_other_crossing_order(const char* name, Mode row, Mode donor) {
    const auto bars = bars_for(donor);
    const auto seen = observe_run(donor, bars, static_cast<int>(bars.size()));
    const auto own = step_case(donor);
    const auto results = step_checks(seen, row);
    const double limit = kStepBasePrice + kStepProfitTicks * kMinTick;
    const Applied* entry = nullptr;
    int entries = 0, limits_first = 0, stops_later = 0;
    for (const auto& fill : seen.applied) {
        if (fill.label == "Long" && fill.opened > 0.0) { ++entries; entry = &fill; }
        if (fill.label == "Half" && fill.closed > 0.0 && fill.bar == step_entry_bar(own) + 1
            && near(fill.price, limit)) ++limits_first;
        if (fill.label == "Rest" && fill.closed > 0.0 && fill.bar == step_entry_bar(own) + 2
            && near(fill.price, kStepStopPrice)) ++stops_later;
    }
    const bool completed = seen.error.empty() && seen.lifecycle == NativeLifecycleKind::Completed;
    const bool donor_ready = completed && seen.end_close.visits == 1
        && seen.stop_first_paths == 0 && entries == 1 && entry->after_calculation_close
        && entry->bar == step_entry_bar(own) && near(entry->price, kStepBasePrice)
        && near(entry->opened, own.entry_qty) && limits_first == 1 && stops_later == 1;
    const std::string prefix = std::string(mode_name(row)) + ":other_touch_order:";
    if (!donor_ready) {
        for (const auto& r : results)
            std::printf("CHECK mode=%s name=%s%s status=NOT COMPARED reason=stale donor "
                        "actual=completed:%d,end:%d,first_stop_paths:%d,Long:%d,first_limit:%d,later_stop:%d "
                        "expected=completed:1,end:1,first_stop_paths:0,Long:1,first_limit:1,later_stop:1\n",
                        name, prefix.c_str(), r.name, completed, seen.end_close.visits,
                        seen.stop_first_paths, entries, limits_first, stops_later);
        return false;
    }
    bool ok = check(!results.empty(), name, (prefix + "checks_were_run").c_str());
    for (const auto& r : results)
        ok = check(!r.ok && r.missing && std::string_view(r.missing) == "N+1 stop-only crossing count",
                   name, (prefix + r.name).c_str(), r.actual, r.expected) && ok;
    return ok;
}

// The stale-input row, for both entry forms and the immediate-stop window.
bool run_step_checks_on_stale_input() {
    const char* name = mode_name(Mode::StepChecksStaleInput);
    bool ok = run_retained_step_controls(Mode::StepChecksStaleInput);
    ok = reject_other_crossing_order(name, Mode::PoocOddStepsStopFirst,
                                     Mode::PoocOddStepsLimitFirst) && ok;
    ok = reject_other_crossing_order(name, Mode::PoocWholeStepsStopFirst,
                                     Mode::PoocWholeStepsLimitFirst) && ok;
    return ok;
}

bool run_mode(Mode mode) {
    const char* name = mode_name(mode);
    if (mode == Mode::StepChecksEmptyInput) return run_step_checks_on_empty_input();
    if (mode == Mode::StepChecksStaleInput) return run_step_checks_on_stale_input();
    Probe probe(mode);
    const auto bars = bars_for(mode);
    probe.fixture_retain_all_events();
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    std::printf("EXECUTION mode=%s status=%s last_error=%s\n", name,
                probe.last_error().empty() ? "completed" : "failed",
                probe.last_error().c_str());
    print_applied(probe, name);
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
    if (is_step_mode(mode) && (!probe.last_error().empty()
            || probe.native_state().kind != NativeLifecycleKind::Completed)) {
        const auto results = step_checks(Observed{}, mode);
        for (const auto& r : results)
            std::printf("CHECK mode=%s name=%s status=NOT COMPARED reason=incomplete run "
                        "actual=state:%d,error:%s expected=state:%d,error:empty\n", name, r.name,
                        static_cast<int>(probe.native_state().kind), probe.last_error().c_str(),
                        static_cast<int>(NativeLifecycleKind::Completed));
        return false;
    }
    if (mode == Mode::Exact || mode == Mode::AbsoluteControl || mode == Mode::BMarketAddControl)
        return check_authored_fills(probe, observe(probe), mode, bars);
    if (!probe.last_error().empty()) return false;
    if (mode == Mode::BPendingEntryControl) return check_b_pending(probe, bars);
    if (mode == Mode::HeldEntryStopFirst || mode == Mode::HeldEntryLimitFirst)
        return check_held_entry(probe, mode, bars);
    if (is_step_mode(mode)) {
        const auto seen = observe(probe);
        const auto results = step_checks(seen, mode);
        report_end_close(seen, mode, "ROW");
        return report_step_checks(results, name);
    }
    return false;
}

bool parse_mode(std::string_view value, Mode& out) {
    if (value == "exact") out = Mode::Exact;
    else if (value == "a_absolute_control") out = Mode::AbsoluteControl;
    else if (value == "b_pending_entry_control") out = Mode::BPendingEntryControl;
    else if (value == "b_market_add_control") out = Mode::BMarketAddControl;
    else if (value == "held_entry_stop_first") out = Mode::HeldEntryStopFirst;
    else if (value == "held_entry_limit_first") out = Mode::HeldEntryLimitFirst;
    else if (value == "pooc_odd_steps_limit_first") out = Mode::PoocOddStepsLimitFirst;
    else if (value == "pooc_odd_steps_stop_first") out = Mode::PoocOddStepsStopFirst;
    else if (value == "pooc_whole_steps_limit_first")
        out = Mode::PoocWholeStepsLimitFirst;
    else if (value == "pooc_whole_steps_stop_first") out = Mode::PoocWholeStepsStopFirst;
    else if (value == "live_entry_odd_steps_limit_first")
        out = Mode::LiveEntryOddStepsLimitFirst;
    else if (value == "live_entry_odd_steps_stop_first")
        out = Mode::LiveEntryOddStepsStopFirst;
    else if (value == "live_entry_whole_steps_limit_first")
        out = Mode::LiveEntryWholeStepsLimitFirst;
    else if (value == "live_entry_whole_steps_stop_first")
        out = Mode::LiveEntryWholeStepsStopFirst;
    else if (value == "step_checks_empty_input") out = Mode::StepChecksEmptyInput;
    else if (value == "step_checks_stale_input") out = Mode::StepChecksStaleInput;
    else return false;
    return true;
}

std::optional<bool> run_guard_control(const char* selector) {
    std::string_view value(selector);
    if (value == "g2_end_close_stale") return run_g2_control(selector, false);
    if (value == "g2_end_close_final_position") return run_g2_control(selector, true);
    if (value.substr(0, 3) == "g3_" || value.substr(0, 3) == "g4_") {
        Mode row;
        if (!parse_mode(value.substr(3), row) || !is_live_step_mode(row)) return std::nullopt;
        const auto field = value[1] == '3' ? Field::Bar : Field::Phase;
        const auto rule = live_entry_rule(row);
        return run_coordinate_control(selector, row, bars_for(row), rule, field, true,
            [row](const Observed& seen) { return live_entry_at_next_open(seen, row); });
    }
    for (const Mode row : {Mode::Exact, Mode::AbsoluteControl, Mode::BMarketAddControl}) {
        const std::string prefix = std::string("f3_") + mode_name(row) + "_";
        if (value.substr(0, prefix.size()) != std::string_view(prefix)) continue;
        const auto bars = bars_for(row);
        const int last = static_cast<int>(bars.size()) - 1;
        const auto check_name = value.substr(prefix.size());
        for (const auto& rule : authored_fills(row, bars)) {
            Field field;
            if (check_name == rule.bar_check) field = Field::Bar;
            else if (check_name == rule.price_check) field = Field::Price;
            else if (rule.phase_check && check_name == rule.phase_check) field = Field::Phase;
            else continue;
            return run_coordinate_control(selector, row, bars, rule, field, false,
                [&](const Observed& seen) { return authored_coordinate(seen, row, last, rule, field); });
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        Mode mode;
        if (parse_mode(argv[1], mode)) return run_mode(mode) ? 0 : 1;
        const auto control = run_guard_control(argv[1]);
        return control ? (*control ? 0 : 1) : 2;
    }
    bool ok = true;
    for (const Mode mode : {Mode::Exact, Mode::AbsoluteControl,
                            Mode::BPendingEntryControl, Mode::BMarketAddControl,
                            Mode::HeldEntryStopFirst, Mode::HeldEntryLimitFirst,
                            Mode::PoocOddStepsLimitFirst, Mode::PoocOddStepsStopFirst,
                            Mode::PoocWholeStepsLimitFirst,
                            Mode::PoocWholeStepsStopFirst,
                            Mode::LiveEntryOddStepsLimitFirst,
                            Mode::LiveEntryOddStepsStopFirst,
                            Mode::LiveEntryWholeStepsLimitFirst,
                            Mode::LiveEntryWholeStepsStopFirst,
                            Mode::StepChecksEmptyInput, Mode::StepChecksStaleInput}) {
        ok = run_mode(mode) && ok;
    }
    return ok ? 0 : 1;
}
