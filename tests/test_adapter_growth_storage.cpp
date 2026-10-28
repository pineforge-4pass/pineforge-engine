// Real settlements with pinned origin rosters, compared with
// test-local copies of the old retain-capacity operations. No hash pins.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace growth_fixture {
using Handle = pineforge::native_order::RequestHandle;
using Units = std::unordered_map<std::uint64_t, double>;
using Opened = std::vector<Handle>;

// Unmodelled containers use production behaviour in both arms: old_clear
// would invent legacy behaviour and falsify the old/new comparison.
template<class Container>
inline constexpr bool modeled = false;
template<>
inline constexpr bool modeled<Units> = true;
template<>
inline constexpr bool modeled<Opened> = true;

struct Probe {
    bool old_implementation = false;
    std::vector<Units*> units;
    std::vector<Opened*> opened;
    // L18's tail summary reuses the same two substituted operations on its own
    // exposure containers (an origin map and a live-cohort set). Their element
    // types are private to that class, so the witness below is by address: the
    // same property, "empty with retained buckets".
    std::vector<const void*> other_exposure_storage;
};
inline Probe* active = nullptr;

template<class Container>
void remember(std::vector<Container*>& rows, Container& container) {
    if (std::find(rows.begin(), rows.end(), &container) == rows.end())
        rows.push_back(&container);
}
inline void remember_any(std::vector<const void*>& rows, const void* container) {
    if (std::find(rows.begin(), rows.end(), container) == rows.end())
        rows.push_back(container);
}
inline void observe(Units& units) {
    if (active && units.empty() && units.bucket_count() > Units{}.bucket_count())
        remember(active->units, units);
}
inline void observe(Opened& opened) {
    if (active && opened.empty() && opened.capacity() > Opened{}.capacity())
        remember(active->opened, opened);
}
template<class K, class V, class H, class E, class A>
void observe(std::unordered_map<K, V, H, E, A>& map) {
    if (active && map.empty()
        && map.bucket_count() > std::unordered_map<K, V, H, E, A>{}.bucket_count())
        remember_any(active->other_exposure_storage, &map);
}
template<class K, class H, class E, class A>
void observe(std::unordered_set<K, H, E, A>& set) {
    if (active && set.empty()
        && set.bucket_count() > std::unordered_set<K, H, E, A>{}.bucket_count())
        remember_any(active->other_exposure_storage, &set);
}

// Before L19m the per-origin erase left the empty map/vector untouched;
// reversal/flat called clear(), which also kept their allocation. Keep those
// implementations here, outside the installed headers and shipped library.
template<class Container>
void old_release_empty(Container&) {}
template<class Container>
void old_clear(Container& container) { container.clear(); }
} // namespace growth_fixture

namespace pineforge::detail {
template<class Container>
void growth_fixture_release_empty(Container& container) {
    if constexpr (!growth_fixture::modeled<Container>) {
        release_empty_exposure_capacity(container);
        return;
    } else {
    growth_fixture::observe(container);
    if (growth_fixture::active && growth_fixture::active->old_implementation)
        growth_fixture::old_release_empty(container);
    else
        release_empty_exposure_capacity(container);
    }
}
template<class Container>
void growth_fixture_clear(Container& container) {
    if constexpr (!growth_fixture::modeled<Container>) {
        clear_exposure_storage(container);
        return;
    } else {
    // Observe the empty container after the old clear, before any release.
    if (growth_fixture::active && growth_fixture::active->old_implementation) {
        growth_fixture::old_clear(container);
        growth_fixture::observe(container);
    } else {
        clear_exposure_storage(container);
    }
    }
}
} // namespace pineforge::detail

// Compile the actual adapter call sites into this executable, substituting
// only the two operations above. Its definitions satisfy the adapter symbols
// before the static archive is searched, so pine_adapter.cpp.o is not pulled
// from libpineforge.a. All other source/kernel objects are the normal library.
// Removing a production release call also removes this test's observation;
// the live witness then fails. No class, access specifier or state is changed.
#define release_empty_exposure_capacity growth_fixture_release_empty
#define clear_exposure_storage growth_fixture_clear
#include "../src/source/pine_adapter.cpp"
#undef clear_exposure_storage
#undef release_empty_exposure_capacity

namespace growth_fixture {
namespace storage = pineforge::detail;
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Exposure {
    Opened opened;
    Units units;
};

Handle handle(std::uint64_t incarnation) {
    Handle result;
    result.incarnation = incarnation;
    return result;
}

void exposure_lifecycles_match_old_containers() {
    Exposure actual, old;
    for (std::uint64_t cycle = 0; cycle < 128; ++cycle) {
        for (std::uint64_t offset = 1; offset <= 9; ++offset) {
            const auto origin = cycle * 9 + offset;
            actual.opened.push_back(handle(origin));
            old.opened.push_back(handle(origin));
            actual.units[origin] += static_cast<double>(offset);
            old.units[origin] += static_cast<double>(offset);
        }
        while (!old.opened.empty()) {
            const auto origin = old.opened.back().incarnation;
            actual.units.erase(origin);
            old.units.erase(origin);
            actual.opened.pop_back();
            old.opened.pop_back();
            storage::release_empty_exposure_capacity(actual.units);
            storage::release_empty_exposure_capacity(actual.opened);
            require(actual.units == old.units && actual.opened == old.opened,
                    "release preserves all quantities and complete handle identities");
        }
        require(actual.units.bucket_count() == Units{}.bucket_count()
                && actual.opened.capacity() == Opened{}.capacity(),
                "drained exposure has the capacity of fresh containers");
        actual.opened.push_back(handle(cycle + 1));
        actual.units[cycle + 1] = 2.0;
        storage::clear_exposure_storage(actual.opened);
        storage::clear_exposure_storage(actual.units);
        require(actual.opened.empty() && actual.units.empty(),
                "clear preserves the old empty logical state");
    }
}

struct Recording {
    Recording() {
        require(!global_allocation::recording, "allocation windows must not nest");
        global_allocation::live_bytes = 0;
        global_allocation::recording = true;
    }
    ~Recording() { global_allocation::recording = false; }
};

std::size_t retained_closed_exposures(std::size_t ids, bool release, bool map) {
    std::vector<Exposure> cohorts(ids);
    const Handle opening = handle(1);
    Recording recording;
    for (auto& cohort : cohorts) {
        if (map) {
            cohort.units[opening.incarnation] = 1.0;
            cohort.units.erase(opening.incarnation);
            if (release) storage::release_empty_exposure_capacity(cohort.units);
        } else {
            cohort.opened.push_back(opening);
            cohort.opened.erase(cohort.opened.begin());
            if (release) storage::release_empty_exposure_capacity(cohort.opened);
        }
    }
    return global_allocation::live_bytes;
}

void closed_id_heap_is_flat() {
    for (const bool map : {false, true}) {
        const auto old_low = retained_closed_exposures(10, false, map);
        const auto old_high = retained_closed_exposures(10000, false, map);
        const auto low = retained_closed_exposures(10, true, map);
        const auto high = retained_closed_exposures(10000, true, map);
        require(old_high > old_low, "old containers reproduce historical-id growth");
        require(high == low && high == 0, "empty exposure children grow with completed ids");
    }
}

struct PinnedOrigins {
    bool previous = source::detail::retain_retired_rows();
    PinnedOrigins() { source::detail::set_retain_retired_rows(true); }
    ~PinnedOrigins() { source::detail::set_retain_retired_rows(previous); }
};

class SettlementHost final : public source::PineStrategyHost {
public:
    explicit SettlementHost(int cycles) : cycles_(cycles) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.close_entries_rule_any = true;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) {
            strategy_entry("keeper", true, kNa, kNa, 1.0);
            return;
        }
        const int cycle = (bar - 1) / 4;
        if (cycle >= cycles_) return;
        const auto id = "settled-" + std::to_string(cycle);
        if ((bar - 1) % 4 == 0) strategy_entry(id, true, kNa, kNa, 1.0);
        if ((bar - 1) % 4 == 1)
            strategy_exit("exit-" + id, id, 100.5, kNa, kNa, kNa, kNa, 100.0);
    }
    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    double keeper_units() const { return adapter_.source_unclosed_qty_for("keeper"); }
private:
    int cycles_;
};

struct RunResult {
    std::vector<Trade> trades;
    std::vector<std::uint64_t> hashes;
    std::uint64_t final_hash = 0;
    std::vector<std::size_t> unit_buckets, opened_capacity;
    double keeper_units = 0.0;
};

RunResult live_run(const std::vector<Bar>& bars, int cycles, bool old) {
    // Retained placement rows pin every origin, so the cohorts cannot spill.
    // The keeper also prevents a flat-book clear from masking a missing
    // per-cohort release. Read the saved container pointers only while both
    // the host and this existing retention override are still alive.
    PinnedOrigins pinned;
    const auto erased_before = source::detail::retired_rows_erased();
    SettlementHost host(cycles);
    Probe probe;
    probe.old_implementation = old;
    active = &probe;
    try {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    } catch (...) {
        active = nullptr;
        throw;
    }
    active = nullptr;
    require(host.last_error().empty(), "live settlement run completes");
    require(source::detail::retired_rows_erased() == erased_before,
            "origin placements remain pinned throughout the run");
    require(host.trade_count() == cycles, "every distinct cohort settles its real trade");
    require(!probe.units.empty() && !probe.opened.empty(),
            "real settlement reaches both production release call sites");
    RunResult result;
    for (const auto* units : probe.units) {
        require(units->empty(), "settled cohort retains no live provenance");
        result.unit_buckets.push_back(units->bucket_count());
    }
    for (const auto* opened : probe.opened) {
        require(opened->empty(), "settled cohort retains no opened handles");
        result.opened_capacity.push_back(opened->capacity());
    }
    for (int i = 0; i < host.trade_count(); ++i) result.trades.push_back(host.get_trade(i));
    result.hashes = host.hashes();
    result.final_hash = host.broker_state_hash();
    result.keeper_units = host.keeper_units();
    return result;
}

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

void live_settlements_release_pinned_cohort_capacity() {
    const int cycles = 32;
    std::vector<Bar> bars;
    for (int i = 0; i < cycles * 4 + 8; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, 1736121600000LL + i * 60000LL});
    const auto reference = live_run(bars, cycles, true);
    const auto actual = live_run(bars, cycles, false);
    require(actual.unit_buckets.size() == reference.unit_buckets.size()
            && actual.opened_capacity.size() == reference.opened_capacity.size(),
            "identical live runs visit the same settled cohort containers");
    for (std::size_t i = 0; i < actual.unit_buckets.size(); ++i)
        require(actual.unit_buckets[i] == Units{}.bucket_count()
                && actual.unit_buckets[i] < reference.unit_buckets[i],
                "real settlement releases map buckets retained by the old implementation");
    for (std::size_t i = 0; i < actual.opened_capacity.size(); ++i)
        require(actual.opened_capacity[i] == Opened{}.capacity()
                && actual.opened_capacity[i] < reference.opened_capacity[i],
                "real settlement releases opened capacity retained by the old implementation");
    require(!actual.hashes.empty() && actual.hashes == reference.hashes
            && actual.final_hash == reference.final_hash,
            "all live broker hashes equal the old implementation");
    require(actual.keeper_units > 0.0 && same_bits(actual.keeper_units, reference.keeper_units),
            "the keeper stays live while other cohorts drain");
    require(actual.trades.size() == reference.trades.size(), "live trade populations match");
    for (std::size_t i = 0; i < actual.trades.size(); ++i) {
        const auto& a = actual.trades[i];
        const auto& b = reference.trades[i];
        require(a.entry_id == b.entry_id && a.exit_id == b.exit_id
                && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
                && a.entry_time == b.entry_time && a.exit_time == b.exit_time
                && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
                && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
                && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
                && a.close_cause == b.close_cause,
                "live trade identities and metadata match");
        require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
                && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl)
                && same_bits(a.pnl_pct, b.pnl_pct) && same_bits(a.commission, b.commission)
                && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown),
                "live trade doubles match bit for bit");
    }
}
} // namespace growth_fixture

int main() {
    try {
        growth_fixture::exposure_lifecycles_match_old_containers();
        growth_fixture::closed_id_heap_is_flat();
        growth_fixture::live_settlements_release_pinned_cohort_capacity();
        std::puts("test_adapter_growth_storage: live settlements release old exposure capacity");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_growth_storage: %s\n", error.what());
        return 1;
    }
}
