// TradingView's callback and lifecycle tapes as ONE strategy module
// (tests/fixtures/callback_lifecycle, test_callback_lifecycle_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for it
// (generated.cpp). Built one module per tape, each module linked its own copy
// of the library; here they share one translation unit and one copy:
// tests/CMakeLists.txt writes callback_lifecycle_tapes.inc, which places
// each tape's generated.cpp in a namespace of its own and prefixes its C entry
// points (strategy_create becomes callback_lifecycle_<tape>__strategy_create, and so on), so
// the test binds one tape's entry points by prefix. The headers the generated
// files include are included here first; their #pragma once makes the
// includes inside the namespaces no-ops.
//
// The module also exports the rule switches (src/compat/pine/callback_lifecycle_rules.hpp)
// of its own library copy, so the test can clear each rule in turn. Test
// code only: nothing here is part of the library.

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#if __has_include(<pineforge/checked_settings.hpp>)
#include <pineforge/checked_settings.hpp>
#endif
#include <pineforge/math.hpp>
#include <pineforge/series.hpp>
#include <pineforge/na.hpp>
#include <pineforge/color.hpp>
#include <pineforge/log.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/session_time.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "compat/pine/callback_lifecycle_rules.hpp"

extern "C" {

// Sets one rule switch (rule = PineCallbackLifecycleRule's value); returns its new state,
// or -1 for an unknown rule.
int callback_lifecycle_tapes_set_rule(int rule, int on) {
    using namespace pineforge::source::detail;
    if (rule < 0 || rule >= kPineCallbackLifecycleRuleCount) return -1;
    set_pine_callback_lifecycle_rule(static_cast<PineCallbackLifecycleRule>(rule), on != 0);
    return pine_callback_lifecycle_rule(static_cast<PineCallbackLifecycleRule>(rule)) ? 1 : 0;
}

int callback_lifecycle_tapes_rule_count(void) {
    return pineforge::source::detail::kPineCallbackLifecycleRuleCount;
}

}  // extern "C"

#include "callback_lifecycle_tapes.inc"
