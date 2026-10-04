#include <pineforge/pineforge.h>

#include <cstdint>

extern "C" int confirmed_scan_push_bars(
    pf_strategy_t state, const pf_bar_t* bars, int begin, int count,
    int* failed_index) {
    if (!state || !bars || begin < 0 || count < begin || !failed_index) return -1;
    *failed_index = -1;
    for (int index = begin; index < count; ++index) {
        const int result = strategy_stream_push_bar(state, &bars[index]);
        if (result != 0) {
            *failed_index = index;
            return result;
        }
        strategy_stream_order_actions_clear(state);
    }
    return 0;
}
