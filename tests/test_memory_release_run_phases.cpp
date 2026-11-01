// Run-phase temporary storage is checked at its ownership boundaries.
// Requested C++ allocation bytes, not RSS or an allocator-layout estimate.
// No product object is populated by a test-side write.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/execution_close_selection.hpp>
#include "../src/native_execution_consumer.hpp"
#include "../src/engine_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// All replaceable C++17 forms use the same prefix and malloc/free pair.
// Unlike a window that resets live counters, this ledger follows every block
// allocated by the test thread until its actual release, across all windows.
// Other-thread activity is counted separately and fails a measured window.
namespace run_phase_heap {
thread_local bool test_thread = false;
std::atomic<bool> observing{false};
std::atomic<std::size_t> live_bytes{0}, live_blocks{0}, allocations{0};
std::atomic<std::size_t> foreign_activity{0};
struct Header { void* base; std::size_t bytes; bool counted; };

void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) size = 1;
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (alignment > maximum - sizeof(Header)
        || size > maximum - sizeof(Header) - alignment) return nullptr;
    void* base = std::malloc(size + sizeof(Header) + alignment);
    if (!base) return nullptr;
    const auto raw = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto aligned = (raw + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    auto* block = reinterpret_cast<unsigned char*>(aligned);
    const Header header{base, size, test_thread};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    if (test_thread) {
        live_bytes.fetch_add(size, std::memory_order_relaxed);
        live_blocks.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
    } else if (observing.load(std::memory_order_relaxed)) {
        foreign_activity.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}
void release(void* block) noexcept {
    if (!block) return;
    Header header{};
    std::memcpy(&header, static_cast<unsigned char*>(block) - sizeof(Header), sizeof header);
    if (header.counted) {
        live_bytes.fetch_sub(header.bytes, std::memory_order_relaxed);
        live_blocks.fetch_sub(1, std::memory_order_relaxed);
    }
    if (!test_thread && observing.load(std::memory_order_relaxed))
        foreign_activity.fetch_add(1, std::memory_order_relaxed);
    std::free(header.base);
}
void* throwing(std::size_t size, std::size_t alignment = 0) {
    for (;;) {
        if (void* block = allocate(size, alignment)) return block;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nothrow(std::size_t size, std::size_t alignment = 0) noexcept {
    try { return throwing(size, alignment); } catch (...) { return nullptr; }
}
struct Sample { std::size_t bytes, blocks, calls, foreign; };
Sample sample() noexcept {
    return {live_bytes.load(std::memory_order_relaxed),
            live_blocks.load(std::memory_order_relaxed),
            allocations.load(std::memory_order_relaxed),
            foreign_activity.load(std::memory_order_relaxed)};
}
bool same_live(const Sample& a, const Sample& b) noexcept {
    return a.bytes == b.bytes && a.blocks == b.blocks && a.foreign == b.foreign;
}
} // namespace run_phase_heap

void* operator new(std::size_t n) { return run_phase_heap::throwing(n); }
void* operator new[](std::size_t n) { return run_phase_heap::throwing(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return run_phase_heap::nothrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return run_phase_heap::nothrow(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    return run_phase_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return run_phase_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return run_phase_heap::nothrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return run_phase_heap::nothrow(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p) noexcept { run_phase_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { run_phase_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { run_phase_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { run_phase_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { run_phase_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { run_phase_heap::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { run_phase_heap::release(p); }

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Consumer = NativeExecutionConsumer;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = INT64_C(1736121600000);
std::size_t checks = 0, failures = 0;
const char* active_row = "allocator";

void check(const char* row, bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", row, __FILE__, line, expression);
}
#define ROW_CHECK(row, expression) check(row, static_cast<bool>(expression), #expression, __LINE__)
#define REQUIRE(expression) do {                                                \
    const bool run_phase_requirement = static_cast<bool>(expression);                 \
    check(active_row, run_phase_requirement, #expression, __LINE__);                   \
    if (!run_phase_requirement) throw std::runtime_error(#expression);                 \
} while (false)

template<class Action> void run_row(const char* row, Action&& action) {
    active_row = row;
    try { action(); }
    catch (const std::exception& error) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s\n", row,
                     __FILE__, __LINE__, error.what());
    } catch (...) {
        ++checks; ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: unknown exception\n", row, __FILE__, __LINE__);
    }
}

// Explicit-instantiation seam from test_id_adapter_storage.cpp's CohortFor.
// Data pointers are used only for reads. Function pointers call production.
template<class Tag, auto Member> struct Access { friend auto access(Tag) { return Member; } };
#define ADAPTER_ACCESS(Tag, member)                                              \
    struct Tag { friend auto access(Tag); };                                    \
    template struct Access<Tag, &Adapter::member>
ADAPTER_ACCESS(Cohorts, cohorts_by_id_);
ADAPTER_ACCESS(Placements, placement_);
ADAPTER_ACCESS(Live, live_handles_);
ADAPTER_ACCESS(Fees, open_entry_fees_);
ADAPTER_ACCESS(Debits, current_debited_applied_ordinals_);
ADAPTER_ACCESS(Shadowed, bracket_shadowed_openings_);
ADAPTER_ACCESS(Tombstones, exit_tombstones_);
ADAPTER_ACCESS(MarketAdds, market_pyramid_adds_);
ADAPTER_ACCESS(Reservations, compute_exit_reservation);
ADAPTER_ACCESS(Reconcile, reconcile_deferred_exit_reservations);
ADAPTER_ACCESS(CloseSized, run_close_sized_open_calls);
ADAPTER_ACCESS(LaggedShort, call_short_with_lagged_follow_up);
ADAPTER_ACCESS(Owing, open_call_owing_follow_up);
ADAPTER_ACCESS(ShortPathScope, short_path_points_scope);
ADAPTER_ACCESS(CloseSizedScope, close_sized_open_call_scope);
ADAPTER_ACCESS(WholeLagScope, whole_share_lag_scope);
ADAPTER_ACCESS(LagScope, lagged_short_scope);
ADAPTER_ACCESS(RetirementScratch, retired_row_scratch_);
#undef ADAPTER_ACCESS
struct ReadTarget { friend auto access(ReadTarget); };
struct PlacementUnits { friend auto access(PlacementUnits); };
template struct Access<ReadTarget, &Consumer::read_target>;
template struct Access<PlacementUnits, &Consumer::placement_scope_units>;

void allocator_control() {
    const auto before = run_phase_heap::sample();
    void* const a = ::operator new(7);
    void* const b = ::operator new[](11);
    void* const c = ::operator new(13, std::nothrow);
    void* const d = ::operator new[](17, std::nothrow);
    void* const e = ::operator new(19, std::align_val_t{64});
    void* const f = ::operator new[](23, std::align_val_t{64});
    void* const g = ::operator new(29, std::align_val_t{64}, std::nothrow);
    void* const h = ::operator new[](31, std::align_val_t{64}, std::nothrow);
    const auto held = run_phase_heap::sample();
    ROW_CHECK("allocator", a && b && c && d && e && f && g && h);
    ROW_CHECK("allocator", held.bytes == before.bytes + 7 + 11 + 13 + 17 + 19 + 23 + 29 + 31);
    ROW_CHECK("allocator", held.blocks == before.blocks + 8 && held.calls == before.calls + 8);
    ROW_CHECK("allocator", reinterpret_cast<std::uintptr_t>(e) % 64 == 0
              && reinterpret_cast<std::uintptr_t>(f) % 64 == 0
              && reinterpret_cast<std::uintptr_t>(g) % 64 == 0
              && reinterpret_cast<std::uintptr_t>(h) % 64 == 0);
    ::operator delete(a);
    ::operator delete[](b, std::size_t{11});
    ::operator delete(c, std::nothrow);
    ::operator delete[](d, std::nothrow);
    ::operator delete(e, std::size_t{19}, std::align_val_t{64});
    ::operator delete[](f, std::align_val_t{64});
    ::operator delete(g, std::align_val_t{64}, std::nothrow);
    ::operator delete[](h, std::align_val_t{64}, std::nothrow);
    ROW_CHECK("allocator", run_phase_heap::same_live(run_phase_heap::sample(), before));
}

std::vector<Bar> tape(std::size_t count) {
    std::vector<Bar> bars;
    bars.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        bars.push_back({100, 101, 99, 100, 1000, start + static_cast<std::int64_t>(i) * 60000});
    return bars;
}

struct NativeHost final : NativeStrategyHost {
    std::size_t bars = 0, opening_fills = 0;
    bool query_in_applied = false;
    no::CohortHandle cohort{};
    std::array<no::RequestHandle, 4> openings{};
    std::int64_t cycle = 0;
    no::RequestHandle closing{};
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bars != 1) return;
        cohort = cohort_open();
        for (std::size_t i = 0; i < openings.size(); ++i) {
            const auto placed = submit(no::Request{no::Transact{2}, "E", ""});
            REQUIRE(placed.status == no::SubmitStatus::Accepted && placed.handle.has_value());
            openings[i] = *placed.handle;
            cohort_add(cohort, openings[i]);
            const auto applied = execute_current({openings[i], NativeCurrentPriceRule::AsPresented});
            REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(applied));
            const auto& event = std::get<no::ExecutionAppliedEvent>(applied);
            REQUIRE(event.opened_units == 2 && event.closed_units == 0);
            cycle = event.cycle_after;
        }
        no::Request request{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "X", ""};
        request.owner = no::BindCohort{cohort};
        request.trigger = no::Limit{1000};
        const auto placed = submit(request);
        REQUIRE(placed.status == no::SubmitStatus::Accepted && placed.handle.has_value());
        closing = *placed.handle;
        native_queries(false);
        settlement_queries();
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        if (event.opened_units == 2) ++opening_fills;
        // Applied callbacks bypass the optional cohort index by source. This
        // forces read_target's OWN local handles, instead of the cache path.
        if (query_in_applied && opening_fills == openings.size()) native_queries(true);
    }
    void native_queries(bool applied_phase);
    void settlement_queries();
};

void NativeHost::native_queries(bool applied_phase) {
    auto& consumer_ref = consumer();
    // A copied live request is test input, prepared outside every window.
    // In Applied, create the close after all four opening receipts exist.
    if (applied_phase && closing.incarnation == 0) {
        no::Request request{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "X", ""};
        request.owner = no::BindCohort{cohort};
        request.trigger = no::Limit{1000};
        const auto placed = submit(request);
        REQUIRE(placed.status == no::SubmitStatus::Accepted && placed.handle.has_value());
        closing = *placed.handle;
    }
    const auto* found = consumer_ref.request_core().find_live(closing);
    REQUIRE(found != nullptr);
    const no::LiveRequest live = *found;
    no::Request by_openings{no::Reduce{no::ExplicitUnits{1}}, "Q", ""};
    by_openings.owner = no::BindOpenings{{openings.begin(), openings.end()}, cycle};
    no::Request by_cohort = by_openings;
    by_cohort.owner = no::BindCohort{cohort};
    no::Request independent = by_openings;
    independent.owner = no::Independent{};
    for (const std::size_t n : {std::size_t{16}, std::size_t{64}}) {
        const auto fixed = run_phase_heap::sample();
        for (std::size_t command = 0; command < n; ++command) {
            if (applied_phase) {
                const auto before = run_phase_heap::sample();
                bool correct = false;
                {
                    const auto target = (consumer_ref.*access(ReadTarget{}))(*this, &live);
                    correct = target.openings.size() == openings.size();
                    for (std::size_t i = 0; correct && i < openings.size(); ++i)
                        correct = target.openings[i].has_live_matching_lot
                            && target.openings[i].queried_opening == openings[i]
                            && target.openings[i].queried_cycle == cycle;
                } // Owning return is also gone before the release checkpoint.
                const auto after = run_phase_heap::sample();
                ROW_CHECK("nativeexecutionconsumer_read_target_handles", correct); // Four real fills / four returned handles.
                ROW_CHECK("nativeexecutionconsumer_read_target_handles", run_phase_heap::same_live(after, before));
            } else {
                auto before = run_phase_heap::sample();
                const auto selected = (consumer_ref.*access(PlacementUnits{}))(*this, by_openings);
                auto after = run_phase_heap::sample();
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_incarnations", selected.has_value() && *selected == 4 * 2);
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_incarnations", run_phase_heap::same_live(after, before));
                before = run_phase_heap::sample();
                const auto cohort_units = (consumer_ref.*access(PlacementUnits{}))(*this, by_cohort);
                after = run_phase_heap::sample();
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_handles", cohort_units.has_value() && *cohort_units == 4 * 2);
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_handles", run_phase_heap::same_live(after, before));
                // Control: Independent avoids both local selection buffers.
                before = run_phase_heap::sample();
                const auto book = (consumer_ref.*access(PlacementUnits{}))(*this, independent);
                after = run_phase_heap::sample();
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_incarnations", book.has_value() && *book == 4 * 2);
                ROW_CHECK("nativeexecutionconsumer_placement_scope_units_handles", run_phase_heap::same_live(after, before));
            }
        }
        const auto end = run_phase_heap::sample();
        if (applied_phase) {
            ROW_CHECK("nativeexecutionconsumer_read_target_handles", run_phase_heap::same_live(end, fixed));
            const auto before = run_phase_heap::sample();
            bool empty = false;
            {
                const auto target = (consumer_ref.*access(ReadTarget{}))(*this, nullptr);
                empty = target.openings.empty() && !target.opening;
            }
            ROW_CHECK("nativeexecutionconsumer_read_target_handles", empty && run_phase_heap::same_live(run_phase_heap::sample(), before));
        }
        else {
            ROW_CHECK("nativeexecutionconsumer_placement_scope_units_incarnations", run_phase_heap::same_live(end, fixed));
            ROW_CHECK("nativeexecutionconsumer_placement_scope_units_handles", run_phase_heap::same_live(end, fixed));
        }
    }
}

void NativeHost::settlement_queries() {
    // Four actual two-unit fills. Select openings 0 and 2: two closing
    // indices/quantities, three quoted costs (two closes + opening zero),
    // two survivors, two selected ids, and two staged closed-trade rows.
    ex::SelectedOpeningSet selection{cycle, {openings[0].incarnation, openings[2].incarnation}};
    const ex::Action action = ex::Flatten{};
    const ex::Fill fill{100, "X", "", 1000};
    struct Counting {
        Counting() { internal::count_settlement_paths(true); }
        ~Counting() { internal::count_settlement_paths(false); }
    } counting;
    const auto entry = static_cast<int>(internal::SettlementEntry::Project);
    for (const std::size_t n : {std::size_t{16}, std::size_t{64}}) {
        const auto fixed = run_phase_heap::sample();
        const auto paths_before = internal::settlement_path_counts();
        for (std::size_t command = 0; command < n; ++command) {
            const auto before = run_phase_heap::sample();
            const auto projection = project_native_settlement_selected_v1(action, fill, selection);
            const auto after = run_phase_heap::sample();
            ROW_CHECK("settlement_worksets", projection.status == ex::Status::Applied
                      && projection.closed_units == 2 * 2 && projection.opened_units == 0
                      && projection.resulting_abs_units == 2 * 2 && projection.resulting_lot_count == 2);
            ROW_CHECK("settlement_worksets", run_phase_heap::same_live(after, before));
            ROW_CHECK("settlement_worksets", pyramid_entries_.size() == 4
                      && physical_position().signed_units == 4 * 2); // Query did not settle.
        }
        const auto paths_after = internal::settlement_path_counts();
        ROW_CHECK("settlement_worksets", paths_after.staged[entry] - paths_before.staged[entry] == n
                  && paths_after.fused[entry] == paths_before.fused[entry]);
        ROW_CHECK("settlement_worksets", run_phase_heap::same_live(run_phase_heap::sample(), fixed));
    }
}

void native_family() {
    NativeHost host;
    host.query_in_applied = true;
    NativeRunSpec spec;
    spec.identity = {"RUN", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:RUN";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000;
    spec.price_tick = 0.25;
    spec.point_value = spec.account_fx = 1;
    const auto bars = tape(8);
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    ROW_CHECK("nativeexecutionconsumer_read_target_handles", host.opening_fills == 4 && host.bars == bars.size());
    ROW_CHECK("nativeexecutionconsumer_placement_scope_units_incarnations", host.last_error().empty());
    ROW_CHECK("nativeexecutionconsumer_placement_scope_units_handles", host.native_state().kind == NativeLifecycleKind::Completed);
}

enum class Script {
    FifoClose, Immediate, ExplicitExit, SpecialSell, CloseSized, LaggedShort,
    OwingShort, PoocPrefix, HashLive, HashBracket, HashDebit, Reservation
};

// Every member below is fixed-size test bookkeeping. In particular a fill
// callback records integers, not a growing copy of the applied-event journal.
class SourceFixture final : public source::PineStrategyHost {
public:
    Script script;
    bool control;
    std::size_t repetitions;
    std::size_t callbacks = 0, opens = 0, closes = 0, branch_calls = 0;
    std::size_t selected_close_fills = 0, prefix_members = 0, hash_calls = 0;
    std::array<std::uint64_t, 8> incarnations{};
    double closed_units = 0, margin_closed_units = 0;

    SourceFixture(Script value, bool without_branch, std::size_t commands)
        : script(value), control(without_branch), repetitions(commands) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000;
        config.margin_long = config.margin_short = 0;
        config.pyramiding = 8;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_CONTRACT);
        config.commission_value = 0.25;
        config.close_entries_rule_any = script == Script::Immediate || script == Script::HashDebit;
        config.process_orders_on_close = script == Script::PoocPrefix || script == Script::OwingShort;
        if (script == Script::CloseSized || script == Script::LaggedShort || script == Script::OwingShort) {
            config.margin_long = config.margin_short = 100;
            config.commission_type = static_cast<int>(CommissionType::PERCENT);
            config.commission_value = 0.05;
            config.slippage = 1;
            if (script == Script::OwingShort) config.initial_capital = 1000;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(script == Script::OwingShort ? 1.0 : 0.25);
        const double step = script == Script::OwingShort || script == Script::HashBracket
            || script == Script::HashLive || script == Script::Immediate
            || script == Script::HashDebit || script == Script::Reservation
            || script == Script::ExplicitExit || script == Script::SpecialSell ? 1.0 : 0.01;
        set_syminfo_metadata("qty_step", step);
    }

    Adapter& adapter() { return adapter_; }
    Consumer& consumer() { return as_native_consumer(execution_consumer()); }
    std::size_t lots() const { return pyramid_entries_.size(); }
    double held() const { return std::abs(physical_position().signed_units); }
    std::size_t opened(const char* id) const {
        const auto& cohorts = adapter_.*access(Cohorts{});
        const auto found = cohorts.find(id);
        return found == cohorts.end() ? 0 : found->second.opened.size();
    }
    double units(const char* id) const {
        const auto& cohorts = adapter_.*access(Cohorts{});
        const auto found = cohorts.find(id);
        double total = 0;
        if (found != cohorts.end())
            for (const auto& item : found->second.live_units_by_origin) total += item.second;
        return total;
    }
    std::size_t legs(const char* id, std::size_t* distinct_origins = nullptr) const {
        std::size_t count = 0, unique = 0;
        std::array<std::uint64_t, 8> origins{};
        const auto& placements = adapter_.*access(Placements{});
        for (const auto& handle : adapter_.*access(Live{})) {
            const auto found = placements.find(handle.incarnation);
            if (found == placements.end() || found->second.source_id != id) continue;
            const auto family = found->second.family;
            if (family != source::PineOrderFamily::ExitLimit
                && family != source::PineOrderFamily::ExitStop
                && family != source::PineOrderFamily::ExitTrail) continue;
            ++count;
            const auto origin = found->second.bracket_origin.incarnation;
            bool seen = false;
            for (std::size_t i = 0; i < unique; ++i) seen = seen || origins[i] == origin;
            if (!seen && unique < origins.size()) origins[unique++] = origin;
        }
        if (distinct_origins) *distinct_origins = unique;
        return count;
    }

    void on_source_bar(const Bar&) override {
        const auto bar = pine_bar_index();
        if (bar == 0) {
            callbacks = opens = closes = branch_calls = selected_close_fills = prefix_members = hash_calls = 0;
            incarnations.fill(0);
            closed_units = margin_closed_units = 0;
        }
        ++callbacks;
        switch (script) {
        case Script::FifoClose:
            if (bar == 0) {
                strategy_entry("E", true, na, na, 2);
                if (!control) strategy_entry("F", true, na, na, 2);
            } else if (bar == 1) {
                REQUIRE(opens == (control ? 1U : 2U) && opened("E") == 1);
                REQUIRE(lots() == (control ? 1U : 2U));
                strategy_close("E");
                ++branch_calls;
            } else if (bar == 3) strategy_close_all();
            break;
        case Script::Immediate:
        case Script::HashDebit:
            if (bar == 0) {
                strategy_entry("E", true, na, na, 2);
                strategy_entry("F", true, na, na, 2);
            } else if (bar == 1) {
                REQUIRE(opens == 2 && units("E") == 2 && lots() == 2);
                strategy_close("E", {}, 1, na, true);
                ++branch_calls;
                // execute_current books now; Applied is delivered when the
                // enclosing callback yields. This is the debit's real gap.
                if (script == Script::HashDebit) {
                    ROW_CHECK("pineexecutionadapter_hash_state_current_debit_ordinals", trade_count() == 1 && units("E") == 1);
                    ROW_CHECK("pineexecutionadapter_hash_state_current_debit_ordinals", (adapter_.*access(Debits{})).size() == 1);
                    hash_queries("pineexecutionadapter_hash_state_current_debit_ordinals");
                }
            } else if (bar == 2 && script == Script::HashDebit) {
                ROW_CHECK("pineexecutionadapter_hash_state_current_debit_ordinals", (adapter_.*access(Debits{})).empty());
                hash_queries("pineexecutionadapter_hash_state_current_debit_ordinals");
            } else if (bar == 3) strategy_close_all();
            break;
        case Script::ExplicitExit:
        case Script::Reservation:
            if (bar == 0 || bar == 1) strategy_entry("E", true, na, na, 2);
            else if (bar == 2) {
                REQUIRE(opens == 2 && lots() == 2 && opened("E") == 2 && units("E") == 4);
                strategy_exit("X", control ? "" : "E", 1000, na,
                              na, na, na, 100, {}, 1);
                ++branch_calls;
            } else if (bar == 3) {
                std::size_t origins = 0;
                const auto count = legs("X", &origins);
                ROW_CHECK("pineexecutionadapter_exit_submit_leg_origins", count == (control ? 1U : 2U));
                ROW_CHECK("pineexecutionadapter_exit_submit_leg_positions", origins == (control ? 1U : 2U));
                if (script == Script::Reservation) reservation_queries();
            }
            break;
        case Script::SpecialSell:
            if (bar == 0) {
                strategy_entry("E", true, na, na, 2);
                strategy_entry("F", true, na, na, 2);
            } else if (bar == 1) {
                strategy_exit("XE", "E", 1000, na, na, na, na, 100, {}, 1);
                strategy_exit("XF", "F", 1000, na, na, na, na, 100, {}, 1);
            } else if (bar == 2) {
                REQUIRE(opens == 2 && lots() == 2 && opened("E") == 1 && opened("F") == 1);
                REQUIRE(legs("XE") == 1 && legs("XF") == 1);
                strategy_entry("S", false); // First default market reversal.
                // Only the replacement takes the carried-bracket branch.
                if (!control) strategy_entry("S", false);
                ++branch_calls;
                ROW_CHECK("pineexecutionadapter_place_entry_carried_brackets", legs("XE") == (control ? 1U : 0U));
                ROW_CHECK("pineexecutionadapter_place_entry_carried_ids", legs("XF") == (control ? 1U : 0U));
                bool replaced = false;
                for (const auto& handle : adapter_.*access(Live{})) {
                    const auto found = (adapter_.*access(Placements{})).find(handle.incarnation);
                    if (found != (adapter_.*access(Placements{})).end()
                        && found->second.source_id == "S")
                        replaced = found->second.replaced_opening
                            && found->second.replacement_predecessor_market;
                }
                ROW_CHECK("pineexecutionadapter_place_entry_cohorts", replaced == !control);
            } else if (bar == 4) strategy_close_all();
            break;
        case Script::CloseSized:
        case Script::LaggedShort:
        case Script::OwingShort:
            margin_script(bar);
            break;
        case Script::PoocPrefix:
            prefix_script(bar);
            break;
        case Script::HashLive:
            if (bar == 0) strategy_entry("E", true, na, na, 2);
            else if (bar == 1 && !control) strategy_entry("F", true, na, na, 2);
            else if (bar == 2) {
                ROW_CHECK("pineexecutionadapter_hash_state_live_origin_keys", units("E") == 2 && opened("E") == 1);
                ROW_CHECK("pineexecutionadapter_hash_state_live_origin_keys", opens == (control ? 1U : 2U));
                const auto& adds = adapter_.*access(MarketAdds{});
                ROW_CHECK("pineexecutionadapter_hash_state_market_add_keys", adds.size() == (control ? 0U : 1U));
                if (!control) ROW_CHECK("pineexecutionadapter_hash_state_market_add_keys", adds.count(incarnations[1]) == 1);
                hash_queries("pineexecutionadapter_hash_state_live_origin_keys");
                hash_queries("pineexecutionadapter_hash_state_market_add_keys");
            }
            break;
        case Script::HashBracket:
            if (bar == 0) {
                strategy_entry("E", true, na, na, 2);
                strategy_entry("F", true, na, na, 2);
            } else if (bar == 1 && !control) {
                // FIFO takes one unit of E, while the bracket belongs to F.
                strategy_exit("X", "F", 100.5, na, na, na, na, 100, {}, 1);
            } else if (bar == 3 && control) {
                ROW_CHECK("pineexecutionadapter_hash_state_shadowed_openings", (adapter_.*access(Shadowed{})).empty());
                ROW_CHECK("pineexecutionadapter_hash_state_keys", (adapter_.*access(Tombstones{})).empty());
                hash_queries("pineexecutionadapter_hash_state_shadowed_openings");
                hash_queries("pineexecutionadapter_hash_state_keys");
            }
            break;
        }
    }

    void validate() const {
        ROW_CHECK(active_row, last_error().empty() && callbacks == 8);
        ROW_CHECK(active_row, native_state().kind == NativeLifecycleKind::Completed);
        switch (script) {
        case Script::FifoClose:
            ROW_CHECK(active_row, opens == (control ? 1U : 2U) && closes == (control ? 1U : 2U));
            ROW_CHECK(active_row, branch_calls == 1 && held() == 0);
            break;
        case Script::Immediate:
        case Script::HashDebit:
            // Two opens; E partial close; one close_all of E's remainder and F.
            ROW_CHECK(active_row, opens == 2 && closes == 2 && trade_count() == 3);
            ROW_CHECK(active_row, branch_calls == 1 && selected_close_fills == 1 && closed_units == 4 && held() == 0);
            if (script == Script::HashDebit) ROW_CHECK("pineexecutionadapter_hash_state_current_debit_ordinals", hash_calls == 2 * repetitions);
            break;
        case Script::ExplicitExit:
        case Script::Reservation:
            ROW_CHECK(active_row, opens == 2 && closes == 0 && branch_calls == 1 && held() == 4);
            break;
        case Script::SpecialSell:
            // The replacement sells its own one unit; the first definition
            // alone reverses four long units into one short unit. Both arms
            // have two closing fills and three physical closed-trade rows.
            ROW_CHECK(active_row, opens == (control ? 3U : 2U) && closes == 2 && trade_count() == 3);
            ROW_CHECK(active_row, closed_units == (control ? 5 : 4) && branch_calls == 1 && held() == 0);
            break;
        case Script::CloseSized:
            ROW_CHECK(active_row, opens == (control ? 1U : 3U) && margin_closed_units == 1);
            ROW_CHECK(active_row, branch_calls == 1 && closed_units == 6 && held() == 0);
            break;
        case Script::LaggedShort:
            ROW_CHECK(active_row, opens == 2 && margin_closed_units == 1);
            ROW_CHECK(active_row, branch_calls == 1 && closed_units == 4 && held() == 0);
            break;
        case Script::OwingShort:
            ROW_CHECK(active_row, opens == 1 && branch_calls == 1 && held() == 0);
            ROW_CHECK(active_row, margin_closed_units == (control ? 0 : 1));
            break;
        case Script::PoocPrefix:
            ROW_CHECK(active_row, opens == 3 && closes == 2 && trade_count() == 3 && held() == 0);
            ROW_CHECK(active_row, branch_calls == 1 && prefix_members == (control ? 0U : 2U));
            break;
        case Script::HashLive:
            ROW_CHECK(active_row, opens == (control ? 1U : 2U) && closes == 0 && hash_calls == 2 * repetitions);
            break;
        case Script::HashBracket:
            ROW_CHECK(active_row, opens == 2 && closes == (control ? 0U : 1U));
            ROW_CHECK(active_row, hash_calls == 2 * repetitions);
            break;
        }
    }

protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.opened_units != 0) {
            if (opens < incarnations.size()) incarnations[opens] = event.handle().incarnation;
            ++opens;
        }
        if (event.closed_units > 0) {
            ++closes;
            closed_units += event.closed_units;
            if (std::holds_alternative<no::SelectedExposure>(event.scope)
                || std::holds_alternative<ex::OpeningExposure>(event.scope)) {
                ++selected_close_fills;
                if (script == Script::Immediate) {
                    // The applied event's selection is nonempty; both fee
                    // and immediate-cohort consumption copy that real scope.
                    const auto* selection = std::get_if<no::SelectedExposure>(&event.scope);
                    ROW_CHECK("pineexecutionadapter_consume_opening_fees_selected", !selection || !selection->incarnations.empty());
                    ROW_CHECK("pineexecutionadapter_consume_cohort_units_selected", !selection || !selection->incarnations.empty());
                    if (closes == 1) {
                        const auto* opening = std::get_if<ex::OpeningExposure>(&event.scope);
                        const bool expected_origin = selection
                            ? selection->incarnations.size() == 1
                                && selection->incarnations.front() == incarnations[0]
                            : opening && opening->incarnation == incarnations[0];
                        ROW_CHECK("pineexecutionadapter_consume_opening_fees_selected", expected_origin && event.closed_units == 1);
                        ROW_CHECK("pineexecutionadapter_consume_cohort_units_selected", expected_origin && event.closed_units == 1);
                        double fee_units = 0, fee_money = 0;
                        for (const auto& fee : adapter_.*access(Fees{})) {
                            fee_units += fee.units;
                            fee_money += fee.nonpercent_fee;
                        }
                        // Two 2-unit opens pay 4*0.25; the selected one-unit
                        // close leaves 3 units and 3*0.25 of opening fees.
                        ROW_CHECK("pineexecutionadapter_consume_opening_fees_selected", fee_units == 3 && fee_money == 3 * 0.25);
                        ROW_CHECK("pineexecutionadapter_consume_cohort_units_selected", units("E") == 1 && units("F") == 2);
                    }
                }
            }
            if (event.request().comment == "Margin call")
                margin_closed_units += event.closed_units;
        }
        if (script == Script::HashBracket && !control && event.closed_units > 0) {
            const auto& shadows = adapter_.*access(Shadowed{});
            const auto& tombstones = adapter_.*access(Tombstones{});
            ROW_CHECK("pineexecutionadapter_hash_state_shadowed_openings", event.closed_units == 1 && units("E") == 1 && units("F") == 2);
            ROW_CHECK("pineexecutionadapter_hash_state_shadowed_openings", shadows.size() == 1 && shadows.count(incarnations[0]) == 1);
            const auto tombstone = tombstones.find(std::string("X") + '\x1f' + "F");
            ROW_CHECK("pineexecutionadapter_hash_state_keys", tombstones.size() == 1 && tombstone != tombstones.end());
            if (tombstone != tombstones.end()) ROW_CHECK("pineexecutionadapter_hash_state_keys", tombstone->second == incarnations[1]);
            hash_queries("pineexecutionadapter_hash_state_shadowed_openings");
            hash_queries("pineexecutionadapter_hash_state_keys");
        }
    }

private:
    void hash_queries(const char* row) {
        const auto fixed = run_phase_heap::sample();
        std::uint64_t first_hash = 0;
        for (std::size_t command = 0; command < repetitions; ++command) {
            BrokerStateHashSink sink;
            const auto initial = sink.h;
            const auto before = run_phase_heap::sample();
            adapter_.hash_state(sink);
            const auto after = run_phase_heap::sample();
            ROW_CHECK(row, sink.h != initial); // Independent evidence: actual fold returned.
            if (command == 0) first_hash = sink.h;
            ROW_CHECK(row, sink.h == first_hash);
            ROW_CHECK(row, run_phase_heap::same_live(after, before));
            ++hash_calls;
        }
        ROW_CHECK(row, run_phase_heap::same_live(run_phase_heap::sample(), fixed));
    }
    void reservation_queries() {
        // Two separate two-unit opening fills; one explicit one-unit leg
        // per origin in family X. X therefore reserves 2*1 = 2 out of 4.
        std::size_t origins = 0;
        REQUIRE(legs("X", &origins) == 2 && origins == 2 && units("E") == 4);
        const std::string next_exit = "Y", entry = "E";
        for (std::size_t command = 0; command < repetitions; ++command) {
            double percent = 100, reserved = na;
            auto before = run_phase_heap::sample();
            const bool accepted = (adapter_.*access(Reservations{}))(next_exit, entry, 1, percent, 4, 4, reserved);
            auto after = run_phase_heap::sample();
            ROW_CHECK("pineexecutionadapter_compute_exit_reservation_reservation_origins", accepted && reserved == 1 && percent == 1.0 / 4.0 * 100.0);
            ROW_CHECK("pineexecutionadapter_compute_exit_reservation_reservation_origins", run_phase_heap::same_live(after, before));
            before = run_phase_heap::sample();
            (adapter_.*access(Reconcile{}))(entry, 4);
            after = run_phase_heap::sample();
            ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_origins", run_phase_heap::same_live(after, before));
            std::size_t normalized = 0;
            for (const auto& handle : adapter_.*access(Live{})) {
                const auto found = (adapter_.*access(Placements{})).find(handle.incarnation);
                if (found != (adapter_.*access(Placements{})).end() && found->second.source_id == "X") {
                    ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_origins", found->second.projection_remaining_qty == 1
                              && found->second.qty_percent == 1.0 / 4.0 * 100.0);
                    ++normalized;
                }
            }
            ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_origins", normalized == 2); // Real family was scanned and rewritten.
        }
    }
    void margin_script(int bar) {
        if (bar == 0) {
            const bool is_long = script == Script::CloseSized || (script == Script::LaggedShort && control);
            if (script == Script::OwingShort) strategy_entry("E", false, na, na, 4);
            else if (script == Script::CloseSized && control) strategy_entry("E", true, na, na, 6);
            else {
                strategy_entry("E", is_long, na, na, 2);
                strategy_entry("F", is_long, na, na, 2);
                if (script == Script::CloseSized) strategy_entry("G", true, na, na, 2);
            }
        } else if (bar == 2) {
            const auto point = current_execution_point();
            REQUIRE(point.has_value());
            const auto context = point->decision;
            const auto count_before = trade_count();
            const double held_before = held();
            if (script == Script::CloseSized) {
                REQUIRE(lots() == (control ? 1U : 3U) && held_before == 6);
                const bool called = (adapter_.*access(CloseSized{}))(1, 100, context);
                ROW_CHECK("run_close_sized_open_calls_before_lots", called && trade_count() == count_before + 1 && held() == 5);
                ROW_CHECK("lot_by_lot_open_follow_up", source::detail::margin_opening_switches().lot_by_lot_open_follow_up);
                ROW_CHECK("pineexecutionadapter_run_close_sized_open_calls_taken", source::detail::margin_opening_switches().open_print_follow_up);
            } else if (script == Script::LaggedShort) {
                REQUIRE(lots() == 2 && held_before == 4);
                ROW_CHECK("pineexecutionadapter_call_short_with_lagged_follow_up_taken", (adapter_.*access(LagScope{}))());
                const double more = (adapter_.*access(LaggedShort{}))(100, 1, context, false);
                ROW_CHECK("pineexecutionadapter_call_short_with_lagged_follow_up_taken", more == 0 && trade_count() == count_before + 1 && held() == 3);
            } else {
                REQUIRE(lots() == 1 && held_before == 4);
                ROW_CHECK("pineexecutionadapter_open_call_owing_follow_up_taken", (adapter_.*access(ShortPathScope{}))(context));
                ROW_CHECK("pineexecutionadapter_open_call_owing_follow_up_taken", (adapter_.*access(CloseSizedScope{}))(true, false));
                ROW_CHECK("pineexecutionadapter_open_call_owing_follow_up_taken", (adapter_.*access(WholeLagScope{}))());
                // At 100 this short is solvent (control); at 190 its
                // requirement is 4*190, equity approximately 1000-4*91.
                // A one-share slice restores it and leaves a nonflat book.
                const bool called = (adapter_.*access(Owing{}))(control ? 100 : 190, context);
                ROW_CHECK("pineexecutionadapter_open_call_owing_follow_up_taken", control ? !called && held() == 4
                          : called && trade_count() == count_before + 1 && held() == 3);
            }
            ++branch_calls;
        } else if (bar == 4) strategy_close_all();
    }
    void prefix_script(int bar) {
        // Decimal 0.1+0.2 differs from literal 0.3 by one binary64 ULP.
        // The exact binary fractions in the control avoid that ambiguity.
        const double a = control ? 0.25 : 0.1;
        const double b = control ? 0.5 : 0.2;
        const double target = control ? 0.75 : 0.3;
        if (bar == 0) {
            strategy_entry("A", true, na, na, a);
            strategy_entry("B", true, na, na, b);
            strategy_entry("C", true, na, na, target);
        } else if (bar == 1) {
            REQUIRE(opens == 3 && lots() == 3 && opened("C") == 1);
            ROW_CHECK("pineexecutionadapter_flush_pending_closes_source_fifo_prefix_openings_included", (a + b == target) == control);
            strategy_close("C", {}, na, na, false, 11);
            adapter_.flush_pending_closes();
            ++branch_calls;
            std::size_t closes_found = 0;
            for (const auto& handle : adapter_.*access(Live{})) {
                const auto found = (adapter_.*access(Placements{})).find(handle.incarnation);
                if (found == (adapter_.*access(Placements{})).end()
                    || found->second.family != source::PineOrderFamily::Close) continue;
                ++closes_found;
                const auto* live = consumer().request_core().find_live(handle);
                REQUIRE(live != nullptr);
                const auto* selection = std::get_if<no::OpeningsClose>(&live->authority);
                if (selection) {
                    prefix_members = selection->openings.size();
                    ROW_CHECK("pineexecutionadapter_flush_pending_closes_source_fifo_prefix_openings_included", prefix_members == 2
                              && selection->openings[0].incarnation == incarnations[0]
                              && selection->openings[1].incarnation == incarnations[1]);
                }
                ROW_CHECK("pineexecutionadapter_flush_pending_closes_fifo_lots", found->second.close_callsite_token == 11);
                ROW_CHECK("pineexecutionadapter_flush_pending_closes_source_fifo_prefix_openings_included", (selection != nullptr) == !control);
            }
            ROW_CHECK("pineexecutionadapter_flush_pending_closes_fifo_lots", closes_found == 1);
        } else if (bar == 3) strategy_close_all();
    }
};

// A legitimate fill leaves a trade and adapter history. Repeat the SAME
// public run on the SAME host to restore exactly the same end-of-run state.
// The first complete cycle is the warm-up/reference, except for the explicit
// exit's two-buffer warm-up below. The owner stays alive between checkpoints.
// Counters never get reset here.
void steady_source(Script script, bool control, std::size_t n) {
    const auto bars = tape(8); // Test input is outside every measured span.
    SourceFixture host(script, control, n);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    host.validate();
    if (script == Script::ExplicitExit) {
        // release_closed_cohort_origins fills next_reissues and swaps it with
        // reissues. reset_for_run and RetiredRowScratch::clear clear sizes,
        // retaining both capacities.
        // The erase_retired_rows Release quiet-bar gate can stop
        // further sweeps once certified. One complete run need not warm BOTH
        // buffers: two named live legs supply two K3 candidates;
        // the global control is excluded by from_entry.empty().
        // Warm a second complete run, but require its entire live delta to
        // equal only the independently counted storage of these two buffers.
        // Thus an exit's retained origins/positions allocation, or
        // any other unexplained allocation, still fails during warm-up.
        const auto reissue_storage = [&] {
            const auto& scratch = host.adapter().*access(RetirementScratch{});
            using Memo = std::pair<std::uint64_t, std::uint64_t>;
            return run_phase_heap::Sample{
                (scratch.reissues.capacity() + scratch.next_reissues.capacity()) * sizeof(Memo),
                std::size_t{scratch.reissues.capacity() != 0}
                    + std::size_t{scratch.next_reissues.capacity() != 0},
                0, 0};
        };
        const auto before = run_phase_heap::sample();
        const auto storage_before = reissue_storage();
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        const auto after = run_phase_heap::sample();
        const auto storage_after = reissue_storage();
        host.validate();
        ROW_CHECK(active_row,
                  after.bytes + storage_before.bytes == before.bytes + storage_after.bytes
                  && after.blocks + storage_before.blocks == before.blocks + storage_after.blocks
                  && after.foreign == before.foreign);
        const auto& scratch = host.adapter().*access(RetirementScratch{});
        ROW_CHECK(active_row, scratch.reissues.size() == (control ? 0U : 2U));
    }
    const auto first = run_phase_heap::sample();
    for (std::size_t cycle = 2; cycle <= n; ++cycle) {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        const auto after = run_phase_heap::sample();
        host.validate();
        ROW_CHECK(active_row, run_phase_heap::same_live(after, first));
    }
    ROW_CHECK(active_row, run_phase_heap::same_live(run_phase_heap::sample(), first));
}

void source_case(Script script) {
    for (const std::size_t n : {std::size_t{16}, std::size_t{64}}) {
        if (script == Script::HashLive || script == Script::HashBracket
            || script == Script::HashDebit || script == Script::Reservation) {
            const auto bars = tape(8);
            SourceFixture host(script, false, n);
            host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
            host.validate();
            if (script == Script::HashLive || script == Script::HashBracket) {
                SourceFixture control(script, true, n);
                control.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
                control.validate();
            }
            continue;
        }
        steady_source(script, false, n);
        // Sibling branch control is a separate host with the same input.
        if (script == Script::FifoClose || script == Script::ExplicitExit
            || script == Script::SpecialSell || script == Script::CloseSized
            || script == Script::LaggedShort || script == Script::OwingShort
            || script == Script::PoocPrefix || script == Script::HashLive
            || script == Script::HashBracket)
            steady_source(script, true, n);
    }
}

} // namespace

int main() {
    run_phase_heap::test_thread = true;
    run_phase_heap::observing.store(true, std::memory_order_relaxed);
    run_row("allocator", allocator_control);
    for (const char* row : {"nativeexecutionconsumer_read_target_handles", "nativeexecutionconsumer_placement_scope_units_incarnations", "nativeexecutionconsumer_placement_scope_units_handles", "settlement_worksets"})
        run_row(row, native_family);
    struct Row { const char* id; Script script; };
    const Row rows[]{
        {"run_close_sized_open_calls_before_lots", Script::CloseSized},
        {"lot_by_lot_open_follow_up", Script::CloseSized},
        {"pineexecutionadapter_run_close_sized_open_calls_taken", Script::CloseSized},
        {"pineexecutionadapter_call_short_with_lagged_follow_up_taken", Script::LaggedShort},
        {"pineexecutionadapter_open_call_owing_follow_up_taken", Script::OwingShort},
        {"pineexecutionadapter_hash_state_live_origin_keys", Script::HashLive},
        {"pineexecutionadapter_hash_state_current_debit_ordinals", Script::HashDebit},
        {"pineexecutionadapter_hash_state_shadowed_openings", Script::HashBracket},
        {"pineexecutionadapter_hash_state_keys", Script::HashBracket},
        {"pineexecutionadapter_hash_state_market_add_keys", Script::HashLive},
        {"pineexecutionadapter_close_openings", Script::FifoClose},
        {"pineexecutionadapter_exit_submit_leg_origins", Script::ExplicitExit},
        {"pineexecutionadapter_exit_submit_leg_positions", Script::ExplicitExit},
        {"fifo_close_remaining_resolution", Script::FifoClose},
        {"pineexecutionadapter_consume_opening_fees_selected", Script::Immediate},
        {"pineexecutionadapter_compute_exit_reservation_reservation_origins", Script::Reservation},
        {"pineexecutionadapter_reconcile_deferred_exit_reservations_family_origins", Script::Reservation},
        {"pineexecutionadapter_consume_cohort_units_selected", Script::Immediate},
        {"pineexecutionadapter_place_entry_carried_brackets", Script::SpecialSell},
        {"pineexecutionadapter_place_entry_carried_ids", Script::SpecialSell},
        {"pineexecutionadapter_place_entry_cohorts", Script::SpecialSell},
        {"pineexecutionadapter_flush_pending_closes_fifo_lots", Script::PoocPrefix},
        {"pineexecutionadapter_flush_pending_closes_source_fifo_prefix_openings_included", Script::PoocPrefix},
    };
    for (const auto& row : rows) run_row(row.id, [&] { source_case(row.script); });
    std::printf("memory proof release_run_phases: %zu checks, %zu failures\n", checks, failures);
    run_phase_heap::observing.store(false, std::memory_order_relaxed);
    run_phase_heap::test_thread = false;
    return failures ? 1 : 0;
}
