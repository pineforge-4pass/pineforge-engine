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

void test_checkpoint_restores_directory_and_counter() {
    DrawingArena<LabelRec> arena(drawing_bound(2));
    const Label first = drawing_test::label(arena, 10, 1010.0, "first");
    const Label second = drawing_test::label(arena, 20, 1020.0, "second");
    const DrawingArena<LabelRec> checkpoint = arena;

    pf_label_delete(arena, first);
    const Label speculative = drawing_test::label(arena, 30, 1030.0, "speculative");
    CHECK(speculative.id == 2);
    CHECK(arena.access_status(first.id) == DrawingAccessStatus::stale);
    CHECK(arena.access_status(speculative.id) == DrawingAccessStatus::present);
    pf_label_set_text(arena, second, "changed");

    arena = checkpoint;
    CHECK(arena.size() == 2);
    CHECK(arena.access_status(first.id) == DrawingAccessStatus::present);
    CHECK(arena.access_status(second.id) == DrawingAccessStatus::present);
    CHECK(arena.access_status(speculative.id) == DrawingAccessStatus::invalid);
    CHECK(pf_label_get_text(arena, first) == "first");
    CHECK(pf_label_get_text(arena, second) == "second");
    CHECK(arena.order().size() == 2u);
    CHECK(arena.order()[0] == first.id);
    CHECK(arena.order()[1] == second.id);

    pf_label_delete(arena, first);
    const Label after_restore = drawing_test::label(arena, 40, 1040.0, "after restore");
    CHECK(after_restore.id == speculative.id);
    CHECK(arena.access_status(after_restore.id) == DrawingAccessStatus::present);

    pf_label_delete(arena, second);
    const Label after_reuse = drawing_test::label(arena, 50, 1050.0, "reused slot");
    CHECK(after_reuse.id == 3);
    CHECK(arena.access_status(second.id) == DrawingAccessStatus::stale);
    CHECK(arena.access_status(after_reuse.id) == DrawingAccessStatus::present);
    CHECK(pf_label_get_text(arena, after_restore) == "after restore");
    CHECK(checkpoint.access_status(first.id) == DrawingAccessStatus::present);
    CHECK(checkpoint.at(first.id).text == "first");
}

} // namespace

int main() {
    test_checkpoint_restores_directory_and_counter();
    if (failures != 0) {
        std::printf("drawing slot checkpoint tests: %d FAILED\n", failures);
        return 1;
    }
    std::puts("drawing slot checkpoint tests: all passed");
    return 0;
}
