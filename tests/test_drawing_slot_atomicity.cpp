#include "global_allocation_replacement.hpp"
#include "drawing_slot_test.hpp"

#include <string>

using namespace pineforge;

namespace {

void unchanged(const DrawingArena<LabelRec>& arena) {
    assert(arena.size() == 2);
    assert(arena.order().size() == 2);
    assert(arena.order()[0] == 0 && arena.order()[1] == 1);
    assert(arena.access_status(0) == DrawingAccessStatus::present);
    assert(arena.access_status(1) == DrawingAccessStatus::present);
    assert(arena.at(0).x == 10 && arena.at(0).y == 1010.0);
    assert(arena.at(0).text == std::string(257, 'a'));
    assert(arena.at(1).x == 20 && arena.at(1).y == 1020.0);
    assert(arena.at(1).text == std::string(513, 'b'));
}

void apply(DrawingArena<LabelRec>& arena, int operation, LabelRec record,
           std::string replacement) {
    if (operation == 0) {
        const auto result = arena.try_alloc(std::move(record));
        assert(result.diagnostic.status == DrawingAllocationStatus::ok && result.id);
    } else if (operation == 1) {
        pf_label_delete(arena, Label{0});
    } else {
        pf_label_set_text(arena, Label{1}, std::move(replacement));
    }
}

void fault_positions(int operation) {
    // Enumerate actual allocation sites through the first nonfaulted
    // completion.  Every expected value below is from these fixed inputs.
    for (std::size_t fail_position = 0;; ++fail_position) {
        DrawingArena<LabelRec> arena(drawing_bound(3, 0));
        drawing_test::label(arena, 10, 1010.0, std::string(257, 'a'));
        drawing_test::label(arena, 20, 1020.0, std::string(513, 'b'));
        const DrawingArena<LabelRec> checkpoint = arena;
        const LabelRec& borrowed = std::as_const(arena).at(0);
        LabelRec record{30, 1030.0, XLoc::bar_index, YLoc::price,
                        std::string(1025, 'c'), true};
        std::string replacement(2049, 'd');
        bool refused = false;
        global_allocation::fail_after = fail_position;
        try {
            apply(arena, operation, std::move(record), std::move(replacement));
        } catch (const std::bad_alloc&) {
            refused = true;
        }
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        unchanged(checkpoint);
        if (!refused) {
            if (operation == 0) {
                assert(arena.size() == 3 && arena.at(2).text == std::string(1025, 'c'));
            } else if (operation == 1) {
                assert(arena.access_status(0) == DrawingAccessStatus::stale);
                assert(arena.order().size() == 1 && arena.order().front() == 1);
            } else {
                assert(arena.at(1).text == std::string(2049, 'd'));
            }
            assert(fail_position > 0); // real storage operations were faulted
            break;
        }
        unchanged(arena);
        assert(borrowed.x == 10 && borrowed.text == std::string(257, 'a'));
        // A real failure leaves the arena usable.
        pf_label_set_y(arena, Label{1}, 9000.0);
        assert(pf_label_get_y(arena, Label{1}) == 9000.0);
    }
}

} // namespace

int main() {
    fault_positions(0);
    fault_positions(1);
    fault_positions(2);
}
