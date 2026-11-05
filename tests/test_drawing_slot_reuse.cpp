#include "drawing_slot_test.hpp"

#include <cstdio>

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

void test_reuse_keeps_logical_ids() {
    DrawingArena<LineRec> arena(drawing_bound(1, 0));
    const Line first = drawing_test::line(arena, 10, 1010.0);
    const auto first_slot = detail::DrawingArenaTestAccess::slot_for(arena, first.id);
    CHECK(first.id == 0 && first_slot == 0);

    pf_line_delete(arena, first);
    CHECK(arena.access_status(first.id) == DrawingAccessStatus::stale);
    const Line replacement = drawing_test::line(arena, 30, 1030.0);
    CHECK(replacement.id == first.id + 1);
    CHECK(arena.access_status(replacement.id) == DrawingAccessStatus::present);
    CHECK(arena.occupied_slots() == 1);
    CHECK(arena.slot_bound() == 1);
    CHECK(detail::DrawingArenaTestAccess::slots(arena) == 1);
    CHECK(detail::DrawingArenaTestAccess::slot_for(arena, replacement.id) == first_slot);
    CHECK(pf_line_get_x1(arena, replacement) == 30);

    pf_line_set_x1(arena, first, 999);
    pf_line_delete(arena, first);
    CHECK(arena.order().size() == 1 && arena.order().front() == replacement.id);
    CHECK(pf_line_get_x1(arena, replacement) == 30);
    CHECK(arena.access_status(first.id) == DrawingAccessStatus::stale);
    Line prior = replacement;
    for (int i = 0; i < 128; ++i) {
        pf_line_delete(arena, prior);
        const Line next = drawing_test::line(arena, 50 + i, 1050.0 + i);
        CHECK(next.id == prior.id + 1);
        CHECK(arena.access_status(prior.id) == DrawingAccessStatus::stale);
        CHECK(is_na(pf_line_get_x1(arena, prior)));
        CHECK(detail::DrawingArenaTestAccess::slot_for(arena, next.id) == first_slot);
        CHECK(arena.order().size() == 1 && arena.order().front() == next.id);
        CHECK(pf_line_get_x1(arena, next) == 50 + i);
        prior = next;
    }
}

} // namespace

int main() {
    test_reuse_keeps_logical_ids();
    if (failures != 0) {
        std::printf("drawing slot reuse tests: %d FAILED\n", failures);
        return 1;
    }
    std::puts("drawing slot reuse tests: all passed");
    return 0;
}
