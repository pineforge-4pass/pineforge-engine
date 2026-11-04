#include "drawing_slot_test.hpp"

#include <exception>
#include <stdexcept>
#include <string>

using namespace pineforge;

namespace {

// What a run reads from a thrown drawing failure: the code and canonical args
// its classification reports, and the exception's English.
struct Reported {
    RunFailureCode code;
    std::string args;
    std::string what;
};

Reported reported(const std::exception& error) {
    const auto failure = classify_run_failure(error);
    assert(failure.args);
    return {failure.code, *failure.args, error.what()};
}

// An unbound arena reaches its own id exhaustion only after 2^31 live records,
// so the two alloc() sites cannot be driven through an arena here; they raise
// through this one helper, whose report must name the kind it was given (the
// bound path, which is reachable, is read through real arenas in
// test_drawing_bounded_creation_failure).
void test_id_exhaustion_helper_reports_its_kind() {
    const std::string max = std::to_string(std::numeric_limits<std::int32_t>::max());
    for (const char* object : {"line", "box", "label", "linefill", "generic"}) {
        bool caught = false;
        try {
            detail::drawing_id_exhausted_error(object);
        } catch (const pine_drawing_id_exhausted& error) {
            caught = true;
            const Reported got = reported(error);
            assert(got.code == RunFailureCode::pine_runtime_limit);
            assert(got.args == "{\"limit\":\"drawing_ids\",\"max\":" + max + "}");
            assert(got.what == std::string(object) + " drawing ID domain exhausted");
        }
        assert(caught);
    }
}

// A strategy() header count a script wrote, outside the int32 domain the slot
// bound is computed in, is a rejected setting of that kind's field; a count
// inside it, up to the largest one the observed slack of 5 still fits, is
// accepted with that bound.  The edges are int32 arithmetic, not values read
// back from the header under test.
template <class Rec>
void check_declared_count_domain(const char* field) {
    constexpr std::int32_t top = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t slack = 5;
    for (const std::int32_t declared : {top, top - slack + 1, top - 1, std::int32_t{-1},
                                        std::numeric_limits<std::int32_t>::min()}) {
        bool caught = false;
        try {
            (void)drawing_bound_from_declared_count<Rec>(declared);
        } catch (const std::invalid_argument& error) {
            caught = true;
            const Reported got = reported(error);
            assert(got.code == RunFailureCode::strategy_settings_rejected);
            assert(got.args == std::string("{\"field\":\"") + field + "\"}");
            assert(got.what == "drawing declared count is outside the int32 domain");
        }
        assert(caught);
    }
    for (const std::int32_t declared : {0, 1, 50, top - slack}) {
        assert(drawing_bound_from_declared_count<Rec>(declared).slot_bound == declared + slack);
    }
}

void test_declared_count_domain_names_its_field() {
    check_declared_count_domain<LineRec>("max_lines_count");
    check_declared_count_domain<BoxRec>("max_boxes_count");
    check_declared_count_domain<LabelRec>("max_labels_count");
}

} // namespace

int main() {
    test_id_exhaustion_helper_reports_its_kind();
    test_declared_count_domain_names_its_field();

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
