#define PINEFORGE_ENGINE_LAYOUT_EXPECTATIONS_ONLY
#include "../src/engine_layout_fingerprint.hpp"
#undef PINEFORGE_ENGINE_LAYOUT_EXPECTATIONS_ONLY
#include <cstdio>
#include <cstring>

namespace {
// Adjacent virtuals give a representation control independent of the engine
// table. Reversing their declarations must reverse their measured order.
struct SlotControl {
    virtual void first();
    virtual void second();
    void ordinary() {}
};

bool check_slots(bool compare_expected) {
    using Probe = pineforge::EngineLayoutProbe;
    const auto first = Probe::virtual_slot(&SlotControl::first);
    const auto second = Probe::virtual_slot(&SlotControl::second);
    const auto invalid = pineforge::engine_layout::unmeasured;
    if (first == invalid || second == invalid || second != first + 1
        || Probe::virtual_slot(&SlotControl::ordinary) != invalid) {
        std::fprintf(stderr, "Unsupported virtual member-function pointer representation\n");
        return false;
    }
    bool ok = true;
#define PINEFORGE_CHECK_SLOT(name, member) \
    do { \
        const auto actual = Probe::name(); \
        const auto expected = pineforge::engine_layout::name; \
        if (actual == invalid || (compare_expected && (expected == invalid || actual != expected))) { \
            std::fprintf(stderr, "Engine virtual slot " #name ": expected=%zu actual=%zu; " \
                         "measure the table and raise PF_STREAM_ENGINE_LAYOUT_VERSION\n", \
                         expected, actual); \
            ok = false; \
        } \
    } while (false);
    PINEFORGE_ENGINE_LAYOUT_VIRTUALS(PINEFORGE_CHECK_SLOT)
#undef PINEFORGE_CHECK_SLOT
    return ok;
}
}

int main(int argc, char** argv) {
#if defined(__linux__) && defined(__LP64__) && defined(__x86_64__)
    constexpr const char* arch = "x86_64";
#elif defined(__linux__) && defined(__LP64__) && defined(__aarch64__)
    constexpr const char* arch = "aarch64";
#else
#error "Run the engine layout probe on Linux x86_64 or aarch64 with LP64"
#endif
    const bool measure = argc == 2 && std::strcmp(argv[1], "--measure") == 0;
    if (argc != 1 && !measure) {
        std::fprintf(stderr, "Usage: test_engine_layout_probe [--measure]\n");
        return 2;
    }
#define PINEFORGE_PRINT_LAYOUT(name, expression) \
    std::printf("LAYOUT %s " #name "=%zu\n", arch, pineforge::EngineLayoutProbe::name);
    PINEFORGE_ENGINE_LAYOUT_VALUES(PINEFORGE_PRINT_LAYOUT)
#undef PINEFORGE_PRINT_LAYOUT
#define PINEFORGE_PRINT_SLOT(name, member) \
    std::printf("LAYOUT %s " #name "=%zu\n", arch, pineforge::EngineLayoutProbe::name());
    PINEFORGE_ENGINE_LAYOUT_VIRTUALS(PINEFORGE_PRINT_SLOT)
#undef PINEFORGE_PRINT_SLOT
    // Measurement must work with an empty expected table. CTest invokes the
    // default mode, which always compares actual slots with the filled table.
    return check_slots(!measure) ? 0 : 1;
}
