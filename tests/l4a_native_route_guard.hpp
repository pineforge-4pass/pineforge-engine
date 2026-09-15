#pragma once

// Every L4a twin binds the switched source host rather than the deleted
// compatibility owner.  Compiling it against the frozen v16 closure is the
// intentional fail-before witness for the new native-route surface.
#include <pineforge/source/pine_strategy_host.hpp>

#ifndef PINEFORGE_HAS_NATIVE_LOWERING_V1
#error "L4a native-route twins require the v17 native lowering surface"
#endif
