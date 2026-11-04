// SPDX-License-Identifier: Apache-2.0
// Prior-layout metadata is test input; this opaque handle still needs fill_report.
#define PF_LIVE_CONTRACT_NATIVE_STUB 1
#include "test_live_contract_plugins.cpp"

extern "C" PF_API uint32_t strategy_stream_engine_layout_version(void) {
    return 2u;
}
