#include "../src/engine_layout_probe.hpp"
#include <cstdio>

int main() {
#if defined(__linux__) && defined(__LP64__) && defined(__x86_64__)
    constexpr const char* arch = "x86_64";
#elif defined(__linux__) && defined(__LP64__) && defined(__aarch64__)
    constexpr const char* arch = "aarch64";
#else
#error "Run the engine layout probe on Linux x86_64 or aarch64 with LP64"
#endif
#define PINEFORGE_PRINT_LAYOUT(name, expression) \
    std::printf("LAYOUT %s " #name "=%zu\n", arch, pineforge::EngineLayoutProbe::name);
    PINEFORGE_ENGINE_LAYOUT_VALUES(PINEFORGE_PRINT_LAYOUT)
#undef PINEFORGE_PRINT_LAYOUT
    return 0;
}
