// Exact native-route twin of the complete L0 percent ShortSeed oracle.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#define pending_orders_ source_pending_view()
#include "oracle/test_oracle_short_seed_percent.cpp"
#undef pending_orders_
#undef signed_position_size
#undef PineStrategyHost
