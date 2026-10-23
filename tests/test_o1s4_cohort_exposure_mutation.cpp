// S4E T2, round 1b. EDIT ONLY: base/mutant verdicts belong to the box run.
//
// Mutation ceiling derivation (independent of any measured count).
// E2 Q2: A(L)=40L+64 loads, W(L)=24L+64 puts, <=2 erases;
// L(n)=ceil(1.45*log2(n+2))+2. Charge BOTH the unit AVL and one position
// AVL of this depth, even for a replacement. PLAN-LANE-E 3.3 combines the
// position structure/summary in ONE 936-byte record (h+2 puts for a replace).
// A whole final evaluation is charged too: <=2046 crossing additions, with
// a descent and a return path each, plus one accepted path: <=4093*L loads.
// Add 64 loads/64 puts for the fixed descriptor/slot/reclaim/control frames.
// Thus loads <=4173*L+192, puts <=48*L+192, erases <=4.
//
// Store per-put key-find factor, including the current replacement descent:
// scratch_store_impl.hpp::Core::{descend,compare_with,put_impl}, with the
// 512-byte capacities (29 separators / 58 leaf entries), binary searches:
// F(d,h)=2d+2+(5(d-1)+7)(h+8)+2h+5.
// h+8 charges radix resolution, the ordered page and lexical's <=3 reads of
// a <=96-byte key (including two-cell boundary reads/hops). The final 2h+5
// charges equality point/read and both key cells. Use the substrate's FULL
// legal maxima, d=kMaxDepth+1=49, h=kMaxHeight=12; no fitted store height.
// A <=936-byte direct point/read costs C=2h+2*ceil(936/56)+4=62 references.
// P=F+2h+4+8*(ceil(96/56)+ceil(936/56))+16d+16h+64:
// hget/hset and fixed framing; <=8 create/retire/undo/free references per
// cell (Core::{touch,cell_alloc,blob_create,blob_free}); <=16 per ordered
// and radix split level (Core::{tree_insert,hset}), and 64 commit frames.
// Charge each of the <=4 direct erases as TWO P, for its own key descent
// and unlink/ghost/free work. For these short fixed inputs this also
// overcharges their paged blobs by using the smaller 56-byte cell payload.
//
// For g=ceil(log2(max(U,2))), L(U+1)<=5g at every sampled U>=17.
// Loads <=21057g, puts plus erase charges <=440g. Therefore
// reads <=(21057*C+440*P)*g <= c*g^2 with c=21057*C+440*P.
// The separate ratio gate uses the ACTUAL log2 ratio, squared, with fixed
// multiplicative slack 5/4; it is essential because c uses legal substrate
// maxima, rather than assuming cache hits or fitting the fixture's geometry.
// Fixed metadata/keys here are <=96 bytes; this is not a variable-SourceId
// bound. No all-population restore/migration or exposure read is hidden in it.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "cohort_exposure_oracle.hpp"

#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/detail/working_store.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_COHORT_EXPOSURE_HASH_GOLDEN
#error "CMake must locate the cohort exposure hash golden data"
#endif

namespace {
using namespace pineforge;
using namespace pineforge::detail;
namespace no = pineforge::native_order;
namespace ps = pineforge::source;
namespace ws = pineforge::detail::working_store;
namespace ce = cohort_exposure_test;
constexpr std::size_t page_bytes = 512;
constexpr std::size_t model_capacity = 2048; // Test inputs only, never an engine limit.
constexpr std::uint64_t golden_seed = 0x4f31533445ULL;
std::size_t checkpoints = 0;

void require(bool ok, const char* message) {
    ++checkpoints;
    ce::require(ok, message);
}
void status_is(ScratchStatus actual, ScratchStatus want, const char* message) {
    if (actual != want)
        std::fprintf(stderr, "%s: status=%u expected=%u\n", message,
                     static_cast<unsigned>(actual), static_cast<unsigned>(want));
    require(actual == want, message);
}
void equal_bits(double actual, std::uint64_t want, const char* message) {
    if (ce::bits(actual) != want)
        std::fprintf(stderr, "%s: actual=%016llx expected=%016llx\n", message,
                     static_cast<unsigned long long>(ce::bits(actual)),
                     static_cast<unsigned long long>(want));
    require(ce::bits(actual) == want, message);
}

// E3's const-only canonical-facts access; never reads a derived E record.
template<class Tag, auto Member> struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
struct CohortsTag { friend auto access(CohortsTag); };
template struct PrivateAccess<CohortsTag, &ps::PineExecutionAdapter::cohorts_by_id_>;

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
struct Charges {
    std::uint64_t reads = 0, writes = 0, syncs = 0, failures = 0;
    static void charge(void* context, const ScratchIoCharge& c) noexcept {
        auto& self = *static_cast<Charges*>(context);
        if (c.operation == ScratchIoOperation::read_page) ++self.reads;
        else if (c.operation == ScratchIoOperation::write_page) ++self.writes;
        else ++self.syncs;
        if (c.status != ScratchStatus::ok) ++self.failures;
    }
    ScratchIoObserver observer() { return {this, &charge}; }
};

// test_o1s4_queue_red.cpp::FaultBacking idiom. Only the OUTER backing has
// an observer. Forwarding to the test-memory medium is not charged twice.
class FaultBacking final : public ScratchBacking {
public:
    FaultBacking(FixedMemoryScratchBacking& medium, ScratchIoObserver observer)
        : ScratchBacking(medium.kind(), medium.page_size(), observer), medium_(medium) {}
    std::uint64_t page_count() const noexcept override { return medium_.page_count(); }
    void arm(std::uint64_t nth, bool tear) { remaining_ = nth; tear_ = tear; }
    void disarm() { remaining_ = 0; }
    std::uint64_t hits = 0;
protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        return medium_.read_page(id, out);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        if (remaining_ && --remaining_ == 0) {
            ++hits;
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
        return medium_.sync(durability);
    }
private:
    FixedMemoryScratchBacking& medium_;
    std::uint64_t remaining_ = 0;
    bool tear_ = false;
};
struct Rig {
    std::vector<std::byte> storage;
    Charges charges;
    FixedMemoryScratchBacking medium;
    FaultBacking backing;
    Rig() : storage(131072 * page_bytes),
            medium({storage.data(), storage.size()}, page_bytes),
            backing(medium, charges.observer()) {}
    ScratchStoreConfig config(ScratchOpenMode mode = ScratchOpenMode::create_empty) const {
        return {2, page_bytes, 1, 1, {0x4f315334, nullptr, &lexical}, mode};
    }
};
struct Host final : ps::PineStrategyHost {
    const Bar warmup{100.0, 101.0, 99.0, 100.0, 1.0, 1736121600000LL};
    ps::PineExecutionAdapter& adapter() { return adapter_; }
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    no::WorkingRequestCore& core() {
        return const_cast<no::WorkingRequestCore&>(consumer().request_core());
    }
    std::uint64_t hash() const {
        BrokerStateHashSink sink; // Fresh basis, never a test-side copy of the fold.
        hash_host_extension(sink);
        return sink.h;
    }
    void on_source_bar(const Bar&) override {}
};
enum class Edit : std::uint8_t {
    lookup, ensure, append_origin, append_opened, erase_opened, clear_opened,
    put_units, erase_units, clear_units, clear_all
};

// Fixed test storage makes the live-byte census independent of oracle size.
// Only the temporary oracle vector allocates; it is destroyed before sampling.
struct Model {
    struct Unit { std::uint64_t incarnation = 0; double value = 0; };
    std::string id;
    bool present = false;
    std::array<no::RequestHandle, model_capacity> origins{}, opened{};
    std::array<Unit, model_capacity> units{};
    std::size_t origin_count = 0, opened_count = 0, unit_count = 0;
    std::size_t locate(std::uint64_t incarnation) const {
        for (std::size_t i = 0; i < unit_count; ++i)
            if (units[i].incarnation == incarnation) return i;
        return unit_count;
    }
    void put(no::RequestHandle h, double value) {
        auto at = locate(h.incarnation);
        if (at == unit_count) {
            require(unit_count < units.size(), "test model unit capacity");
            ++unit_count;
        }
        units[at] = {h.incarnation, value};
    }
    void erase(no::RequestHandle h) {
        const auto at = locate(h.incarnation);
        if (at != unit_count) units[at] = units[--unit_count];
    }
    std::uint64_t expected() const {
        std::vector<ce::Row> rows;
        for (std::size_t p = 0; p < origin_count; ++p) {
            const auto at = locate(origins[p].incarnation);
            if (at != unit_count) rows.push_back({p, origins[p].incarnation, units[at].value});
        }
        return ce::bits(ce::oracle(rows));
    }
};
struct Subject {
    Host host;
    std::array<Model, 2> models{};
    explicit Subject(ScratchStore& store) {
        status_is(store.status(), ScratchStatus::ok, "fixture store opens");
        require(store.page_size() == page_bytes, "fixture keeps 512-byte pages");
        host.set_syminfo_timezone("UTC");
        host.set_syminfo_session("24x7");
        host.set_syminfo_mintick(0.25);
        require(host.stream_begin(&host.warmup, 1, "1", "1"), "native stream begins");
        require_realtime();
        rebind(store); // Begin BEFORE binding; begin resets both canonical owners.
        require(!store.transaction_active(), "bindings leave a committed boundary");
    }
    void require_realtime() const {
        const auto state = host.native_state();
        require(host.last_error().empty() && host.stream_is_realtime()
                    && state.kind == NativeLifecycleKind::Running
                    && state.phase == NativeRunPhase::Realtime,
                "fixture runs in idle native Realtime");
    }
    void rebind(ScratchStore& store) {
        host.core().bind_scratch_store(store);
        host.adapter().bind_scratch_store(store);
    }
    no::RequestHandle handle(std::uint64_t incarnation) {
        return {host.consumer().request_core().identity(), incarnation};
    }
    auto raw(const std::string& id, Edit edit, no::RequestHandle h = {}, double value = 0) {
        return host.adapter().fixture_cohort_state(id, static_cast<std::uint8_t>(edit), h, value);
    }
    Model& ensure(const std::string& id, std::size_t slot = 0) {
        auto& m = models[slot];
        require(!m.present, "model slot used once before a table clear");
        require(raw(id, Edit::ensure).first.value != 0, "ensure creates a real native cohort");
        m.id = id; m.present = true;
        return m;
    }
    void append(Model& m, no::RequestHandle h) {
        (void)raw(m.id, Edit::append_origin, h);
        require(m.origin_count < m.origins.size(), "test model origin capacity");
        m.origins[m.origin_count++] = h;
    }
    void opened(Model& m, no::RequestHandle h) {
        (void)raw(m.id, Edit::append_opened, h);
        require(m.opened_count < m.opened.size(), "test model opened capacity");
        m.opened[m.opened_count++] = h;
    }
    void put(Model& m, no::RequestHandle h, double value) {
        (void)raw(m.id, Edit::put_units, h, value);
        m.put(h, value);
    }
    void erase(Model& m, no::RequestHandle h) {
        (void)raw(m.id, Edit::erase_units, h);
        m.erase(h);
    }
    void clear(Model& m) {
        (void)raw(m.id, Edit::clear_units);
        m.unit_count = 0;
    }
    void canonical(const Model& m) {
        const auto& cohorts = std::as_const(host.adapter()).*access(CohortsTag{});
        const auto at = cohorts.find(m.id);
        require(at != cohorts.end(), "canonical cohort exists");
        const auto& facts = at->second;
        require(facts.origins.members().bound() && facts.opened.bound()
                    && facts.live_units_by_origin.bound(), "canonical facts use borrowed store");
        require(facts.origins.base() == 0 && facts.origins.size() == m.origin_count,
                "canonical retained positions match independent append multiset");
        for (std::size_t p = 0; p < m.origin_count; ++p)
            require(facts.origins[p] == m.origins[p], "canonical origin full handle matches");
        require(facts.opened.size() == m.opened_count, "canonical opened count matches");
        for (std::size_t p = 0; p < m.opened_count; ++p)
            require(std::as_const(facts.opened)[p] == m.opened[p], "canonical opened full handle matches");
        require(facts.live_units_by_origin.size() == m.unit_count, "canonical unit count matches");
        std::size_t count = 0;
        std::uint64_t previous = 0;
        for (const auto& unit : facts.live_units_by_origin) {
            const auto found = m.locate(unit.first);
            require(found != m.unit_count, "canonical unit key has an independent fact");
            require(count == 0 || previous < unit.first, "unit enumeration is ascending unsigned-64");
            equal_bits(unit.second, ce::bits(m.units[found].value), "canonical raw unit payload");
            previous = unit.first;
            ++count;
        }
        require(count == m.unit_count, "canonical enumeration visits every unit row");
    }
    void one_unit(const Model& m, no::RequestHandle h, bool present, double value = 0) {
        const auto& cohorts = std::as_const(host.adapter()).*access(CohortsTag{});
        const auto at = cohorts.find(m.id);
        require(at != cohorts.end(), "measured edit still has a real cohort");
        const auto& units = at->second.live_units_by_origin;
        require(units.size() == m.unit_count, "measured mutation changed actual unit count");
        const auto unit = units.find(h.incarnation);
        require((unit != units.end()) == present, "measured mutation changed actual key presence");
        if (present) equal_bits(unit->second, ce::bits(value), "measured mutation published actual bits");
    }
    void exposure(const Model& m) {
        canonical(m);
        const auto expected = m.expected();
        const auto actual = host.adapter().source_unclosed_qty_for(m.id);
        equal_bits(actual, expected, "real exposure equals independent oracle bits");
    }
};

std::uint64_t digest(const ScratchStore& store) {
    std::array<std::byte, page_bytes> page{};
    std::uint64_t result = 0;
    status_is(store.state_digest({page.data(), page.size()}, result),
              ScratchStatus::ok, "read complete committed store digest");
    return result;
}
void reopen(std::unique_ptr<ScratchStore>& store, Rig& rig, Subject& s) {
    const auto before = digest(*store);
    const auto hash = s.host.hash();
    store.reset();
    const auto writes = rig.charges.writes;
    store = std::make_unique<ScratchStore>(rig.config(ScratchOpenMode::reopen), &rig.backing);
    status_is(store->status(), ScratchStatus::ok, "same live backing reopens");
    s.rebind(*store);
    require(digest(*store) == before, "read-only reopen preserves store state digest");
    require(s.host.hash() == hash, "reopen preserves production host-extension hash");
    for (const auto& m : s.models) if (m.present) s.exposure(m);
    require(rig.charges.writes == writes, "reopen, rebind, hash and facade issue no writes");
}

struct AllocationWindow {
    static inline AllocationWindow* active = nullptr;
    std::size_t baseline = 0, peak = 0, calls = 0;
    static void allocated(std::size_t) {
        active->peak = std::max(active->peak, global_allocation::live_bytes);
    }
    AllocationWindow() {
        require(global_allocation::recording && active == nullptr
                    && global_allocation::on_recorded == nullptr,
                "allocation windows use continuous lifetime byte recording");
        baseline = peak = global_allocation::live_bytes;
        calls = global_allocation::allocations;
        active = this; global_allocation::on_recorded = &allocated;
    }
    ~AllocationWindow() { global_allocation::on_recorded = nullptr; active = nullptr; }
    std::size_t count() const { return global_allocation::allocations - calls; }
    std::size_t growth() const { return peak - baseline; }
};
struct Cost { std::uint64_t reads = 0; std::size_t allocations = 0, peak_bytes = 0; };
template<class Mutation>
Cost measure(Subject& s, ScratchStore& store, Rig& rig, Mutation&& mutation) {
    s.require_realtime();
    const auto before = store.stats();
    const auto charges = rig.charges;
    Cost result;
    {
        AllocationWindow window;
        mutation(); // Entire actual fixture publication, including its allocations.
        result.allocations = window.count();
        result.peak_bytes = window.growth();
    }
    const auto after = store.stats();
    result.reads = after.page_reads - before.page_reads;
    require(result.reads == rig.charges.reads - charges.reads,
            "mutation store stats and public backing observer agree on reads");
    require(after.page_writes - before.page_writes == rig.charges.writes - charges.writes,
            "mutation store stats and public backing observer agree on writes");
    require(after.syncs - before.syncs == rig.charges.syncs - charges.syncs
                && rig.charges.failures == charges.failures, "mutation has no failed I/O");
    require(after.page_writes > before.page_writes && !store.transaction_active(),
            "measured mutation really writes and commits the borrowed store");
    require(after.resident_bytes == before.resident_bytes, "store resident workspace is fixed");
    return result;
}

constexpr std::uint64_t store_d = 49, store_h = 12;
static_assert(store_d == scratch_impl::kMaxDepth + 1 && store_h == scratch_impl::kMaxHeight,
              "rederive the store-cost ceiling if legal substrate depths change");
constexpr std::uint64_t key_find =
    2 * store_d + 2 + (5 * (store_d - 1) + 7) * (store_h + 8) + 2 * store_h + 5;
constexpr std::uint64_t direct_load = 2 * store_h + 2 * ((936 + 55) / 56) + 4;
constexpr std::uint64_t put_cost = key_find + 2 * store_h + 4
    + 8 * ((96 + 55) / 56 + (936 + 55) / 56) + 16 * store_d + 16 * store_h + 64;
constexpr std::uint64_t mutation_c = 21057 * direct_load + 440 * put_cost;
// PLAN-LANE-E 3.2 fixed workspace: stack, three combined nodes, four structural
// frames, two read frames. This is a fixed allowance, never U * frame_size.
constexpr std::size_t resident_allowance = 128 * 16 + 3 * 936 + 4 * 512 + 2 * 512;
constexpr std::size_t allocation_allowance = 32;
constexpr double growth_slack = 1.25;
constexpr std::array<const char*, 5> operation_names{{
    "existing_write", "middle_insert", "middle_erase", "end_insert", "clear_regain"
}};
constexpr std::array<std::size_t, 4> populations{{17, 65, 257, 1025}};

void populate(Subject& s, Model& m, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        // Gaps reserve absent middle keys. Append in ascending order during
        // SETUP, avoiding a quadratic suffix-copy setup at U=1025.
        const auto h = s.handle(1000 + 2 * i);
        s.append(m, h);
        s.put(m, h, 0.25 + 0.125 * static_cast<double>(i % 7));
    }
    require(m.unit_count == n && m.origin_count == n, "exact U distinct live contributing units");
    s.canonical(m);
}

void s4_cohort_exposure_mutation_bound_red() {
    std::array<std::array<Cost, 5>, 4> costs{};
    std::array<std::size_t, 4> resident{};
    for (std::size_t sample = 0; sample < populations.size(); ++sample) {
        const auto n = populations[sample];
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        auto& m = s.ensure("cohort");
        populate(s, m, n);
        const auto middle = s.handle(1000 + 2 * (n / 2));
        const auto inserted = s.handle(middle.incarnation + 1);
        const auto end = s.handle(1000 + 2 * n);
        costs[sample][0] = measure(s, store, rig, [&] {
            (void)s.raw(m.id, Edit::put_units, middle, 3.5);
        });
        m.put(middle, 3.5);
        s.one_unit(m, middle, true, 3.5);

        costs[sample][1] = measure(s, store, rig, [&] {
            (void)s.raw(m.id, Edit::append_origin, inserted);
            (void)s.raw(m.id, Edit::put_units, inserted, 0.5);
        });
        m.origins[m.origin_count++] = inserted; m.put(inserted, 0.5);
        s.one_unit(m, inserted, true, 0.5);
        s.erase(m, inserted); // Outside the window; next op again starts with U.

        costs[sample][2] = measure(s, store, rig, [&] {
            (void)s.raw(m.id, Edit::erase_units, middle);
        });
        m.erase(middle);
        s.one_unit(m, middle, false);
        s.put(m, middle, 3.5); // Restore U before the end insertion.

        costs[sample][3] = measure(s, store, rig, [&] {
            (void)s.raw(m.id, Edit::append_origin, end);
            (void)s.raw(m.id, Edit::put_units, end, 0.75);
        });
        m.origins[m.origin_count++] = end; m.put(end, 0.75);
        s.one_unit(m, end, true, 0.75);
        s.erase(m, end); // Clear below starts with exactly U unit rows.

        costs[sample][4] = measure(s, store, rig, [&] {
            (void)s.raw(m.id, Edit::clear_units);
            (void)s.raw(m.id, Edit::put_units, middle, 0.125);
        });
        m.unit_count = 0; m.put(middle, 0.125);
        // The REAL facade runs only AFTER all five mutation windows. With
        // one regained unit this cannot mask write costs with the base scan.
        s.exposure(m);
        resident[sample] = global_allocation::live_bytes;
        const auto g = static_cast<std::uint64_t>(std::ceil(std::log2(static_cast<double>(n))));
        const auto bound = mutation_c * g * g;
        for (std::size_t op = 0; op < operation_names.size(); ++op) {
            const auto& cost = costs[sample][op];
            std::printf("cohort-mutation U=%zu op=%s reads=%llu bound=%llu ratio=%.9f "
                        "allocations=%zu peak_bytes=%zu live_bytes=%zu c=%llu\n",
                        n, operation_names[op], static_cast<unsigned long long>(cost.reads),
                        static_cast<unsigned long long>(bound),
                        static_cast<double>(cost.reads) / static_cast<double>(bound),
                        cost.allocations, cost.peak_bytes, resident[sample],
                        static_cast<unsigned long long>(mutation_c));
        }
        std::fflush(stdout);
    }
    // Print EVERY ratio before the first assertion. Base middle insert/erase
    // shift the suffix; m5 additionally makes the EXISTING write linear.
    bool growth_ok = true;
    for (std::size_t op = 0; op < operation_names.size(); ++op) {
        for (const std::size_t low : {std::size_t{0}, std::size_t{2}}) {
            const auto high = low + 1;
            const auto log_ratio = std::log2(static_cast<double>(populations[high]))
                                 / std::log2(static_cast<double>(populations[low]));
            const auto allowed = log_ratio * log_ratio * growth_slack;
            const auto small = costs[low][op].reads, large = costs[high][op].reads;
            const auto ratio = small ? static_cast<double>(large) / static_cast<double>(small)
                                     : std::numeric_limits<double>::infinity();
            std::printf("cohort-mutation-growth op=%s U=%zu/%zu reads=%llu/%llu "
                        "ratio=%.9f limit=%.9f slack=%.2f\n", operation_names[op],
                        populations[high], populations[low],
                        static_cast<unsigned long long>(large), static_cast<unsigned long long>(small),
                        ratio, allowed, growth_slack);
            if (!small || static_cast<double>(large) > static_cast<double>(small) * allowed) {
                growth_ok = false;
                std::fprintf(stderr, "mutation growth bound exceeded: op=%s U=%zu/%zu\n",
                             operation_names[op], populations[high], populations[low]);
            }
        }
    }
    std::fflush(stdout);
    require(growth_ok, "mutation growth bound: base WorkingMap suffix shifts; m5 also scans existing writes");
    for (std::size_t sample = 0; sample < populations.size(); ++sample) {
        const auto g = static_cast<std::uint64_t>(std::ceil(std::log2(static_cast<double>(populations[sample]))));
        for (const auto& cost : costs[sample]) {
            require(cost.reads <= mutation_c * g * g, "mutation reads <= derived c * ceil(log2 U)^2");
            require(cost.peak_bytes <= resident_allowance, "mutation peak live bytes fit fixed frames");
        }
    }
    for (std::size_t op = 0; op < operation_names.size(); ++op)
        require(costs.back()[op].allocations <= costs.front()[op].allocations + allocation_allowance,
                "U=1025 allocation count <= U=17 count + fixed 32");
    require(resident.back() <= resident.front() + resident_allowance,
            "U=1025 final resident live bytes <= U=17 + fixed workspace allowance");
}

struct Scenario { const char* name; std::size_t size; };
constexpr std::array<Scenario, 12> scenarios{{
    {"empty_u0", 0}, {"zero_u1", 1}, {"signed_zero_u2", 2},
    {"nan_payloads_u3", 3}, {"nonfinite_u7", 7}, {"payloads_u17", 17},
    {"shuffled_u33", 33}, {"unsigned_u65", 65}, {"dense_u257", 257},
    {"orphan_u3", 3}, {"clear_regain_u7", 7}, {"two_cohorts_reopen_u17", 17}
}};
double scenario_value(std::size_t i, ce::Generator& rng) {
    const std::array<double, 12> palette{{
        0.0, ce::from_bits(0x8000000000000000ULL),
        ce::from_bits(0x7ff8000000000001ULL), ce::from_bits(0x7ff8abcdef012345ULL),
        ce::from_bits(0x7ff0000000000000ULL), -1.25, ce::from_bits(1),
        ce::from_bits(0x7fefffffffffffffULL), 0.1, 0.2, 0.3,
        ce::from_bits(0xfff0000000000000ULL)
    }};
    return i < palette.size() ? palette[i] : rng.quantity();
}
void scenario_units(Subject& s, Model& m, std::size_t n, ce::Generator& rng,
                    bool nan_first = false) {
    std::array<no::RequestHandle, model_capacity> handles{};
    std::array<std::size_t, model_capacity> order{};
    std::array<double, model_capacity> values{};
    for (std::size_t i = 0; i < n; ++i) {
        // Full unsigned key domain; roster order differs from map order.
        const auto key = (i % 2 ? (std::uint64_t{1} << 63) : 0) + 4 * (i + 1);
        handles[i] = s.handle(key); order[i] = i;
        values[i] = scenario_value(i + (nan_first ? 2 : 0), rng);
        s.append(m, handles[i]);
        if (i % 3 == 0) s.opened(m, handles[i]);
    }
    for (std::size_t i = n; i > 1; --i)
        std::swap(order[i - 1], order[static_cast<std::size_t>(rng.next() % i)]);
    // Keep the seeded permutation, forcing an inversion if it happened to
    // be ascending (including the two-row case). Never rely on RNG luck for
    // the out-of-incarnation-order coverage.
    if (n > 1 && std::is_sorted(order.begin(), order.begin() + n,
            [&](std::size_t a, std::size_t b) {
                return handles[a].incarnation < handles[b].incarnation;
            }))
        std::swap(order[0], order[1]);
    for (std::size_t i = 0; i < n; ++i) s.put(m, handles[order[i]], values[order[i]]);
}

void s4_cohort_exposure_hash_golden() {
    std::array<std::uint64_t, scenarios.size()> actual{};
    ce::Generator rng;
    rng.state = golden_seed; // SplitMix64, fixed 0x4f31533445; never a runtime seed.
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        Rig rig;
        auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
        Subject s(*store);
        auto& m = s.ensure("cohort");
        scenario_units(s, m, scenarios[i].size, rng, i == 3);
        if (i == 9) s.put(m, s.handle(0xfffffffffffffff1ULL), 0.375); // No origin.
        if (i == 10) {
            s.clear(m);
            equal_bits(s.host.adapter().source_unclosed_qty_for(m.id), 0, "cleared cohort is positive zero");
            for (std::size_t p = m.origin_count; p > 0; --p)
                s.put(m, m.origins[p - 1], scenario_value(p + 5, rng));
        }
        if (i == 11) {
            // Insert the lexicographically earlier id SECOND.
            auto& side = s.ensure("alpha", 1);
            scenario_units(s, side, 3, rng, true);
            s.put(side, s.handle(0xfffffffffffffff3ULL), 0.625); // Side orphan too.
            s.exposure(side);
        }
        s.exposure(m);
        actual[i] = s.host.hash(); // Production host -> adapter, not a copied cohort fold.
        std::printf("%s %016llx\n", scenarios[i].name,
                    static_cast<unsigned long long>(actual[i]));
        std::fflush(stdout); // Print even when the table is absent/pending/mismatched.
        reopen(store, rig, s); // Every scenario also has the same-store reopen assertion.
    }

    std::ifstream input(PINEFORGE_COHORT_EXPOSURE_HASH_GOLDEN);
    require(input.is_open(), "golden file missing (base hashes required)");
    std::array<std::uint64_t, scenarios.size()> expected{};
    std::array<bool, scenarios.size()> seen{};
    std::size_t lines = 0;
    bool pending = true; // The shipped ALL-zero table is an explicit pending receipt.
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
        std::istringstream fields(line);
        std::string name, hex, extra;
        require(bool(fields >> name >> hex) && !(fields >> extra), "golden line must be name + 16 hex digits");
        require(hex.size() == 16, "golden hash must have exactly 16 hex digits");
        std::uint64_t word = 0;
        for (const char c : hex) {
            const auto digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            require(digit >= 0, "golden hash contains a non-hex digit");
            word = (word << 4) | static_cast<unsigned>(digit);
        }
        std::size_t at = 0;
        while (at < scenarios.size() && name != scenarios[at].name) ++at;
        require(at != scenarios.size(), "golden contains an unknown scenario name");
        require(!seen[at], "golden contains a duplicate scenario name");
        seen[at] = true; expected[at] = word;
        pending = pending && word == 0;
    }
    require(input.eof() && !input.bad(), "golden file read failed");
    require(lines == scenarios.size(), "golden line count differs from twelve scenarios");
    bool identical = true;
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        require(seen[i], "golden scenario name is absent");
        if (actual[i] == expected[i]) { ++checkpoints; continue; }
        identical = false;
        std::fprintf(stderr, "golden mismatch: %s actual=%016llx expected=%016llx\n",
                     scenarios[i].name, static_cast<unsigned long long>(actual[i]),
                     static_cast<unsigned long long>(expected[i]));
    }
    require(!pending, "golden pending: supervisor must fill twelve hashes from the BASE run");
    require(identical, "cohort exposure production hash golden mismatch");
}

void s4_cohort_exposure_reopen_rollback_fault() {
    {
        Rig rig;
        auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
        Subject s(*store);
        auto& m = s.ensure("cohort");
        populate(s, m, 7);
        s.exposure(m);
        reopen(store, rig, s);
        const auto before = digest(*store), hash = s.host.hash();
        const auto middle = s.handle(1006), fresh = s.handle(1007);
        status_is(store->begin(), ScratchStatus::ok, "begin explicit fixture rollback");
        (void)s.raw(m.id, Edit::put_units, middle, 9.0);
        (void)s.raw(m.id, Edit::append_origin, fresh);
        (void)s.raw(m.id, Edit::put_units, fresh, 0.75);
        status_is(store->rollback(), ScratchStatus::ok, "explicit fixture rollback");
        require(digest(*store) == before, "rollback restores pre-edit complete store digest");
        s.rebind(*store); // Discard provisional cached sizes, roots and frames.
        s.exposure(m);
        require(s.host.hash() == hash, "rollback/rebind restores production host hash");
        // Reuse the aborted incarnation, then reuse the textual cohort id.
        s.append(m, fresh); s.put(m, fresh, 0.5); s.exposure(m);
        const auto old = s.raw(m.id, Edit::lookup).first.value;
        (void)s.raw("", Edit::clear_all);
        m.present = false; m.origin_count = m.opened_count = m.unit_count = 0;
        auto& reused = s.ensure("cohort");
        require(s.raw(reused.id, Edit::lookup).first.value != old, "id reuse gets a fresh cohort");
        s.append(reused, fresh); s.put(reused, fresh, 0.25); s.exposure(reused);
        reopen(store, rig, s);
    }

    // Calibrate successful write count ONLY to enumerate every fault point.
    // No measured count enters a resource bound. Every nth is exercised;
    // a zero-write publication or zero commit writes is a failed control.
    std::uint64_t total_writes = 0, publication_writes = 0;
    std::size_t publication_faults = 0, commit_faults = 0, recoveries = 0;
    for (std::uint64_t nth = 0; nth <= total_writes; ++nth) {
        Rig rig;
        auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
        Subject s(*store);
        auto& m = s.ensure("cohort");
        populate(s, m, 7);
        s.exposure(m);
        const auto before = digest(*store), hash = s.host.hash();
        const auto fresh = s.handle(1007);
        const auto writes = rig.charges.writes, hits = rig.backing.hits;
        status_is(store->begin(), ScratchStatus::ok, "begin faulted fixture publication");
        if (nth) rig.backing.arm(nth, nth % 2 == 0);
        bool refused = false, in_commit = false;
        try {
            (void)s.raw(m.id, Edit::append_origin, fresh);
            (void)s.raw(m.id, Edit::put_units, fresh, 0.75);
            if (!nth) publication_writes = rig.charges.writes - writes;
            in_commit = true;
            ws::check(store->commit(ScratchDurability::buffered));
        } catch (const ScratchIoError& error) {
            refused = true;
            require(classify_run_failure(error).code == RunFailureCode::scratch_io_failed,
                    "injected backing refusal remains typed scratch I/O failure");
        }
        if (!nth) {
            require(!refused, "fault-sweep calibration actually commits");
            total_writes = rig.charges.writes - writes;
            require(publication_writes > 0 && total_writes > publication_writes,
                    "fault sweep has both publication writes and commit writes");
            m.origins[m.origin_count++] = fresh; m.put(fresh, 0.75);
            s.exposure(m);
            std::printf("cohort-fault calibration publication_writes=%llu commit_writes=%llu\n",
                        static_cast<unsigned long long>(publication_writes),
                        static_cast<unsigned long long>(total_writes - publication_writes));
            std::fflush(stdout);
            continue;
        }
        require(refused && rig.backing.hits == hits + 1, "every injected nth write is reached and refused");
        require(store->transaction_active(), "failed fixture publication remains rollback-only");
        status_is(store->commit(ScratchDurability::buffered), ScratchStatus::programming_error,
                  "partial source/derived state cannot commit");
        rig.backing.disarm();
        status_is(store->rollback(), ScratchStatus::ok, "recover injected publication/commit fault");
        require(digest(*store) == before, "fault rollback restores pre-failure complete store digest");
        s.rebind(*store);
        s.exposure(m);
        require(s.host.hash() == hash, "fault rollback restores production host hash");
        reopen(store, rig, s);
        require(digest(*store) == before, "fault reopen keeps pre-failure complete store digest");
        // Same incarnation and same textual id can be used after recovery.
        s.append(m, fresh); s.put(m, fresh, 0.5); s.exposure(m);
        const auto old = s.raw(m.id, Edit::lookup).first.value;
        (void)s.raw("", Edit::clear_all);
        m.present = false; m.origin_count = m.opened_count = m.unit_count = 0;
        auto& reused = s.ensure("cohort");
        require(s.raw(reused.id, Edit::lookup).first.value != old, "post-fault id reuse gets a fresh cohort");
        s.append(reused, fresh); s.put(reused, fresh, 0.125); s.exposure(reused);
        if (in_commit) ++commit_faults; else ++publication_faults;
        ++recoveries;
        std::printf("cohort-fault nth=%llu stage=%s tear=%u digest_restored=1 hash_restored=1\n",
                    static_cast<unsigned long long>(nth), in_commit ? "commit" : "publication",
                    static_cast<unsigned>(nth % 2 == 0));
        std::fflush(stdout);
    }
    require(publication_faults != 0 && commit_faults != 0 && recoveries == total_writes,
            "fault sweep covers every write with nonzero publication and commit checkpoints");
    std::printf("cohort-fault recoveries=%zu publication_faults=%zu commit_faults=%zu\n",
                recoveries, publication_faults, commit_faults);
}

void s4_cohort_exposure_churn_bounded() {
    struct Sample { unsigned cycle = 0; std::uint64_t records = 0, bytes = 0, workspace = 0;
                    std::size_t live = 0; };
    std::array<Sample, 4> samples{};
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    auto& m = s.ensure("cohort");
    populate(s, m, 65);
    std::size_t at = 0;
    for (unsigned cycle = 1; cycle <= 64; ++cycle) {
        s.clear(m);
        // Regain at EXISTING positions; fixed live/retained U, no test history.
        for (std::size_t p = 0; p < m.origin_count; ++p)
            s.put(m, m.origins[p], 0.125 * static_cast<double>(1 + p % 5));
        require(m.unit_count == 65 && m.origin_count == 65, "churn fixed U=65");
        if (cycle != 8 && cycle != 16 && cycle != 32 && cycle != 64) continue;
        s.canonical(m);
        const auto stats = store.stats();
        samples[at++] = {cycle, store.record_count(), stats.backing_bytes,
                         stats.resident_bytes, global_allocation::live_bytes};
        const auto& sample = samples[at - 1];
        std::printf("cohort-churn U=65 cycle=%u records=%llu extent_bytes=%llu "
                    "workspace=%llu live_bytes=%zu\n", sample.cycle,
                    static_cast<unsigned long long>(sample.records),
                    static_cast<unsigned long long>(sample.bytes),
                    static_cast<unsigned long long>(sample.workspace), sample.live);
        std::fflush(stdout);
    }
    require(at == samples.size(), "churn records all four nonzero checkpoints");
    // Fixed debt/control envelope after eight complete populations have
    // serviced reclamation: eight job/header records and eight backing pages.
    // Resident envelope is the same shared fixed frame ledger as the cost row.
    bool bounded = true;
    for (const auto& sample : samples)
        bounded = bounded && sample.records <= samples.front().records + 8
            && sample.bytes <= samples.front().bytes + 8 * page_bytes
            && sample.workspace == samples.front().workspace
            && sample.live <= samples.front().live + resident_allowance;
    std::printf("cohort-churn verdict=%s record_allowance=8 extent_allowance=%zu resident_allowance=%zu\n",
                bounded ? "BOUNDED" : "RED: clear/regain accumulates records, extent or resident bytes",
                8 * page_bytes, resident_allowance);
    std::fflush(stdout);
    require(bounded, "fixed-U churn must not accumulate clear records, extent or live resident bytes");
    s.exposure(m);
}

void multiset_positions(Subject& s, const Model& m, no::RequestHandle h) {
    const auto& cohorts = std::as_const(s.host.adapter()).*access(CohortsTag{});
    const auto at = cohorts.find(m.id);
    require(at != cohorts.end(), "multiset fixture cohort exists");
    std::array<std::size_t, 2> actual{};
    std::size_t seen = 0;
    at->second.origins.visit_incarnation_positions(h.incarnation, [&](std::size_t position) {
        require(seen < actual.size(), "fixture duplicate has exactly two retained positions");
        actual[seen++] = position;
        return true;
    });
    require(seen == 2 && actual[0] == 0 && actual[1] == 2, "fixture multiset keeps k=2 at positions 0 and 2");
}
void s4_cohort_exposure_fixture_multiset_kept() {
    Rig rig;
    auto store = std::make_unique<ScratchStore>(rig.config(), &rig.backing);
    Subject s(*store);
    auto& m = s.ensure("cohort");
    const auto a = s.handle(30), b = s.handle(20);
    s.append(m, a); s.append(m, b); s.append(m, a);
    s.put(m, a, 0.1); s.put(m, b, 0.2);
    multiset_positions(s, m, a); s.exposure(m);
    equal_bits(s.host.adapter().source_unclosed_qty_for(m.id), 0x3fd999999999999aULL,
               "A+B+A is (.1+.2)+.1 = binary64 .4");
    s.put(m, a, 0.3); s.exposure(m);
    equal_bits(s.host.adapter().source_unclosed_qty_for(m.id), 0x3fe999999999999aULL,
               "set A updates both contributions: (.3+.2)+.3 = binary64 .8");
    s.erase(m, a); s.exposure(m); multiset_positions(s, m, a);
    equal_bits(s.host.adapter().source_unclosed_qty_for(m.id), ce::bits(0.2),
               "erase A removes both contributions and retains B");
    s.put(m, a, 0.1); s.exposure(m);
    const auto before = s.host.hash();
    reopen(store, rig, s);
    multiset_positions(s, m, a); s.exposure(m);
    require(s.host.hash() == before, "reopen retains both fixture positions and their production hash");
    // Production second-append refusal and production writers: round 2.
}

struct TestCase { const char* name; void (*body)(); };
const TestCase cases[] = {
    {"s4_cohort_exposure_mutation_bound_red", &s4_cohort_exposure_mutation_bound_red},
    {"s4_cohort_exposure_hash_golden", &s4_cohort_exposure_hash_golden},
    {"s4_cohort_exposure_reopen_rollback_fault", &s4_cohort_exposure_reopen_rollback_fault},
    {"s4_cohort_exposure_churn_bounded", &s4_cohort_exposure_churn_bounded},
    {"s4_cohort_exposure_fixture_multiset_kept", &s4_cohort_exposure_fixture_multiset_kept}
};
} // namespace

int main(int argc, char** argv) {
    // Never reset live_bytes or turn recording off between measured windows.
    global_allocation::recording = true;
    if (argc > 2) { std::fprintf(stderr, "expected at most one named cohort case\n"); return 1; }
    bool selected = false;
    for (const auto& test : cases) {
        if (argc == 2 && std::strcmp(argv[1], test.name) != 0) continue;
        selected = true; checkpoints = 0;
        try {
            ce::require_round_to_nearest();
            test.body();
            ce::require(checkpoints != 0, "a row cannot pass with zero checkpoints");
            std::printf("%s: PASS checkpoints=%zu\n", test.name, checkpoints);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: FAIL checkpoints=%zu: %s\n", test.name, checkpoints, error.what());
            return 1; // A failed control stops the sequence.
        }
    }
    if (!selected) std::fprintf(stderr, "unknown cohort-exposure mutation case\n");
    return selected ? 0 : 1;
}
