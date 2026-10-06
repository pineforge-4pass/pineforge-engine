// TradingView's per-entry exit tapes as ONE strategy module
// (tests/fixtures/per_entry_exit, test_per_entry_exit_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for it
// (generated.cpp). They share one translation unit and one copy of the
// library: tests/CMakeLists.txt writes per_entry_exit_tapes.inc, which places
// each tape's generated.cpp in a namespace of its own and prefixes its C entry
// points (strategy_create becomes per_entry_exit_<tape>__strategy_create, and
// so on), so the test binds one tape's entry points by prefix. The headers the
// generated files include are included here first; their #pragma once makes
// the includes inside the namespaces no-ops.
//
// The module also exports the switch of the rule part the tapes pin
// (ExitBindingRuleSwitches::global_exit_per_entry_levels) of its own library
// copy, so the test can clear it. Test code only: nothing here is part of the
// library.

#include <pineforge/source/pine_adapter.hpp>
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

extern "C" {

// Sets ExitBindingRuleSwitches::global_exit_per_entry_levels and returns its
// new state.
int per_entry_exit_tapes_set_switch(int on) {
    auto& switches = pineforge::source::detail::exit_binding_rule_switches();
    switches.global_exit_per_entry_levels = on != 0;
    return switches.global_exit_per_entry_levels ? 1 : 0;
}

}  // extern "C"

#include "per_entry_exit_tapes.inc"
