// SPDX-License-Identifier: Apache-2.0
// Prior-release control: the engine layout stamp a module built against 1.13.x
// headers declares. The runner must refuse it before strategy_create; this
// opaque handle still needs fill_report so that only the stamp can refuse it.
#define PF_LIVE_CONTRACT_NATIVE_STUB 1
#include "test_live_contract_plugins.cpp"

extern "C" PF_API uint32_t strategy_stream_engine_layout_version(void) {
    return 4u;
}
