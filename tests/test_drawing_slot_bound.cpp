#include "drawing_slot_test.hpp"

using namespace pineforge;

int main() {
    // B=1 plus one independently accounted pending record.
    DrawingArena<LineRec> arena(drawing_bound(1, 1));
    const auto first = drawing_test::line(arena, 10, 1010.0);
    const auto pending = drawing_test::line(arena, 20, 1020.0);
    const DrawingArena<LineRec> checkpoint = arena;
    LineRec& sink_source = arena.at(first.id);
    sink_source.ext_l = true;
    const auto rejected = arena.try_alloc(
        LineRec{30, 31, 1030.0, 1031.0, XLoc::bar_index, false, false, true});
    assert(!rejected.id);
    assert(rejected.diagnostic.status == DrawingAllocationStatus::drawing_slot_bound_exceeded);
    assert(rejected.diagnostic.kind == DrawingKind::line);
    assert(rejected.diagnostic.configured_bound == 1);
    assert(rejected.diagnostic.pending_creation_slots == 1);
    assert(rejected.diagnostic.required_slots == 3);
    assert(arena.size() == 2);
    assert(arena.occupied_slots() == 2);
    assert(arena.order().size() == 2);
    assert(arena.order()[0] == first.id && arena.order()[1] == pending.id);
    // The unsuccessful status preserves the existing borrow interval too.
    assert(sink_source.ext_l);
    assert(checkpoint.at(first.id).ext_l == false);
    pf_line_delete(arena, pending);
    const auto next = drawing_test::line(arena, 40, 1040.0);
    assert(next.id == 2);
    assert(pf_line_get_x1(arena, first) == 10);

    DrawingArena<BoxRec> zero(drawing_bound(0, 0));
    const auto result = zero.try_alloc(BoxRec{1, 3, 2.0, 4.0, XLoc::bar_index, true});
    assert(!result.id && result.diagnostic.required_slots == 1);
    assert(result.diagnostic.status == DrawingAllocationStatus::drawing_slot_bound_exceeded);
    assert(zero.size() == 0 && zero.order().empty());
}
