// Public synthetic native proof: a later partial percentage exit placed while
// flat behind a waiting partial percentage definition of the same pending
// entry (a follow-up row of issue #810 Case A).
//
// Every engine row makes three calls on its signal bar, in this order: a
// market entry of a whole number of quantity steps, a 50% exit whose limit
// is a profit distance in ticks (it waits for the entry's fill as a
// definition), and a 75% exit at an absolute stop.  Bar 2 reaches the stop
// and bar 3 the limit, each travelling past its level.
//
//   pooc_*                process_orders_on_close: the entry is submitted at
//                         once and fills at the signal bar's close.
//   held_*                neither process_orders_on_close nor a commission:
//                         the entry is held to the end of its calculation and
//                         fills at the next bar's open.
//   live_*                a commission and no process_orders_on_close: the
//                         entry is a working order, not held, when the exits
//                         are placed, and fills at the next bar's open.
//   *_three_steps         the entry is three steps, so the waiting share is
//                         one and a half steps.
//   *_whole_step_control  the entry is two steps: the waiting share is one
//                         whole step, and what it leaves equals the later
//                         exit's own floored share, so a cap at the
//                         unrounded remainder does not lower it.
//   self_check_*          no engine: the input checks on the rules' own
//                         observation (PASS), EMPTY and both STALE inputs
//                         (NOT COMPARED). The phase pair needs a real run.
//
// Every expected value is computed here from two order rules, in whole
// steps; none is read from a run.
//   RQ  A percentage exit's quantity is its percentage of what the entry
//       opens, rounded down to the quantity step (src/source/pine_adapter.cpp:
//       quantize_percent_exit_units, quantize_close_units).
//   RO  The exits of one entry take its quantity in the order they were
//       placed, each taking what the ones before it leave (the rule comment
//       of compute_exit_reservation; exit_queue_rank).
// So the partial exit closes floor(50% of n) steps, the later exit
// min(floor(75% of n), n - floor(50% of n)) steps, and every fill is a whole
// number of steps.  The prices follow the fill rules the adapter states: a
// market order fills at the signal bar's close under process_orders_on_close
// and at the next bar's open otherwise, a tick level is the entry's fill
// price moved by that many ticks (materialize_relative_exits), a limit fills
// at its level, and a sell stop at the lower of the bar's open and its level.
//
// The signal form comes from pending rows. For this five-bar batch the entry
// check binds one applied opening by label, row comment, bar, observed path
// phase, price and quantity. The flat entry bars cannot identify the phase by
// price. Each real row also tries that same opening at the opposite phase.
// Exit checks need both the signal form and the entry, and bind exit fills by
// label, comment and bar.

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The probe's quantity step and price tick.
constexpr double kQtyStep = 0.01;
constexpr double kMinTick = 0.01;
// Both the open and close of each entry bar are at this price.
constexpr double kFixtureOpen = 100.0;
// The two exits' percentages, whole numbers so that the floors are exact.
constexpr int kPartialPercent = 50;
constexpr int kLaterPercent = 75;
// The partial exit's profit distance in ticks, and the later exit's stop.
constexpr double kPartialTicks = 200.0;
constexpr double kLaterStop = 98.0;
// How far past a level the bar that reaches it travels.
constexpr double kReach = 1.0;
// The bars that reach the stop and the limit.
constexpr int kLaterBar = 2;
constexpr int kPartialBar = 3;

enum class ClipForm { Pooc, Held, Live };

struct ClipRow {
    const char* name;
    ClipForm form;
    int entry_steps;
};

const ClipRow kRows[] = {
    {"pooc_three_steps", ClipForm::Pooc, 3},
    {"held_three_steps", ClipForm::Held, 3},
    {"live_three_steps", ClipForm::Live, 3},
    {"pooc_whole_step_control", ClipForm::Pooc, 2},
    {"held_whole_step_control", ClipForm::Held, 2},
    {"live_whole_step_control", ClipForm::Live, 2},
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// RQ: the partial exit's share of the entry, in whole steps.
int partial_steps(const ClipRow& row) { return row.entry_steps * kPartialPercent / 100; }

// RQ and RO: the later exit's own share in whole steps, capped at what the
// partial exit placed before it leaves.
int later_steps(const ClipRow& row) {
    const int own = row.entry_steps * kLaterPercent / 100;
    const int left = row.entry_steps - partial_steps(row);
    return own < left ? own : left;
}

double qty_of_steps(int steps) { return static_cast<double>(steps) * kQtyStep; }

// The entry fills at the signal bar's close under process_orders_on_close,
// and at the next bar's open otherwise.
int entry_bar_of(const ClipRow& row) { return row.form == ClipForm::Pooc ? 0 : 1; }
NativePathPhase entry_phase_of(const ClipRow& row) {
    return row.form == ClipForm::Pooc ? NativePathPhase::Close : NativePathPhase::Open;
}

const char* phase_name(NativePathPhase phase) {
    switch (phase) {
    case NativePathPhase::None: return "None";
    case NativePathPhase::Open: return "Open";
    case NativePathPhase::High: return "High";
    case NativePathPhase::Low: return "Low";
    case NativePathPhase::Close: return "Close";
    }
    return "unknown";
}

// The partial exit's limit: the entry's fill price moved by its ticks.
double partial_level() { return kFixtureOpen + kPartialTicks * kMinTick; }

// Each order's comment names its row, so a fill tells which run it is from.
std::string row_comment(const ClipRow& row, const char* leg) {
    return std::string(row.name) + ":" + leg;
}

Bar bar(int index, double open, double high, double low, double close) {
    return {open, high, low, close, 1000.0,
            1'704'067'200'000LL + static_cast<std::int64_t>(index) * 3'600'000LL};
}

// Bars 0 and 1 are flat at the open: the signal bar, and the bar whose open
// fills a held or working entry.  Bar 2 reaches the stop, bar 3 the limit,
// and bar 4 is flat.
std::vector<Bar> fixture_bars() {
    return {
        bar(0, kFixtureOpen, kFixtureOpen, kFixtureOpen, kFixtureOpen),
        bar(1, kFixtureOpen, kFixtureOpen, kFixtureOpen, kFixtureOpen),
        bar(2, kFixtureOpen, kFixtureOpen, kLaterStop - kReach, kFixtureOpen),
        bar(3, kFixtureOpen, partial_level() + kReach, kFixtureOpen, kFixtureOpen),
        bar(4, kFixtureOpen, kFixtureOpen, kFixtureOpen, kFixtureOpen),
    };
}

struct ClipFill {
    std::string label;
    std::string comment;
    int bar = -1;
    NativePathPhase phase = NativePathPhase::None;
    double opened = 0.0;
    double closed = 0.0;
    double price = kNaN;
};

// The pending rows of the signal bar once its three calls are made.
struct ClipSignal {
    int bar = -1;
    bool flat = false;
    std::string entry_comment;
    bool entry_held = false;
    bool entry_working = false;
    std::string partial_comment;
    std::string later_comment;
    // Printed, not checked: the later exit's quantity and percentage then.
    double later_qty = kNaN;
    double later_percent = kNaN;
};

struct ClipObservation {
    ClipSignal signal;
    std::vector<ClipFill> fills;
    double final_position = kNaN;
    int closed_trades = -1;
};

class ClipProbe final : public source::PineNativeHost {
public:
    explicit ClipProbe(const ClipRow& row) : row_(row) {
        source::PineStrategyConfig config;
        config.initial_capital = 10'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kQtyStep;
        // At a pyramiding of one, a commission takes a market entry out of
        // the scopes that hold it to the end of its calculation
        // (same_bar_market_tx_scope).  It changes money only.
        config.commission_value = row.form == ClipForm::Live ? 0.1 : 0.0;
        config.slippage = 0;
        config.pyramiding = 1;
        config.process_orders_on_close = row.form == ClipForm::Pooc;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_mintick(kMinTick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", kQtyStep);
    }

    void on_source_bar(const Bar&) override {
        last_bar_ = static_cast<int>(pine_bar_index());
        if (last_bar_ != 0) return;
        const bool flat_before = physical_position().signed_units == 0.0;
        strategy_entry("Long", true, kNaN, kNaN, qty_of_steps(row_.entry_steps),
                       row_comment(row_, "entry").c_str());
        strategy_exit("Half", "Long", kNaN, kNaN, kNaN, kNaN, kNaN,
                      static_cast<double>(kPartialPercent),
                      row_comment(row_, "half").c_str(), kNaN, "", kPartialTicks, kNaN);
        strategy_exit("Later", "Long", kNaN, kLaterStop, kNaN, kNaN, kNaN,
                      static_cast<double>(kLaterPercent),
                      row_comment(row_, "later").c_str(), kNaN, "", kNaN, kNaN);
        record_signal(flat_before);
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        if (event.opened_units == 0.0 && event.closed_units == 0.0) return;
        ClipFill fill;
        fill.label = event.request().label;
        fill.comment = event.request().comment;
        fill.bar = static_cast<int>(context.coordinate.interval_index);
        fill.phase = context.coordinate.path_phase;
        fill.opened = event.opened_units;
        fill.closed = event.closed_units;
        fill.price = event.resolved_price;
        observation_.fills.push_back(fill);
    }

    const ClipObservation& final_observation() {
        observation_.final_position = physical_position().signed_units;
        observation_.closed_trades = trade_count();
        return observation_;
    }
    int last_bar() const { return last_bar_; }

private:
    void record_signal(bool flat_before) {
        ClipSignal& seen = observation_.signal;
        seen.bar = static_cast<int>(pine_bar_index());
        seen.flat = flat_before && physical_position().signed_units == 0.0;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            const auto& snapshot = row.snapshot;
            if (snapshot.source_id == "Long" && snapshot.opening) {
                seen.entry_comment = snapshot.comment;
                // A held entry waits in the calculation's own command list,
                // marked as a frozen market instruction.
                seen.entry_held = row.staged && snapshot.frozen_market_instruction;
                seen.entry_working = !row.staged;
            }
            if (snapshot.source_id == "Half" && snapshot.from_entry == "Long" && row.staged
                && !std::isfinite(snapshot.exit_levels.limit)
                && std::isfinite(snapshot.exit_levels.profit_ticks))
                seen.partial_comment = snapshot.comment;
            if (snapshot.source_id == "Later" && snapshot.from_entry == "Long" && !row.staged) {
                seen.later_comment = snapshot.comment;
                seen.later_qty = snapshot.projection_remaining_qty;
                seen.later_percent = snapshot.qty_percent;
            }
        }
    }

    ClipRow row_;
    ClipObservation observation_;
    int last_bar_ = -1;
};

bool near(double left, double right) {
    return std::isfinite(left) && std::isfinite(right) && std::fabs(left - right) <= 1e-9;
}

// A quantity that is a whole number of steps.
bool on_qty_step(double qty) {
    if (!std::isfinite(qty)) return false;
    const double steps = qty / kQtyStep;
    return std::fabs(steps - std::round(steps)) <= 1e-6;
}

struct ClipMatch {
    const ClipFill* fill;
    const char* unbound;
};

// Keep a unique but late fill for diagnostics; it cannot bind a comparison.
ClipMatch match_fill(const ClipObservation& obs, const char* label, const std::string& comment,
                     bool opening, int bar) {
    const ClipFill* own = nullptr;
    bool saw_label = false;
    for (const auto& fill : obs.fills) {
        if (fill.label != label || !(opening ? fill.opened > 0.0 : fill.closed > 0.0)) continue;
        saw_label = true;
        if (fill.comment != comment) continue;
        if (own) return {nullptr, "ambiguous_fill"};
        own = &fill;
    }
    if (!own) return {nullptr, saw_label ? "wrong_row_comment" : "missing_fill"};
    return {own, own->bar == bar ? nullptr : "wrong_bar"};
}

enum class ClipStatus { NotCompared, Fail, Pass };
enum class ClipResult { Fail, ControlFailed, Pass };

struct ClipVerdict {
    ClipStatus status;
    std::string reason;
    double actual;
    double expected;

    bool passed() const { return status == ClipStatus::Pass; }
};

ClipVerdict compared(bool equal, const char* mismatch, double actual, double expected) {
    return {equal ? ClipStatus::Pass : ClipStatus::Fail, equal ? "matched" : mismatch,
            actual, expected};
}

// Only the pending rows at the signal: flat, a held or working entry, a
// waiting partial exit definition and the later exit, each with its comment.
ClipVerdict form_at_signal(const ClipRow& row, const ClipObservation& obs) {
    const ClipSignal& seen = obs.signal;
    const bool held = row.form == ClipForm::Held;
    const double expected = held ? 1.0 : 0.0;
    const bool own = seen.entry_comment == row_comment(row, "entry");
    const double actual = own ? (seen.entry_held ? 1.0 : 0.0) : kNaN;
    if (seen.bar < 0) return {ClipStatus::NotCompared, "missing_signal", actual, expected};
    if (seen.bar != 0) return {ClipStatus::NotCompared, "wrong_signal_bar", actual, expected};
    if (seen.entry_comment.empty() || seen.partial_comment.empty() || seen.later_comment.empty())
        return {ClipStatus::NotCompared, "missing_signal_row", actual, expected};
    if (!own || seen.partial_comment != row_comment(row, "half")
        || seen.later_comment != row_comment(row, "later"))
        return {ClipStatus::NotCompared, "wrong_signal_row_comment", actual, expected};
    if (!seen.flat) return compared(false, "not_flat_at_signal", actual, expected);
    return compared(seen.entry_held == held && seen.entry_working != held,
                    "wrong_signal_entry_state", actual, expected);
}

struct ClipEntry {
    const ClipFill* fill;
    ClipVerdict verdict;
};

// Both real and flipped observations must bind the same way.
ClipEntry entry_fill(const ClipRow& row, const ClipObservation& obs) {
    const ClipMatch opening = match_fill(obs, "Long", row_comment(row, "entry"), true,
                                         entry_bar_of(row));
    const double expected = qty_of_steps(row.entry_steps);
    const double actual = opening.fill ? opening.fill->opened : kNaN;
    if (opening.unbound)
        return {opening.fill, {ClipStatus::NotCompared, opening.unbound, actual, expected}};
    const ClipFill& fill = *opening.fill;
    if (!std::isfinite(fill.price))
        return {&fill, {ClipStatus::NotCompared, "missing_entry_price", actual, expected}};
    if (!near(fill.price, kFixtureOpen))
        return {&fill, compared(false, "wrong_entry_price", actual, expected)};
    if (!near(actual, expected))
        return {&fill, compared(false, "wrong_entry_quantity", actual, expected)};
    return {&fill, compared(fill.phase == entry_phase_of(row), "wrong_phase", actual, expected)};
}

ClipVerdict exit_quantity(const ClipMatch& exit, double expected, const std::string& blocked) {
    const double actual = exit.fill ? exit.fill->closed : kNaN;
    if (!blocked.empty()) return {ClipStatus::NotCompared, blocked, actual, expected};
    if (exit.unbound) return {ClipStatus::NotCompared, exit.unbound, actual, expected};
    return compared(near(actual, expected), "wrong_exit_quantity", actual, expected);
}

// Count off-step fills only once the entry and both exits are bound.
ClipVerdict fills_on_quantity_step(const ClipObservation& obs, const std::string& blocked) {
    if (!blocked.empty()) return {ClipStatus::NotCompared, blocked, kNaN, 0.0};
    int off = 0;
    for (const auto& fill : obs.fills) {
        if (!on_qty_step(fill.opened) || !on_qty_step(fill.closed)) ++off;
    }
    return compared(off == 0, "off_quantity_step", static_cast<double>(off), 0.0);
}

// The only calls are Long, Half and Later; the feed reaches Later before Half.
// The individual fill checks bind their comments, bars, prices and quantities.
ClipVerdict complete_fill_list(const ClipObservation& obs, const std::string& blocked) {
    constexpr const char* labels[] = {"Long", "Later", "Half"};
    constexpr std::size_t count = sizeof(labels) / sizeof(labels[0]);
    if (!blocked.empty()) return {ClipStatus::NotCompared, blocked, kNaN, count};
    if (obs.fills.size() != count)
        return compared(false, "wrong_fill_count", obs.fills.size(), count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& fill = obs.fills[i];
        if (fill.label != labels[i])
            return {ClipStatus::Fail, "expected_" + std::string(labels[i]) + "_got_" + fill.label,
                    0.0, 1.0};
        const double opposite = i == 0 ? fill.closed : fill.opened;
        if (!near(opposite, 0.0)) return compared(false, "unexpected_opposite_units", opposite, 0);
    }
    return compared(true, "wrong_fill_count", obs.fills.size(), count);
}

ClipVerdict exit_fill_prices(double stop, double limit, const std::string& blocked) {
    const double expected = partial_level();
    if (!blocked.empty()) return {ClipStatus::NotCompared, blocked, limit, expected};
    if (!std::isfinite(stop)) return {ClipStatus::NotCompared, "missing_exit_price", stop, kLaterStop};
    if (!std::isfinite(limit))
        return {ClipStatus::NotCompared, "missing_exit_price", limit, expected};
    if (!near(stop, kLaterStop)) return compared(false, "wrong_stop_price", stop, kLaterStop);
    return compared(near(limit, expected), "wrong_limit_price", limit, expected);
}

const char* status_name(ClipStatus status) {
    switch (status) {
    case ClipStatus::NotCompared: return "NOT COMPARED";
    case ClipStatus::Fail: return "FAIL";
    case ClipStatus::Pass: return "PASS";
    }
    return "unknown";
}

void print_check(const char* mode, const char* name, const ClipVerdict& verdict) {
    std::printf("CHECK mode=%s name=%s status=%s actual=%.17g expected=%.17g reason=%s\n",
                mode, name, status_name(verdict.status), verdict.actual, verdict.expected,
                verdict.reason.c_str());
}

void print_opening(const char* mode, const char* name, const ClipRow& row, const ClipFill* fill) {
    std::printf("ENTRY mode=%s name=%s row=%s label=%s comment=%s bar=%.0f phase=%s "
                "price=%.17g qty=%.17g expected_label=Long expected_comment=%s "
                "expected_bar=%d expected_phase=%s expected_price=%.17g expected_qty=%.17g\n",
                mode, name, row.name, fill ? fill->label.c_str() : "missing",
                fill ? fill->comment.c_str() : "missing", fill ? fill->bar : kNaN,
                fill ? phase_name(fill->phase) : "missing", fill ? fill->price : kNaN,
                fill ? fill->opened : kNaN, row_comment(row, "entry").c_str(), entry_bar_of(row),
                phase_name(entry_phase_of(row)), kFixtureOpen, qty_of_steps(row.entry_steps));
}

bool wrong_phase_control(const ClipRow& row, const ClipObservation& real, const ClipEntry& entry,
                         const std::string& blocked) {
    constexpr const char* name = "entry_fill_wrong_phase";
    const bool ready = blocked.empty() && entry.verdict.passed();
    const char* original = entry.fill ? phase_name(entry.fill->phase) : "missing";
    const char* opposite = "missing";
    ClipVerdict negative{ClipStatus::NotCompared,
                         blocked.empty() ? "entry_unbound:" + entry.verdict.reason : blocked,
                         kNaN, entry.verdict.expected};
    if (ready) {
        // The positive bound a unique element of real.fills. The copy has
        // that same element at the same index; change only its phase.
        ClipObservation flipped = real;
        const auto index = static_cast<std::size_t>(entry.fill - real.fills.data());
        ClipFill& changed = flipped.fills[index];
        changed.phase = changed.phase == NativePathPhase::Close ? NativePathPhase::Open
                                                                : NativePathPhase::Close;
        opposite = phase_name(changed.phase);
        const ClipEntry result = entry_fill(row, flipped);
        negative = result.verdict;
        print_opening(row.name, name, row, result.fill);
    }
    const bool rejected = negative.status == ClipStatus::Fail && negative.reason == "wrong_phase";
    print_check(row.name, name, negative);
    std::printf("CONTROL mode=%s name=%s status=%s verdict=%s wanted=FAIL wanted_reason=wrong_phase "
                "original_phase=%s opposite_phase=%s\n",
                row.name, name, ready ? (rejected ? "PASS" : "FAIL") : "NOT COMPARED",
                status_name(negative.status), original, opposite);
    return rejected;
}

enum class ClipInput { Rule, Empty, StaleBar, StaleRow };

struct ClipSelfCheck {
    const char* name;
    ClipInput input;
    const char* signal_reason;
    const char* entry_reason;
};

const ClipSelfCheck kSelfChecks[] = {
    {"self_check_rule", ClipInput::Rule, "", ""},
    {"self_check_empty", ClipInput::Empty, "missing_signal", "missing_fill"},
    {"self_check_stale_bar", ClipInput::StaleBar, "wrong_signal_bar", "wrong_bar"},
    {"self_check_stale_row", ClipInput::StaleRow, "wrong_signal_row_comment", "wrong_row_comment"},
};

ClipResult check_observation(const ClipRow& row, const ClipObservation& obs, bool completed,
                             const ClipSelfCheck* self = nullptr, ClipVerdict* first_failure = nullptr) {
    const std::string extra_name = first_failure ? std::string(row.name) + ":extra_fill" : "";
    const char* mode = self ? self->name : (first_failure ? extra_name.c_str() : row.name);
    const ClipSignal& seen = obs.signal;
    std::printf("SIGNAL mode=%s bar=%.0f flat=%s entry=%s entry_held=%s partial=%s later=%s "
                "later_qty=%.17g later_percent=%.17g entry_working=%s row=%s "
                "expected_bar=0 expected_flat=1 expected_entry_working=%d "
                "expected_entry=%s expected_partial=%s expected_later=%s\n",
                mode, seen.bar < 0 ? kNaN : seen.bar,
                seen.bar < 0 ? "missing" : (seen.flat ? "1" : "0"),
                seen.entry_comment.empty() ? "missing" : seen.entry_comment.c_str(),
                seen.entry_comment.empty() ? "missing" : (seen.entry_held ? "1" : "0"),
                seen.partial_comment.empty() ? "missing" : seen.partial_comment.c_str(),
                seen.later_comment.empty() ? "missing" : seen.later_comment.c_str(),
                seen.later_qty, seen.later_percent,
                seen.entry_comment.empty() ? "missing" : (seen.entry_working ? "1" : "0"),
                row.name, row.form == ClipForm::Held ? 0 : 1,
                row_comment(row, "entry").c_str(), row_comment(row, "half").c_str(),
                row_comment(row, "later").c_str());

    bool ok = completed;
    auto report = [&](const char* check, const ClipVerdict& verdict, const std::string& want_reason) {
        const std::string name = self ? std::string(row.name) + ":" + check : check;
        print_check(mode, name.c_str(), verdict);
        if (!self) {
            if (first_failure && ok && !verdict.passed()) *first_failure = verdict;
            ok = verdict.passed() && ok;
            return;
        }
        const ClipStatus wanted = want_reason.empty() ? ClipStatus::Pass : ClipStatus::NotCompared;
        const bool as_wanted = verdict.status == wanted
            && (want_reason.empty() || verdict.reason == want_reason);
        std::printf("CONTROL mode=%s name=%s status=%s verdict=%s wanted=%s wanted_reason=%s\n",
                    mode, name.c_str(), as_wanted ? "PASS" : "FAIL", status_name(verdict.status),
                    status_name(wanted), want_reason.empty() ? "matched" : want_reason.c_str());
        ok = as_wanted && ok;
    };

    const ClipVerdict form = form_at_signal(row, obs);
    const ClipEntry entry = entry_fill(row, obs);
    report("form_at_signal", form, self ? self->signal_reason : "");
    if (self && !ok) return ClipResult::ControlFailed;
    print_opening(mode, "entry_fill", row, entry.fill);
    report("entry_fill", entry.verdict, self ? self->entry_reason : "");
    if (self && !ok) return ClipResult::ControlFailed;

    std::string blocked;
    if (!completed) blocked = "execution_failed";
    else if (!entry.verdict.passed()) blocked = "entry_unbound:" + entry.verdict.reason;
    else if (!form.passed()) blocked = "form_unbound:" + form.reason;
    if (!self && !first_failure && !wrong_phase_control(row, obs, entry, blocked) && blocked.empty())
        return ClipResult::ControlFailed;

    const ClipMatch later = match_fill(obs, "Later", row_comment(row, "later"), false, kLaterBar);
    const ClipMatch partial = match_fill(obs, "Half", row_comment(row, "half"), false, kPartialBar);
    const std::string want_block = self && *self->entry_reason
        ? std::string("entry_unbound:") + self->entry_reason : "";
    report("later_exit_quantity", exit_quantity(later, qty_of_steps(later_steps(row)), blocked),
           want_block);
    if (self && !ok) return ClipResult::ControlFailed;
    report("partial_exit_quantity", exit_quantity(partial, qty_of_steps(partial_steps(row)), blocked),
           want_block);
    if (self && !ok) return ClipResult::ControlFailed;

    std::string pair_blocked = blocked;
    if (pair_blocked.empty() && later.unbound) pair_blocked = std::string("later_exit:") + later.unbound;
    if (pair_blocked.empty() && partial.unbound) pair_blocked = std::string("partial_exit:") + partial.unbound;
    report("fills_on_quantity_step", fills_on_quantity_step(obs, pair_blocked), want_block);
    if (self && !ok) return ClipResult::ControlFailed;
    report("complete_fill_list", complete_fill_list(obs, pair_blocked), want_block);
    if (self && !ok) return ClipResult::ControlFailed;
    const double stop = later.fill ? later.fill->price : kNaN;
    const double limit = partial.fill ? partial.fill->price : kNaN;
    std::printf("EXIT_FILLS mode=%s row=%s later_bar=%.0f expected_later_bar=%d "
                "partial_bar=%.0f expected_partial_bar=%d stop=%.17g expected_stop=%.17g "
                "limit=%.17g expected_limit=%.17g\n",
                mode, row.name, later.fill ? later.fill->bar : kNaN, kLaterBar,
                partial.fill ? partial.fill->bar : kNaN, kPartialBar, stop, kLaterStop,
                limit, partial_level());
    report("exit_fill_prices", exit_fill_prices(stop, limit, pair_blocked), want_block);
    const double remaining = qty_of_steps(row.entry_steps - later_steps(row) - partial_steps(row));
    const int exits = (later_steps(row) > 0) + (partial_steps(row) > 0);
    const auto position = !pair_blocked.empty() || !std::isfinite(obs.final_position)
        ? ClipVerdict{ClipStatus::NotCompared, pair_blocked.empty() ? "missing_position" : pair_blocked,
                      obs.final_position, remaining}
        : compared(near(obs.final_position, remaining), "wrong_final_position", obs.final_position, remaining);
    const auto trades = !pair_blocked.empty() || obs.closed_trades < 0
        ? ClipVerdict{ClipStatus::NotCompared, pair_blocked.empty() ? "missing_trades" : pair_blocked,
                      static_cast<double>(obs.closed_trades), static_cast<double>(exits)}
        : compared(obs.closed_trades == exits, "wrong_closed_trade_count", obs.closed_trades, exits);
    report("final_position", position, want_block);
    report("closed_trade_count", trades, want_block);
    return ok ? ClipResult::Pass : ClipResult::Fail;
}

ClipResult run_row(const ClipRow& row) {
    ClipProbe probe(row);
    const auto bars = fixture_bars();
    probe.fixture_retain_all_events();
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const int last = static_cast<int>(bars.size()) - 1;
    const bool completed = probe.last_error().empty()
        && probe.native_state().kind == NativeLifecycleKind::Completed && probe.last_bar() == last;
    std::printf("EXECUTION mode=%s status=%s last_error=%s last_bar=%d expected_last_bar=%d\n",
                row.name, completed ? "completed" : "incomplete", probe.last_error().c_str(),
                probe.last_bar(), last);
    const ClipObservation& obs = probe.final_observation();
    for (const auto& fill : obs.fills) {
        std::printf("APPLIED mode=%s label=%s comment=%s bar=%d opened=%.17g closed=%.17g "
                    "price=%.17g phase=%s\n",
                    row.name, fill.label.c_str(), fill.comment.c_str(), fill.bar,
                    fill.opened, fill.closed, fill.price, phase_name(fill.phase));
    }
    std::printf("POSITION mode=%s signed_units=%.17g trades=%d\n", row.name,
                obs.final_position, obs.closed_trades);
    std::printf("REPORT mode=%s broker_state_hash=%llu\n", row.name,
                static_cast<unsigned long long>(probe.broker_state_hash()));
    const ClipResult real = check_observation(row, obs, completed);
    if (real != ClipResult::Pass) {
        std::printf("CONTROL mode=%s name=reject_extra_fill status=NOT COMPARED "
                    "reason=real row did not pass\n", row.name);
        return real;
    }

    // Preserve every real fill and the final position and ledger; append one
    // differently named opening of one whole step on the final, flat bar.
    ClipObservation extra = obs;
    extra.fills.push_back({"Extra", row_comment(row, "extra"), last, NativePathPhase::Close,
                           kQtyStep, 0.0, bars.back().close});
    ClipVerdict failure{ClipStatus::Pass, "matched", kNaN, kNaN};
    const auto result = check_observation(row, extra, completed, nullptr, &failure);
    const bool rejected = result == ClipResult::Fail && failure.status == ClipStatus::Fail
        && failure.reason == "wrong_fill_count";
    std::printf("CONTROL mode=%s name=reject_extra_fill status=%s verdict=%s reason=%s "
                "actual=%.17g expected=%.17g\n", row.name, rejected ? "PASS" : "FAIL",
                status_name(failure.status), failure.reason.c_str(), failure.actual, failure.expected);
    return rejected ? ClipResult::Pass : ClipResult::Fail;
}

// The observation the rules give for a row, built from the fixture.
ClipObservation rule_observation(const ClipRow& row) {
    ClipObservation obs;
    obs.signal.bar = 0;
    obs.signal.flat = true;
    obs.signal.entry_comment = row_comment(row, "entry");
    obs.signal.entry_held = row.form == ClipForm::Held;
    obs.signal.entry_working = !obs.signal.entry_held;
    obs.signal.partial_comment = row_comment(row, "half");
    obs.signal.later_comment = row_comment(row, "later");
    obs.fills.push_back({"Long", row_comment(row, "entry"), entry_bar_of(row), entry_phase_of(row),
                         qty_of_steps(row.entry_steps), 0.0, kFixtureOpen});
    obs.fills.push_back({"Later", row_comment(row, "later"), kLaterBar, NativePathPhase::None, 0.0,
                         qty_of_steps(later_steps(row)), kLaterStop});
    obs.fills.push_back({"Half", row_comment(row, "half"), kPartialBar, NativePathPhase::None, 0.0,
                         qty_of_steps(partial_steps(row)), partial_level()});
    obs.final_position = qty_of_steps(row.entry_steps - later_steps(row) - partial_steps(row));
    obs.closed_trades = (later_steps(row) > 0) + (partial_steps(row) > 0);
    return obs;
}

// EMPTY is nothing observed.  STALE bar is the row's own observation one bar
// late, signal record included; STALE row is the next row's observation.
ClipObservation input_for(ClipInput input, int index) {
    if (input == ClipInput::Empty) return ClipObservation{};
    if (input == ClipInput::StaleRow)
        return rule_observation(kRows[(index + 1) % kRowCount]);
    ClipObservation obs = rule_observation(kRows[index]);
    if (input == ClipInput::StaleBar) {
        obs.signal.bar += 1;
        for (auto& fill : obs.fills) fill.bar += 1;
    }
    return obs;
}

// These inputs exercise the predicates without supplying a real positive.
bool run_self_check(const ClipSelfCheck& self) {
    for (int index = 0; index < kRowCount; ++index) {
        const auto obs = input_for(self.input, index);
        if (check_observation(kRows[index], obs, true, &self) != ClipResult::Pass) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        const std::string mode = argv[1];
        for (const auto& row : kRows)
            if (mode == row.name) return run_row(row) == ClipResult::Pass ? 0 : 1;
        for (const auto& self : kSelfChecks)
            if (mode == self.name) return run_self_check(self) ? 0 : 1;
        return 2;
    }
    for (const auto& self : kSelfChecks) {
        if (!run_self_check(self)) {
            std::printf("RESULT status=FAIL\n");
            return 1;
        }
    }
    bool ok = true;
    for (const auto& row : kRows) {
        const ClipResult result = run_row(row);
        ok = result == ClipResult::Pass && ok;
        if (result == ClipResult::ControlFailed) break;
    }
    std::printf("RESULT status=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
