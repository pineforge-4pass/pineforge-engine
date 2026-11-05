#include "drawing_slot_test.hpp"

using namespace pineforge;

int main() {
    // Independent expectations are the observed sampled maxima: 15 at a
    // label declaration of 10; 10 at line and box declarations of 5.
    const auto labels = drawing_bound_from_declared_count<LabelRec>(10);
    const auto lines = drawing_bound_from_declared_count<LineRec>(5);
    const auto boxes = drawing_bound_from_declared_count<BoxRec>(5);
    assert(labels.slot_bound == 15);
    assert(lines.slot_bound == 10 && boxes.slot_bound == 10);
    // No declaration for fills was present in that observation.
    assert(drawing_bound_from_linefill_sample().slot_bound == 10);
    assert(drawing_bound_from_linefill_sample(12).slot_bound == 12);
    // The omitted declaration uses the existing count default (50), and the
    // configured default extends it by the measured difference 15 - 10.
    assert(drawing_bound_from_declared_count<LabelRec>().slot_bound == 50 + (15 - 10));
    assert(drawing_bound_from_declared_count<LabelRec>(20).slot_bound == 20 + (15 - 10));
    assert(drawing_bound_from_declared_count<LabelRec>(10, 7).pending_creation_slots == 7);
    assert(labels.pending_creation_slots == 1); // one creation per try_alloc call
    bool rejected = false;
    try {
        (void)drawing_bound_from_declared_count<LabelRec>(
            std::numeric_limits<std::int32_t>::max());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
