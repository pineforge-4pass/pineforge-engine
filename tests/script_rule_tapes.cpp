// TradingView's explicit-quantity and close-time bar-fill tapes as ONE strategy
// module (tests/fixtures/explicit_qty_floor, tests/fixtures/pooc_close_bar_fills,
// test_script_rule_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for its
// strategy.pine (generated.cpp), so the test runs the product path: the
// generated code, the library and scripts/run_strategy.py's configuration.
// tests/CMakeLists.txt writes script_rule_tapes.inc, which places each tape's
// generated.cpp in a namespace of its own and prefixes its C entry points
// (strategy_create becomes script_rule_<root>_<tape>__strategy_create, and so
// on), so the test binds one tape's entry points by prefix. The headers the
// generated files include are included here first; their #pragma once makes
// the includes inside the namespaces no-ops.
//
// The module also exports the ScriptRuleSwitches of its own library copy, so
// the test can turn each switch off in turn. Test code only: nothing here is
// part of the library.

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_adapter.hpp>
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

extern "C" {

// Sets one switch (0 explicit_qty_decimal_floor, 1 equity_tick_mark,
// 2 pooc_bracket_skips_inert_exits); returns its new state, or -1 for an
// unknown switch.
int script_rule_tapes_set_switch(int which, int on) {
    auto& switches = pineforge::source::detail::script_rule_switches();
    bool* field = which == 0 ? &switches.explicit_qty_decimal_floor
                : which == 1 ? &switches.equity_tick_mark
                : which == 2 ? &switches.pooc_bracket_skips_inert_exits
                             : nullptr;
    if (field == nullptr) return -1;
    *field = on != 0;
    return *field ? 1 : 0;
}

int script_rule_tapes_switch_count(void) { return 3; }

}  // extern "C"

#include "script_rule_tapes.inc"
