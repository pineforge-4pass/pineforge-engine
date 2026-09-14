// Exact native-route twin of the complete L0 frozen-size oracle. The sole
// legacy id-ledger observation is projected from the adapter's live cohort.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_native_host.hpp>

#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#define id_unclosed_qty_ source_id_ledger_view()
#include "oracle/test_oracle_frozen_size.cpp"
#undef id_unclosed_qty_
#undef signed_position_size
#undef PineStrategyHost
