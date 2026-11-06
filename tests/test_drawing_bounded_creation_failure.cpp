#include "drawing_slot_test.hpp"

#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

using namespace pineforge;

namespace {

// An exhausted drawing id domain, read the way a run reads it: the code and
// canonical args classify_run_failure reports for the thrown exception, and
// its English.  The id bound is the int32 domain the fixture places the arena
// at the edge of, not a value read back from the header under test.
void require_id_exhaustion_reported(const std::exception& error, const char* object) {
    constexpr std::int32_t end = std::numeric_limits<std::int32_t>::max();
    const auto failure = classify_run_failure(error);
    assert(failure.code == RunFailureCode::pine_runtime_limit);
    assert(failure.args);
    assert(*failure.args
           == "{\"limit\":\"drawing_ids\",\"max\":" + std::to_string(end) + "}");
    assert(std::string(error.what()) == std::string(object) + " drawing ID domain exhausted");
}

// A bound arena placed `remaining` ids before the end of the domain and filled
// to the edge with real creations refuses the next creation, and a copy of a
// live drawing, with the typed id-exhaustion failure: for every kind, with the
// slot bound full or not.  Both refusals leave the arena as it was.
template <class Rec, class Handle, class Create, class Copy>
void check_bound_id_exhaustion(const char* object, DrawingBoundConfig bound,
                               std::int32_t remaining, Create create, Copy copy) {
    constexpr std::int32_t end = std::numeric_limits<std::int32_t>::max();
    DrawingArena<Rec> arena(bound);
    detail::DrawingArenaTestAccess::issued_empty_domain(
        arena, static_cast<std::int64_t>(end) - remaining);
    Handle last{};
    for (std::int32_t i = 0; i < remaining; ++i) {
        last = create(arena, i);
        assert(last.id == end - remaining + i);
    }
    assert(arena.size() == end);
    const DrawingArena<Rec> before = arena;

    bool created_threw = false;
    try {
        (void)create(arena, remaining);
    } catch (const pine_drawing_id_exhausted& error) {
        created_threw = true;
        require_id_exhaustion_reported(error, object);
    }
    assert(created_threw);
    bool copied_threw = false;
    try {
        (void)copy(arena, last);
    } catch (const pine_drawing_id_exhausted& error) {
        copied_threw = true;
        require_id_exhaustion_reported(error, object);
    }
    assert(copied_threw);
    assert(arena.size() == before.size());
    assert(arena.order() == before.order());
    assert(arena.last_allocation_diagnostic().status
           == before.last_allocation_diagnostic().status);
}

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
    } catch (const pine_drawing_id_exhausted& error) {
        caught = true;
        require_id_exhaustion_reported(error, "line");
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

void test_bound_id_exhaustion_is_typed_for_every_kind() {
    // Lines: the slot bound is full when the ids run out (the id check comes
    // first), and again with room to spare and with pending creation slots.
    const auto new_line = [](DrawingArena<LineRec>& a, std::int32_t i) {
        return pf_line_new(a, 30 + i, 3000.0 + i, 40 + i, 4000.5 + i);
    };
    const auto copy_line = [](DrawingArena<LineRec>& a, Line h) { return pf_line_copy(a, h); };
    check_bound_id_exhaustion<LineRec, Line>("line", drawing_bound(2, 0), 2, new_line, copy_line);
    check_bound_id_exhaustion<LineRec, Line>("line", drawing_bound(5, 1), 2, new_line, copy_line);
    check_bound_id_exhaustion<LineRec, Line>("line", drawing_bound(3, 2), 3, new_line, copy_line);
    check_bound_id_exhaustion<BoxRec, Box>(
        "box", drawing_bound(4, 1), 3,
        [](DrawingArena<BoxRec>& a, std::int32_t i) {
            return pf_box_new(a, 50 + i, 5050.25 + i, 60 + i, 5000.75 + i);
        },
        [](DrawingArena<BoxRec>& a, Box h) { return pf_box_copy(a, h); });
    check_bound_id_exhaustion<LabelRec, Label>(
        "label", drawing_bound(1, 0), 1,
        [](DrawingArena<LabelRec>& a, std::int32_t i) {
            return pf_label_new(a, 70 + i, 7070.5 + i, "label " + std::to_string(i));
        },
        [](DrawingArena<LabelRec>& a, Label h) { return pf_label_copy(a, h); });
    check_bound_id_exhaustion<LabelRec, Label>(
        "label", drawing_bound(6, 3), 4,
        [](DrawingArena<LabelRec>& a, std::int32_t i) {
            return pf_label_new(a, 80 + i, 8080.125 + i, "note " + std::to_string(i * 7));
        },
        [](DrawingArena<LabelRec>& a, Label h) { return pf_label_copy(a, h); });
}

} // namespace

int main() {
    test_zero_bound_refusal_returns_na();
    test_bound_slot_refusal_returns_na();
    test_bound_id_exhaustion_throws_before_mutation();
    test_bound_id_exhaustion_is_typed_for_every_kind();
    return 0;
}
