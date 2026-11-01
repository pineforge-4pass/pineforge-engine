// Bracket-family commands are checked for storage release and reuse.
// Two boundaries: nothing allocated by a command outlives its fresh host;
// no per-run live-heap growth on ONE retained host after fixed warm-up runs
// of each stimulus (normally three; see measure_cycles), at N=16 and 4N=64.
// This is not a proof of immediate release at each inaccessible function's
// return. Direct query windows separately check that boundary with the host
// alive. Every checkpoint reads BOTH live requested bytes and live blocks.
// No measured byte constant is an expectation.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <variant>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Same complete C++17 replacement surface as global_allocation_replacement.hpp,
// with a thread-local recording switch. A block remembers whether THIS test
// thread allocated it while recording; its eventual delete is counted even
// outside the window or on another thread. Counters are never reset with live
// blocks. The engine fixture is synchronous and starts no worker threads.
namespace bracket_heap {
struct Counters {
    std::atomic<std::size_t> bytes{0}, blocks{0}, calls{0};
};
Counters counts;
thread_local bool recording = false;
struct Header { void* base; std::size_t size; bool counted; };
struct Sample { std::size_t bytes, blocks, calls; };
Sample sample() noexcept {
    return {counts.bytes.load(std::memory_order_relaxed),
            counts.blocks.load(std::memory_order_relaxed),
            counts.calls.load(std::memory_order_relaxed)};
}
bool same_live(Sample a, Sample b) noexcept {
    return a.bytes == b.bytes && a.blocks == b.blocks;
}
void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) size = 1;
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (alignment > maximum - sizeof(Header)
        || size > maximum - sizeof(Header) - alignment) return nullptr;
    void* base = std::malloc(size + sizeof(Header) + alignment - 1);
    if (!base) return nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto aligned = (start + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    auto* block = reinterpret_cast<unsigned char*>(aligned);
    ::new (static_cast<void*>(block - sizeof(Header))) Header{base, size, recording};
    if (recording) {
        counts.bytes.fetch_add(size, std::memory_order_relaxed);
        counts.blocks.fetch_add(1, std::memory_order_relaxed);
        counts.calls.fetch_add(1, std::memory_order_relaxed);
    }
    return block;
}
void release(void* value) noexcept {
    if (!value) return;
    Header header{};
    std::memcpy(&header, static_cast<unsigned char*>(value) - sizeof(Header), sizeof header);
    if (header.counted) {
        counts.bytes.fetch_sub(header.size, std::memory_order_relaxed);
        counts.blocks.fetch_sub(1, std::memory_order_relaxed);
    }
    std::free(header.base);
}
void* throwing(std::size_t size, std::size_t alignment) {
    for (;;) {
        if (void* result = allocate(size, alignment)) return result;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nonthrowing(std::size_t size, std::size_t alignment) noexcept {
    try { return throwing(size, alignment); } catch (...) { return nullptr; }
}
} // namespace bracket_heap

void* operator new(std::size_t n) { return bracket_heap::throwing(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return bracket_heap::throwing(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return bracket_heap::nonthrowing(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return bracket_heap::nonthrowing(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) { return bracket_heap::throwing(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return bracket_heap::throwing(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return bracket_heap::nonthrowing(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return bracket_heap::nonthrowing(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { bracket_heap::release(p); }
void operator delete[](void* p) noexcept { bracket_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { bracket_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { bracket_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { bracket_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { bracket_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { bracket_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { bracket_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { bracket_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { bracket_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { bracket_heap::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { bracket_heap::release(p); }

namespace memory_release_bracket_families {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Family = source::PineOrderFamily;
using Handle = no::RequestHandle;
using Snapshot = source::PlacementSnapshot;
using Word = std::uint64_t;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = 1736121600000LL;
constexpr std::size_t maximum_width = 64;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEM_ACCESS(tag, member) \
    struct tag { friend auto access(tag); }; \
    template struct Access<tag, &Adapter::member>
MEM_ACCESS(Placements, placement_);
MEM_ACCESS(Live, live_handles_);
MEM_ACCESS(LiveKeys, live_by_source_key_);
MEM_ACCESS(Brackets, bracket_families_);
MEM_ACCESS(TrailOpen, trail_state_at_open_);
MEM_ACCESS(Cohorts, cohorts_by_id_);
MEM_ACCESS(PendingLegs, pending_bracket_legs_);
MEM_ACCESS(PendingEntries, pending_entries_);
MEM_ACCESS(Stashed, id_bound_exits_);
MEM_ACCESS(PositionOpenBar, position_open_bar_index_);
MEM_ACCESS(PositionOpenPhase, position_open_phase_);
MEM_ACCESS(Compute, compute_exit_reservation);
MEM_ACCESS(Reconcile, reconcile_deferred_exit_reservations);
MEM_ACCESS(AfterMargin, reconcile_exit_reservations_after_margin);
MEM_ACCESS(CancelOrigin, cancel_bracket_origin);
MEM_ACCESS(CancelSiblings, cancel_bracket_siblings);
MEM_ACCESS(RetireFlat, retire_in_position_exits_at_flat);
MEM_ACCESS(Initialize, initialize_l4c_policy);
MEM_ACCESS(Suspend, suspend_brackets_for_reversal);
MEM_ACCESS(Purge, purge_brackets_after_applied_reversal);
MEM_ACCESS(WithdrawVoid, withdraw_void_exits);
MEM_ACCESS(SettleGlobal, settle_void_global_exits);
#undef MEM_ACCESS

enum class Test {
    Hash, Prefix, Owners, VoidNamed, Stash, VoidGlobal, ReservedMap, DropLeg,
    PreDrain, Ordered, Anchor, Policy, Suspend, Purge, Reservation, Reconcile,
    Margin, Origin, Siblings, Flat, Recalc, Sell, Child, Empty
};
struct Group {
    Test test;
    const char* name;
    std::array<const char*, 4> rows;
};
constexpr Group groups[] = {
    {Test::Hash, "hash", {{"pineexecutionadapter_hash_state_live_keys", "pineexecutionadapter_hash_state_bracket_keys", "pineexecutionadapter_hash_state_trail_open_keys", nullptr}}},
    {Test::Prefix, "prefix", {{"pineexecutionadapter_retire_origin_prefixes_named", nullptr, nullptr, nullptr}}},
    {Test::Owners, "reversal-owners", {{"pineexecutionadapter_cancel_reversal_exit_owners_owners", nullptr, nullptr, nullptr}}},
    {Test::VoidNamed, "void-named", {{"pineexecutionadapter_withdraw_void_exits_handles", nullptr, nullptr, nullptr}}},
    {Test::Stash, "id-bound-stash", {{"pineexecutionadapter_stash_id_bound_exits_handles", nullptr, nullptr, nullptr}}},
    {Test::VoidGlobal, "void-global", {{"pineexecutionadapter_settle_void_global_exits_handles", nullptr, nullptr, nullptr}}},
    {Test::ReservedMap, "host-sized-reservations", {{"pineexecutionadapter_exit_submit_leg_submit_one_reserved_by_exit", nullptr, nullptr, nullptr}}},
    {Test::DropLeg, "level-reissue", {{"pineexecutionadapter_exit_withdrawn", nullptr, nullptr, nullptr}}},
    {Test::PreDrain, "pre-script-drain", {{"pineexecutionadapter_flush_pending_bracket_legs_book_keys", "pineexecutionadapter_flush_pending_bracket_legs_resting", nullptr, nullptr}}},
    {Test::Ordered, "ordered-entries", {{"pineexecutionadapter_flush_pending_entries_ordered", nullptr, nullptr, nullptr}}},
    {Test::Anchor, "relative-parents", {{"pineexecutionadapter_anchor_relative_exits_parents", nullptr, nullptr, nullptr}}},
    {Test::Policy, "reservation-growth", {{"pineexecutionadapter_initialize_l4c_policy_candidates", "pineexecutionadapter_initialize_l4c_policy_candidate_refs", nullptr, nullptr}}},
    {Test::Suspend, "cross-side-suspension", {{"pineexecutionadapter_suspend_brackets_for_reversal_cross_side_by_entry", nullptr, nullptr, nullptr}}},
    {Test::Purge, "reversal-purge", {{"pineexecutionadapter_purge_brackets_after_applied_reversal_stale", nullptr, nullptr, nullptr}}},
    {Test::Reservation, "reservation-query", {{"pineexecutionadapter_compute_exit_reservation_reservations", nullptr, nullptr, nullptr}}},
    {Test::Reconcile, "deferred-reservations", {{"pineexecutionadapter_reconcile_deferred_exit_reservations_family_handles", "pineexecutionadapter_reconcile_deferred_exit_reservations_family_queued", "pineexecutionadapter_reconcile_deferred_exit_reservations_families", "pineexecutionadapter_reconcile_deferred_exit_reservations_cancel"}}},
    {Test::Margin, "margin-reservations", {{"pineexecutionadapter_reconcile_exit_reservations_after_margin_entries", nullptr, nullptr, nullptr}}},
    {Test::Origin, "origin-cancel", {{"pineexecutionadapter_cancel_bracket_origin_matches", nullptr, nullptr, nullptr}}},
    {Test::Siblings, "sibling-cancel", {{"pineexecutionadapter_cancel_bracket_siblings_matches", nullptr, nullptr, nullptr}}},
    {Test::Flat, "flat-retirement", {{"pineexecutionadapter_retire_in_position_exits_at_flat_handles", nullptr, nullptr, nullptr}}},
    {Test::Recalc, "recalc-entries", {{"pineexecutionadapter_place_entry_stale_recalc_entries", nullptr, nullptr, nullptr}}},
    {Test::Sell, "sell-replacement", {{"pineexecutionadapter_place_entry_dynamic_carried_legs", nullptr, nullptr, nullptr}}},
    {Test::Child, "named-cancel-families", {{"pineexecutionadapter_submit_entry_with_policy_child_families", nullptr, nullptr, nullptr}}}
};

struct Totals { std::size_t checks = 0, failures = 0; } totals;
const Group* current = nullptr;
std::size_t width_now = 0, iteration_now = 0;
bool control_now = false;
bool check(const char* row, bool passed, const char* expression, int line) {
    ++totals.checks;
    if (!passed) {
        ++totals.failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s (family=%s width=%zu cycle=%zu control=%d)\n",
            row, __FILE__, line, expression, current ? current->name : "allocator",
            width_now, iteration_now, int(control_now));
    }
    return passed;
}
#define ROW_CHECK(row, expression) check(row, bool(expression), #expression, __LINE__)
bool family_check(bool passed, const char* expression, int line) {
    for (const auto* row : current->rows) if (row) check(row, passed, expression, line);
    return passed;
}
#define EVIDENCE(expression) family_check(bool(expression), #expression, __LINE__)

// The only test-owned strings and bar arrays are made before recording starts.
// They are reused by reference; all in-window dynamic storage belongs to the
// real host/adapter/kernel, not to an allocation model or diagnostic stream.
struct Inputs {
    std::array<std::string, maximum_width> parents, exits;
    const std::string seed = "exit-held-entry";
    const std::string other = "exit-pending-parent";
    const std::string reversal = "exit-reversal";
    const std::string probe = "exit-probe";
    const std::string global = "exit-global-exit";
    const std::string empty;
    std::array<Bar, 72> bars{};
    Inputs() {
        for (std::size_t i = 0; i < maximum_width; ++i) {
            char name[80];
            std::snprintf(name, sizeof name, "exit-parent-with-owned-text-%02zu", i);
            parents[i] = name;
            std::snprintf(name, sizeof name, "exit-exit-with-owned-text-%02zu", i);
            exits[i] = name;
        }
        for (std::size_t i = 0; i < bars.size(); ++i)
            bars[i] = {100.0, 101.0, 99.0, 100.0, 1.0,
                       start_ms + static_cast<std::int64_t>(i) * 60000};
    }
};

bool is_exit(Family family) noexcept {
    return family == Family::ExitLimit || family == Family::ExitStop || family == Family::ExitTrail;
}

class Host final : public source::PineStrategyHost {
public:
    Host(Test test, const Inputs& input, std::size_t width, bool control, int hold)
        : test_(test), input_(input), width_(width), control_(control), hold_(hold) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 1000000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = static_cast<int>(2 * maximum_width + 4);
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 0.0;
        config.close_entries_rule_any = test != Test::ReservedMap && test != Test::Stash
            && test != Test::Anchor;
        config.process_orders_on_close = test == Test::PreDrain || test == Test::Policy || test == Test::Child;
        config.calc_on_order_fills = test == Test::Ordered || test == Test::Recalc;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    bool reached = false, cleaned = false;
    std::size_t held_count = 0;
    double final_position = na;
    void on_source_bar(const Bar&) override;
    void prepare_cycle(std::size_t width, bool control, int hold) {
        width_ = width;
        control_ = control;
        hold_ = hold;
        // Reset only this fixture's callback state. Host::run performs the
        // product's normal begin/reset; keep the SAME adapter and engine.
        reached = cleaned = seeded_ = recalc_placed_ = false;
        held_count = 0;
        final_position = na;
        prefixes_before_ = origins_before_ = 0;
    }

private:
    Test test_;
    const Inputs& input_;
    std::size_t width_;
    bool control_;
    int hold_;
    bool seeded_ = false, recalc_placed_ = false;
    Word prefixes_before_ = 0, origins_before_ = 0;

    auto& placements() { return adapter_.*access(Placements{}); }
    const auto& placements() const { return adapter_.*access(Placements{}); }
    const auto& live() const { return adapter_.*access(Live{}); }
    auto& legs() { return adapter_.*access(PendingLegs{}); }
    auto& entries() { return adapter_.*access(PendingEntries{}); }
    const auto& cohorts() const { return adapter_.*access(Cohorts{}); }
    template<class Predicate> std::size_t count(Predicate&& predicate) const {
        std::size_t result = 0;
        for (const auto& handle : live()) {
            const auto row = placements().find(handle.incarnation);
            if (row != placements().end() && predicate(row->second)) ++result;
        }
        return result;
    }
    std::size_t exit_count() const { return count([](const Snapshot& row) { return is_exit(row.family); }); }
    Handle find_live(Family family, const std::string& id) const {
        for (const auto& handle : live()) {
            const auto row = placements().find(handle.incarnation);
            if (row != placements().end() && row->second.family == family && row->second.source_id == id)
                return handle;
        }
        return {};
    }
    Handle opening(const std::string& id) const {
        const auto row = cohorts().find(id);
        if (row == cohorts().end() || row->second.opened.empty()) return {};
        return row->second.opened.front();
    }
    void explicit_exits(const std::string& parent, bool stop = true) {
        for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], parent, 300.0, stop ? 50.0 : na,
                          na, na, na, 100.0, input_.empty, 1.0);
    }
    void flush() { adapter_.flush_pending_entries(); adapter_.flush_pending_bracket_legs(); }
    void finish_commands() {
        strategy_cancel_all();
        strategy_close_all();
        adapter_.flush_pending_entries();
    }
    void hash();
    void prefix(int bar);
    void owners(int bar);
    void void_named(int bar);
    void stash_or_map(int bar);
    void void_global(int bar);
    void drop_leg(int bar);
    void pre_drain(int bar);
    void ordered(int bar);
    void anchor(int bar);
    void policy(int bar);
    void suspend(int bar);
    void purge(int bar);
    void reservation_family(int bar);
    void margin(int bar);
    void recalc(int bar);
    void sell(int bar);
    void child(int bar);
};

void Host::hash() {
    const auto keys = (adapter_.*access(LiveKeys{})).size();
    const auto families = (adapter_.*access(Brackets{})).size();
    const auto trails = (adapter_.*access(TrailOpen{})).size();
    if (!EVIDENCE(keys == (control_ ? 0 : 2 * width_)
                  && families == (control_ ? 0 : width_)
                  && trails == (control_ ? 0 : width_))) return;
    BrokerStateHashSink first, second;
    const auto seed_hash = first.h;
    const auto before = bracket_heap::sample();
    adapter_.hash_state(first);
    adapter_.hash_state(second);
    const auto after = bracket_heap::sample();
    // Returned sink scalars independently witness both real query calls.
    EVIDENCE(first.h != seed_hash && first.h == second.h);
    EVIDENCE(control_ || after.calls > before.calls);
    // hash_state is a read-only query: unlike the stateful command families,
    // its exact call window can also require net zero while the host lives.
    ROW_CHECK("pineexecutionadapter_hash_state_live_keys", bracket_heap::same_live(before, after));
    ROW_CHECK("pineexecutionadapter_hash_state_bracket_keys", bracket_heap::same_live(before, after));
    ROW_CHECK("pineexecutionadapter_hash_state_trail_open_keys", bracket_heap::same_live(before, after));
    held_count = keys + families + trails;
    reached = true;
}

void Host::prefix(int bar) {
    if (bar == 0) {
        for (std::size_t i = 0; i < width_; ++i)
            strategy_order(input_.parents[i], true, 1.0, 1.0);
        strategy_order(input_.seed, true, 1.0, 1.0);
        EVIDENCE(live().size() == width_ + 1);
    } else if (bar == 1) {
        const auto& work = adapter_.retirement_work();
        prefixes_before_ = work.missing_fronts;
        origins_before_ = work.origins_retired;
        if (!control_) strategy_cancel(input_.seed);
    } else if (bar == 2) {
        // Bar-open erasure precedes retire_origin_prefixes. A missing seed
        // front and N still-live requests force a NONEMPTY named vector.
        const auto& work = adapter_.retirement_work();
        EVIDENCE(live().size() == width_ + (control_ ? 1 : 0));
        EVIDENCE(control_ ? work.origins_retired == origins_before_
                          : work.missing_fronts > prefixes_before_
                            && work.origins_retired > origins_before_);
        held_count = live().size();
        reached = true;
    }
}

void Host::owners(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1))) return;
        explicit_exits(input_.seed, false);
        flush();
        EVIDENCE(exit_count() == width_);
        strategy_entry(input_.reversal, control_, control_ ? 1.0 : na, na, 1.0);
    } else if (bar == 2) {
        EVIDENCE(control_ ? physical_position().signed_units > 0.0
                          : physical_position().signed_units < 0.0);
        // On the real Applied flip, cancel_reversal_exit_owners runs before
        // source cohort cleanup. Its N working exit legs all name the seed.
        EVIDENCE(exit_count() == (control_ ? width_ : 0));
        EVIDENCE(control_ || (fixture_applied_receipt_count() >= 2 && closed_trade_count() != 0));
        reached = true;
    }
}

void Host::void_named(int bar) {
    if (bar != 0) return;
    for (std::size_t i = 0; i < width_; ++i)
        strategy_exit(input_.exits[i], input_.seed, 300.0, na);
    if (!EVIDENCE(count([&](const Snapshot& row) {
            return is_exit(row.family) && row.void_issue && row.from_entry == input_.seed;
        }) == width_)) return;
    const auto before = bracket_heap::sample();
    (adapter_.*access(WithdrawVoid{}))(control_ ? input_.other : input_.seed);
    const auto after = bracket_heap::sample();
    EVIDENCE(exit_count() == (control_ ? width_ : 0));
    EVIDENCE(control_ || after.calls > before.calls);
    if (!control_) strategy_entry(input_.seed, true, na, na, 1.0);
    reached = true;
}

void Host::stash_or_map(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
        return;
    }
    if (bar == 1) {
        strategy_entry(input_.other, true, 1.0, na, 1.0);
        EVIDENCE(find_live(Family::Entry, input_.other).incarnation != 0);
        return;
    }
    if (bar != 2) return;
    if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1))) return;
    const auto parent = find_live(Family::Entry, input_.other);
    if (!EVIDENCE(parent.incarnation != 0
                  && placements().at(parent.incarnation).projection_created_bar < bar
                  && placements().at(parent.incarnation).is_long)) return;
    const bool explicit_control = test_ == Test::ReservedMap && control_;
    // PineExecutionAdapter::exit stages an explicit leg per origin;
    // flush_pending_bracket_legs keeps a pending origin queued.
    // Give the finite-quantity control the HELD seed origin, so its N
    // one-unit reservations fit Q=2N+1 and all 2N legs can become live.
    // The active default-quantity case still names the pending other id
    // and exercises exit's reserved_by_exit workset.
    const auto& exit_parent = explicit_control ? input_.seed : input_.other;
    for (std::size_t i = 0; i < width_; ++i)
        strategy_exit(input_.exits[i], exit_parent, 300.0, 50.0,
                      na, na, na, 100.0, input_.empty, explicit_control ? 1.0 : na);
    adapter_.flush_pending_bracket_legs();
    if (!EVIDENCE(exit_count() == 2 * width_)) return;
    if (test_ == Test::ReservedMap) {
        // FIFO gives the pending, same-side parent's id zero live exposure.
        // compute_exit_reservation leaves a default request unresolved;
        // submit_one's map gives the FIRST exit family all Q units, then
        // clamps every other family's two legs to Q-Q=0. Distinct ids, not
        // recorded counts, supply the N-1 map nodes on the last family.
        const auto held = cohorts().find(input_.other);
        EVIDENCE(held != cohorts().end() && held->second.opened.empty());
        EVIDENCE(count([&](const Snapshot& row) {
            return is_exit(row.family) && row.from_entry == exit_parent
                && row.projection_remaining_qty == 0.0;
        }) == (control_ ? 0 : 2 * (width_ - 1)));
        EVIDENCE(count([&](const Snapshot& row) {
            return is_exit(row.family) && row.from_entry == exit_parent
                && row.projection_remaining_qty == (control_ ? 1.0 : double(2 * width_ + 1));
        }) == (control_ ? 2 * width_ : 2));
    } else {
        if (!EVIDENCE(count([](const Snapshot& row) {
                return is_exit(row.family) && row.pending_bound_exit;
            }) == 2 * width_)) return;
        strategy_cancel(control_ ? input_.probe : input_.other);
        EVIDENCE((adapter_.*access(Stashed{})).size() == (control_ ? 0 : width_));
        EVIDENCE(exit_count() == (control_ ? 2 * width_ : 0));
        if (!control_) {
            strategy_entry(input_.other, true, 2.0, na, 1.0);
            adapter_.flush_pending_bracket_legs();
            EVIDENCE((adapter_.*access(Stashed{})).empty());
            // The fresh one-unit parent participates in same-bar sizing:
            // its first whole bracket reserves 1, the other N-1 reserve 0.
            EVIDENCE(exit_count() == 2);
        }
    }
    held_count = exit_count();
    reached = true;
}

void Host::void_global(int bar) {
    if (bar != 0) return;
    for (std::size_t i = 0; i < width_; ++i)
        strategy_exit(input_.exits[i], input_.empty, 300.0, 50.0);
    if (!EVIDENCE(count([](const Snapshot& row) {
            return is_exit(row.family) && row.void_global_issue;
        }) == 2 * width_)) return;
    const auto before = bracket_heap::sample();
    (adapter_.*access(SettleGlobal{}))(!control_);
    const auto after = bracket_heap::sample();
    EVIDENCE(exit_count() == (control_ ? 2 * width_ : 0));
    EVIDENCE(count([](const Snapshot& row) { return row.void_global_issue; }) == 0);
    EVIDENCE(control_ || after.calls > before.calls);
    strategy_entry(input_.seed, true, na, na, 1.0);
    reached = true;
}

void Host::drop_leg(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1))) return;
        explicit_exits(input_.seed);
        flush();
        if (!EVIDENCE(exit_count() == 2 * width_)) return;
        for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], input_.seed, 301.0, control_ ? 50.0 : na,
                          na, na, na, 100.0, input_.empty, 1.0);
        flush();
        EVIDENCE(count([](const Snapshot& row) { return row.family == Family::ExitLimit; }) == width_);
        EVIDENCE(count([](const Snapshot& row) { return row.family == Family::ExitStop; })
                  == (control_ ? width_ : 0));
        reached = true;
    }
}

void Host::pre_drain(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, 99.5, na, double(2 * width_ + 1));
    } else if (bar == 1) {
        // The 99.5 buy limit rests below the 100 open and fills on the
        // later path to 99. on_applied records that fill phase.
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1)
                      && (adapter_.*access(PositionOpenBar{})) == bar
                      && (adapter_.*access(PositionOpenPhase{})) != NativePathPhase::Open
                      && fixture_applied_receipt_count() == 1
                      && adapter_.priority.attached() && adapter_.priority.retained_parent_first())) return;
        explicit_exits(input_.seed);
        if (!EVIDENCE(legs().size() == 2 * width_ && exit_count() == 0)) return;
        const auto origin = opening(input_.seed);
        // PlacementSnapshot defaults source_sequence to zero.
        // These explicit legs stage in exit before submit_or_replace
        // assigns a sequence.
        if (!EVIDENCE(origin.incarnation != 0
                      && placements().at(origin.incarnation).source_sequence > 0
                      && std::all_of(legs().begin(), legs().end(), [](const auto& leg) {
                          return leg.snapshot.source_sequence == 0;
                      }))) return;
        const auto before = bracket_heap::sample();
        adapter_.flush_pending_bracket_legs({}, true, !control_);
        const auto after = bracket_heap::sample();
        // flush_pending_bracket_legs compares SOURCE sequences, not
        // command acceptance time: this parent's mid-path fill and positive
        // sequence cannot precede the staged legs' zero sequences. The
        // pre-script call collects N book_keys, reserves room for 2N
        // resting legs, then parks all 2N back in pending_bracket_legs_.
        // The ordinary post-calculation control submits all 2N instead.
        EVIDENCE(legs().size() == (control_ ? 0 : 2 * width_)
                  && exit_count() == (control_ ? 2 * width_ : 0));
        EVIDENCE(after.calls > before.calls);
        if (!control_) {
            // flush_pending_bracket_legs parks only in pre_script_drain; an ordinary
            // post-calculation call releases this queue to submission.
            adapter_.flush_pending_bracket_legs({}, true, false);
            EVIDENCE(legs().empty() && exit_count() == 2 * width_);
            // Keep the parked-parent scenario above. Additionally stage
            // explicit origin-zero legs for an ABSENT parent: exit falls
            // back to origin zero and stages each new instance.
            // No placement/cohort member is fabricated or changed here.
            // flush_pending_bracket_legs' resolve_parent returns null for
            // every such leg, so has_parent is false and ALL take resting.push_back,
            // not merely resting.reserve. Far-away prices keep them live.
            for (std::size_t i = 0; i < width_; ++i)
                strategy_exit(input_.parents[i], input_.other, 300.0, 50.0,
                              na, na, na, 100.0, input_.empty, 1.0);
            const auto absent = cohorts().find(input_.other);
            EVIDENCE(absent != cohorts().end() && absent->second.opened.empty()
                      && find_live(Family::Entry, input_.other).incarnation == 0);
            EVIDENCE(legs().size() == 2 * width_
                      && std::all_of(legs().begin(), legs().end(), [&](const auto& leg) {
                          return leg.snapshot.from_entry == input_.other
                              && leg.snapshot.bracket_origin.incarnation == 0
                              && leg.snapshot.void_issue;
                      }));
            const auto before_resting = bracket_heap::sample();
            adapter_.flush_pending_bracket_legs({}, true, true);
            const auto after_resting = bracket_heap::sample();
            EVIDENCE(legs().empty() && exit_count() == 4 * width_
                      && count([&](const Snapshot& row) {
                          return is_exit(row.family) && row.from_entry == input_.other
                              && row.void_issue;
                      }) == 2 * width_);
            EVIDENCE(after_resting.calls > before_resting.calls);
        }
        reached = true;
    }
}

void Host::ordered(int bar) {
    if (bar != 0) return;
    for (std::size_t i = 0; i < width_; ++i)
        strategy_entry(input_.parents[i], true, 1.0, na, 1.0);
    if (!control_) for (std::size_t i = 0; i < width_; ++i)
        strategy_exit(input_.exits[i], input_.parents[i], 300.0, 50.0,
                      na, na, na, 100.0, input_.empty, 1.0);
    if (!EVIDENCE(entries().size() == width_ && legs().size() == (control_ ? 0 : 2 * width_))) return;
    adapter_.flush_pending_entries();
    EVIDENCE(entries().empty() && legs().empty());
    // ordered receives N entry candidates plus one limit and one stop per
    // staged family. All 3N commands are accepted through submit_or_replace.
    EVIDENCE(count([](const Snapshot& row) { return row.family == Family::Entry; }) == width_);
    EVIDENCE(exit_count() == (control_ ? 0 : 2 * width_));
    reached = true;
}

void Host::anchor(int bar) {
    if (bar != 0) return;
    for (std::size_t i = 0; i < width_; ++i) {
        strategy_entry(input_.parents[i], true, na, 150.0, 1.0);
        if (!control_)
            strategy_exit(input_.exits[i], input_.parents[i], na, na,
                          na, na, na, 100.0, input_.empty, na, input_.empty, 16.0, 16.0);
    }
    const auto before = adapter_.anchored_relative_stats().anchored;
    adapter_.anchor_relative_exits();
    const auto after = adapter_.anchored_relative_stats().anchored;
    // Both relative operands yield a real accepted WaitForApplied child.
    // The count is increased only after require_host().submit accepts one.
    EVIDENCE(after - before == (control_ ? 0 : 2 * width_));
    EVIDENCE(count([](const Snapshot& row) { return row.family == Family::Entry; }) == width_);
    held_count = after - before;
    reached = true;
}

void Host::policy(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, 1.0);
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == 1.0)) return;
        for (std::size_t i = 0; i < width_; ++i)
            strategy_entry(input_.parents[i], true, na, na, 1.0,
                           input_.empty, input_.parents[i], 0);
        if (!EVIDENCE(count([](const Snapshot& row) {
                return row.family == Family::Entry && row.opening;
            }) == width_)) return;
        strategy_exit(input_.global, input_.empty, 300.0, na,
                      na, na, na, 100.0, input_.empty, control_ ? 1.0 : na);
        const auto handle = find_live(Family::ExitLimit, input_.global);
        if (!EVIDENCE(handle.incarnation != 0)) return;
        auto& row = placements().at(handle.incarnation);
        const auto before = bracket_heap::sample();
        (adapter_.*access(Initialize{}))(row, handle);
        const auto after = bracket_heap::sample();
        const auto owned = count([&](const Snapshot& entry) {
            return entry.family == Family::Entry && entry.opening
                && entry.reservation_growth_owner_incarnation == handle.incarnation;
        });
        // CandidateRef's real read-back assigns every accepted source
        // its exit owner. A finite explicit quantity is the guard control.
        EVIDENCE(owned == (control_ ? 0 : width_));
        EVIDENCE(control_ || (row.pooc_global_full_exit_dynamic_qty && row.reservation_expansion.capture()
                             && after.calls > before.calls));
        reached = true;
    }
}

void Host::suspend(int bar) {
    if (bar == 0) {
        for (std::size_t i = 0; i < width_; ++i) {
            // on_applied reissues a prearmed bracket if its creation bar
            // matches the filling parent's. submit_or_replace then captures
            // the held side, bypassing suspend_brackets_for_reversal's cross-side map.
            // First keep these parents unfilled below every input low.
            // Replace them on the NEXT bar: their carried brackets keep
            // bar 0 while the replacements get bar 1 in submit_or_replace.
            // Their old bracket origins also differ from the filling new
            // handles in on_applied, excluding that reissue.
            strategy_entry(input_.parents[i], true, 1.0, na, 2.0,
                           input_.empty, input_.parents[i], 0);
            if (!control_)
                strategy_exit(input_.exits[i], input_.parents[i], 300.0, 50.0);
        }
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == 0.0
                      && exit_count() == (control_ ? 0 : 2 * width_))) return;
        // PineExecutionAdapter::entry keeps flat pending parents cohort-bound through
        // same-id replacement. A nonempty OCA spelling with type 0 avoids
        // the entry-only market batch without grouping these N openings.
        for (std::size_t i = 0; i < width_; ++i)
            strategy_entry(input_.parents[i], true, na, na, 2.0,
                           input_.empty, input_.parents[i], 0);
    } else if (bar == 2) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_))) return;
        if (control_) for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], input_.parents[i], 300.0, 50.0);
        flush();
        if (!EVIDENCE(exit_count() == 2 * width_)) return;
        strategy_entry(input_.reversal, false, 300.0, na, 1.0);
        adapter_.flush_pending_entries();
        const auto handle = find_live(Family::Entry, input_.reversal);
        if (!EVIDENCE(handle.incarnation != 0)) return;
        const Snapshot& reversal = placements().at(handle.incarnation);
        const auto off_side = count([&](const Snapshot& row) {
            return is_exit(row.family) && !row.from_entry.empty()
                && !row.legs.dormant() && row.legs.target().incarnation != 0
                && row.projection_position_side != reversal.projection_position_side;
        });
        EVIDENCE(off_side == (control_ ? 0 : 2 * width_));
        struct FindBirth { Handle handle; Word ordinal = 0; } birth{handle};
        const auto status = visit_native_working_requests(&birth,
            [](void* opaque, const NativeWorkingRequest& row) {
                auto& result = *static_cast<FindBirth*>(opaque);
                if (row.definition->handle == result.handle)
                    result.ordinal = row.definition->birth.acceptance_ordinal;
                return true;
            });
        if (!EVIDENCE(status == detail::ScratchStatus::ok && birth.ordinal != 0)) return;
        // Existing function seam, real accepted reversal and its native
        // acceptance ordinal. No placement or lifecycle is injected. The
        // helpers can be called before settlement to keep their candidate
        // population available, independently of earlier cleanup helpers.
        const exit_legs::Frame cause{birth.ordinal, bar, exit_legs::Domain::Ordinary,
                                     exit_legs::Phase::Observation};
        const auto before = bracket_heap::sample();
        (adapter_.*access(Suspend{}))(reversal, cause, 100.0);
        const auto after = bracket_heap::sample();
        EVIDENCE(count([](const Snapshot& row) {
            return is_exit(row.family) && row.legs.dormant();
        }) == 2 * width_);
        EVIDENCE(after.calls > before.calls);
        // Finish this real reversal as well as exercising its helper.
        strategy_entry(input_.reversal, false, na, na, 1.0);
        adapter_.flush_pending_entries();
    } else if (bar == 3) {
        EVIDENCE(physical_position().signed_units < 0.0 && closed_trade_count() != 0);
        reached = true;
    }
}

void Host::purge(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1))) return;
        explicit_exits(control_ ? input_.empty : input_.seed);
        flush();
        if (!EVIDENCE(exit_count() == 2 * width_)) return;
        strategy_entry(input_.reversal, false, 300.0, na, 1.0);
        adapter_.flush_pending_entries();
        const auto handle = find_live(Family::Entry, input_.reversal);
        if (!EVIDENCE(handle.incarnation != 0)) return;
        const Snapshot& reversal = placements().at(handle.incarnation);
        EVIDENCE(reversal.reverse_to && !reversal.is_long);
        // Exercise the existing helper while the real reversal is pending.
        // Its usual earlier owner cleanup can already have removed all exits
        // by Applied; this seam preserves genuine held-cohort candidates.
        (adapter_.*access(Purge{}))(reversal);
        EVIDENCE(exit_count() == (control_ ? 2 * width_ : 0));
        const auto seed = cohorts().find(input_.seed);
        EVIDENCE(seed != cohorts().end() && seed->second.opened.empty());
        strategy_entry(input_.reversal, false, na, na, 1.0);
        adapter_.flush_pending_entries();
    } else if (bar == 2) {
        EVIDENCE(physical_position().signed_units < 0.0 && closed_trade_count() != 0);
        reached = true;
    }
}

void Host::reservation_family(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
        return;
    }
    if (bar != 1) return;
    if (!EVIDENCE(physical_position().signed_units == double(2 * width_ + 1))) return;
    const auto origin = opening(input_.seed);
    if (!EVIDENCE(origin.incarnation != 0)) return;
    if (test_ == Test::Reconcile)
        strategy_entry(input_.seed, true, 1.0, na, 1.0);
    explicit_exits(input_.seed);
    flush();
    if (!EVIDENCE(exit_count() == 2 * width_)) return;
    if (test_ == Test::Reservation) {
        double percent = 100.0, reserved = na;
        const double basis = double(2 * width_ + 1);
        const auto before = bracket_heap::sample();
        const bool accepted = (adapter_.*access(Compute{}))(
            input_.probe, control_ ? input_.other : input_.seed,
            1.0, percent, basis, basis, reserved);
        const auto after = bracket_heap::sample();
        EVIDENCE(accepted && reserved == 1.0 && std::abs(percent - 100.0 / basis) < 1e-12);
        EVIDENCE(control_ || after.calls > before.calls);
        // This query changes no persistent state, so also check the exact
        // immediate return boundary while the engine is still alive.
        ROW_CHECK("pineexecutionadapter_compute_exit_reservation_reservations", bracket_heap::same_live(before, after));
    } else if (test_ == Test::Reconcile) {
        if (!EVIDENCE(legs().size() == 2 * width_)) return;
        const auto before = bracket_heap::sample();
        (adapter_.*access(Reconcile{}))(input_.seed, control_ ? double(2 * width_ + 1) : 1.0);
        const auto after = bracket_heap::sample();
        // Each family contains two live handles AND two queued indices for
        // the still-pending same-id add. With one unit left, queue rank gives
        // the first family that unit; the remaining N-1 families get zero.
        EVIDENCE(exit_count() == (control_ ? 2 * width_ : 2));
        EVIDENCE(legs().size() == (control_ ? 2 * width_ : 2));
        EVIDENCE(count([](const Snapshot& row) {
            return is_exit(row.family) && row.projection_remaining_qty == 1.0
                && row.fixed_exit_reservation && !row.reservation_deferred_to_pending_entry;
        }) == (control_ ? 2 * width_ : 2));
        EVIDENCE(after.calls > before.calls);
    } else if (test_ == Test::Origin) {
        Handle target = origin;
        if (control_) {
            strategy_entry(input_.other, true, 1.0, na, 1.0);
            target = find_live(Family::Entry, input_.other);
            if (!EVIDENCE(target.incarnation != 0)) return;
        }
        (adapter_.*access(CancelOrigin{}))(target);
        EVIDENCE(exit_count() == (control_ ? 2 * width_ : 0));
    } else if (test_ == Test::Siblings) {
        if (control_) {
            (adapter_.*access(CancelSiblings{}))(origin, 0);
        } else {
            for (std::size_t i = 0; i < width_; ++i) {
                const auto limit = find_live(Family::ExitLimit, input_.exits[i]);
                if (!EVIDENCE(limit.incarnation != 0)) return;
                (adapter_.*access(CancelSiblings{}))(limit, 0);
            }
        }
        // Exactly one stop sibling per limit is cancelled; the N initiating
        // limits remain. The non-exit opening handle is the early-return
        // control. No lower-bound or work-counter proxy proves this row.
        EVIDENCE(count([](const Snapshot& row) { return row.family == Family::ExitLimit; }) == width_);
        EVIDENCE(count([](const Snapshot& row) { return row.family == Family::ExitStop; })
                  == (control_ ? width_ : 0));
    } else if (test_ == Test::Flat) {
        EVIDENCE(count([](const Snapshot& row) {
            return is_exit(row.family) && !row.pending_bound_exit
                && row.projection_position_side != static_cast<std::int32_t>(PositionSide::FLAT);
        }) == 2 * width_);
        (adapter_.*access(RetireFlat{}))(false, control_, nullptr);
        EVIDENCE(exit_count() == (control_ ? 2 * width_ : 0));
    }
    held_count = exit_count();
    reached = true;
}

void Host::margin(int bar) {
    if (bar == 0) {
        for (std::size_t i = 0; i < width_; ++i)
            strategy_entry(input_.parents[i], true, na, na, 2.0,
                           input_.empty, input_.parents[i], 0);
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_))) return;
        if (!control_) for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], input_.parents[i], 300.0, 50.0,
                          na, na, na, 100.0, input_.empty, 1.0);
        flush();
        EVIDENCE(exit_count() == (control_ ? 0 : 2 * width_));
        const auto before = bracket_heap::sample();
        (adapter_.*access(AfterMargin{}))();
        const auto after = bracket_heap::sample();
        // Each of N distinct entry ids holds 2 units; each requested exit
        // holds 1, so reconciliation normalizes each of its two legs to 50%.
        EVIDENCE(count([](const Snapshot& row) {
            return is_exit(row.family) && row.projection_remaining_qty == 1.0
                && row.qty_percent == 50.0 && row.fixed_exit_reservation;
        }) == (control_ ? 0 : 2 * width_));
        EVIDENCE(control_ || after.calls > before.calls);
        reached = true;
    }
}

void Host::recalc(int bar) {
    if (adapter_.fixture_coof_recalc_active()) {
        if (!recalc_placed_ && physical_position().signed_units > 0.0) {
            recalc_placed_ = true;
            for (std::size_t i = 0; i < width_; ++i)
                strategy_entry(input_.parents[i], true, 1.0, na, 1.0);
            EVIDENCE(count([](const Snapshot& row) {
                return row.family == Family::Entry && row.opening && row.is_long
                    && row.projection_created_during_coof;
            }) == width_);
        }
        return;
    }
    if (!seeded_ && bar == 0) {
        seeded_ = true;
        strategy_entry(input_.seed, true, na, na, 1.0);
    } else if (recalc_placed_ && !reached) {
        EVIDENCE(count([&](const Snapshot& row) {
            return row.family == Family::Entry && row.opening && row.is_long
                && row.projection_created_during_coof && row.projection_created_bar == bar;
        }) == width_);
        // A plain market call is the unpriced guard control. A priced entry
        // at the close cancels all N requests born in the open's real fill
        // recalculation, which have rested unfilled throughout this bar.
        strategy_entry(input_.probe, true, control_ ? na : 2.0, na, 1.0);
        EVIDENCE(count([](const Snapshot& row) {
            return row.family == Family::Entry && row.projection_created_during_coof;
        }) == (control_ ? width_ : 0));
        reached = true;
    }
}

void Host::sell(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, na, double(2 * width_));
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == double(2 * width_))) return;
        const double one_unit_percent = 100.0 / double(2 * width_);
        for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], input_.seed, 300.0, na,
                          na, na, na, one_unit_percent);
        flush();
        if (!EVIDENCE(exit_count() == width_ && count([](const Snapshot& row) {
                return is_exit(row.family) && row.bracket_origin.incarnation == 0;
            }) == width_)) return;
        strategy_entry(input_.reversal, false);
        EVIDENCE(exit_count() == width_); // A fresh sell has no replacement branch.
        if (!control_) strategy_entry(input_.reversal, false);
        EVIDENCE(exit_count() == (control_ ? width_ : 0));
        reached = true;
    }
}

void Host::child(int bar) {
    if (bar == 0) {
        strategy_entry(input_.seed, true, na, 150.0);
        for (std::size_t i = 0; i < width_; ++i)
            strategy_exit(input_.exits[i], input_.seed, 300.0, 50.0);
        EVIDENCE(exit_count() == 2 * width_);
    } else if (bar == 1) {
        if (!EVIDENCE(physical_position().signed_units == 0.0 && exit_count() == 2 * width_)) return;
        if (!control_) strategy_cancel(input_.seed);
        EVIDENCE(adapter_.fixture_named_entry_cancel_active(input_.seed) == !control_);
        strategy_entry(input_.seed, true, na, 160.0);
        std::size_t recreated = 0, captured = 0;
        for (const auto& pending : entries()) {
            if (pending.snapshot.source_id != input_.seed) continue;
            if (pending.snapshot.retained_parent_topology) ++recreated;
            if (pending.snapshot.named_cancel_surviving_exit_incarnation != 0
                && pending.snapshot.recreated_after_named_cancelled_entry_incarnation != 0) ++captured;
        }
        // The function reads child_families.size(): one family carries its
        // exact surviving-child token; N>1 families deliberately carry none.
        EVIDENCE(recreated == (control_ ? 0 : 1));
        EVIDENCE(captured == (!control_ && width_ == 1 ? 1 : 0));
        EVIDENCE(exit_count() == 2 * width_);
        held_count = recreated;
        reached = true;
    }
}

void Host::on_source_bar(const Bar&) {
    const int bar = pine_bar_index();
    const bool coof = adapter_.fixture_coof_recalc_active();
    if (test_ == Test::Empty) return;
    if (cleaned) {
        final_position = physical_position().signed_units;
        return;
    }
    if (!coof && bar == hold_ + 3) {
        // End each complete command cycle by cancelling the pending ladder
        // and physically closing any surviving position. The caller either
        // destroys this host before sampling or retains it for reuse sampling.
        cleaned = true;
        finish_commands();
        return;
    }
    if (coof && test_ != Test::Recalc) return;
    switch (test_) {
    case Test::Hash:
        if (control_) {
            if (bar == 2) hash();
        } else if (bar == 0) {
            strategy_entry(input_.seed, true, na, na, double(2 * width_ + 1));
        } else if (bar == 1) {
            for (std::size_t i = 0; i < width_; ++i) {
                strategy_exit(input_.exits[i], input_.seed, na, na,
                              na, 8.0, 300.0, 100.0, input_.empty, 1.0);
                strategy_order(input_.parents[i], true, 1.0, 1.0);
            }
            flush();
            EVIDENCE(exit_count() == width_);
        } else if (bar == 2) hash();
        break;
    case Test::Prefix: prefix(bar); break;
    case Test::Owners: owners(bar); break;
    case Test::VoidNamed: void_named(bar); break;
    case Test::Stash: case Test::ReservedMap: stash_or_map(bar); break;
    case Test::VoidGlobal: void_global(bar); break;
    case Test::DropLeg: drop_leg(bar); break;
    case Test::PreDrain: pre_drain(bar); break;
    case Test::Ordered: ordered(bar); break;
    case Test::Anchor: anchor(bar); break;
    case Test::Policy: policy(bar); break;
    case Test::Suspend: suspend(bar); break;
    case Test::Purge: purge(bar); break;
    case Test::Reservation: case Test::Reconcile: case Test::Origin:
    case Test::Siblings: case Test::Flat: reservation_family(bar); break;
    case Test::Margin: margin(bar); break;
    case Test::Recalc: recalc(bar); break;
    case Test::Sell: sell(bar); break;
    case Test::Child: child(bar); break;
    case Test::Empty: break;
    }
}

void assert_release(Test test, bracket_heap::Sample now, bracket_heap::Sample before_first,
                    bracket_heap::Sample after_warmup) {
    // Original post-destruction boundary, including the first nonempty run.
    // This detects fixed static retention even when reuse warm-up absorbs it.
    // It proves only that nothing allocated by the command outlives its host.
#define RELEASE_ROW(row) ROW_CHECK(row, now.bytes == before_first.bytes \
    && now.blocks == before_first.blocks && now.bytes == after_warmup.bytes \
    && now.blocks == after_warmup.blocks)
    switch (test) {
    case Test::Hash:
        RELEASE_ROW("pineexecutionadapter_hash_state_live_keys");
        RELEASE_ROW("pineexecutionadapter_hash_state_bracket_keys");
        RELEASE_ROW("pineexecutionadapter_hash_state_trail_open_keys");
        break;
    case Test::Prefix: RELEASE_ROW("pineexecutionadapter_retire_origin_prefixes_named"); break;
    case Test::Owners: RELEASE_ROW("pineexecutionadapter_cancel_reversal_exit_owners_owners"); break;
    case Test::VoidNamed: RELEASE_ROW("pineexecutionadapter_withdraw_void_exits_handles"); break;
    case Test::Stash: RELEASE_ROW("pineexecutionadapter_stash_id_bound_exits_handles"); break;
    case Test::VoidGlobal: RELEASE_ROW("pineexecutionadapter_settle_void_global_exits_handles"); break;
    case Test::ReservedMap: RELEASE_ROW("pineexecutionadapter_exit_submit_leg_submit_one_reserved_by_exit"); break;
    case Test::DropLeg: RELEASE_ROW("pineexecutionadapter_exit_withdrawn"); break;
    case Test::PreDrain:
        RELEASE_ROW("pineexecutionadapter_flush_pending_bracket_legs_book_keys");
        RELEASE_ROW("pineexecutionadapter_flush_pending_bracket_legs_resting");
        break;
    case Test::Ordered: RELEASE_ROW("pineexecutionadapter_flush_pending_entries_ordered"); break;
    case Test::Anchor: RELEASE_ROW("pineexecutionadapter_anchor_relative_exits_parents"); break;
    case Test::Policy:
        RELEASE_ROW("pineexecutionadapter_initialize_l4c_policy_candidates");
        RELEASE_ROW("pineexecutionadapter_initialize_l4c_policy_candidate_refs");
        break;
    case Test::Suspend: RELEASE_ROW("pineexecutionadapter_suspend_brackets_for_reversal_cross_side_by_entry"); break;
    case Test::Purge: RELEASE_ROW("pineexecutionadapter_purge_brackets_after_applied_reversal_stale"); break;
    case Test::Reservation: RELEASE_ROW("pineexecutionadapter_compute_exit_reservation_reservations"); break;
    case Test::Reconcile:
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_family_handles");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_family_queued");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_families");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_cancel");
        break;
    case Test::Margin: RELEASE_ROW("pineexecutionadapter_reconcile_exit_reservations_after_margin_entries"); break;
    case Test::Origin: RELEASE_ROW("pineexecutionadapter_cancel_bracket_origin_matches"); break;
    case Test::Siblings: RELEASE_ROW("pineexecutionadapter_cancel_bracket_siblings_matches"); break;
    case Test::Flat: RELEASE_ROW("pineexecutionadapter_retire_in_position_exits_at_flat_handles"); break;
    case Test::Recalc: RELEASE_ROW("pineexecutionadapter_place_entry_stale_recalc_entries"); break;
    case Test::Sell: RELEASE_ROW("pineexecutionadapter_place_entry_dynamic_carried_legs"); break;
    case Test::Child: RELEASE_ROW("pineexecutionadapter_submit_entry_with_policy_child_families"); break;
    case Test::Empty: break;
    }
#undef RELEASE_ROW
    if (!bracket_heap::same_live(now, before_first) || !bracket_heap::same_live(now, after_warmup))
        std::fprintf(stderr, "HEAP [%s] phase=post-destruction before=%zu/%zu warm=%zu/%zu after=%zu/%zu bytes/blocks\n",
                     current->name, before_first.bytes, before_first.blocks,
                     after_warmup.bytes, after_warmup.blocks, now.bytes, now.blocks);
}

void assert_release(Test test, bracket_heap::Sample now, bracket_heap::Sample reference) {
    // The reference follows a fixed number of runs of the same width,
    // control and tape on this very host. Subsequent engine-owned growth
    // remains counted: no destruction, capacity deduction or tolerance.
    // Fixed capacity already in the reference is not immediate-release proof.
#define RELEASE_ROW(row) ROW_CHECK(row, now.bytes == reference.bytes \
    && now.blocks == reference.blocks)
    switch (test) {
    case Test::Hash:
        RELEASE_ROW("pineexecutionadapter_hash_state_live_keys");
        RELEASE_ROW("pineexecutionadapter_hash_state_bracket_keys");
        RELEASE_ROW("pineexecutionadapter_hash_state_trail_open_keys");
        break;
    case Test::Prefix: RELEASE_ROW("pineexecutionadapter_retire_origin_prefixes_named"); break;
    case Test::Owners: RELEASE_ROW("pineexecutionadapter_cancel_reversal_exit_owners_owners"); break;
    case Test::VoidNamed: RELEASE_ROW("pineexecutionadapter_withdraw_void_exits_handles"); break;
    case Test::Stash: RELEASE_ROW("pineexecutionadapter_stash_id_bound_exits_handles"); break;
    case Test::VoidGlobal: RELEASE_ROW("pineexecutionadapter_settle_void_global_exits_handles"); break;
    case Test::ReservedMap: RELEASE_ROW("pineexecutionadapter_exit_submit_leg_submit_one_reserved_by_exit"); break;
    case Test::DropLeg: RELEASE_ROW("pineexecutionadapter_exit_withdrawn"); break;
    case Test::PreDrain:
        RELEASE_ROW("pineexecutionadapter_flush_pending_bracket_legs_book_keys");
        RELEASE_ROW("pineexecutionadapter_flush_pending_bracket_legs_resting");
        break;
    case Test::Ordered: RELEASE_ROW("pineexecutionadapter_flush_pending_entries_ordered"); break;
    case Test::Anchor: RELEASE_ROW("pineexecutionadapter_anchor_relative_exits_parents"); break;
    case Test::Policy:
        RELEASE_ROW("pineexecutionadapter_initialize_l4c_policy_candidates");
        RELEASE_ROW("pineexecutionadapter_initialize_l4c_policy_candidate_refs");
        break;
    case Test::Suspend: RELEASE_ROW("pineexecutionadapter_suspend_brackets_for_reversal_cross_side_by_entry"); break;
    case Test::Purge: RELEASE_ROW("pineexecutionadapter_purge_brackets_after_applied_reversal_stale"); break;
    case Test::Reservation: RELEASE_ROW("pineexecutionadapter_compute_exit_reservation_reservations"); break;
    case Test::Reconcile:
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_family_handles");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_family_queued");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_families");
        RELEASE_ROW("pineexecutionadapter_reconcile_deferred_exit_reservations_cancel");
        break;
    case Test::Margin: RELEASE_ROW("pineexecutionadapter_reconcile_exit_reservations_after_margin_entries"); break;
    case Test::Origin: RELEASE_ROW("pineexecutionadapter_cancel_bracket_origin_matches"); break;
    case Test::Siblings: RELEASE_ROW("pineexecutionadapter_cancel_bracket_siblings_matches"); break;
    case Test::Flat: RELEASE_ROW("pineexecutionadapter_retire_in_position_exits_at_flat_handles"); break;
    case Test::Recalc: RELEASE_ROW("pineexecutionadapter_place_entry_stale_recalc_entries"); break;
    case Test::Sell: RELEASE_ROW("pineexecutionadapter_place_entry_dynamic_carried_legs"); break;
    case Test::Child: RELEASE_ROW("pineexecutionadapter_submit_entry_with_policy_child_families"); break;
    case Test::Empty: break;
    }
#undef RELEASE_ROW
    if (!bracket_heap::same_live(now, reference))
        std::fprintf(stderr, "HEAP [%s] phase=retained-host width=%zu cycle=%zu control=%d reference=%zu/%zu after=%zu/%zu bytes/blocks\n",
                     current->name, width_now, iteration_now, int(control_now),
                     reference.bytes, reference.blocks, now.bytes, now.blocks);
}

bracket_heap::Sample cycle(Host& host, const Inputs& input, std::size_t width,
                       bool control, int hold) {
    width_now = width;
    control_now = control;
    try {
        // The caller owns host across EVERY warm-up and measured run in this
        // family. Reset the test's flags, not the product's retained storage.
        host.prepare_cycle(width, control, hold);
        host.run(input.bars.data(), hold + 6, "1", "1");
        EVIDENCE(host.last_error().empty());
        if (!host.last_error().empty())
            std::fprintf(stderr, "engine error [%s]: %s\n", current->name, host.last_error().c_str());
        EVIDENCE(host.reached && host.cleaned);
        EVIDENCE(host.final_position == 0.0 && host.physical_position().signed_units == 0.0);
    } catch (const std::exception& error) {
        family_check(false, error.what(), __LINE__);
    } catch (...) {
        family_check(false, "unknown exception in command cycle", __LINE__);
    }
    return bracket_heap::sample(); // host is still alive, even on a failed run.
}

void measure_cycles(Host& host, const Inputs& input, std::size_t width,
                    bool control, int hold) {
    iteration_now = 0;
    std::size_t warmup_runs = 3;
    // PlacementTable::erase keeps up to kSpareNodes whole nodes;
    // clear() leaves spare_ intact, and overwrite assigns their strings.
    // A short id can retain a previous long id's capacity
    // while another recycled node receives that long id. These fixtures add
    // replacement/reversal and exit roles, warming more than three rotations.
    // Suspend width=16 stabilises at total run 6 (measured
    // cycles 3..64); Purge, Reconcile and Sell stabilise at total run 4
    // (cycles 1..64). Deltas are multiples of one 31-byte string allocation,
    // not continuing per-run growth. These are FIXED family schedules; no
    // allocation value decides when warm-up ends. Keep all 64 exact checks.
    switch (current->test) {
    case Test::Suspend: warmup_runs = 6; break;
    case Test::Purge: case Test::Reconcile: case Test::Sell: warmup_runs = 4; break;
    default: break;
    }
    for (std::size_t run = 0; run < warmup_runs; ++run)
        cycle(host, input, width, control, hold);
    const auto reference = bracket_heap::sample();
    bracket_heap::Sample at_n{}, at_4n{};
    for (std::size_t iteration = 1; iteration <= 64; ++iteration) {
        iteration_now = iteration;
        const auto after = cycle(host, input, width, control, hold);
        // Keep successful samples visible too for the four late-warming
        // families. Diagnostics borrow scalars and allocate no model storage.
        if (current->test == Test::Suspend || current->test == Test::Purge
            || current->test == Test::Reconcile || current->test == Test::Sell)
            std::fprintf(stderr, "CYCLE [%s] width=%zu cycle=%zu control=%d reference=%zu/%zu after=%zu/%zu bytes/blocks\n",
                         current->name, width, iteration, int(control),
                         reference.bytes, reference.blocks, after.bytes, after.blocks);
        assert_release(current->test, after, reference);
        if (iteration == 16) at_n = after;
        if (iteration == 64) at_4n = after;
    }
    // Endpoint checks supplement, rather than replace, EVERY-run sampling.
    assert_release(current->test, at_n, reference);
    assert_release(current->test, at_4n, reference);
    assert_release(current->test, at_4n, at_n);
    std::fprintf(stderr, "MEMORY [%s] width=%zu control=%d reference=%zu/%zu N=%zu/%zu 4N=%zu/%zu bytes/blocks\n",
                 current->name, width, int(control),
                 reference.bytes, reference.blocks, at_n.bytes, at_n.blocks,
                 at_4n.bytes, at_4n.blocks);
}

bracket_heap::Sample cycle(const Inputs& input, std::size_t width, bool control, int hold) {
    width_now = width;
    control_now = control;
    try {
        // Original fresh-host cycle: sample only after this owner is gone.
        Host host(current->test, input, width, control, hold);
        host.run(input.bars.data(), hold + 6, "1", "1");
        EVIDENCE(host.last_error().empty());
        if (!host.last_error().empty())
            std::fprintf(stderr, "engine error [%s]: %s\n", current->name, host.last_error().c_str());
        EVIDENCE(host.reached && host.cleaned);
        EVIDENCE(host.final_position == 0.0 && host.physical_position().signed_units == 0.0);
    } catch (const std::exception& error) {
        family_check(false, error.what(), __LINE__);
    } catch (...) {
        family_check(false, "unknown exception in command cycle", __LINE__);
    }
    return bracket_heap::sample();
}

void run_group(const Inputs& input) {
    // Prime only process-global, input-independent setup. This run places no
    // request: no tested local can acquire backing here. Reusing UTC/24x7
    // also prevents SessionKey's documented intern pool from becoming a
    // different retained population in each command cycle.
    {
        Host empty(Test::Empty, input, 1, false, 4);
        empty.run(input.bars.data(), 10, "1", "1");
        EVIDENCE(empty.last_error().empty());
    }
    bracket_heap::recording = true;
    // Run the original destruction proof FIRST, before any populated
    // retained-host warm-up can hide a fixed static in its initial sample.
    const auto initial = bracket_heap::sample();
    iteration_now = 0;
    const auto warm = cycle(input, 1, false, 4);
    assert_release(current->test, warm, initial, initial);
    for (const std::size_t width : {std::size_t{16}, std::size_t{64}}) {
        iteration_now = 0;
        const int hold = static_cast<int>(width) - 6;
        const auto guard = cycle(input, width, true, hold);
        assert_release(current->test, guard, initial, warm);
        for (std::size_t iteration = 1; iteration <= width; ++iteration) {
            iteration_now = iteration;
            const auto after = cycle(input, width, false, hold);
            assert_release(current->test, after, initial, warm);
        }
    }
    // A separate proof with the SAME engine retained for every reuse run.
    Host host(current->test, input, 1, false, 4);
    // Preserve the original width-one stimulus (including Child's unique
    // single-family provenance branch) and measure its reuse as well.
    measure_cycles(host, input, 1, false, 4);
    // Preserve BOTH original controls and populations, including the 16/64
    // bar tapes. Warm each fixed stimulus on the schedule above;
    // increasing the input population is not itself per-run storage growth.
    // All configurations use this ONE host, without reconstructing it.
    for (const std::size_t width : {std::size_t{16}, std::size_t{64}}) {
        const int hold = static_cast<int>(width) - 6;
        measure_cycles(host, input, width, true, hold);
        measure_cycles(host, input, width, false, hold);
    }
    bracket_heap::recording = false;
}

void allocator_control() {
    const auto before = bracket_heap::sample();
    bracket_heap::recording = true;
    const auto alignment = std::align_val_t{64};
    // Independent sum of the requested sizes: 1+3+5+7+11+13+17+19=76.
    std::array<void*, 8> blocks{{
        ::operator new(1), ::operator new[](3),
        ::operator new(5, std::nothrow), ::operator new[](7, std::nothrow),
        ::operator new(11, alignment), ::operator new[](13, alignment),
        ::operator new(17, alignment, std::nothrow), ::operator new[](19, alignment, std::nothrow)}};
    const auto held = bracket_heap::sample();
    ROW_CHECK("allocator-release", held.bytes == before.bytes + 76 && held.blocks == before.blocks + 8);
    bool aligned = true;
    for (std::size_t i = 4; i < blocks.size(); ++i)
        aligned = aligned && reinterpret_cast<std::uintptr_t>(blocks[i]) % 64 == 0;
    ROW_CHECK("allocator-release", aligned);
    ::operator delete(blocks[0], std::size_t{1});
    ::operator delete[](blocks[1]);
    ::operator delete(blocks[2], std::nothrow);
    ::operator delete[](blocks[3], std::size_t{7});
    ::operator delete(blocks[4], std::size_t{11}, alignment);
    ::operator delete[](blocks[5], alignment);
    ::operator delete(blocks[6], alignment, std::nothrow);
    ::operator delete[](blocks[7], std::size_t{19}, alignment);
    bracket_heap::recording = false;
    ROW_CHECK("allocator-release", bracket_heap::same_live(bracket_heap::sample(), before));
}

bool write_totals(int fd, const Totals& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    std::size_t done = 0;
    while (done != sizeof value) {
        const auto result = ::write(fd, bytes + done, sizeof value - done);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        done += static_cast<std::size_t>(result);
    }
    return true;
}
bool read_totals(int fd, Totals& value) noexcept {
    auto* bytes = reinterpret_cast<unsigned char*>(&value);
    std::size_t done = 0;
    while (done != sizeof value) {
        const auto result = ::read(fd, bytes + done, sizeof value - done);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        done += static_cast<std::size_t>(result);
    }
    return true;
}
} // namespace memory_release_bracket_families

int main(int argc, char** argv) {
    using namespace memory_release_bracket_families;
    allocator_control();
    const Inputs input;
    bool selected = false;
    for (const auto& group : groups) {
        if (argc == 2 && std::strcmp(argv[1], group.name) != 0) continue;
        current = &group;
        selected = true;
        int descriptors[2];
        if (!EVIDENCE(::pipe(descriptors) == 0)) continue;
        // Only the test runner forks. The parent never invokes an engine
        // command, so each child's function-static storage starts cold.
        const auto child = ::fork();
        if (child == 0) {
            ::close(descriptors[0]);
            totals = {};
            try {
                run_group(input);
            } catch (const std::exception& error) {
                family_check(false, error.what(), __LINE__);
            } catch (...) {
                family_check(false, "unknown exception in family dispatch", __LINE__);
            }
            bracket_heap::recording = false;
            const bool sent = write_totals(descriptors[1], totals);
            ::close(descriptors[1]);
            std::fflush(stderr);
            ::_exit(!sent || totals.failures ? 1 : 0);
        }
        ::close(descriptors[1]);
        if (child < 0) {
            ::close(descriptors[0]);
            EVIDENCE(false && "fork failed");
            continue;
        }
        Totals result;
        const bool read = read_totals(descriptors[0], result);
        ::close(descriptors[0]);
        int status = 0;
        pid_t waited;
        do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (!EVIDENCE(read && waited == child && WIFEXITED(status))) continue;
        EVIDENCE(WEXITSTATUS(status) == (result.failures ? 1 : 0));
        totals.checks += result.checks;
        totals.failures += result.failures;
    }
    ROW_CHECK("bracket-dispatch", selected && argc <= 2);
    std::printf("memory proof release_bracket_families: %zu checks, %zu failures\n", totals.checks, totals.failures);
    return totals.failures ? 1 : 0;
}
