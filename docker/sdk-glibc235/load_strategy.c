/* Actual module loading only: no bars, backtest, or replacement strategy. */
#define _POSIX_C_SOURCE 200809L
#include <pineforge/pineforge.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static void *required(void *module, const char *name) {
    const char *error;
    void *symbol;
    dlerror();
    symbol = dlsym(module, name);
    error = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "dlsym failed: %s: %s\n", name,
                error != NULL ? error : "null symbol");
        return NULL;
    }
    return symbol;
}

int main(int argc, char **argv) {
    static const char *const exports[] = {
        "strategy_create", "strategy_set_input", "strategy_set_override",
        "run_backtest_full", "strategy_free", "report_free"
    };
    int (*abi_version)(void);
    pf_strategy_t (*create)(const char *);
    void (*destroy)(pf_strategy_t);
    void *module;
    void *symbol;
    pf_strategy_t instance;
    int actual;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: load_strategy module.so\n");
        return 2;
    }
    printf("{\"event\":\"loader_start\",\"expected_abi\":%d}\n", PF_ABI_VERSION);
    if (fflush(stdout) != 0) return 3;
    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 10;
    }
    symbol = required(module, "pf_abi_version");
    if (symbol == NULL) return 11;
    /* POSIX dlsym function addresses; memcpy avoids a C aliasing violation. */
    _Static_assert(sizeof(abi_version) == sizeof(symbol), "dlsym pointer size");
    memcpy(&abi_version, &symbol, sizeof(abi_version));
    actual = abi_version();
    printf("{\"event\":\"abi\",\"expected\":%d,\"actual\":%d}\n",
           PF_ABI_VERSION, actual);
    if (actual != PF_ABI_VERSION) {
        fprintf(stderr, "ABI mismatch: actual=%d expected=%d\n", actual, PF_ABI_VERSION);
        return 12;
    }
    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); ++i) {
        if (required(module, exports[i]) == NULL) return 11;
    }
    symbol = required(module, "strategy_create");
    if (symbol == NULL) return 11;
    _Static_assert(sizeof(create) == sizeof(symbol), "create pointer size");
    memcpy(&create, &symbol, sizeof(create));
    symbol = required(module, "strategy_free");
    if (symbol == NULL) return 11;
    _Static_assert(sizeof(destroy) == sizeof(symbol), "free pointer size");
    memcpy(&destroy, &symbol, sizeof(destroy));
    instance = create("{}");
    if (instance == NULL) {
        fprintf(stderr, "strategy_create returned null\n");
        return 13;
    }
    destroy(instance);
    printf("{\"event\":\"create_free\",\"non_null\":true}\n");
    if (dlclose(module) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return 14;
    }
    printf("{\"event\":\"loader_complete\"}\n");
    return fflush(stdout) == 0 ? 0 : 3;
}
