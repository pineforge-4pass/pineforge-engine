// TradingView's calc_on_order_fills competing-order tapes as ONE strategy
// module (tests/fixtures/coof_competing_tick, test_coof_competing_tick_tapes).
//
// Every tape keeps the frozen generated strategy codegen emitted for it
// (generated.cpp). They share one translation unit and one copy of the
// library: tests/CMakeLists.txt writes coof_competing_tick_tapes.inc, which
// places each tape's generated.cpp in a namespace of its own and prefixes its C
// entry points (strategy_create becomes coof_competing_tick_<tape>__
// strategy_create, and so on), so the test binds one tape's entry points by
// prefix. The headers the generated files include are included here first;
// their #pragma once makes the includes inside the namespaces no-ops. Test
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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "coof_competing_tick_tapes.inc"
