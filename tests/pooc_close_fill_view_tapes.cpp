// TradingView's POOC same-pass position-view tapes as ONE strategy module
// (tests/fixtures/pooc_close_fill_view, test_pooc_close_fill_view_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for it
// (generated.cpp). They share one translation unit and one copy of the
// library: tests/CMakeLists.txt writes pooc_close_fill_view_tapes.inc, which
// places each tape's generated.cpp in a namespace of its own and prefixes its
// C entry points (strategy_create becomes
// pooc_close_fill_view_<tape>__strategy_create, and so on), so the test binds
// one tape's entry points by prefix. The headers the generated files include
// are included here first; their #pragma once makes the includes inside the
// namespaces no-ops.
//
// The module also exports the switches of the rule the tapes pin
// (PoocCloseFillViewSwitches) of its own library copy, so the test can clear
// them. Test code only: nothing here is part of the library.

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

// Sets PoocCloseFillViewSwitches: current_exit_keeps_position_view = view,
// current_exit_keeps_average_price = average. Returns view * 2 + average, the
// switches' new state.
int pooc_close_fill_view_tapes_set_switches(int view, int average) {
    auto& switches = pineforge::source::detail::pooc_close_fill_view_switches();
    switches.current_exit_keeps_position_view = view != 0;
    switches.current_exit_keeps_average_price = average != 0;
    return (switches.current_exit_keeps_position_view ? 2 : 0)
        + (switches.current_exit_keeps_average_price ? 1 : 0);
}

}  // extern "C"

#include "pooc_close_fill_view_tapes.inc"
