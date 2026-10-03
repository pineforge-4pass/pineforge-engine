// TradingView's N6 callback and lifecycle tapes as ONE strategy module
// (tests/fixtures/n6_callback_lifecycle, test_n6_callback_lifecycle_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for it
// (generated.cpp). Built one module per tape, each module linked its own copy
// of the library; here they share one translation unit and one copy:
// tests/CMakeLists.txt writes n6_callback_lifecycle_tapes.inc, which places
// each tape's generated.cpp in a namespace of its own and prefixes its C entry
// points (strategy_create becomes n6_<tape>__strategy_create, and so on), so
// the test binds one tape's entry points by prefix. The headers the generated
// files include are included here first; their #pragma once makes the
// includes inside the namespaces no-ops.
//
// The module also exports the N6 rule switches (src/source/pine_n6_rules.hpp)
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

#include "source/pine_n6_rules.hpp"

extern "C" {

// Sets one N6 rule switch (rule = PineN6Rule's value); returns its new state,
// or -1 for an unknown rule.
int n6_tapes_set_rule(int rule, int on) {
    using namespace pineforge::source::detail;
    if (rule < 0 || rule >= kPineN6RuleCount) return -1;
    set_pine_n6_rule(static_cast<PineN6Rule>(rule), on != 0);
    return pine_n6_rule(static_cast<PineN6Rule>(rule)) ? 1 : 0;
}

int n6_tapes_rule_count(void) {
    return pineforge::source::detail::kPineN6RuleCount;
}

}  // extern "C"

#include "n6_callback_lifecycle_tapes.inc"
