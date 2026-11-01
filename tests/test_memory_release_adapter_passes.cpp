// Five query windows release at return with the host alive; stateful command
// groups show no per-run growth after one warm-up run (two for Declined/Gap/SameId).
// Check both live requested bytes and live blocks;
// neither equality relies on a measured byte allowance.
// This executable owns its allocation replacement; do not combine its TU
// with another executable's replacement or include a product .cpp here.
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

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
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Unlike the shared legacy allocation helper, this ledger identifies the
// test thread. A tagged allocation is debited even if another thread frees
// it. Foreign-thread activity is counted separately and MUST stay unchanged
// during every measured span; an asynchronous allocation cannot be hidden.
// No hook uses a C++ container, allocates bookkeeping, or resets live totals.
namespace adapter_pass_heap {
struct Ledger {
    std::atomic<std::size_t> bytes{0}, blocks{0}, allocations{0}, releases{0};
};
Ledger test;
Ledger foreign;
std::atomic<std::size_t> foreign_activity{0};
thread_local bool is_test_thread = false;

struct Header {
    void* base;
    std::size_t requested;
    bool test_owned;
};

void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) size = 1;
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto max = std::numeric_limits<std::size_t>::max();
    if (alignment - 1 > max - sizeof(Header)) return nullptr;
    const std::size_t overhead = sizeof(Header) + alignment - 1;
    if (size > max - overhead) return nullptr;
    void* base = std::malloc(size + overhead);
    if (!base) return nullptr;
    const auto first = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto aligned = (first + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    auto* block = reinterpret_cast<unsigned char*>(aligned);
    const Header header{base, size, is_test_thread};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    auto& ledger = header.test_owned ? test : foreign;
    ledger.bytes.fetch_add(size, std::memory_order_relaxed);
    ledger.blocks.fetch_add(1, std::memory_order_relaxed);
    ledger.allocations.fetch_add(1, std::memory_order_relaxed);
    if (!is_test_thread) foreign_activity.fetch_add(1, std::memory_order_relaxed);
    return block;
}

void release(void* block) noexcept {
    if (!block) return;
    Header header{};
    std::memcpy(&header, static_cast<unsigned char*>(block) - sizeof(Header), sizeof header);
    auto& ledger = header.test_owned ? test : foreign;
    ledger.bytes.fetch_sub(header.requested, std::memory_order_relaxed);
    ledger.blocks.fetch_sub(1, std::memory_order_relaxed);
    ledger.releases.fetch_add(1, std::memory_order_relaxed);
    if (!is_test_thread) foreign_activity.fetch_add(1, std::memory_order_relaxed);
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
} // namespace adapter_pass_heap

// All eight allocation forms and twelve corresponding release forms use
// the same prefix and allocator, including temporary buffers from the STL.
void* operator new(std::size_t n) { return adapter_pass_heap::throwing(n); }
void* operator new[](std::size_t n) { return adapter_pass_heap::throwing(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return adapter_pass_heap::nothrow(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return adapter_pass_heap::nothrow(n);
}
void* operator new(std::size_t n, std::align_val_t a) {
    return adapter_pass_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return adapter_pass_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return adapter_pass_heap::nothrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return adapter_pass_heap::nothrow(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { adapter_pass_heap::release(p); }
void operator delete[](void* p) noexcept { adapter_pass_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { adapter_pass_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { adapter_pass_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { adapter_pass_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { adapter_pass_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { adapter_pass_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { adapter_pass_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { adapter_pass_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { adapter_pass_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    adapter_pass_heap::release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    adapter_pass_heap::release(p);
}

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Family = source::PineOrderFamily;
using Handle = no::RequestHandle;
using Snapshot = source::PlacementSnapshot;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr int kN = 16, kFourN = 4 * kN;

// The existing explicit-instantiation seam, without a product accessor,
// friend, altered class declaration, or copied implementation unit.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct Entries { friend auto access(Entries); };
struct Brackets { friend auto access(Brackets); };
struct Rosters { friend auto access(Rosters); };
struct DeferredSells { friend auto access(DeferredSells); };
struct Retire { friend auto access(Retire); };
struct FlushSameBar { friend auto access(FlushSameBar); };
struct Waypoint { friend auto access(Waypoint); };
struct WithdrawWaypoint { friend auto access(WithdrawWaypoint); };
struct BookQueuedCall { friend auto access(BookQueuedCall); };
struct OpenQueuedCall { friend auto access(OpenQueuedCall); };
struct DeclinedStops { friend auto access(DeclinedStops); };
struct DeferSells { friend auto access(DeferSells); };
struct AdmitSells { friend auto access(AdmitSells); };
struct GapPolicy { friend auto access(GapPolicy); };
struct SameIdGap { friend auto access(SameIdGap); };
struct OrderLimits { friend auto access(OrderLimits); };
struct RefreshView { friend auto access(RefreshView); };
struct EnsureView { friend auto access(EnsureView); };
struct Indexes { friend auto access(Indexes); };
struct Next { friend auto access(Next); };
struct RawReads { friend auto access(RawReads); };
struct DualEntryPath { friend auto access(DualEntryPath); };
struct RetirementScratch { friend auto access(RetirementScratch); };
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<Entries, &Adapter::pending_entries_>;
template struct Access<Brackets, &Adapter::pending_bracket_legs_>;
template struct Access<Rosters, &Adapter::bracket_families_>;
template struct Access<DeferredSells, &Adapter::deferred_open_marketable_sells_>;
template struct Access<Retire, &Adapter::retire>;
template struct Access<FlushSameBar, &Adapter::flush_pending_same_bar_commands>;
template struct Access<Waypoint, &Adapter::rest_waypoint_margin_call>;
template struct Access<WithdrawWaypoint, &Adapter::withdraw_waypoint_margin_calls>;
template struct Access<BookQueuedCall, &Adapter::book_close_point_call>;
template struct Access<OpenQueuedCall, &Adapter::close_point_margin_call_at_open>;
template struct Access<DeclinedStops, &Adapter::defer_declined_reversal_exits_at_adverse>;
template struct Access<DeferSells, &Adapter::defer_open_marketable_sells>;
template struct Access<AdmitSells, &Adapter::admit_deferred_open_marketable_sells>;
template struct Access<GapPolicy, &Adapter::apply_reversal_gap_bracket_policy>;
template struct Access<SameIdGap, &Adapter::reaccept_gapped_bracket_behind_same_id_add>;
template struct Access<OrderLimits, &Adapter::order_open_marketable_limit_entries>;
template struct Access<RefreshView, &Adapter::refresh_pending_view>;
template struct Access<EnsureView, &Adapter::ensure_projected_pending_index>;
template struct Access<Indexes, &Adapter::projected_pending_indexes_>;
template struct Access<Next, &Adapter::projected_pending_next_>;
template struct Access<RawReads, &Adapter::projected_raw_pending_calls_>;
// Adapter::last_bar_dual_entry_path_ is private data;
// the public getter belongs to PendingIntentView, not Adapter.
template struct Access<DualEntryPath, &Adapter::last_bar_dual_entry_path_>;
template struct Access<RetirementScratch, &Adapter::retired_row_scratch_>;

enum class Case {
    Fixture, Roster, NativeAll, NativeWhere, Cancel, CancelAll, Waypoint,
    QueuedCall, Declined, Cap, Sells, Gap, SameId, Limits, Dual, Reversal, Index
};
struct Group {
    Case which;
    const char* name;
    std::array<const char*, 3> rows;
    int count;
    bool within_callback;
};
constexpr Group groups[] = {
    {Case::Fixture, "fixture_pending_snapshots", {"fixture_pending_snapshots_staged_parents", "fixture_pending_snapshots_pending_bracket_families", nullptr}, 2, true},
    {Case::Roster, "BracketRoster::remove_all", {"bracketroster_remove_all_incarnations", nullptr, nullptr}, 1, true},
    {Case::NativeAll, "NativeExecutionConsumer::cancel_all", {"nativeexecutionconsumer_cancel_all_handles", nullptr, nullptr}, 1, false},
    {Case::NativeWhere, "NativeExecutionConsumer::cancel_where", {"nativeexecutionconsumer_cancel_where_handles", nullptr, nullptr}, 1, false},
    {Case::Cancel, "PineExecutionAdapter::cancel", {"pineexecutionadapter_cancel_matches", "pineexecutionadapter_cancel_cancelled", nullptr}, 2, false},
    {Case::CancelAll, "PineExecutionAdapter::cancel_all", {"pineexecutionadapter_cancel_all_handles", "pineexecutionadapter_cancel_all_cancelled", nullptr}, 2, false},
    {Case::Waypoint, "withdraw_waypoint_margin_calls", {"pineexecutionadapter_withdraw_waypoint_margin_calls_stale", nullptr, nullptr}, 1, false},
    {Case::QueuedCall, "close_point_margin_call_at_open", {"pineexecutionadapter_close_point_margin_call_at_open_queued", "pineexecutionadapter_close_point_margin_call_at_open_kept", nullptr}, 2, false},
    {Case::Declined, "defer_declined_reversal_exits_at_adverse", {"pineexecutionadapter_defer_declined_reversal_exits_at_adverse_stops", nullptr, nullptr}, 1, false},
    {Case::Cap, "observe_intraday_cap", {"pineexecutionadapter_observe_intraday_cap_candidates", "pineexecutionadapter_observe_intraday_cap_inheritors", nullptr}, 2, false},
    {Case::Sells, "defer_open_marketable_sells", {"pineexecutionadapter_defer_open_marketable_sells_buys", "pineexecutionadapter_defer_open_marketable_sells_sells", "pineexecutionadapter_defer_open_marketable_sells_deferred"}, 3, false},
    {Case::Gap, "apply_reversal_gap_bracket_policy", {"pineexecutionadapter_apply_reversal_gap_bracket_policy_retired_legs", nullptr, nullptr}, 1, false},
    {Case::SameId, "reaccept_gapped_bracket_behind_same_id_add", {"pineexecutionadapter_reaccept_gapped_bracket_behind_same_id_add_gapped", nullptr, nullptr}, 1, false},
    {Case::Limits, "order_open_marketable_limit_entries", {"pineexecutionadapter_order_open_marketable_limit_entries_limits", nullptr, nullptr}, 1, true},
    {Case::Dual, "on_bar_open", {"pineexecutionadapter_on_bar_open_stops", nullptr, nullptr}, 1, true},
    {Case::Reversal, "on_applied", {"pineexecutionadapter_on_applied_preserved_cohorts", nullptr, nullptr}, 1, false},
    {Case::Index, "ensure_projected_pending_index", {"pineexecutionadapter_ensure_projected_pending_index_exits", nullptr, nullptr}, 1, true},
};

std::size_t checks = 0, failures = 0;
const Group* current = nullptr;
bool active_arm = false;
int cycle_number = 0, command_number = 0;

struct Heap {
    std::size_t bytes = 0, blocks = 0, foreign = 0;
};
Heap heap() noexcept {
    return {adapter_pass_heap::test.bytes.load(std::memory_order_relaxed),
            adapter_pass_heap::test.blocks.load(std::memory_order_relaxed),
            adapter_pass_heap::foreign_activity.load(std::memory_order_relaxed)};
}
bool same(const Heap& after, const Heap& before) noexcept {
    return after.bytes == before.bytes && after.blocks == before.blocks
        && after.foreign == before.foreign;
}

void check(const char* row, bool pass, const char* expression, int line,
           const Heap* after = nullptr, const Heap* before = nullptr) {
    ++checks;
    if (pass) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (function=%s arm=%s cycle=%d command=%d)",
                 row, __FILE__, line, expression, current ? current->name : "allocator",
                 active_arm ? "insert" : "control", cycle_number, command_number);
    if (after && before)
        std::fprintf(stderr, " live bytes=%zu/%zu blocks=%zu/%zu foreign=%zu/%zu",
                     after->bytes, before->bytes, after->blocks, before->blocks,
                     after->foreign, before->foreign);
    std::fputc('\n', stderr);
}
#define ROW(row, expression) check(row, bool(expression), #expression, __LINE__)
#define RELEASE(row) check(row, same(after, before), \
    "after.bytes == before.bytes && after.blocks == before.blocks && after.foreign == before.foreign", \
    __LINE__, &after, &before)

// Each member row has its own named assertion. Both live metrics must agree;
// an allocation count, an empty-only witness, or a work counter cannot pass it.
void release_rows(Case which, const Heap& after, const Heap& before) {
    switch (which) {
    case Case::Fixture:
        RELEASE("fixture_pending_snapshots_staged_parents");
        RELEASE("fixture_pending_snapshots_pending_bracket_families");
        break;
    case Case::Roster: RELEASE("bracketroster_remove_all_incarnations"); break;
    case Case::NativeAll: RELEASE("nativeexecutionconsumer_cancel_all_handles"); break;
    case Case::NativeWhere: RELEASE("nativeexecutionconsumer_cancel_where_handles"); break;
    case Case::Cancel:
        RELEASE("pineexecutionadapter_cancel_matches");
        RELEASE("pineexecutionadapter_cancel_cancelled");
        break;
    case Case::CancelAll:
        RELEASE("pineexecutionadapter_cancel_all_handles");
        RELEASE("pineexecutionadapter_cancel_all_cancelled");
        break;
    case Case::Waypoint: RELEASE("pineexecutionadapter_withdraw_waypoint_margin_calls_stale"); break;
    case Case::QueuedCall:
        RELEASE("pineexecutionadapter_close_point_margin_call_at_open_queued");
        RELEASE("pineexecutionadapter_close_point_margin_call_at_open_kept");
        break;
    case Case::Declined: RELEASE("pineexecutionadapter_defer_declined_reversal_exits_at_adverse_stops"); break;
    case Case::Cap:
        RELEASE("pineexecutionadapter_observe_intraday_cap_candidates");
        RELEASE("pineexecutionadapter_observe_intraday_cap_inheritors");
        break;
    case Case::Sells:
        RELEASE("pineexecutionadapter_defer_open_marketable_sells_buys");
        RELEASE("pineexecutionadapter_defer_open_marketable_sells_sells");
        RELEASE("pineexecutionadapter_defer_open_marketable_sells_deferred");
        break;
    case Case::Gap: RELEASE("pineexecutionadapter_apply_reversal_gap_bracket_policy_retired_legs"); break;
    case Case::SameId: RELEASE("pineexecutionadapter_reaccept_gapped_bracket_behind_same_id_add_gapped"); break;
    case Case::Limits: RELEASE("pineexecutionadapter_order_open_marketable_limit_entries_limits"); break;
    case Case::Dual: RELEASE("pineexecutionadapter_on_bar_open_stops"); break;
    case Case::Reversal: RELEASE("pineexecutionadapter_on_applied_preserved_cohorts"); break;
    case Case::Index: RELEASE("pineexecutionadapter_ensure_projected_pending_index_exits"); break;
    }
}

void evidence(bool pass, const char* expression, int line) {
    for (int i = 0; i < current->count; ++i)
        check(current->rows[static_cast<std::size_t>(i)], pass, expression, line);
}
#define EVIDENCE(expression) evidence(bool(expression), #expression, __LINE__)

std::array<Bar, 6> tape() {
    std::array<Bar, 6> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100.0, 100.0, 100.0, 100.0, 1.0,
                   kStart + static_cast<std::int64_t>(i) * 60000};
    return bars;
}

class SourceHost final : public source::PineStrategyHost {
public:
    SourceHost(Case which, bool active) : which_(which), active_(active) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 10;
        config.process_orders_on_close = which == Case::Cap;
        if (which == Case::Waypoint) {
            // Short 10 @99.75 on capital 2000 is solvent on real 100 bars.
            // The synthetic waypoint's call must also pass the slipped-short
            // restoring gate in short_call_vetoed, not merely
            // have positive shortfall; see the arithmetic in waypoint().
            config.initial_capital = 2000.0;
            config.margin_long = config.margin_short = 100.0;
            config.slippage = 1;
        }
        if (which == Case::Declined || which == Case::Gap) {
            config.initial_capital = 10000.0;
            config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            config.default_qty_value = 100.0;
            config.margin_long = config.margin_short = 100.0;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        if (which == Case::Waypoint) set_syminfo_metadata("qty_step", 1.0);
        if (which == Case::Declined || which == Case::Gap) set_margin_call_enabled(false);
    }

    int target_calls = 0, applied = 0, margin_fills = 0, reversal_fills = 0;
    double margin_closed = 0.0, reversal_closed = 0.0, reversal_opened = 0.0;
    void start_cycle() {
        target_calls = applied = margin_fills = reversal_fills = 0;
        margin_closed = reversal_closed = reversal_opened = 0.0;
    }

    void on_source_bar(const Bar& bar) override;

    Heap retirement_backing() const {
        // release_closed_cohort_origins exchanges the backing allocations
        // of the K3 memo and refill vector in retired_row_scratch_.
        const auto& scratch = adapter_.*access(RetirementScratch{});
        using Pair = std::pair<std::uint64_t, std::uint64_t>;
        return {(scratch.reissues.capacity() + scratch.next_reissues.capacity()) * sizeof(Pair),
                (scratch.reissues.capacity() != 0 ? 1U : 0U)
                    + (scratch.next_reissues.capacity() != 0 ? 1U : 0U), 0};
    }

private:
    Case which_;
    bool active_;
    int charged_before_close_ = 0;

    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        ++applied;
        if (event.request().comment == "Margin call") {
            ++margin_fills;
            margin_closed += event.closed_units;
        }
        if (event.request().label == "R") {
            ++reversal_fills;
            reversal_closed += event.closed_units;
            reversal_opened += event.opened_units;
        }
    }

    const auto& placements() const { return adapter_.*access(Placements{}); }
    const auto& handles() const { return adapter_.*access(Live{}); }
    std::size_t live_count(const char* id) const {
        std::size_t count = 0;
        for (const auto& handle : handles()) {
            const auto found = placements().find(handle.incarnation);
            if (found != placements().end() && found->second.source_id == id) ++count;
        }
        return count;
    }
    Handle handle_of(const char* id) const {
        for (const auto& handle : handles()) {
            const auto found = placements().find(handle.incarnation);
            if (found != placements().end() && found->second.source_id == id) return handle;
        }
        return {};
    }
    bool snapshot_is(const Handle& handle, Family family) const {
        const auto found = placements().find(handle.incarnation);
        return found != placements().end() && found->second.family == family;
    }
    void flush() {
        adapter_.flush_pending_entries();
        (adapter_.*access(FlushSameBar{}))(false, true);
        adapter_.flush_pending_bracket_legs();
    }
    // Cleanup uses individual native commands and the adapter's existing
    // retirement entry point. It never calls either bulk-cancel function or
    // source cancel(), keeping cleanup independent of those bulk paths.
    void cancel_live() {
        const auto working = native_working_requests();
        for (const auto& row : working) {
            const auto handle = row.definition->handle;
            if (cancel(handle).status == no::CancelStatus::Cancelled)
                (adapter_.*access(Retire{}))(handle);
        }
    }
    void close_later() { cancel_live(); strategy_close_all(); }
    void bracket(const char* id, const char* parent, double limit, double stop,
                 double units = kNa) {
        strategy_exit(id, parent, limit, stop, kNa, kNa, kNa, 100.0, "", units);
    }

    void fixture_query();
    void roster_release();
    void source_cancel();
    void waypoint(const Bar&, const NativeDecisionContext&);
    void queued_call(const Bar&, const NativeDecisionContext&);
    void declined(const NativeDecisionContext&);
    void cap_close();
    void defer_sells(const Bar&);
    void gap_policy(const NativeDecisionContext&);
    void same_id(const NativeDecisionContext&);
    void order_limits(const NativeDecisionContext&);
    void dual_stops(const NativeDecisionContext&);
    void reverse();
    void projected_index();
};

void SourceHost::on_source_bar(const Bar& bar) {
    const int index = pine_bar_index();
    if (index == 0) {
        if (which_ == Case::Cancel || which_ == Case::CancelAll) {
            if (which_ == Case::Cancel || active_)
                for (const char* id : {"A", "B", "C", "D"})
                    strategy_entry(id, true, 50.0, kNa, 1.0);
        } else if (which_ == Case::Sells || which_ == Case::Limits || which_ == Case::Dual) {
            // These three functions require a flat book at their call.
        } else {
            const bool short_book = which_ == Case::Waypoint || which_ == Case::QueuedCall;
            const bool percent = which_ == Case::Declined || which_ == Case::Gap;
            const double units = percent ? kNa
                : (short_book ? 10.0 : (which_ == Case::Index ? 4.0
                    : (which_ == Case::Roster || which_ == Case::Reversal ? 2.0 : 1.0)));
            if (which_ == Case::Cap) set_pine_risk_max_intraday_filled_orders(active_ ? 100 : 0);
            strategy_entry("L", !short_book, kNa, kNa, units);
        }
    }
    if (index == 2) {
        // NativeExecutionConsumer::execute_current settles inside the source
        // callback, but applied
        // notifications wait until it returns. Check the observer at the
        // following source bar, keeping its book/candidates alive meanwhile.
        if (which_ == Case::QueuedCall) {
            // submit_margin_call_units sums the sole saved unit
            // and submits it once: 10 short units minus 1 closed leaves 9.
            EVIDENCE(margin_fills == (active_ ? 1 : 0));
            EVIDENCE(margin_closed == (active_ ? 1.0 : 0.0));
            close_later();
            return;
        }
        if (which_ == Case::Cap) {
            // In on_applied, P/Q must still
            // be live when the close notification collects both worksets.
            EVIDENCE(live_count("P") == 1 && live_count("Q") == 1);
            // IntradayOrderBudget charges one committed
            // close; two nonmarket candidates supply no inheriting entry.
            EVIDENCE(adapter_.cap.budget().charged_slots()
                     == charged_before_close_ + (active_ ? 1 : 0));
            cancel_live();
            return;
        }
        if (which_ == Case::Reversal) {
            // R asks for 3 own units: opposite +2 -> -3 closes the held 2
            // and opens 3; the same-side control adds 3 to +2. Notification
            // delivery is the boundary cited above, not execute_current's return.
            EVIDENCE(reversal_fills == 1 && reversal_closed == (active_ ? 2.0 : 0.0));
            EVIDENCE(std::abs(reversal_opened) == 3.0);
            // In on_applied, the flipped physical side
            // and these new-side candidates must survive until on_applied
            // builds preserved_cohorts, before any cleanup close is issued.
            EVIDENCE(live_count("S0") == 1 && live_count("S1") == 1);
            close_later();
            return;
        }
    }
    const bool at_first_bar = which_ == Case::Sells || which_ == Case::Limits || which_ == Case::Dual;
    if (index != (at_first_bar ? 0 : 1)) return;
    const auto point = current_execution_point();
    EVIDENCE(point.has_value());
    if (!point) return; // unsafe fixture failure was recorded for every row
    const auto& context = point->decision;
    switch (which_) {
    case Case::Fixture: fixture_query(); break;
    case Case::Roster: roster_release(); break;
    case Case::Cancel:
    case Case::CancelAll: source_cancel(); break;
    case Case::Waypoint: waypoint(bar, context); break;
    case Case::QueuedCall: queued_call(bar, context); break;
    case Case::Declined: declined(context); break;
    case Case::Cap: cap_close(); break;
    case Case::Sells: defer_sells(bar); break;
    case Case::Gap: gap_policy(context); break;
    case Case::SameId: same_id(context); break;
    case Case::Limits: order_limits(context); break;
    case Case::Dual: dual_stops(context); break;
    case Case::Reversal: reverse(); break;
    case Case::Index: projected_index(); break;
    case Case::NativeAll:
    case Case::NativeWhere: EVIDENCE(false); break;
    }
}

void SourceHost::fixture_query() {
    EVIDENCE(live_position_size() == 1.0);
    if (active_) {
        // Same-direction default market adds are staged in pending_entries_
        // (submit_entry_with_policy). Each following whole-entry bracket
        // takes defer_for_same_bar_add_exit and stages its two priced legs.
        strategy_entry("P0", true);
        bracket("X0", "P0", 200.0, 50.0);
        strategy_entry("P1", true);
        bracket("X1", "P1", 210.0, 40.0);
    }
    EVIDENCE((adapter_.*access(Entries{})).size() == (active_ ? 2U : 0U));
    EVIDENCE((adapter_.*access(Brackets{})).size() == (active_ ? 4U : 0U));
    Heap at_n{};
    for (int call = 1; call <= kFourN; ++call) {
        command_number = call;
        std::array<std::size_t, 4> counts{};
        std::array<std::uint64_t, 4> incarnations{};
        bool valid = true;
        const auto before = heap();
        {
            const auto rows = adapter_.fixture_pending_snapshots();
            ++target_calls;
            // Independent command arithmetic: two staged parents plus ONE
            // projected row for each of the two two-leg bracket families.
            valid = rows.size() == (active_ ? 2U + 2U : 0U);
            for (const auto& row : rows) {
                int slot = -1;
                if (row.snapshot.source_id == "P0") slot = 0;
                if (row.snapshot.source_id == "P1") slot = 1;
                if (row.snapshot.source_id == "X0") slot = 2;
                if (row.snapshot.source_id == "X1") slot = 3;
                valid = valid && row.staged && slot >= 0;
                if (slot >= 0) {
                    ++counts[static_cast<std::size_t>(slot)];
                    incarnations[static_cast<std::size_t>(slot)] = row.incarnation;
                }
            }
        } // returned owning projection is gone; the adapter itself is untouched
        const auto after = heap();
        release_rows(which_, after, before);
        if (active_) {
            // Every exit call immediately follows its parent command: its
            // command-sequence offset is 1. This tests the staged-parent
            // search as well as family deduplication, without naming a local.
            valid = valid && counts == std::array<std::size_t, 4>{{1, 1, 1, 1}}
                && incarnations[0] != 0 && incarnations[1] != 0
                && incarnations[2] == incarnations[0] + 1
                && incarnations[3] == incarnations[1] + 1;
        }
        EVIDENCE(valid);
        if (call == kN) at_n = after;
        if (call == kFourN) release_rows(which_, after, at_n);
    }
    command_number = 0;
    // The staged parents are allowed to fill through the ordinary broker
    // after this callback. No query-owned allocation remains alive here.
}

void SourceHost::roster_release() {
    EVIDENCE(live_position_size() == 2.0);
    bracket("X", "L", 200.0, kNa, 1.0);
    bracket("Y", "L", 210.0, kNa, 1.0);
    flush();
    const auto x = handle_of("X"), y = handle_of("Y");
    EVIDENCE(x.incarnation != 0 && y.incarnation != 0 && x != y);
    source::BracketRoster* x_roster = nullptr;
    source::BracketRoster* y_roster = nullptr;
    for (auto& family : adapter_.*access(Rosters{})) {
        if (family.second.count(x.incarnation) != 0) x_roster = &family.second;
        if (family.second.count(y.incarnation) != 0) y_roster = &family.second;
    }
    EVIDENCE(x_roster && y_roster && x_roster != y_roster);
    if (!x_roster || !y_roster || x_roster == y_roster) return;
    EVIDENCE(x_roster->size() == 1 && y_roster->size() == 1);
    // This list is prepared outside every measured call. It consists of a
    // real never-filled bracket handle twice, so incarnation deduplication
    // has one insertion, not two. No simulated roster is populated here.
    std::vector<Handle> doomed;
    if (active_) {
        doomed = {x, x};
        const auto result = cancel(x);
        EVIDENCE(result.status == no::CancelStatus::Cancelled);
        if (result.status == no::CancelStatus::Cancelled)
            (adapter_.*access(Retire{}))(x);
        x_roster->remove_all(doomed); // sharing>0 arm, real family loses its one member
        EVIDENCE(x_roster->empty() && live_count("X") == 0);
    }
    const auto baseline = heap();
    Heap at_n{};
    for (int call = 1; call <= kFourN; ++call) {
        command_number = call;
        const auto before = heap();
        // Positive local insertion also precedes sharing==0's early return.
        // Y is a distinct real family; its sole handle must survive intact.
        y_roster->remove_all(doomed);
        ++target_calls;
        const auto after = heap();
        release_rows(which_, after, before);
        release_rows(which_, after, baseline);
        EVIDENCE(y_roster->size() == 1 && y_roster->count(y.incarnation) == 1);
        EVIDENCE(y_roster->working_tail().size() == 1
                 && y_roster->working_tail().front() == y);
        if (call == kN) at_n = after;
        if (call == kFourN) release_rows(which_, after, at_n);
    }
    command_number = 0;
    close_later();
}

void SourceHost::source_cancel() {
    const auto before = native_working_requests().size();
    EVIDENCE(before == (which_ == Case::CancelAll && !active_ ? 0U : 4U));
    EVIDENCE(live_position_size() == 0.0 && applied == 0);
    if (which_ == Case::Cancel) strategy_cancel(active_ ? "A" : "absent");
    else strategy_cancel_all();
    ++target_calls;
    const auto after = native_working_requests().size();
    const std::size_t removed = which_ == Case::Cancel ? (active_ ? 1U : 0U) : before;
    EVIDENCE(after == before - removed);
    EVIDENCE(live_count("A") == (active_ ? 0U : (which_ == Case::Cancel ? 1U : 0U)));
    EVIDENCE(live_position_size() == 0.0 && applied == 0);
    cancel_live();
    EVIDENCE(native_working_requests().empty());
}

void SourceHost::waypoint(const Bar& bar, const NativeDecisionContext& context) {
    EVIDENCE(live_position_size() == -10.0);
    Bar path = bar;
    // In rest_waypoint_margin_call and short_call_vetoed:
    // at H175, equity=2000+10*(99.75-175)=1247.5, required=1750;
    // floor((1750-1247.5)/175)*4=8 units. Their slipped value
    // 8*175.25=1402 exceeds 10*175.25-1247.5=505, so the gate admits.
    // H300 instead gives equity=-2.5: even all 10 units cannot restore
    // that book, and the gate correctly vetoes the old stimulus.
    if (active_) { path.high = 175.0; path.low = 25.0; }
    const bool placed = (adapter_.*access(Waypoint{}))(path, 1, 3, context);
    EVIDENCE(placed == active_);
    // The helper itself submitted this Margin request; no placement flag is
    // written by the test. It is still pending, never consumed by a matcher.
    std::size_t margin_rows = 0, waypoint_rows = 0;
    for (const auto& handle : handles()) {
        const auto found = placements().find(handle.incarnation);
        if (found == placements().end() || found->second.family != Family::Margin) continue;
        ++margin_rows;
        waypoint_rows += found->second.waypoint_margin_call ? 1U : 0U;
    }
    EVIDENCE(margin_rows == (active_ ? 1U : 0U) && waypoint_rows == margin_rows);
    const auto working = native_working_requests().size();
    (adapter_.*access(WithdrawWaypoint{}))();
    ++target_calls;
    EVIDENCE(native_working_requests().size() == working - margin_rows);
    EVIDENCE(margin_fills == 0 && live_position_size() == -10.0);
    close_later();
}

void SourceHost::queued_call(const Bar& bar, const NativeDecisionContext& context) {
    EVIDENCE(live_position_size() == -10.0);
    if (active_) {
        // A one-unit follow-up with a saved fill of 100 is taken at an open
        // of 100 (buy: open <= saved fill). Queuing uses the production
        // margin-order constructor, including its prior-bar timestamp flag.
        const bool booked = (adapter_.*access(BookQueuedCall{}))(100.0, 1.0, context, true, 100.0);
        EVIDENCE(booked);
    }
    const auto queued = native_working_requests().size();
    EVIDENCE(queued == (active_ ? 1U : 0U));
    if (active_) {
        const auto& row = placements().at(handles().front().incarnation);
        EVIDENCE(row.family == Family::Margin && row.crosses_zero
                 && row.placement_script_open_ms == bar.timestamp);
    }
    Bar opening = bar;
    opening.timestamp += 60000;
    // No ordinary market order waits behind it: the kept call is withdrawn
    // and executed through submit_margin_call_units, yielding a real fill.
    const bool behind = (adapter_.*access(OpenQueuedCall{}))(opening, bar, context);
    ++target_calls;
    EVIDENCE(!behind);
    // Observer assertions run at source bar 2, after execute_current's
    // deferred notification delivery.
    EVIDENCE(live_position_size() == (active_ ? -9.0 : -10.0));
    EVIDENCE(native_working_requests().empty());
}

void SourceHost::declined(const NativeDecisionContext& context) {
    // One all-equity long: 10000/100 = 100 units. A default short signal at
    // 100 freezes 100 units; at 101 its required 10100 exceeds equity 10000.
    EVIDENCE(live_position_size() == 100.0);
    bracket("X", "L", kNa, 90.0);
    strategy_entry("S", false);
    flush();
    const auto s = handle_of("S");
    EVIDENCE(live_count("X") == 1 && s.incarnation != 0);
    const auto found = placements().find(s.incarnation);
    EVIDENCE(found != placements().end());
    if (found != placements().end()) {
        EVIDENCE(found->second.reverse_to && !found->second.projection_after_close
                 && std::isnan(found->second.requested_qty)
                 && found->second.sizing.frozen_units == 100.0);
    }
    const Bar adverse{active_ ? 101.0 : 100.0, 102.0, 89.0, 100.0, 1.0,
                      context.script_bar_open_ms};
    (adapter_.*access(DeclinedStops{}))(adverse, context, false);
    ++target_calls;
    EVIDENCE(live_count("X") == (active_ ? 0U : 1U));
    EVIDENCE(live_position_size() == 100.0 && trade_count() == 0);
    close_later();
}

void SourceHost::cap_close() {
    EVIDENCE(live_position_size() == 1.0);
    strategy_entry("P", true, 50.0, kNa, 1.0);
    strategy_entry("Q", true, 40.0, kNa, 1.0);
    flush();
    EVIDENCE(live_count("P") == 1 && live_count("Q") == 1);
    const int charged = adapter_.cap.budget().charged_slots();
    charged_before_close_ = charged;
    EVIDENCE(charged == (active_ ? 1 : 0));
    // Both never-filled limits precede the full close command. There are
    // two continuation candidates and two sequence-qualified inheritors;
    // neither is a market continuation, so neither can execute here.
    strategy_close("L", "pass close", kNa, 100.0, true);
    ++target_calls;
    EVIDENCE(live_position_size() == 0.0 && trade_count() == 1);
    // Check the unchanged two-candidate population and the added cap slot at
    // source bar 2, after on_applied.
}

void SourceHost::defer_sells(const Bar& bar) {
    // The older sell and later buy are both marketable at 100 in the insert
    // arm: 100<=110 for the sell, 100>=90 for the buy. The control has the
    // same pair but first opens one real long, taking the nonflat early return.
    if (!active_) {
        strategy_entry("held", true, kNa, kNa, 1.0);
        flush();
        const auto outcome = execute_current({handle_of("held"), NativeCurrentPriceRule::NearestTick});
        EVIDENCE(std::holds_alternative<no::ExecutionAppliedEvent>(outcome));
    }
    strategy_entry("sell", false, kNa, 110.0, 1.0);
    // PineExecutionAdapter::entry stages a priced reversal on the held
    // long, while the same-side buy submits directly. Flush that older sell
    // before issuing the buy so the control has the stated native ordering.
    if (!active_) flush();
    strategy_entry("buy", true, kNa, 90.0, 1.0);
    flush();
    EVIDENCE(live_position_size() == (active_ ? 0.0 : 1.0)
             && live_count("sell") == 1 && live_count("buy") == 1);
    EVIDENCE(handle_of("sell").incarnation < handle_of("buy").incarnation);
    Bar path = bar;
    path.high = 105.0; path.low = 95.0;
    (adapter_.*access(DeferSells{}))(path);
    ++target_calls;
    const auto& deferred = adapter_.*access(DeferredSells{});
    EVIDENCE(deferred.size() == (active_ ? 1U : 0U));
    EVIDENCE(live_count("buy") == 1 && live_count("sell") == (active_ ? 0U : 1U));
    if (!deferred.empty())
        EVIDENCE(deferred.front().snapshot.source_id == "sell" && deferred.front().open_marketable);
    // Cancel the still-native candidates individually, then consume the
    // deferrer's real queued sell, keeping a later opposite fill out of this
    // family's on_applied path. This is part of the repeatable run cycle.
    cancel_live();
    (adapter_.*access(AdmitSells{}))();
    EVIDENCE((adapter_.*access(DeferredSells{})).empty());
    EVIDENCE(live_position_size() == (active_ ? -1.0 : 1.0));
    strategy_close_all();
}

void SourceHost::gap_policy(const NativeDecisionContext& context) {
    EVIDENCE(live_position_size() == 100.0);
    bracket("X", "L", kNa, 95.0);
    strategy_entry("S", false);
    flush();
    const auto old = handle_of("X"), opposite = handle_of("S");
    EVIDENCE(snapshot_is(old, Family::ExitStop) && opposite.incarnation != 0);
    const auto entry = placements().find(opposite.incarnation);
    EVIDENCE(entry != placements().end());
    if (entry != placements().end())
        EVIDENCE(entry->second.opening && std::isnan(entry->second.requested_qty)
                 && entry->second.sizing.frozen_units == 100.0);
    // At 90 the short's 100*90 requirement is affordable; the gapped long
    // stop at 95 therefore takes the reorder_priced_leg branch, which holds
    // a RetiredLeg, cancels its old handle and submits a stop at the low 80.
    // At 100 the same stop has no gap and the RetiredLeg vector stays empty.
    const Bar opening{active_ ? 90.0 : 100.0, 110.0, 80.0, 100.0, 1.0,
                      context.script_bar_open_ms};
    NativeDecisionContext next = context;
    ++next.coordinate.interval_index; // source_bar is this signal's bar
    (adapter_.*access(GapPolicy{}))(opening, next, false);
    ++target_calls;
    const auto fresh = handle_of("X");
    EVIDENCE(live_count("X") == 1 && (active_ ? fresh != old : fresh == old));
    const auto leg = placements().find(fresh.incarnation);
    EVIDENCE(leg != placements().end());
    if (active_ && leg != placements().end())
        EVIDENCE(leg->second.forced_execution_price == 90.0);
    if (active_) {
        bool reissued = false;
        for (const auto& row : native_working_requests()) {
            if (row.definition->handle != fresh) continue;
            const auto* stop = std::get_if<no::Stop>(&row.definition->request.trigger);
            // Half-up on a quarter tick: all prints just below 80.125
            // project to 80. The sell-stop boundary is the last such print.
            reissued = stop && stop->price > 80.0 && stop->price < 80.0 + 0.25 / 2
                && std::floor(stop->price / 0.25 + 0.5) == 80.0 / 0.25;
        }
        EVIDENCE(reissued);
    }
    EVIDENCE(live_position_size() == 100.0 && trade_count() == 0);
    close_later();
}

void SourceHost::same_id(const NativeDecisionContext& context) {
    EVIDENCE(live_position_size() == 1.0);
    bracket("X", "L", 110.0, kNa);
    adapter_.flush_pending_bracket_legs();
    const auto old = handle_of("X");
    strategy_entry("L", true); // default same-id add is staged, then really submitted
    flush();
    const auto add = handle_of("L");
    EVIDENCE(snapshot_is(old, Family::ExitLimit) && add.incarnation > old.incarnation);
    const auto leg_before = placements().find(old.incarnation);
    EVIDENCE(leg_before != placements().end());
    if (leg_before != placements().end())
        EVIDENCE(leg_before->second.deferred_cohort && std::isnan(leg_before->second.requested_qty));
    const Bar opening{active_ ? 120.0 : 100.0, 125.0, 95.0, 100.0, 1.0,
                      context.script_bar_open_ms};
    (adapter_.*access(SameIdGap{}))(opening, context);
    ++target_calls;
    const auto fresh = handle_of("X");
    EVIDENCE(live_count("X") == 1 && (active_ ? fresh.incarnation > add.incarnation : fresh == old));
    const auto found = placements().find(fresh.incarnation);
    EVIDENCE(found != placements().end());
    if (active_ && found != placements().end())
        EVIDENCE(found->second.forced_execution_price == 120.0);
    EVIDENCE(live_position_size() == 1.0 && trade_count() == 0);
    close_later();
}

void SourceHost::order_limits(const NativeDecisionContext& context) {
    strategy_entry("high", true, 130.0, kNa, 1.0);
    strategy_entry("low", true, 120.0, kNa, 1.0);
    strategy_entry("market", true, kNa, kNa, 1.0);
    flush();
    const auto high = handle_of("high"), low = handle_of("low"), market = handle_of("market");
    EVIDENCE(high.incarnation != 0 && low.incarnation != 0 && market.incarnation != 0);
    EVIDENCE(live_position_size() == 0.0 && native_working_requests().size() == 3);
    const Bar opening{active_ ? 100.0 : 200.0, 200.0, 100.0, 100.0, 1.0,
                      context.script_bar_open_ms};
    (adapter_.*access(OrderLimits{}))(opening, context); // warm reprice; priority 0 means incarnation
    std::uint64_t hp = high.incarnation, lp = low.incarnation, mp = market.incarnation;
    for (const auto& row : native_working_requests()) {
        if (row.definition->handle == high && row.definition->priority != 0) hp = row.definition->priority;
        if (row.definition->handle == low && row.definition->priority != 0) lp = row.definition->priority;
        if (row.definition->handle == market && row.definition->priority != 0) mp = row.definition->priority;
    }
    EVIDENCE(active_ ? mp < lp && lp < hp : hp < lp && lp < mp);
    if (active_) {
        EVIDENCE(placements().at(high.incarnation).forced_execution_price == 100.0);
        EVIDENCE(placements().at(low.incarnation).forced_execution_price == 100.0);
    }
    const auto baseline = heap();
    Heap at_n{};
    for (int call = 1; call <= kFourN; ++call) {
        command_number = call;
        const auto before = heap();
        // Even when now in order, the function first fills and sorts its
        // two-element limits vector, then returns through in_order.
        (adapter_.*access(OrderLimits{}))(opening, context);
        ++target_calls;
        const auto after = heap();
        release_rows(which_, after, before);
        release_rows(which_, after, baseline);
        EVIDENCE(live_count("high") == 1 && live_count("low") == 1 && live_count("market") == 1);
        if (call == kN) at_n = after;
        if (call == kFourN) release_rows(which_, after, at_n);
    }
    command_number = 0;
    cancel_live();
}

void SourceHost::dual_stops(const NativeDecisionContext& context) {
    strategy_entry("buy", true, kNa, 105.0, 1.0);
    strategy_entry("sell", false, kNa, 95.0, 1.0);
    flush();
    EVIDENCE(live_position_size() == 0.0 && live_count("buy") == 1 && live_count("sell") == 1);
    // Neither stop is marketable at 100, so the sell deferrer is quiet.
    // A symmetric [94,106] bar touches both; Auto visits the low first.
    const Bar opening{100.0, active_ ? 106.0 : 104.0, active_ ? 94.0 : 96.0,
                      100.0, 1.0, context.script_bar_open_ms};
    adapter_.on_bar_open(opening, context); // warm-up initializes ordinary open caches
    EVIDENCE((adapter_.*access(DualEntryPath{})) == (active_ ? 2 : 0));
    const auto baseline = heap();
    Heap at_n{};
    for (int call = 1; call <= kFourN; ++call) {
        command_number = call;
        const auto before = heap();
        adapter_.on_bar_open(opening, context);
        ++target_calls;
        const auto after = heap();
        release_rows(which_, after, before);
        release_rows(which_, after, baseline);
        EVIDENCE((adapter_.*access(DualEntryPath{})) == (active_ ? 2 : 0));
        EVIDENCE(live_position_size() == 0.0 && native_working_requests().size() == 2);
        if (call == kN) at_n = after;
        if (call == kFourN) release_rows(which_, after, at_n);
    }
    command_number = 0;
    cancel_live();
}

void SourceHost::reverse() {
    EVIDENCE(live_position_size() == 2.0);
    strategy_entry("S0", false, 200.0, kNa, 1.0);
    strategy_entry("S1", false, 210.0, kNa, 1.0);
    flush();
    EVIDENCE(live_count("S0") == 1 && live_count("S1") == 1);
    strategy_entry("R", !active_, kNa, kNa, 3.0);
    flush();
    const auto handle = handle_of("R");
    EVIDENCE(handle.incarnation != 0);
    const auto outcome = execute_current({handle, NativeCurrentPriceRule::NearestTick});
    ++target_calls;
    EVIDENCE(std::holds_alternative<no::ExecutionAppliedEvent>(outcome));
    EVIDENCE(live_position_size() == (active_ ? -3.0 : 5.0));
    // Leave this physical side and S0/S1 intact until the queued R event has
    // reached on_applied after execute_current; the observer/survival checks
    // and cleanup are at bar 2.
}

void SourceHost::projected_index() {
    EVIDENCE(live_position_size() == 4.0);
    if (active_) {
        bracket("X", "L", 200.0, 50.0, 1.0);
        bracket("Y", "L", 210.0, 40.0, 1.0);
        flush();
    }
    // Two ids with two priced legs each: four raw requests, two projected
    // groups. Their first slots are 0/2 and sibling links 0->1, 2->3.
    const std::size_t raw = active_ ? 2U * 2U : 0U;
    EVIDENCE(handles().size() == raw);
    (adapter_.*access(RefreshView{}))();
    EVIDENCE((adapter_.*access(EnsureView{}))());
    const auto baseline = heap(); // only the two published integer vectors persist
    Heap at_n{};
    for (int call = 1; call <= kFourN; ++call) {
        command_number = call;
        const auto reads = adapter_.*access(RawReads{});
        (adapter_.*access(RefreshView{}))();
        const bool ready = (adapter_.*access(EnsureView{}))();
        ++target_calls;
        const auto after = heap();
        release_rows(which_, after, baseline);
        EVIDENCE(ready && (adapter_.*access(RawReads{})) - reads == raw);
        const auto& indexes = adapter_.*access(Indexes{});
        const auto& next = adapter_.*access(Next{});
        EVIDENCE(indexes.size() == (active_ ? 2U : 0U) && next.size() == raw);
        if (indexes.size() == 2 && next.size() == 4)
            EVIDENCE(indexes[0] == 0 && indexes[1] == 2
                     && next[0] == 1 && next[1] == -1 && next[2] == 3 && next[3] == -1);
        if (call == kN) at_n = after;
        if (call == kFourN) release_rows(which_, after, at_n);
    }
    command_number = 0;
    close_later();
}

class NativeHost final : public NativeStrategyHost {
public:
    NativeHost(Case which, bool active) : which_(which), active_(active) {}
    int target_calls = 0, applied = 0;

    void start_cycle(std::uint64_t number) {
        target_calls = applied = 0;
        NativeRunSpec spec;
        spec.identity = {"pass-native", number};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:PASS";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 1000000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.fee_kind = NativeFeeKind::CashPerExecution;
        spec.fee_value = 0.0;
        spec.close_execution = NativeCloseExecution::AfterCalculation;
        spec.event_retention = NativeEventRetention::Full;
        EVIDENCE(configure_native(spec).status == NativeSetupStatus::Applied);
        // The identity changes numerically, never textually: the same host's
        // native consumed-high-water requires a greater run_number each time.
    }

    void on_native_run_begin() override {
        if (which_ == Case::NativeAll && !active_) return;
        int i = 0;
        for (const char* label : {"A", "B", "C", "D"}) {
            no::Request request{no::Transact{1.0}, label, i++ < 3 ? "drop" : "keep"};
            request.trigger = no::Limit{50.0};
            const auto accepted = submit(request);
            EVIDENCE(accepted.status == no::SubmitStatus::Accepted && accepted.handle.has_value());
        }
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (target_calls != 0) return;
        const auto before = native_working_requests().size();
        EVIDENCE(before == (which_ == Case::NativeAll && !active_ ? 0U : 4U));
        EVIDENCE(physical_position().signed_units == 0.0 && applied == 0);
        std::size_t cancelled = 0;
        if (which_ == Case::NativeAll) cancelled = cancel_all();
        else cancelled = cancel_where(active_ ? "drop" : "absent", NativeRequestField::Comment);
        ++target_calls;
        const std::size_t expected = which_ == Case::NativeAll ? before : (active_ ? 3U : 0U);
        EVIDENCE(cancelled == expected);
        EVIDENCE(native_working_requests().size() == before - expected);
        std::size_t receipts = 0;
        for (const auto& row : native_events(0))
            if (row.command && std::holds_alternative<no::CancelledEvent>(*row.command)) ++receipts;
        EVIDENCE(receipts == expected);
        // Individual cleanup keeps cancel_all and cancel_where independent
        // of each other. None of the four limits can reach a 100 bar.
        const auto remainder = native_working_requests();
        for (const auto& row : remainder)
            EVIDENCE(cancel(row.definition->handle).status == no::CancelStatus::Cancelled);
        EVIDENCE(native_working_requests().empty() && applied == 0);
    }

    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        ++applied;
    }

private:
    Case which_;
    bool active_;
};

// Stateful commands legitimately alter the book, journal, trades and caches.
// One cycle is one identical six-bar run on the SAME live host (no host
// destruction in the measured loop). Its public begin resets the previous
// run and the script returns to the same flat, empty-request terminal point.
// The first completed cycle is the warm-up baseline, never a byte constant.
// N and 4N count target commands, rather than padding one call with idle bars.
template<class Host, class Prepare>
void steady_cycles(Host& host, const Group& group, Prepare prepare) {
    const auto bars = tape(); // fixed test-owned input exists before any checkpoint
    Heap baseline{}, at_n{};
    for (int cycle = 0; cycle <= kFourN; ++cycle) {
        cycle_number = cycle;
        command_number = cycle;
        prepare(cycle);
        host.run(bars.data(), static_cast<int>(bars.size()));
        const auto after = heap(); // callback temporaries are already gone
        EVIDENCE(host.last_error().empty());
        EVIDENCE(host.native_state().kind == NativeLifecycleKind::Completed);
        EVIDENCE(host.target_calls == 1);
        EVIDENCE(host.physical_position().signed_units == 0.0);
        EVIDENCE(host.native_working_requests().empty());
        if (cycle == 0) baseline = after;
        else release_rows(group.which, after, baseline);
        if (cycle == kN) at_n = after;
        if (cycle == kFourN) release_rows(group.which, after, at_n);
        // A false check only records a failure; it does not leave this loop.
    }
}

void exercise(const Group& group, bool active) {
    current = &group;
    active_arm = active;
    cycle_number = command_number = 0;
    try {
        if (group.which == Case::NativeAll || group.which == Case::NativeWhere) {
            NativeHost host(group.which, active);
            steady_cycles(host, group, [&](int cycle) {
                host.start_cycle(static_cast<std::uint64_t>(cycle) + 1);
            });
        } else {
            SourceHost host(group.which, active);
            if (group.within_callback) {
                // Pure query/helper windows run their own actual 64-call
                // loop at one stable pending population, with the 16th and
                // 64th checkpoints compared explicitly inside the callback.
                const auto bars = tape();
                host.start_cycle();
                host.run(bars.data(), static_cast<int>(bars.size()));
                EVIDENCE(host.last_error().empty());
                EVIDENCE(host.native_state().kind == NativeLifecycleKind::Completed);
                EVIDENCE(host.target_calls == kFourN);
            } else {
                const bool paired_retirement = group.which == Case::Declined
                    || group.which == Case::Gap || group.which == Case::SameId;
                Heap first_heap{}, first_backing{};
                if (paired_retirement) {
                    // reset_for_run clears memo sizes, retaining capacity;
                    // release_closed_cohort_origins refills then swaps the
                    // two K3 buffers. One run need not warm both owners.
                    // Warm a complete cycle before steady_cycles' baseline.
                    // Below, independently account for ALL warm-up growth
                    // using the two actual capacities, never a byte allowance.
                    const auto bars = tape();
                    host.start_cycle();
                    host.run(bars.data(), static_cast<int>(bars.size()));
                    first_heap = heap();
                    first_backing = host.retirement_backing();
                    EVIDENCE(host.last_error().empty());
                    EVIDENCE(host.native_state().kind == NativeLifecycleKind::Completed);
                    EVIDENCE(host.target_calls == 1);
                    EVIDENCE(host.physical_position().signed_units == 0.0);
                    EVIDENCE(host.native_working_requests().empty());
                }
                steady_cycles(host, group, [&](int cycle) {
                    if (paired_retirement && cycle == 1) {
                        const auto after = heap();
                        const auto backing = host.retirement_backing();
                        // Exact attribution of the first-to-second-run
                        // difference to the paired buffer capacity above.
                        // Unexplained retained allocations still fail here.
                        EVIDENCE(after.bytes + first_backing.bytes == first_heap.bytes + backing.bytes
                                 && after.blocks + first_backing.blocks == first_heap.blocks + backing.blocks
                                 && after.foreign == first_heap.foreign);
                    }
                    host.start_cycle();
                });
            }
        }
    } catch (const std::exception& error) {
        evidence(false, error.what(), __LINE__);
    } catch (...) {
        evidence(false, "no non-standard exception", __LINE__);
    }
    // Exceptions also leave the next control arm and next row family runnable.
}

void allocator_controls() {
    current = nullptr;
    const auto before = heap();
    const std::size_t sizes[] = {13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59};
    void* p[12]{};
    p[0] = ::operator new(sizes[0]);
    p[1] = ::operator new[](sizes[1]);
    p[2] = ::operator new(sizes[2], std::nothrow);
    p[3] = ::operator new[](sizes[3], std::nothrow);
    p[4] = ::operator new(sizes[4], std::align_val_t{64});
    p[5] = ::operator new[](sizes[5], std::align_val_t{64});
    p[6] = ::operator new(sizes[6], std::align_val_t{64}, std::nothrow);
    p[7] = ::operator new[](sizes[7], std::align_val_t{64}, std::nothrow);
    p[8] = ::operator new(sizes[8]);
    p[9] = ::operator new[](sizes[9]);
    p[10] = ::operator new(sizes[10], std::align_val_t{64});
    p[11] = ::operator new[](sizes[11], std::align_val_t{64});
    std::size_t requested = 0;
    for (const auto size : sizes) requested += size;
    const auto held = heap();
    ROW("allocator", held.bytes == before.bytes + requested && held.blocks == before.blocks + 12);
    for (std::size_t i = 4; i < 8; ++i)
        ROW("allocator", reinterpret_cast<std::uintptr_t>(p[i]) % 64 == 0);
    ROW("allocator", reinterpret_cast<std::uintptr_t>(p[10]) % 64 == 0
        && reinterpret_cast<std::uintptr_t>(p[11]) % 64 == 0);
    ::operator delete(p[0]);
    ::operator delete[](p[1]);
    ::operator delete(p[2], std::nothrow);
    ::operator delete[](p[3], std::nothrow);
    ::operator delete(p[4], std::align_val_t{64});
    ::operator delete[](p[5], std::align_val_t{64});
    ::operator delete(p[6], std::align_val_t{64}, std::nothrow);
    ::operator delete[](p[7], std::align_val_t{64}, std::nothrow);
    ::operator delete(p[8], sizes[8]);
    ::operator delete[](p[9], sizes[9]);
    ::operator delete(p[10], sizes[10], std::align_val_t{64});
    ::operator delete[](p[11], sizes[11], std::align_val_t{64});
    const auto after = heap();
    ROW("allocator", same(after, before));
}
} // namespace

int main() {
    adapter_pass_heap::is_test_thread = true;
    try { allocator_controls(); }
    catch (const std::exception& error) { check("allocator", false, error.what(), __LINE__); }
    catch (...) { check("allocator", false, "no non-standard exception", __LINE__); }
    for (const auto& group : groups) {
        // Every family's positive and non-insert control arm gets dispatched,
        // regardless of any preceding assertion or exception.
        exercise(group, true);
        exercise(group, false);
    }
    std::printf("memory proof release_adapter_passes: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
