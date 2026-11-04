#include "drawing_slot_test.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>

using namespace pineforge;

namespace {

void test_zero_bound_refusal_returns_na() {
    DrawingArena<BoxRec> arena(drawing_bound(0, 0));
    const DrawingArena<BoxRec> checkpoint = arena;
    Box refused;
    bool threw = false;
    try {
        refused = pf_box_new(arena, 1, 2.0, 3, 4.0);
    } catch (...) {
        threw = true;
    }
    assert(!threw);
    assert(is_na(refused));
    const auto diagnostic = arena.last_allocation_diagnostic();
    assert(diagnostic.status == DrawingAllocationStatus::drawing_slot_bound_exceeded);
    assert(diagnostic.kind == DrawingKind::box);
    assert(diagnostic.configured_bound == 0);
    assert(diagnostic.pending_creation_slots == 0);
    assert(diagnostic.required_slots == 1);
    assert(arena.size() == 0);
    assert(arena.order().empty());
    assert(checkpoint.size() == 0 && checkpoint.order().empty());
}

void test_bound_slot_refusal_returns_na() {
    DrawingArena<LabelRec> arena(drawing_bound(1, 0));
    const Label first = pf_label_new(arena, 10, 1010.0, "first");
    const DrawingArena<LabelRec> checkpoint = arena;

    bool threw = false;
    Label refused;
    try {
        refused = pf_label_new(arena, 20, 1020.0, "refused");
    } catch (...) {
        threw = true;
    }
    assert(!threw);
    assert(is_na(refused));
    const auto diagnostic = arena.last_allocation_diagnostic();
    assert(diagnostic.status == DrawingAllocationStatus::drawing_slot_bound_exceeded);
    assert(diagnostic.kind == DrawingKind::label);
    assert(diagnostic.configured_bound == 1);
    assert(diagnostic.required_slots == 2);
    assert(diagnostic.pending_creation_slots == 0);
    assert(arena.size() == 1);
    assert(arena.occupied_slots() == 1);
    assert(arena.order().size() == 1 && arena.order().front() == first.id);
    assert(pf_label_get_x(arena, first) == 10);
    assert(pf_label_get_text(arena, first) == "first");
    assert(checkpoint.size() == arena.size());
    assert(checkpoint.order() == arena.order());
    assert(checkpoint.at(first.id).text == "first");

    const Label refused_copy = pf_label_copy(arena, first);
    assert(is_na(refused_copy));
    assert(arena.last_allocation_diagnostic().status
           == DrawingAllocationStatus::drawing_slot_bound_exceeded);

    const DrawingArena<LabelRec> failed_snapshot = arena;
    assert(failed_snapshot.last_allocation_diagnostic().status
           == DrawingAllocationStatus::drawing_slot_bound_exceeded);

    DrawingArena<LabelRec> restored(drawing_bound(1, 0));
    restored = checkpoint;
    assert(restored.last_allocation_diagnostic().status == DrawingAllocationStatus::ok);
    restored = failed_snapshot;
    assert(restored.last_allocation_diagnostic().status
           == DrawingAllocationStatus::drawing_slot_bound_exceeded);

    DrawingArena<LabelRec> moved = std::move(restored);
    assert(moved.last_allocation_diagnostic().status
           == DrawingAllocationStatus::drawing_slot_bound_exceeded);
}

void test_bound_id_exhaustion_throws_before_mutation() {
    DrawingArena<LineRec> arena(drawing_bound(1, 0));
    constexpr std::int32_t end = std::numeric_limits<std::int32_t>::max();
    detail::DrawingArenaTestAccess::issued_empty_domain(
        arena, static_cast<std::int64_t>(end) - 1);
    const Line last = pf_line_new(arena, 11, 2011.0, 12, 2012.0);
    assert(last.id == end - 1);
    const DrawingArena<LineRec> before = arena;

    bool caught = false;
    try {
        (void)pf_line_new(arena, 13, 2013.0, 14, 2014.0);
    } catch (const pine_drawing_id_exhausted&) {
        caught = true;
    }
    assert(caught);
    assert(arena.last_allocation_diagnostic().status == DrawingAllocationStatus::ok);
    assert(arena.size() == end);
    assert(arena.order().size() == 1 && arena.order().front() == last.id);
    assert(pf_line_get_x1(arena, last) == 11);
    assert(pf_line_get_y2(arena, last) == 2012.0);
    assert(before.size() == arena.size());
    assert(before.order() == arena.order());
    assert(before.at(last.id).x1 == 11);
}

} // namespace

int main() {
    test_zero_bound_refusal_returns_na();
    test_bound_slot_refusal_returns_na();
    test_bound_id_exhaustion_throws_before_mutation();
    return 0;
}
