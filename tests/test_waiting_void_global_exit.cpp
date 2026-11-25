#include "waiting_exit_fixture.hpp"

#include <algorithm>

namespace {
using namespace waiting_exit;
enum class Opening { Market, Priced, ParentFirst };

class VoidExit final : public Host {
public:
    explicit VoidExit(Opening opening) : Host(true), opening(opening) {}
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        need(bar == callbacks++, "missing or repeated chart bar");
        if (bar == 0) {
            need(physical_position().signed_units == 0 && book().live().empty(),
                 "void exit requires a flat position and no pending parent");
            strategy_exit("Guard", "", 120, opening == Opening::ParentFirst ? 80 : absent);
            limit = working("Guard");
            strategy_order("Park", true, 1, 50);
            park = working("Park");
            if (opening == Opening::ParentFirst) {
                stop = working<no::Stop>("Guard");
                wait_for(limit, park, true);
                wait_for(stop, limit, true);
            } else {
                wait_for(limit, park);
            }
        }
        if (bar == 1) {
            pf_pending_order_v1_t row{};
            need(projected("Guard", &row) == 1 && row.from_entry[0] == '\0'
                 && row.created_position_side == static_cast<int>(PositionSide::FLAT)
                 && live_count("Guard") == (stop.incarnation ? 2 : 1),
                 "original void exit was not on both books before the entry");
            strategy_entry("X", true, opening == Opening::Priced ? 96 : absent, absent, 3);
        }
    }
    void check(Checks& checks) const {
        need(last_run_status() == 0 && last_error().empty() && callbacks == 4 && openings == 1,
             "incomplete run or missing opening observation");
        int user = 0, dependent = 0, ends = 0, misses = 0, observed = 0;
        std::uint64_t limit_end = 0;
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* end = std::get_if<no::CancelledEvent>(&*row.command)) {
                if (end->handle() != limit && end->handle() != stop) continue;
                ++ends;
                if (end->ordinal <= fill_ordinal || end->ordinal > after_opening) continue;
                const auto* wait = std::get_if<no::Wait>(&end->prior_authority);
                if (end->handle() == limit && wait && wait->parent == park
                    && end->reason == no::CancelReason::User) {
                    ++user;
                    limit_end = end->ordinal;
                }
                if (end->handle() == stop && wait && wait->parent == limit
                    && end->reason == no::CancelReason::OwnerGone && limit_end && end->cause
                    && end->cause->run == limit.run && end->cause->ordinal == limit_end)
                    ++dependent;
            } else if (const auto* miss = std::get_if<no::NotWorkingEvent>(&*row.command)) {
                if (miss->target == limit || miss->target == stop) ++misses;
            } else if (const auto* fill = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
                if (fill->ordinal == fill_ordinal && fill->request().label == "X") ++observed;
            }
        }
        need(observed == 1, "missing opening in retained journal");
        const bool keep = opening == Opening::Priced;
        checks.equal("User cancel at market opening", user, keep ? 0 : 1);
        checks.equal("OwnerGone caused by limit cancel", dependent, stop.incarnation ? 1 : 0);
        checks.equal("all cancels", ends, keep ? 0 : stop.incarnation ? 2 : 1);
        checks.equal("redundant cancels", misses, 0);
        checks.equal("live Guard at opening", live_after, keep ? 1 : 0);
        checks.equal("projected Guard at opening", projected_after, keep ? 1 : 0);
        checks.equal("original unarmed limit retained", waiting_after, keep);
    }
private:
    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext&) override {
        if (fill.request().label != "X") return;
        need(fill.interval_index() == 2 && fill.opened_units == 3 && fill.closed_units == 0
             && book().find_live(park), "missing first entry beside separate resting parent");
        ++openings;
        fill_ordinal = fill.ordinal;
        after_opening = book().last_ordinal();
        live_after = live_count("Guard");
        projected_after = projected("Guard");
        const auto* leg = book().find_live(limit);
        const auto* wait = leg ? std::get_if<no::Wait>(&leg->authority) : nullptr;
        waiting_after = wait && wait->parent == park && !native_working_request(limit);
    }
    const Opening opening;
    Handle limit{}, park{}, stop{};
    int callbacks = 0, openings = 0, live_after = -1, projected_after = -1;
    std::uint64_t fill_ordinal = 0, after_opening = 0;
    bool waiting_after = false;
};
} // namespace

int main() {
    using namespace waiting_exit;
    int status = 0;
    for (Opening opening : {Opening::Market, Opening::ParentFirst, Opening::Priced}) {
        const char* name = opening == Opening::Market ? "hidden void global at market opening"
            : opening == Opening::ParentFirst ? "void global parent cancelled first"
                                             : "hidden void global at priced opening";
        status = std::max(status, compare(name, [&](Checks& checks) {
            auto tape = bars<4>();
            if (opening == Opening::Priced)
                tape[2] = {95, 96, 94, 95, 1000, tape[2].timestamp};
            VoidExit host(opening);
            host.run(tape.data(), static_cast<int>(tape.size()));
            need(host.last_error().empty(), host.last_error().c_str());
            host.check(checks);
        }));
    }
    return status;
}
