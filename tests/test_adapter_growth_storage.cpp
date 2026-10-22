// CR-150 L19m: real settlements with pinned origin rosters, compared with
// test-local copies of the old retain-capacity operations. No hash pins.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/detail/working_members.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace growth_fixture {
namespace storage = pineforge::detail;
using Handle = pineforge::native_order::RequestHandle;
using Units = std::unordered_map<std::uint64_t, double>;
using Opened = std::vector<Handle>;
using StoredUnits = storage::WorkingMap<double>;
using StoredOpened = storage::WorkingSequence<Handle>;
constexpr std::size_t page_size = 512;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

storage::ScratchStatus lexical(void*, const storage::ScratchByteSource& a,
                               const storage::ScratchByteSource& b, int& order) noexcept {
    std::array<std::byte, 32> left{}, right{};
    const auto common = std::min(a.size(), b.size());
    for (std::uint64_t at = 0; at < common;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), common - at));
        auto status = a.read(at, {left.data(), n});
        if (status != storage::ScratchStatus::ok) return status;
        status = b.read(at, {right.data(), n});
        if (status != storage::ScratchStatus::ok) return status;
        const int compared = std::memcmp(left.data(), right.data(), n);
        if (compared) { order = compared < 0 ? -1 : 1; return storage::ScratchStatus::ok; }
        at += n;
    }
    order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    return storage::ScratchStatus::ok;
}

struct Rig {
    std::vector<std::byte> bytes = std::vector<std::byte>(128 * 1024 * 1024);
    storage::FixedMemoryScratchBacking backing{{bytes.data(), bytes.size()}, page_size};
    storage::ScratchStore store{{2, page_size, 1, 1, {0x4f315334, nullptr, &lexical},
                                 storage::ScratchOpenMode::create_empty}, &backing};
    Rig() { storage::working_store::check(store.status()); }
};

struct Sample { std::uint64_t bytes = 0, records = 0; };
Sample sample(const storage::ScratchStore& store) {
    return {store.stats().backing_bytes, store.record_count()};
}

// Scan every owner in the exposure domain, including moved-away owners whose
// container no longer exists. The keeper's nonempty count is not a dead owner.
struct OwnerCensus { std::uint64_t empty = 0, nonempty = 0; };
OwnerCensus owner_records(const storage::ScratchStore& store, std::uint64_t domain) {
    namespace ws = storage::working_store;
    const ws::Key start(domain, 0, 0);
    std::array<std::byte, 32> prefix{}, key{};
    ws::check(start.read(0, {prefix.data(), prefix.size()}));
    storage::ScratchHandle cursor = 0;
    auto status = store.lower_bound(start, cursor);
    OwnerCensus count;
    while (status == storage::ScratchStatus::ok) {
        storage::ScratchRecordInfo info;
        ws::check(store.point(cursor, info));
        if (info.key_bytes < 16) break;
        ws::check(store.read(cursor, storage::ScratchField::key, 0, {key.data(), 16}));
        if (std::memcmp(key.data(), prefix.data(), 16)) break;
        if (info.key_bytes != key.size()) ws::check(storage::ScratchStatus::corrupt);
        ws::check(store.read(cursor, storage::ScratchField::key, 16, {key.data() + 16, 16}));
        if (std::equal(key.begin() + 24, key.end(), prefix.begin() + 24)) {
            if (info.value_bytes != 8) ws::check(storage::ScratchStatus::corrupt);
            std::array<std::byte, 8> value{};
            ws::check(store.read(cursor, storage::ScratchField::value, 0,
                                 {value.data(), value.size()}));
            if (ws::word(value.data()) == 0) ++count.empty;
            else ++count.nonempty;
        }
        status = store.next(cursor, cursor);
    }
    if (status != storage::ScratchStatus::ok && status != storage::ScratchStatus::end)
        ws::check(status);
    return count;
}

struct Observation {
    bool empty = false;
    bool bound_before = false;
    Sample before, after;
    std::uint64_t empty_owners = 0;
};

struct Probe {
    bool old_implementation = false;
    storage::ScratchStore* store = nullptr;
    std::vector<Observation> units, opened;
};
inline Probe* active = nullptr;

Sample before_operation() {
    return active ? sample(*active->store) : Sample{};
}
// Select by the pre-operation state; save post-operation emptiness even if
// release/clear incorrectly leaves a nonempty container.
inline void observe(StoredUnits& units, Sample before, bool bound_before, bool expected_empty) {
    if (active && expected_empty) {
        active->units.push_back({units.empty(), bound_before, before, sample(*active->store),
                                owner_records(*active->store, 45).empty});
    }
}
inline void observe(StoredOpened& opened, Sample before, bool bound_before, bool expected_empty) {
    if (active && expected_empty) {
        active->opened.push_back({opened.empty(), bound_before, before, sample(*active->store),
                                 owner_records(*active->store, 44).empty});
    }
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
    const auto before = growth_fixture::before_operation();
    const bool bound_before = container.bound();
    const bool expected_empty = container.empty();
    if (growth_fixture::active && growth_fixture::active->old_implementation)
        growth_fixture::old_release_empty(container);
    else
        release_empty_exposure_capacity(container);
    growth_fixture::observe(container, before, bound_before, expected_empty);
}
template<class Container>
void growth_fixture_clear(Container& container) {
    const auto before = growth_fixture::before_operation();
    const bool bound_before = container.bound();
    if (growth_fixture::active && growth_fixture::active->old_implementation)
        growth_fixture::old_clear(container);
    else
        clear_exposure_storage(container);
    growth_fixture::observe(container, before, bound_before, true);
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
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

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

enum class Operation { release_empty, clear_empty, clear_populated };
const char* operation_name(Operation operation) {
    return operation == Operation::release_empty ? "release-empty"
        : operation == Operation::clear_empty ? "clear-empty" : "clear-populated";
}

void add_origin(StoredUnits& units, const Handle& opening) { units[opening.incarnation] = 1.0; }
void add_origin(StoredOpened& opened, const Handle& opening) { opened.push_back(opening); }
void erase_origin(StoredUnits& units, const Handle& opening) { units.erase(opening.incarnation); }
void erase_origin(StoredOpened& opened, const Handle&) { opened.erase(opened.begin()); }

struct BackingSample {
    Sample store;
    std::uint64_t empty_owners = 0;
};

template<class Container>
BackingSample retained_closed_backing(std::size_t ids, bool old, Operation operation,
                                     std::uint64_t domain) {
    Rig rig;
    // Keep the closed cohort children alive until AFTER the census, just as
    // retained origins keep the real adapter's empty children addressable.
    std::vector<Container> cohorts(ids);
    for (std::size_t i = 0; i < ids; ++i) {
        auto& container = cohorts[i];
        container.bind(rig.store, domain, i + 1);
        const auto opening = handle(i + 1);
        add_origin(container, opening);
        if (operation != Operation::clear_populated) erase_origin(container, opening);
        if (operation == Operation::release_empty) {
            if (old) old_release_empty(container);
            else storage::release_empty_exposure_capacity(container);
        } else {
            if (old) old_clear(container);
            else storage::clear_exposure_storage(container);
        }
        require(container.empty(), "bound release and clear leave the logical exposure empty");
    }
    return {sample(rig.store), owner_records(rig.store, domain).empty};
}

void print_backing(bool map, Operation operation, std::size_t ids, bool old, BackingSample value) {
    std::printf("bound %s %s N=%zu old=%d bytes=%llu records=%llu empty_owners=%llu\n",
                map ? "units" : "opened", operation_name(operation), ids, old,
                static_cast<unsigned long long>(value.store.bytes),
                static_cast<unsigned long long>(value.store.records),
                static_cast<unsigned long long>(value.empty_owners));
}

bool within_one_page(std::uint64_t a, std::uint64_t b) {
    return a > b ? a - b <= page_size : b - a <= page_size;
}

void closed_bound_backing_is_flat() {
    struct Case {
        BackingSample fresh, low, high, old_low, old_high;
    };
    std::array<Case, 6> cases{};
    std::size_t index = 0;
    for (const auto operation : {Operation::release_empty, Operation::clear_empty,
                                  Operation::clear_populated}) {
        for (const bool map : {false, true}) {
            const auto measure = [&](std::size_t ids, bool old) {
                return map ? retained_closed_backing<StoredUnits>(ids, old, operation, 45)
                    : retained_closed_backing<StoredOpened>(ids, old, operation, 44);
            };
            auto& row = cases[index++];
            // One fresh exposure exercises the same insertion/erase peak.
            // Extent cannot shrink, so the only byte allowance is one 512-byte
            // page; record counts have NO allowance.
            row.fresh = measure(1, false);
            row.low = measure(64, false);
            row.high = measure(512, false);
            row.old_low = measure(64, true);
            row.old_high = measure(512, true);
            print_backing(map, operation, 1, false, row.fresh);
            print_backing(map, operation, 64, false, row.low);
            print_backing(map, operation, 512, false, row.high);
            print_backing(map, operation, 64, true, row.old_low);
            print_backing(map, operation, 512, true, row.old_high);
        }
    }
    // Collect and print BOTH N for EVERY case before the predicted RED.
    std::fflush(stdout);
    for (const auto& row : cases) {
        require(row.old_high.store.records > row.old_low.store.records
                    && row.old_high.store.bytes > row.old_low.store.bytes,
                "old bound operations retain backing with completed origins");
        require(row.low.store.records == row.high.store.records,
                "released bound exposures leave no dead owner records across N=64,512");
        require(within_one_page(row.low.store.bytes, row.high.store.bytes),
                "released bound exposure backing is flat within one 512-byte page across N");
        require(row.low.store.records == row.fresh.store.records
                    && row.high.store.records == row.fresh.store.records,
                "released bound exposures leave the record count of one fresh exposure");
        require(within_one_page(row.low.store.bytes, row.fresh.store.bytes)
                    && within_one_page(row.high.store.bytes, row.fresh.store.bytes),
                "released bound exposures leave fresh backing within one 512-byte page");
        require(row.low.empty_owners == row.fresh.empty_owners
                    && row.high.empty_owners == row.fresh.empty_owners,
                "the census includes moved-away empty owner records");
        require(row.low.store.records < row.old_low.store.records
                    && row.high.store.records < row.old_high.store.records,
                "production release removes records kept by the old bound operations");
        require(row.low.store.bytes < row.old_low.store.bytes
                    && row.high.store.bytes < row.old_high.store.bytes,
                "production release reuses backing retained by the old bound operations");
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
    void bind_working_store(storage::ScratchStore& store) { adapter_.bind_scratch_store(store); }
    native_order::CohortHandle keeper_cohort() {
        return adapter_.fixture_cohort_state("keeper", 0, {}, 0.0).first;
    }
private:
    int cycles_;
};

struct RunResult {
    std::vector<Trade> trades;
    std::vector<std::uint64_t> hashes;
    std::uint64_t final_hash = 0;
    std::vector<Observation> unit_storage, opened_storage;
    Sample backing;
    double keeper_units = 0.0;
};

RunResult live_run(const std::vector<Bar>& bars, int cycles, bool old) {
    // Retained placement rows pin every origin, so the cohorts cannot spill.
    // The keeper also prevents a flat-book clear from masking a missing
    // per-cohort release. Snapshot at each release: the store-backed cohort
    // table may evict its two row frames before the end of the run.
    PinnedOrigins pinned;
    Rig rig;
    const auto erased_before = source::detail::retired_rows_erased();
    SettlementHost host(cycles);
    host.bind_working_store(rig.store);
    Probe probe;
    probe.old_implementation = old;
    probe.store = &rig.store;
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
    require(std::any_of(probe.units.begin(), probe.units.end(),
                        [](const Observation& row) { return row.bound_before; })
                && std::any_of(probe.opened.begin(), probe.opened.end(),
                               [](const Observation& row) { return row.bound_before; }),
            "real settlement releases store-bound exposure containers");
    const auto keeper = host.keeper_cohort();
    const auto keeper_units = storage::working_store::get_word(rig.store, {45, keeper.value, 0});
    const auto keeper_opened = storage::working_store::get_word(rig.store, {44, keeper.value, 0});
    require(keeper.value != 0 && keeper_units && *keeper_units > 0
                && keeper_opened && *keeper_opened > 0,
            "the live keeper owns both nonempty exposure count records");
    const auto final_units = owner_records(rig.store, 45);
    const auto final_opened = owner_records(rig.store, 44);
    RunResult result;
    for (const auto& units : probe.units) {
        require(units.empty && final_units.nonempty == 1,
                "settled cohort retains no live provenance");
        result.unit_storage.push_back(units);
    }
    for (const auto& opened : probe.opened) {
        require(opened.empty && final_opened.nonempty == 1,
                "settled cohort retains no opened handles");
        result.opened_storage.push_back(opened);
    }
    for (int i = 0; i < host.trade_count(); ++i) result.trades.push_back(host.get_trade(i));
    result.hashes = host.hashes();
    result.final_hash = host.broker_state_hash();
    result.keeper_units = host.keeper_units();
    result.backing = sample(rig.store);
    std::printf("live N=%d old=%d bytes=%llu records=%llu\n", cycles, old,
                static_cast<unsigned long long>(result.backing.bytes),
                static_cast<unsigned long long>(result.backing.records));
    return result;
}

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

void live_settlements_release_pinned_cohort_capacity(int cycles) {
    std::vector<Bar> bars;
    for (int i = 0; i < cycles * 4 + 8; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, 1736121600000LL + i * 60000LL});
    const auto reference = live_run(bars, cycles, true);
    const auto actual = live_run(bars, cycles, false);
    const auto fresh_units = retained_closed_backing<StoredUnits>(1, false,
        Operation::release_empty, 45);
    const auto fresh_opened = retained_closed_backing<StoredOpened>(1, false,
        Operation::release_empty, 44);
    require(actual.unit_storage.size() == reference.unit_storage.size()
            && actual.opened_storage.size() == reference.opened_storage.size(),
            "identical live runs visit the same settled cohort containers");
    // Whole-store totals include the deliberately pinned placements and
    // origins. The isolated cases check extent flatness; this live census
    // includes every empty owner, including owners that lost their container.
    for (std::size_t i = 0; i < actual.unit_storage.size(); ++i) {
        require(actual.unit_storage[i].empty_owners == fresh_units.empty_owners
                && actual.unit_storage[i].empty_owners < reference.unit_storage[i].empty_owners,
                "real settlement releases map buckets retained by the old implementation");
        require(actual.unit_storage[i].after.records <= actual.unit_storage[i].before.records
                    && actual.unit_storage[i].after.bytes
                        <= actual.unit_storage[i].before.bytes + page_size,
                "real provenance release adds no records or more than one backing page");
    }
    for (std::size_t i = 0; i < actual.opened_storage.size(); ++i) {
        require(actual.opened_storage[i].empty_owners == fresh_opened.empty_owners
                && actual.opened_storage[i].empty_owners < reference.opened_storage[i].empty_owners,
                "real settlement releases opened capacity retained by the old implementation");
        require(actual.opened_storage[i].after.records <= actual.opened_storage[i].before.records
                    && actual.opened_storage[i].after.bytes
                        <= actual.opened_storage[i].before.bytes + page_size,
                "real opened release adds no records or more than one backing page");
    }
    require(actual.backing.records < reference.backing.records
                && actual.backing.bytes <= reference.backing.bytes,
            "real releases reduce total store records without increasing backing");
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

void live_settlements_release_pinned_cohort_capacity() {
    for (const int cycles : {8, 32})
        live_settlements_release_pinned_cohort_capacity(cycles);
}
} // namespace growth_fixture

int main() {
    try {
        growth_fixture::closed_bound_backing_is_flat();
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
