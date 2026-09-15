// Exact native-route twin of the L0 COOF cascade oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_coof.cpp"
#undef signed_position_size
#undef PineStrategyHost
