// Order and close commands show no per-run growth on one retained host after
// three warm-up runs; order_same_point_entries and hash_state release at return
// with the host alive.
// The command fixtures exercise real source/native submission and settlement.
// No expected heap byte literal comes from recorded output or an implementation layout.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_intent_state.hpp"

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
#include <string>
#include <utility>
#include <vector>

// A complete replacement belongs to this executable alone. Counting is
// enabled in main's thread BEFORE any fixture is constructed. Other threads
// start with enabled=false. A counted block's eventual delete is charged even
// if another thread releases it. There is no allocating registry, no sampling,
// no size-class filter, and no reset of the live counters between trials.
namespace order_close_heap {
thread_local bool enabled = false;
std::atomic<std::size_t> bytes{0}, blocks{0}, allocations{0};
struct Header { void* base; std::size_t size; bool counted; };
struct Sample { std::size_t bytes, blocks; };
Sample sample() noexcept {
    return {bytes.load(std::memory_order_relaxed), blocks.load(std::memory_order_relaxed)};
}
void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) size = 1;
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (alignment - 1 > limit - sizeof(Header)) return nullptr;
    const auto extra = sizeof(Header) + alignment - 1;
    if (size > limit - extra) return nullptr;
    void* base = std::malloc(size + extra);
    if (!base) return nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto address = (start + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    auto* result = reinterpret_cast<unsigned char*>(address);
    const Header header{base, size, enabled};
    std::memcpy(result - sizeof(Header), &header, sizeof header);
    if (header.counted) {
        bytes.fetch_add(size, std::memory_order_relaxed);
        blocks.fetch_add(1, std::memory_order_relaxed);
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
void release(void* pointer) noexcept {
    if (!pointer) return;
    Header header{};
    std::memcpy(&header, static_cast<unsigned char*>(pointer) - sizeof(Header), sizeof header);
    if (header.counted) {
        bytes.fetch_sub(header.size, std::memory_order_relaxed);
        blocks.fetch_sub(1, std::memory_order_relaxed);
    }
    std::free(header.base);
}
void* throwing(std::size_t size, std::size_t alignment = 0) {
    for (;;) {
        if (void* result = allocate(size, alignment)) return result;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nothrow(std::size_t size, std::size_t alignment = 0) noexcept {
    try { return throwing(size, alignment); } catch (...) { return nullptr; }
}
} // namespace order_close_heap

void* operator new(std::size_t n) { return order_close_heap::throwing(n); }
void* operator new[](std::size_t n) { return order_close_heap::throwing(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return order_close_heap::nothrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return order_close_heap::nothrow(n); }
void* operator new(std::size_t n, std::align_val_t a) { return order_close_heap::throwing(n, std::size_t(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return order_close_heap::throwing(n, std::size_t(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return order_close_heap::nothrow(n, std::size_t(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return order_close_heap::nothrow(n, std::size_t(a));
}
void operator delete(void* p) noexcept { order_close_heap::release(p); }
void operator delete[](void* p) noexcept { order_close_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { order_close_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { order_close_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { order_close_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { order_close_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { order_close_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { order_close_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { order_close_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { order_close_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { order_close_heap::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { order_close_heap::release(p); }

namespace {
namespace pf = pineforge;
namespace no = pineforge::native_order;
namespace sd = pineforge::source::detail;
using Adapter = pf::source::PineExecutionAdapter;
using Family = pf::source::PineOrderFamily;
using Snapshot = pf::source::PlacementSnapshot;
using Sample = order_close_heap::Sample;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = 1736121600000LL;

// Existing explicit-instantiation access pattern. Only real owners and
// real member functions are observed; nothing writes their data members.
template<class Tag, auto Member> struct Access { friend auto access(Tag) { return Member; } };
#define SEAM(Tag, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Adapter::member>
SEAM(Placements, placement_);
SEAM(Live, live_handles_);
SEAM(Entries, pending_entries_);
SEAM(Bindings, id_bound_exits_);
SEAM(Brackets, pending_bracket_legs_);
SEAM(Relative, pending_relative_exits_);
SEAM(Delayed, delayed_market_orders_);
SEAM(SameBar, pending_same_bar_commands_);
SEAM(Cancels, named_entry_cancel_tokens_);
SEAM(CloseSites, close_batch_callsites_);
SEAM(FillEntries, fill_pooc_close_entries);
SEAM(FillPair, fill_pooc_close_pair);
SEAM(OrderEntries, order_same_point_entries);
SEAM(Reconcile, reconcile_deferred_exit_reservations);
#undef SEAM
struct Terminal { friend auto access(Terminal); };
template struct Access<Terminal, static_cast<void (Adapter::*)(
    const pf::NativeDecisionContext&, bool)>(&Adapter::apply_terminal_explicit_market_policy)>;

unsigned checks = 0, failures = 0;
unsigned cycle_number = 0;
bool is_control = false;
const char* phase = "dispatch";
void check(const char* row, bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (control=%d cycle=%u phase=%s)\n",
                 row, __FILE__, line, expression, int(is_control), cycle_number, phase);
}
#define ROW_CHECK(row, expression) check(row, bool(expression), #expression, __LINE__)

// The allocator control uses direct allocation-function calls so C++'s
// permitted new-expression elision cannot remove the experiment. Eight
// separately requested blocks of 11..18 bytes sum to (11+18)*8/2 = 116.
void allocator_control() {
    const auto before = order_close_heap::sample();
    std::array<void*, 8> p{{
        ::operator new(11), ::operator new[](12),
        ::operator new(13, std::nothrow), ::operator new[](14, std::nothrow),
        ::operator new(15, std::align_val_t{64}),
        ::operator new[](16, std::align_val_t{64}),
        ::operator new(17, std::align_val_t{64}, std::nothrow),
        ::operator new[](18, std::align_val_t{64}, std::nothrow)}};
    const auto held = order_close_heap::sample();
    ROW_CHECK("allocator-release", std::all_of(p.begin(), p.end(), [](void* x) { return x != nullptr; }));
    ROW_CHECK("allocator-release", held.bytes == before.bytes + 116);
    ROW_CHECK("allocator-release", held.blocks == before.blocks + p.size());
    for (std::size_t i = 4; i != p.size(); ++i)
        ROW_CHECK("allocator-release", reinterpret_cast<std::uintptr_t>(p[i]) % 64 == 0);
    ::operator delete(p[0], std::size_t{11});
    ::operator delete[](p[1], std::size_t{12});
    ::operator delete(p[2], std::nothrow);
    ::operator delete[](p[3], std::nothrow);
    ::operator delete(p[4], std::size_t{15}, std::align_val_t{64});
    ::operator delete[](p[5], std::size_t{16}, std::align_val_t{64});
    ::operator delete(p[6], std::align_val_t{64}, std::nothrow);
    ::operator delete[](p[7], std::align_val_t{64}, std::nothrow);
    const auto after = order_close_heap::sample();
    ROW_CHECK("allocator-release", after.bytes == before.bytes);
    ROW_CHECK("allocator-release", after.blocks == before.blocks);
}

enum class Mode {
    terminal, marketable, pair, order, hash, close_empty, fifo, newborns,
    close_all, cap_add, rearm, bracket, flags, relative, delayed, children,
    buy_replacement, sell_replacement, close_sites
};
struct Row { const char* id; Mode mode; };
constexpr Row rows[] = {
    {"pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", Mode::terminal}, {"pineexecutionadapter_fill_pooc_close_entries_marketable", Mode::marketable},
    {"pineexecutionadapter_fill_pooc_close_pair_legs", Mode::pair}, {"pineexecutionadapter_order_same_point_entries_candidates", Mode::order},
    {"pineexecutionadapter_hash_state_named_cancel_keys", Mode::hash}, {"pineexecutionadapter_close_opposite_entries", Mode::close_empty},
    {"pineexecutionadapter_close_preceding_fifo_closes", Mode::fifo}, {"pineexecutionadapter_close_all_newborns", Mode::newborns},
    {"pineexecutionadapter_close_all_opposite_entries", Mode::close_all}, {"pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", Mode::cap_add},
    {"pineexecutionadapter_rearm_id_bound_exits_rearm", Mode::rearm}, {"pineexecutionadapter_materialize_pending_bracket_legs_ready", Mode::bracket},
    {"pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", Mode::flags}, {"pineexecutionadapter_materialize_relative_exits_pending", Mode::relative},
    {"pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", Mode::delayed}, {"pineexecutionadapter_stage_flat_children_before_parent_children", Mode::children},
    {"pineexecutionadapter_place_entry_superseded_buy_replacements", Mode::buy_replacement}, {"pineexecutionadapter_place_entry_superseded_siblings", Mode::sell_replacement},
    {"pineexecutionadapter_flush_pending_closes_sites", Mode::close_sites}
};

// Each row has its own named release assertion, even when the dispatcher and
// repetition mechanism are shared. Both counts must agree, at EVERY cycle.
void released(const Row& row, Sample first, Sample current) {
    switch (row.mode) {
    case Mode::terminal:
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", current.blocks == first.blocks); break;
    case Mode::marketable:
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_entries_marketable", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_entries_marketable", current.blocks == first.blocks); break;
    case Mode::pair:
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_pair_legs", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_pair_legs", current.blocks == first.blocks); break;
    case Mode::order:
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", current.blocks == first.blocks); break;
    case Mode::hash:
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", current.blocks == first.blocks); break;
    case Mode::close_empty:
        ROW_CHECK("pineexecutionadapter_close_opposite_entries", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_close_opposite_entries", current.blocks == first.blocks); break;
    case Mode::fifo:
        ROW_CHECK("pineexecutionadapter_close_preceding_fifo_closes", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_close_preceding_fifo_closes", current.blocks == first.blocks); break;
    case Mode::newborns:
        ROW_CHECK("pineexecutionadapter_close_all_newborns", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_close_all_newborns", current.blocks == first.blocks); break;
    case Mode::close_all:
        ROW_CHECK("pineexecutionadapter_close_all_opposite_entries", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_close_all_opposite_entries", current.blocks == first.blocks); break;
    case Mode::cap_add:
        ROW_CHECK("pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", current.blocks == first.blocks); break;
    case Mode::rearm:
        ROW_CHECK("pineexecutionadapter_rearm_id_bound_exits_rearm", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_rearm_id_bound_exits_rearm", current.blocks == first.blocks); break;
    case Mode::bracket:
        ROW_CHECK("pineexecutionadapter_materialize_pending_bracket_legs_ready", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_materialize_pending_bracket_legs_ready", current.blocks == first.blocks); break;
    case Mode::flags:
        ROW_CHECK("pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", current.blocks == first.blocks); break;
    case Mode::relative:
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", current.blocks == first.blocks); break;
    case Mode::delayed:
        ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", current.blocks == first.blocks); break;
    case Mode::children:
        ROW_CHECK("pineexecutionadapter_stage_flat_children_before_parent_children", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_stage_flat_children_before_parent_children", current.blocks == first.blocks); break;
    case Mode::buy_replacement:
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_buy_replacements", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_buy_replacements", current.blocks == first.blocks); break;
    case Mode::sell_replacement:
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", current.blocks == first.blocks); break;
    case Mode::close_sites:
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", current.bytes == first.bytes);
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", current.blocks == first.blocks); break;
    }
}

class Host final : public pf::source::PineStrategyHost {
public:
    Host(const Row& row, bool control) : row_(row), control_(control) {
        pf::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pf::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 3;
        config.margin_long = config.margin_short = 100.0;
        config.commission_value = 0.0;
        switch (row.mode) {
        case Mode::terminal:
        case Mode::pair:
            config.process_orders_on_close = true;
            config.pyramiding = row.mode == Mode::pair && control ? 1 : 0; break;
        case Mode::marketable:
            config.process_orders_on_close = true; break;
        case Mode::order:
            config.close_entries_rule_any = control; break;
        case Mode::newborns:
            config.calc_on_order_fills = !control; break;
        case Mode::cap_add:
            config.process_orders_on_close = true;
            config.pyramiding = control ? 2 : 1;
            config.margin_long = config.margin_short = 0.0; break;
        case Mode::rearm:
        case Mode::bracket:
            config.margin_long = config.margin_short = 0.0; break;
        case Mode::flags:
            config.pyramiding = control ? 3 : 2; break;
        case Mode::delayed:
            config.calc_on_order_fills = true; break;
        case Mode::children:
        case Mode::close_sites:
            config.process_orders_on_close = true;
            config.margin_long = config.margin_short = 0.0; break;
        default: break;
        }
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void cycle(const std::array<pf::Bar, 4>& tape) {
        // Only test-owned scalars are reset here. The public run door resets
        // the engine's and adapter's logical run state and receipts itself;
        // reusable storage can keep capacity across runs (see run_row).
        started_ = observed_ = coof_done_ = cap_added_ = false;
        fills_ = seed_fills_ = 0;
        phase = "real source run";
        run(tape.data(), static_cast<int>(tape.size()), "1", "1", false);
        ROW_CHECK(row_.id, last_error().empty());
        ROW_CHECK(row_.id, native_state().kind == pf::NativeLifecycleKind::Completed);
        ROW_CHECK(row_.id, started_ && observed_);
        if (!last_error().empty())
            std::fprintf(stderr, "DETAIL [%s] engine error: %s\n", row_.id, last_error().c_str());
    }

    void on_source_bar(const pf::Bar&) override {
        if (adapter_.fixture_coof_recalc_active()) {
            if (row_.mode == Mode::delayed && !coof_done_) deferred_callback();
            return;
        }
        const int bar = pine_bar_index();
        if (bar == 0 && !started_) {
            started_ = true;
            phase = "command branch";
            switch (row_.mode) {
            case Mode::terminal: terminal(); break;
            case Mode::marketable: marketable(); break;
            case Mode::pair: pair(); break;
            case Mode::order: order(); break;
            case Mode::hash: hash(); break;
            case Mode::close_empty:
            case Mode::newborns:
            case Mode::close_all:
            case Mode::cap_add:
            case Mode::rearm:
            case Mode::sell_replacement:
                queue_seed(); break;
            case Mode::fifo:
                queue_seed("seed-A", true, 1.0);
                if (!control_) queue_seed("seed-B", true, 2.0);
                break;
            case Mode::bracket: bracket_begin(); break;
            case Mode::flags: flags(); break;
            case Mode::relative: relative_begin(); break;
            case Mode::delayed:
                // A priced fill halfway down a leg, rather than at Open or
                // an extreme, takes the non-first-open COOF delay branch.
                adapter_.entry("seed", true, 99.5, na, 1.0);
                adapter_.flush_pending_entries();
                ROW_CHECK(row_.id, working("seed") == 1); break;
            case Mode::children: children(); break;
            case Mode::buy_replacement: queue_seed("seed", false); break;
            case Mode::close_sites:
                queue_seed("seed-A");
                queue_seed("seed-B");
                break;
            }
        }
        if (bar == 1 && !observed_) {
            phase = "following broker point";
            switch (row_.mode) {
            case Mode::terminal:
                ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", fills_ == (control_ ? 1U : 2U));
                observed_ = true; break;
            case Mode::marketable:
                ROW_CHECK("pineexecutionadapter_fill_pooc_close_entries_marketable", fills_ == (control_ ? 0U : 1U));
                observed_ = true; break;
            case Mode::pair:
                if (!control_) ROW_CHECK("pineexecutionadapter_fill_pooc_close_pair_legs", fills_ == 2);
                observed_ = true; break;
            case Mode::close_empty: transfer(false); break;
            case Mode::fifo: fifo(); break;
            case Mode::newborns: newborns(); break;
            case Mode::close_all: transfer(true); break;
            case Mode::cap_add:
                if (!cap_added_) cap_add_begin();
                break;
            case Mode::rearm: rearm(); break;
            case Mode::bracket: bracket_end(); break;
            case Mode::relative: relative_end(); break;
            case Mode::buy_replacement: buy_replacement(); break;
            case Mode::sell_replacement: sell_replacement(); break;
            case Mode::close_sites: close_sites(); break;
            default: break;
            }
        }
        if (bar == 2 && row_.mode == Mode::cap_add && !observed_) cap_add_end();
    }

protected:
    void on_native_applied_event(const no::ExecutionAppliedEvent& event,
                                 const pf::NativeDecisionContext&) override {
        ++fills_;
        if (event.opened_units != 0.0 && event.definition
            && (event.definition->request.label == "seed"
                || event.definition->request.label == "seed-A"
                || event.definition->request.label == "seed-B")) ++seed_fills_;
    }

private:
    const pf::NativeDecisionContext* context() const {
        const auto* point = sd::intent_live_point(*this);
        ROW_CHECK(row_.id, point != nullptr);
        return point ? &point->decision : nullptr;
    }
    const Snapshot* live(const char* id, Family family) const {
        const auto& placement = adapter_.*access(Placements{});
        for (const auto& handle : adapter_.*access(Live{})) {
            const auto it = placement.find(handle.incarnation);
            if (it != placement.end() && it->second.source_id == id
                && it->second.family == family) return &it->second;
        }
        return nullptr;
    }
    struct WorkingRead {
        const char* id;
        std::size_t count = 0;
        std::uint64_t priority = 0, incarnation = 0;
    };
    WorkingRead read_working(const char* id) const {
        WorkingRead result{id};
        const auto status = visit_native_working_requests(&result,
            [](void* opaque, const pf::NativeWorkingRequest& request) {
                auto& read = *static_cast<WorkingRead*>(opaque);
                if (request.definition && request.definition->request.label == read.id) {
                    ++read.count;
                    read.priority = request.definition->priority ? request.definition->priority : request.definition->handle.incarnation;
                    read.incarnation = request.definition->handle.incarnation;
                }
                return true;
            });
        ROW_CHECK(row_.id, status == pf::detail::ScratchStatus::ok);
        return result;
    }
    std::size_t working(const char* id) const { return read_working(id).count; }
    std::uint64_t priority(const char* id) const { return read_working(id).priority; }
    void queue_seed(const char* id = "seed", bool buy = true, double qty = 1.0) {
        // The next broker point fills this through the matcher. Its Applied
        // notification and Pine cohort/ledger updates finish BEFORE the next
        // normal source callback. Executing a seed inside that callback
        // would leave those notifications queued until the callback returned.
        adapter_.entry(id, buy, na, na, qty);
        adapter_.flush_pending_entries();
        ROW_CHECK(row_.id, working(id) == 1);
    }
    void terminal() {
        const auto* point = context();
        if (!point) return;
        adapter_.entry("buy", true, na, na, 1.0);
        if (!control_) adapter_.entry("sell", false, na, na, 1.0);
        ROW_CHECK(row_.id, working("buy") == 1);
        ROW_CHECK(row_.id, working("sell") == (control_ ? 0U : 1U));
        const auto old_buy = read_working("buy").incarnation;
        const auto old_sell = read_working("sell").incarnation;
        // Use the existing submit-only overload (also used by selected
        // replay). The normal post-script policy/matcher supplies the fills;
        // this avoids forcing a second execution of already-filled rows
        // before their deferred Applied notifications have been delivered.
        (adapter_.*access(Terminal{}))(*point, false);
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", physical_position().signed_units == 0.0);
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", working("buy") == 1);
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", working("sell") == (control_ ? 0U : 1U));
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", control_ ? read_working("buy").incarnation == old_buy
                                                : read_working("buy").incarnation > old_buy);
        if (!control_) ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_terminal_review_handles", read_working("sell").incarnation > old_sell);
        // Two unit targets in source order give two ordinary Applied
        // notifications after the normal close pass; checked on bar 1.
    }
    void marketable() {
        const auto* point = context();
        if (!point) return;
        // A buy limit at 101 reaches the close of 100; at 50 it does not.
        adapter_.entry("buy", true, control_ ? 50.0 : 101.0, na, 1.0);
        ROW_CHECK(row_.id, working("buy") == 1);
        (adapter_.*access(FillEntries{}))(100.0, *point, false);
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_entries_marketable", physical_position().signed_units == (control_ ? 0.0 : 1.0));
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_entries_marketable", working("buy") == (control_ ? 1U : 0U));
    }
    void pair() {
        const auto* point = context();
        if (!point) return;
        adapter_.entry("buy", true, na, na, 1.0);
        adapter_.entry("sell", false, na, 100.0, 1.0);
        ROW_CHECK(row_.id, working("buy") == 1 && working("sell") == 1);
        const bool filled = (adapter_.*access(FillPair{}))(100.0, *point);
        // Market buy first: +1. Later sell stop transacts -(1+1): net -1.
        // The pyramiding=1 control returns at the guard before Leg exists.
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_pair_legs", filled == !control_);
        ROW_CHECK("pineexecutionadapter_fill_pooc_close_pair_legs", physical_position().signed_units == (control_ ? 0.0 : -1.0));
    }
    void order() {
        adapter_.entry("sell", false, na, na, 1.0);
        adapter_.entry("buy", true, na, na, 1.0);
        ROW_CHECK(row_.id, working("sell") == 1 && working("buy") == 1);
        ROW_CHECK(row_.id, priority("sell") < priority("buy"));
        (adapter_.*access(OrderEntries{}))();
        // Source rank puts a buy market before a sell market; ANY excludes
        // the scope. Changed native priority independently witnesses the pass.
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", control_ ? priority("sell") < priority("buy")
                                               : priority("buy") < priority("sell"));
        const auto before = order_close_heap::sample();
        (adapter_.*access(OrderEntries{}))(); // Same populated candidates, now ordered.
        const auto after = order_close_heap::sample();
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", after.bytes == before.bytes);
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", after.blocks == before.blocks);
        ROW_CHECK("pineexecutionadapter_order_same_point_entries_candidates", working("sell") == 1 && working("buy") == 1);
        observed_ = true;
    }
    void hash() {
        if (!control_) {
            adapter_.entry("cancelled-parent", true, 1.0, na, 1.0);
            ROW_CHECK(row_.id, working("cancelled-parent") == 1);
            adapter_.cancel("cancelled-parent");
            ROW_CHECK(row_.id, working("cancelled-parent") == 0);
        }
        const auto& tokens = adapter_.*access(Cancels{});
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", tokens.size() == (control_ ? 0U : 1U));
        if (!tokens.empty()) ROW_CHECK(row_.id, tokens.begin()->second.entry_incarnation != 0);
        pf::BrokerStateHashSink warm;
        adapter_.hash_state(warm); // Initialize read caches before the return-equality window.
        pf::BrokerStateHashSink actual;
        const auto before = order_close_heap::sample();
        adapter_.hash_state(actual);
        const auto after = order_close_heap::sample();
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", after.bytes == before.bytes);
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", after.blocks == before.blocks);
        ROW_CHECK("pineexecutionadapter_hash_state_named_cancel_keys", actual.h == warm.h);
        observed_ = true;
    }
    void transfer(bool all) {
        ROW_CHECK(row_.id, physical_position().signed_units == 1.0 && seed_fills_ == 1);
        // Both variants submit through the actual entry door and flush its
        // priced-reversal staging. Only the unpriced same-bar one qualifies.
        adapter_.entry("opposite", false, control_ ? 200.0 : na, na, 1.0);
        adapter_.flush_pending_entries();
        ROW_CHECK(row_.id, working("opposite") == 1);
        if (all) adapter_.close_all(); else adapter_.close("");
        const auto& pending = adapter_.*access(Entries{});
        ROW_CHECK(row_.id, pending.size() == (control_ ? 0U : 1U));
        ROW_CHECK(row_.id, working("opposite") == (control_ ? 1U : 0U));
        ROW_CHECK(row_.id, working(all ? "__close__" : "__pine_close_all") == 1);
        if (!control_ && pending.size() == 1) {
            ROW_CHECK(row_.id, pending.front().snapshot.source_id == "opposite");
            ROW_CHECK(row_.id, pending.front().snapshot.paired_reversal_parent.incarnation != 0);
        }
        observed_ = true;
    }
    void fifo() {
        ROW_CHECK(row_.id, seed_fills_ == (control_ ? 1U : 2U));
        ROW_CHECK(row_.id, physical_position().lot_count == (control_ ? 1U : 2U));
        adapter_.close("seed-A");
        ROW_CHECK(row_.id, working("__close__seed-A") == 1);
        if (!control_) adapter_.close("seed-B");
        // The second default-FIFO call scans the first current-bar close;
        // neither executes at this callback. One submitted close per id.
        ROW_CHECK("pineexecutionadapter_close_preceding_fifo_closes", working("__close__seed-B") == (control_ ? 0U : 1U));
        ROW_CHECK("pineexecutionadapter_close_preceding_fifo_closes", physical_position().signed_units == (control_ ? 1.0 : 3.0));
        const auto* first = live("seed-A", Family::Close);
        const auto* second = live("seed-B", Family::Close);
        ROW_CHECK(row_.id, first && first->from_entry.empty() && !first->immediately);
        if (!control_) ROW_CHECK(row_.id, second && second->from_entry.empty() && !second->immediately);
        observed_ = true;
    }
    void newborns() {
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == 1.0);
        adapter_.entry("newborn", true, 50.0, na, 1.0);
        adapter_.flush_pending_entries();
        ROW_CHECK(row_.id, working("newborn") == 1);
        adapter_.close_all();
        // The COOF configuration cancels even a priced newborn. With COOF
        // off it stays live; it cannot fill on this tape whose low is 99.
        ROW_CHECK("pineexecutionadapter_close_all_newborns", working("newborn") == (control_ ? 1U : 0U));
        ROW_CHECK("pineexecutionadapter_close_all_newborns", working("__close__") == 1);
        ROW_CHECK("pineexecutionadapter_close_all_newborns", physical_position().signed_units == 1.0);
        observed_ = true;
    }
    void cap_add_begin() {
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == 1.0);
        adapter_.entry("cap-add", true, 50.0, na, 1.0);
        ROW_CHECK(row_.id, working("cap-add") == 1);
        const auto* add = live("cap-add", Family::Entry);
        ROW_CHECK(row_.id, add && add->projection_over_pyramiding == !control_);
        cap_added_ = true;
    }
    void cap_add_end() {
        // The preceding broker-open pass, before this source callback,
        // withdraws the prior bar's unplaced add iff the single lot holds
        // pyramiding=1. Pyramiding=2 is the below-cap control.
        ROW_CHECK("pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", seed_fills_ == 1 && physical_position().lot_count == 1);
        ROW_CHECK("pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", working("cap-add") == (control_ ? 1U : 0U));
        ROW_CHECK("pineexecutionadapter_withdraw_unplaced_cap_adds_unplaced", (live("cap-add", Family::Entry) != nullptr) == control_);
        observed_ = true;
    }
    void rearm() {
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == 1.0);
        adapter_.entry("parent", true, 1.0, na, 1.0);
        adapter_.exit("exit", "parent", 1000.0, na);
        adapter_.flush_pending_bracket_legs();
        const auto* bound = live("exit", Family::ExitLimit);
        ROW_CHECK(row_.id, bound && bound->pending_bound_exit);
        adapter_.cancel("parent");
        ROW_CHECK(row_.id, (adapter_.*access(Bindings{})).size() == 1);
        ROW_CHECK(row_.id, working("exit") == 0);
        // Recreating another id cannot select this stashed definition.
        adapter_.entry(control_ ? "other-parent" : "parent", true, 1.0, na, 1.0);
        adapter_.flush_pending_bracket_legs();
        ROW_CHECK("pineexecutionadapter_rearm_id_bound_exits_rearm", (adapter_.*access(Bindings{})).size() == (control_ ? 1U : 0U));
        ROW_CHECK("pineexecutionadapter_rearm_id_bound_exits_rearm", working("exit") == (control_ ? 0U : 1U));
        if (!control_) {
            const auto* rearmed = live("exit", Family::ExitLimit);
            ROW_CHECK(row_.id, rearmed && rearmed->pending_bound_exit && rearmed->from_entry == "parent");
        }
        observed_ = true;
    }
    void bracket_begin() {
        // A full global absolute bracket beside a PRICED pending entry
        // remains staged while flat. The real fill selects its origin-zero
        // legs in materialize_pending_bracket_legs, not in a fixture copy.
        adapter_.entry("seed", true, control_ ? 1.0 : 99.5, na, 1.0);
        adapter_.flush_pending_entries();
        adapter_.exit("exit", "", 1000.0, 50.0);
        ROW_CHECK(row_.id, (adapter_.*access(Brackets{})).size() == 2);
        ROW_CHECK(row_.id, working("seed") == 1 && working("exit") == 0);
    }
    void bracket_end() {
        // One absolute limit plus one stop is two legs, independently of
        // vector growth policy. The unreached-parent control retains both
        // staged definitions and produces no parent fill.
        ROW_CHECK("pineexecutionadapter_materialize_pending_bracket_legs_ready", seed_fills_ == (control_ ? 0U : 1U));
        ROW_CHECK("pineexecutionadapter_materialize_pending_bracket_legs_ready", (adapter_.*access(Brackets{})).size() == (control_ ? 2U : 0U));
        ROW_CHECK("pineexecutionadapter_materialize_pending_bracket_legs_ready", working("exit") == (control_ ? 0U : 2U));
        if (!control_) {
            ROW_CHECK(row_.id, live("exit", Family::ExitLimit) != nullptr);
            ROW_CHECK(row_.id, live("exit", Family::ExitStop) != nullptr);
        }
        observed_ = true;
    }
    void flags() {
        adapter_.entry("A", true, na, na, 1.0);
        adapter_.entry("B", false, na, na, 1.0);
        adapter_.entry("C", true, na, na, 1.0);
        // Three commands cannot be the exactly-two explicit pair. At
        // pyramiding=2 this is the flat_pair_member(3,false) branch;
        // pyramiding=3 submits directly and has no such queued batch.
        ROW_CHECK(row_.id, (adapter_.*access(SameBar{})).size() == (control_ ? 0U : 3U));
        adapter_.flush_pending_entries();
        ROW_CHECK("pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", (adapter_.*access(SameBar{})).empty());
        ROW_CHECK("pineexecutionadapter_flush_pending_same_bar_commands_flat_pair_member", working("A") + working("B") + working("C") == 3);
        if (!control_) {
            const auto* a = live("A", Family::Entry);
            const auto* b = live("B", Family::Entry);
            const auto* c = live("C", Family::Entry);
            ROW_CHECK(row_.id, a && b && c && a->reverse_to && b->reverse_to && c->reverse_to);
        }
        observed_ = true;
    }
    void relative_begin() {
        adapter_.entry("seed", true, na, na, 1.0);
        adapter_.flush_pending_entries();
        // Entry fill is 100, tick is 1/4, profit is 400 ticks: 100+400/4=200.
        // The control already supplies the absolute 200 and needs no local
        // relative definition when that same parent fills.
        adapter_.exit("exit", "seed", control_ ? 200.0 : na, na,
                      na, na, na, 100.0, {}, na, {}, control_ ? na : 400.0, na);
        ROW_CHECK(row_.id, (adapter_.*access(Relative{})).size() == (control_ ? 0U : 1U));
        ROW_CHECK(row_.id, working("seed") == 1);
    }
    void relative_end() {
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", seed_fills_ == 1);
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", (adapter_.*access(Relative{})).empty());
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", working("exit") == 1);
        const auto* exit = live("exit", Family::ExitLimit);
        ROW_CHECK("pineexecutionadapter_materialize_relative_exits_pending", exit && exit->exit_levels.limit == 100.0 + 400.0 / 4.0);
        observed_ = true;
    }
    void deferred_callback() {
        coof_done_ = true;
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == 1.0);
        const auto* point = context();
        if (!point) return;
        ROW_CHECK(row_.id, point->coordinate.path_phase != pf::NativePathPhase::Open);
        adapter_.exit("exit", "seed", na, control_ ? 50.0 : 110.0);
        auto& delayed = adapter_.*access(Delayed{});
        ROW_CHECK(row_.id, delayed.size() == (control_ ? 0U : 1U));
        if (!control_ && delayed.size() == 1) {
            ROW_CHECK(row_.id, delayed.front().snapshot.family == Family::ExitStop);
            ROW_CHECK(row_.id, delayed.front().snapshot.from_entry == "seed");
            ROW_CHECK(row_.id, !delayed.front().snapshot.fixed_exit_reservation);
        }
        (adapter_.*access(Reconcile{}))("seed", std::abs(physical_position().signed_units));
        ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", delayed.size() == (control_ ? 0U : 1U));
        if (!control_ && delayed.size() == 1) {
            // Reconcile's delayed-index loop changes fixed=false to true:
            // this is independent evidence that Family::delayed was filled
            // and its index was used. One full exit reserves 1*100/100 = 1.
            ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", delayed.front().snapshot.fixed_exit_reservation);
            ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", delayed.front().snapshot.projection_remaining_qty == 1.0);
            ROW_CHECK("pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed", delayed.front().snapshot.qty_percent == 100.0);
        }
        observed_ = true;
    }
    void children() {
        adapter_.exit("exit", "parent", 1000.0, 50.0);
        adapter_.flush_pending_bracket_legs();
        ROW_CHECK(row_.id, working("exit") == 2);
        ROW_CHECK(row_.id, physical_position().signed_units == 0.0);
        // A default-sized POOC stop alone calls stage_flat_children_before_parent.
        // The explicit unit control takes the same entry door without that call.
        adapter_.entry("parent", true, na, 105.0, control_ ? 1.0 : na);
        const auto& staged = adapter_.*access(Brackets{});
        ROW_CHECK("pineexecutionadapter_stage_flat_children_before_parent_children", staged.size() == (control_ ? 0U : 2U));
        ROW_CHECK("pineexecutionadapter_stage_flat_children_before_parent_children", working("exit") == (control_ ? 2U : 0U));
        ROW_CHECK("pineexecutionadapter_stage_flat_children_before_parent_children", working("parent") == 1);
        for (const auto& leg : staged) {
            ROW_CHECK(row_.id, leg.snapshot.from_entry == "parent");
            ROW_CHECK(row_.id, leg.snapshot.defer_until_post_parent_calculation);
        }
        observed_ = true;
    }
    void buy_replacement() {
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == -1.0);
        adapter_.entry("buy-A", true); // Market predecessor.
        adapter_.entry("buy-A", true); // Real same-id market replacement.
        const auto* prior = live("buy-A", Family::Entry);
        ROW_CHECK(row_.id, prior && prior->replaced_opening && prior->replacement_predecessor_market);
        ROW_CHECK(row_.id, working("buy-A") == 1);
        // Only the default-sized long reversal collects buy-A. Explicit
        // quantity is the control; both variants create the new buy-B.
        adapter_.entry("buy-B", true, na, na, control_ ? 1.0 : na);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_buy_replacements", working("buy-A") == (control_ ? 1U : 0U));
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_buy_replacements", working("buy-B") == 1);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_buy_replacements", seed_fills_ == 1 && physical_position().signed_units == -1.0);
        observed_ = true;
    }
    void sell_replacement() {
        ROW_CHECK(row_.id, seed_fills_ == 1 && physical_position().signed_units == 1.0);
        adapter_.entry("sell-sibling", false);
        adapter_.entry("sell-main", false);
        ROW_CHECK(row_.id, working("sell-sibling") == 1 && working("sell-main") == 1);
        const auto* prior = live("sell-main", Family::Entry);
        ROW_CHECK(row_.id, prior && !prior->replaced_opening);
        adapter_.entry("sell-main", false, na, na, control_ ? 1.0 : na);
        // The real market replacement sets both flags. Its default-sized
        // sell-reversal branch retires the other id's same-bar short entry.
        const auto* replacement = live("sell-main", Family::Entry);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", replacement && replacement->replaced_opening
                  && replacement->replacement_predecessor_market);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", working("sell-sibling") == (control_ ? 1U : 0U));
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", working("sell-main") == 1);
        ROW_CHECK("pineexecutionadapter_place_entry_superseded_siblings", seed_fills_ == 1 && physical_position().signed_units == 1.0);
        observed_ = true;
    }
    void close_sites() {
        ROW_CHECK(row_.id, seed_fills_ == 2 && physical_position().signed_units == 2.0);
        if (!control_) {
            adapter_.close("seed-A", {}, na, na, false, 101);
            adapter_.close("seed-B", {}, na, na, false, 102);
        } else {
            adapter_.close("absent", {}, na, na, false, 101);
        }
        ROW_CHECK(row_.id, (adapter_.*access(CloseSites{})).size() == (control_ ? 0U : 2U));
        adapter_.flush_pending_closes();
        // Two independently held unit ids and two distinct callsites yield
        // two copied active sites, two submitted close handles, and no batch.
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", (adapter_.*access(CloseSites{})).empty());
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", working("__close__seed-A") == (control_ ? 0U : 1U));
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", working("__close__seed-B") == (control_ ? 0U : 1U));
        ROW_CHECK("pineexecutionadapter_flush_pending_closes_sites", seed_fills_ == 2 && physical_position().signed_units == 2.0);
        observed_ = true;
    }

    const Row& row_;
    bool control_;
    bool started_ = false, observed_ = false, coof_done_ = false, cap_added_ = false;
    unsigned fills_ = 0, seed_fills_ = 0;
};

void run_cycle(Host& host, const Row& row, const std::array<pf::Bar, 4>& tape) {
    try {
        host.cycle(tape);
    } catch (const std::exception& error) {
        ROW_CHECK(row.id, false);
        std::fprintf(stderr, "DETAIL [%s] exception: %s\n", row.id, error.what());
    } catch (...) {
        ROW_CHECK(row.id, false);
    }
    // A check does not throw, and an exception ends only this cycle. The
    // dispatcher continues the remaining cycles, controls and member rows.
}

void run_row(const Row& row) {
    const std::array<pf::Bar, 4> tape{{
        {100, 101, 99, 100, 1, start_ms},
        {100, 101, 99, 100, 1, start_ms + 60000},
        {100, 101, 99, 100, 1, start_ms + 120000},
        {100, 101, 99, 100, 1, start_ms + 180000}}};
    for (const bool control : {true, false}) {
        is_control = control;
        Host host(row, control);
        cycle_number = 0;
        run_cycle(host, row, tape); // Seed the retained host's reusable storage.
        // The baseline must include a reuse cycle, not only a fresh-host run.
        // PineExecutionAdapter::close("") gives a placement the owning
        // "__pine_close_all" source_id. PlacementTable::erase recycles whole
        // nodes, overwrite assigns their strings, and clear() leaves spare_
        // intact. Reusing a long-id node for a short id can keep its string
        // capacity while another node receives the long id (pineexecutionadapter_close_opposite_entries).
        // In its priced control, seed and close retire into spare_, while
        // opposite stays live. The next run gives the spare close node to
        // seed; the following run can give that long-capacity seed node to
        // opposite. Warm the fresh run and BOTH reuse transitions so these
        // three simultaneous placement roles have seen the long id.
        // Separately, release_closed_cohort_origins fills next_reissues and
        // swaps it with reissues. reset_for_run clears reissues without releasing
        // either backing buffer. A fresh run can warm only one of the pair
        // (pineexecutionadapter_rearm_id_bound_exits_rearm, pineexecutionadapter_materialize_relative_exits_pending's absolute control, pineexecutionadapter_reconcile_deferred_exit_reservations_family_delayed).
        // Run the SAME script twice more to exercise reuse before the
        // reference sample. These are fixed warm-up cycles, never a loop
        // that waits for equality: all subsequent byte/block equalities,
        // 64 measured cycles, N/4N endpoints and branch checks stay intact.
        run_cycle(host, row, tape);
        run_cycle(host, row, tape);
        const auto first = order_close_heap::sample();
        Sample at_n{}, at_4n{};
        for (cycle_number = 1; cycle_number <= 64; ++cycle_number) {
            run_cycle(host, row, tape);
            const auto current = order_close_heap::sample();
            phase = "completed cycle: same retained host and script";
            released(row, first, current);
            if (cycle_number == 16) at_n = current;
            if (cycle_number == 64) at_4n = current;
        }
        phase = "N=16 and 4N=64 completed command cycles";
        released(row, first, at_n);
        released(row, first, at_4n);
        released(row, at_n, at_4n);
        std::printf("MEMORY [%s] control=%d first=%zu/%zu N=%zu/%zu 4N=%zu/%zu bytes/blocks\n",
                    row.id, int(control), first.bytes, first.blocks,
                    at_n.bytes, at_n.blocks, at_4n.bytes, at_4n.blocks);
    }
}
} // namespace

int main() {
    order_close_heap::enabled = true;
    allocator_control();
    for (const auto& row : rows) {
        try {
            run_row(row);
        } catch (const std::exception& error) {
            ROW_CHECK(row.id, false);
            std::fprintf(stderr, "DETAIL [%s] fixture exception: %s\n", row.id, error.what());
        } catch (...) {
            ROW_CHECK(row.id, false);
        }
    }
    std::printf("memory proof release_orders_closes: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
