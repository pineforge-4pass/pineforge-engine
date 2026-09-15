// Exact native-route twin of the L0 source day-key oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>
#include "oracle_fixture_accessors.hpp"

#define PineStrategyHost OraclePineNativeHost
#define signed_position_size oracle_script_position_size
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_day_key.cpp"
#undef signed_position_size
#undef PineStrategyHost
