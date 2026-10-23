// LAB-O1SEAMS s4, round 1; frozen base 87ba09a7. EDIT-ONLY: never compiled/run.
// Contract: scratch-store.md copy, sha256
// 71adb08d2a4308f961cc8f8f24a7c91fffd38e9b487d04cef558c133cac6ce5e,
// section "O(1) seams s3 and s4", lines 357-767.
//
// Interface debt, deliberately left RED:
//   WorkingRequestCore::bind_scratch_store(ScratchStore&) -> void
//   PineExecutionAdapter::bind_scratch_store(ScratchStore&) -> void
//   range_end_trades_.row(std::size_t) const -> const Trade&
//   OriginRoster::visit_positions_of(const RequestHandle&, Visitor&&) const
//       -> void; Visitor is a callable bool(std::size_t)
//   PineExecutionAdapter::fixture_cohort_state(
//       const SourceId&, std::uint8_t edit, const RequestHandle&, double)
//       -> std::pair<CohortHandle, const OriginRoster*>
//   PineExecutionAdapter::fixture_projection_state(
//       std::uint8_t edit, const RequestHandle&, const PlacementSnapshot*) -> void
//   PineExecutionAdapter::fixture_queue_state(
//       std::uint8_t queue, const Request*, const PlacementSnapshot*,
//       const RequestHandle& handle, const RequestHandle& parent) -> void
//   PineExecutionAdapter::fixture_source_key(
//       std::optional<std::uint64_t> key, const RequestHandle*) -> void
//   BacktestEngine::fixture_open_lot(PositionSide, const PyramidEntry*) -> void
//   PineExecutionAdapter::fixture_retire_origin_prefixes() -> void
//   NativeExecutionConsumer::fixture_note_committed(
//       const EventRange&, bool cohort_receipts) -> void
// These are minimal test spellings for borrowing the explicitly selected
// store, installing fixture inputs and borrowing existing roster/report
// projections, NOT declarations of production APIs. Request, RequestHandle,
// CohortHandle and EventRange are native_order types; SourceId,
// PlacementSnapshot and OriginRoster are source types. The spec explicitly
// leaves that interface decision open (758-763). Edit/queue codes are the
// uint8_t values of CohortEdit/ProjectionEdit/QueueId below. Cohort lookup
// returns the ACTUAL native handle and borrowed OriginRoster (null on absence);
// no seam computes a predicate, expected result, count or digest. All input
// pointers are borrowed for the call, never transferred. A null request AND
// snapshot clears the selected queue; otherwise append one row, with the
// supplied handle/parent used only by anchored rows. A null source key clears
// that relation; a present key plus null handle erases just that key.
// A null lot clears the physical book; otherwise append that lot and update
// its side/quantity scalars. Projection remember/retire, prefix retirement and
// native notification MUST invoke the existing production transitions, not
// recreate their effects in the fixture. Projection put/erase change only the
// placement; name/unname change only source-live membership. Cohort clear_all
// changes only source cohort state. Other edits install only their named state.
// No seam implementation or substitute container is supplied here.
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
// The red gate starts at LIVE construction bytes plus the explicit fixed-frame
// ledger below. It sees transient allocate/free peaks as well as final live
// bytes. Allocation count alone is not a memory bound.
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
#include <chrono>
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

// Fixture transport codes, not production enum declarations. erase_opened
// removes ONE full-handle occurrence; unit edits use the incarnation key even
// for a zero value. lookup never creates state; ensure alone opens a cohort.
enum class CohortEdit : std::uint8_t {
    lookup, ensure, append_origin, append_opened, erase_opened, clear_opened,
    put_units, erase_units, clear_units, clear_all
};
enum class ProjectionEdit : std::uint8_t { remember, retire, put, erase, name, unname };
enum class QueueId : std::uint8_t {
    bracket, entry, delayed, sell, rearm, same_bar, shadow, coof, revival, anchored
};

// All comparisons of identities in the reference model compare TEXT and the
// entire run, never SessionKey's interned pointer or incarnation alone.
bool same_handle(const no::RequestHandle& a, const no::RequestHandle& b) {
    return a.incarnation == b.incarnation && a.run.run_number == b.run.run_number
        && a.run.session_key.string() == b.run.session_key.string();
}
bool within_four_ulps(double actual, double expected) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return actual == expected;
    for (unsigned step = 0; step < 4 && expected != actual; ++step)
        expected = std::nextafter(expected, actual);
    return actual == expected;
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
    std::size_t live_at_seal = 0;
    std::size_t calls = 0;
    // B_u ledger: post-construction LIVE owners (including W) plus F.
    // F covers simultaneous old/new immutable definitions and shared control
    // blocks; old/new live/event frames; old/new source placement frames; one
    // decoded lot/report row; and one fixed codec transfer frame. These are
    // frames for ONE update/read, never slots per resident queue/member.
    // The four-pointer control-block envelope is the s3 ledger's spelling.
    // Stack caller pages in the tests are disjoint and counted only there.
    // No population, queue/handle count, record/text length or retained peak
    // enters this sum; allocator rounding A and physical charge M are separate.
    using SharedControlFrame = std::array<void*, 4>;
    using CodecTransferFrame = std::array<std::byte, page_bytes>;
    static constexpr std::size_t F =
        2 * (sizeof(no::RequestDefinition) + sizeof(SharedControlFrame))
            // S4-O (401), S34-T (588): old/new definitions at one publication.
        + 2 * (sizeof(no::LiveRequest) + sizeof(no::CommandEvent))
            // S4-M (547), S34-T (588): current and provisional native frames.
        + 2 * sizeof(ps::PlacementSnapshot)
            // S4-M (547), S34-T (588): current and provisional source frames.
        + sizeof(PyramidEntry) + sizeof(Trade)
            // S4-RE (548): one borrowed lot and one terminal row at a time.
        + sizeof(CodecTransferFrame);
            // S34-R (697): bounded field transfer, independent of record length.
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
    void seal() {
        live_at_seal = global_allocation::live_bytes;
        peak = live_at_seal;
    }
    void fixed(const ScratchStore& store, std::uint64_t resident_at_construction) const {
        if (peak > live_at_seal + F) {
            std::fprintf(stderr, "heap census: peak=%zu live_at_seal=%zu F=%zu calls=%zu\n",
                         peak, live_at_seal, F, calls);
        }
        require(store.stats().resident_bytes == resident_at_construction,
                "ScratchStore workspace never grows");
        require(peak <= live_at_seal + F,
                "B_u: live construction owners plus the fixed simultaneous frames");
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
    using BacktestEngine::range_end_trades_;
    using BacktestEngine::calc_commission;
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
        host.consumer().fixture_note_committed({first, core().history_end() - first}, false);
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
    void cohort_note() { host.consumer().fixture_note_committed({}, true); }
    std::pair<no::CohortHandle, const ps::OriginRoster*> cohort_state(
            const std::string& id, CohortEdit edit,
            no::RequestHandle h = {}, double units = 0.0) {
        return adapter().fixture_cohort_state(id, static_cast<std::uint8_t>(edit), h, units);
    }
    no::CohortHandle cohort(const std::string& id) {
        return cohort_state(id, CohortEdit::ensure).first;
    }
    const ps::OriginRoster* roster(const std::string& id) {
        return cohort_state(id, CohortEdit::lookup).second;
    }
    void append(const std::string& id, no::RequestHandle h) {
        (void)cohort_state(id, CohortEdit::ensure);
        (void)cohort_state(id, CohortEdit::append_origin, h);
    }
    void projection(ProjectionEdit edit, no::RequestHandle h,
                    const ps::PlacementSnapshot* snapshot = nullptr) {
        adapter().fixture_projection_state(static_cast<std::uint8_t>(edit), h, snapshot);
    }
    void queue(QueueId id, const no::Request* request = nullptr,
               const ps::PlacementSnapshot* snapshot = nullptr,
               no::RequestHandle handle = {}, no::RequestHandle parent = {}) {
        adapter().fixture_queue_state(static_cast<std::uint8_t>(id),
                                     request, snapshot, handle, parent);
    }
    void source_key(std::optional<std::uint64_t> key, const no::RequestHandle* h = nullptr) {
        adapter().fixture_source_key(key, h);
    }
    void retire() { adapter().fixture_retire_origin_prefixes(); }
    void remember(no::RequestHandle h, ps::PlacementSnapshot snapshot) {
        projection(ProjectionEdit::remember, h, &snapshot);
    }
    void retire_handle(no::RequestHandle h) {
        projection(ProjectionEdit::retire, h);
    }
    void rebind(ScratchStore& reopened) {
        core().bind_scratch_store(reopened);
        adapter().bind_scratch_store(reopened);
    }
};

std::uint64_t source_hash(Subject& s) {
    BrokerStateHashSink hash;
    s.adapter().hash_state(hash);
    return hash.h;
}

// Validate the whole source-key relation through its existing semantic fold,
// including cardinality and full-handle values. Replay ONLY oracle inputs;
// no fixture returns an expected answer or enumerates a private container.
void check_source_keys(Subject& s, const Model& model) {
    const auto before = source_hash(s);
    s.source_key(std::nullopt);
    require((source_hash(s) == before) == model.source_keys.empty(),
            "source-key presence contributes semantic hash bytes");
    for (const auto& key : model.source_keys) s.source_key(key.first, &key.second);
    require(source_hash(s) == before,
            "source-key count and full-handle relation equal the independent model");
}

void check_current_membership(Subject& s, const std::string& id, const Model& model) {
    const auto& expected = model.cohorts.at(id);
    const auto opened = source_hash(s);
    (void)s.cohort_state(id, CohortEdit::clear_opened);
    require((source_hash(s) == opened) == expected.opened.empty(),
            "opened presence contributes semantic hash bytes");
    for (auto h : expected.opened) (void)s.cohort_state(id, CohortEdit::append_opened, h);
    require(source_hash(s) == opened,
            "opened count, full-handle values, duplicates and order equal the model");
    const auto units = source_hash(s);
    (void)s.cohort_state(id, CohortEdit::clear_units);
    require((source_hash(s) == units) == expected.units.empty(),
            "live-unit key presence contributes semantic hash bytes even at zero");
    for (const auto& unit : expected.units)
        (void)s.cohort_state(id, CohortEdit::put_units, {model.run, unit.first}, unit.second);
    require(source_hash(s) == units,
            "live-unit key count and values equal the independent model");
}

// Every current placement is observed through the production pending facade.
// Seen flags belong to the oracle; a duplicate cannot hide an omitted handle.
void check_pending_placements(Subject& s, const Model& model) {
    const auto& pending = s.adapter().pending_intent_view();
    const int count = pending.size();
    require(count == static_cast<int>(model.queue.size()),
            "pending projection has exactly the current executable handle count");
    std::vector<bool> seen;
    { OracleWork oracle; seen.resize(model.queue.size()); }
    for (int i = 0; i < count; ++i) {
        pf_pending_order_v1_t row{};
        require(pending.copy_v1(i, &row) >= 0, "production placement projection reads");
        std::size_t found = 0;
        while (found < model.queue.size()
               && model.queue[found].handle.incarnation != row.incarnation) ++found;
        require(found != model.queue.size(), "pending projection names only a current model handle");
        require(!seen[found], "one current placement projection per full issued handle");
        seen[found] = true;
        require(row.qty == model.queue[found].units,
                "placement value readback equals the input model");
    }
    for (bool present : seen)
        require(present, "pending projection omits no current model handle");
}

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
    const auto* roster = s.roster(id);
    require(roster != nullptr, "the installed cohort has a truthful roster projection");
    const auto& actual = *roster;
    require(actual.size() == expected.origins.size(), "retirement preserves lifetime size");
    require(actual.digest() == expected.digest, "append digest equals independent word fold");
    require(actual.base() == expected.base, "only the all-false contiguous prefix is released");
    std::size_t position = expected.base;
    for (; position < actual.size(); ++position) {
        require(position < expected.origins.size()
                    && same_handle(actual[position], expected.origins[position]),
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Ownership" (S4-O), clause 1 — kept-handle queue order, replacement values and source-only intent match the model.
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
        s.source_key(100 + i, &handles[i]);
        { OracleWork oracle; model.source_keys[100 + i] = expected; }
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = "q";
        snapshot.requested_qty = 1.0 + i;
        s.remember(handles[i], std::move(snapshot));
    }
    for (const char* label : {"intent-a", "intent-b"}) {
        const auto request = limit_order(9.0, 40.0, label);
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = label;
        snapshot.requested_qty = 9.0;
        s.queue(QueueId::entry, &request, &snapshot);
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
    s.source_key(101, &successor);
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
    const auto& projection = s.host.source_pending_view();
    require(projection.size() == model.queue.size() + model.queued_labels.size(),
            "pending observer reads executable projection plus unsubmitted intent");
    std::size_t queued = 0, live = 0;
    for (const auto& row : projection) {
        if (row.id != "q") {
            require(queued < model.queued_labels.size()
                        && row.id == model.queued_labels[queued++] && row.qty == 9.0,
                    "queued intent values and order are unchanged");
        } else {
            ++live;
        }
    }
    require(queued == model.queued_labels.size(), "source intent remains queued");
    require(live == model.queue.size(), "retire removes only the handle projection");
    const auto& pending = s.adapter().pending_intent_view();
    require(pending.size() == static_cast<int>(projection.size()),
            "both public pending projections expose the same logical population");
    for (const auto& expected : model.queue) {
        std::size_t matches = 0;
        for (int i = 0; i < pending.size(); ++i) {
            pf_pending_order_v1_t row{};
            require(pending.copy_v1(i, &row) >= 0, "pending projection row reads");
            if (row.incarnation == expected.handle.incarnation) {
                ++matches;
                require(row.qty == expected.units,
                        "placement projection follows the current accepted definition");
            }
        }
        require(matches == 1, "retired handles do not duplicate current source projections");
    }
    // The owning fixture facade projects source_id, not Request::label.
    // Check the request values/order as well through the existing source fold.
    const auto queued_state = source_hash(s);
    s.queue(QueueId::entry);
    for (const auto& label : model.queued_labels) {
        const auto request = limit_order(9.0, 40.0, label.c_str());
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = label; snapshot.requested_qty = 9.0;
        s.queue(QueueId::entry, &request, &snapshot);
    }
    require(source_hash(s) == queued_state, "queued request labels, values and order equal the model");
    check_source_keys(s, model);
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Fixed memory" (S4-M), clause 2 — Q/P/C growth, drain/reuse and spill keep the live-at-seal plus F heap/workspace bound.
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
                        const auto cohort = s.cohort(id);
                        s.append(id, actual); s.append(id, actual);
                        (void)s.cohort_state(id, CohortEdit::append_opened, actual);
                        (void)s.cohort_state(id, CohortEdit::put_units, actual, 0.0);
                        s.core().cohort_add(cohort, actual);
                        s.core().cohort_add(cohort, actual);
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
                        s.host.fixture_open_lot(PositionSide::LONG, &lot);
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
                    ps::PlacementSnapshot repriced;
                    repriced.source_id = "q"; repriced.requested_qty = 3.0;
                    s.projection(ProjectionEdit::put, h, &repriced);
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
                    s.projection(ProjectionEdit::erase, ordinary_target);
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
            }
            for (const auto h : handles) {
                if (s.core().find_live(h)) s.cancel(h);
                s.retire_handle(h);
                s.projection(ProjectionEdit::erase, h);
            }
            for (const auto& c : model.cohorts) {
                const auto cohort = s.cohort(c.first);
                for (const auto h : c.second.opened) s.core().cohort_remove(cohort, h);
                (void)s.cohort_state(c.first, CohortEdit::clear_opened);
                (void)s.cohort_state(c.first, CohortEdit::clear_units);
            }
            s.cohort_note();
            s.host.fixture_open_lot(PositionSide::FLAT, nullptr);
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

// Diagnostic copy of the islands case: small populations only, with disjoint
// subject/oracle/check timers. The original full-population red gate is above.
void s4_islands_small_n_probe_red() {
    using Clock = std::chrono::steady_clock;
    struct TimedWork {
        Clock::duration& elapsed;
        const Clock::time_point start;
        explicit TimedWork(Clock::duration& out) : elapsed(out), start(Clock::now()) {}
        ~TimedWork() { elapsed += Clock::now() - start; }
    };

    std::printf("islands-probe start\n");
    std::fflush(stdout);
    for (const unsigned axis : {0u, 1u, 2u}) {
        Model model;
        std::vector<no::RequestHandle> handles;
        { OracleWork oracle; handles.reserve(257); }
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
        heap.seal();
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            for (std::size_t n : {std::size_t{17}, std::size_t{257}}) {
                std::printf("islands-probe entering axis=%u cycle=%u n=%zu\n", axis, cycle, n);
                std::fflush(stdout);
                Clock::duration subject_time{}, oracle_time{}, check_time{};
                const auto subject_work = [&](auto&& work) -> decltype(auto) {
                    TimedWork timer(subject_time);
                    return work();
                };
                while (handles.size() < n) {
                    const auto i = handles.size();
                    const auto actual = subject_work([&] {
                        return s.submit(limit_order(1.0, 50.0 + i % 13));
                    });
                    no::RequestHandle expected;
                    { OracleWork oracle; TimedWork timer(oracle_time);
                      expected = model.submit(1.0, 50.0 + i % 13);
                      handles.push_back(expected);
                    }
                    require(same_handle(actual, expected), "no capacity refusal on growing population");
                    const std::string id = axis == 2 ? std::to_string(i) : "one";
                    if (axis != 0) {
                        subject_work([&] {
                            const auto cohort = s.cohort(id);
                            s.append(id, actual); s.append(id, actual);
                            (void)s.cohort_state(id, CohortEdit::append_opened, actual);
                            (void)s.cohort_state(id, CohortEdit::put_units, actual, 0.0);
                            s.core().cohort_add(cohort, actual);
                            s.core().cohort_add(cohort, actual);
                            s.cohort_note();
                        });
                        { OracleWork oracle; TimedWork timer(oracle_time);
                          auto& c = model.cohorts[id];
                          c.append(expected); c.append(expected);
                          c.opened.push_back(expected); c.units[expected.incarnation] = 0.0;
                          c.canonical.push_back(expected);
                        }
                    }
                    ps::PlacementSnapshot snapshot;
                    snapshot.source_id = "q";
                    snapshot.requested_qty = 1.0;
                    subject_work([&] { s.remember(actual, std::move(snapshot)); });
                    { OracleWork oracle; TimedWork timer(oracle_time);
                      model.placements[expected.incarnation].source_id = "q";
                      model.placements[expected.incarnation].requested_qty = 1.0;
                    }
                    if (axis == 1) {
                        // P alone grows: the executable queue is drained on
                        // every iteration, while source opened/lot facts stay.
                        PyramidEntry lot{};
                        lot.price = 100; lot.time = static_cast<std::int64_t>(i);
                        lot.qty = 1; lot.entry_incarnation = actual.incarnation;
                        subject_work([&] {
                            s.host.fixture_open_lot(PositionSide::LONG, &lot);
                            s.cancel(actual); s.retire_handle(actual);
                        });
                        { OracleWork oracle; TimedWork timer(oracle_time); model.erase(expected); }
                    } else if (axis == 2) {
                        // C alone grows; only one working row exists at once.
                        subject_work([&] { s.cancel(actual); s.retire_handle(actual); });
                        { OracleWork oracle; TimedWork timer(oracle_time); model.erase(expected); }
                    }
                }
                if (axis == 0) {
                    const auto h = handles[n / 2];
                    const auto moved = subject_work([&] {
                        return s.replace(h, limit_order(3.0, 61.0), true);
                    });
                    { OracleWork oracle; TimedWork timer(oracle_time); model.replace(h, 3.0, 61.0, true); }
                    require(same_handle(h, moved), "repriced_ uses the kept handle");
                    ps::PlacementSnapshot repriced;
                    repriced.source_id = "q"; repriced.requested_qty = 3.0;
                    subject_work([&] { s.projection(ProjectionEdit::put, h, &repriced); });
                    { OracleWork oracle; TimedWork timer(oracle_time);
                      model.placements.at(h.incarnation).requested_qty = 3.0;
                    }
                    const auto ordinary_target = handles[n / 3];
                    const auto ordinary = subject_work([&] {
                        return s.replace(ordinary_target, limit_order(4.0, 62.0), false);
                    });
                    no::RequestHandle want;
                    { OracleWork oracle; TimedWork timer(oracle_time);
                      want = model.replace(ordinary_target, 4.0, 62.0, false);
                      handles[n / 3] = want;
                    }
                    require(same_handle(ordinary, want), "ordinary replacement changes only its chain row");
                    subject_work([&] {
                        s.retire_handle(ordinary_target);
                        s.projection(ProjectionEdit::erase, ordinary_target);
                    });
                    ps::PlacementSnapshot replacement;
                    replacement.source_id = "q"; replacement.requested_qty = 4.0;
                    subject_work([&] { s.remember(ordinary, std::move(replacement)); });
                    { OracleWork oracle; TimedWork timer(oracle_time);
                      model.placements.erase(ordinary_target.incarnation);
                      model.placements[ordinary.incarnation].source_id = "q";
                      model.placements[ordinary.incarnation].requested_qty = 4.0;
                    }
                }
                {
                    TimedWork timer(check_time);
                    check_book(s.core(), model);
                    for (const auto& c : model.cohorts) check_roster(s, c.first, c.second);
                }
                subject_work([&] { s.retire(); });
                // Production pending facade; do not charge an explicit owning
                // diagnostic snapshot as if it were a bounded production read.
                // Time facade calls separately from their model lookup.
                const auto& pending = subject_work([&]() -> decltype(auto) {
                    return s.adapter().pending_intent_view();
                });
                for (int i = 0; i < subject_work([&] { return pending.size(); }); ++i) {
                    pf_pending_order_v1_t row{};
                    require(subject_work([&] { return pending.copy_v1(i, &row); }) >= 0,
                            "production observer row reads successfully");
                    {
                        OracleWork oracle; TimedWork timer(oracle_time);
                        const auto expected = model.placements.find(row.incarnation);
                        require(expected != model.placements.end()
                                    && row.qty == expected->second.requested_qty,
                                "production observer returns the input placement value");
                    }
                }
                rig.accounting(store);
                const auto stats = store.stats();
                std::printf("islands-probe axis=%u cycle=%u n=%zu "
                            "subject_ms=%.3f oracle_ms=%.3f check_ms=%.3f "
                            "store_reads=%llu store_writes=%llu store_records=%llu "
                            "backing_pages=%llu heap_live=%zu\n",
                            axis, cycle, n,
                            std::chrono::duration<double, std::milli>(subject_time).count(),
                            std::chrono::duration<double, std::milli>(oracle_time).count(),
                            std::chrono::duration<double, std::milli>(check_time).count(),
                            static_cast<unsigned long long>(stats.page_reads),
                            static_cast<unsigned long long>(stats.page_writes),
                            static_cast<unsigned long long>(store.record_count()),
                            static_cast<unsigned long long>(stats.backing_pages),
                            global_allocation::live_bytes);
                std::fflush(stdout);
            }
            for (const auto h : handles) {
                if (s.core().find_live(h)) s.cancel(h);
                s.retire_handle(h);
                s.projection(ProjectionEdit::erase, h);
            }
            for (const auto& c : model.cohorts) {
                const auto cohort = s.cohort(c.first);
                for (const auto h : c.second.opened) s.core().cohort_remove(cohort, h);
                (void)s.cohort_state(c.first, CohortEdit::clear_opened);
                (void)s.cohort_state(c.first, CohortEdit::clear_units);
            }
            s.cohort_note();
            s.host.fixture_open_lot(PositionSide::FLAT, nullptr);
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
        // The full-population case alone requires overflow at N=4096.
        heap.fixed(store, resident);
    }
    std::printf("islands-probe done\n");
    std::fflush(stdout);
}

// The probe's axis-0 inputs and replacements, with one fresh population.
// Keep construction outside every observer read delta; no pending enumeration
// is needed to know either the population or its first/last projected handle.
std::array<no::RequestHandle, 2> populate_pending_read_bound(
        Subject& s, Model& model, std::size_t n) {
    std::vector<no::RequestHandle> handles;
    { OracleWork oracle; handles.reserve(n); }
    for (std::size_t i = 0; i < n; ++i) {
        const auto actual = s.submit(limit_order(1.0, 50.0 + i % 13));
        no::RequestHandle expected;
        { OracleWork oracle;
          expected = model.submit(1.0, 50.0 + i % 13);
          handles.push_back(expected);
        }
        require(same_handle(actual, expected), "no capacity refusal on growing population");
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = "q"; snapshot.requested_qty = 1.0;
        s.remember(actual, std::move(snapshot));
        { OracleWork oracle;
          model.placements[expected.incarnation].source_id = "q";
          model.placements[expected.incarnation].requested_qty = 1.0;
        }
    }
    const auto h = handles[n / 2];
    const auto moved = s.replace(h, limit_order(3.0, 61.0), true);
    { OracleWork oracle; model.replace(h, 3.0, 61.0, true); }
    require(same_handle(h, moved), "repriced_ uses the kept handle");
    ps::PlacementSnapshot repriced;
    repriced.source_id = "q"; repriced.requested_qty = 3.0;
    s.projection(ProjectionEdit::put, h, &repriced);
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
    s.projection(ProjectionEdit::erase, ordinary_target);
    ps::PlacementSnapshot replacement;
    replacement.source_id = "q"; replacement.requested_qty = 4.0;
    s.remember(ordinary, std::move(replacement));
    { OracleWork oracle;
      model.placements.erase(ordinary_target.incarnation);
      model.placements[ordinary.incarnation].source_id = "q";
      model.placements[ordinary.incarnation].requested_qty = 4.0;
    }
    s.retire();
    // Keeping a handle leaves its projection slot in place; retiring the
    // ordinary predecessor and remembering its successor appends the last row.
    return {{handles.front(), ordinary}};
}

void s4_roster_scan_read_bound_red() {
    namespace ws = ::pineforge::detail::working_store;
    constexpr std::array<std::size_t, 2> populations{{17, 65}};
    std::array<std::uint64_t, 2> append_reads{}, erase_reads{}, geometry_reads{};
    // scratch_store_impl.hpp:68,178-184: U=512-16=496, ordered leaf=58,
    // node=29 separators (30 children), radix leaf/node=15/60. Depth/height
    // two can hold at most 58*30=1740 live keys / 15*60=900 issued handles.
    constexpr std::uint64_t usable = page_bytes - 16;
    constexpr std::uint64_t ordered_leaf = (usable - 32) / 8;
    constexpr std::uint64_t ordered_node = (usable - 24) / 16;
    constexpr std::uint64_t radix_leaf = (usable - 16) / 32;
    constexpr std::uint64_t radix_node = (usable - 16) / 8;
    // Ascending splits leave 30 keys / 16 children on the left
    // (scratch_store_impl.hpp:908-930,977-999). Populate 17 such child groups:
    // 30*16*17+1=8161 rows, plus the version allocator, yield 272 leaves and
    // 16 root separators. This also avoids the root-search step at 15 keys
    // (scratch_store_impl.hpp:800-810); lower-key subject inserts have room
    // before the next root split, instead of starting just above depth two.
    constexpr auto leaf_split = (ordered_leaf + 2) / 2;
    constexpr auto child_split = (ordered_node + 1) / 2 + 1;
    constexpr auto filler_rows = leaf_split * child_split * (child_split + 1) + 1;
    static_assert(filler_rows > ordered_leaf * (ordered_node + 1)
                      && filler_rows > radix_leaf * radix_node,
                  "the live filler forces both store trees past height two");
    constexpr auto filler_domain = std::numeric_limits<std::uint64_t>::max();
    constexpr auto filler_owner = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t canonical_bytes = 0;
    for (std::size_t sample = 0; sample < populations.size(); ++sample) {
        const auto n = populations[sample];
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        // An independent fixture owner, never named by the adapter. Empty
        // values need one root and no payload chunks (working_store.hpp:418,
        // 518-531), so the filler cardinality follows the geometry above.
        // Keep every even key live for this sample; the odd probe is absent.
        WorkingRowTable<std::monostate> filler;
        filler.bind(store, filler_domain, filler_owner);
        ws::transaction(store, [&] {
            for (std::uint64_t i = 1; i <= filler_rows; ++i)
                filler.put(2 * i, std::monostate{});
        });
        const auto anchor = ws::find(store, {filler_domain, filler_owner, 2});
        require(anchor != 0, "geometry filler has a live anchor");
        Subject s(store);
        std::vector<no::RequestHandle> handles;
        ps::PlacementSnapshot snapshot;
        snapshot.source_id = "q"; snapshot.requested_qty = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto h = s.submit(limit_order(1.0, 50.0 + i % 13));
            s.remember(h, snapshot);
            handles.push_back(h);
        }
        // Warm the actual write paths, not an observer/cache of their answer.
        // Remove the warm row so both charged operations start with N names.
        const auto warm = s.submit(limit_order(1.0, 61.0));
        s.remember(warm, snapshot);
        s.retire_handle(warm);
        s.projection(ProjectionEdit::erase, warm);
        s.cancel(warm);

        // point traverses three radix pages through two slots, leaving the
        // same two anchor pages before each isolated probe (impl:693-703).
        ScratchRecordInfo anchor_info{};
        status_is(store.point(anchor, anchor_info), ScratchStatus::ok,
                  "geometry filler stays live before the charged calls");
        // The base has this contains API: working_row_table.hpp:123-125 calls
        // one working_store::find (working_store.hpp:139-144), the same single
        // lower_bound/equality path as PendingRoster::find_identity
        // (pending_roster_impl.hpp:203-210). No row pin or payload is decoded.
        const auto geometry_before = store.stats().page_reads;
        const bool found = filler.contains(2 * filler_rows - 1);
        geometry_reads[sample] = store.stats().page_reads - geometry_before;
        require(!found && geometry_reads[sample] != 0,
                "geometry probe proves an absent key through actual store reads");
        if (sample != 0)
            require(geometry_reads[sample] == geometry_reads[0],
                    "store geometry was not pinned across roster populations");

        const auto append_before = store.stats().page_reads;
        const auto extra = s.submit(limit_order(1.0, 62.0));
        s.remember(extra, snapshot);
        const auto append_after = store.stats().page_reads;
        append_reads[sample] = append_after - append_before;

        const auto middle = handles[n / 2];
        const auto erase_before = store.stats().page_reads;
        s.retire_handle(middle);
        s.projection(ProjectionEdit::erase, middle);
        const auto erase_after = store.stats().page_reads;
        erase_reads[sample] = erase_after - erase_before;
        require(s.core().find_live(extra) != nullptr,
                "the charged append actually submits a new native handle");
        // A byte bounds one nonempty scalar/chunk-piece decode request. This
        // is an encoding-derived allowance, not a fitted page-read constant.
        canonical_bytes = std::max(canonical_bytes,
            ::pineforge::detail::working_store::Source<ps::PlacementSnapshot>(snapshot).size()
            + ::pineforge::detail::working_store::Source<no::Request>(
                limit_order(1.0, 62.0)).size());
        std::printf("roster-scan-read-bound n=%zu geometry_reads=%llu append_reads=%llu erase_reads=%llu\n", n,
                    static_cast<unsigned long long>(geometry_reads[sample]),
                    static_cast<unsigned long long>(append_reads[sample]),
                    static_cast<unsigned long long>(erase_reads[sample]));
        std::fflush(stdout);
    }
    // Reuse s4_pending_size_read_bound_red's geometry: 512-byte pages, two
    // slots, pinned d=h=3, ordered capacities 58/29 and radix capacities 15/60.
    // L=2*d+2+[5*(d-1)+7]*(h+2)+2*h+1=100 bounds one 32-byte key find.
    // C(S)=2*h+2*ceil(S/56)+4 bounds a direct fixed-record load; reserve S=128.
    // An AVL of height 9 needs at least 88 names (the 1,2,4,7,... recurrence),
    // so the largest charged roster (66 names) has height <=8. Budget
    // A(H)=40*H+16 direct loads and W(H)=12*H+16 publications: per ancestor,
    // two child loads, two balance loads, up to two rotations (eight loads /
    // four writes each), parent repair and endpoint/identity/root work.
    // Payload pieces cost <=L+h+9. Allow 16 such indexed passes per encoded
    // byte for canonical decode/write/retirement; for each structural write
    // allow 16 passes for descent, key/value cells, radix and undo/split work.
    // For N=17 ->65, allow 64 extra indexed paths (two additional counted
    // ancestors, their rotations/publications, and canonical path splitting).
    // No population or measured delta occurs in either allowance.
    //
    // Old remember visits every prior name; old retire visits every name and
    // rewrites the suffix. A decoded handle needs four payload finds, besides
    // its root. Each find compares an ordered page, >=2 radix pages and a key
    // page through two slots, repeatedly for the binary-search comparisons.
    // The 48 additional decoded/dirty-compared rows therefore have a linear
    // read term; unlike the indexed paths it is not a height-change allowance.
    constexpr std::uint64_t d = 3, h = 3;
    constexpr std::uint64_t lookup =
        2 * d + 2 + (5 * (d - 1) + 7) * (h + 2) + 2 * h + 1;
    constexpr std::uint64_t slack = 64 * lookup;
    constexpr std::uint64_t height = 8;
    constexpr std::uint64_t direct = 2 * h + 2 * ((128 + 55) / 56) + 4;
    const auto absolute = 16 * canonical_bytes * (lookup + h + 9)
        + (40 * height + 16) * direct
        + (12 * height + 16) * 16 * (lookup + h + 9);
    for (const auto& reads : {append_reads, erase_reads}) {
        require(reads[1] <= reads[0] + slack,
                "one roster mutation has no live-population scan read term");
        require(reads[0] <= absolute && reads[1] <= absolute,
                "one roster mutation stays under the geometry-derived ceiling");
    }
}

void check_live_roster_exact(const ps::PendingRoster& actual,
        const std::vector<no::RequestHandle>& expected,
        const std::vector<no::RequestHandle>& universe) {
    require(actual.size() == expected.size(), "roster cardinality equals the vector model");
    std::size_t rank = 0;
    std::optional<ps::PendingRoster::Order> previous;
    for (const auto& h : actual) {
        require(rank < expected.size() && same_handle(h, expected[rank]),
                "const roster iteration preserves every full handle in order");
        const auto occurrence = actual.occurrence(h);
        require(occurrence.has_value() && same_handle(occurrence->handle, h),
                "full-handle membership resolves the iterated occurrence");
        require(!previous || *previous < occurrence->order,
                "stable occurrence labels express the same logical order");
        previous = occurrence->order;
        require(same_handle(actual[rank], expected[rank])
                    && same_handle(actual.observe_at(rank), expected[rank]),
                "counted rank and read-only observation agree with vector order");
        ++rank;
    }
    require(rank == expected.size(), "roster iteration omits no model row");
    rank = expected.size();
    for (auto at = actual.rbegin(); at != actual.rend(); ++at) {
        require(rank != 0 && same_handle(*at, expected[--rank]),
                "reverse const iteration preserves the reverse vector order");
    }
    require(rank == 0, "reverse iteration omits no model row");
    for (const auto& h : universe) {
        const bool present = has_handle(expected, h);
        require(actual.contains(h) == present && actual.occurrence(h).has_value() == present,
                "membership includes actual run and all session-key bytes");
        std::vector<no::RequestHandle> aliases;
        actual.visit_incarnation(h.incarnation, [&](const ps::PendingRoster::Occurrence& row) {
            require(row.handle.incarnation == h.incarnation
                        && has_handle(expected, row.handle) && !has_handle(aliases, row.handle),
                    "incarnation relation includes only distinct current full handles");
            aliases.push_back(row.handle);
            return true;
        });
        std::size_t wanted = 0;
        for (const auto& row : expected) {
            if (row.incarnation != h.incarnation) continue;
            ++wanted;
            require(has_handle(aliases, row), "incarnation relation keeps every alias, including holes");
        }
        require(aliases.size() == wanted, "incarnation relation has exact alias cardinality");
    }
}

void s4_roster_index_exact_red() {
    using Roster = ps::PendingRoster;
    static_assert(std::is_same_v<decltype(*std::declval<Roster&>().begin()),
                                 const no::RequestHandle&>,
                  "even a nonconst owner exposes only const roster iteration");
    Rig rig;
    auto store_owner = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
    auto& store = *store_owner;
    Subject s(store);
    std::vector<no::RequestHandle> model, universe, issued;
    ps::PlacementSnapshot snapshot;
    snapshot.source_id = "q"; snapshot.requested_qty = 1.0;
    const auto check = [&] {
        check_live_roster_exact(s.adapter().fixture_pending_roster(), model, universe);
    };
    const auto model_append = [&](no::RequestHandle h) {
        if (!has_handle(model, h)) model.push_back(h);
    };
    const auto model_erase = [&](no::RequestHandle h) {
        if (has_handle(model, h)) remove_one(model, h);
    };
    check();
    for (unsigned i = 0; i < 17; ++i) {
        const auto h = s.submit(limit_order(1.0, 50.0 + i % 13));
        universe.push_back(h); issued.push_back(h);
        check(); // Native publication alone does not name a source occurrence.
        s.remember(h, snapshot); model_append(h); check();
    }

    const auto kept = issued[5];
    require(same_handle(s.replace(kept, limit_order(3.0, 61.0), true), kept),
            "real kept-handle replacement retains full identity");
    check();
    snapshot.requested_qty = 3.0;
    s.projection(ProjectionEdit::put, kept, &snapshot); check();
    const auto kept_id = s.adapter().fixture_pending_roster().occurrence(kept)->id;
    s.remember(kept, snapshot); model_append(kept); check();
    require(s.adapter().fixture_pending_roster().occurrence(kept)->id == kept_id,
            "duplicate remember neither moves nor recreates an occurrence");

    const auto predecessor = issued[8];
    const auto successor = s.replace(predecessor, limit_order(4.0, 62.0), false);
    require(!same_handle(predecessor, successor), "real ordinary replace issues a successor");
    universe.push_back(successor); check();
    s.retire_handle(predecessor); model_erase(predecessor); check();
    s.projection(ProjectionEdit::erase, predecessor); check();
    snapshot.requested_qty = 4.0;
    s.remember(successor, snapshot); model_append(successor); check();

    const auto hole = issued[3];
    s.projection(ProjectionEdit::erase, hole); check();
    s.projection(ProjectionEdit::name, hole); model_append(hole); check();
    s.projection(ProjectionEdit::put, hole, &snapshot); check();
    const no::RequestHandle absent{{"absent-roster", 99}, 999999};
    universe.push_back(absent);
    s.retire_handle(absent); model_erase(absent); check();

    // Both a different run and a different session with identical incarnation
    // are distinct aliases. Embedded NULs and a >page session exercise full
    // bytes; no truncated string or incarnation hash may decide membership.
    const auto original = issued[2];
    auto run_alias = original;
    ++run_alias.run.run_number;
    auto session_alias = original;
    std::string session(521, 'x');
    session[13] = '\0';
    session_alias.run.session_key = session;
    auto tail_alias = original;
    session.back() = 'y';
    tail_alias.run.session_key = session;
    universe.push_back(run_alias); universe.push_back(session_alias); universe.push_back(tail_alias);
    s.remember(run_alias, snapshot); model_append(run_alias); check();
    s.projection(ProjectionEdit::name, session_alias); model_append(session_alias); check();
    s.projection(ProjectionEdit::name, tail_alias); model_append(tail_alias); check();
    s.retire_handle(original); model_erase(original); check();
    s.projection(ProjectionEdit::erase, original); check(); // Aliases are still named holes.
    s.projection(ProjectionEdit::put, run_alias, &snapshot); check();
    s.projection(ProjectionEdit::unname, run_alias); model_erase(run_alias); check();
    s.retire_handle(session_alias); model_erase(session_alias); check();
    s.retire_handle(session_alias); check(); // Already absent.
    s.retire_handle(tail_alias); model_erase(tail_alias); check();

    const auto first = model.front();
    s.retire_handle(first); model_erase(first); check();
    s.projection(ProjectionEdit::erase, first); check();
    const auto last = model.back();
    s.retire_handle(last); model_erase(last); check();
    s.projection(ProjectionEdit::erase, last); check();

    const auto digest = source_hash(s);
    std::array<std::byte, page_bytes> page{};
    std::uint64_t stored = 0, rebound = 0;
    status_is(store.ordered_digest({page.data(), page.size()}, stored), ScratchStatus::ok,
              "committed roster digest before rebind");
    s.rebind(store);
    check();
    require(source_hash(s) == digest, "normal rebind preserves both source roster hash folds");
    status_is(store.ordered_digest({page.data(), page.size()}, rebound), ScratchStatus::ok,
              "committed roster digest after rebind");
    require(rebound == stored, "normal rebind does not rewrite or rebuild derived roster records");

    // Exercise the two operations used by the real reorder sites, with the
    // same kernel-issued handles and an independent plain vector. An observer
    // witnesses one joined publication per move and stable occurrence ids.
    Roster moves;
    moves.bind(store, 2);
    struct Events { ScratchStore* store; std::vector<Roster::Change> rows; } events{&store, {}};
    moves.set_observer(&events, [](void* context, const Roster::Change& event) {
        auto& out = *static_cast<Events*>(context);
        require(out.store->transaction_active(), "B receives roster events inside the source transaction");
        out.rows.push_back(event);
    });
    std::vector<no::RequestHandle> order;
    for (const auto& h : issued) {
        moves.append(h); order.push_back(h);
        check_live_roster_exact(moves, order, universe);
    }
    const auto moving = issued[9];
    const auto identity = moves.occurrence(moving)->id;
    const auto count = events.rows.size();
    require(moves.move_to_front(moving), "real front-move facade changes order");
    remove_one(order, moving); order.insert(order.begin(), moving);
    check_live_roster_exact(moves, order, universe);
    require(events.rows.size() == count + 1 && events.rows.back().kind == Roster::ChangeKind::move
                && events.rows.back().before.id == identity && events.rows.back().after.id == identity,
            "one move event keeps a stable occurrence id");
    require(moves.move_to_back(moving), "real back-move facade changes order");
    remove_one(order, moving); order.push_back(moving);
    check_live_roster_exact(moves, order, universe);
    const auto noops = events.rows.size();
    require(!moves.append(moving) && !moves.erase(absent) && !moves.move_to_back(moving)
                && events.rows.size() == noops,
            "idempotent mutations do not invent occurrence events");
    const auto records = store.record_count();
    {
        const auto detached = moves.snapshot();
        std::size_t i = 0;
        for (const auto& h : detached)
            require(i < order.size() && same_handle(h, order[i++]), "detached workset preserves order");
        require(i == order.size(), "detached workset contains every occurrence");
        moves.erase(order.front()); order.erase(order.begin());
        check_live_roster_exact(moves, order, universe);
    }
    require(store.record_count() == records - 2,
            "detached workset reclaims rows and its count; one erased roster row reclaims two records");
    const auto committed = store_digest(store);
    const auto writes = store.stats().page_writes;
    check_live_roster_exact(moves, order, universe);
    require(store.stats().page_writes == writes && store_digest(store) == committed,
            "roster reads leave every store record and allocation byte unchanged");
    status_is(store.begin(), ScratchStatus::ok, "explicit provisional roster transaction");
    moves.erase(order[order.size() / 2]);
    moves.append(absent);
    moves.move_to_front(order.back());
    status_is(store.rollback(), ScratchStatus::ok, "explicit roster rollback");
    moves.bind(store, 2);
    check_live_roster_exact(moves, order, universe);
    require(store_digest(store) == committed,
            "rollback plus rebind preserves exact index, source, and allocation bytes");
    // A fresh two-slot cache reads the persisted owner without rebuilding.
    store_owner.reset();
    store_owner = std::make_unique<ScratchStore>(rig.config(ScratchOpenMode::reopen), &rig.backing);
    auto& reopened = *store_owner;
    status_is(reopened.status(), ScratchStatus::ok, "cold roster store reopens");
    const auto reopen_writes = reopened.stats().page_writes;
    moves.bind(reopened, 2);
    events.store = &reopened;
    s.rebind(reopened);
    check_live_roster_exact(moves, order, universe);
    check();
    require(reopened.stats().page_writes == reopen_writes
                && store_digest(reopened) == committed,
            "cold reopen and both owner rebindings neither write nor regenerate ids");
    moves.clear(); order.clear();
    check_live_roster_exact(moves, order, universe);
    moves.set_observer(nullptr, nullptr);
    s.adapter().reset_for_run(); model.clear(); check();
}

// A plain-vector specification of the six raw owners. It neither enumerates
// the production projection nor uses its grouping/count helpers.
struct PendingSizeModel {
    std::vector<no::RequestHandle> live;
    std::vector<std::pair<std::uint64_t, ps::PlacementSnapshot>> placements;
    // The raw API order, deliberately different from QueueId's enum order.
    std::array<std::vector<ps::PlacementSnapshot>, 5> queued;

    static std::size_t segment(QueueId id) {
        switch (id) {
        case QueueId::same_bar: return 0;
        case QueueId::entry: return 1;
        case QueueId::bracket: return 2;
        case QueueId::coof: return 3;
        case QueueId::shadow: return 4;
        default: throw std::runtime_error("count model requires a projected queue");
        }
    }
    void edit(ProjectionEdit edit, const no::RequestHandle& h,
              const ps::PlacementSnapshot* snapshot = nullptr) {
        auto at = std::find_if(placements.begin(), placements.end(),
            [&](const auto& row) { return row.first == h.incarnation; });
        if (edit == ProjectionEdit::remember || edit == ProjectionEdit::put) {
            require(snapshot != nullptr, "model placement input is explicit");
            if (at == placements.end()) placements.emplace_back(h.incarnation, *snapshot);
            else at->second = *snapshot;
        } else if (edit == ProjectionEdit::erase && at != placements.end()) {
            placements.erase(at);
        }
        if (edit == ProjectionEdit::remember || edit == ProjectionEdit::name) {
            if (!has_handle(live, h)) live.push_back(h);
        } else if (edit == ProjectionEdit::retire || edit == ProjectionEdit::unname) {
            live.erase(std::remove_if(live.begin(), live.end(),
                [&](const auto& row) { return same_handle(row, h); }), live.end());
        }
    }
    int count() const {
        std::vector<ps::PlacementSnapshot> visible;
        std::size_t raw = 0;
        const auto exit = [](const ps::PlacementSnapshot& row) {
            switch (row.family) {
            case ps::PineOrderFamily::ExitLimit:
            case ps::PineOrderFamily::ExitStop:
            case ps::PineOrderFamily::ExitTrail: return true;
            default: return false;
            }
        };
        const auto offer = [&](const ps::PlacementSnapshot* row) {
            if (raw++ >= static_cast<std::size_t>(std::numeric_limits<int>::max()) || !row)
                return;
            if (exit(*row)) {
                for (const auto& prior : visible) {
                    if (exit(prior) && prior.source_id == row->source_id
                        && prior.from_entry == row->from_entry
                        && same_handle(prior.bracket_origin, row->bracket_origin))
                        return;
                }
            }
            visible.push_back(*row);
        };
        for (const auto& h : live) {
            const auto at = std::find_if(placements.begin(), placements.end(),
                [&](const auto& row) { return row.first == h.incarnation; });
            offer(at == placements.end() ? nullptr : &at->second);
        }
        for (const auto& owner : queued)
            for (const auto& row : owner) offer(&row);
        return static_cast<int>(visible.size());
    }
    void check(const ps::PineExecutionAdapter& adapter) const {
        require(adapter.pending_intent_view().size() == count(),
                "pending size equals the independent vector model after every edit");
    }
};

struct PendingSizeSchedulerHost final : ps::PineStrategyHost {
    PendingSizeModel model;
    unsigned callbacks = 0;
    explicit PendingSizeSchedulerHost(ScratchStore& store) {
        ps::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        auto& core = const_cast<no::WorkingRequestCore&>(
            as_native_consumer(execution_consumer()).request_core());
        core.reset(run_id());
        core.bind_scratch_store(store);
        adapter_.bind_scratch_store(store);
    }
    void on_source_bar(const Bar&) override {
        ++callbacks;
        if (pine_bar_index() == 0) {
            // Invalid native quantities make these real drains remove rows,
            // independently of a successful submit's later remember().
            const auto invalid = limit_order(0.0, 50.0);
            ps::PlacementSnapshot row;
            row.source_id = "drain-invalid";
            row.requested_qty = 0.0;
            for (auto owner : {QueueId::same_bar, QueueId::entry, QueueId::bracket}) {
                adapter_.fixture_queue_state(static_cast<std::uint8_t>(owner),
                    &invalid, &row, {}, {});
                model.queued[PendingSizeModel::segment(owner)].push_back(row);
                model.check(adapter_);
            }
            adapter_.flush_pending_entries();
            for (auto& owner : model.queued) owner.clear();
            model.check(adapter_);
            for (auto owner : {QueueId::coof, QueueId::shadow}) {
                adapter_.fixture_queue_state(static_cast<std::uint8_t>(owner),
                    &invalid, &row, {}, {});
                model.queued[PendingSizeModel::segment(owner)].push_back(row);
                model.check(adapter_);
            }
            return;
        }
        if (pine_bar_index() == 1) {
            // The real broker open drains COOF and expires shadows.
            for (auto& owner : model.queued) owner.clear();
            model.check(adapter_);
            ps::PlacementSnapshot parent;
            parent.source_id = "count-parent";
            parent.requested_qty = 1.0;
            strategy_entry(parent.source_id, true);
            // For these small populations, symbolic raw rows suffice: the
            // count is independent of whether the scheduler queues or submits them.
            auto& rows = model.queued[1];
            rows.push_back(parent);
            model.check(adapter_);
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            strategy_exit("count-exit", parent.source_id, 110.0, 90.0,
                          nan, nan, nan, 100.0, {}, 1.0);
            auto limit = parent;
            limit.family = ps::PineOrderFamily::ExitLimit;
            limit.source_id = "count-exit";
            limit.from_entry = parent.source_id;
            rows.push_back(limit);
            limit.family = ps::PineOrderFamily::ExitStop;
            rows.push_back(limit);
            model.check(adapter_);
            strategy_exit("count-exit", parent.source_id, 112.0, 88.0,
                          nan, nan, nan, 100.0, {}, 1.0);
            model.check(adapter_); // Same-id queued/live leg replacement keeps one exit group.
            return;
        }
        if (pine_bar_index() == 2) {
            // The parent fills at 100, away from both resting exit levels.
            auto& rows = model.queued[1];
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& row) {
                return row.family == ps::PineOrderFamily::Entry;
            }), rows.end());
            model.check(adapter_);
            strategy_cancel("count-exit");
            rows.clear();
            model.check(adapter_); // Materialized/rebound groups must erase under their new key.
            strategy_close("count-parent", {}, 1.0);
            ps::PlacementSnapshot close;
            close.family = ps::PineOrderFamily::Close;
            close.source_id = close.from_entry = "count-parent";
            rows.push_back(close);
            model.check(adapter_);
            strategy_cancel_all();
            rows.clear();
            model.check(adapter_);
        }
    }
};

void s4_pending_size_count_exact_red() {
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    require(ps::PendingProjectionCounts::prefix_lengths({{maximum - 2, 1, maximum, 5, 8, 13}})
                == std::array<std::size_t, 6>{{maximum - 2, 1, 1, 0, 0, 0}},
            "INT_MAX clips raw slots across segment boundaries");
    require(ps::PendingProjectionCounts::prefix_lengths({{
                std::numeric_limits<std::size_t>::max(), 1, 1, 1, 1, 1}})
                == std::array<std::size_t, 6>{{maximum, 0, 0, 0, 0, 0}},
            "prefix arithmetic does not sum overflowing raw lengths");
    Rig rig;
    auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
    Subject s(*store);
    PendingSizeModel model;
    const auto check = [&] { model.check(s.adapter()); };
    const auto edit = [&](ProjectionEdit action, no::RequestHandle h,
                          const ps::PlacementSnapshot* row = nullptr) {
        s.projection(action, h, row);
        model.edit(action, h, row);
        check();
    };
    const auto queue = [&](QueueId owner, const ps::PlacementSnapshot* row) {
        const auto request = limit_order(1.0, 50.0);
        s.queue(owner, row ? &request : nullptr, row);
        auto& rows = model.queued[PendingSizeModel::segment(owner)];
        if (row) rows.push_back(*row);
        else rows.clear();
        check();
    };
    check();
    ps::PendingIntentView unattached;
    require(unattached.size() == 0, "unattached pending view stays empty");
    ps::PlacementSnapshot entry;
    entry.source_id = "count-entry";
    entry.requested_qty = 1.0;
    const auto h = s.submit(limit_order(1.0, 50.0));
    check(); // Native submission alone does not create a source placement.
    edit(ProjectionEdit::remember, h, &entry);
    edit(ProjectionEdit::remember, h, &entry); // Repeated remember is not another raw slot.
    const auto kept = s.replace(h, limit_order(2.0, 51.0), true);
    require(same_handle(kept, h), "count test takes the real keep-handle route");
    check();
    entry.requested_qty = 2.0;
    edit(ProjectionEdit::put, kept, &entry);
    const auto successor = s.replace(kept, limit_order(3.0, 52.0), false);
    require(!same_handle(successor, kept), "ordinary replacement issues a successor");
    check();
    edit(ProjectionEdit::retire, kept);
    edit(ProjectionEdit::erase, kept);
    edit(ProjectionEdit::remember, successor, &entry);

    ps::PlacementSnapshot limit;
    limit.family = ps::PineOrderFamily::ExitLimit;
    limit.source_id = std::string(100, 'x') + std::string("\0one", 4);
    limit.from_entry = "parent";
    limit.bracket_origin = {{"count-origin", 41}, 9};
    auto stop = limit;
    stop.family = ps::PineOrderFamily::ExitStop;
    auto trail = limit;
    trail.family = ps::PineOrderFamily::ExitTrail;
    const no::RequestHandle a{run_id(), 101}, b{run_id(), 102}, c{run_id(), 103};
    edit(ProjectionEdit::name, a); // A hole consumes a raw slot but contributes no row.
    edit(ProjectionEdit::put, a, &limit);
    edit(ProjectionEdit::put, b, &stop); // An unrostered placement contributes nothing.
    edit(ProjectionEdit::name, b);
    edit(ProjectionEdit::remember, c, &trail);
    const no::RequestHandle alias{{"foreign-roster", 88}, a.incarnation};
    edit(ProjectionEdit::name, alias); // Full handles sharing an incarnation are distinct raw slots.
    auto split = limit;
    split.family = ps::PineOrderFamily::Entry;
    edit(ProjectionEdit::put, a, &split); // Both aliases become separate non-exits.
    edit(ProjectionEdit::put, a, &limit);
    split = stop;
    split.source_id = std::string(100, 'x') + std::string("\0two", 4);
    edit(ProjectionEdit::put, b, &split);
    split = stop; split.from_entry = "other-parent";
    edit(ProjectionEdit::put, b, &split);
    split = stop; split.bracket_origin.run.run_number += 1;
    edit(ProjectionEdit::put, b, &split);
    split = stop; split.bracket_origin.run.session_key = "different-session";
    edit(ProjectionEdit::put, b, &split);
    split = stop; split.bracket_origin.incarnation += 1;
    edit(ProjectionEdit::put, b, &split);
    edit(ProjectionEdit::remember, b, &stop); // Whole-row replacement rejoins the old group.
    edit(ProjectionEdit::erase, a); // All aliases become holes before they are unnamed.
    edit(ProjectionEdit::put, a, &limit);
    edit(ProjectionEdit::unname, a); // Removing the first member promotes a later one.
    edit(ProjectionEdit::retire, b); // Removing another member leaves the group present.
    edit(ProjectionEdit::unname, alias);
    edit(ProjectionEdit::erase, c); // Removing the final placement drops the group.
    edit(ProjectionEdit::unname, c);
    edit(ProjectionEdit::retire, successor);

    constexpr std::array<QueueId, 5> owners{{
        QueueId::same_bar, QueueId::entry, QueueId::bracket, QueueId::coof, QueueId::shadow}};
    for (auto owner : owners) {
        queue(owner, &limit);
        queue(owner, &stop); // Equal exit groups merge across every raw owner.
        auto plain = limit;
        plain.family = ps::PineOrderFamily::Order;
        plain.source_id = "queue-plain";
        queue(owner, &plain);
        queue(owner, &plain); // Identical non-exits remain separate.
    }
    s.adapter().cancel("queue-plain");
    for (std::size_t segment : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
        auto& rows = model.queued[segment];
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.source_id == "queue-plain";
        }), rows.end());
    }
    check(); // Predicate erasure observes keys before survivor moves.
    s.adapter().cancel_all();
    for (std::size_t segment : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
        model.queued[segment].clear();
    check(); // cancel_all deliberately retains COOF and shadow rows.
    for (auto owner : owners) queue(owner, nullptr);

    // These real drains requeue their intact rows without needing a native
    // execution point. Omitting either the removal or reinsert doubles/loses rows.
    ps::PlacementSnapshot close;
    close.family = ps::PineOrderFamily::Close;
    close.source_id = "held-close";
    const no::RequestHandle parent{run_id(), 900};
    edit(ProjectionEdit::put, parent, &close);
    auto held = entry;
    held.paired_reversal_parent = parent;
    queue(QueueId::entry, &held);
    queue(QueueId::entry, &held);
    s.adapter().flush_pending_entries();
    check();
    s.adapter().flush_pending_entries();
    check(); // Recycled empty storage is not a logical insertion.
    queue(QueueId::entry, nullptr);
    auto parked = limit;
    parked.reservation_deferred_to_pending_entry = true;
    parked.from_entry = "absent-parent";
    queue(QueueId::bracket, &parked);
    queue(QueueId::bracket, &parked);
    s.adapter().flush_pending_bracket_legs();
    check();
    queue(QueueId::bracket, nullptr);

    // Recovery selects the committed derived count as well as canonical rows.
    queue(QueueId::coof, &stop);
    const auto saved = model;
    status_is(store->begin(), ScratchStatus::ok, "begin count rollback");
    queue(QueueId::shadow, &entry);
    status_is(store->rollback(), ScratchStatus::ok, "rollback count and queue together");
    s.rebind(*store);
    model = saved;
    check();
    const auto hash = source_hash(s);
    store.reset();
    store = std::make_unique<ScratchStore>(rig.config(ScratchOpenMode::reopen), &rig.backing);
    s.rebind(*store);
    check();
    require(source_hash(s) == hash, "count rebind preserves every semantic hash byte");
    s.adapter().reset_for_run();
    model = {};
    check();

    // First binding migrates counts already maintained by the compatibility backend.
    ps::PineExecutionAdapter unbound;
    unbound.fixture_use_memory_auxiliary_tables();
    PendingSizeModel migration;
    unbound.fixture_projection_state(static_cast<std::uint8_t>(ProjectionEdit::name), a, nullptr);
    migration.edit(ProjectionEdit::name, a);
    migration.check(unbound);
    unbound.fixture_projection_state(static_cast<std::uint8_t>(ProjectionEdit::put), a, &limit);
    migration.edit(ProjectionEdit::put, a, &limit);
    migration.check(unbound);
    Rig migration_rig;
    ScratchStore migrated(migration_rig.config(), &migration_rig.backing);
    unbound.bind_scratch_store(migrated);
    migration.check(unbound);
    unbound.fixture_projection_state(static_cast<std::uint8_t>(ProjectionEdit::erase), a, nullptr);
    migration.edit(ProjectionEdit::erase, a);
    migration.check(unbound); // Exercise the migrated group multiplicity, not just its scalar.
    unbound.fixture_projection_state(static_cast<std::uint8_t>(ProjectionEdit::unname), a, nullptr);
    migration.edit(ProjectionEdit::unname, a);
    migration.check(unbound);
    unbound.fixture_projection_state(static_cast<std::uint8_t>(ProjectionEdit::put), a, &entry);
    migration.edit(ProjectionEdit::put, a, &entry);
    migration.check(unbound); // The migrated hole membership must have been removed too.

    Rig scheduled_rig;
    ScratchStore scheduled_store(scheduled_rig.config(), &scheduled_rig.backing);
    PendingSizeSchedulerHost scheduled(scheduled_store);
    const std::array<Bar, 3> bars{{
        {100, 100, 100, 100, 1, 1736121600000LL},
        {100, 100, 100, 100, 1, 1736121660000LL},
        {100, 100, 100, 100, 1, 1736121720000LL}}};
    scheduled.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(scheduled.last_error().empty() && scheduled.callbacks == bars.size(),
            "count test executes every real scheduler callback and drain");
}

void s4_pending_size_read_bound_red() {
    constexpr std::array<std::size_t, 2> populations{{17, 65}};
    std::array<std::uint64_t, 2> size_reads{};
    for (std::size_t sample = 0; sample < 2; ++sample) {
        const auto n = populations[sample];
        Model model;
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        (void)populate_pending_read_bound(s, model, n);
        const auto& pending = s.adapter().pending_intent_view();
        require(pending.size() == static_cast<int>(n), "warm pending size is exact");
        const auto before = store.stats().page_reads;
        const auto count = pending.size();
        const auto after = store.stats().page_reads;
        size_reads[sample] = after - before;
        require(count == static_cast<int>(n), "measured pending size is exact");
        std::printf("pending-size-read-bound n=%zu size_reads=%llu\n", n,
                    static_cast<unsigned long long>(size_reads[sample]));
        std::fflush(stdout);
    }
    // 512-byte pages give 58 ordered-leaf entries, 29 separators, 14 handle
    // entries/leaf and 60 handle children/node (scratch_store_impl.hpp Core).
    // These short axis-0 records at N <= 65 need at most d=3 ordered levels
    // and h=3 handle levels. A 32-byte key find costs at most
    // L=2*d+2+[5*(d-1)+7]*(h+2)+2*h+1=100 page references; reading an
    // eight-byte maintained count adds 2*h+1, hence <=107 even without hits.
    // Round that to 112 growth slack, with a 128 absolute ceiling. Zero-read
    // scalar access also passes. Neither allowance depends on observed reads.
    // On the old head size(65) fetches T(65)=2145 rows. Each placement find
    // compares an ordered page, >=2 handle pages and a key page through two
    // cache slots: >=2 reads/fetch, so >=4290 reads, >33 times the ceiling.
    constexpr std::uint64_t slack = 112, absolute = 128;
    require(size_reads[1] <= size_reads[0] + slack,
            "one pending size call has population-independent store reads");
    require(size_reads[1] <= absolute,
            "one pending size call reads at most a maintained scalar allowance");
}

void s4_pending_copy_read_bound_red() {
    constexpr std::array<std::size_t, 2> populations{{17, 65}};
    std::array<std::array<std::uint64_t, 2>, 2> copy_reads{};
    for (std::size_t sample = 0; sample < 2; ++sample) {
        const auto n = populations[sample];
        Model model;
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        const auto endpoints = populate_pending_read_bound(s, model, n);
        const auto& pending = s.adapter().pending_intent_view();
        const std::array<int, 2> indices{{0, static_cast<int>(n - 1)}};
        for (std::size_t endpoint = 0; endpoint < 2; ++endpoint) {
            pf_pending_order_v1_t warm{};
            require(pending.copy_v1(indices[endpoint], &warm) >= 0,
                    "warm pending observer row reads successfully");
            pf_pending_order_v1_t row{};
            const auto before = store.stats().page_reads;
            const auto status = pending.copy_v1(indices[endpoint], &row);
            const auto after = store.stats().page_reads;
            copy_reads[sample][endpoint] = after - before;
            require(status >= 0, "measured pending observer row reads successfully");
            { OracleWork oracle;
              const auto expected = model.placements.find(row.incarnation);
              require(row.incarnation == endpoints[endpoint].incarnation
                          && expected != model.placements.end()
                          && row.qty == expected->second.requested_qty,
                      "pending observer returns the selected input placement value");
            }
        }
        std::printf("pending-copy-read-bound n=%zu first_reads=%llu last_reads=%llu\n", n,
                    static_cast<unsigned long long>(copy_reads[sample][0]),
                    static_cast<unsigned long long>(copy_reads[sample][1]));
        std::fflush(stdout);
    }
    // Permit a factor of two for indexed rank selection and the small index
    // height change, while retaining the same fixed snapshot decode. Each
    // endpoint has its own warm-up, and no size() call is part of this case.
    // The old Entry path performs 2*T(i+1)+N raw fetches per copy, including
    // short-seed reselection and siblings: first/last = 19/323 at N=17 and
    // 67/4355 at N=65. Its last/first fetch ratio is N. With F>=100 fixed
    // placement chunk-piece reads, the two-slot geometry bounds a raw fetch
    // by 6*F <= reads <= 112*F+669 at N=65 (REPORT-o1s4red.md). Thus even
    // the conservative last/first page-read ratio exceeds 3.28, above 2.
    constexpr std::uint64_t factor = 2;
    for (const auto& reads : copy_reads) {
        require(reads[1] <= factor * reads[0],
                "last pending row costs at most twice the first row's store reads");
    }
    require(copy_reads[1][1] <= factor * copy_reads[0][1],
            "last pending row's store reads grow at most logarithmically");
}

// Fixed memory, S4-RE (548). Input: P physical lots, two successive terminal
// marks, row(index) reads in lot order, clear, then another mark; nonfinite
// mark refusal. fails to compile: range_end_trades_.row(index), in addition
// to the store binding. With the old vector reader substituted, the fixed
// frame allowance fails in append_open_position_report_rows itself (9048),
// before any C accessor. OpenReportRowSource/materialized_ do not exist here.
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Fixed memory" (S4-RE), clause 3 — repeated marks expose one row per lot, exact row values/left-fold P&L and fixed heap.
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
        require(host.calc_commission(100.0, 1.0) == 0.0, "terminal fixture has zero default fees");
        host.set_syminfo_pointvalue(1.0);
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
            host.fixture_open_lot(PositionSide::LONG, &lot);
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
                require(within_four_ulps(row.pnl_pct, pnl / (e.price * e.qty) * 100.0),
                        "percentage P&L uses this physical lot's entry cost");
                // Full lot, zero fees, and pre-existing excursion maxima
                // greater than every candidate mark's excursion: preserve
                // those economic inputs, without calling the trade builder.
                require(row.max_runup >= e.favorable && row.max_drawdown >= e.adverse,
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Transactions and retain windows" (S34-T), clause 4 — every injected update-write failure stays rollback-only and recovers the old committed relation after reopen.
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
            const auto cohort = s.cohort("tx");
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-P1), clause 5 — retained placement alone pins an ended origin until actual erasure.
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
    heap.seal();
    const auto resident = store.stats().resident_bytes;
    const auto live = s.submit(limit_order(2.0, 45.0));
    require(same_handle(live, origin), "fixture uses a genuinely issued origin");
    s.cancel(live);
    s.append("p1", origin);
    s.append("p1", expected.origins.back());
    s.projection(ProjectionEdit::put, origin, &evidence);
    require(s.core().find_live(origin) == nullptr, "native order ended");
    require(s.adapter().pending_intent_view().size() == 0
                && s.host.source_pending_view().empty(), "no live source name");
    isolated(model, expected, origin, 0);
    s.retire(); model.retire(expected);
    check_roster(s, "p1", expected);
    const auto retained = source_hash(s);
    s.projection(ProjectionEdit::erase, origin);
    require(source_hash(s) != retained, "actual placement erasure changes semantic state");
    { OracleWork oracle; model.placements.erase(origin.incarnation); model.retire(expected); }
    s.retire();
    check_roster(s, "p1", expected);
    require(expected.base == expected.origins.size(), "removing the sole pin releases the suffix");
    rig.accounting(store);
    heap.fixed(store, resident);
}

// The current-membership indexes and the five predicates, S4-P3 (635).
// Input: same origin in two cohorts, ZERO-valued present keys, erase one key
// only. Other pins false. fails to compile: Subject store bindings and
// OriginRoster::visit_positions_of. A resident member map must be replaced,
// not changed into units > 0; expected bases below follow key presence alone.
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-P3), clause 7 — a present zero-valued live-unit key pins only its own cohort until erased.
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
    heap.seal();
    const auto resident = store.stats().resident_bytes;
    const auto real = s.submit(limit_order(1.0, 43.0));
    require(same_handle(real, origin), "issued input handle");
    s.cancel(real);
    for (const char* id : {"alpha", "beta"}) {
        s.append(id, origin);
        (void)s.cohort_state(id, CohortEdit::put_units, origin, 0.0);
        isolated(model, model.cohorts.at(id), origin, 2);
    }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    (void)s.cohort_state("alpha", CohortEdit::erase_units, origin);
    { OracleWork oracle; model.cohorts.at("alpha").units.erase(origin.incarnation); }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    require(model.cohorts.at("alpha").base != model.cohorts.at("beta").base,
            "erasing alpha cannot unpin beta's present zero key");
    (void)s.cohort_state("beta", CohortEdit::erase_units, origin);
    { OracleWork oracle; model.cohorts.at("beta").units.erase(origin.incarnation); }
    s.retire();
    for (auto& c : model.cohorts) { model.retire(c.second); check_roster(s, c.first, c.second); }
    rig.accounting(store);
    heap.fixed(store, resident);
}

// The current-membership indexes and the five predicates, S4-P4 (636).
// Input: duplicate opened handles, same incarnation in different run numbers
// AND different session text, remove duplicates singly. Other four pins false.
// fails to compile: Subject bindings and OriginRoster::visit_positions_of.
// The comparison must retain full-handle identity and ordered multiplicity,
// rather than making an incarnation-only membership index.
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-P4), clause 8 — opened membership uses full handles and preserves duplicate pins until the last removal.
void s4_predicate_opened_full_handle_red() {
    for (std::size_t length : {std::size_t{17}, std::size_t{8193}, std::size_t{65539}}) {
        // Intern caller-owned identities before measuring. Both the PRESENT
        // member and a same-length, last-byte-different nonmember are long;
        // a fixed prefix, an incarnation bucket or a copied text key is not
        // enough. Length never contributes to F.
        const std::string session(length, 'x');
        std::string foreign_session = session;
        foreign_session.back() = 'y';
        Model model;
        model.run = {session, 7};
        const auto a = model.submit(1.0, 42.0);
        model.erase(a);
        const no::RequestHandle b{{session, 8}, a.incarnation};
        const no::RequestHandle c{{foreign_session, a.run.run_number}, a.incarnation};
        auto& expected = model.cohorts["opened"];
        // The foreign rows come FIRST: incarnation-only equality would pin
        // them and hide the correct frontier at the following real member.
        expected.append(b); expected.append(c); expected.append(a); expected.append(a);
        expected.opened = {a, a};
        Rig rig;
        HeapCensus heap;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        heap.seal();
        const auto resident = store.stats().resident_bytes;
        s.core().reset(model.run);
        const auto real = s.submit(limit_order(1.0, 42.0));
        require(same_handle(real, a), "issued input handle");
        s.cancel(real);
        for (auto h : expected.origins) s.append("opened", h);
        for (auto h : expected.opened) (void)s.cohort_state("opened", CohortEdit::append_opened, h);
        isolated(model, expected, a, 3);
        for (unsigned remaining = 2; remaining != 0; --remaining) {
            s.retire(); model.retire(expected);
            check_roster(s, "opened", expected);
            require(expected.opened.size() == remaining, "oracle retains duplicate multiplicity");
            check_current_membership(s, "opened", model);
            (void)s.cohort_state("opened", CohortEdit::erase_opened, a);
            { OracleWork oracle; remove_one(expected.opened, a); }
            check_current_membership(s, "opened", model);
        }
        s.retire(); model.retire(expected);
        check_roster(s, "opened", expected);
        require(expected.base == expected.origins.size(), "last full-handle member unpins the prefix");
        rig.accounting(store);
        heap.fixed(store, resident);
    }
}

// The current-membership indexes and the five predicates, S4-P5 (637).
// Input: canonical cohort, long normal/kept replacement chain, live and
// terminal successors after history retirement, unknown and foreign handles.
// Only P5 pins during retirement (all successors have ended by then).
// fails to compile: Subject bindings and OriginRoster::visit_positions_of.
// Root reads must use the selected store and propagate I/O, never false/self.
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-P5), clause 9 — live/terminal successors use canonical cohort roots after history retirement and root I/O failure throws.
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
    heap.seal();
    const auto resident = store->stats().resident_bytes;
    auto current = s.submit(limit_order(1.0, 41.0));
    require(same_handle(current, root), "real root matches model");
    const auto cohort = s.cohort("canonical");
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
    heap.fixed(*store, resident);
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
void request_name(no::Request& row, NameField field, no::RequestHandle h,
                  std::size_t length) {
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
        // A long QUEUED input, including names unrelated to the
        // target. Only one end names h; checking only front() cannot pass.
        for (std::size_t i = 0; i < length; ++i)
            owners.openings.push_back({h.run, h.incarnation + 10000 + i});
        owners.openings[field == NameField::openings_first ? 0 : owners.openings.size() - 1] = h;
        row.owner = std::move(owners);
        break;
    }
    default: break;
    }
}
QueueId queue_id(NameOwner owner) {
    switch (owner) {
    case NameOwner::bracket: return QueueId::bracket;
    case NameOwner::entry: return QueueId::entry;
    case NameOwner::delayed: return QueueId::delayed;
    case NameOwner::sell: return QueueId::sell;
    case NameOwner::rearm: return QueueId::rearm;
    case NameOwner::same_bar: return QueueId::same_bar;
    case NameOwner::shadow: return QueueId::shadow;
    case NameOwner::coof: return QueueId::coof;
    case NameOwner::revival: return QueueId::revival;
    case NameOwner::anchored: return QueueId::anchored;
    default: throw std::runtime_error("fixture owner is a queue");
    }
}

struct InstalledName {
    NameOwner owner;
    no::RequestHandle carrier{};
};

InstalledName install_name(Subject& s, NameCase input, no::RequestHandle origin,
                           std::size_t length = 17) {
    const auto field = input.field;
    const auto owner = input.owner;
    InstalledName result{owner, {}};
    switch (owner) {
    case NameOwner::source_live:
        result.carrier = origin;
        s.projection(ProjectionEdit::name, origin);
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
        s.projection(ProjectionEdit::put, result.carrier, &row);
        if (owner == NameOwner::source_snapshot) {
            s.projection(ProjectionEdit::name, result.carrier);
            s.cancel(result.carrier); // ensure ONLY the source enumeration can see it
        }
        break;
    }
    case NameOwner::native_request: {
        no::Request request = limit_order(1.0, 48.0);
        const bool many = field == NameField::openings_first || field == NameField::openings_last;
        if (!many) request_name(request, field, origin, length);
        no::CommandContext context;
        context.opening = no::OpeningObservation{origin, 1, no::PositionNonflat{1, no::Side::Long}, true};
        if (many) {
            // Native enrollment has the existing unique, nonzero membership
            // rules. Each length row really enrolls that many issued handles.
            // Only caller input-vector work is excluded from the census;
            // issuance, cancellation and apply_submit all remain charged.
            no::BindOpenings owners;
            owners.cycle = 1;
            { OracleWork oracle;
              owners.openings.reserve(length); context.openings.reserve(length);
            }
            const auto target = field == NameField::openings_first ? 0 : length - 1;
            for (std::size_t i = 0; i < length; ++i) {
                const auto h = i == target ? origin : s.submit(limit_order(1.0, 49.0));
                { OracleWork oracle;
                  owners.openings.push_back(h);
                  context.openings.push_back({h, 1, no::PositionNonflat{1, no::Side::Long}, true});
                }
                if (i != target) s.cancel(h);
            }
            request.intent = no::Reduce{no::ExplicitUnits{1}};
            request.owner = std::move(owners);
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
    case NameOwner::entry:
    case NameOwner::delayed:
    case NameOwner::sell:
    case NameOwner::rearm:
    case NameOwner::same_bar:
    case NameOwner::shadow:
    case NameOwner::coof:
    case NameOwner::revival:
    case NameOwner::anchored: {
        no::Request request;
        ps::PlacementSnapshot snapshot;
        { OracleWork oracle; // Borrowed fixture input, never a production call.
          request = limit_order(1.0, owner == NameOwner::anchored ? 44.0 : 40.0);
          snapshot_name(snapshot, field, origin);
          request_name(request, field, origin, length);
        }
        const bool snapshot_only = owner == NameOwner::sell || owner == NameOwner::rearm
            || owner == NameOwner::shadow || owner == NameOwner::revival;
        s.queue(queue_id(owner), snapshot_only ? nullptr : &request,
                owner == NameOwner::anchored ? nullptr : &snapshot,
                field == NameField::anchored_handle ? origin : no::RequestHandle{},
                field == NameField::anchored_parent ? origin : no::RequestHandle{});
        break;
    }
    }
    return result;
}

void erase_name(Subject& s, InstalledName name) {
    switch (name.owner) {
    case NameOwner::source_live:
        s.projection(ProjectionEdit::unname, name.carrier); break;
    case NameOwner::source_snapshot:
    case NameOwner::native_snapshot:
        s.projection(ProjectionEdit::erase, name.carrier);
        s.retire_handle(name.carrier);
        if (s.core().find_live(name.carrier)) s.cancel(name.carrier);
        break;
    case NameOwner::native_live:
    case NameOwner::native_root:
    case NameOwner::native_request:
        s.cancel(name.carrier); break;
    case NameOwner::bracket:
    case NameOwner::entry:
    case NameOwner::delayed:
    case NameOwner::sell:
    case NameOwner::rearm:
    case NameOwner::same_bar:
    case NameOwner::shadow:
    case NameOwner::coof:
    case NameOwner::revival:
    case NameOwner::anchored: s.queue(queue_id(name.owner)); break;
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-P2), clause 6 — each current owner/name field pins until the last reference is removed; zero never pins.
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
    for (const auto input : inputs) for (const bool primary_first : {false, true})
    for (std::size_t length : {std::size_t{17}, std::size_t{1025}, std::size_t{4097}}) {
        const bool many = input.field == NameField::openings_first
            || input.field == NameField::openings_last;
        if (!many && length != 17) continue; // This axis varies BindOpenings only.
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
        heap.seal();
        const auto resident = store.stats().resident_bytes;
        require(same_handle(s.submit(limit_order(1.0, 40.0)), origin), "issued named input");
        const auto primary = install_name(s, input, origin, length);
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
        heap.fixed(store, resident);
    }
    // All explicit names zero: zero is excluded, even when a queue physically
    // contains a row. It must not pin a zero-incarnation roster position.
    Model zero;
    const no::RequestHandle h{zero.run, 0};
    auto& expected = zero.cohorts["zero"];
    expected.append(h);
    Rig rig;
    HeapCensus heap;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    heap.seal();
    const auto resident = store.stats().resident_bytes;
    s.append("zero", h);
    (void)install_name(s, {NameOwner::entry, NameField::bracket}, h);
    (void)install_name(s, {NameOwner::anchored, NameField::anchored_parent}, h);
    s.retire(); zero.retire(expected);
    check_roster(s, "zero", expected);
    rig.accounting(store);
    heap.fixed(store, resident);
}

// The current-membership indexes and the five predicates, S4-I (638).
// Input: all 32 truth combinations, duplicate global positions/full-handle
// aliases, pin at position 1 before a large suffix, release, replace/erase,
// spill and run reset. fails to compile: Subject bindings and
// OriginRoster::visit_positions_of. The exact assertion is global frontier/
// position equality PLUS the construction heap bound behind the pinned suffix.
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "The current-membership indexes and the five predicates" (S4-I), clause 10 — all 32 pin combinations preserve the modeled prefix frontier/global positions through spill, release and reset.
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
        s.adapter().reset_for_run();
        const auto empty_source = source_hash(s);
        const auto real = s.submit(limit_order(1.0, 39.0));
        require(same_handle(real, pin), "issued frontier pin");
        const auto cohort = s.cohort("frontier");
        if (mask & 16) { s.core().cohort_add(cohort, real); s.cohort_note(); }
        s.cancel(real);
        for (auto h : c.origins) s.append("frontier", h);
        const ps::PlacementSnapshot initial;
        if (mask & 1) s.projection(ProjectionEdit::put, pin, &initial);
        if (mask & 2) (void)install_name(s, {NameOwner::entry, NameField::bracket}, pin);
        if (mask & 4) (void)s.cohort_state("frontier", CohortEdit::put_units, pin, 0.0);
        if (mask & 8) {
            (void)s.cohort_state("frontier", CohortEdit::append_opened, pin);
            (void)s.cohort_state("frontier", CohortEdit::append_opened, pin);
        }
        s.retire(); model.retire(c);
        check_roster(s, "frontier", c);
        if (mask != 0) {
            require(c.origins.size() - c.base > 4096, "a front pin retains the large suffix");
            // Remove one pin at a time: a later disjunct still pins, even
            // when a derived cache was last populated under an earlier one.
            for (unsigned bit = 0; bit < 5; ++bit) {
                if (!(mask & (1u << bit))) continue;
                if (bit == 0) {
                    s.projection(ProjectionEdit::erase, pin);
                    OracleWork oracle; model.placements.erase(pin.incarnation);
                } else if (bit == 1) {
                    s.queue(QueueId::entry);
                    OracleWork oracle; model.names.clear();
                } else if (bit == 2) {
                    (void)s.cohort_state("frontier", CohortEdit::erase_units, pin);
                    OracleWork oracle; c.units.clear();
                } else if (bit == 3) {
                    (void)s.cohort_state("frontier", CohortEdit::erase_opened, pin);
                    { OracleWork oracle; remove_one(c.opened, pin); }
                    s.retire(); model.retire(c); check_roster(s, "frontier", c);
                    (void)s.cohort_state("frontier", CohortEdit::clear_opened);
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
        (void)s.cohort_state("frontier", CohortEdit::append_opened, reused);
        ps::PlacementSnapshot replacement;
        replacement.requested_qty = 3.0 + mask;
        { OracleWork oracle; model.placements[reused.incarnation] = replacement; }
        s.projection(ProjectionEdit::put, reused, &initial);
        s.projection(ProjectionEdit::put, reused, &replacement);
        // Borrow the ordinary pending projection to observe this retained
        // placement; withdraw the temporary name before asking retirement.
        s.projection(ProjectionEdit::name, reused);
        const auto& pending = s.adapter().pending_intent_view();
        pf_pending_order_v1_t row{};
        require(pending.size() == 1 && pending.copy_v1(0, &row) >= 0
                    && row.qty == model.placements.at(reused.incarnation).requested_qty,
                "replacement updates the existing lookup row");
        s.projection(ProjectionEdit::unname, reused);
        s.projection(ProjectionEdit::erase, reused);
        { OracleWork oracle; model.placements.erase(reused.incarnation); }
        s.retire(); model.retire(c); check_roster(s, "frontier", c);
        (void)s.cohort_state("frontier", CohortEdit::clear_opened);
        { OracleWork oracle; c.opened.clear(); model.retire(c); }
        s.retire(); check_roster(s, "frontier", c);
        rig.accounting(store);
        require(store.stats().backing_pages > rig.config().page_count,
                "suffix/index records spilled beyond the resident slots");
        s.adapter().reset_for_run();
        s.core().reset(run_id(8));
        require(source_hash(s) == empty_source
                    && s.adapter().pending_intent_view().size() == 0
                    && s.host.source_pending_view().empty()
                    && s.roster("frontier") == nullptr,
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Digests" (S4-D), clause 11 — native/source hashes ignore storage changes while a durable live-unit value changes the source hash.
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
        const auto cohort = s.cohort("hash");
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
            s.source_key(i + 10, &h);
            (void)s.cohort_state("hash", CohortEdit::append_opened, h);
            (void)s.cohort_state("hash", CohortEdit::put_units, h, i % 2 ? -0.0 : 2.0);
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
            check_current_membership(s, "hash", model);
            check_source_keys(s, model);
            check_pending_placements(s, model);
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
            s.projection(ProjectionEdit::erase, h);
            (void)s.cohort_state("hash", CohortEdit::erase_units, h);
            (void)s.cohort_state("hash", CohortEdit::erase_opened, h);
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
        const auto key = model.cohorts.at("hash").units.begin()->first;
        const auto units = model.cohorts.at("hash").units.at(key);
        (void)s.cohort_state("hash", CohortEdit::put_units, {model.run, key}, units + 3.0);
        const auto changed = semantic_hashes(s);
        require(changed.source != old.source, "independent live-unit value is hashed");
        (void)s.cohort_state("hash", CohortEdit::put_units, {model.run, key}, units);
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
        require(s.roster("hash") == nullptr, "reset clears the old cohort's roster/index relation");
        const auto empty_cohorts = source_hash(s);
        (void)s.cohort_state("", CohortEdit::clear_all);
        require(source_hash(s) == empty_cohorts, "reset leaves no other durable cohort/index rows");
        require(s.adapter().pending_intent_view().size() == 0
                    && s.host.source_pending_view().empty(), "reset clears source pending projections");
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
// Spec: scratch-store.md (pinned copy) §"O(1) seams s3 and s4" / "Statuses" (S34-S), clause 12 — capacity spills; I/O/corruption/misuse/cancellation stay distinct from proven absence and failed lookups throw.
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
    const auto cohort = s.cohort("status");
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
    {"s4_islands_small_n_probe_red", &s4_islands_small_n_probe_red},
    {"s4_roster_scan_read_bound_red", &s4_roster_scan_read_bound_red},
    {"s4_roster_index_exact_red", &s4_roster_index_exact_red},
    {"s4_pending_size_read_bound_red", &s4_pending_size_read_bound_red},
    {"s4_pending_size_count_exact_red", &s4_pending_size_count_exact_red},
    {"s4_pending_copy_read_bound_red", &s4_pending_copy_read_bound_red},
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

// The supervisor handoff for this file (citation audit, interface assumptions, CMake
// registration lines) is kept outside the source, in lab-o1s4.handoff.txt beside the brief.

