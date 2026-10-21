// LAB-O1SEAMS s4, round 1; frozen base 87ba09a7. EDIT-ONLY: never compiled/run.
// Contract: scratch-store.md copy, sha256
// 71adb08d2a4308f961cc8f8f24a7c91fffd38e9b487d04cef558c133cac6ce5e,
// section "O(1) seams s3 and s4", lines 357-767.
//
// Interface debt, deliberately left RED:
//   WorkingRequestCore::bind_scratch_store(ScratchStore&)
//   PineExecutionAdapter::bind_scratch_store(ScratchStore&)
//   range_end_trades_.row(index)
//   OriginRoster::visit_positions_of(handle, visitor)
// These are minimal test spellings for borrowing the explicitly selected
// store and reading the terminal descriptor, NOT declarations of production
// APIs. The spec explicitly leaves that interface decision open (758-763).
// The existing bind_scratch_slab is a different substrate, not this binding.
// Rebinding an owner after orderly same-backing reopen must recover its last
// committed record/index relation. No cross-process persistence is requested.
//
// Each named function below has a fresh, plain value/order oracle. Oracle
// allocations and the test-memory medium are outside the engine heap census.
// For the queue/index census, engine owners, shared payloads, spare capacity
// and transient allocations are inside it from construction through
// destruction. S4-RE separately identifies the producer's physical-book input.
// No warm-up population is a memory allowance. Caller pages are fixed first.
// The red gate uses the construction high-water byte census as a fixed
// allowance, never a warm-up population. It sees transient allocate/free peaks
// as well as final live bytes. Allocation count alone is not a memory bound.
// Allocator rounding A and physical charge M still belong to the measuring lane.
// There is no s3 journal codec, S34-R or S34-B test in this file.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;
namespace no = pineforge::native_order;
namespace ps = pineforge::source;
constexpr std::size_t page_bytes = 512;
constexpr std::uint64_t fold_basis = 1469598103934665603ULL;
constexpr std::uint64_t fold_prime = 1099511628211ULL;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
void status_is(ScratchStatus actual, ScratchStatus expected, const char* why) {
    if (actual != expected) {
        std::fprintf(stderr, "%s: status %u, expected %u\n", why,
                     static_cast<unsigned>(actual), static_cast<unsigned>(expected));
        throw std::runtime_error(why);
    }
}

// Same explicit-instantiation technique as test_origin_roster_retirement.cpp.
// Access existing members only; do not change visibility or add fixture state
// to a broker class.
template<class Tag, auto Member> struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
#define MEMBER_TAG(tag, owner, member)                                        \
    struct tag { friend auto access(tag); };                                 \
    template struct PrivateAccess<tag, &owner::member>
MEMBER_TAG(Cohorts, ps::PineExecutionAdapter, cohorts_by_id_);
MEMBER_TAG(Placements, ps::PineExecutionAdapter, placement_);
MEMBER_TAG(SourceKeys, ps::PineExecutionAdapter, live_by_source_key_);
MEMBER_TAG(SourceLive, ps::PineExecutionAdapter, live_handles_);
MEMBER_TAG(BracketQueue, ps::PineExecutionAdapter, pending_bracket_legs_);
MEMBER_TAG(EntryQueue, ps::PineExecutionAdapter, pending_entries_);
MEMBER_TAG(DelayedQueue, ps::PineExecutionAdapter, delayed_market_orders_);
MEMBER_TAG(SellQueue, ps::PineExecutionAdapter, deferred_open_marketable_sells_);
MEMBER_TAG(RearmQueue, ps::PineExecutionAdapter, throttled_reopen_rearm_);
MEMBER_TAG(SameBarQueue, ps::PineExecutionAdapter, pending_same_bar_commands_);
MEMBER_TAG(ShadowQueue, ps::PineExecutionAdapter, source_shadow_pending_);
MEMBER_TAG(CoofQueue, ps::PineExecutionAdapter, pending_coof_requests_);
MEMBER_TAG(RevivalQueue, ps::PineExecutionAdapter, pending_margin_revivals_);
MEMBER_TAG(AnchoredQueue, ps::PineExecutionAdapter, anchored_relative_legs_);
MEMBER_TAG(RetireOrigins, ps::PineExecutionAdapter, retire_origin_prefixes);
MEMBER_TAG(Remember, ps::PineExecutionAdapter, remember);
MEMBER_TAG(RetireHandle, ps::PineExecutionAdapter, retire);
MEMBER_TAG(NoteEvents, NativeExecutionConsumer, note_committed_events);
MEMBER_TAG(NoteCohorts, NativeExecutionConsumer, note_cohort_receipts);
#undef MEMBER_TAG

template<class Tag> auto& member(ps::PineExecutionAdapter& adapter, Tag tag) {
    return adapter.*access(tag);
}

// All comparisons of identities in the reference model compare TEXT and the
// entire run, never SessionKey's interned pointer or incarnation alone.
bool same_handle(const no::RequestHandle& a, const no::RequestHandle& b) {
    return a.incarnation == b.incarnation && a.run.run_number == b.run.run_number
        && a.run.session_key.string() == b.run.session_key.string();
}
bool has_handle(const std::vector<no::RequestHandle>& rows, const no::RequestHandle& h) {
    for (const auto& row : rows) if (same_handle(row, h)) return true;
    return false;
}
void remove_one(std::vector<no::RequestHandle>& rows, const no::RequestHandle& h) {
    for (auto at = rows.begin(); at != rows.end(); ++at) {
        if (same_handle(*at, h)) { rows.erase(at); return; }
    }
    throw std::runtime_error("oracle removal names a present member");
}

// This pause is ONLY for test-owned oracle work. Never wrap a production call
// in it: marked engine blocks must also be deleted while recording is true.
struct OracleWork {
    bool saved = global_allocation::recording;
    OracleWork() { global_allocation::recording = false; }
    ~OracleWork() { global_allocation::recording = saved; }
};

struct HeapCensus {
    static inline HeapCensus* active = nullptr;
    std::size_t peak = 0;
    std::size_t construction_peak = 0;
    std::size_t construction_calls = 0;
    std::size_t calls = 0;
    static void allocated(std::size_t) {
        ++active->calls;
        active->peak = std::max(active->peak, global_allocation::live_bytes);
    }
    HeapCensus() {
        require(active == nullptr && !global_allocation::recording,
                "census windows are disjoint");
        global_allocation::live_bytes = 0;
        active = this;
        global_allocation::on_recorded = &allocated;
        global_allocation::recording = true;
    }
    void seal() { construction_peak = peak; construction_calls = calls; }
    void fixed(const ScratchStore& store, std::uint64_t resident_at_construction) const {
        if (peak > construction_peak) {
            std::fprintf(stderr, "heap census: peak=%zu construction_bound=%zu calls=%zu\n",
                         peak, construction_peak, calls);
        }
        require(store.stats().resident_bytes == resident_at_construction,
                "ScratchStore workspace never grows");
        require(peak <= construction_peak,
                "B_u: a population or transient allocation grew past construction");
    }
    ~HeapCensus() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        active = nullptr;
    }
};

struct Charges {
    std::uint64_t reads = 0, writes = 0, syncs = 0, bytes = 0, failures = 0;
    static void charge(void* context, const ScratchIoCharge& c) noexcept {
        auto& self = *static_cast<Charges*>(context);
        if (c.operation == ScratchIoOperation::read_page) ++self.reads;
        else if (c.operation == ScratchIoOperation::write_page) ++self.writes;
        else ++self.syncs;
        self.bytes += c.bytes;
        if (c.status != ScratchStatus::ok) ++self.failures;
    }
    ScratchIoObserver observer() { return {this, &charge}; }
};

// The same fault-wrapper shape as test_scratch_store.cpp, with fixed tear
// scratch rather than an allocating fault path. Only the outer backing has an
// observer: forwarding a public call to the memory medium is not double billed.
class FaultBacking final : public ScratchBacking {
public:
    FaultBacking(FixedMemoryScratchBacking& medium, ScratchIoObserver observer)
        : ScratchBacking(medium.kind(), medium.page_size(), observer), medium_(medium) {}
    std::uint64_t page_count() const noexcept override { return medium_.page_count(); }
    void arm(ScratchIoOperation op, std::uint64_t nth, bool tear = false) {
        operation_ = op; remaining_ = nth; tear_ = tear;
    }
    void disarm() { remaining_ = 0; corrupt_reads = false; }
    std::uint64_t hits = 0;
    std::uint64_t cap = std::numeric_limits<std::uint64_t>::max();
    bool corrupt_reads = false;

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        if (hit(ScratchIoOperation::read_page)) return ScratchStatus::io_error;
        const auto result = medium_.read_page(id, out);
        if (result == ScratchStatus::ok && corrupt_reads) {
            // Actual invalid stored framing reaches the store, rather than a
            // fixture that simply manufactures a "corrupt" return value.
            std::memset(out.data, 0, out.size);
            ++corruptions;
        }
        return result;
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        if (id >= cap) { ++hits; return ScratchStatus::io_error; }
        if (hit(ScratchIoOperation::write_page)) {
            if (tear_) {
                std::array<std::byte, page_bytes> old{};
                (void)medium_.read_page(id, {old.data(), old.size()});
                std::memcpy(old.data(), in.data, old.size() / 2);
                (void)medium_.write_page(id, {old.data(), old.size()});
            }
            return ScratchStatus::io_error;
        }
        return medium_.write_page(id, in);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        if (hit(ScratchIoOperation::sync)) return ScratchStatus::io_error;
        return medium_.sync(durability);
    }

public:
    mutable std::uint64_t corruptions = 0;

private:
    bool hit(ScratchIoOperation op) const noexcept {
        if (remaining_ == 0 || op != operation_) return false;
        if (--remaining_ != 0) return false;
        ++const_cast<FaultBacking*>(this)->hits;
        return true;
    }
    FixedMemoryScratchBacking& medium_;
    ScratchIoOperation operation_ = ScratchIoOperation::write_page;
    mutable std::uint64_t remaining_ = 0;
    bool tear_ = false;
};

ScratchStatus lexical(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                      int& order) noexcept {
    std::array<std::byte, 32> left{}, right{};
    const auto common = std::min(a.size(), b.size());
    for (std::uint64_t at = 0; at < common;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), common - at));
        auto status = a.read(at, {left.data(), n});
        if (status != ScratchStatus::ok) return status;
        status = b.read(at, {right.data(), n});
        if (status != ScratchStatus::ok) return status;
        for (std::size_t i = 0; i < n; ++i) {
            const auto x = std::to_integer<unsigned>(left[i]);
            const auto y = std::to_integer<unsigned>(right[i]);
            if (x != y) { order = x < y ? -1 : 1; return ScratchStatus::ok; }
        }
        at += n;
    }
    order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    return ScratchStatus::ok;
}

struct Rig {
    // A fixture medium, NOT an engine allocation island or physical O(1)
    // proof. It is provisioned before the census and never grows.
    std::vector<std::byte> storage;
    Charges charges;
    FixedMemoryScratchBacking medium;
    FaultBacking backing;
    explicit Rig(std::size_t pages = 131072)
        : storage(pages * page_bytes),
          medium({storage.data(), storage.size()}, page_bytes),
          backing(medium, charges.observer()) {}
    ScratchStoreConfig config(ScratchOpenMode mode = ScratchOpenMode::create_empty,
                              std::size_t cache = 2) const {
        return {cache, page_bytes, 1, 1, {0x4f315334, nullptr, &lexical}, mode};
    }
    void accounting(const ScratchStore& store, const Charges& at_open = {}) const {
        const auto s = store.stats();
        require(s.page_reads == charges.reads - at_open.reads, "all reads charged once");
        require(s.page_writes == charges.writes - at_open.writes, "all writes charged once");
        require(s.syncs == charges.syncs - at_open.syncs, "all sync calls charged once");
        require(charges.bytes - at_open.bytes
                    == (s.page_reads + s.page_writes) * store.page_size(),
                "observer reports requested page bytes, not guessed traffic");
        require(s.backing_pages == backing.page_count(), "stats names the actual extent");
        require(s.backing_bytes == s.backing_pages * store.page_size(),
                "backing extent proxy is pages times page_size, counted separately");
    }
};

no::RunIdentity run_id(std::uint64_t number = 7) { return {"o1s4-red", number}; }
no::Request limit_order(double units, double price, const char* label = "q") {
    no::Request r{no::Transact{units}, label, ""};
    r.trigger = no::Limit{price};
    return r;
}

struct Host final : ps::PineStrategyHost {
    ps::PineExecutionAdapter& adapter() { return adapter_; }
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    no::WorkingRequestCore& core() {
        return const_cast<no::WorkingRequestCore&>(consumer().request_core());
    }
    void on_source_bar(const Bar&) override {}
    using BacktestEngine::pyramid_entries_;
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::range_end_trades_;
    using BacktestEngine::commission_type_;
    using BacktestEngine::commission_value_;
    using ps::PineStrategyHost::source_pending_view;
};

// A deliberately ordinary oracle. Its growing containers are test inputs, not
// proposed production data structures. No method below calls the engine.
struct Model {
    struct Row {
        no::RequestHandle handle;
        std::uint64_t priority;
        double units, price;
        std::string label;
    };
    struct Cohort {
        std::vector<no::RequestHandle> origins, opened, canonical;
        std::map<std::uint64_t, double> units;
        std::size_t base = 0;
        std::uint64_t digest = fold_basis;
        void append(no::RequestHandle h) {
            origins.push_back(h);
            digest = (digest ^ h.incarnation) * fold_prime;
        }
    };
    no::RunIdentity run = run_id();
    std::uint64_t next_incarnation = 1;
    std::vector<Row> queue;
    std::map<std::string, Cohort> cohorts;
    std::map<std::uint64_t, ps::PlacementSnapshot> placements;
    std::map<std::uint64_t, no::RequestHandle> source_keys;
    std::map<std::uint64_t, std::uint64_t> roots;
    std::map<std::uint64_t, std::size_t> names;
    std::vector<std::string> queued_labels;

    no::RequestHandle submit(double units, double price, const std::string& label = "q") {
        const no::RequestHandle h{run, next_incarnation++};
        queue.push_back({h, h.incarnation, units, price, label});
        roots[h.incarnation] = h.incarnation;
        return h;
    }
    void erase(no::RequestHandle h) {
        for (auto at = queue.begin(); at != queue.end(); ++at) {
            if (same_handle(at->handle, h)) { queue.erase(at); return; }
        }
        throw std::runtime_error("model command targets a live handle");
    }
    no::RequestHandle replace(no::RequestHandle h, double units, double price, bool keep) {
        const auto root = roots.at(h.incarnation);
        erase(h);
        const auto priority = next_incarnation++;
        const no::RequestHandle successor = keep ? h : no::RequestHandle{run, priority};
        roots[successor.incarnation] = root;
        queue.push_back({successor, priority, units, price, "q"});
        return successor;
    }
    bool native_member(const Cohort& c, no::RequestHandle h) const {
        if (h.run.run_number != run.run_number
            || h.run.session_key.string() != run.session_key.string()) return false;
        const auto found = roots.find(h.incarnation);
        if (found == roots.end()) return false;
        return has_handle(c.canonical, {run, found->second});
    }
    std::array<bool, 5> pins(const Cohort& c, no::RequestHandle h) const {
        const auto named = names.find(h.incarnation);
        return {{placements.count(h.incarnation) != 0,
                 h.incarnation != 0 && named != names.end() && named->second != 0,
                 c.units.count(h.incarnation) != 0, has_handle(c.opened, h),
                 native_member(c, h)}};
    }
    void retire(Cohort& c) const {
        while (c.base < c.origins.size()) {
            const auto p = pins(c, c.origins[c.base]);
            bool keep = false;
            for (bool bit : p) keep = keep || bit;
            if (keep) break;
            ++c.base;
        }
    }
};

struct Subject {
    Host host;
    std::uint64_t inc = 1, ord = 1;
    explicit Subject(ScratchStore& store) {
        status_is(store.status(), ScratchStatus::ok, "fixture opens");
        host.core().reset(run_id());
        // Expected first diagnostics on the frozen head: these bindings do
        // not exist. No declaration or implementation is supplied by this TU.
        host.core().bind_scratch_store(store);
        host.adapter().bind_scratch_store(store);
    }
    no::WorkingRequestCore& core() { return host.core(); }
    ps::PineExecutionAdapter& adapter() { return host.adapter(); }
    void note(std::size_t first) {
        (host.consumer().*access(NoteEvents{}))({first, core().history_end() - first});
    }
    no::RequestHandle submit(const no::Request& r) {
        const auto first = core().history_end();
        const auto out = core().submit(r, 0, inc, ord);
        require(out.status == no::SubmitStatus::Accepted && out.handle.has_value(),
                "real submit accepted, never silently capped");
        note(first);
        return *out.handle;
    }
    no::RequestHandle replace(no::RequestHandle h, const no::Request& r, bool keep) {
        const auto first = core().history_end();
        no::ReplaceOptions options;
        options.keep_handle = keep;
        const auto out = core().replace(h, r, 0, inc, ord, std::nullopt, options);
        require(out.status == no::ReplaceStatus::Replaced && out.successor.has_value(),
                "real replace completed");
        note(first);
        return *out.successor;
    }
    void cancel(no::RequestHandle h) {
        const auto first = core().history_end();
        require(core().cancel(h, ord).status == no::CancelStatus::Cancelled, "real cancel");
        note(first);
    }
    void cohort_note() { (host.consumer().*access(NoteCohorts{}))(); }
    auto cohort(const std::string& id) {
        auto& table = member(adapter(), Cohorts{});
        auto found = table.find(id);
        if (found != table.end()) return found;
        using Facts = std::remove_reference_t<decltype(found->second)>;
        Facts facts;
        facts.handle = core().cohort_open();
        return table.emplace(id, std::move(facts)).first;
    }
    void append(const std::string& id, no::RequestHandle h) {
        cohort(id)->second.origins.push_back(h);
    }
    void retire() { (adapter().*access(RetireOrigins{}))(); }
    void remember(no::RequestHandle h, ps::PlacementSnapshot snapshot) {
        (adapter().*access(Remember{}))(h, std::move(snapshot));
    }
    void retire_handle(no::RequestHandle h) {
        (adapter().*access(RetireHandle{}))(h);
    }
    void rebind(ScratchStore& reopened) {
        core().bind_scratch_store(reopened);
        adapter().bind_scratch_store(reopened);
    }
};

void check_book(const no::WorkingRequestCore& core, const Model& model) {
    require(core.live_size() == model.queue.size(), "one executable row per model handle");
    std::size_t rank = 0;
    for (const auto& actual : core.live()) {
        require(rank < model.queue.size(), "walk does not duplicate executable rows");
        const auto& expected = model.queue[rank++];
        require(same_handle(actual.handle(), expected.handle), "queue order equals reference order");
        require(actual.priority() == expected.priority, "matching priority equals command model");
        require(std::get<no::Transact>(actual.request().intent).signed_units == expected.units,
                "queue quantity survives replacement/spill");
        require(std::get<no::Limit>(actual.request().trigger).price == expected.price,
                "queue price survives replacement/spill");
        require(actual.request().label == expected.label, "queue label is a value");
        const auto* by_handle = core.find_live(expected.handle);
        require(by_handle && same_handle(by_handle->handle(), expected.handle),
                "handle lookup agrees with ordered book");
    }
    require(rank == model.queue.size(), "walk reaches every executable row");
}

void check_roster(Subject& s, const std::string& id, const Model::Cohort& expected) {
    const auto facts = s.cohort(id);
    const auto& actual = facts->second.origins;
    require(actual.size() == expected.origins.size(), "retirement preserves lifetime size");
    require(actual.digest() == expected.digest, "append digest equals independent word fold");
    require(actual.base() == expected.base, "only the all-false contiguous prefix is released");
    std::size_t position = expected.base;
    for (const auto& h : actual) {
        require(position < expected.origins.size()
                    && same_handle(h, expected.origins[position++]),
                "roster readback retains exact global order and full handles");
    }
    require(position == expected.origins.size(), "all retained roster rows are addressable");
    // Reference positions are calculated without positions_of or any other
    // engine index. Group once in the ORACLE to avoid a quadratic test harness;
    // the production helper must still filter an incarnation bucket in full.
    using Key = std::tuple<std::string, std::uint64_t, std::uint64_t>;
    std::map<Key, std::vector<std::size_t>> positions;
    {
        OracleWork oracle;
        for (std::size_t i = expected.base; i < expected.origins.size(); ++i) {
            const auto& h = expected.origins[i];
            positions[{h.run.session_key.string(), h.run.run_number, h.incarnation}].push_back(i);
        }
    }
    for (const auto& group : positions) {
        const auto& want = group.second;
        std::size_t next = 0;
        actual.visit_positions_of(expected.origins[want.front()], [&](std::size_t position) {
            require(next < want.size() && position == want[next++],
                    "positions remain global, ascending, duplicate preserving");
            return true;
        });
        require(next == want.size(), "position index omitted no matching row");
    }
}

void isolated(const Model& model, const Model::Cohort& facts,
              no::RequestHandle h, std::size_t which) {
    const auto p = model.pins(facts, h);
    for (std::size_t i = 0; i < p.size(); ++i)
        require(p[i] == (i == which), "fixture pins exactly the requested disjunct");
}

std::uint64_t store_digest(ScratchStore& store) {
    std::array<std::byte, page_bytes> page{};
    std::uint64_t result = 0;
    status_is(store.state_digest({page.data(), page.size()}, result), ScratchStatus::ok,
              "committed scratch state digest");
    return result;
}

// Ownership, S4-O (spec 401). Input: normal replacement, kept-handle re-price,
// cancellation and execution with two unsubmitted source instructions.
// fails to compile: WorkingRequestCore::bind_scratch_store and
// PineExecutionAdapter::bind_scratch_store (Subject). Once bound, the exact
// failing assertions on this head are the heap gate and nonzero store traffic:
// the current core/source queues are not owned by this ScratchStore.
void s4_one_queue_owner_kept_handle_order_red() {
    Model model;
    std::array<no::RequestHandle, 4> handles{};
    Rig rig;
    HeapCensus heap;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    const auto resident = store.stats().resident_bytes;
    heap.seal();
    for (std::size_t i = 0; i < handles.size(); ++i) {
        no::RequestHandle expected;
        { OracleWork oracle; expected = model.submit(1.0 + i, 50.0 + i); }
        handles[i] = s.submit(limit_order(1.0 + i, 50.0 + i));
        require(same_handle(handles[i], expected), "issued handle equals the input model");
        member(s.adapter(), SourceKeys{})[100 + i] = handles[i];
        { OracleWork oracle; model.source_keys[100 + i] = expected; }
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = "q";
        snapshot.requested_qty = 1.0 + i;
        s.remember(handles[i], std::move(snapshot));
    }
    auto& queued = member(s.adapter(), EntryQueue{});
    for (const char* label : {"intent-a", "intent-b"}) {
        queued.emplace_back();
        queued.back().request = limit_order(9.0, 40.0, label);
        queued.back().snapshot.source_id = label;
        queued.back().snapshot.requested_qty = 9.0;
        { OracleWork oracle; model.queued_labels.emplace_back(label); }
    }
    const auto old = handles[1];
    no::RequestHandle successor;
    { OracleWork oracle; successor = model.replace(old, 7.0, 70.0, false); }
    require(same_handle(s.replace(old, limit_order(7.0, 70.0), false), successor),
            "normal replace issues exactly one successor");
    s.retire_handle(old);
    ps::PlacementSnapshot successor_projection;
    successor_projection.source_id = "q";
    successor_projection.requested_qty = 7.0;
    s.remember(successor, std::move(successor_projection));
    member(s.adapter(), SourceKeys{})[101] = successor;
    { OracleWork oracle; model.source_keys[101] = successor; }
    { OracleWork oracle; model.replace(handles[0], 5.0, 80.0, true); }
    require(same_handle(s.replace(handles[0], limit_order(5.0, 80.0), true), handles[0]),
            "kept handle stays the same and moves behind the normal successor");
    ps::PlacementSnapshot kept_projection;
    kept_projection.source_id = "q";
    kept_projection.requested_qty = model.queue.back().units;
    s.remember(handles[0], std::move(kept_projection));
    check_book(s.core(), model);
    s.cancel(handles[2]); s.retire_handle(handles[2]);
    { OracleWork oracle; model.erase(handles[2]); model.source_keys.erase(102); }
    check_book(s.core(), model);

    // Execute the FIRST priority, independently chosen by the model. No source
    // instruction is submitted in order to make the native fill happen.
    const auto filled = model.queue.front().handle;
    const double units = model.queue.front().units;
    no::EvaluationContext evaluation;
    evaluation.cursor.point.ordinal = s.ord + 1;
    evaluation.cursor.point.effective_time_ms = 1000;
    evaluation.cursor.point.provenance = NativePriceProvenance::ObservedPrint;
    evaluation.driver_class = no::DriverEligibilityClass::ObservedPrint;
    evaluation.existing_matching_bit = true;
    no::TargetObservation observation;
    observation.current_position = no::PositionFlat{};
    const auto before_evaluation = s.core().history_end();
    auto prepared = s.core().prepare_evaluation(filled, evaluation, observation, s.ord);
    if (std::holds_alternative<no::PreparedMutation>(prepared)) {
        const auto installed = s.core().install_mutation(
            std::get<no::PreparedMutation>(std::move(prepared)));
        require(std::holds_alternative<no::Installed>(installed), "fill candidate evaluation installs");
        s.ord += std::get<no::Installed>(installed).events.count;
        s.note(before_evaluation);
    }
    no::ExecutionProposal proposal;
    proposal.cursor.point.ordinal = s.ord + 1;
    proposal.cursor.point.effective_time_ms = 1000;
    proposal.raw_price = 50.0;
    proposal.resolved_price = 50.0;
    proposal.physical_action = no::Transact{units};
    proposal.pre_fill = no::PositionFlat{};
    proposal.pre_target.current_position = no::PositionFlat{};
    proposal.inspected_opened_units = units;
    proposal.inspected_current_ticket = 1;
    no::CommittedExecutionFacts facts;
    facts.result.status = execution::Status::Applied;
    facts.result.opened_units = units;
    facts.result.current_ticket = 1;
    facts.cycle_after = 1;
    facts.post_target.current_position = no::PositionNonflat{1, no::Side::Long};
    facts.committed_action = no::Transact{units};
    const auto first = s.core().history_end();
    require(std::holds_alternative<no::Installed>(
                s.core().apply_execution(filled, proposal, facts, s.ord)), "native execution");
    s.note(first);
    s.retire_handle(filled);
    { OracleWork oracle;
      model.erase(filled);
      for (auto at = model.source_keys.begin(); at != model.source_keys.end();) {
          if (same_handle(at->second, filled)) at = model.source_keys.erase(at);
          else ++at;
      }
    }
    check_book(s.core(), model);
    require(s.core().find_live(old) == nullptr && s.core().find_live(filled) == nullptr,
            "neither projection nor queued intent becomes execution authority");
    require(queued.size() == model.queued_labels.size(), "source intent remains queued");
    for (std::size_t i = 0; i < queued.size(); ++i)
        require(queued[i].request.label == model.queued_labels[i], "queued intent order is unchanged");
    const auto& projection = member(s.adapter(), SourceLive{});
    require(projection.size() == model.queue.size(), "retire removes only the handle projection");
    require(member(s.adapter(), SourceKeys{}).size() == model.source_keys.size(),
            "source-key projection has no stale retired handle");
    for (const auto& key : model.source_keys)
        require(same_handle(member(s.adapter(), SourceKeys{}).at(key.first), key.second),
                "source-key projection retains its original meaning across replacement");
    for (const auto& row : model.queue)
        require(member(s.adapter(), Placements{}).at(row.handle.incarnation).requested_qty == row.units,
                "placement projection follows the current accepted definition");
    require(s.host.source_pending_view().size() == model.queue.size() + model.queued_labels.size(),
            "pending observer reads executable projection plus unsubmitted intent");
    require(store.stats().page_writes != 0 && store.record_count() != 0,
            "queue really uses the selected backing");
    rig.accounting(store);
    heap.fixed(store, resident);
}

// Fixed memory, S4-M (547). Input: independently grow Q, P and C, duplicate
// roster entries, churn kept and ordinary replacements/receipts, drain/reuse.
// fails to compile: the two Subject bind_scratch_store calls. The later
// assertion heap.fixed fails on resident definitions, source maps/vectors,
// roster buckets/positions and retained capacity, even if values are correct.
void s4_queue_and_membership_islands_fixed_red() {
    for (const unsigned axis : {0u, 1u, 2u}) {
        Model model;
        std::vector<no::RequestHandle> handles;
        { OracleWork oracle; handles.reserve(4096); }
        Rig rig;
        HeapCensus heap;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        const auto resident = store.stats().resident_bytes;
        const auto construction = global_allocation::live_bytes;
        require(construction >= resident, "store workspace is already in the heap census");
        // Four disjoint callback/reader pages. They are stack allocations and
        // counted once, not also added as heap workspace.
        std::array<std::array<std::byte, page_bytes>, 4> caller_pages{};
        const auto fixed_stack = sizeof(s) + sizeof(store) + sizeof(caller_pages);
        heap.seal();
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            for (std::size_t n : {std::size_t{17}, std::size_t{257}, std::size_t{4096}}) {
                while (handles.size() < n) {
                    const auto i = handles.size();
                    const auto actual = s.submit(limit_order(1.0, 50.0 + i % 13));
                    no::RequestHandle expected;
                    { OracleWork oracle;
                      expected = model.submit(1.0, 50.0 + i % 13);
                      handles.push_back(expected);
                    }
                    require(same_handle(actual, expected), "no capacity refusal on growing population");
                    const std::string id = axis == 2 ? std::to_string(i) : "one";
                    if (axis != 0) {
                        auto facts = s.cohort(id);
                        facts->second.origins.push_back(actual);
                        facts->second.origins.push_back(actual);
                        facts->second.opened.push_back(actual);
                        facts->second.live_units_by_origin[actual.incarnation] = 0.0;
                        s.core().cohort_add(facts->second.handle, actual);
                        s.core().cohort_add(facts->second.handle, actual);
                        s.cohort_note();
                        { OracleWork oracle;
                          auto& c = model.cohorts[id];
                          c.append(expected); c.append(expected);
                          c.opened.push_back(expected); c.units[expected.incarnation] = 0.0;
                          c.canonical.push_back(expected);
                        }
                    }
                    ps::PlacementSnapshot snapshot;
                    snapshot.source_id = "q";
                    snapshot.requested_qty = 1.0;
                    s.remember(actual, std::move(snapshot));
                    { OracleWork oracle;
                      model.placements[expected.incarnation].source_id = "q";
                      model.placements[expected.incarnation].requested_qty = 1.0;
                    }
                    if (axis == 1) {
                        // P alone grows: the executable queue is drained on
                        // every iteration, while source opened/lot facts stay.
                        PyramidEntry lot{};
                        lot.price = 100; lot.time = static_cast<std::int64_t>(i);
                        lot.qty = 1; lot.entry_incarnation = actual.incarnation;
                        s.host.pyramid_entries_.push_back(std::move(lot));
                        s.cancel(actual); s.retire_handle(actual);
                        { OracleWork oracle; model.erase(expected); }
                    } else if (axis == 2) {
                        // C alone grows; only one working row exists at once.
                        s.cancel(actual); s.retire_handle(actual);
                        { OracleWork oracle; model.erase(expected); }
                    }
                }
                if (axis == 0) {
                    const auto h = handles[n / 2];
                    const auto moved = s.replace(h, limit_order(3.0, 61.0), true);
                    { OracleWork oracle; model.replace(h, 3.0, 61.0, true); }
                    require(same_handle(h, moved), "repriced_ uses the kept handle");
                    member(s.adapter(), Placements{}).at(h.incarnation).requested_qty = 3.0;
                    { OracleWork oracle; model.placements.at(h.incarnation).requested_qty = 3.0; }
                    const auto ordinary_target = handles[n / 3];
                    const auto ordinary = s.replace(ordinary_target, limit_order(4.0, 62.0), false);
                    no::RequestHandle want;
                    { OracleWork oracle;
                      want = model.replace(ordinary_target, 4.0, 62.0, false);
                      handles[n / 3] = want;
                    }
                    require(same_handle(ordinary, want), "ordinary replacement changes only its chain row");
                    s.retire_handle(ordinary_target);
                    auto& placement = member(s.adapter(), Placements{});
                    if (placement.contains(ordinary_target.incarnation))
                        placement.erase(ordinary_target.incarnation);
                    ps::PlacementSnapshot replacement;
                    replacement.source_id = "q"; replacement.requested_qty = 4.0;
                    s.remember(ordinary, std::move(replacement));
                    { OracleWork oracle;
                      model.placements.erase(ordinary_target.incarnation);
                      model.placements[ordinary.incarnation].source_id = "q";
                      model.placements[ordinary.incarnation].requested_qty = 4.0;
                    }
                }
                check_book(s.core(), model);
                for (const auto& c : model.cohorts) check_roster(s, c.first, c.second);
                s.retire();
                // Production pending facade; do not charge an explicit owning
                // diagnostic snapshot as if it were a bounded production read.
                const auto& pending = s.adapter().pending_intent_view();
                for (int i = 0; i < pending.size(); ++i) {
                    pf_pending_order_v1_t row{};
                    require(pending.copy_v1(i, &row) >= 0, "production observer row reads successfully");
                    const auto expected = model.placements.find(row.incarnation);
                    require(expected != model.placements.end()
                                && row.qty == expected->second.requested_qty,
                            "production observer returns the input placement value");
                }
                rig.accounting(store);
                std::fprintf(stdout, "S4-M axis=%u cycle=%u n=%zu heap=%zu peak=%zu "
                             "stack=%zu resident=%llu backing=%llu\n", axis, cycle, n,
                             global_allocation::live_bytes, heap.peak, fixed_stack,
                             static_cast<unsigned long long>(resident),
                             static_cast<unsigned long long>(store.stats().backing_bytes));
            }
            for (const auto h : handles) {
                if (s.core().find_live(h)) s.cancel(h);
                s.retire_handle(h);
                auto& placement = member(s.adapter(), Placements{});
                if (placement.contains(h.incarnation)) placement.erase(h.incarnation);
            }
            for (auto& c : member(s.adapter(), Cohorts{})) {
                for (const auto h : c.second.opened) s.core().cohort_remove(c.second.handle, h);
                c.second.opened.clear(); c.second.live_units_by_origin.clear();
            }
            s.cohort_note();
            s.host.pyramid_entries_.clear();
            s.retire();
            { OracleWork oracle;
              model.queue.clear();
              model.placements.clear();
              for (auto& c : model.cohorts) {
                  c.second.opened.clear(); c.second.units.clear(); c.second.canonical.clear();
                  model.retire(c.second);
              }
              handles.clear();
            }
            check_book(s.core(), model);
            for (const auto& c : model.cohorts) check_roster(s, c.first, c.second);
            rig.accounting(store);
        }
        require(store.stats().backing_pages > rig.config().page_count,
                "population overflows resident geometry into backing");
        heap.fixed(store, resident);
    }
}

// Fixed memory, S4-RE (548). Input: P physical lots, two successive terminal
// marks, row(index) reads in lot order, clear, then another mark; nonfinite
// mark refusal. fails to compile: range_end_trades_.row(index), in addition
// to the store binding. With the old vector reader substituted, peak <=
// construction_peak fails in append_open_position_report_rows itself (9048),
// before any C accessor. OpenReportRowSource/materialized_ do not exist here.
void s4_terminal_mark_descriptor_stays_fixed_red() {
    struct Expected {
        std::int64_t time;
        std::uint64_t incarnation;
        double price, qty, entry_fee;
        int entry_bar;
        double favorable, adverse;
        std::string label, comment;
    };
    for (std::size_t population : {std::size_t{19}, std::size_t{513}, std::size_t{4096}}) {
        std::vector<Expected> model;
        Host host;
        host.commission_type_ = CommissionType::PERCENT;
        host.commission_value_ = 0.0;
        host.set_syminfo_pointvalue(1.0);
        host.position_side_ = PositionSide::LONG;
        // The physical book is the producer's INPUT. Provision it before the
        // producer census: this row measures the terminal descriptor/reader,
        // not a second test of the lot storage covered by S4-M.
        for (std::size_t i = 0; i < population; ++i) {
            const std::array<double, 4> prices{{96.0, 99.0, 104.0, 97.0}};
            const double price = prices[i % prices.size()];
            const double qty = i % 2 == 0 ? std::ldexp(1.0, 52) : 1.0;
            // At mark 100 a quartet contributes [2^54, 1, -2^54, 3].
            // Its left fold differs from a cancellation-first/pairwise sum.
            Expected e{1000 + static_cast<std::int64_t>(i), 10 + i, price, qty, 0.0,
                       static_cast<int>(i % 11), 1.0e100, 2.0e100,
                       "lot-" + std::to_string(i), "entry-" + std::to_string(i)};
            model.push_back(e);
            PyramidEntry lot{};
            lot.time = e.time; lot.entry_incarnation = e.incarnation;
            lot.price = e.price; lot.qty = e.qty;
            lot.entry_id = e.label; lot.entry_comment = e.comment;
            lot.entry_commission_account = e.entry_fee;
            lot.entry_bar_index = e.entry_bar;
            lot.max_runup = e.favorable; lot.max_drawdown = e.adverse;
            host.position_qty_ += e.qty;
            host.pyramid_entries_.push_back(std::move(lot));
        }
        Rig rig;
        HeapCensus heap;
        ScratchStore store(rig.config(), &rig.backing);
        host.core().bind_scratch_store(store);
        const auto resident = store.stats().resident_bytes;
        heap.seal();
        const std::array<double, 3> marks{{100.0, 100.5, 99.5}};
        for (std::size_t pass = 0; pass < marks.size(); ++pass) {
            const double mark = marks[pass];
            // Two terminal descriptor updates, then clear and update again.
            // A second mark must not leave a second P-row resident cache.
            if (pass == 2) host.range_end_trades_.clear();
            double expected_sum = 0.0; // preserve the existing LEFT fold
            for (const auto& lot : model) expected_sum += (mark - lot.price) * lot.qty;
            const auto actual_sum = host.consumer().append_open_position_report_rows(
                host, mark, 9000, 11, 1.0);
            require(actual_sum == expected_sum, "terminal P&L equals independent left fold");
            require(host.range_end_trades_.size() == model.size(), "one terminal row per input lot");
            for (std::size_t i = 0; i < model.size(); ++i) {
                const auto& e = model[i];
                const auto& row = host.range_end_trades_.row(i);
                const double pnl = (mark - e.price) * e.qty;
                require(row.entry_time == e.time && row.exit_time == 9000,
                        "terminal row times follow the input and mark");
                require(row.entry_incarnation == e.incarnation && row.qty == e.qty,
                        "lot identity, quantity and order survive remark");
                require(row.entry_price == e.price && row.exit_price == mark && row.pnl == pnl,
                        "row economics equal the independent formula");
                require(row.entry_id == e.label && row.entry_comment == e.comment,
                        "row text survives without a P-row cache");
                require(row.entry_bar_index == e.entry_bar && row.exit_bar_index == 11,
                        "row bar indexes are input and mark coordinates");
                require(row.pnl_pct == pnl / (e.price * e.qty) * 100.0,
                        "percentage P&L uses this physical lot's entry cost");
                // Full lot, zero fees, and pre-existing excursion maxima
                // greater than every candidate mark's excursion: preserve
                // those economic inputs, without calling the trade builder.
                require(row.max_runup == e.favorable && row.max_drawdown == e.adverse,
                        "full-lot historical excursion values survive remark");
                require(row.exit_comment.empty() && row.exit_id.empty()
                            && !row.exit_from_bracket
                            && row.close_cause == execution::CloseCause::Unspecified,
                        "a report mark has no source exit instruction or bracket cause");
                require(row.is_long && row.open_at_end && row.commission == e.entry_fee,
                        "terminal flags and costs match the input book");
            }
            rig.accounting(store);
        }
        host.range_end_trades_.clear();
        bool nonfinite_refused = false;
        try {
            (void)host.consumer().append_open_position_report_rows(
                host, std::numeric_limits<double>::infinity(), 9000, 11, 1.0);
        } catch (const std::logic_error&) { nonfinite_refused = true; }
        require(nonfinite_refused, "nonfinite terminal row retains the existing refusal");
        heap.fixed(store, resident);
    }
}

// Transactions and retain windows, S34-T (588). Input: replace/cancel/submit
// record+lookup updates, fault EACH attempted backing write through put, erase
// and commit, tear writes, fail rollback, then reopen the SAME backing.
// fails to compile: Subject's bind_scratch_store/rebind. The concrete missing
// guarantee is that a failed command leaves old queue/handle/cohort relations
// and committed digests recoverable; an unattached store has zero fault hits.
// Counter values after a failed native command are deliberately not rewound
// by this oracle: their existing refusal semantics are outside this clause.
void s34_record_index_publication_rollback_red() {
    for (const unsigned mutation : {0u, 1u, 2u}) {
        std::uint64_t write_count = 0;
        bool failed_rollback_witnessed = false;
        // First calibrate a successful transition. The subsequent sweep
        // targets every write, with no arbitrary upper cutoff or skipped nth.
        for (std::uint64_t nth = 0; nth <= write_count; ++nth) {
            Model before;
            Rig rig;
            HeapCensus heap;
            auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
            Subject s(*store);
            const auto resident = store->stats().resident_bytes;
            heap.seal();
            for (std::size_t i = 0; i < 37; ++i) {
                no::RequestHandle expected;
                { OracleWork oracle; expected = before.submit(1.0, 40.0 + i % 7); }
                require(same_handle(s.submit(limit_order(1.0, 40.0 + i % 7)), expected),
                        "transaction baseline follows the model");
            }
            const auto target = before.queue[9].handle;
            const auto cohort = s.cohort("tx")->second.handle;
            s.core().cohort_add(cohort, target); s.cohort_note();
            { OracleWork oracle; before.cohorts["tx"].canonical.push_back(target); }
            const auto old_digest = store_digest(*store);
            const auto old_native = s.host.consumer().continuation_hash();
            const auto writes_before = rig.charges.writes;
            const auto hits_before = rig.backing.hits;
            if (nth != 0) rig.backing.arm(ScratchIoOperation::write_page, nth, nth % 2 == 0);
            bool io_refused = false;
            try {
                if (mutation == 0) (void)s.replace(target, limit_order(5.0, 63.0), false);
                else if (mutation == 1) s.cancel(target);
                else (void)s.submit(limit_order(7.0, 64.0));
            } catch (const ScratchIoError& error) {
                io_refused = true;
                require(classify_run_failure(error).code == RunFailureCode::scratch_io_failed,
                        "medium failure reaches the typed scratch I/O failure");
            }
            if (nth == 0) {
                require(!io_refused, "calibration command succeeds");
                write_count = rig.charges.writes - writes_before;
                require(write_count != 0, "consumer record/index update reaches scratch");
                rig.accounting(*store);
                heap.fixed(*store, resident);
                continue;
            }
            require(io_refused && rig.backing.hits == hits_before + 1,
                    "each injected write really fails its logical update");
            require(store->transaction_active(), "failed update is rollback-only, not published");
            ScratchRecordInfo info{};
            status_is(store->point(1, info), ScratchStatus::programming_error,
                      "rollback-only cannot be observed as empty membership");
            status_is(store->begin(), ScratchStatus::programming_error, "only rollback may proceed");
            status_is(store->erase(1), ScratchStatus::programming_error, "erase cannot retry the update");
            status_is(store->commit(ScratchDurability::buffered), ScratchStatus::programming_error,
                      "commit cannot publish a partial relation");
            // Force cleanup itself to fail. This is not a successful rollback,
            // and its failure must not disappear behind a destructor/retry.
            const auto rollback_hits = rig.backing.hits;
            rig.backing.arm(ScratchIoOperation::write_page, 1);
            const auto rollback_status = store->rollback();
            if (rig.backing.hits != rollback_hits) {
                failed_rollback_witnessed = true;
                status_is(rollback_status, ScratchStatus::io_error, "failed rollback is reported");
                require(store->transaction_active(), "failed cleanup leaves rollback-only state");
                rig.backing.disarm();
                status_is(store->rollback(), ScratchStatus::ok, "explicit resumed rollback");
            } else {
                // A fault before the first dirty page can leave no undo I/O.
                // Do not require a gratuitous backing write just to hit a hook.
                status_is(rollback_status, ScratchStatus::ok, "empty undo can roll back without I/O");
                rig.backing.disarm();
            }
            check_book(s.core(), before);
            require(s.core().cohort_contains(cohort, target)
                        == before.native_member(before.cohorts.at("tx"), target),
                    "cohort lookup still names the old committed handle");
            require(store_digest(*store) == old_digest, "rollback preserves committed records/handles");
            require(s.host.consumer().continuation_hash() == old_native,
                    "no failed record/index publication contributes semantic hash bytes");
            rig.accounting(*store);
            store.reset();
            const auto open_charges = rig.charges;
            store = std::make_unique<ScratchStore>(
                rig.config(ScratchOpenMode::reopen), &rig.backing);
            status_is(store->status(), ScratchStatus::ok, "same live backing reopens");
            s.rebind(*store);
            check_book(s.core(), before);
            require(store_digest(*store) == old_digest, "reopen recovers the old committed relation");
            require(s.core().cohort_contains(cohort, target), "reopened index is not silently empty");
            rig.accounting(*store, open_charges);
            heap.fixed(*store, resident);
        }
        require(failed_rollback_witnessed, "the sweep reached a rollback with real restoration I/O");
    }
}

// The current-membership indexes and the five predicates, S4-P1 (633).
// Input: terminal native origin with a retained source placement, then actual
// erasure. Other pins false. fails to compile: Subject's store bindings and
// OriginRoster::visit_positions_of. The value truth is already present on the
// base; the missing bounded placement/index binding is this row's red seam.
void s4_predicate_retained_placement_red() {
    Model model;
    auto origin = model.submit(2.0, 45.0);
    model.erase(origin);
    ps::PlacementSnapshot evidence;
    evidence.source_id = "retained";
    evidence.requested_qty = 2.0;
    model.placements.emplace(origin.incarnation, evidence);
    auto& expected = model.cohorts["p1"];
    expected.append(origin);
    expected.append({model.run, origin.incarnation + 1000});
    Rig rig;
    HeapCensus heap;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    const auto live = s.submit(limit_order(2.0, 45.0));
    require(same_handle(live, origin), "fixture uses a genuinely issued origin");
    s.cancel(live);
    s.append("p1", origin);
    s.append("p1", expected.origins.back());
    member(s.adapter(), Placements{}).try_emplace(origin.incarnation, evidence);
    require(s.core().find_live(origin) == nullptr, "native order ended");
    require(member(s.adapter(), SourceLive{}).empty(), "no live source name");
    isolated(model, expected, origin, 0);
    s.retire(); model.retire(expected);
    check_roster(s, "p1", expected);
    require(member(s.adapter(), Placements{}).erase(origin.incarnation), "actual placement erasure");
    { OracleWork oracle; model.placements.erase(origin.incarnation); model.retire(expected); }
    s.retire();
    check_roster(s, "p1", expected);
    require(expected.base == expected.origins.size(), "removing the sole pin releases the suffix");
    rig.accounting(store);
}

// The current-membership indexes and the five predicates, S4-P3 (635).
// Input: same origin in two cohorts, ZERO-valued present keys, erase one key
// only. Other pins false. fails to compile: Subject store bindings and
// OriginRoster::visit_positions_of. A resident member map must be replaced,
// not changed into units > 0; expected bases below follow key presence alone.
void s4_predicate_live_units_key_presence_red() {
    Model model;
    const auto origin = model.submit(1.0, 43.0);
    model.erase(origin);
    for (const char* id : {"alpha", "beta"}) {
        auto& c = model.cohorts[id]; c.append(origin); c.units[origin.incarnation] = 0.0;
    }
    Rig rig;
    HeapCensus heap;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    const auto real = s.submit(limit_order(1.0, 43.0));
    require(same_handle(real, origin), "issued input handle");
    s.cancel(real);
    for (const char* id : {"alpha", "beta"}) {
        s.append(id, origin);
        auto facts = s.cohort(id);
        facts->second.live_units_by_origin[origin.incarnation] = 0.0;
        isolated(model, model.cohorts.at(id), origin, 2);
    }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    s.cohort("alpha")->second.live_units_by_origin.erase(origin.incarnation);
    { OracleWork oracle; model.cohorts.at("alpha").units.erase(origin.incarnation); }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    require(model.cohorts.at("alpha").base != model.cohorts.at("beta").base,
            "erasing alpha cannot unpin beta's present zero key");
    s.cohort("beta")->second.live_units_by_origin.erase(origin.incarnation);
    { OracleWork oracle; model.cohorts.at("beta").units.erase(origin.incarnation); }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    rig.accounting(store);
}

// The current-membership indexes and the five predicates, S4-P4 (636).
// Input: duplicate opened handles, same incarnation in different run numbers
// AND different session text, remove duplicates singly. Other four pins false.
// fails to compile: Subject bindings and OriginRoster::visit_positions_of.
// The comparison must retain full-handle identity and ordered multiplicity,
// rather than making an incarnation-only membership index.
void s4_predicate_opened_full_handle_red() {
    Model model;
    const auto a = model.submit(1.0, 42.0);
    model.erase(a);
    const no::RequestHandle b{run_id(8), a.incarnation};
    const std::string long_session(8193, 'x');
    const no::RequestHandle c{{long_session, a.run.run_number}, a.incarnation};
    auto& expected = model.cohorts["opened"];
    // The foreign row comes FIRST: incarnation-only equality would pin it
    // and hide the correct frontier at the following real member.
    expected.append(b); expected.append(c); expected.append(a); expected.append(a);
    expected.opened = {a, a};
    Rig rig;
    HeapCensus heap;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    const auto real = s.submit(limit_order(1.0, 42.0));
    require(same_handle(real, a), "issued input handle");
    s.cancel(real);
    for (auto h : expected.origins) s.append("opened", h);
    auto facts = s.cohort("opened");
    facts->second.opened = expected.opened;
    isolated(model, expected, a, 3);
    for (unsigned remaining = 2; remaining != 0; --remaining) {
        s.retire(); model.retire(expected);
        check_roster(s, "opened", expected);
        require(expected.opened.size() == remaining, "oracle retains duplicate multiplicity");
        facts->second.opened.erase(facts->second.opened.begin());
        { OracleWork oracle; remove_one(expected.opened, a); }
        require(facts->second.opened.size() == expected.opened.size(), "one duplicate removed at a time");
    }
    s.retire(); model.retire(expected);
    check_roster(s, "opened", expected);
    require(expected.base == expected.origins.size(), "last full-handle member unpins the prefix");
    rig.accounting(store);
}

// The current-membership indexes and the five predicates, S4-P5 (637).
// Input: canonical cohort, long normal/kept replacement chain, live and
// terminal successors after history retirement, unknown and foreign handles.
// Only P5 pins during retirement (all successors have ended by then).
// fails to compile: Subject bindings and OriginRoster::visit_positions_of.
// Root reads must use the selected store and propagate I/O, never false/self.
void s4_predicate_native_canonical_cohort_red() {
    Model model;
    std::vector<no::RequestHandle> successors;
    const auto root = model.submit(1.0, 41.0);
    auto& expected = model.cohorts["canonical"];
    expected.canonical.push_back(root);
    Rig rig;
    HeapCensus heap;
    auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
    Subject s(*store);
    auto current = s.submit(limit_order(1.0, 41.0));
    require(same_handle(current, root), "real root matches model");
    const auto cohort = s.cohort("canonical")->second.handle;
    s.core().cohort_add(cohort, current); s.cohort_note();
    for (std::size_t i = 0; i < 257; ++i) {
        const bool keep = i % 3 == 1;
        no::RequestHandle want;
        { OracleWork oracle;
          want = model.replace(current, 1.0, 41.0 + i % 5, keep);
          successors.push_back(want);
        }
        current = s.replace(current, limit_order(1.0, 41.0 + i % 5), keep);
        require(same_handle(current, want), "successor or kept identity follows input commands");
        require(s.core().cohort_contains(cohort, current) == model.native_member(expected, want),
                "live successor normalizes to canonical root");
    }
    s.cancel(current);
    { OracleWork oracle; model.erase(current); }
    s.core().retire_history(s.ord);
    require(s.core().history_base() != 0, "terminal membership is not a journal-tail scan");
    const no::RequestHandle unknown{model.run, model.next_incarnation + 1000};
    const no::RequestHandle foreign{run_id(8), root.incarnation};
    { OracleWork oracle;
      expected.append(unknown); expected.append(foreign); expected.append(root);
      for (auto h : successors) expected.append(h);
    }
    for (auto h : expected.origins) s.append("canonical", h);
    isolated(model, expected, root, 4);
    require(!s.core().cohort_contains(cohort, unknown)
                && !s.core().cohort_contains(cohort, foreign), "unknown/foreign are proven absence");
    for (auto h : successors)
        require(s.core().cohort_contains(cohort, h) == model.native_member(expected, h),
                "terminal successor is still a member after history retirement");
    s.retire(); model.retire(expected);
    check_roster(s, "canonical", expected);
    // Orderly reopen discards resident store pages before faulting a terminal
    // successor-root read. The owner remains run-owned, not process-persisted.
    store.reset();
    const auto open_charges = rig.charges;
    store = std::make_unique<ScratchStore>(rig.config(ScratchOpenMode::reopen), &rig.backing);
    s.rebind(*store);
    const auto hits = rig.backing.hits;
    rig.backing.arm(ScratchIoOperation::read_page, 1);
    bool failed = false;
    try { (void)s.core().cohort_contains(cohort, successors.front()); }
    catch (const ScratchIoError&) { failed = true; }
    require(failed && rig.backing.hits == hits + 1, "root I/O cannot become false or self-root");
    rig.backing.disarm();
    s.core().cohort_remove(cohort, successors.back()); s.cohort_note();
    { OracleWork oracle; expected.canonical.clear(); model.retire(expected); }
    s.retire();
    check_roster(s, "canonical", expected);
    rig.accounting(*store, open_charges);
}

enum class NameField {
    self, bracket, paired, preserved, predecessor, legs, reservation,
    wait, opening, openings_first, openings_last, root, anchored_handle, anchored_parent
};
enum class NameOwner {
    source_live, source_snapshot, native_live, native_snapshot, native_request, native_root,
    bracket, entry, delayed, sell, rearm, same_bar, shadow, coof, revival, anchored
};
struct NameCase { NameOwner owner; NameField field; };

void snapshot_name(ps::PlacementSnapshot& row, NameField field, no::RequestHandle h) {
    switch (field) {
    case NameField::bracket: row.bracket_origin = h; break;
    case NameField::paired: row.paired_reversal_parent = h; break;
    case NameField::preserved: row.preserved_by_close_all = h; break;
    case NameField::predecessor: row.projection_predecessor = h.incarnation; break;
    case NameField::legs:
        if (h.incarnation != 0) row.legs.attach(h.incarnation, 1);
        break;
    case NameField::reservation: row.reservation_growth_owner_incarnation = h.incarnation; break;
    default: break;
    }
}
void request_name(no::Request& row, NameField field, no::RequestHandle h) {
    switch (field) {
    case NameField::wait: row.owner = no::WaitForApplied{h}; break;
    case NameField::opening:
        row.intent = no::Reduce{no::ExplicitUnits{1}};
        row.owner = no::BindOpening{h, 1};
        break;
    case NameField::openings_first:
    case NameField::openings_last: {
        row.intent = no::Reduce{no::ExplicitUnits{1}};
        no::BindOpenings owners;
        owners.cycle = 1;
        // A long QUEUED input, including zero and names unrelated to the
        // target. Only one end names h; checking only front() cannot pass.
        for (std::uint64_t i = 0; i < 1025; ++i)
            owners.openings.push_back({h.run, h.incarnation + 10000 + i});
        owners.openings[field == NameField::openings_first ? 0 : owners.openings.size() - 1] = h;
        row.owner = std::move(owners);
        break;
    }
    default: break;
    }
}
template<class Queue>
void queued_name(Queue& q, NameField field, no::RequestHandle h) {
    q.emplace_back();
    q.back().request = limit_order(1.0, 40.0);
    snapshot_name(q.back().snapshot, field, h);
    request_name(q.back().request, field, h);
}
template<class Queue>
void snapshot_queue_name(Queue& q, NameField field, no::RequestHandle h) {
    q.emplace_back();
    snapshot_name(q.back().snapshot, field, h);
}

struct InstalledName {
    NameOwner owner;
    no::RequestHandle carrier{};
};

InstalledName install_name(Subject& s, NameCase input, no::RequestHandle origin) {
    const auto field = input.field;
    const auto owner = input.owner;
    InstalledName result{owner, {}};
    switch (owner) {
    case NameOwner::source_live:
        member(s.adapter(), SourceLive{}).push_back(origin);
        break;
    case NameOwner::native_live:
        result.carrier = origin; // submitted by the test; remains live
        break;
    case NameOwner::native_root:
        result.carrier = s.replace(origin, limit_order(1.0, 46.0), false);
        break;
    case NameOwner::source_snapshot:
    case NameOwner::native_snapshot: {
        result.carrier = s.submit(limit_order(1.0, 47.0));
        ps::PlacementSnapshot row;
        snapshot_name(row, field, origin);
        member(s.adapter(), Placements{}).try_emplace(result.carrier.incarnation, std::move(row));
        if (owner == NameOwner::source_snapshot) {
            member(s.adapter(), SourceLive{}).push_back(result.carrier);
            s.cancel(result.carrier); // ensure ONLY the source enumeration can see it
        }
        break;
    }
    case NameOwner::native_request: {
        no::Request request = limit_order(1.0, 48.0);
        request_name(request, field, origin);
        no::CommandContext context;
        context.opening = no::OpeningObservation{origin, 1, no::PositionNonflat{1, no::Side::Long}, true};
        if (auto* owners = std::get_if<no::BindOpenings>(&request.owner)) {
            // Native enrollment has the existing unique, nonzero membership
            // rules. Exercise each position with real issued handles; the
            // arbitrarily long queued form above has no enrollment yet.
            const auto other = s.submit(limit_order(1.0, 49.0));
            owners->openings = field == NameField::openings_first
                ? std::vector<no::RequestHandle>{origin, other}
                : std::vector<no::RequestHandle>{other, origin};
            for (auto h : owners->openings)
                context.openings.push_back({h, 1, no::PositionNonflat{1, no::Side::Long}, true});
            s.cancel(other);
        }
        const auto first = s.core().history_end();
        const auto out = s.core().apply_submit(request, context, s.inc, s.ord);
        require(out.result.status == no::SubmitStatus::Accepted && out.result.handle.has_value(),
                "owner-bearing native request enrolled through the real command");
        result.carrier = *out.result.handle;
        s.note(first);
        break;
    }
    case NameOwner::bracket:
        queued_name(member(s.adapter(), BracketQueue{}), field, origin); break;
    case NameOwner::entry:
        queued_name(member(s.adapter(), EntryQueue{}), field, origin); break;
    case NameOwner::delayed:
        queued_name(member(s.adapter(), DelayedQueue{}), field, origin); break;
    case NameOwner::sell:
        snapshot_queue_name(member(s.adapter(), SellQueue{}), field, origin); break;
    case NameOwner::rearm:
        member(s.adapter(), RearmQueue{}).emplace_back();
        snapshot_name(member(s.adapter(), RearmQueue{}).back(), field, origin); break;
    case NameOwner::same_bar:
        queued_name(member(s.adapter(), SameBarQueue{}), field, origin); break;
    case NameOwner::shadow:
        snapshot_queue_name(member(s.adapter(), ShadowQueue{}), field, origin); break;
    case NameOwner::coof:
        queued_name(member(s.adapter(), CoofQueue{}), field, origin); break;
    case NameOwner::revival:
        snapshot_queue_name(member(s.adapter(), RevivalQueue{}), field, origin); break;
    case NameOwner::anchored: {
        auto& q = member(s.adapter(), AnchoredQueue{});
        q.emplace_back();
        q.back().request = limit_order(1.0, 44.0);
        if (field == NameField::anchored_handle) q.back().handle = origin;
        else if (field == NameField::anchored_parent) q.back().parent = origin;
        else request_name(q.back().request, field, origin);
        break;
    }
    }
    return result;
}

void erase_name(Subject& s, InstalledName name) {
    switch (name.owner) {
    case NameOwner::source_live:
        member(s.adapter(), SourceLive{}).clear(); break;
    case NameOwner::source_snapshot:
    case NameOwner::native_snapshot:
        member(s.adapter(), Placements{}).erase(name.carrier.incarnation);
        s.retire_handle(name.carrier);
        if (s.core().find_live(name.carrier)) s.cancel(name.carrier);
        break;
    case NameOwner::native_live:
    case NameOwner::native_root:
    case NameOwner::native_request:
        s.cancel(name.carrier); break;
    case NameOwner::bracket: member(s.adapter(), BracketQueue{}).clear(); break;
    case NameOwner::entry: member(s.adapter(), EntryQueue{}).clear(); break;
    case NameOwner::delayed: member(s.adapter(), DelayedQueue{}).clear(); break;
    case NameOwner::sell: member(s.adapter(), SellQueue{}).clear(); break;
    case NameOwner::rearm: member(s.adapter(), RearmQueue{}).clear(); break;
    case NameOwner::same_bar: member(s.adapter(), SameBarQueue{}).clear(); break;
    case NameOwner::shadow: member(s.adapter(), ShadowQueue{}).clear(); break;
    case NameOwner::coof: member(s.adapter(), CoofQueue{}).clear(); break;
    case NameOwner::revival: member(s.adapter(), RevivalQueue{}).clear(); break;
    case NameOwner::anchored: member(s.adapter(), AnchoredQueue{}).clear(); break;
    }
}

// The current-membership indexes and the five predicates, S4-P2 (634).
// Input: EVERY enumerated current owner, each snapshot name field, all three
// request-owner variants/every BindOpenings position, native roots and zero.
// Two owners name the same otherwise released origin; remove in BOTH orders.
// The other four disjuncts are false throughout each true-pin phase.
// fails to compile: Subject bindings and OriginRoster::visit_positions_of.
// Correct truth already exists for resident inputs; no bounded named workset
// is connected to the store. Missing external BindOpenings enumeration is
// additionally exposed if enrollment freezes that vector on this head.
void s4_predicate_named_by_each_current_owner_red() {
    std::vector<NameCase> inputs;
    const std::array<NameField, 6> snapshot_fields{{
        NameField::bracket, NameField::paired, NameField::preserved,
        NameField::predecessor, NameField::legs, NameField::reservation}};
    const std::array<NameField, 4> request_fields{{
        NameField::wait, NameField::opening, NameField::openings_first, NameField::openings_last}};
    inputs.push_back({NameOwner::source_live, NameField::self});
    inputs.push_back({NameOwner::native_live, NameField::self});
    inputs.push_back({NameOwner::native_root, NameField::root});
    for (const auto owner : {NameOwner::source_snapshot, NameOwner::native_snapshot,
                            NameOwner::bracket, NameOwner::entry, NameOwner::delayed,
                            NameOwner::sell, NameOwner::rearm, NameOwner::same_bar,
                            NameOwner::shadow, NameOwner::coof, NameOwner::revival})
        for (const auto field : snapshot_fields) inputs.push_back({owner, field});
    for (const auto owner : {NameOwner::native_request, NameOwner::bracket, NameOwner::entry,
                            NameOwner::delayed, NameOwner::same_bar, NameOwner::coof,
                            NameOwner::anchored})
        for (const auto field : request_fields) inputs.push_back({owner, field});
    inputs.push_back({NameOwner::anchored, NameField::anchored_handle});
    inputs.push_back({NameOwner::anchored, NameField::anchored_parent});
    for (const auto input : inputs) for (const bool primary_first : {false, true}) {
        Model model;
        const auto origin = model.submit(1.0, 40.0);
        model.erase(origin);
        auto& expected = model.cohorts["named"];
        expected.append(origin);
        expected.append({model.run, origin.incarnation + 9000});
        model.names[origin.incarnation] = 2; // independent multiset, not a boolean cache
        Rig rig;
        HeapCensus heap;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        require(same_handle(s.submit(limit_order(1.0, 40.0)), origin), "issued named input");
        const auto primary = install_name(s, input, origin);
        if (input.owner != NameOwner::native_live && input.owner != NameOwner::native_root)
            s.cancel(origin);
        // Use a different queue for the second reference so clearing either
        // owner exercises multiplicity across owners, not only duplicate rows.
        const NameCase second{input.owner == NameOwner::rearm ? NameOwner::entry : NameOwner::rearm,
                              NameField::bracket};
        const auto secondary = install_name(s, second, origin);
        for (auto h : expected.origins) s.append("named", h);
        for (unsigned phase = 0; phase < 3; ++phase) {
            if (phase < 2) isolated(model, expected, origin, 1);
            s.retire(); model.retire(expected);
            check_roster(s, "named", expected);
            if (phase == 2) break;
            erase_name(s, (phase == 0) == primary_first ? primary : secondary);
            { OracleWork oracle; --model.names.at(origin.incarnation); }
        }
        require(expected.base == expected.origins.size(), "last current owner releases its name");
        rig.accounting(store);
    }
    // All explicit names zero: zero is excluded, even when a queue physically
    // contains a row. It must not pin a zero-incarnation roster position.
    Model zero;
    const no::RequestHandle h{zero.run, 0};
    auto& expected = zero.cohorts["zero"];
    expected.append(h);
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    s.append("zero", h);
    (void)install_name(s, {NameOwner::entry, NameField::bracket}, h);
    (void)install_name(s, {NameOwner::anchored, NameField::anchored_parent}, h);
    s.retire(); zero.retire(expected);
    check_roster(s, "zero", expected);
}

// The current-membership indexes and the five predicates, S4-I (638).
// Input: all 32 truth combinations, duplicate global positions/full-handle
// aliases, pin at position 1 before a large suffix, release, replace/erase,
// spill and run reset. fails to compile: Subject bindings and
// OriginRoster::visit_positions_of. The exact assertion is global frontier/
// position equality PLUS the construction heap bound behind the pinned suffix.
void s4_five_pin_frontier_and_indexes_red() {
    for (unsigned mask = 0; mask != 32; ++mask) {
        Model model;
        const auto pin = model.submit(1.0, 39.0);
        model.erase(pin);
        auto& c = model.cohorts["frontier"];
        c.append({run_id(9), pin.incarnation + 1000}); // unpinned front
        c.append(pin);
        c.append(pin); // duplicate positions are not a set
        for (std::uint64_t i = 0; i < 4096; ++i)
            c.append({model.run, 10000 + i});
        if (mask & 1) model.placements.emplace(pin.incarnation, ps::PlacementSnapshot{});
        if (mask & 2) model.names[pin.incarnation] = 1;
        if (mask & 4) c.units[pin.incarnation] = 0.0;
        if (mask & 8) c.opened = {pin, pin};
        if (mask & 16) c.canonical.push_back(pin);
        Rig rig;
        HeapCensus heap;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        const auto resident = store.stats().resident_bytes;
        heap.seal();
        const auto real = s.submit(limit_order(1.0, 39.0));
        require(same_handle(real, pin), "issued frontier pin");
        const auto cohort = s.cohort("frontier")->second.handle;
        if (mask & 16) { s.core().cohort_add(cohort, real); s.cohort_note(); }
        s.cancel(real);
        for (auto h : c.origins) s.append("frontier", h);
        if (mask & 1) member(s.adapter(), Placements{}).try_emplace(pin.incarnation);
        if (mask & 2) (void)install_name(s, {NameOwner::entry, NameField::bracket}, pin);
        auto facts = s.cohort("frontier");
        if (mask & 4) facts->second.live_units_by_origin[pin.incarnation] = 0.0;
        if (mask & 8) { facts->second.opened.push_back(pin); facts->second.opened.push_back(pin); }
        s.retire(); model.retire(c);
        check_roster(s, "frontier", c);
        if (mask != 0) {
            require(c.origins.size() - c.base > 4096, "a front pin retains the large suffix");
            // Remove one pin at a time: a later disjunct still pins, even
            // when a derived cache was last populated under an earlier one.
            for (unsigned bit = 0; bit < 5; ++bit) {
                if (!(mask & (1u << bit))) continue;
                if (bit == 0) {
                    member(s.adapter(), Placements{}).erase(pin.incarnation);
                    OracleWork oracle; model.placements.erase(pin.incarnation);
                } else if (bit == 1) {
                    member(s.adapter(), EntryQueue{}).clear();
                    OracleWork oracle; model.names.clear();
                } else if (bit == 2) {
                    facts->second.live_units_by_origin.erase(pin.incarnation);
                    OracleWork oracle; c.units.clear();
                } else if (bit == 3) {
                    facts->second.opened.erase(facts->second.opened.begin());
                    { OracleWork oracle; remove_one(c.opened, pin); }
                    s.retire(); model.retire(c); check_roster(s, "frontier", c);
                    facts->second.opened.clear();
                    OracleWork oracle; c.opened.clear();
                } else {
                    s.core().cohort_remove(cohort, pin); s.cohort_note();
                    OracleWork oracle; c.canonical.clear();
                }
                s.retire(); model.retire(c); check_roster(s, "frontier", c);
            }
        }
        const auto count = c.origins.size();
        require(c.base == count, "all-false prefix reaches the end");
        // Reinsert after a global nonzero base and replace a placement value.
        const no::RequestHandle reused{model.run, 20000};
        s.append("frontier", reused);
        { OracleWork oracle; c.append(reused); c.opened.push_back(reused); }
        facts->second.opened.push_back(reused);
        ps::PlacementSnapshot replacement;
        replacement.requested_qty = 3.0 + mask;
        { OracleWork oracle; model.placements[reused.incarnation] = replacement; }
        member(s.adapter(), Placements{}).try_emplace(reused.incarnation);
        member(s.adapter(), Placements{}).replace(reused.incarnation, std::move(replacement));
        require(member(s.adapter(), Placements{}).at(reused.incarnation).requested_qty
                    == model.placements.at(reused.incarnation).requested_qty,
                "replacement updates the existing lookup row");
        member(s.adapter(), Placements{}).erase(reused.incarnation);
        { OracleWork oracle; model.placements.erase(reused.incarnation); }
        s.retire(); model.retire(c); check_roster(s, "frontier", c);
        facts->second.opened.clear();
        { OracleWork oracle; c.opened.clear(); model.retire(c); }
        s.retire(); check_roster(s, "frontier", c);
        rig.accounting(store);
        require(store.stats().backing_pages > rig.config().page_count,
                "suffix/index records spilled beyond the resident slots");
        s.adapter().reset_for_run();
        s.core().reset(run_id(8));
        require(member(s.adapter(), SourceKeys{}).empty()
                    && member(s.adapter(), SourceLive{}).empty()
                    && member(s.adapter(), Placements{}).size() == 0
                    && member(s.adapter(), Cohorts{}).size() == 0,
                "reset clears every current membership relation");
        require(!s.core().cohort_contains(cohort, pin), "old-run canonical membership is not reused");
        heap.fixed(store, resident);
    }
}

struct SemanticHashes {
    std::uint64_t source, native;
    bool operator==(const SemanticHashes& other) const {
        return source == other.source && native == other.native;
    }
};
SemanticHashes semantic_hashes(Subject& s) {
    BrokerStateHashSink source;
    s.adapter().hash_state(source);
    return {source.h, s.host.consumer().continuation_hash()};
}

// Digests, S4-D (680). Input: identical commands under different geometries,
// eviction, prefix compaction, explicit derived-cache discard, spill,
// rollback and a real host run reset. fails to compile: Subject bindings and
// OriginRoster::visit_positions_of. The exact additional red assertion is
// that hash/cache work allocates no population-sized temporary; the base
// source hash collects live-unit/source-key vectors. Expected VALUE/ORDER
// comes from Model; complete production hashes are compared metamorphically
// only where that independent model says the two logical states are equal.
void s4_membership_hashes_ignore_storage_red() {
    std::array<SemanticHashes, 6> reference{};
    for (std::size_t geometry : {std::size_t{2}, std::size_t{5}}) {
        Model model;
        std::vector<no::RequestHandle> handles;
        Rig rig;
        HeapCensus heap;
        auto store = std::make_unique<ScratchStore>(
            rig.config(ScratchOpenMode::create_empty, geometry), &rig.backing);
        Subject s(*store);
        const auto resident = store->stats().resident_bytes;
        heap.seal();
        const auto cohort = s.cohort("hash")->second.handle;
        for (std::size_t i = 0; i < 257; ++i) {
            no::RequestHandle expected;
            { OracleWork oracle; expected = model.submit(1.0 + i % 3, 35.0 + i % 7);
              handles.push_back(expected);
            }
            const auto h = s.submit(limit_order(1.0 + i % 3, 35.0 + i % 7));
            require(same_handle(h, expected), "identical issued handles across storage layouts");
            s.append("hash", h);
            ps::PlacementSnapshot snapshot;
            snapshot.source_id = "q";
            snapshot.requested_qty = 1.0 + i % 3;
            s.remember(h, snapshot);
            member(s.adapter(), SourceKeys{})[i + 10] = h;
            auto facts = s.cohort("hash");
            facts->second.opened.push_back(h);
            facts->second.live_units_by_origin[h.incarnation] = i % 2 ? -0.0 : 2.0;
            s.core().cohort_add(cohort, h); s.cohort_note();
            { OracleWork oracle;
              auto& c = model.cohorts["hash"];
              c.append(h); c.opened.push_back(h); c.canonical.push_back(h);
              c.units[h.incarnation] = i % 2 ? -0.0 : 2.0;
              model.placements.emplace(h.incarnation, snapshot);
              model.source_keys[i + 10] = h;
              model.names[h.incarnation] = 1;
            }
        }
        const auto check_values = [&] {
            check_book(s.core(), model);
            check_roster(s, "hash", model.cohorts.at("hash"));
            const auto facts = s.cohort("hash");
            const auto& expected = model.cohorts.at("hash");
            require(facts->second.opened.size() == expected.opened.size(), "opened order count");
            for (std::size_t i = 0; i < expected.opened.size(); ++i)
                require(same_handle(facts->second.opened[i], expected.opened[i]), "opened full-handle order");
            require(facts->second.live_units_by_origin.size() == expected.units.size(),
                    "live-unit key count is not a quantity predicate");
            for (const auto& unit : expected.units) {
                const auto at = facts->second.live_units_by_origin.find(unit.first);
                require(at != facts->second.live_units_by_origin.end() && at->second == unit.second,
                        "live-unit keys and values survive storage changes");
            }
            require(member(s.adapter(), SourceKeys{}).size() == model.source_keys.size(),
                    "source-key lookup cardinality");
            for (const auto& key : model.source_keys) {
                const auto& actual = member(s.adapter(), SourceKeys{}).at(key.first);
                require(same_handle(actual, key.second), "source-key relation agrees with independent model");
            }
            for (const auto& placement : model.placements)
                require(member(s.adapter(), Placements{}).at(placement.first).requested_qty
                            == placement.second.requested_qty, "placement value readback");
        };
        const auto checkpoint = [&](std::size_t phase) {
            check_values();
            const auto got = semantic_hashes(s);
            heap.fixed(*store, resident);
            if (geometry == 2) reference[phase] = got;
            else require(got == reference[phase], "existing native/source folds ignore storage geometry");
        };
        checkpoint(0);
        // Store digest and reverse point seeks displace every resident page.
        // No physical address or storage digest is an expected semantic hash.
        const auto before_eviction = semantic_hashes(s);
        (void)store_digest(*store);
        ScratchHandle cursor = 0;
        auto scan = store->first(cursor);
        std::uint64_t visited = 0;
        while (scan == ScratchStatus::ok) {
            ScratchRecordInfo info{};
            status_is(store->point(cursor, info), ScratchStatus::ok, "eviction point seek");
            ++visited;
            scan = store->next(cursor, cursor);
        }
        status_is(scan, ScratchStatus::end, "ordered scratch scan ends distinctly");
        require(visited == store->record_count() && visited > geometry,
                "eviction really walks more records than resident slots");
        require(semantic_hashes(s) == before_eviction, "eviction has no semantic hash bytes");
        checkpoint(1);
        const auto before_cache = semantic_hashes(s);
        s.host.consumer().set_host_cache(false);
        check_values();
        require(semantic_hashes(s) == before_cache, "discarding rebuildable cache adds no hash leaf");
        s.host.consumer().set_host_cache(true);
        checkpoint(2);
        // Retire logical rows only after all their pins have been removed.
        for (std::size_t i = 0; i < 193; ++i) {
            const auto h = handles[i];
            s.cancel(h); s.retire_handle(h);
            member(s.adapter(), Placements{}).erase(h.incarnation);
            auto facts = s.cohort("hash");
            facts->second.live_units_by_origin.erase(h.incarnation);
            auto& opened = facts->second.opened;
            opened.erase(std::find(opened.begin(), opened.end(), h));
            s.core().cohort_remove(cohort, h); s.cohort_note();
            { OracleWork oracle;
              model.erase(h); model.placements.erase(h.incarnation);
              model.source_keys.erase(i + 10); model.names.erase(h.incarnation);
              auto& c = model.cohorts.at("hash");
              c.units.erase(h.incarnation); remove_one(c.opened, h); remove_one(c.canonical, h);
            }
        }
        const auto before_compaction = semantic_hashes(s);
        s.retire();
        { OracleWork oracle; model.retire(model.cohorts.at("hash")); }
        require(semantic_hashes(s) == before_compaction, "prefix retirement changes no existing hash bytes");
        s.core().retire_history(s.ord);
        checkpoint(3);
        const auto old = semantic_hashes(s);
        const auto old_store = store_digest(*store);
        status_is(store->begin(), ScratchStatus::ok, "begin physical rollback exercise");
        ScratchHandle first = 0;
        status_is(store->first(first), ScratchStatus::ok, "existing record to erase provisionally");
        status_is(store->erase(first), ScratchStatus::ok, "provisional record/index erase");
        status_is(store->rollback(), ScratchStatus::ok, "rollback restores every logical byte");
        require(store_digest(*store) == old_store, "store rollback agrees with its committed-state witness");
        require(semantic_hashes(s) == old, "aborted scratch edits add no semantic hash bytes");
        store.reset();
        const auto at_open = rig.charges;
        store = std::make_unique<ScratchStore>(
            rig.config(ScratchOpenMode::reopen, geometry), &rig.backing);
        s.rebind(*store);
        checkpoint(4);
        rig.accounting(*store, at_open);
        // Mutation sensitivity: equality of two broken constant hashes is not
        // evidence. One independent durable live-unit value changes the
        // source hash, and restoring it recovers the original word exactly.
        auto facts = s.cohort("hash");
        const auto key = model.cohorts.at("hash").units.begin()->first;
        const auto units = model.cohorts.at("hash").units.at(key);
        facts->second.live_units_by_origin.at(key) = units + 3.0;
        const auto changed = semantic_hashes(s);
        require(changed.source != old.source, "independent live-unit value is hashed");
        facts->second.live_units_by_origin.at(key) = units;
        require(semantic_hashes(s) == old, "restoring the value restores the original folds");
        // Use the real host reset path, including the consumer's running
        // digest reset, rather than clearing core counters behind its back.
        const std::array<Bar, 2> bars{{
            {100, 101, 99, 100, 1, 1736121600000LL},
            {100, 101, 99, 100, 1, 1736121660000LL}}};
        s.host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(s.host.last_error().empty(), "real empty-strategy run reset succeeds");
        { OracleWork oracle; model = Model{}; }
        check_book(s.core(), model);
        require(member(s.adapter(), Cohorts{}).size() == model.cohorts.size(), "reset clears cohort indexes");
        const auto reset_hash = semantic_hashes(s);
        if (geometry == 2) reference[5] = reset_hash;
        else require(reset_hash == reference[5], "run reset has no storage-dependent hash leaf");
    }
}

ScratchBytes text_bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
ScratchStatus put_text(ScratchStore& store, const std::string& key,
                       const std::string& value, ScratchHandle& result) {
    ScratchSpanSource k(text_bytes(key)), v(text_bytes(value));
    return store.put(k, v, result);
}

class RepeatedSource final : public ScratchByteSource {
public:
    explicit RepeatedSource(std::uint64_t length) : length_(length) {}
    std::uint64_t size() const noexcept override { return length_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        if (offset > length_ || out.size > length_ - offset || (out.size && !out.data))
            return ScratchStatus::programming_error;
        if (out.size) std::memset(out.data, 'z', out.size);
        return ScratchStatus::ok;
    }
private:
    std::uint64_t length_;
};

using StatusModel = std::map<std::string, std::pair<ScratchHandle, std::string>>;
void check_status_contract(const StatusModel& model, const std::string& key) {
    Rig rig;
    HeapCensus heap;
    auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
    const auto resident = store->stats().resident_bytes;
    heap.seal();
    status_is(store->begin(), ScratchStatus::ok, "slots spill in one long transaction");
    for (const auto& row : model) {
        ScratchHandle out = 0;
        status_is(put_text(*store, row.first, row.second.second, out), ScratchStatus::ok,
                  "resident fullness must spill, never drop or cap the workload");
        require(out == row.second.first, "handle allocation equals the independent insertion model");
    }
    status_is(store->commit(ScratchDurability::buffered), ScratchStatus::ok, "spilled records commit");
    require(store->record_count() == model.size()
                && store->stats().backing_pages > rig.config().page_count, "the spill population was reached");
    // Read values and ordering through the production page sources. This is
    // an oracle for this status row, not the s3 lane's S34-R qualification.
    auto expected = model.begin();
    ScratchHandle cursor = 0;
    auto status = store->first(cursor);
    std::array<std::byte, page_bytes> chunk{};
    while (status == ScratchStatus::ok) {
        require(expected != model.end() && cursor == expected->second.first, "ordered membership is exact");
        const auto& value = expected->second.second;
        for (std::size_t offset = 0; offset < value.size();) {
            const auto n = std::min(chunk.size(), value.size() - offset);
            status_is(store->read(cursor, ScratchField::value, offset, {chunk.data(), n}),
                      ScratchStatus::ok, "spilled value chunk");
            require(std::memcmp(chunk.data(), value.data() + offset, n) == 0, "no sampled/dropped payload");
            offset += n;
        }
        ++expected;
        status = store->next(cursor, cursor);
    }
    status_is(status, ScratchStatus::end, "end is proven exhausted membership");
    require(expected == model.end(), "walk did not truncate at resident capacity");
    ScratchRecordInfo untouched{999, 11, 22};
    status_is(store->point(0, untouched), ScratchStatus::not_found, "invalid/absent scratch handle");
    require(untouched.handle == 999 && untouched.key_bytes == 11 && untouched.value_bytes == 22,
            "absence preserves caller output");
    status_is(store->read(1, ScratchField::value, 0, {nullptr, 1}),
              ScratchStatus::programming_error, "bad buffer is misuse, not absence");
    struct CancelProbe {
        std::uint64_t calls = 0;
        static bool stop(void* p, const ScratchRecordView&) noexcept {
            ++static_cast<CancelProbe*>(p)->calls; return false;
        }
    } cancellation;
    status_is(store->for_each({chunk.data(), chunk.size()}, &CancelProbe::stop, &cancellation),
              ScratchStatus::cancelled, "visitor cancellation remains distinct");
    require(cancellation.calls == 1, "cancellation really reached a present record");
    const auto before = store_digest(*store);
    // Larger than the entire present extent: reuse of already free pages
    // cannot accidentally avoid the GROWTH cap and turn this into a no-op.
    RepeatedSource payload(store->stats().backing_bytes + page_bytes);
    ScratchSpanSource key_source(text_bytes(key));
    status_is(store->begin(), ScratchStatus::ok, "start growth-cap transaction");
    rig.backing.cap = rig.backing.page_count();
    ScratchHandle output = 0xdead;
    const auto hits = rig.backing.hits;
    status_is(store->put(key_source, payload, output), ScratchStatus::io_error, "extent growth cap is I/O failure");
    require(rig.backing.hits > hits && output == 0xdead, "cap really hit, no success handle published");
    require(store->transaction_active(), "cap failure remains rollback-only");
    rig.backing.cap = std::numeric_limits<std::uint64_t>::max();
    status_is(store->rollback(), ScratchStatus::ok, "explicit cap rollback");
    require(store_digest(*store) == before && store->record_count() == model.size(),
            "cap failure never becomes a successful empty relation");
    status_is(store->begin(), ScratchStatus::ok, "start injected-medium transaction");
    rig.backing.arm(ScratchIoOperation::write_page, 1);
    const auto medium_hits = rig.backing.hits;
    status_is(store->put(key_source, payload, output), ScratchStatus::io_error, "medium failure stays io_error");
    require(rig.backing.hits == medium_hits + 1 && output == 0xdead, "no retry or fallback medium");
    rig.backing.disarm();
    status_is(store->rollback(), ScratchStatus::ok, "explicit medium rollback");
    rig.accounting(*store);
    heap.fixed(*store, resident);
    // Geometry is construction misuse; no lookup is entitled to report an
    // empty success from an unserviceable store.
    auto invalid_config = rig.config(ScratchOpenMode::reopen);
    invalid_config.page_count = 1;
    ScratchStore invalid(invalid_config, &rig.backing);
    status_is(invalid.status(), ScratchStatus::programming_error, "invalid geometry");
    status_is(invalid.point(1, untouched), ScratchStatus::programming_error, "bad store is not empty success");
    store.reset();
    rig.backing.corrupt_reads = true;
    ScratchStore corrupt(rig.config(ScratchOpenMode::reopen), &rig.backing);
    status_is(corrupt.status(), ScratchStatus::corrupt, "invalid stored bytes are corruption");
    require(rig.backing.corruptions != 0, "actual invalid framing was consumed");
    rig.backing.disarm();
}

// Statuses, S34-S (722). Input: overfull resident slots, a real growth cap,
// injected medium/corruption faults, invalid geometry/handle/buffer and a
// cancelled walk, followed by a consumer membership read under a read fault.
// fails to compile: Subject bind_scratch_store/rebind. Independently, the
// current ScratchStore put/erase contract maps backing I/O to programming_error;
// the spec requires io_error (copy 139-150), so that status assertion is red
// even after a binding-only implementation. Failed membership must THROW the
// existing typed I/O failure, never produce false or an empty relation.
void s34_failures_are_not_empty_membership_red() {
    // Independent ordered key/value/handle model, not a copy of a store walk.
    StatusModel model;
    for (std::uint64_t i = 0; i < 73; ++i) {
        const auto key = std::to_string(1000 + i);
        model.emplace(key, std::make_pair(i + 1, std::string(1300 + i, static_cast<char>('a' + i % 20))));
    }
    check_status_contract(model, "cap-new");
    // A separate consumer fixture keeps its own real queue/cohort schema.
    // Never insert this status oracle's arbitrary blob keys into broker state.
    Model members;
    Rig consumer_rig;
    HeapCensus failure_heap;
    auto consumer_store = std::make_unique<ScratchStore>(consumer_rig.config(), &consumer_rig.backing);
    Subject s(*consumer_store);
    const auto failure_resident = consumer_store->stats().resident_bytes;
    failure_heap.seal();
    const auto h = s.submit(limit_order(1.0, 33.0));
    no::RequestHandle expected_h;
    { OracleWork oracle; expected_h = members.submit(1.0, 33.0); }
    require(same_handle(h, expected_h), "membership fixture uses real issuance");
    const auto cohort = s.cohort("status")->second.handle;
    s.core().cohort_add(cohort, h); s.cohort_note();
    { OracleWork oracle; members.cohorts["status"].canonical.push_back(expected_h); }
    auto successor = h;
    for (std::size_t i = 0; i < 257; ++i) {
        successor = s.replace(successor, limit_order(1.0, 34.0 + i % 3), false);
        { OracleWork oracle;
          members.replace(members.queue.front().handle, 1.0, 34.0 + i % 3, false);
        }
    }
    s.cancel(successor);
    { OracleWork oracle; members.erase(members.queue.front().handle); }
    s.core().retire_history(s.ord);
    consumer_store.reset();
    consumer_store = std::make_unique<ScratchStore>(
        consumer_rig.config(ScratchOpenMode::reopen), &consumer_rig.backing);
    s.rebind(*consumer_store);
    consumer_rig.backing.arm(ScratchIoOperation::read_page, 1);
    const auto read_hits = consumer_rig.backing.hits;
    const no::RequestHandle oldest_successor{members.run, h.incarnation + 1};
    bool io = false;
    try { (void)s.core().cohort_contains(cohort, oldest_successor); }
    catch (const ScratchIoError& error) {
        io = classify_run_failure(error).code == RunFailureCode::scratch_io_failed;
    }
    require(io && consumer_rig.backing.hits == read_hits + 1, "failed lookup is never false membership");
    consumer_rig.backing.disarm();
    require(s.core().cohort_contains(cohort, oldest_successor)
                == members.native_member(members.cohorts.at("status"), oldest_successor),
            "after fault removal, committed membership remains exact");
    require(!s.core().cohort_contains(cohort, {members.run, members.next_incarnation + 1000}),
            "a successfully proved absent member still returns false");
    consumer_store.reset();
    consumer_store = std::make_unique<ScratchStore>(
        consumer_rig.config(ScratchOpenMode::reopen), &consumer_rig.backing);
    s.rebind(*consumer_store);
    consumer_rig.backing.corrupt_reads = true;
    const auto corruptions = consumer_rig.backing.corruptions;
    bool bad_membership_refused = false;
    try { (void)s.core().cohort_contains(cohort, oldest_successor); }
    catch (const std::exception&) { bad_membership_refused = true; }
    require(bad_membership_refused && consumer_rig.backing.corruptions > corruptions,
            "invalid stored membership framing cannot become false or an empty relation");
    consumer_rig.backing.disarm();
    failure_heap.fixed(*consumer_store, failure_resident);
}

struct TestCase { const char* name; void (*body)(); };
const TestCase tests[] = {
    {"s4_one_queue_owner_kept_handle_order_red", &s4_one_queue_owner_kept_handle_order_red},
    {"s4_queue_and_membership_islands_fixed_red", &s4_queue_and_membership_islands_fixed_red},
    {"s4_terminal_mark_descriptor_stays_fixed_red", &s4_terminal_mark_descriptor_stays_fixed_red},
    {"s34_record_index_publication_rollback_red", &s34_record_index_publication_rollback_red},
    {"s4_predicate_retained_placement_red", &s4_predicate_retained_placement_red},
    {"s4_predicate_named_by_each_current_owner_red", &s4_predicate_named_by_each_current_owner_red},
    {"s4_predicate_live_units_key_presence_red", &s4_predicate_live_units_key_presence_red},
    {"s4_predicate_opened_full_handle_red", &s4_predicate_opened_full_handle_red},
    {"s4_predicate_native_canonical_cohort_red", &s4_predicate_native_canonical_cohort_red},
    {"s4_five_pin_frontier_and_indexes_red", &s4_five_pin_frontier_and_indexes_red},
    {"s4_membership_hashes_ignore_storage_red", &s4_membership_hashes_ignore_storage_red},
    {"s34_failures_are_not_empty_membership_red", &s34_failures_are_not_empty_membership_red},
};
} // namespace

// With no argument, execute every clause and report each independently. An
// exact test-name argument selects one CTest row; an unknown name is failure,
// never a successful zero-test run.
int main(int argc, char** argv) {
    if (argc > 2) return 2;
    unsigned selected = 0, failed = 0;
    for (const auto& test : tests) {
        if (argc == 2 && std::strcmp(argv[1], test.name) != 0) continue;
        ++selected;
        try {
            test.body();
            std::fprintf(stdout, "PASS %s\n", test.name);
        } catch (const std::exception& error) {
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        } catch (...) {
            ++failed;
            std::fprintf(stderr, "FAIL %s: nonstandard exception\n", test.name);
        }
    }
    if (selected == 0) return 2;
    return failed == 0 ? 0 : 1;
}

/*
EDIT-ONLY HANDOFF -- no compilation, test, preflight or corpus execution.
Frozen HEAD: 87ba09a7013383847afd002755be046424a0c010, branch lab-o1s4.
Only deliverable: tests/test_o1s4_queue_red.cpp. No staging, commits, scratch
files, removals, production edits, CMake edits or fixture edits.

Clause receipts are PREDICTIONS, not executed fail-before receipts:

S4-O   s4_one_queue_owner_kept_handle_order_red
       Compile red: missing core/adapter bind_scratch_store. Queue/projection
       order, replacement values and authority have independent models;
       construction heap bound and real store traffic are later assertions.
S4-M   s4_queue_and_membership_islands_fixed_red
       Compile red: those bindings and OriginRoster::visit_positions_of.
       Heap byte census from construction, stats, fixed caller pages and
       charged I/O witness Q/P/C growth, drain and reuse, not a small-window
       or largest-population allowance.
S4-RE  s4_terminal_mark_descriptor_stays_fixed_red
       Compile red: range_end_trades_.row(index), plus the shared binding.
       The old producer builds P Trade rows/strings. Its heap peak and second
       mark's row count also violate the single-descriptor model.
S34-T  s34_record_index_publication_rollback_red
       Compile red: bindings/rebind. Sweep every actual write in each
       submit/replace/cancel update; require rollback-only, old committed
       values/order/handles/digests and same-backing recovery. Failed rollback
       is checked only when its hook actually hits restoration I/O.
S4-P1  s4_predicate_retained_placement_red
       Compile red: binding/position visitor. Native terminality is not
       placement absence; an actual erase releases the otherwise unpinned row.
S4-P2  s4_predicate_named_by_each_current_owner_red
       Compile red: binding/position visitor. Every enumerated queue, snapshot
       name, request owner/member, native root and zero is exercised; two
       references are removed in both orders. No sixth predicate is inferred.
S4-P3  s4_predicate_live_units_key_presence_red
       Compile red: binding/position visitor. Zero-valued key presence is
       independently modeled per cohort; erasing alpha never unpins beta.
S4-P4  s4_predicate_opened_full_handle_red
       Compile red: binding/position visitor. Duplicate opened rows, different
       runs/session text (including text larger than the cache), and global
       duplicate positions use full value equality.
S4-P5  s4_predicate_native_canonical_cohort_red
       Compile red: binding/position visitor. Real normal/kept replacements,
       terminal successors after journal retirement, authentication and an
       injected old-root read fault test canonical membership.
S4-I   s4_five_pin_frontier_and_indexes_red
       Compile red: binding/position visitor. All 32 combinations, single
       pins, duplicate/global positions, a large suffix, release, replace,
       erase, backing spill and reset agree with the independent frontier.
S4-D   s4_membership_hashes_ignore_storage_red
       Compile red: binding/position visitor. Value/order models are separate
       from both production hashes; equal modeled states require equal native
       and source hashes across storage schedules. A changed durable unit
       value must change the source fold. Hashing shares the heap bound.
S34-S  s34_failures_are_not_empty_membership_red
       Compile red: shared binding; independently predicted assertion red:
       ScratchStore currently converts put/erase I/O failures to
       programming_error, whereas the supplied contract requires io_error.
       Raw status fixtures and the consumer fixture do not share a key schema.

No assigned clause is omitted. S34-R, S34-B and all s3 rows are not assigned.

Interface questions/deviations for the implementation supervisor:
* The spec says (758-763), "A C++ binding ... is not declared by this
  document" and "No new method, index member, broker field, ABI layout ...
  is implied." The four missing call spellings at the file head are TEST
  interface debt, not production declarations or an assertion that CORE-TOP
  approved those exact C++ signatures. Select the actual borrowing/streaming
  interface before making these tests green; do not add broker state.
* S4-RE (548), "Mark P open lots twice, read terminal rows, clear, then mark
  again" is read as updating the terminal descriptor twice before clearing.
  The latest mark supplies P rows. The old head's append API instead
  accumulates materialized rows. That base discrepancy is explicit here.
* The construction high-water heap gate is deliberately conservative: fixed
  workspaces must be provisioned before the workload, not inferred by warming
  a population. It is a red userspace witness, not the measuring lane's F/A/M
  byte census or a physical-memory verdict.
* Fault sweeps inject at every actual page write reached by the production
  command. The undeclared owner binding must make its record/index puts,
  erases and commit traverse this borrowed ScratchStore; a detached test store
  fails the nonzero-traffic/fault-hit assertions.

CITATION AUDIT -- find by SYMBOL on this HEAD, not by the spec's old offset.
Abbreviations below are exact repository paths:
  N  = include/pineforge/native_order.hpp
  NC = src/native_order.cpp
  E  = include/pineforge/engine.hpp
  C  = src/native_execution_consumer.cpp
  CH = src/native_execution_consumer.hpp
  A  = include/pineforge/source/pine_adapter.hpp
  AC = src/source/pine_adapter.cpp
  H  = include/pineforge/source/pine_strategy_host.hpp
  HC = src/source/pine_strategy_host.cpp
  SH = src/source/pine_state_hash.cpp

Spec citation(s)               Symbol / location on 87ba09a7
N:2302                        history_: ABSENT; journal_ N:2518 and
                              JournalStore NC:1463 now own retained events.
N:2306                        ordinal_index_: ABSENT; JournalStore::find_ordinal
                              NC:1680 supplies absolute-position lookup.
E:415,2059                    OpenReportRowSource, materialized_, materialize():
                              ABSENT. range_end_trades_ is vector<Trade>
                              E:553, read by get_report_trade E:1835.
N:2301                        live_ N:2509 (plus BookSpill owner N:2510).
CH:1739                       requests_ CH:1728.
A:1851                        admission_journal A:1833.
AC:3507,3522                  remember AC:3465; retire AC:3509.
HC:1444,1454                  source_pending_view pending queues HC:1440
                              (all queued forms HC:1428 onward), live projection
                              HC:1450; function begins HC:1317.
N:1706                        WorkingRequestCore::live N:1793.
N:781                         LiveRequest::priority N:856.
N:1312                        CommandEvent N:1387. All nineteen alternatives
                              exist: AcceptedEvent 1080, RejectedEvent 1089,
                              ReplacedEvent 1096, ReplaceRejectedEvent 1112,
                              CancelledEvent 1122, NotWorkingEvent 1134,
                              InvalidHandleEvent 1141, NoEffectEvent 1151,
                              MatchRejectedEvent 1187,
                              ExecutionAppliedEvent 1225, CloseBoundEvent 1200,
                              ActivatedEvent 1210, ReservationReducedEvent 1270,
                              DeferredGroupAdjustmentEvent 1290,
                              QuantityBoundEvent 1308, ArmedEvent 1325,
                              TermsResolvedEvent 1168, MarginCallEvent 1341,
                              NativeRiskEvent 1374 (all in N).
N:1021                        ReplacedEvent N:1096; both definitions 1098-1099.
N:726,755                     RequestDefinition N:801; predecessor 805,
                              root 815, priority 824, kept_binding 830.
N:1710                        history N:1808; history_base 1810, history_end
                              1813; retained-order contract at N:1800.
AC:5017,21429,21493            admission append sites AC:5004,21349,21413.
scratch_store.hpp:44          ScratchStoreStats :45, resident_bytes :46.
scratch_store.hpp:55          no-growth workspace rule :55-61.
N:2320,2329,2337              issued_ N:2534; successor_roots_ 2535;
                              repriced_ 2543; receipt_store_ 2548;
                              cohorts_ 2553; cohort_receipts_ 2554.
A:2598                        placements/queues A:2563-2587:
                              placement_ 2563, live_by_source_key_ 2564,
                              bracket_families_ 2565, pending_bracket_legs_ 2566,
                              pending_entries_ 2567, delayed_market_orders_ 2568,
                              deferred_open_marketable_sells_ 2569,
                              throttled_reopen_rearm_ 2572,
                              pending_same_bar_commands_ 2575,
                              source_shadow_pending_ 2576,
                              pending_relative_exits_ 2578,
                              anchored_relative_legs_ 2581,
                              pending_coof_requests_ 2585,
                              pending_margin_revivals_ 2586, live_handles_ 2587.
NC:5878,5879                  SuccessorRootIndex::Row sizeof=16 NC:7678;
                              kTailRows=64 NC:7679.
C:9084,9091,9096              append_open_position_report_rows C:9048;
                              nonfinite refusal C:9065; vector push C:9068.
                              record_open_position_report_rows C:9097.
C:3432                       retire_journal C:3402; oldest notification
                              dependency C:3406; retirement call C:3410.
NC:1107,1129                  retire_history NC:2630, pending_chain_floor
                              NC:2596; collect_pending_chain NC:2883.
HC:406                       native_acknowledge_events(0) HC:411.
C:10475                      pump_batch Window presizer skip C:10446;
                              reserve hint C:10451.
AC:24254,24264                on_bar_close admission boundary AC:24178;
                              finish_admission_bar/fold AC:24184-24185;
                              retire_prefix AC:24186.
HC:3084                      process-on-close finish_admission_bar HC:3141.
AC:4373                      command gate last_rejected_command_bar AC:4360.
AC:28338                     retire_origin_prefixes keep loop AC:28186.
AC:28270                     retained-front-placement shortcut AC:28118.
NC:1073                      find_live NC:2552.
A:2600,2601                  live_by_source_key_ A:2564; placement_ A:2563.
A:979                        append_positions_of A:978; positions_of A:987.
A:1882                       CohortFacts A:1845; opened A:1848;
                              live_units_by_origin A:1858.
NC:1190,1197                  canonical_cohort_origin NC:2729;
                              terminal successor_root read NC:2740.
NC:1291                      cohort_contains NC:2830.
AC:28277                     named workset AC:28125, snapshot/request name
                              fields AC:28129-28145; sort/dedup 28179-28180.
AC:28303                     current owner/queue enumeration AC:28153-28178.
AC:28338-28343                P1 AC:28186; P2 28187; P3 28188-28189;
                              P4 28190; P5 28191.
AC:28217,28225                OriginRoster::append_positions_of AC:28065;
                              OriginRoster::retire_before AC:28073.
C:949                        hash_event_record C:954.
C:11894                      note_committed_events C:11853.
C:12074                      note_cohort_receipts C:12009.
C:2048,2051                  continuation count checks C:2020,2023-2025.
SH:803                       admission reflect("event") fold SH:813.
E:481                        BrokerStateHashSink E:351, normalization E:361.
C:150                        WordSink C:127.
SH:774,793                   journal frontier/count/digest SH:782-783;
                              cursor mismatch/prefix restart SH:800-808.
SH:307                       cohort roster/opened/live-unit folds SH:309-330.
AC:1530                      AdapterLookupIndex AC:1535 (kind AC:1531).
AC:1559,1576                 cohort_sides AC:1554; unbound 1573;
                              legs_by_origin 1574; immediate_closes_by_bar 1575.
NC:1096,1080                  history_at NC:2577; event_at NC:2570.
native_host.hpp:1330          native_events declaration :1370; window-floor
                              contract :1369 and :1387.
docs/scratch-store.md:218-224  observer contract is now :158-172, NOT that
                              old offset (which describes statuses here).
docs/scratch-store.md:351      unspecified disk/memfd/tmpfs implementations
                              are listed at :287 on this HEAD.

Unchanged cited locations/symbols checked too:
H:1072 adapter_; A:995 OriginRoster::positions_ (items_ A:993);
include/pineforge/native_order_identity.hpp:125 RunIdentity (RequestHandle 135);
include/pineforge/source/market_admission.hpp:75 CommandObservation, :145
CommandEvent, :181 Event, :193 Prefix;
src/source/market_admission.cpp:154 append, :157 engine_invariant refusal,
:166 retain, :171 reset, :184 retire_prefix, :266/:286/:326 Reflect methods;
src/compat/pine/market_admission.cpp:141 last_rejected_command_bar and :153
admission_history; SH:21 hash_admission_field.
work/DESIGN.md and work/STREAM-MODE.md are not specification sources in this
tree, as the supplied spec itself says. No production declaration is added to
resolve any absent symbol or differing description.

CMAKE HANDOFF -- exact lines for the separate registration lane, appended in
its authorized registration file. This TU needs the source layer. The single
target supports an exact-name argument; each add_test selects one clause.

if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_o1s4_queue_red)
    add_executable(test_o1s4_queue_red test_o1s4_queue_red.cpp)
    target_link_libraries(test_o1s4_queue_red PRIVATE pineforge)
    target_compile_options(test_o1s4_queue_red PRIVATE -ffp-contract=off -UNDEBUG)
    add_test(NAME s4_one_queue_owner_kept_handle_order_red
        COMMAND test_o1s4_queue_red s4_one_queue_owner_kept_handle_order_red)
    add_test(NAME s4_queue_and_membership_islands_fixed_red
        COMMAND test_o1s4_queue_red s4_queue_and_membership_islands_fixed_red)
    add_test(NAME s4_terminal_mark_descriptor_stays_fixed_red
        COMMAND test_o1s4_queue_red s4_terminal_mark_descriptor_stays_fixed_red)
    add_test(NAME s34_record_index_publication_rollback_red
        COMMAND test_o1s4_queue_red s34_record_index_publication_rollback_red)
    add_test(NAME s4_predicate_retained_placement_red
        COMMAND test_o1s4_queue_red s4_predicate_retained_placement_red)
    add_test(NAME s4_predicate_named_by_each_current_owner_red
        COMMAND test_o1s4_queue_red s4_predicate_named_by_each_current_owner_red)
    add_test(NAME s4_predicate_live_units_key_presence_red
        COMMAND test_o1s4_queue_red s4_predicate_live_units_key_presence_red)
    add_test(NAME s4_predicate_opened_full_handle_red
        COMMAND test_o1s4_queue_red s4_predicate_opened_full_handle_red)
    add_test(NAME s4_predicate_native_canonical_cohort_red
        COMMAND test_o1s4_queue_red s4_predicate_native_canonical_cohort_red)
    add_test(NAME s4_five_pin_frontier_and_indexes_red
        COMMAND test_o1s4_queue_red s4_five_pin_frontier_and_indexes_red)
    add_test(NAME s4_membership_hashes_ignore_storage_red
        COMMAND test_o1s4_queue_red s4_membership_hashes_ignore_storage_red)
    add_test(NAME s34_failures_are_not_empty_membership_red
        COMMAND test_o1s4_queue_red s34_failures_are_not_empty_membership_red)
endif()
*/
