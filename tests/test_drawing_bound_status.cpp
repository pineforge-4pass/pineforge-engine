#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"
#include "drawing_slot_test.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <class F>
void check_no_throw(F&& operation, const char* expression) {
    try {
        operation();
    } catch (...) {
        std::printf("FAIL %s:%d: unexpected throw: %s\n",
                    __FILE__, __LINE__, expression);
        ++failures;
    }
}

void test_stale_status_and_sink() {
    DrawingArena<LabelRec> arena(drawing_bound(2, 3));
    const Label old = drawing_test::label(arena, 10, 1010.0, "old");
    const Label live = drawing_test::label(arena, 20, 1020.0, "live");
    pf_label_delete(arena, old);

    CHECK(arena.access_status(old.id) == DrawingAccessStatus::stale);
    CHECK(arena.access_status(live.id) == DrawingAccessStatus::present);
    CHECK(arena.access_status(-1) == DrawingAccessStatus::invalid);
    CHECK(arena.access_status(arena.size()) == DrawingAccessStatus::invalid);

    LabelRec& sink = arena.at(old.id);
    sink.x = 777;
    sink.y = 777.0;
    sink.text.assign("stale\0payload", 13);
    sink.xloc = XLoc::bar_time;
    sink.yloc = YLoc::belowbar;
    sink.alive = true;

    const LabelRec& missing = std::as_const(arena).at(old.id);
    CHECK(missing.x == na<std::int64_t>());
    CHECK(is_na(missing.y));
    CHECK(missing.text.empty());
    CHECK(static_cast<int>(missing.xloc) == -1);
    CHECK(static_cast<int>(missing.yloc) == -1);
    CHECK(!missing.alive);
    CHECK(&missing != &sink);

    const LabelRec& helper_missing = pf_require_live(std::as_const(arena), old);
    CHECK(&helper_missing == &missing);
    LabelRec& helper_sink = pf_require_live(arena, old);
    CHECK(&helper_sink == &sink);
    CHECK(helper_sink.x == na<std::int64_t>());
    CHECK(is_na(helper_sink.y));
    CHECK(helper_sink.text.empty());
    CHECK(!helper_sink.alive);

    check_no_throw([&] { pf_label_set_x(arena, old, 1); }, "pf_label_set_x(arena, old, 1)");
    check_no_throw([&] { pf_label_set_y(arena, old, 2.0); }, "pf_label_set_y(arena, old, 2.0)");
    check_no_throw([&] { pf_label_set_text(arena, old, "ignored"); },
                   "pf_label_set_text(arena, old, \"ignored\")");
    check_no_throw([&] { (void)pf_label_get_x(arena, old); },
                   "pf_label_get_x(arena, old)");
    CHECK(is_na(pf_label_get_x(arena, old)));
    CHECK(is_na(pf_label_get_y(arena, old)));
    CHECK(pf_label_get_text(arena, old).empty());
    CHECK(pf_label_get_x(arena, live) == 20);
    CHECK(pf_label_get_y(arena, live) == 1020.0);
    CHECK(pf_label_get_text(arena, live) == "live");

    // Raw alive=false remains an occupied record, independently of builtin
    // missingness.  erase remains a no-op for that flag-only mutation.
    arena.at(live.id).alive = false;
    CHECK(arena.access_status(live.id) == DrawingAccessStatus::present);
    CHECK(is_na(pf_label_get_x(arena, live)));
    pf_label_delete(arena, live);
    CHECK(arena.occupied_slots() == 1);
    CHECK(arena.order().size() == 1 && arena.order().front() == live.id);
    arena.at(live.id).alive = true;
    CHECK(pf_label_get_x(arena, live) == 20);

    // Releasing the sink interval actually releases out-of-line text, rather
    // than retaining its capacity through clear().
    global_allocation::recording = true;
    arena.at(old.id).text.assign(4096, 'z');
    CHECK(global_allocation::live_bytes > 0);
    (void)arena.at(old.id);
    CHECK(global_allocation::live_bytes == 0);
    global_allocation::recording = false;

    global_allocation::recording = true;
    arena.at(old.id).text.assign(4096, 'c');
    global_allocation::recording = false;
    DrawingArena<LabelRec> copy = arena;
    CHECK(global_allocation::live_bytes == 0); // source copy boundary
    LabelRec& copy_sink = copy.at(old.id);
    LabelRec& arena_sink = arena.at(old.id);
    CHECK(&copy_sink != &arena_sink);
    copy_sink.text = "copy sink";
    arena_sink.text = "arena sink";
    CHECK(copy_sink.text == "copy sink");
    CHECK(arena_sink.text == "arena sink");

    const LabelRec& copy_na = std::as_const(copy).at(old.id);
    const LabelRec& arena_na = std::as_const(arena).at(old.id);
    CHECK(&copy_na == &arena_na);
    CHECK(&copy_na != &copy_sink);
    CHECK(copy_na.text.empty() && arena_na.text.empty());

    global_allocation::recording = true;
    arena.at(old.id).text.assign(4096, 'a');
    copy.at(old.id).text.assign(4096, 'b');
    global_allocation::recording = false;
    copy = arena;
    CHECK(global_allocation::live_bytes == 0); // both assignment boundaries

    global_allocation::recording = true;
    arena.at(old.id).text.assign(4096, 's');
    global_allocation::recording = false;
    DrawingArena<LabelRec>& alias = arena;
    alias = alias;
    CHECK(global_allocation::live_bytes == 0);

    global_allocation::recording = true;
    copy.at(old.id).text.assign(4096, 'm');
    global_allocation::recording = false;
    DrawingArena<LabelRec> moved = std::move(copy);
    CHECK(global_allocation::live_bytes == 0);
    CHECK(copy.is_bound() && copy.size() == 0 && copy.order().empty());
    CHECK(pf_label_get_x(moved, live) == 20);
    const auto reinitialized = drawing_test::label(copy, 30, 1030.0, "fresh source");
    CHECK(reinitialized.id == 0 && pf_label_get_x(copy, reinitialized) == 30);

    bool invalid_threw = false;
    try {
        (void)arena.at(-1);
    } catch (const pine_drawing_error&) {
        invalid_threw = true;
    }
    CHECK(invalid_threw);

    bool escaped = false;
    const auto allocations_before = global_allocation::allocations;
    global_allocation::refusing = true;
    try { (void)arena.at(old.id); } catch (...) { escaped = true; }
    try { (void)std::as_const(arena).at(old.id); } catch (...) { escaped = true; }
    try { (void)pf_require_live(arena, old); } catch (...) { escaped = true; }
    try { (void)pf_require_live(std::as_const(arena), old); } catch (...) { escaped = true; }
    try { (void)pf_label_get_text(arena, old); } catch (...) { escaped = true; }
    global_allocation::refusing = false;
    CHECK(!escaped);
    CHECK(global_allocation::allocations == allocations_before);
}

void test_stale_geometry_is_typed() {
    DrawingArena<LineRec> lines(drawing_bound(1));
    const Line line = drawing_test::line(lines, 10, 10.0, XLoc::bar_time);
    pf_line_delete(lines, line);
    CHECK(lines.access_status(line.id) == DrawingAccessStatus::stale);
    CHECK(is_na(pf_line_get_x1(lines, line)));
    CHECK(is_na(pf_line_get_x2(lines, line)));
    CHECK(is_na(pf_line_get_y1(lines, line)));
    CHECK(is_na(pf_line_get_y2(lines, line)));
    CHECK(is_na(pf_line_get_price(lines, line, 10)));
    const auto& absent_line = std::as_const(lines).at(line.id);
    CHECK(absent_line.x1 == INT64_MIN && absent_line.x2 == INT64_MIN);
    CHECK(std::isnan(absent_line.y1) && std::isnan(absent_line.y2));
    CHECK(static_cast<int>(absent_line.xloc) == -1);
    CHECK(!absent_line.ext_l && !absent_line.ext_r && !absent_line.alive);
    check_no_throw([&] { pf_line_set_x1(lines, line, 99); },
                   "pf_line_set_x1(lines, line, 99)");
    CHECK(is_na(pf_line_copy(lines, line)));

    DrawingArena<BoxRec> boxes(drawing_bound(1));
    const Box box = drawing_test::create<Box>(
        boxes, BoxRec{1, 3, 2.0, 4.0, XLoc::bar_index, true});
    pf_box_delete(boxes, box);
    CHECK(boxes.access_status(box.id) == DrawingAccessStatus::stale);
    CHECK(is_na(pf_box_get_left(boxes, box)));
    CHECK(is_na(pf_box_get_right(boxes, box)));
    CHECK(is_na(pf_box_get_top(boxes, box)));
    CHECK(is_na(pf_box_get_bottom(boxes, box)));
    const auto& absent_box = std::as_const(boxes).at(box.id);
    CHECK(absent_box.left == INT64_MIN && absent_box.right == INT64_MIN);
    CHECK(std::isnan(absent_box.top) && std::isnan(absent_box.bottom));
    CHECK(static_cast<int>(absent_box.xloc) == -1 && !absent_box.alive);
    check_no_throw([&] { pf_box_set_left(boxes, box, 99); },
                   "pf_box_set_left(boxes, box, 99)");
    CHECK(is_na(pf_box_copy(boxes, box)));
}

} // namespace

int main() {
    test_stale_status_and_sink();
    test_stale_geometry_is_typed();
    if (failures != 0) {
        std::printf("drawing bound status tests: %d FAILED\n", failures);
        return 1;
    }
    std::puts("drawing bound status tests: all passed");
    return 0;
}
