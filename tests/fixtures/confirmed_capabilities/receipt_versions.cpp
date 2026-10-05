#if defined(PF_RECEIPT_request)
#define strategy_confirmed_bar_api_version original_confirmed_version
#define strategy_confirmed_bar_receipt original_confirmed_receipt
#include "generated/htf5_close.cpp"
#undef strategy_confirmed_bar_api_version
#undef strategy_confirmed_bar_receipt
#elif defined(PF_RECEIPT_pooc)
#define strategy_confirmed_bar_api_version original_confirmed_version
#define strategy_confirmed_bar_receipt original_confirmed_receipt
#include "generated/pooc_market.cpp"
#undef strategy_confirmed_bar_api_version
#undef strategy_confirmed_bar_receipt
#else
#include "../strategy_capabilities/generated/close.cpp"
#endif

extern "C" {
#if !defined(PF_RECEIPT_missing)
uint32_t strategy_confirmed_bar_api_version(void) { return 2u; }
#endif
int strategy_confirmed_bar_receipt(void*, char*, size_t, size_t*, char*, size_t) {
    return PF_SETTINGS_INVALID_ARGUMENT;
}
}
