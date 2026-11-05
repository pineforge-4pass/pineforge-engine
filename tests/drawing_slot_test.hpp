#pragma once

#include <pineforge/drawing.hpp>
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>

namespace pineforge::detail {

// Fixture access supplies a reachable empty, previously issued domain without
// performing billions of creations.  Production ID checks remain unchanged.
struct DrawingArenaTestAccess {
    template <class Rec>
    static void issued_empty_domain(DrawingArena<Rec>& arena, std::int64_t next) {
        assert(arena.bound_ && arena.bound_->directory.empty());
        assert(next >= 0 && next <= std::numeric_limits<std::int32_t>::max());
        auto state = std::make_shared<BoundDrawingState<Rec>>(*arena.bound_);
        state->next_id = next;
        arena.bound_.swap(state);
    }

    template <class Rec>
    static std::size_t slots(const DrawingArena<Rec>& arena) {
        assert(arena.bound_);
        return arena.bound_->slots.size();
    }

    template <class Rec>
    static std::int32_t slot_for(const DrawingArena<Rec>& arena, std::int32_t id) {
        assert(arena.bound_);
        return arena.bound_->slot_for(id);
    }
};

} // namespace pineforge::detail

namespace drawing_test {

template <class Handle, class Rec>
Handle create(pineforge::DrawingArena<Rec>& arena, Rec record) {
    const auto result = arena.try_alloc(std::move(record));
    assert(result.diagnostic.status == pineforge::DrawingAllocationStatus::ok);
    assert(result.id.has_value());
    return Handle{*result.id};
}

inline pineforge::Label label(pineforge::DrawingArena<pineforge::LabelRec>& arena,
                              std::int64_t x, double y, std::string text) {
    return create<pineforge::Label>(arena, pineforge::LabelRec{
        x, y, pineforge::XLoc::bar_index, pineforge::YLoc::price, std::move(text), true});
}

inline pineforge::Line line(pineforge::DrawingArena<pineforge::LineRec>& arena,
                            std::int64_t x, double y,
                            pineforge::XLoc xloc = pineforge::XLoc::bar_index) {
    return create<pineforge::Line>(arena, pineforge::LineRec{
        x, x + 1, y, y + 1, xloc, false, false, true});
}

} // namespace drawing_test
