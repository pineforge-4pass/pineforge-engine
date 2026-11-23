#pragma once

#include <pineforge/engine.hpp>

#include <dlfcn.h>
#include <typeinfo>

// Generated exports are optional. The weak reference also works for a
// statically linked strategy executable without an exported symbol table.
#if defined(__APPLE__)
#define PF_WEAK_STRATEGY_EXPORT __attribute__((weak_import, visibility("default")))
#else
#define PF_WEAK_STRATEGY_EXPORT __attribute__((weak, visibility("default")))
#endif

namespace pineforge::detail {

// The live strategy owns the image for the lifetime of the returned function.
// Never take another loaded strategy's declaration, including a symbol that
// dlsym found in a dependency. A separate runtime or coalesced RTTI is not
// proof of ownership. Internal linkage keeps the image marker local.
static constexpr unsigned char strategy_module_image = 0;
template<class Query>
static Query strategy_module_symbol(const BacktestEngine& engine, const char* name,
                                    Query linked) noexcept {
    Dl_info owner{}, runtime{}, symbol{};
    if (!::dladdr(&typeid(engine), &owner) || !owner.dli_fbase || !owner.dli_fname ||
        !::dladdr(&strategy_module_image, &runtime) || runtime.dli_fbase != owner.dli_fbase)
        return nullptr;
    void* module = ::dlopen(owner.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    Query query = module ? reinterpret_cast<Query>(::dlsym(module, name)) : nullptr;
    if (module) ::dlclose(module);
    if (!query) query = linked;
    return query && ::dladdr(reinterpret_cast<const void*>(query), &symbol) &&
           symbol.dli_fbase == owner.dli_fbase ? query : nullptr;
}

} // namespace pineforge::detail
