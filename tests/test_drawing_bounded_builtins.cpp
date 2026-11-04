#include <pineforge/drawing.hpp>

#include <cassert>
#include <cstdint>
#include <string>

using namespace pineforge;

namespace {

void test_line_builtins_on_bound_arena() {
    DrawingArena<LineRec> arena(drawing_bound(3, 0));
    const Line scalar = pf_line_new(arena, 11, 1.5, 17, 2.5, XLoc::bar_index, true, false);
    assert(scalar.id == 0);
    assert(pf_line_get_x1(arena, scalar) == 11);
    assert(pf_line_get_x2(arena, scalar) == 17);
    assert(pf_line_get_y1(arena, scalar) == 1.5);
    assert(pf_line_get_y2(arena, scalar) == 2.5);
    assert(arena.at(scalar.id).ext_l && !arena.at(scalar.id).ext_r);

    const Line points = pf_line_new_pts(
        arena, ChartPoint{23, 1700000000000LL, 3.25},
        ChartPoint{29, 1700000001000LL, 4.75});
    assert(points.id == 1);
    assert(pf_line_get_x1(arena, points) == 23);
    assert(pf_line_get_x2(arena, points) == 29);
    assert(pf_line_get_y1(arena, points) == 3.25);
    assert(pf_line_get_y2(arena, points) == 4.75);

    const Line copy = pf_line_copy(arena, scalar);
    assert(copy.id == 2);
    assert(pf_line_get_x1(arena, copy) == 11);
    assert(pf_line_get_x2(arena, copy) == 17);
    assert(pf_line_get_y1(arena, copy) == 1.5);
    assert(pf_line_get_y2(arena, copy) == 2.5);
    assert(arena.at(copy.id).xloc == XLoc::bar_index);
    assert(arena.at(copy.id).ext_l && !arena.at(copy.id).ext_r);
    assert(arena.last_allocation_diagnostic().status == DrawingAllocationStatus::ok);
}

void test_box_builtins_on_bound_arena() {
    DrawingArena<BoxRec> arena(drawing_bound(3, 0));
    const Box scalar = pf_box_new(arena, 5, 20.5, 9, 10.25, XLoc::bar_index);
    assert(scalar.id == 0);
    assert(pf_box_get_left(arena, scalar) == 5);
    assert(pf_box_get_right(arena, scalar) == 9);
    assert(pf_box_get_top(arena, scalar) == 20.5);
    assert(pf_box_get_bottom(arena, scalar) == 10.25);

    const Box points = pf_box_new_pts(
        arena, ChartPoint{13, 1700000002000LL, 30.0},
        ChartPoint{19, 1700000003000LL, 12.0});
    assert(points.id == 1);
    assert(pf_box_get_left(arena, points) == 13);
    assert(pf_box_get_right(arena, points) == 19);
    assert(pf_box_get_top(arena, points) == 30.0);
    assert(pf_box_get_bottom(arena, points) == 12.0);

    const Box copy = pf_box_copy(arena, scalar);
    assert(copy.id == 2);
    assert(pf_box_get_left(arena, copy) == 5);
    assert(pf_box_get_right(arena, copy) == 9);
    assert(pf_box_get_top(arena, copy) == 20.5);
    assert(pf_box_get_bottom(arena, copy) == 10.25);
    assert(arena.at(copy.id).xloc == XLoc::bar_index);
    assert(arena.last_allocation_diagnostic().status == DrawingAllocationStatus::ok);
}

void test_label_builtins_on_bound_arena() {
    DrawingArena<LabelRec> arena(drawing_bound(3, 0));
    const Label scalar = pf_label_new(
        arena, 31, 41.5, std::string("text\0with bytes", 15),
        XLoc::bar_time, YLoc::belowbar);
    assert(scalar.id == 0);
    assert(pf_label_get_x(arena, scalar) == 31);
    assert(pf_label_get_y(arena, scalar) == 41.5);
    assert(pf_label_get_text(arena, scalar) == std::string("text\0with bytes", 15));
    assert(arena.at(scalar.id).xloc == XLoc::bar_time);
    assert(arena.at(scalar.id).yloc == YLoc::belowbar);

    const Label point = pf_label_new_pt(
        arena, ChartPoint{na<std::int64_t>(), 1700000004000LL, 51.25},
        "point text", YLoc::abovebar);
    assert(point.id == 1);
    assert(pf_label_get_x(arena, point) == 1700000004000LL);
    assert(pf_label_get_y(arena, point) == 51.25);
    assert(pf_label_get_text(arena, point) == "point text");
    assert(arena.at(point.id).xloc == XLoc::bar_time);
    assert(arena.at(point.id).yloc == YLoc::abovebar);

    const Label copy = pf_label_copy(arena, scalar);
    assert(copy.id == 2);
    assert(pf_label_get_x(arena, copy) == 31);
    assert(pf_label_get_y(arena, copy) == 41.5);
    assert(pf_label_get_text(arena, copy) == std::string("text\0with bytes", 15));
    assert(arena.at(copy.id).xloc == XLoc::bar_time);
    assert(arena.at(copy.id).yloc == YLoc::belowbar);
    assert(arena.last_allocation_diagnostic().status == DrawingAllocationStatus::ok);
}

} // namespace

int main() {
    test_line_builtins_on_bound_arena();
    test_box_builtins_on_bound_arena();
    test_label_builtins_on_bound_arena();
    return 0;
}
