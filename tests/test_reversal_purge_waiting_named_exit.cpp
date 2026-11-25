#include "waiting_exit_fixture.hpp"

#include <algorithm>

namespace {
using namespace waiting_exit;
enum class Route { Hidden, Visible, Global, ParentBefore, ParentFirst, Reservation };

class Reversal final : public Host {
public:
    Reversal(Route route, int sign) : Host(true), route(route), sign(sign) {}

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        need(bar == callbacks++, "missing or repeated chart bar");
        const char* from = route == Route::Global ? "" : "X";
        if (bar == 0) strategy_entry("X", sign < 0, absent, absent, 10);
        if (bar == 2) {
            need(physical_position().signed_units == -sign * 10, "missing original ten-unit lot");
            if (route == Route::ParentFirst) {
                // Park is itself a named exit of X in this row. The purge
                // selects it before Guard; cancelling it ends Guard inline.
                strategy_exit("Park", "X", 100 - sign * 20, absent, absent, absent, absent, 50);
            }
            strategy_exit("Guard", from, 100 - sign * 4, absent);
        }
        if (bar == 3) {
            if (route != Route::ParentFirst)
                strategy_order("Park", sign > 0, 2, 100 - sign * 10);
            park = working("Park");
            guard = working("Guard");
            pf_pending_order_v1_t row{};
            need(projected("Guard", &row) == 1 && row.incarnation == guard.incarnation
                 && std::string(row.from_entry) == from && row.limit_price == 100 - sign * 4
                 && row.created_position_side == static_cast<int>(
                        sign > 0 ? PositionSide::SHORT : PositionSide::LONG),
                 "missing limit on the old position in the adapter projection");
            if (route == Route::Reservation)
                wait_for(guard, park, false, no::Reduce{no::ExplicitUnits{10}});
            else
                wait_for(guard, park, route == Route::Visible);
        }
        if (bar == 4) {
            const auto* leg = book().find_live(guard);
            const auto* wait = leg ? std::get_if<no::Wait>(&leg->authority) : nullptr;
            need(wait && wait->parent == park && projected("Guard") == 1
                 && book().find_live(park), "waiting leg did not reach the reversal");
            strategy_entry("X", sign > 0, absent, absent, 30);
            if (route == Route::ParentBefore) {
                const auto ended = cancel(park);
                need(ended.status == no::CancelStatus::Cancelled && !book().find_live(guard),
                     "explicit parent cancel did not end the waiting leg before the reversal");
                parent_before = ended.event_ordinal;
            }
        }
        if (bar == 6) {
            // A surviving Park trades at 90 (110 into short). The old limit
            // at 96 (104) is not reached until the open of bar 8.
            no::Request request{no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings}},
                                "Available", ""};
            const auto accepted = submit(request);
            need(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                 "could not place the independent close net of sibling reservations");
            available = *accepted.handle;
        }
        if (bar == 8) {
            final_position = physical_position().signed_units;
            final_live = live_count("Guard");
            final_projected = projected("Guard");
        }
    }

    void check(Checks& checks) const {
        need(last_run_status() == 0 && last_error().empty() && callbacks == 9
             && reversals == 1 && available.incarnation && live_after >= 0 && final_live >= 0,
             "incomplete run or missing reversal observation");
        int ends = 0, user = 0, dependent = 0, misses = 0, guard_fills = 0, arms = 0;
        int park_fills = 0, net_fills = 0;
        bool park_fill_at_level = true;
        double net_units = 0;
        std::uint64_t park_cancel = 0;
        const bool cancelled_before = route == Route::ParentBefore;
        const bool parent_ends_leg = cancelled_before || route == Route::ParentFirst;
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* end = std::get_if<no::CancelledEvent>(&*row.command)) {
                const bool at_reversal = end->ordinal > reversal && end->ordinal <= last_at_reversal;
                const bool in_time = cancelled_before
                    ? end->ordinal >= parent_before && end->ordinal < reversal : at_reversal;
                if (in_time && end->handle() == park && end->reason == no::CancelReason::User
                    && (!cancelled_before || end->ordinal == parent_before))
                    park_cancel = end->ordinal;
                if (end->handle() != guard) continue;
                ++ends;
                const auto* wait = std::get_if<no::Wait>(&end->prior_authority);
                if (!in_time || !wait || wait->parent != park) continue;
                if (end->reason == no::CancelReason::User) ++user;
                if (end->reason == no::CancelReason::OwnerGone && park_cancel && end->cause
                    && end->cause->run == park.run && end->cause->ordinal == park_cancel
                    && end->ordinal > park_cancel) ++dependent;
            } else if (const auto* miss = std::get_if<no::NotWorkingEvent>(&*row.command)) {
                if (miss->target == guard || miss->target == park) ++misses;
            } else if (const auto* arm = std::get_if<no::ArmedEvent>(&*row.command)) {
                if (arm->definition->handle == guard) ++arms;
            } else if (const auto* fill = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                if (fill->request().label == "Guard") ++guard_fills;
                if (fill->handle() == park) {
                    ++park_fills;
                    park_fill_at_level = park_fill_at_level && fill->interval_index() == 6
                        && fill->resolved_price == 100 - sign * 10
                        && fill->opened_units == sign * 2 && fill->closed_units == 0;
                }
                if (fill->handle() == available) {
                    checks.equal("net close bar", fill->interval_index(), 7);
                    ++net_fills;
                    net_units += fill->closed_units;
                }
            }
        }
        checks.equal("User cancel at reversal", user, parent_ends_leg ? 0 : 1);
        checks.equal("parent cancelled at the expected point", park_cancel != 0, parent_ends_leg);
        checks.equal("OwnerGone caused by parent cancel", dependent, parent_ends_leg ? 1 : 0);
        checks.equal("all terminal cancels of original leg", ends, 1);
        checks.equal("redundant cancels", misses, 0);
        checks.equal("original handle live at reversal", handle_after, false);
        checks.equal("live Guard requests at reversal", live_after, 0);
        checks.equal("projected Guard at reversal", projected_after, 0);
        checks.equal("Park live at reversal", park_after, !parent_ends_leg);
        checks.equal("Park fills", park_fills, parent_ends_leg ? 0 : 1);
        need(parent_ends_leg || (park_fills == 1 && park_fill_at_level),
             "later fill and reservation comparison requires the two-unit parent fill at its level");
        checks.equal("late arms", arms, 0);
        checks.equal("Guard fills", guard_fills, 0);
        const double half = parent_ends_leg ? 15 : 16;  // (30 + 0 or 2) / 2
        checks.equal("net close fills", net_fills, 1);
        checks.equal("units available to the half close", net_units, half);
        checks.equal("final signed units", final_position, sign * half);
        checks.equal("final live Guard requests", final_live, 0);
        checks.equal("final projected Guard", final_projected, 0);
    }

private:
    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext&) override {
        if (fill.request().label != "X" || fill.closed_units == 0) return;
        ++reversals;
        need(fill.interval_index() == 5 && fill.closed_units == 10 && fill.opened_units == sign * 30
             && fill.cycle_before != fill.cycle_after && fill.closed_trade_count == 1
             && get_trade(static_cast<int>(fill.first_trade_index)).entry_id == "X"
             && physical_position().signed_units == sign * 30, "expected the same-id reversal");
        reversal = fill.ordinal;
        last_at_reversal = book().last_ordinal();
        handle_after = book().find_live(guard) != nullptr;
        park_after = book().find_live(park) != nullptr;
        live_after = live_count("Guard");
        projected_after = projected("Guard");
    }

    const Route route;
    const int sign;
    Handle guard{}, park{}, available{};
    int callbacks = 0, reversals = 0, live_after = -1, projected_after = -1;
    int final_live = -1, final_projected = -1;
    bool handle_after = false, park_after = false;
    double final_position = absent;
    std::uint64_t reversal = 0, last_at_reversal = 0, parent_before = 0;
};

// P previously held a long. Its new short limit and bracket are pending
// when X reverses A. Only the side of that pending P determines its fate.
class RestingEntry final : public Host {
public:
    explicit RestingEntry(bool old_side) : Host(false, 2, true), old_side(old_side) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        need(i == callbacks++, "missing or repeated chart bar");
        if (i == 0) strategy_entry("P", true, absent, absent, 2);
        if (i == 1) strategy_close_all();
        if (i == 2) strategy_entry("A", !old_side, absent, absent, 1);
        if (i == 3) {
            strategy_entry("P", false, 110, absent, 1);
            strategy_exit("Guard", "P", 90, 120);
        }
        if (i == 4) {
            parent = working("P");
            leg = working("Guard");
            stop = working<no::Stop>("Guard");
            pf_pending_order_v1_t row{};
            need(closed_trade_count() == 1 && get_trade(0).entry_id == "P"
                 && get_trade(0).entry_incarnation != parent.incarnation
                 && physical_position().signed_units == (old_side ? -1 : 1) && projected("Guard", &row) == 1
                 && std::string(row.from_entry) == "P" && row.limit_price == 90
                 && row.stop_price == 120, "missing reused pending parent and bracket");
            // P has orders on both sides in its history. Its short order
            // can wait as a transaction even when Pine sized it at match.
            strategy_order("Park", true, 1, 50);
            wait_for(parent, working("Park"), false, no::Transact{-1});
            wait_for(leg, parent);
            strategy_entry("X", old_side, absent, absent, 1);
        }
    }
    void check(Checks& checks) const {
        need(callbacks == 6 && last_run_status() == 0 && last_error().empty() && reversals == 1,
             "incomplete surviving-entry run");
        int ends = 0, user = 0, misses = 0, observed = 0;
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* fill = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                if (fill->ordinal == reversal && fill->request().label == "X") ++observed;
            } else if (const auto* end = std::get_if<no::CancelledEvent>(&*row.command)) {
                if (end->handle() == leg || end->handle() == stop) {
                    ++ends;
                    if (end->reason == no::CancelReason::User && end->ordinal > reversal
                        && end->ordinal <= after) ++user;
                }
            } else if (const auto* miss = std::get_if<no::NotWorkingEvent>(&*row.command)) {
                if (miss->target == leg || miss->target == stop) ++misses;
            }
        }
        need(observed == 1, "missing X reversal in retained journal");
        checks.equal("original waiting leg retained with its parent", still_waiting, !old_side);
        checks.equal("original stop retained", stop_live, !old_side);
        checks.equal("hidden resting parent retained", parent_waiting, true);
        checks.equal("projected brackets from P", projected_after, old_side ? 0 : 1);
        if (!old_side) checks.equal("projected bracket levels", same_levels, true);
        checks.equal("cancellations of bracket legs", ends, old_side ? 2 : 0);
        checks.equal("User cancels at the side change", user, old_side ? 2 : 0);
        checks.equal("redundant bracket cancels", misses, 0);
    }
private:
    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext&) override {
        if (fill.request().label != "X") return;
        need(fill.interval_index() == 4 && fill.closed_units == 1 && fill.opened_units == (old_side ? 1 : -1)
             && fill.closed_trade_count == 1 && get_trade(static_cast<int>(fill.first_trade_index)).entry_id == "A",
             "X did not reverse A");
        ++reversals;
        reversal = fill.ordinal;
        after = book().last_ordinal();
        parent_waiting = book().find_live(parent) && !native_working_request(parent);
        const auto* live = book().find_live(leg);
        const auto* wait = live ? std::get_if<no::Wait>(&live->authority) : nullptr;
        still_waiting = wait && wait->parent == parent && !native_working_request(leg);
        stop_live = book().find_live(stop) != nullptr;
        pf_pending_order_v1_t row{};
        projected_after = projected("Guard", &row);
        same_levels = std::string(row.from_entry) == "P" && row.limit_price == 90 && row.stop_price == 120;
    }
    const bool old_side;
    Handle parent{}, leg{}, stop{};
    int callbacks = 0, reversals = 0, projected_after = -1;
    std::uint64_t reversal = 0, after = 0;
    bool still_waiting = false, stop_live = false, parent_waiting = false, same_levels = false;
};
} // namespace

int main() {
    using namespace waiting_exit;
    struct Case { const char* name; Route route; int sign; };
    int status = 0;
    for (const Case c : {Case{"hidden named into long", Route::Hidden, 1},
                         Case{"hidden named into short", Route::Hidden, -1},
                         Case{"resting parent cancelled before reversal", Route::ParentBefore, -1},
                         Case{"named parent cancelled first", Route::ParentFirst, 1},
                         Case{"hidden named reservation", Route::Reservation, -1},
                         Case{"visible named", Route::Visible, 1},
                         Case{"hidden global", Route::Global, -1}}) {
        status = std::max(status, compare(c.name, [&](Checks& checks) {
            auto tape = bars<9>();
            const double level = 100 - c.sign * 10;
            for (int i : {6, 7}) tape[i] = {level, level, level, level, 1000, tape[i].timestamp};
            Reversal host(c.route, c.sign);
            host.run(tape.data(), static_cast<int>(tape.size()));
            need(host.last_error().empty(), host.last_error().c_str());
            host.check(checks);
        }));
    }
    for (bool old_side : {false, true}) {
        status = std::max(status, compare(old_side ? "hidden resting entry on closed side"
                                                  : "hidden named surviving entry", [&](Checks& checks) {
            const auto tape = bars<6>();
            RestingEntry host(old_side);
            host.run(tape.data(), static_cast<int>(tape.size()));
            need(host.last_error().empty(), host.last_error().c_str());
            host.check(checks);
        }));
    }
    return status;
}
