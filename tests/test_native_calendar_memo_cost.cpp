// R5 lane PERF-K1: canonical slot labels no longer resolve the calendar per
// bar.
//
// A bare native host on the default Canonical labels had every bar's input
// and script intervals, the next bar's intervals (the session-day facts look
// one bar ahead, R5 lane F5) and the preflight's interval resolved from
// scratch: up to three fresh session days per lookup, each a handful of
// vectors and strings, and a mktime per declared boundary for a zone. PERF0-K
// counted 84 heap allocations per idle bar where FeedTolerant labels at an
// equal pairing, which ask the calendar for nothing but the session-day
// facts of a new day, take 0.05. Lane PERF-K1 reads the calendar through a
// memo of its session days, so a bar inside a day already resolved costs
// arithmetic and no allocation.
//
// The witness is that count, because it is deterministic where a time is
// not: the same idle run under both label policies, every heap allocation
// counted by this TU's operator new, and the canonical run may take at most
// kExtraPerBar more per bar than the tolerant one. A new session day is still
// resolved once (its spans, and the facts' copy of it), a few allocations a
// day, which is what the bound leaves room for. Measured on this TU against
// engine main fc7aad62, before the lane: UTC 5m 83.98 extra allocations per
// bar, Asia/Tokyo 1m with a lunch break 224.26; after it 0.02 and 0.02 --
// the same on macOS arm64 (AppleClang 17) and Linux aarch64 (gcc 13.3).
//
// Both zones here are named in fewer than sixteen characters, so the
// timezone guard's copy of the name never allocates under any standard
// library's short-string buffer: what remains counted is the calendar's own.
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

bool g_counting = false;
std::uint64_t g_allocations = 0;

void* counted(std::size_t size) {
    if (g_counting) ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) { return counted(size); }
void* operator new[](std::size_t size) { return counted(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting) ++g_allocations;
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting) ++g_allocations;
    return std::malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {
using namespace pineforge;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double kExtraPerBar = 0.25;

struct Idle final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

std::vector<Bar> tape(std::int64_t first, std::int64_t step, int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + 0.25 * static_cast<double>(i % 9);
        bars.push_back({price, price + 0.5, price - 0.5, price, 1.0,
                        first + static_cast<std::int64_t>(i) * step});
    }
    return bars;
}

// Heap allocations of one idle run, configure excluded.
std::uint64_t allocations(const char* timezone, const char* session, const char* tf,
                          NativeSlotLabelPolicy policy, const std::vector<Bar>& bars) {
    Idle host;
    NativeRunSpec spec;
    spec.identity = {"k1-cost", 1};
    spec.input_tf = tf;
    spec.script_tf = tf;
    spec.tickerid = "TEST:K1";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.slot_label_policy = policy;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    g_allocations = 0;
    g_counting = true;
    host.run(bars.data(), static_cast<int>(bars.size()));
    g_counting = false;
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    return g_allocations;
}

void canonical_costs_what_tolerant_costs(const char* name, const char* timezone,
                                         const char* session, const char* tf,
                                         const std::vector<Bar>& bars) {
    const std::uint64_t canonical =
        allocations(timezone, session, tf, NativeSlotLabelPolicy::Canonical, bars);
    const std::uint64_t tolerant =
        allocations(timezone, session, tf, NativeSlotLabelPolicy::FeedTolerant, bars);
    const double count = static_cast<double>(bars.size());
    const double extra = (static_cast<double>(canonical) - static_cast<double>(tolerant)) / count;
    std::printf("  %s: %zu bars, canonical %llu, tolerant %llu allocations, %.2f extra per bar "
                "(bound %.2f)\n",
                name, bars.size(), static_cast<unsigned long long>(canonical),
                static_cast<unsigned long long>(tolerant), extra, kExtraPerBar);
    CHECK(extra <= kExtraPerBar);
}

}  // namespace

int main() {
    // UTC 24x7, 5m: 2025-01-06 00:00 UTC on, ten days.
    canonical_costs_what_tolerant_costs("UTC 24x7 5m", "UTC", "24x7", "5",
                                        tape(1736121600000LL, 5 * 60'000, 2880));
    // Asia/Tokyo with a lunch break, 1m: 2025-01-06 08:00 JST on, two days.
    canonical_costs_what_tolerant_costs("Asia/Tokyo lunch 1m", "Asia/Tokyo",
                                        "0900-1130,1230-1500", "1",
                                        tape(1736118000000LL, 60'000, 2880));
    if (failures == 0) std::printf("test_native_calendar_memo_cost: ok\n");
    return failures == 0 ? 0 : 1;
}
