// R5 lane PERF-L1: an intrabar path delivers its script bars without
// allocating.
//
// deliver_intrabar_script listed each script bar's sub-bars and each sub-bar's
// samples in two vectors local to the call, so a run with a synthesized path
// (the bar magnifier) or a retained lower feed paid two heap allocations and
// two frees per script bar before any order work. The lane keeps both lists in
// consumer-owned buffers that keep their capacity from bar to bar.
//
// Cost row: a host with no orders runs N and then 2N bars under each path --
// synthesized (ENDPOINTS, a uniform distribution, volume-weighted samples) and
// a lower feed, under both slot-label policies -- and the heap allocations the
// extra N bars made, per bar, must sit within 0.25 of the same run with no
// intrabar path at all (which is the whole per-bar allocation the kernel has
// left on an idle bar). At the lane's base the synthesized and lower paths
// allocated two more per bar.
//
// The values the buffers feed -- sub-bar lists, samples, driver rows, the
// continuation and broker-state hashes -- are pinned by
// tests/test_native_lean_path.cpp over the same paths.
//
// Fail-before: compiled and run against f71cd820 the cost check fails for
// every intrabar path (the lane report records the counts).
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

bool g_counting = false;
std::uint64_t g_allocations = 0;

}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_counting) ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting) ++g_allocations;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    if (g_counting) ++g_allocations;
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace pineforge;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

struct IdleHost final : NativeStrategyHost {
    std::uint64_t bars = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++bars; }
};

enum class Path : int { None, Endpoints, Uniform, VolumeWeighted, Lower };
constexpr const char* kPathNames[] = {"none", "synthesized/endpoints", "synthesized/uniform",
                                      "synthesized/volume-weighted", "lower"};

std::vector<Bar> bars_of(int n, std::int64_t step) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + (i % 11) * 0.25 - (i % 5) * 0.5;
        bars.push_back(Bar{p, p + 1.0, p - 1.0, p + 0.25, 1.0 + (i % 7), kT0 + i * step});
    }
    return bars;
}

NativeRunSpec spec_for(Path path, bool tolerant, int n) {
    NativeRunSpec s;
    s.identity = {"perf-l1-intrabar-scratch", 1};
    s.input_tf = path == Path::Lower ? "5" : "1";
    s.script_tf = s.input_tf;
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.slot_label_policy = tolerant ? NativeSlotLabelPolicy::FeedTolerant
                                   : NativeSlotLabelPolicy::Canonical;
    switch (path) {
    case Path::None:
        break;
    case Path::Endpoints:
    case Path::Uniform:
    case Path::VolumeWeighted: {
        IntrabarPath::synthesized synthesized;
        synthesized.samples = path == Path::Endpoints ? 4 : 6;
        synthesized.distribution = path == Path::Endpoints ? MagnifierDistribution::ENDPOINTS
                                                           : MagnifierDistribution::UNIFORM;
        synthesized.volume_weighted = path == Path::VolumeWeighted;
        s.intrabar.value = synthesized;
        break;
    }
    case Path::Lower: {
        IntrabarPath::lower_tf lower;
        lower.tf = "1";
        lower.bars = bars_of(n * 5, kMinute);
        s.intrabar.value = lower;
        break;
    }
    }
    return s;
}

// Heap allocations made by one run of `n` bars, the host's construction and
// the spec's excluded.
std::uint64_t allocations(Path path, bool tolerant, int n) {
    IdleHost host;
    const NativeRunSpec spec = spec_for(path, tolerant, n);
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        std::fprintf(stderr, "  %s: configure refused\n", kPathNames[static_cast<int>(path)]);
        return ~0ULL;
    }
    const auto bars = bars_of(n, path == Path::Lower ? 5 * kMinute : kMinute);
    g_allocations = 0;
    g_counting = true;
    host.run(bars.data(), n);
    g_counting = false;
    const auto state = host.native_state();
    if (state.kind != NativeLifecycleKind::Completed || host.bars != static_cast<std::uint64_t>(n)) {
        std::fprintf(stderr, "  %s: the run did not complete\n", kPathNames[static_cast<int>(path)]);
        return ~0ULL;
    }
    return g_allocations;
}

double per_bar(Path path, bool tolerant, int n) {
    const std::uint64_t once = allocations(path, tolerant, n);
    const std::uint64_t twice = allocations(path, tolerant, 2 * n);
    if (once == ~0ULL || twice == ~0ULL) return 1e9;
    return static_cast<double>(static_cast<std::int64_t>(twice) - static_cast<std::int64_t>(once))
        / static_cast<double>(n);
}

void intrabar_paths_do_not_allocate_per_bar() {
    constexpr int kBars = 2000;
    for (int tolerant = 0; tolerant < 2; ++tolerant) {
        const double base = per_bar(Path::None, tolerant == 1, kBars);
        std::printf("  %s labels, no intrabar path: %.3f allocations/bar\n",
                    tolerant ? "tolerant" : "canonical", base);
        for (int p = 1; p <= 4; ++p) {
            const Path path = static_cast<Path>(p);
            const double cost = per_bar(path, tolerant == 1, kBars);
            std::printf("  %s labels, %s: %.3f allocations/bar\n",
                        tolerant ? "tolerant" : "canonical", kPathNames[p], cost);
            CHECK(cost <= base + 0.25);
        }
    }
}

}  // namespace

int main() {
    intrabar_paths_do_not_allocate_per_bar();
    std::printf("test_native_intrabar_scratch: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
