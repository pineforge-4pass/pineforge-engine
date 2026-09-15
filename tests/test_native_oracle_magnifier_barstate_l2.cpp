// Exact native-route twin of the L0 magnifier/barstate oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>
#include "oracle_fixture_accessors.hpp"

#define PineStrategyHost OraclePineNativeHost
#define is_first_tick_ is_first_tick()
#include "oracle/test_oracle_magnifier_barstate.cpp"
#undef is_first_tick_
#undef PineStrategyHost
