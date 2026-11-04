// The 1.11 public layout change updates the shared engine epoch once.
// A changed layout must reject the previous engine epoch.
#include <pineforge/cache_policy.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#if defined(PINEFORGE_LEDGER_OLD_HEADER_SKEW)
// Compile this branch against the actual frozen pre-1.11 header closure,
// then link to the final current archive with --no-gc-sections. The call must
// fail to link for the old engine epoch; a successful link is the skew RED.
// This branch deliberately uses no new cache symbol as a substitute for the
// actual existing set_cache_policy(CacheDefaults const&) skew.
extern "C" void old_layout_cache_policy_call(pineforge::BacktestEngine& engine) {
    pineforge::CacheDefaults policy;
    engine.set_cache_policy(policy);
}
int main() {
    using Call = void (*)(pineforge::BacktestEngine&);
    Call volatile required = &old_layout_cache_policy_call;
    return required == nullptr;
}
#else
// Reopen the real old namespace when it is still installed. After the shared
// bump these are only incomplete old-epoch sentinel types, with no symbols.
namespace pineforge::engine_script_run_v19 {
class BacktestEngine;
class TradeLedger;
class NativeStrategyHost;
}
namespace {
using pineforge::CacheCapacity;
using pineforge::CacheDefaults;

// Literal layout of the public pre-1.11 v19 header.
// Actual sizeof/alignof/offsetof compile on the target architecture; there
// is no copied compiler output or hardcoded "160 bytes means pass" result.
struct FrozenV19CacheDefaults {
    CacheCapacity ledger_rows;
    CacheCapacity trade_pages, dependency_pages, exit_pages, book_pages;
    CacheCapacity retained_sequence_pages;
    CacheCapacity cohort_id_pages, exit_rank_pages, erased_minima_pages, consumed_cycle_pages;
    CacheCapacity position_tail_pages, successor_root_pages, receipt_tail_pages;
    CacheCapacity chain_segment_pages, equity_curve_pages, ta_history_pages;
    CacheCapacity feed_calendar_pages, recorded_series_pages, projection_exit_pages;
};
static_assert(std::is_standard_layout_v<CacheDefaults>);
static_assert(std::is_standard_layout_v<FrozenV19CacheDefaults>);

template<class T>
constexpr std::array<std::size_t, 21> layout() {
    return {{
        sizeof(T), alignof(T),
        offsetof(T, ledger_rows), offsetof(T, trade_pages), offsetof(T, dependency_pages),
        offsetof(T, exit_pages), offsetof(T, book_pages), offsetof(T, retained_sequence_pages),
        offsetof(T, cohort_id_pages), offsetof(T, exit_rank_pages),
        offsetof(T, erased_minima_pages), offsetof(T, consumed_cycle_pages),
        offsetof(T, position_tail_pages), offsetof(T, successor_root_pages),
        offsetof(T, receipt_tail_pages), offsetof(T, chain_segment_pages),
        offsetof(T, equity_curve_pages), offsetof(T, ta_history_pages),
        offsetof(T, feed_calendar_pages), offsetof(T, recorded_series_pages),
        offsetof(T, projection_exit_pages)
    }};
}
template<std::size_t N>
constexpr bool equal(const std::array<std::size_t, N>& a,
                     const std::array<std::size_t, N>& b) {
    for (std::size_t i = 0; i < N; ++i) if (a[i] != b[i]) return false;
    return true;
}
constexpr bool current_is_v19 =
    std::is_same_v<pineforge::BacktestEngine, pineforge::engine_script_run_v19::BacktestEngine>;
constexpr bool ledger_is_v19 =
    std::is_same_v<pineforge::TradeLedger, pineforge::engine_script_run_v19::TradeLedger>;
constexpr bool host_is_v19 =
    std::is_same_v<pineforge::NativeStrategyHost, pineforge::engine_script_run_v19::NativeStrategyHost>;
constexpr bool changed = !equal(layout<CacheDefaults>(), layout<FrozenV19CacheDefaults>());

static_assert(!changed || !current_is_v19,
    "CacheDefaults layout changed without updating the shared engine epoch");
static_assert(!changed || (!ledger_is_v19 && !host_is_v19),
    "Engine, ledger and host epochs must match the changed public layout");

constexpr std::uint64_t fingerprint() {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto value : layout<CacheDefaults>()) {
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= offsetof(CacheDefaults, ledger_cache_max_bytes);
    hash *= UINT64_C(1099511628211);
    hash ^= sizeof(decltype(CacheDefaults::ledger_cache_max_bytes));
    return hash * UINT64_C(1099511628211);
}
}
int main() {
    std::printf("CacheDefaults actual layout fingerprint=%llu size=%zu alignment=%zu bound_offset=%zu\n",
        static_cast<unsigned long long>(fingerprint()), sizeof(CacheDefaults), alignof(CacheDefaults),
        offsetof(CacheDefaults, ledger_cache_max_bytes));
    return 0; // The genuine layout/epoch rejection is the compile-time contract above.
}
#endif
