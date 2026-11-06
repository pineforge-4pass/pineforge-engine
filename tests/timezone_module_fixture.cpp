#include <pineforge/session_time.hpp>

#include <string>

extern "C" __attribute__((visibility("default")))
int module_local_hour(long long ms, const char* tz) {
    return pineforge::local_hour(ms, std::string(tz));
}

// Whole-archive strategy modules provide this metadata export.
extern "C" __attribute__((visibility("default")))
int strategy_reads_trade_history() {
    return 0;
}
