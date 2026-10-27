#pragma once

#include <pineforge/engine.hpp>

#include <dlfcn.h>
#include <typeinfo>

// Generated-module capability, deliberately not a runtime export. A weak
// reference also permits a statically linked strategy executable to advertise
// the same capability without exporting its entire symbol table.
#if defined(__APPLE__)
extern "C" int strategy_reads_trade_history()
    __attribute__((weak_import, visibility("default")));
#else
extern "C" int strategy_reads_trade_history()
    __attribute__((weak, visibility("default")));
#endif

namespace pineforge::detail {

// Internal linkage prevents a second module's inline resolver or coalesced
// RTTI from selecting the first module's capability. If the engine lives in a
// separate shared runtime, ownership is unproven and full history is retained.
static constexpr unsigned char strategy_history_policy_image = 0;
static bool strategy_reads_history_by_index(const BacktestEngine& engine) noexcept {
    try {
        // Resolve in the strategy's image, not RTLD_DEFAULT: another module's
        // zero must never authorize retirement of this module's history.
        Dl_info owner{};
        Dl_info runtime{};
        if (::dladdr(&typeid(engine), &owner) == 0 || !owner.dli_fbase || !owner.dli_fname
            || ::dladdr(&strategy_history_policy_image, &runtime) == 0
            || runtime.dli_fbase != owner.dli_fbase)
            return true;
        struct Module {
            void* handle;
            ~Module() { if (handle) ::dlclose(handle); }
        } module{::dlopen(owner.dli_fname, RTLD_LAZY | RTLD_NOLOAD)};
        using Query = int (*)();
        Query query = module.handle ? reinterpret_cast<Query>(
            ::dlsym(module.handle, "strategy_reads_trade_history")) : nullptr;
        if (!query) query = strategy_reads_trade_history;
        if (!query) return true;
        Dl_info symbol{};
        if (::dladdr(reinterpret_cast<const void*>(query), &symbol) == 0
            || symbol.dli_fbase != owner.dli_fbase)
            return true; // includes symbols inherited from a dependency
        return query() != 0;
    } catch (...) {
        return true;
    }
}

} // namespace pineforge::detail
