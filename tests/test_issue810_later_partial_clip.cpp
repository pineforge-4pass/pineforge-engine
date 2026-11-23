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
//   self_check_*          no engine: every check of every engine row on the
//                         rules' own observation (all pass), on an EMPTY one
//                         and on two STALE ones (all fail).
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
// Each check first finds the row's own entry fill by its label, its comment
// (which names the row), its bar, its price and its quantity, and each exit
// fill by its label, comment and bar.  So every check fails when nothing was
// observed, and when the observation is another row's or another bar's.

#include <pineforge/source/pine_native_host.hpp>

#include <cmath>
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
// Every bar opens here, so the entry fills here in every form.
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
    std::string partial_comment;
    std::string later_comment;
    // Printed, not checked: the later exit's quantity and percentage then.
    double later_qty = kNaN;
    double later_percent = kNaN;
};

struct ClipObservation {
    ClipSignal signal;
    std::vector<ClipFill> fills;
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
        if (pine_bar_index() != 0) return;
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
        fill.opened = event.opened_units;
        fill.closed = event.closed_units;
        fill.price = event.resolved_price;
        observation_.fills.push_back(fill);
    }

    const ClipObservation& observation() const { return observation_; }

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
            }
            if (snapshot.source_id == "Half" && snapshot.from_entry == "Long" && row.staged)
                seen.partial_comment = snapshot.comment;
            if (snapshot.source_id == "Later" && snapshot.from_entry == "Long") {
                seen.later_comment = snapshot.comment;
                seen.later_qty = snapshot.projection_remaining_qty;
                seen.later_percent = snapshot.qty_percent;
            }
        }
    }

    ClipRow row_;
    ClipObservation observation_;
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

// Every fill of `label` with `comment` that opens units (opening) or closes
// them, in the order they applied.
std::vector<const ClipFill*> fills_of(const ClipObservation& obs, const char* label,
                                      const std::string& comment, bool opening) {
    std::vector<const ClipFill*> rows;
    for (const auto& fill : obs.fills) {
        if (fill.label != label || fill.comment != comment) continue;
        if (opening ? fill.opened > 0.0 : fill.closed > 0.0) rows.push_back(&fill);
    }
    return rows;
}

// The row's own entry fill: its one opening of "Long", on its bar, at the
// open, of its quantity.  Every check starts from it.
const ClipFill* own_entry(const ClipRow& row, const ClipObservation& obs) {
    const auto rows = fills_of(obs, "Long", row_comment(row, "entry"), true);
    if (rows.size() != 1) return nullptr;
    const ClipFill& fill = *rows.front();
    if (fill.bar != entry_bar_of(row) || !near(fill.price, kFixtureOpen)
        || !near(fill.opened, qty_of_steps(row.entry_steps))) {
        return nullptr;
    }
    return &fill;
}

// The row's one fill of an exit, on the bar that reaches its level, or none.
const ClipFill* own_exit(const ClipRow& row, const ClipObservation& obs, const char* label,
                         const char* leg, int on_bar) {
    const auto rows = fills_of(obs, label, row_comment(row, leg), false);
    if (rows.size() != 1 || rows.front()->bar != on_bar) return nullptr;
    return rows.front();
}

struct ClipVerdict {
    bool ok = false;
    double actual = kNaN;
    double expected = kNaN;
};

// The form, read on the signal bar before any fill: flat, the entry held or
// not as the form says, the partial exit waiting as a definition and the
// later exit placed, each with this row's comment.
ClipVerdict form_at_signal(const ClipRow& row, const ClipObservation& obs) {
    const ClipSignal& seen = obs.signal;
    const bool held = row.form == ClipForm::Held;
    ClipVerdict verdict;
    verdict.actual = seen.entry_held ? 1.0 : 0.0;
    verdict.expected = held ? 1.0 : 0.0;
    verdict.ok = seen.bar == 0 && seen.flat
        && seen.entry_comment == row_comment(row, "entry") && seen.entry_held == held
        && seen.partial_comment == row_comment(row, "half")
        && seen.later_comment == row_comment(row, "later");
    return verdict;
}

// The entry opens the row's quantity once, on its bar, at the open.
ClipVerdict entry_fill(const ClipRow& row, const ClipObservation& obs) {
    const auto rows = fills_of(obs, "Long", row_comment(row, "entry"), true);
    ClipVerdict verdict;
    verdict.expected = qty_of_steps(row.entry_steps);
    if (!rows.empty()) verdict.actual = rows.front()->opened;
    verdict.ok = own_entry(row, obs) != nullptr;
    return verdict;
}

// RQ and RO: the later exit closes min(floor(75% of n), n - floor(50% of n))
// steps, once, on the bar that reaches its stop.
ClipVerdict later_exit_quantity(const ClipRow& row, const ClipObservation& obs) {
    const auto rows = fills_of(obs, "Later", row_comment(row, "later"), false);
    const ClipFill* later = own_exit(row, obs, "Later", "later", kLaterBar);
    ClipVerdict verdict;
    verdict.expected = qty_of_steps(later_steps(row));
    if (!rows.empty()) verdict.actual = rows.front()->closed;
    verdict.ok = own_entry(row, obs) != nullptr && later != nullptr
        && near(later->closed, verdict.expected);
    return verdict;
}

// RQ: the partial exit closes floor(50% of n) steps, once, on the bar that
// reaches its limit.
ClipVerdict partial_exit_quantity(const ClipRow& row, const ClipObservation& obs) {
    const auto rows = fills_of(obs, "Half", row_comment(row, "half"), false);
    const ClipFill* partial = own_exit(row, obs, "Half", "half", kPartialBar);
    ClipVerdict verdict;
    verdict.expected = qty_of_steps(partial_steps(row));
    if (!rows.empty()) verdict.actual = rows.front()->closed;
    verdict.ok = own_entry(row, obs) != nullptr && partial != nullptr
        && near(partial->closed, verdict.expected);
    return verdict;
}

// RQ and RO give whole steps only: every fill of the run is a whole number of
// steps, and the run has its entry fill and both exit fills.  The actual
// value is the number of fills off the step.
ClipVerdict fills_on_quantity_step(const ClipRow& row, const ClipObservation& obs) {
    int off = 0;
    for (const auto& fill : obs.fills) {
        if (!on_qty_step(fill.opened) || !on_qty_step(fill.closed)) ++off;
    }
    ClipVerdict verdict;
    verdict.actual = static_cast<double>(off);
    verdict.expected = 0.0;
    verdict.ok = own_entry(row, obs) != nullptr
        && own_exit(row, obs, "Later", "later", kLaterBar) != nullptr
        && own_exit(row, obs, "Half", "half", kPartialBar) != nullptr && off == 0;
    return verdict;
}

// The later exit fills at its stop, below the open of the bar that reaches
// it, and the partial exit at its limit.  The actual value is the partial
// exit's price.
ClipVerdict exit_fill_prices(const ClipRow& row, const ClipObservation& obs) {
    const ClipFill* later = own_exit(row, obs, "Later", "later", kLaterBar);
    const ClipFill* partial = own_exit(row, obs, "Half", "half", kPartialBar);
    ClipVerdict verdict;
    verdict.expected = partial_level();
    if (partial) verdict.actual = partial->price;
    verdict.ok = own_entry(row, obs) != nullptr && later != nullptr
        && near(later->price, kLaterStop) && partial != nullptr
        && near(partial->price, partial_level());
    return verdict;
}

struct ClipCheck {
    const char* name;
    ClipVerdict (*fn)(const ClipRow&, const ClipObservation&);
};

const ClipCheck kChecks[] = {
    {"form_at_signal", form_at_signal},
    {"entry_fill", entry_fill},
    {"later_exit_quantity", later_exit_quantity},
    {"partial_exit_quantity", partial_exit_quantity},
    {"fills_on_quantity_step", fills_on_quantity_step},
    {"exit_fill_prices", exit_fill_prices},
};

bool run_row(const ClipRow& row) {
    ClipProbe probe(row);
    const auto bars = fixture_bars();
    probe.fixture_retain_all_events();
    probe.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    std::printf("EXECUTION mode=%s status=%s last_error=%s\n", row.name,
                probe.last_error().empty() ? "completed" : "failed",
                probe.last_error().c_str());
    const ClipObservation& obs = probe.observation();
    const ClipSignal& seen = obs.signal;
    std::printf("SIGNAL mode=%s bar=%d flat=%d entry=%s entry_held=%d partial=%s later=%s "
                "later_qty=%.17g later_percent=%.17g\n",
                row.name, seen.bar, seen.flat ? 1 : 0, seen.entry_comment.c_str(),
                seen.entry_held ? 1 : 0, seen.partial_comment.c_str(),
                seen.later_comment.c_str(), seen.later_qty, seen.later_percent);
    for (const auto& fill : obs.fills) {
        std::printf("APPLIED mode=%s label=%s comment=%s bar=%d opened=%.17g closed=%.17g "
                    "price=%.17g\n",
                    row.name, fill.label.c_str(), fill.comment.c_str(), fill.bar,
                    fill.opened, fill.closed, fill.price);
    }
    std::printf("POSITION mode=%s signed_units=%.17g trades=%d\n", row.name,
                probe.physical_position().signed_units, probe.trade_count());
    std::printf("REPORT mode=%s broker_state_hash=%llu\n", row.name,
                static_cast<unsigned long long>(probe.broker_state_hash()));
    if (!probe.last_error().empty()) return false;
    bool ok = true;
    for (const auto& check : kChecks) {
        const ClipVerdict verdict = check.fn(row, obs);
        std::printf("CHECK mode=%s name=%s status=%s actual=%.17g expected=%.17g\n",
                    row.name, check.name, verdict.ok ? "PASS" : "FAIL", verdict.actual,
                    verdict.expected);
        ok = verdict.ok && ok;
    }
    return ok;
}

// The observation the rules give for a row, built from the fixture.
ClipObservation rule_observation(const ClipRow& row) {
    ClipObservation obs;
    obs.signal.bar = 0;
    obs.signal.flat = true;
    obs.signal.entry_comment = row_comment(row, "entry");
    obs.signal.entry_held = row.form == ClipForm::Held;
    obs.signal.partial_comment = row_comment(row, "half");
    obs.signal.later_comment = row_comment(row, "later");
    obs.fills.push_back({"Long", row_comment(row, "entry"), entry_bar_of(row),
                         qty_of_steps(row.entry_steps), 0.0, kFixtureOpen});
    obs.fills.push_back({"Later", row_comment(row, "later"), kLaterBar, 0.0,
                         qty_of_steps(later_steps(row)), kLaterStop});
    obs.fills.push_back({"Half", row_comment(row, "half"), kPartialBar, 0.0,
                         qty_of_steps(partial_steps(row)), partial_level()});
    return obs;
}

enum class ClipInput { Rule, Empty, StaleBar, StaleRow };

struct ClipSelfCheck {
    const char* name;
    ClipInput input;
};

const ClipSelfCheck kSelfChecks[] = {
    {"self_check_rule", ClipInput::Rule},
    {"self_check_empty", ClipInput::Empty},
    {"self_check_stale_bar", ClipInput::StaleBar},
    {"self_check_stale_row", ClipInput::StaleRow},
};

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

// Every check of every engine row passes on the rules' own observation and
// fails on the EMPTY and on both STALE ones.
bool run_self_check(const ClipSelfCheck& self) {
    const bool want_pass = self.input == ClipInput::Rule;
    bool ok = true;
    for (int index = 0; index < kRowCount; ++index) {
        const ClipRow& row = kRows[index];
        const ClipObservation obs = input_for(self.input, index);
        for (const auto& check : kChecks) {
            const bool passed = check.fn(row, obs).ok;
            const bool as_wanted = passed == want_pass;
            std::printf("CHECK mode=%s name=%s:%s status=%s verdict=%s wanted=%s\n",
                        self.name, row.name, check.name, as_wanted ? "PASS" : "FAIL",
                        passed ? "pass" : "fail", want_pass ? "pass" : "fail");
            ok = as_wanted && ok;
        }
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    if (argc == 2) {
        const std::string mode = argv[1];
        for (const auto& row : kRows)
            if (mode == row.name) return run_row(row) ? 0 : 1;
        for (const auto& self : kSelfChecks)
            if (mode == self.name) return run_self_check(self) ? 0 : 1;
        return 2;
    }
    bool ok = true;
    for (const auto& row : kRows) ok = run_row(row) && ok;
    for (const auto& self : kSelfChecks) ok = run_self_check(self) && ok;
    std::printf("RESULT status=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
