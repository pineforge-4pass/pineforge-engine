// Command storage is checked at call, run and host lifetime boundaries.
// No byte pins: the oracles compare live requested bytes AND live blocks.
// A marked allocation remains charged until its actual delete, even outside
// the call window. Cycle fixtures count the retained host's whole lifetime and
// sample every run before destruction. Commands retaining accepted-order state
// also have a separate fresh-host destruction check, not a call-return equality.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_host_reads.hpp"

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
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace command_heap {
// Only the thread opening a Window marks allocations. Releases of marked
// blocks count on ANY thread; atomics also make such a release race-free.
// There is no allocating registry, reset, size filter, or row-specific hook.
thread_local bool recording = false;
std::atomic<std::size_t> live_bytes{0}, live_blocks{0}, allocations{0};
struct Header { void* base; std::size_t bytes; bool marked; };
struct Sample {
    std::size_t bytes, blocks, calls;
};
Sample sample() noexcept {
    return {live_bytes.load(), live_blocks.load(), allocations.load()};
}
bool same_live(Sample a, Sample b) noexcept {
    return a.bytes == b.bytes && a.blocks == b.blocks;
}
struct Window {
    bool previous = recording;
    Window() noexcept { recording = true; }
    ~Window() { recording = previous; }
};
void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
    if (bytes == 0) bytes = 1;
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (alignment - 1 > maximum - sizeof(Header)) return nullptr;
    const std::size_t overhead = sizeof(Header) + alignment - 1;
    if (bytes > maximum - overhead) return nullptr;
    void* base = std::malloc(bytes + overhead);
    if (!base) return nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto address = (start + alignment - 1) & ~(alignment - 1);
    auto* block = reinterpret_cast<unsigned char*>(address);
    const Header header{base, bytes, recording};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    if (header.marked) {
        live_bytes.fetch_add(bytes);
        live_blocks.fetch_add(1);
        allocations.fetch_add(1);
    }
    return block;
}
void release(void* value) noexcept {
    if (!value) return;
    auto* block = static_cast<unsigned char*>(value);
    Header header{};
    std::memcpy(&header, block - sizeof(Header), sizeof header);
    if (header.marked) {
        live_bytes.fetch_sub(header.bytes);
        live_blocks.fetch_sub(1);
    }
    std::free(header.base);
}
void* throwing(std::size_t bytes, std::size_t alignment) {
    for (;;) {
        if (void* value = allocate(bytes, alignment)) return value;
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
void* nonthrowing(std::size_t bytes, std::size_t alignment) noexcept {
    try { return throwing(bytes, alignment); } catch (...) { return nullptr; }
}
} // namespace command_heap

// All replaceable C++17 forms share the header and malloc/free pair, including
// the nothrow/sized forms used by standard-library temporary buffers.
void* operator new(std::size_t n) { return command_heap::throwing(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return command_heap::throwing(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return command_heap::nonthrowing(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return command_heap::nonthrowing(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t a) {
    return command_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return command_heap::throwing(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return command_heap::nonthrowing(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return command_heap::nonthrowing(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { command_heap::release(p); }
void operator delete[](void* p) noexcept { command_heap::release(p); }
void operator delete(void* p, std::size_t) noexcept { command_heap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { command_heap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { command_heap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { command_heap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { command_heap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { command_heap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { command_heap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { command_heap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { command_heap::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { command_heap::release(p); }

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Family = source::PineOrderFamily;
using Handle = no::RequestHandle;
using command_heap::sample;
using command_heap::same_live;
using command_heap::Window;

// The established explicit-instantiation seam changes no product definition.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define ADAPTER_ACCESS(Tag, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Adapter::member>
ADAPTER_ACCESS(Placements, placement_);
ADAPTER_ACCESS(Live, live_handles_);
ADAPTER_ACCESS(DroppedCount, dropped_close_count_);
ADAPTER_ACCESS(DroppedDigest, dropped_close_digest_);
ADAPTER_ACCESS(Delayed, delayed_market_orders_);
ADAPTER_ACCESS(AdmitOpen, apply_open_market_admission);
ADAPTER_ACCESS(DeferDeclined, defer_declined_reversal_exits_at_adverse);
ADAPTER_ACCESS(ImmediateOn, immediate_close_placed_on);
ADAPTER_ACCESS(FlushSameBar, flush_pending_same_bar_commands);
#undef ADAPTER_ACCESS
struct Spares { friend auto access(Spares); };
template struct Access<Spares, &source::PlacementTable::spare_>;
struct Validate { friend auto access(Validate); };
template struct Access<Validate, &no::WorkingRequestCore::validate_request>;
struct Terminal { friend auto access(Terminal); };
template struct Access<Terminal,
    static_cast<void (Adapter::*)(const NativeDecisionContext&, bool)>(
        &Adapter::apply_terminal_explicit_market_policy)>;

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL, kMinute = 60000;
int checks = 0, failures = 0, trial_bars = 0, trial_commands = 0, trial_cycle = 0;
void check(const char* row, const char* explanation, const char* expression,
           bool passed, int line) {
    ++checks;
    if (passed) return;
    ++failures;
    const auto held = sample();
    std::fprintf(stderr,
        "FAIL [%s] %s:%d: %s -- %s (bars=%d commands=%d cycle=%d live=%zu/%zu)\n",
        row, __FILE__, line, expression, explanation, trial_bars,
        trial_commands, trial_cycle, held.bytes, held.blocks);
}
#define ROW_CHECK(row, explanation, ...) \
    check(row, explanation, #__VA_ARGS__, (__VA_ARGS__), __LINE__)

std::vector<Bar> tape(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int b = 0; b < n; ++b)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + b * kMinute});
    return bars;
}
std::string key(const char* stem, int i) {
    return std::string(stem) + "/" + std::to_string(i);
}

class Host : public source::PineStrategyHost {
public:
    Host(int pyramiding = 8, bool pooc = false, bool percent = false) {
        source::PineStrategyConfig c;
        c.initial_capital = percent ? 1000.0 : 1000000.0;
        c.default_qty_type = static_cast<int>(percent ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        c.default_qty_value = percent ? 100.0 : 1.0;
        c.pyramiding = pyramiding;
        c.process_orders_on_close = pooc;
        c.margin_long = c.margin_short = percent ? 100.0 : 0.0;
        c.commission_value = 0.0;
        configure_pine_strategy(c);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_margin_call_enabled(false);
    }
    Adapter& adapter() { return adapter_; }
    auto& placements() { return adapter_.*access(Placements{}); }
    const auto& live() { return adapter_.*access(Live{}); }
    auto& consumer() { return NativeExecutionConsumer::bound(*this); }
    void flush() {
        // flush_pending_same_bar_commands defaults to false, true.
        // Member pointers carry no default arguments; preserve both explicitly.
        (adapter_.*access(FlushSameBar{}))(false, true);
        adapter_.flush_pending_entries();
    }
    Handle one_live(const char* row, Family family, const std::string& id) {
        Handle result;
        std::size_t count = 0;
        for (const auto& h : live()) {
            const auto found = placements().find(h.incarnation);
            if (found != placements().end() && found->second.family == family
                && found->second.source_id == id) {
                result = h;
                ++count;
            }
        }
        ROW_CHECK(row, "one real submitted request has this family and source id", count == 1);
        return result;
    }
    void run_tape(const std::vector<Bar>& bars, const char* row) {
        run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
        ROW_CHECK(row, "engine run completed", last_error().empty());
    }
};

// Receipt strings are longer than either common library's small-string buffer.
// The short warm-up exercises the same dropped-close path without retaining
// backing when dropped receipts are retained. No measured text size or heap count sets a limit.
class ReceiptHost final : public Host {
public:
    explicit ReceiptHost(int commands) : commands_(commands), id_(257, 'i'), comment_(769, 'c') {}
    void on_source_bar(const Bar&) override {
        ++callbacks;
        strategy_close("warm", "", 1.0, kNa, false, 1);
        const auto start_count = adapter().*access(DroppedCount{});
        const auto anchor = sample();
        for (int i = 0; i < commands_; ++i) {
            const auto before = sample();
            const auto digest = adapter().*access(DroppedDigest{});
            {
                Window window;
                strategy_close(id_, comment_, 1.0 + i, 25.0, (i % 2) != 0,
                               static_cast<std::uint64_t>(i + 2));
            }
            const auto after = sample();
            ROW_CHECK("droppedclosereceipt_source_id", "source_id copy is released when the real close returns",
                      same_live(before, after) && same_live(anchor, after));
            ROW_CHECK("droppedclosereceipt_comment", "comment copy is released when the real close returns",
                      same_live(before, after) && same_live(anchor, after));
            ROW_CHECK("droppedclosereceipt_transient_command_result", "the folded receipt leaves no live allocation",
                      same_live(before, after) && same_live(anchor, after));
            ROW_CHECK("droppedclosereceipt_transient_command_result", "each real empty-cohort close folded one receipt",
                      (adapter().*access(DroppedCount{})) == start_count + i + 1
                          && (adapter().*access(DroppedDigest{})) != digest);
            ROW_CHECK("droppedclosereceipt_source_id", "long source text really took the allocating receipt path",
                      after.calls > before.calls);
            ROW_CHECK("droppedclosereceipt_comment", "the same receipt includes the nonempty long comment",
                      after.calls > before.calls);
        }
        const auto count = adapter().*access(DroppedCount{});
        strategy_close("", "", kNa, kNa, false);
        ROW_CHECK("droppedclosereceipt_transient_command_result", "flat empty-id control creates no dropped receipt",
                  (adapter().*access(DroppedCount{})) == count);
        ++windows;
    }
    int callbacks = 0, windows = 0;
private:
    int commands_;
    const std::string id_, comment_;
};

void receipts() {
    for (int commands : {16, 64}) {
        trial_commands = commands;
        std::array<command_heap::Sample, 2> ends{};
        int leg = 0;
        for (int bars : {16, 64}) {
            trial_bars = bars;
            ReceiptHost host(commands);
            const auto input = tape(bars);
            host.run_tape(input, "droppedclosereceipt_transient_command_result");
            ROW_CHECK("droppedclosereceipt_transient_command_result", "every input bar exercised all command calls",
                      host.callbacks == bars && host.windows == bars);
            // One short warm-up plus commands long receipts on each bar.
            ROW_CHECK("droppedclosereceipt_transient_command_result", "receipt count equals the independent command arithmetic",
                      (host.adapter().*access(DroppedCount{}))
                          == static_cast<std::uint64_t>(bars * (commands + 1)));
            ends[leg++] = sample();
        }
        ROW_CHECK("droppedclosereceipt_source_id", "no marked source_id backing remains at N or 4N bars",
                  same_live(ends[0], ends[1]));
        ROW_CHECK("droppedclosereceipt_comment", "no marked comment backing remains at N or 4N bars",
                  same_live(ends[0], ends[1]));
        ROW_CHECK("droppedclosereceipt_transient_command_result", "receipt release is fixed as bars grow fourfold",
                  same_live(ends[0], ends[1]));
    }
}

// Real strategy.entry fills supply the handles and physical observations to
// the production validation routine; an actual submit/fill follows the probes.
class CanonicalHost final : public Host {
public:
    explicit CanonicalHost(int commands) : commands_(commands) {}
    void on_source_bar(const Bar&) override {
        const char* row = "workingrequestcore_validate_request_canonical";
        ++callbacks;
        if (pine_bar_index() == 0) {
            for (int i = 0; i < 4; ++i)
                strategy_entry(key("cmds-inline", i), true, kNa, kNa, 1.0);
        }
        if (pine_bar_index() == 1) {
            const auto lots = native_open_lots(kNa);
            ROW_CHECK(row, "four submitted one-unit entries actually filled", lots.size() == 4);
            if (lots.size() != 4) return;
            const auto& core = consumer().request_core();
            no::CommandContext context;
            context.decision_time_ms = kStart + kMinute;
            no::BindOpenings binding;
            binding.cycle = lots.front().cycle;
            for (const auto& lot : lots) {
                Handle h{core.identity(), lot.entry_incarnation};
                binding.openings.push_back(h);
                context.openings.push_back({h, lot.cycle,
                    no::PositionNonflat{lot.cycle, lot.side}, true});
            }
            std::reverse(binding.openings.begin(), binding.openings.end());
            no::Request request{no::Reduce{no::ExplicitUnits{4.0}}, "cmds-bound-close", ""};
            request.owner = std::move(binding);
            const std::optional<Handle> no_replacement;
            const auto anchor = sample();
            for (int i = 0; i < commands_; ++i) {
                const auto before = sample();
                std::optional<no::RequestRejectReason> reason;
                {
                    Window window;
                    reason = (core.*access(Validate{}))(request, context, no_replacement);
                }
                const auto after = sample();
                ROW_CHECK(row, "inline canonical copy and its handle strings return all storage",
                          same_live(before, after) && same_live(anchor, after));
                ROW_CHECK(row, "production validator consumed the four real opening handles",
                          !reason && after.calls > before.calls);
                ++validations;
            }
            no::Request control{no::Reduce{no::ExplicitUnits{1.0}}, "book-control", ""};
            const auto before = sample();
            std::optional<no::RequestRejectReason> reason;
            {
                Window window;
                reason = (core.*access(Validate{}))(control, context, no_replacement);
            }
            ROW_CHECK(row, "independent-owner control bypasses the canonical vector",
                      !reason && same_live(before, sample()) && sample().calls == before.calls);
            const auto accepted = submit(request);
            ROW_CHECK(row, "the probed request also passes the real submit door",
                      accepted.status == no::SubmitStatus::Accepted && accepted.handle.has_value());
        }
        if (pine_bar_index() == 2) {
            ROW_CHECK(row, "the actual bound close filled all four submitted lots",
                      native_open_lots(kNa).empty() && trade_count() == 4);
        }
    }
    int callbacks = 0, validations = 0;
private:
    int commands_;
};

void canonical() {
    for (int commands : {16, 64}) {
        trial_commands = commands;
        std::array<command_heap::Sample, 2> ends{};
        int leg = 0;
        for (int bars : {16, 64}) {
            trial_bars = bars;
            const auto input = tape(bars);
            CanonicalHost host(commands);
            host.run_tape(input, "workingrequestcore_validate_request_canonical");
            ROW_CHECK("workingrequestcore_validate_request_canonical", "both bar lengths executed every validation",
                      host.callbacks == bars && host.validations == commands);
            ends[leg++] = sample();
        }
        ROW_CHECK("workingrequestcore_validate_request_canonical", "no canonical backing survives at N and 4N bars",
                  same_live(ends[0], ends[1]));
    }
}

// One qualifying candidate reaches the real push_back, then the <2 return.
// No policy re-submission or journal mutation occurs on that branch.
class TerminalHost final : public Host {
public:
    explicit TerminalHost(int commands) : Host(0, true), commands_(commands),
        id_(193, 't'), comment_(389, 'm') {}
    void on_source_bar(const Bar&) override {
        const char* row = "pineexecutionadapter_apply_terminal_explicit_market_policy_candidates";
        ++callbacks;
        if (pine_bar_index() == 0) {
            const auto* point = consumer().current_point();
            ROW_CHECK(row, "terminal policy has a real callback context", point != nullptr);
            if (!point) return;
            auto before = sample();
            {
                Window window;
                (adapter().*access(Terminal{}))(point->decision, false);
            }
            ROW_CHECK(row, "empty-book control builds no candidate", same_live(before, sample()));
            strategy_entry(id_, true, kNa, kNa, 1.0, comment_);
            flush();
            const Handle handle = one_live(row, Family::Entry, id_);
            const auto found = placements().find(handle.incarnation);
            ROW_CHECK(row, "one current-bar unpriced fixed entry qualifies for the candidate copy",
                      found != placements().end() && found->second.opening
                          && found->second.requested_qty == 1.0
                          && found->second.projection_created_bar == adapter().projection_bar_index(point->decision)
                          && found->second.source_id == id_ && found->second.comment == comment_
                          && !found->second.birth.from_fill() && !found->second.birth.at_terminal_fill());
            const auto anchor = sample();
            for (int i = 0; i < commands_; ++i) {
                before = sample();
                {
                    Window window;
                    (adapter().*access(Terminal{}))(point->decision, false);
                }
                const auto after = sample();
                ROW_CHECK(row, "single-candidate return releases vector and copied snapshot text",
                          same_live(before, after) && same_live(anchor, after));
                ROW_CHECK(row, "candidate-copy branch allocated but kept the original working handle",
                          after.calls > before.calls && consumer().request_core().find_live(handle)
                              && live().size() == 1);
                ++policy_calls;
            }
        }
        if (pine_bar_index() == 1) {
            const auto lots = native_open_lots(kNa);
            ROW_CHECK(row, "the copied candidate still filled through the real matcher",
                      lots.size() == 1 && lots.front().entry_label == id_ && lots.front().signed_units == 1.0);
            strategy_close_all();
        }
    }
    int callbacks = 0, policy_calls = 0;
private:
    int commands_;
    const std::string id_, comment_;
};

void terminal() {
    for (int commands : {16, 64}) {
        trial_commands = commands;
        std::array<command_heap::Sample, 2> ends{};
        int leg = 0;
        for (int bars : {16, 64}) {
            trial_bars = bars;
            const auto input = tape(bars);
            TerminalHost host(commands);
            host.run_tape(input, "pineexecutionadapter_apply_terminal_explicit_market_policy_candidates");
            ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_candidates", "all calls ran and the one real entry was closed",
                      host.callbacks == bars && host.policy_calls == commands
                          && host.trade_count() == 1 && host.native_open_lots(kNa).empty());
            ends[leg++] = sample();
        }
        ROW_CHECK("pineexecutionadapter_apply_terminal_explicit_market_policy_candidates", "candidate storage stays released when bars grow fourfold",
                  same_live(ends[0], ends[1]));
    }
}

// The lookup index is a cache owner, not a call-local. Public cache adoption
// and release make a repeatable empty-cache -> query -> empty-cache cycle.
// The actual immediate close plus twenty real resting orders crosses the
// source's >16 indexing branch; the no-close bar is a separate query control.
class LookupHost final : public Host {
public:
    explicit LookupHost(int commands) : commands_(commands) {
        for (int i = 0; i < 20; ++i) resting_[i] = key("cmds-cache", i);
    }
    void on_source_bar(const Bar&) override {
        const char* row = "adapterlookupindex_immediate_closes_by_bar";
        ++callbacks;
        if (pine_bar_index() == 0) strategy_entry("cmds-held", true, kNa, kNa, 2.0);
        if (pine_bar_index() == 1) {
            strategy_close("cmds-held", "", 1.0, kNa, true);
            for (const auto& id : resting_) strategy_order(id, true, 1.0, 1.0);
            std::size_t immediate = 0;
            for (const auto& entry : placements())
                if (entry.second.family == Family::Close && entry.second.immediately
                    && entry.second.projection_created_bar == 1) ++immediate;
            ROW_CHECK(row, "real immediate-close placement is in the indexed population",
                      immediate == 1 && placements().size() > 16);
            consumer().set_host_cache(false);
            const auto anchor = sample();
            for (int i = 0; i < commands_; ++i) {
                const auto before = sample();
                bool answer = false, absent = true;
                std::uint64_t answered = 0;
                {
                    Window window;
                    consumer().set_host_cache(true);
                    answer = (adapter().*access(ImmediateOn{}))(1);
                    absent = (adapter().*access(ImmediateOn{}))(-1);
                    auto* cache = dynamic_cast<source::detail::PineRunCache*>(consumer().host_cache());
                    if (cache) answered = cache->answered();
                    consumer().set_host_cache(false);
                }
                const auto after = sample();
                ROW_CHECK(row, "cache-owner release returns all immediate-close map storage",
                          same_live(before, after) && same_live(anchor, after));
                ROW_CHECK(row, "both indexed queries actually ran over a real immediate close",
                          answer && !absent && answered == 2 && after.calls > before.calls);
                ++queries;
            }
            strategy_cancel_all();
            strategy_close_all();
        }
    }
    int callbacks = 0, queries = 0;
private:
    int commands_;
    std::array<std::string, 20> resting_{};
};

void lookup() {
    for (int commands : {16, 64}) {
        trial_commands = commands;
        std::array<command_heap::Sample, 2> ends{};
        int leg = 0;
        for (int bars : {16, 64}) {
            trial_bars = bars;
            const auto input = tape(bars);
            LookupHost host(commands);
            host.run_tape(input, "adapterlookupindex_immediate_closes_by_bar");
            ROW_CHECK("adapterlookupindex_immediate_closes_by_bar", "every cache cycle ran and both physical closes filled",
                      host.callbacks == bars && host.queries == commands
                          && host.trade_count() == 2 && host.native_open_lots(kNa).empty());
            ends[leg++] = sample();
        }
        ROW_CHECK("adapterlookupindex_immediate_closes_by_bar", "released cache backing is fixed at N and 4N bars",
                  same_live(ends[0], ends[1]));
    }
}

enum class Cycle { CloseReplacement, Defaults, Declined, Admission, EraseOverflow };
const char* primary_row(Cycle c) {
    switch (c) {
    case Cycle::CloseReplacement: return "pineexecutionadapter_order_replaced_close_all";
    case Cycle::Defaults: return "pineexecutionadapter_order_same_bar_defaults";
    case Cycle::Declined: return "pineexecutionadapter_defer_declined_reversal_exits_at_adverse_dependent_closes";
    case Cycle::Admission: return "pineexecutionadapter_record_market_review_selected";
    case Cycle::EraseOverflow: return "placementtable_erase_node";
    }
    return "fixture";
}

// The phase identifies the lifetime claim: a valid direct call window, a
// completed run on the retained host, or destruction of a separate fresh host.
// Each equality compares live requested bytes AND blocks without a tolerance.
void released(Cycle kind, command_heap::Sample before, command_heap::Sample reference,
              command_heap::Sample after, const char* phase) {
    switch (kind) {
    case Cycle::CloseReplacement:
        ROW_CHECK("pineexecutionadapter_order_replaced_close_all", phase,
                  same_live(before, after) && same_live(reference, after));
        break;
    case Cycle::Defaults:
        ROW_CHECK("pineexecutionadapter_order_same_bar_defaults", phase,
                  same_live(before, after) && same_live(reference, after));
        break;
    case Cycle::Declined:
        ROW_CHECK("pineexecutionadapter_defer_declined_reversal_exits_at_adverse_dependent_closes", phase,
                  same_live(before, after) && same_live(reference, after));
        break;
    case Cycle::Admission:
        ROW_CHECK("pineexecutionadapter_record_market_review_selected", phase,
                  same_live(before, after) && same_live(reference, after));
        ROW_CHECK("pineexecutionadapter_apply_open_market_admission_market", phase,
                  same_live(before, after) && same_live(reference, after));
        ROW_CHECK("pineexecutionadapter_apply_open_market_admission_cancellations", phase,
                  same_live(before, after) && same_live(reference, after));
        ROW_CHECK("pineexecutionadapter_apply_open_market_admission_review_handles", phase,
                  same_live(before, after) && same_live(reference, after));
        break;
    case Cycle::EraseOverflow:
        ROW_CHECK("placementtable_erase_node", phase,
                  same_live(before, after) && same_live(reference, after));
        break;
    }
    if (!same_live(before, after) || !same_live(reference, after))
        std::fprintf(stderr,
            "MEMORY [%s] %s before=%zu/%zu reference=%zu/%zu after=%zu/%zu bytes/blocks\n",
            primary_row(kind), phase, before.bytes, before.blocks,
            reference.bytes, reference.blocks, after.bytes, after.blocks);
}

// Reuse trials retain ONE host across warm-up and measured runs; host-exit
// trials use a fresh instance. Only callback/reach counters reset here;
// run() owns all engine resets.
class CycleHost final : public Host {
public:
    CycleHost(Cycle kind, bool active) : Host(kind == Cycle::Admission ? 1 : 8,
        false, kind == Cycle::Admission), kind_(kind), active_(active) {
        if (kind == Cycle::Declined) {
            source::PineStrategyConfig c;
            c.initial_capital = 1000.0;
            // CASH avoids the fixed-default same-point transaction lowering:
            // the real source row must remain a default ReverseTo candidate.
            c.default_qty_type = static_cast<int>(QtyType::CASH);
            c.default_qty_value = 100.0;
            c.pyramiding = 1;
            c.margin_long = c.margin_short = 100.0;
            c.commission_value = 0.0;
            configure_pine_strategy(c);
        }
        for (int i = 0; i < 65; ++i) ids_[i] = key("cmds-erase", i);
    }
    void cycle(const std::vector<Bar>& input, const char* row) {
        callbacks = reached = 0;
        run_tape(input, row);
        if (active_) {
            ROW_CHECK(row, "real positive command branch ran once in this cycle",
                      callbacks == static_cast<int>(input.size()) && reached == 1);
        } else {
            ROW_CHECK(row, "control completed every input bar",
                      callbacks == static_cast<int>(input.size()) && reached == 0);
        }
        ROW_CHECK(row, "cycle returns the physical book to flat", native_open_lots(kNa).empty());
    }
    void on_source_bar(const Bar&) override {
        ++callbacks;
        switch (kind_) {
        case Cycle::CloseReplacement: close_replacement(); break;
        case Cycle::Defaults: defaults(); break;
        case Cycle::Declined: declined(); break;
        case Cycle::Admission: admission(); break;
        case Cycle::EraseOverflow: erase_overflow(); break;
        }
    }
    int callbacks = 0, reached = 0;
private:
    Cycle kind_;
    bool active_;
    const std::string held_ = "cmds-cycle-held", raw_ = "cmds-cycle-raw";
    const std::string close_id_ = "__close__", a_ = "cmds-A", b_ = "cmds-B";
    std::array<std::string, 65> ids_{};

    void close_replacement() {
        const char* row = "pineexecutionadapter_order_replaced_close_all";
        if (pine_bar_index() == 0) strategy_entry(held_, true, kNa, kNa, 1.0);
        if (pine_bar_index() != 1) return;
        ROW_CHECK(row, "replacement stimulus holds the real entry lot",
                  native_open_lots(kNa).size() == 1);
        Handle previous;
        if (active_) {
            strategy_close_all();
            previous = one_live(row, Family::CloseAll, close_id_);
        }
        const auto& id = active_ ? close_id_ : raw_;
        {
            Window window;
            strategy_order(id, false, 1.0, 250.0);
        }
        const auto replacement = one_live(row, Family::Order, id);
        ROW_CHECK(row, "raw priced order was actually accepted",
                  consumer().request_core().find_live(replacement) != nullptr);
        if (active_) {
            ROW_CHECK(row, "__close__ branch cancelled the same-bar CloseAll handle",
                      previous.incarnation != 0 && consumer().request_core().find_live(previous) == nullptr);
            ++reached;
        } else {
            ROW_CHECK(row, "ordinary-id control has no CloseAll to replace", previous.incarnation == 0);
        }
        strategy_cancel(id);
        strategy_close_all();
    }

    void defaults() {
        const char* row = "pineexecutionadapter_order_same_bar_defaults";
        if (pine_bar_index() == 0) {
            const double qty = active_ ? kNa : 1.0;
            strategy_entry(a_, true, kNa, kNa, qty);
            strategy_entry(b_, true, kNa, kNa, qty);
            flush();
            std::size_t default_entries = 0;
            for (const auto& h : live()) {
                const auto found = placements().find(h.incarnation);
                if (found != placements().end() && found->second.family == Family::Entry
                    && found->second.opening && !std::isfinite(found->second.requested_qty)
                    && found->second.projection_created_bar == 0
                    && !std::isfinite(found->second.exit_levels.limit)
                    && !std::isfinite(found->second.exit_levels.stop)) ++default_entries;
            }
            ROW_CHECK(row, "two real same-bar default entries, or zero in the explicit control",
                      default_entries == (active_ ? 2U : 0U));
            const auto before = (adapter().*access(Delayed{})).size();
            {
                Window window;
                strategy_order(raw_, true, 1.0);
            }
            const auto& delayed = adapter().*access(Delayed{});
            ROW_CHECK(row, "same_bar_defaults>=2 delayed exactly this raw market command",
                      delayed.size() == before + (active_ ? 1U : 0U));
            if (active_) {
                ROW_CHECK(row, "delayed command is the actual submitted raw order",
                          !delayed.empty() && delayed.back().snapshot.source_id == raw_);
                ++reached;
            }
        }
        if (pine_bar_index() == 3) strategy_close_all();
    }

    void declined() {
        const char* row = "pineexecutionadapter_defer_declined_reversal_exits_at_adverse_dependent_closes";
        if (pine_bar_index() == 0) strategy_entry(held_, true, kNa, kNa, 1.0);
        if (pine_bar_index() != 1) return;
        const auto lots = native_open_lots(kNa);
        ROW_CHECK(row, "one real long lot precedes the reversal command",
                  lots.size() == 1 && lots.front().signed_units == 1.0);
        strategy_entry(b_, false);
        flush();
        const auto reversal = one_live(row, Family::Entry, b_);
        strategy_close(held_);
        flush();
        const auto close = one_live(row, Family::Close, held_);
        const auto rev = placements().find(reversal.incarnation);
        const auto dep = placements().find(close.incarnation);
        ROW_CHECK(row, "real close is full-sized, after a default reversal on the same bar",
                  rev != placements().end() && dep != placements().end()
                      && rev->second.reverse_to && !std::isfinite(rev->second.requested_qty)
                      && !rev->second.projection_after_close
                      && dep->second.command_ordinal > rev->second.command_ordinal
                      && dep->second.projection_created_bar == rev->second.projection_created_bar
                      && dep->second.qty_percent == 100.0);
        const auto* point = consumer().current_point();
        ROW_CHECK(row, "adverse helper has a real decision context", point != nullptr);
        if (!point) return;
        // Default CASH=100 at signal 100 buys one unit. Point value=1, FX=1,
        // margin=100%, frozen equity
        // 1000. The supplied next-open price 2000 costs 2000>1000; control
        // price 100 costs 100<1000. No product snapshot is manufactured.
        Bar next{active_ ? 2000.0 : 100.0, 2001.0, 99.0, 100.0, 1.0, kStart + 2 * kMinute};
        if (rev != placements().end()) {
            const double units = std::isfinite(rev->second.sizing.frozen_units)
                && rev->second.sizing.frozen_units > 0.0 ? rev->second.sizing.frozen_units : 1.0;
            ROW_CHECK(row, "independent one-unit margin arithmetic selects the intended branch",
                      (units * next.open > rev->second.sizing.equity) == active_);
        }
        const auto before_call = sample();
        {
            Window window;
            (adapter().*access(DeferDeclined{}))(next, point->decision, false);
        }
        released(kind_, before_call, before_call, sample(),
                 "adverse helper return with host alive, before script cleanup");
        ROW_CHECK(row, "only the declined branch cancels the actual dependent close",
                  (consumer().request_core().find_live(close) == nullptr) == active_);
        ROW_CHECK(row, "helper preserves the reversal's live opening half",
                  consumer().request_core().find_live(reversal) != nullptr);
        if (active_) ++reached;
        strategy_cancel_all();
        strategy_close_all();
    }

    void admission() {
        const char* row = "pineexecutionadapter_record_market_review_selected";
        if (pine_bar_index() == 0) {
            // The priced control never enters market/review_handles/selected.
            strategy_entry(a_, true, kNa, active_ ? kNa : 200.0);
            // PineExecutionAdapter::entry freezes an unmarketable default-percent stop at its
            // stop price. The control long sizes to 1000/200=5 units; the
            // old short sized to 1000/50=20. entry's margin check
            // uses the signal mark 100, so 20*100=2000>1000 rejects the short
            // before submission. Give only that control an explicit unit:
            // entry then checks 1*100=100<=1000.
            // The default long also fits (5*100=500). Stops 200 and 50 stay
            // outside tape high=101/low=99, so both control handles are live.
            // The active pair keeps default sizing and its gross rejection;
            // apply_open_market_admission excludes both priced controls from market.
            strategy_entry(b_, false, kNa, active_ ? kNa : 50.0,
                           active_ ? kNa : 1.0);
            flush();
            const auto first = one_live(row, Family::Entry, a_);
            const auto second = one_live(row, Family::Entry, b_);
            const auto* point = consumer().current_point();
            ROW_CHECK(row, "admission has a real source decision context", point != nullptr);
            if (!point) return;
            auto next = point->decision;
            ++next.coordinate.interval_index;
            next.script_bar_open_ms += kMinute;
            next.sub_bar_open_ms += kMinute;
            // This directly invokes the production next-open helper, before
            // matching, with the next input's unchanged price and coordinate.
            // It sees two REAL accepted orders from the previous source bar.
            {
                Window window;
                (adapter().*access(AdmitOpen{}))(next);
            }
            const bool first_live = consumer().request_core().find_live(first) != nullptr;
            const bool second_live = consumer().request_core().find_live(second) != nullptr;
            ROW_CHECK("pineexecutionadapter_apply_open_market_admission_cancellations", "gross pair cancels exactly the later accepted handle",
                      first_live && second_live == !active_);
            std::size_t reviewed = 0;
            for (const auto& h : {first, second}) {
                const auto found = placements().find(h.incarnation);
                if (found != placements().end() && found->second.market_admission.review()
                    && found->second.market_admission.review()->checkpoint == pineforge::admission::Checkpoint::DefaultGross)
                    ++reviewed;
            }
            ROW_CHECK("pineexecutionadapter_record_market_review_selected", "selected produced one real review receipt for each command",
                      reviewed == (active_ ? 2U : 0U));
            ROW_CHECK("pineexecutionadapter_apply_open_market_admission_market", "market population reached the two-candidate gross rule",
                      reviewed == (active_ ? 2U : 0U) && first_live && second_live == !active_);
            ROW_CHECK("pineexecutionadapter_apply_open_market_admission_review_handles", "review_handles delivered both accepted command handles",
                      reviewed == (active_ ? 2U : 0U));
            // Own arithmetic: each 100%-of-1000 entry at 100 is ten units;
            // gross (10+10)*100=2000 exceeds 1000. Each own 10*100 fits.
            if (active_) {
                const auto f = placements().find(first.incarnation);
                const auto s = placements().find(second.incarnation);
                ROW_CHECK("pineexecutionadapter_apply_open_market_admission_cancellations", "gross rejection is funded by the independent ten-unit model",
                          f != placements().end() && s != placements().end()
                              && f->second.sizing.frozen_units == 1000.0 / 100.0
                              && s->second.sizing.frozen_units == 1000.0 / 100.0);
                ++reached;
            }
        }
        if (pine_bar_index() == 1) {
            const auto lots = native_open_lots(kNa);
            ROW_CHECK("pineexecutionadapter_apply_open_market_admission_cancellations", "only the earlier gross-pair entry physically filled",
                      active_ ? (lots.size() == 1 && lots.front().entry_label == a_
                          && lots.front().signed_units == 10.0) : lots.empty());
            strategy_cancel_all();
            strategy_close_all();
        }
    }

    void erase_overflow() {
        const char* row = "placementtable_erase_node";
        if (pine_bar_index() != 0) return;
        // Source pine_adapter.hpp declares kSpareNodes=64. The control moves
        // exactly 64 real nodes to spares; one more accepted/cancelled node
        // takes the local's non-transfer branch and must end at return.
        const int count = active_ ? 65 : 64;
        std::array<std::uint64_t, 65> incarnations{};
        for (int i = 0; i < count; ++i) {
            {
                Window window;
                strategy_order(ids_[i], true, 1.0, 1.0);
            }
            const auto h = one_live(row, Family::Order, ids_[i]);
            incarnations[i] = h.incarnation;
        }
        strategy_cancel_all();
        ROW_CHECK(row, "all table nodes came from real accepted then cancelled orders",
                  live().empty() && placements().size() == static_cast<std::size_t>(count));
        const auto erased = placements().erased_rows();
        for (int i = 0; i < count; ++i) {
            bool removed = false;
            const auto before = sample();
            {
                Window window;
                removed = placements().erase(incarnations[i]);
            }
            const auto after = sample();
            ROW_CHECK(row, "production erase consumed this actual terminal placement", removed);
            ROW_CHECK(row, "spare transfer has the source's fixed 64-node maximum",
                      (placements().*access(Spares{})).size()
                          == std::min<std::size_t>(static_cast<std::size_t>(i + 1), 64));
            if (i == 64) {
                // All 65 nodes were allocated while recording. The overflow
                // unique_ptr owns an EXISTING block, so its destruction must
                // decrease live storage, rather than leave before == after.
                ROW_CHECK(row, "overflow erase releases storage at return with table and host alive",
                          after.bytes < before.bytes && after.blocks < before.blocks);
                if (!(after.bytes < before.bytes && after.blocks < before.blocks))
                    std::fprintf(stderr,
                        "MEMORY [%s] overflow return before=%zu/%zu after=%zu/%zu bytes/blocks\n",
                        row, before.bytes, before.blocks, after.bytes, after.blocks);
            }
        }
        ROW_CHECK(row, "all inserted nodes were removed; the last positive node overflowed the spare cap",
                  placements().size() == 0 && placements().erased_rows() - erased == static_cast<std::uint64_t>(count)
                      && (placements().*access(Spares{})).size() == 64);
        if (active_) ++reached;
    }
};

// Both original tape lengths and both variants keep their full stimulus.
// Three fixed warm-up runs precede the reference, then EVERY completed run is
// compared with it on the same live host, including N=16 and 4N=64 endpoints.
// Accepted orders legitimately retain engine state at call return. For those
// families, ALSO restore the original fresh-host exit checks BEFORE populated
// warm-up can hide clear-on-entry static capacity. These prove only that command
// allocations do not outlive the host; reuse equalities prove no per-run growth.
void cycles(Cycle kind) {
    const char* row = primary_row(kind);
    const bool check_host_exit = kind == Cycle::CloseReplacement
        || kind == Cycle::Defaults || kind == Cycle::Admission;
    command_heap::Sample destroyed_ends[2]{};
    int leg = 0;
    for (int commands : {16, 64}) {
        trial_commands = commands;
        trial_bars = commands;
        const auto control_input = tape(commands);
        auto input = control_input;
        if (kind == Cycle::Declined)
            input[2] = {2000.0, 2001.0, 99.0, 100.0, 1.0, kStart + 2 * kMinute};
        if (check_host_exit) {
            // Keep the original command-only allocation windows here. Setup
            // is outside recording, and each host dies before its exit sample.
            trial_cycle = 0;
            {
                CycleHost control(kind, false);
                control.cycle(control_input, row);
            }
            const auto steady = sample();
            for (int cycle = 1; cycle <= commands; ++cycle) {
                trial_cycle = cycle;
                const auto before_cycle = sample();
                {
                    CycleHost host(kind, true);
                    host.cycle(input, row);
                }
                const auto after = sample();
                released(kind, before_cycle, steady, after,
                         "fresh host destroyed: command allocations do not outlive their host");
            }
            destroyed_ends[leg++] = sample();
        }
        for (bool active : {false, true}) {
            // Count setup, warm-up, reuse, and all receiving owners uniformly.
            // The host is destroyed only AFTER all call/run/endpoint samples.
            Window lifetime;
            CycleHost host(kind, active);
            const auto& script = active ? input : control_input;
            trial_cycle = 0;
            host.cycle(script, row);
            host.cycle(script, row);
            host.cycle(script, row);
            const auto steady = sample();
            command_heap::Sample at_n{}, at_4n{};
            for (int cycle = 1; cycle <= 64; ++cycle) {
                trial_cycle = cycle;
                const auto before_cycle = sample();
                host.cycle(script, row);
                const auto after = sample();
                released(kind, before_cycle, steady, after,
                         "completed run with same host alive after three fixed warm-ups");
                if (cycle == 16) at_n = after;
                if (cycle == 64) at_4n = after;
            }
            released(kind, steady, steady, at_n, "N=16 runs with host alive");
            released(kind, steady, steady, at_4n, "4N=64 runs with host alive");
            released(kind, at_n, at_n, at_4n, "N versus 4N on same live host");
            std::printf(
                "MEMORY [%s] active=%d bars=%d reference=%zu/%zu N=%zu/%zu 4N=%zu/%zu bytes/blocks\n",
                row, int(active), commands, steady.bytes, steady.blocks,
                at_n.bytes, at_n.blocks, at_4n.bytes, at_4n.blocks);
        }
    }
    if (check_host_exit)
        released(kind, destroyed_ends[0], destroyed_ends[0], destroyed_ends[1],
                 "fresh hosts destroyed: N=16 versus 4N=64 command cycles");
    trial_cycle = 0;
}

void replacement_cycles() { cycles(Cycle::CloseReplacement); }
void default_cycles() { cycles(Cycle::Defaults); }
void declined_cycles() { cycles(Cycle::Declined); }
void admission_cycles() { cycles(Cycle::Admission); }
void overflow_cycles() { cycles(Cycle::EraseOverflow); }

void allocator_control() {
    const auto before = sample();
    void* values[4]{};
    {
        Window window;
        values[0] = ::operator new(37);
        values[1] = ::operator new[](73, std::nothrow);
        values[2] = ::operator new(129, std::align_val_t{64});
        values[3] = ::operator new[](257, std::align_val_t{128}, std::nothrow);
    }
    const auto held = sample();
    ROW_CHECK("allocator", "all four forms are counted outside their allocation window",
              held.bytes == before.bytes + 37 + 73 + 129 + 257 && held.blocks == before.blocks + 4);
    ROW_CHECK("allocator", "over-aligned forms preserve the requested alignment",
              reinterpret_cast<std::uintptr_t>(values[2]) % 64 == 0
                  && reinterpret_cast<std::uintptr_t>(values[3]) % 128 == 0);
    ::operator delete(values[0], std::size_t{37});
    ::operator delete[](values[1], std::nothrow);
    ::operator delete(values[2], std::size_t{129}, std::align_val_t{64});
    ::operator delete[](values[3], std::align_val_t{128}, std::nothrow);
    ROW_CHECK("allocator", "sized/aligned/nothrow deletes release the marked cohort even with recording off",
              same_live(before, sample()));
}

struct Case { std::initializer_list<const char*> rows; void (*run)(); };
} // namespace

int main() {
    const Case cases[] = {
        {{"allocator"}, allocator_control},
        {{"droppedclosereceipt_source_id", "droppedclosereceipt_comment", "droppedclosereceipt_transient_command_result"}, receipts},
        {{"pineexecutionadapter_record_market_review_selected", "pineexecutionadapter_apply_open_market_admission_market", "pineexecutionadapter_apply_open_market_admission_cancellations", "pineexecutionadapter_apply_open_market_admission_review_handles"}, admission_cycles},
        {{"workingrequestcore_validate_request_canonical"}, canonical},
        {{"pineexecutionadapter_apply_terminal_explicit_market_policy_candidates"}, terminal},
        {{"adapterlookupindex_immediate_closes_by_bar"}, lookup},
        {{"pineexecutionadapter_order_replaced_close_all"}, replacement_cycles},
        {{"pineexecutionadapter_order_same_bar_defaults"}, default_cycles},
        {{"pineexecutionadapter_defer_declined_reversal_exits_at_adverse_dependent_closes"}, declined_cycles},
        {{"placementtable_erase_node"}, overflow_cycles},
    };
    // Admission precedes every other fixture that submits unpriced entries:
    // any retained static capacity must be charged on its first use.
    for (const auto& item : cases) {
        try {
            item.run();
        } catch (const std::exception& e) {
            command_heap::recording = false;
            for (const char* row : item.rows)
                check(row, e.what(), "case completed without exception", false, __LINE__);
        } catch (...) {
            command_heap::recording = false;
            for (const char* row : item.rows)
                check(row, "non-standard exception", "case completed without exception", false, __LINE__);
        }
    }
    std::printf("memory proof release_commands: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
