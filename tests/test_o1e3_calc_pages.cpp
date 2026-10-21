// Uses the old API too: on 6cfcff0696 the reached schedule retains an O(N)
// vector. The remote RED must be the byte-bound assertion, not a missing API.
#include "o1e3_allocation_probe.hpp"
#include "../src/native_calc_plan.hpp"

#include <cstdio>
#include <stdexcept>

namespace nc = pineforge::native_calc;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
nc::BarClock clock_grid() {
    return [](std::int64_t at, nc::Slot& slot) {
        slot.end_ms = (at / 1000 + 1) * 1000;
        slot.next_open_ms = slot.end_ms;
        slot.tradable = true;
        return true;
    };
}
void check_schedule(std::size_t population) {
    nc::State before;
    before.has_last_key = true;
    before.has_prev_event = true;
    before.cursor_ms = 0;
    const nc::Event event{nc::EventKind::AdvanceTime, static_cast<std::int64_t>(population * 1000)};
    const auto clock = clock_grid();
    const auto counted = nc::plan_schedule(before, nc::kCadenceBarClose, &event, 1, clock, false);
    require(counted.status == nc::Status::Ok && counted.count == population, "E18 population not reached");
    require(counted.points.empty(), "count pass materialized points");
    o1e3_test::AllocationProbe memory;
    const auto plan = nc::plan_schedule(before, nc::kCadenceBarClose, &event, 1, clock, true);
    const auto peak = memory.stop();
    require(plan.status == nc::Status::Ok && plan.count == population, "E18 apply population");
    require(plan.points.size() == population, "E18 missing rows");
    require(peak <= 16384, "E18 schedule retained more than fixed workspace");
    require(plan.after.tie() == counted.after.tie(), "count/apply final state differs");
    std::size_t delivered = 0;
    for (const auto& point : plan.points) {
        require(point.event_index == 0 && point.event == 1, "event association changed");
        require(point.bar_key == delivered + 1 && point.calc == delivered
                && point.calc_ordinal == 0, "schedule order changed");
        require(point.at_ms == static_cast<std::int64_t>((delivered + 1) * 1000), "schedule time changed");
        require(point.phase == nc::Phase::Pre && point.finality == nc::Finality::Confirmed,
                "schedule phase/finality changed");
        // Independent nested reads must not evict the iterator's own row.
        const auto far = plan.points[population - 1 - delivered];
        require(far.bar_key == population - delivered, "nonlocal point read");
        require(point.bar_key == delivered + 1, "nested read evicted borrowed point");
        ++delivered;
    }
    require(delivered == population, "E18 full-consumption witness");
    std::size_t reversed = 0;
    for (auto it = plan.points.rbegin(); it != plan.points.rend(); ++it) {
        const auto& row = *it;
        require(row.bar_key == population - reversed, "E18 reverse cursor order");
        (void)plan.points[reversed];
        require(row.bar_key == population - reversed, "E18 reverse cursor evicted row");
        ++reversed;
    }
    require(reversed == population, "E18 reverse full consumption");
    // A late invalid event must leave no rows or partial state.
    const nc::Event invalid[] = {event, {static_cast<nc::EventKind>(255), event.time_ms}};
    const auto refused = nc::plan_schedule(before, nc::kCadenceBarClose, invalid, 2, clock, true);
    require(refused.status == nc::Status::UnknownEventKind && refused.count == 0
            && refused.points.empty() && refused.after.tie() == before.tie(), "late refusal changed state");
}
}
int main() {
    try { check_schedule(1024); check_schedule(8192); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
