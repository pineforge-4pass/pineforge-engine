#include "drawing_slot_test.hpp"

using namespace pineforge;

int main() {
    DrawingArena<LineRec> arena(drawing_bound(1, 0));
    constexpr std::int32_t end = std::numeric_limits<std::int32_t>::max();
    detail::DrawingArenaTestAccess::issued_empty_domain(arena,
                                                      static_cast<std::int64_t>(end) - 2);
    const auto penultimate = drawing_test::line(arena, 10, 2010.0);
    assert(penultimate.id == end - 2);
    pf_line_delete(arena, penultimate);
    const DrawingArena<LineRec> checkpoint = arena;
    const auto last = drawing_test::line(arena, 11, 2011.0);
    assert(last.id == end - 1 && arena.size() == end);
    const auto exhausted = arena.try_alloc(
        LineRec{12, 13, 2012.0, 2013.0, XLoc::bar_index, false, false, true});
    assert(!exhausted.id);
    assert(exhausted.diagnostic.status == DrawingAllocationStatus::pine_drawing_id_exhausted);
    assert(arena.size() == end && arena.order().size() == 1);
    assert(arena.order().front() == last.id);
    assert(arena.access_status(penultimate.id) == DrawingAccessStatus::stale);
    assert(arena.access_status(end) == DrawingAccessStatus::invalid);
    assert(pf_line_get_x1(arena, last) == 11);
    arena = checkpoint;
    const auto replayed = drawing_test::line(arena, 14, 2014.0);
    assert(replayed.id == end - 1);
}
