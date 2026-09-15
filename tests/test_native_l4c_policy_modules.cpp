#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
#include <pineforge/compat/pine/reservation_expansion.hpp>
#include <pineforge/engine.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)

native_order::RequestHandle handle(std::uint64_t incarnation) {
    native_order::RequestHandle result{};
    result.incarnation = incarnation;
    return result;
}

void priority_policy() {
    compat::pine::OrderPriority policy;
    policy.attach();
    compat::pine::OrderPriorityCandidate parent;
    parent.handle = handle(10); parent.kind = compat::pine::OrderPriorityKind::Entry;
    parent.id = "L"; parent.created_bar = 4; parent.source_sequence = 2;
    parent.recreated_after_named_cancelled = 8; parent.named_cancel_surviving_exit = 9;
    parent.created_flat = true; parent.default_quantity = true; parent.stop = 99.0;
    compat::pine::OrderPriorityCandidate child;
    child.handle = handle(11); child.kind = compat::pine::OrderPriorityKind::Exit;
    child.from_entry = "L"; child.created_bar = 4; child.source_sequence = 1;
    child.predecessor = 9; child.created_flat = true; child.stop = 95.0; child.limit = 105.0;
    const auto decision = policy.select({true, true, false, false, false, false, true, 5},
                                        {parent, child});
    CHECK(decision.has_value());
    if (decision) CHECK(decision->parent == parent.handle && decision->child == child.handle);
    child.oca_name = "OCA";
    CHECK(!policy.select({true, true, false, false, false, false, true, 5}, {parent, child}));
}

void activation_and_lifecycle_policy() {
    compat::pine::ExitActivationPolicy activation({7, 3, 1, 94.0, 95.0, 105.0, {}});
    const auto bounds = activation.resolve(7, 3);
    CHECK(bounds.position_cycle == 7 && bounds.stop_first_bar == 4 && bounds.limit_first_bar == 3);

    exit_legs::Lifecycle legs;
    legs.set_prices({105.0, 95.0, std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::quiet_NaN()});
    legs.attach(41, 7);
    const auto suspended = compat::pine::select_exit_suspension(
        legs, {{1, 3, exit_legs::Domain::Ordinary, exit_legs::Phase::Observation},
               1, 100.0, 1.0, 100.0, 100.0, false, true});
    CHECK(suspended.has_value());
    if (suspended) {
        const exit_legs::Action action{legs.target(), legs.revision(),
                                       {1, 3, exit_legs::Domain::Ordinary,
                                        exit_legs::Phase::Observation}, *suspended};
        CHECK(legs.apply(legs.target(), action) == exit_legs::Result::Applied);
        CHECK(legs.dormant());
    }
}

void birth_and_reservation_policy() {
    const auto birth = OrderBirth::fill_evaluation(
        3, 3000, BirthCursor::point(BirthCursorDomain::HistoricalPath, 1, 4),
        101.0, 2, 2, 2);
    CHECK(compat::pine::select_historical_birth_reach(birth, false)
          == compat::pine::HistoricalBirthReach::ExtremeWaypoints);
    CHECK(compat::pine::select_historical_birth_reach(birth, true)
          == compat::pine::HistoricalBirthReach::ExtremeWaypoints);
    ReservationExpansion expansion;
    expansion.capture(50, 7, PositionSide::LONG, 10.0);
    double projected = 10.0;
    expansion.grow(projected, 7, PositionSide::LONG, 10.0,
                   7, PositionSide::LONG, 12.0, 1e-9);
    CHECK(projected == 12.0);
    expansion.close_population(51);
    CHECK(!expansion.population_open());
    compat::pine::ReservationGrowthCandidate candidate;
    candidate.incarnation = 41; candidate.market_entry = true; candidate.is_long = true;
    candidate.created_position_side = PositionSide::LONG; candidate.created_bar = 3;
    const auto selected = compat::pine::select_reservation_growth_sources(
        {candidate}, "", true, false, 100.0, 3, PositionSide::LONG);
    CHECK(selected.size() == 1 && selected.front() == 41);
    CHECK(compat::pine::admits_reservation_expansion(selected, false, 12.0, 12.0));
}
} // namespace

int main() {
    priority_policy();
    activation_and_lifecycle_policy();
    birth_and_reservation_policy();
    std::printf("L4c policy modules: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
