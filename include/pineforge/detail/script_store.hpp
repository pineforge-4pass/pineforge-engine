#pragma once

#include <pineforge/detail/scratch_backing.hpp>
#include <memory>

namespace pineforge::detail {

// Compatibility owner for map.new() without an explicit run Store scope.
// Uses the existing scratch-file medium selection, with no logical byte cap.
// Run owners should instead bind their one shared o1::Store with MapStoreScope.
std::unique_ptr<ScratchBacking> make_script_scratch_backing();

} // namespace pineforge::detail
