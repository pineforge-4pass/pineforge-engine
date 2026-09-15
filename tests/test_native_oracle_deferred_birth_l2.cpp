// Exact native-route twin of the L0 deferred-birth oracle.  Pre-including the
// legacy declaration keeps its include guard closed; the scenario itself is
// then instantiated on the separately named L2 fixture host.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
#include "oracle/test_oracle_deferred_birth.cpp"
#undef signed_position_size
#undef PineStrategyHost
