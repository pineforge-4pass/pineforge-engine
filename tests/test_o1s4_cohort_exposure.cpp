// LAB-o1s4 E3. New semantic controls plus behaviour-red read ceilings.
// No implementation, new fixture declaration, or edits to existing s4 cases.
// EDIT ONLY: actual base red and Release/Debug results belong to the supervisor.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "cohort_exposure_oracle.hpp"

#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/detail/working_store.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;
namespace no = pineforge::native_order;
namespace ps = pineforge::source;
namespace ce = cohort_exposure_test;
namespace ws = pineforge::detail::working_store;
constexpr std::size_t page_bytes = 512;
constexpr std::uint64_t exposure_read_ceiling = 1024, exposure_read_slack = 512;

// Existing test_origin_roster_retirement.cpp access idiom, used ONLY for
// canonical facts/optional empty-from-entry semantics, never an E derived row.
template<class Tag, auto Member> struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
struct CohortsTag { friend auto access(CohortsTag); };
template struct PrivateAccess<CohortsTag, &ps::PineExecutionAdapter::cohorts_by_id_>;
struct FromEntryTag { friend auto access(FromEntryTag); };
template struct PrivateAccess<FromEntryTag, &ps::PineExecutionAdapter::from_entry_units>;

void equal_bits(double actual, std::uint64_t expected, const char* context) {
    const auto word = ce::bits(actual);
    if (word == expected) return;
    std::fprintf(stderr, "%s: actual=%016llx oracle/hand=%016llx\n", context,
                 static_cast<unsigned long long>(word),
                 static_cast<unsigned long long>(expected));
    throw std::runtime_error("production exposure differs by bits");
}

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

struct Rig {
    // Test-owned backing, allocated before any query allocation window.
    std::vector<std::byte> storage;
    Charges charges;
    FixedMemoryScratchBacking backing;
    Rig() : storage(131072 * page_bytes),
            backing({storage.data(), storage.size()}, page_bytes, charges.observer()) {}
    ScratchStoreConfig config() const {
        // Same two 512-byte pages and one staging slot as test_o1s4_queue_red.
        return {2, page_bytes, 1, 1, {0x4f315334, nullptr, &lexical},
                ScratchOpenMode::create_empty};
    }
};

no::RunIdentity run_id() { return {"o1s4-e3", 7}; }
no::RequestHandle handle(std::uint64_t incarnation) { return {run_id(), incarnation}; }

struct Host final : ps::PineStrategyHost {
    ps::PineExecutionAdapter& adapter() { return adapter_; }
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    no::WorkingRequestCore& core() {
        return const_cast<no::WorkingRequestCore&>(consumer().request_core());
    }
    void on_source_bar(const Bar&) override {}
};

// Exact fixture transport codes from test_o1s4_queue_red and the base
// fixture_cohort_state definition. No new test seam is declared here.
enum class CohortEdit : std::uint8_t {
    lookup, ensure, append_origin, append_opened, erase_opened, clear_opened,
    put_units, erase_units, clear_units, clear_all
};

struct CohortModel {
    std::vector<no::RequestHandle> origins; // Lifetime positions, including retired prefix.
    std::size_t base = 0;
    std::map<std::uint64_t, double> units;
    std::vector<ce::Row> rows() const {
        std::vector<ce::Row> result;
        for (std::size_t position = base; position < origins.size(); ++position) {
            const auto incarnation = origins[position].incarnation;
            const auto found = units.find(incarnation);
            if (found != units.end()) result.push_back({position, incarnation, found->second});
        }
        return result;
    }
};

struct Subject {
    Host host;
    std::map<std::string, CohortModel> model;
    std::size_t comparisons = 0;
    bool observe_edits;
    explicit Subject(ScratchStore& store, bool observe_each_edit = true)
        : observe_edits(observe_each_edit) {
        ce::require(store.status() == ScratchStatus::ok, "fixture store opens");
        host.core().reset(run_id());
        host.core().bind_scratch_store(store);
        host.adapter().bind_scratch_store(store);
    }
    ps::PineExecutionAdapter& adapter() { return host.adapter(); }
    std::pair<no::CohortHandle, const ps::OriginRoster*> edit(
            const std::string& id, CohortEdit kind, no::RequestHandle h = {}, double value = 0) {
        return adapter().fixture_cohort_state(id, static_cast<std::uint8_t>(kind), h, value);
    }
    std::uint64_t expected(const std::string& id) const {
        const auto found = model.find(id);
        return ce::bits(found == model.end() ? 0.0 : ce::oracle(found->second.rows()));
    }
    void canonical_matches(const std::string& id) {
        const auto& cohorts = std::as_const(adapter()).*access(CohortsTag{});
        const auto actual = cohorts.find(id);
        const auto want = model.find(id);
        if (want == model.end()) {
            ce::require(actual == cohorts.end(), "absent canonical cohort remains absent");
            return;
        }
        ce::require(actual != cohorts.end(), "fixture created a real canonical cohort");
        const auto& facts = actual->second;
        const auto& m = want->second;
        ce::require(facts.origins.base() == m.base && facts.origins.size() == m.origins.size(),
                    "canonical retained/lifetime positions match fixture model");
        for (std::size_t p = m.base; p < m.origins.size(); ++p) {
            const auto origin = facts.origins[p];
            ce::require(origin.incarnation == m.origins[p].incarnation
                            && origin.run.run_number == m.origins[p].run.run_number
                            && origin.run.session_key.string() == m.origins[p].run.session_key.string(),
                        "canonical roster compares the full input handle");
        }
        ce::require(facts.live_units_by_origin.size() == m.units.size(),
                    "canonical unit count includes invalid and orphan rows");
        std::size_t seen = 0;
        for (const auto& unit : facts.live_units_by_origin) {
            const auto value = m.units.find(unit.first);
            ce::require(value != m.units.end(), "canonical unit has a model incarnation");
            equal_bits(unit.second, ce::bits(value->second), "canonical per-origin value");
            ++seen;
        }
        ce::require(seen == m.units.size(), "canonical unit read visits every input row");
    }
    void check(const std::string& id, const char* context) {
        // Canonical checks/model arithmetic precede the REAL production read.
        canonical_matches(id);
        const auto want = expected(id);
        const auto actual = adapter().source_unclosed_qty_for(id);
        if (ce::bits(actual) != want)
            std::fprintf(stderr, "cohort='%s' comparison=%zu mutation=%s\n",
                         id.c_str(), comparisons, context);
        equal_bits(actual, want, context);
        ++comparisons;
    }
    void check_all(const char* context) {
        for (const auto& row : model) check(row.first, context);
        check("never-created", context);
    }
    void after_edit(const char* context) {
        // Semantic cases observe every edit. Cost cases MUST NOT pre-query
        // exposure: their first query is inside the guarded warm measurement,
        // so a read-side rebuild cannot hide behind fixture validation.
        if (observe_edits) check_all(context);
    }
    no::CohortHandle ensure(const std::string& id) {
        const auto result = edit(id, CohortEdit::ensure).first;
        model.try_emplace(id);
        ce::require(result.value != 0, "ensure returns an actual native cohort handle");
        after_edit("ensure");
        return result;
    }
    void append(const std::string& id, no::RequestHandle h) {
        ce::require(model.count(id) != 0, "append names an ensured cohort");
        (void)edit(id, CohortEdit::append_origin, h);
        model.at(id).origins.push_back(h);
        after_edit("origin append");
    }
    void put(const std::string& id, no::RequestHandle h, double value) {
        (void)edit(id, CohortEdit::put_units, h, value);
        model.at(id).units[h.incarnation] = value;
        after_edit("unit set/credit");
    }
    void erase(const std::string& id, no::RequestHandle h) {
        (void)edit(id, CohortEdit::erase_units, h);
        model.at(id).units.erase(h.incarnation);
        after_edit("unit erase");
    }
    void clear_units(const std::string& id) {
        (void)edit(id, CohortEdit::clear_units);
        model.at(id).units.clear();
        after_edit("unit clear");
    }
    void clear_all() {
        std::vector<std::string> ids;
        for (const auto& row : model) ids.push_back(row.first);
        (void)edit("", CohortEdit::clear_all);
        model.clear();
        if (observe_edits)
            for (const auto& id : ids) check(id, "cohort table clear");
    }
    void retire_single_cohort(const std::string& id, std::size_t new_base) {
        ce::require(model.size() == 1, "retirement model covers every fixture cohort");
        adapter().fixture_retire_origin_prefixes();
        model.at(id).base = new_base;
        after_edit("prefix retirement");
    }
};

// Peak LIVE allocation growth during a query, including transient peaks.
// Main leaves byte recording enabled throughout object lifetimes so a later
// deletion of a recorded block is charged correctly across query windows.
// Model/backing/digest work updates the baseline outside the peak window.
// Repeated fixed-frame allocate/free churn is not resident heap growth.
struct QueryAllocations {
    static inline QueryAllocations* active = nullptr;
    std::size_t baseline = 0, peak = 0;
    static void allocated(std::size_t) {
        active->peak = std::max(active->peak, global_allocation::live_bytes);
    }
    QueryAllocations() {
        ce::require(global_allocation::recording && active == nullptr
                        && global_allocation::on_recorded == nullptr,
                    "query peak windows are disjoint within continuous byte recording");
        baseline = peak = global_allocation::live_bytes;
        active = this;
        global_allocation::on_recorded = &allocated;
    }
    ~QueryAllocations() {
        global_allocation::on_recorded = nullptr;
        active = nullptr;
    }
    std::size_t growth() const { return peak - baseline; }
    // Fixed simultaneous decode/control frames, independent of cohort U.
    static constexpr std::size_t allowance =
        2 * (sizeof(ps::PlacementSnapshot) + 4 * sizeof(void*))
        + 2 * (sizeof(no::RequestDefinition) + sizeof(no::LiveRequest) + 4 * sizeof(void*))
        + page_bytes;
};

std::uint64_t digest(const ScratchStore& store) {
    std::array<std::byte, page_bytes> page{};
    std::uint64_t result = 0;
    ce::require(store.state_digest({page.data(), page.size()}, result) == ScratchStatus::ok,
                "query guard reads committed store digest");
    return result;
}

struct QueryCost {
    std::array<std::uint64_t, 2> reads{}; // First guarded warm call, then measured call.
    std::array<std::size_t, 2> peak_bytes{};
};

struct QueryGuard {
    ScratchStore& store;
    Rig& rig;
    std::uint64_t before_digest, records;
    ScratchStoreStats before;
    Charges charged;
    QueryCost cost{};
    QueryGuard(ScratchStore& s, Rig& r)
        : store(s), rig(r), before_digest(digest(s)), records(s.record_count()),
          before(s.stats()), charged(r.charges) {}
    // The first query after publication is bounded too: no unmeasured cache
    // repair, population allocation or source traversal is a warm-up allowance.
    // Neither digest walk is part of either query's I/O/allocation window.
    void start(std::size_t warm_peak) {
        const auto warm = store.stats();
        cost.reads[0] = warm.page_reads - before.page_reads;
        cost.peak_bytes[0] = warm_peak;
        ce::require(cost.reads[0] == rig.charges.reads - charged.reads,
                    "each warm backing read is charged exactly once");
        ce::require(warm.page_writes == before.page_writes && warm.syncs == before.syncs
                        && rig.charges.writes == charged.writes
                        && rig.charges.syncs == charged.syncs
                        && rig.charges.failures == charged.failures,
                    "warm observer performs no writes, syncs or failed I/O");
        ce::require(warm.resident_bytes == before.resident_bytes
                        && warm.backing_pages == before.backing_pages
                        && store.record_count() == records,
                    "warm observer preserves workspace, extent and record count");
        ce::require(warm_peak <= QueryAllocations::allowance,
                    "first observer peak live bytes fit fixed decode frames");
        before = warm;
        charged = rig.charges;
    }
    QueryCost finish(std::size_t peak_growth, const char* label,
                     std::size_t n, std::size_t live_units, std::size_t phase) const {
        const auto after = store.stats();
        const auto reads = after.page_reads - before.page_reads;
        std::printf("%s initial_U=%zu live_unit_rows=%zu phase=%zu "
                    "warm_reads=%llu reads=%llu writes=%llu "
                    "warm_peak_bytes=%zu peak_bytes=%zu\n",
                    label, n, live_units, phase,
                    static_cast<unsigned long long>(cost.reads[0]),
                    static_cast<unsigned long long>(reads),
                    static_cast<unsigned long long>(after.page_writes - before.page_writes),
                    cost.peak_bytes[0], peak_growth);
        std::fflush(stdout);
        ce::require(after.page_reads - before.page_reads == rig.charges.reads - charged.reads,
                    "each query backing read is charged exactly once");
        ce::require(after.page_writes == before.page_writes
                        && rig.charges.writes == charged.writes && after.syncs == before.syncs
                        && rig.charges.syncs == charged.syncs
                        && rig.charges.failures == charged.failures,
                    "exposure/copy query performs no writes, syncs or failed I/O");
        ce::require(after.resident_bytes == before.resident_bytes
                        && after.backing_pages == before.backing_pages
                        && store.record_count() == records,
                    "query changes no resident workspace, backing extent or record count");
        ce::require(peak_growth <= QueryAllocations::allowance,
                    "query peak live bytes fit fixed decode frames, independent of U");
        ce::require(digest(store) == before_digest, "query preserves complete store state digest");
        auto result = cost;
        result.reads[1] = reads;
        result.peak_bytes[1] = peak_growth;
        return result;
    }
};

QueryCost measure_exposure(Subject& s, ScratchStore& store, Rig& rig,
                           const std::string& id, std::size_t n, std::size_t phase) {
    const auto expected = s.expected(id); // Oracle is outside both measured windows.
    QueryGuard guard(store, rig);
    double warm = 0.0, actual = 0.0;
    std::size_t warm_peak = 0, peak = 0;
    {
        QueryAllocations window;
        warm = s.adapter().source_unclosed_qty_for(id);
        warm_peak = window.growth();
    }
    equal_bits(warm, expected, "warm exposure");
    guard.start(warm_peak);
    {
        QueryAllocations window;
        actual = s.adapter().source_unclosed_qty_for(id);
        peak = window.growth();
    }
    equal_bits(actual, expected, "measured exposure");
    const auto cost = guard.finish(
        peak, "cohort-exposure-read-bound", n, s.model.at(id).units.size(), phase);
    s.canonical_matches(id); // Validate fixture facts AFTER both production calls.
    return cost;
}

std::vector<no::RequestHandle> populate(Subject& s, const std::string& id, std::size_t n) {
    (void)s.ensure(id);
    std::vector<no::RequestHandle> handles;
    for (std::size_t i = 0; i < n; ++i) {
        // Roster order intentionally differs from incarnation/map order.
        const auto h = handle(1000 + n - i);
        handles.push_back(h);
        s.append(id, h);
        s.put(id, h, 0.25 + 0.125 * static_cast<double>(i % 7));
    }
    ce::require(s.model.at(id).units.size() == n && s.model.at(id).origins.size() == n,
                "cost fixture has exactly U distinct contributing origins");
    return handles;
}

void enforce_exposure_bounds(const std::array<std::array<QueryCost, 4>, 2>& reads) {
    for (std::size_t phase = 0; phase < reads[0].size(); ++phase) {
        for (std::size_t call = 0; call < 2; ++call) {
            ce::require(reads[0][phase].reads[call] <= exposure_read_ceiling
                            && reads[1][phase].reads[call] <= exposure_read_ceiling,
                        "warm AND measured exposure read <= 1024 backing pages per call");
            ce::require(reads[1][phase].reads[call]
                            <= reads[0][phase].reads[call] + exposure_read_slack,
                        "U=65 warm/measured exposure reads <= U=17 counterpart + 512");
        }
    }
}

void s4_cohort_exposure_read_bound_red() {
    std::array<std::array<QueryCost, 4>, 2> reads{};
    const std::array<std::size_t, 2> sizes{{17, 65}};
    for (std::size_t sample = 0; sample < sizes.size(); ++sample) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store, false); // First exposure read belongs to the guarded probe.
        const auto handles = populate(s, "cohort", sizes[sample]);
        reads[sample][0] = measure_exposure(s, store, rig, "cohort", sizes[sample], 0);
        const auto first = s.expected("cohort");
        s.put("cohort", handles[handles.size() / 2], 3.5);
        ce::require(s.expected("cohort") != first, "middle mutation changes the oracle answer");
        reads[sample][1] = measure_exposure(s, store, rig, "cohort", sizes[sample], 1);
        s.erase("cohort", handles.back());
        reads[sample][2] = measure_exposure(s, store, rig, "cohort", sizes[sample], 2);
        s.put("cohort", handles.back(), 0.001); // Reuse its retained old position.
        reads[sample][3] = measure_exposure(s, store, rig, "cohort", sizes[sample], 3);
    }
    // Owner's proposed maintained-result contract, fixed before any green
    // implementation measurement. Base performs U(U+1)=306/4290 position
    // searches through TWO cache pages. CI must retain the actual read red.
    // The eventual E descriptor/id lookup layout must prove these allowances;
    // they must not be fitted to its observed read counts.
    enforce_exposure_bounds(reads);
}

void s4_cohort_exposure_seeded_bits() {
    ce::Generator rng; // Exact seed 0x4f31533445, shared deterministic generator only.
    enum Coverage : unsigned {
        origin_first = 1U, units_first = 2U, set = 4U, credit = 8U, erase = 16U,
        orphan = 32U, repeated = 64U, interleaved = 128U, clear = 256U, reuse = 512U
    };
    unsigned coverage = 0;
    std::size_t comparisons = 0;
    for (std::size_t n : {0U, 1U, 2U, 3U, 7U, 17U, 33U, 65U}) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        const auto old_cohort = s.ensure("cohort");
        (void)s.ensure("side");
        std::vector<no::RequestHandle> handles;
        for (std::size_t i = 0; i < n; ++i) {
            const auto h = handle(1000 + n - i);
            handles.push_back(h);
            const double value = i < 3 ? std::array<double, 3>{{0.1, 0.2, 0.3}}[i]
                                       : rng.quantity();
            if (i % 2) {
                s.put("cohort", h, value);
                s.append("cohort", h);
                coverage |= units_first;
            } else {
                s.append("cohort", h);
                s.put("cohort", h, value);
                coverage |= origin_first;
            }
        }
        ce::require(s.model.at("cohort").units.size() == n, "seeded initial U is exact");
        // Same incarnation in another cohort must not alias the primary value.
        s.append("side", handle(1001));
        s.put("side", handle(1001), 0.01);
        coverage |= interleaved;
        const auto before_orphan = s.expected("cohort");
        s.put("cohort", handle(800000), 0x1.fffffffffffffp1023);
        ce::require(s.expected("cohort") == before_orphan, "orphan units cannot contribute");
        coverage |= orphan;
        s.erase("cohort", handle(800000));
        const std::size_t steps = n <= 17 ? 32 : 8;
        for (std::size_t step = 0; step < steps; ++step) {
            const auto h = handles.empty() ? handle(700000)
                : handles[static_cast<std::size_t>(rng.next() % handles.size())];
            switch (step % 4) {
            case 0:
                s.put("cohort", h, rng.finite());
                coverage |= set;
                break;
            case 1: {
                const auto at = s.model.at("cohort").units.find(h.incarnation);
                const double old = at == s.model.at("cohort").units.end() ? 0.0 : at->second;
                // Fixture writes the already-rounded canonical per-origin value,
                // exactly as an opening credit does; it is not a fill-event fold.
                s.put("cohort", h, old + (step % 8 == 1 ? 0.01 : 0.125));
                coverage |= credit;
                break;
            }
            case 2:
                s.erase("cohort", h);
                coverage |= erase;
                break;
            case 3: {
                const auto fresh = handle(900000 + step);
                s.put("cohort", fresh, rng.quantity());
                s.append("cohort", fresh);
                handles.push_back(fresh);
                coverage |= units_first;
                break;
            }
            }
        }
        const auto repeat = handles.empty() ? handle(600000) : handles.front();
        s.put("cohort", repeat, 0.1);
        s.append("cohort", repeat);
        s.append("cohort", repeat);
        coverage |= repeated;
        s.clear_units("cohort");
        equal_bits(s.adapter().source_unclosed_qty_for("cohort"), 0, "all units lost -> +0");
        s.put("cohort", repeat, 0.3); // Both retained occurrences regain their value.
        coverage |= clear;
        s.clear_all();
        const auto new_cohort = s.ensure("cohort");
        ce::require(new_cohort.value != old_cohort.value, "clear/reuse gets a fresh native cohort");
        s.append("cohort", handle(999001));
        s.put("cohort", handle(999001), 0.001);
        coverage |= reuse;
        comparisons += s.comparisons;
    }
    ce::require(coverage == 1023U, "seeded stream executes every required canonical mutation class");
    std::printf("cohort seeded bits seed=0x4f31533445 coverage=%x comparisons=%zu\n",
                coverage, comparisons);
}

void s4_cohort_exposure_middle_delete_bits() {
    const std::array<std::vector<double>, 4> triples{{
        {0.1, 0.2, 0.3}, {1, 0x1p53, 1}, {0x1p53, 1, 1}, {1, 1, 0x1p53}
    }};
    std::set<std::uint64_t> observed_expected;
    for (std::size_t test = 0; test < triples.size(); ++test) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store);
        (void)s.ensure("middle");
        const std::array<no::RequestHandle, 3> handles{{handle(30), handle(10), handle(20)}};
        for (std::size_t i = 0; i < 3; ++i) {
            s.append("middle", handles[i]);
            s.put("middle", handles[i], triples[test][i]);
        }
        observed_expected.insert(s.expected("middle"));
        s.erase("middle", handles[1]);
        if (test == 1) {
            // [1,2^53,1] -> [1,1], exactly 2 (not old-total - 2^53 = 0).
            equal_bits(s.adapter().source_unclosed_qty_for("middle"), 0x4000000000000000ULL,
                       "middle-delete analytic pin");
        }
        observed_expected.insert(s.expected("middle"));
        s.put("middle", handles[1], triples[test][1]); // Same retained roster position.
        s.put("middle", handles[1], 0.01);              // Replace middle value.
        observed_expected.insert(s.expected("middle"));
        s.put("middle", handles[1], triples[test][1]);
        // Monotone appends/reverse unit installation force rebalancing in a
        // future position tree; the test does not depend on its shape APIs.
        for (std::size_t i = 0; i < 9; ++i) s.append("middle", handle(100 + i));
        for (std::size_t i = 9; i > 0; --i)
            s.put("middle", handle(99 + i), i % 2 ? 0.001 : 0.125);
        for (std::size_t i = 0; i < 9; i += 2) s.erase("middle", handle(100 + i));
        s.put("middle", handles[1], 0.5);
        observed_expected.insert(s.expected("middle"));
    }
    ce::require(observed_expected.size() >= 4,
                "middle-delete/replace witnesses require distinct oracle bit answers");
}

void s4_cohort_exposure_filters_and_regain_bits() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    (void)s.ensure("filters");
    const std::vector<double> values{
        ce::from_bits(0x7ff8000000000001ULL), ce::from_bits(0x7ff8abcdef012345ULL),
        ce::from_bits(0xfff8000000000042ULL), ce::from_bits(0x7ff0000000000000ULL),
        ce::from_bits(0xfff0000000000000ULL), -1.0, -0x1p-1074, 0.0,
        ce::from_bits(0x8000000000000000ULL), 0x1p-1074
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        s.append("filters", handle(i + 1));
        s.put("filters", handle(i + 1), values[i]);
    }
    equal_bits(s.adapter().source_unclosed_qty_for("filters"), 1, "epsilon-free minimum subnormal");
    s.append("filters", handle(20));
    s.put("filters", handle(20), 0x1.fffffffffffffp1023);
    s.append("filters", handle(21));
    s.put("filters", handle(21), 0x1.fffffffffffffp1023);
    equal_bits(s.adapter().source_unclosed_qty_for("filters"), 0, "finite-row overflow clamps +0");
    s.erase("filters", handle(21));
    equal_bits(s.adapter().source_unclosed_qty_for("filters"), 0x7fefffffffffffffULL,
               "deleting overflowing row restores maximum finite exposure");
    s.clear_units("filters");
    equal_bits(s.adapter().source_unclosed_qty_for("filters"), 0, "clear clamps positive zero");
    const auto named_zero = (std::as_const(s.adapter()).*access(FromEntryTag{}))("filters");
    ce::require(named_zero.has_value(), "empty named cohort still has an optional quantity");
    equal_bits(*named_zero, 0, "empty named optional is positive zero");
    s.put("filters", handle(1), 0.001); // Old origin changes from filtered NaN to valid.
    s.append("filters", handle(30));
    s.put("filters", handle(30), 0.01);
    s.erase("filters", handle(1));
    s.erase("filters", handle(30));
    equal_bits(s.adapter().source_unclosed_qty_for("filters"), 0, "all rows erased -> +0");
    s.append("filters", handle(31));
    s.put("filters", handle(31), 1.0 / 3.0);
    // The ACTUAL empty source id is a cohort. Empty from_entry is an optional
    // whole-position selector, and must remain a different API behaviour.
    (void)s.ensure("");
    s.append("", handle(40));
    s.put("", handle(40), 0.25);
    equal_bits(s.adapter().source_unclosed_qty_for(""), 0x3fd0000000000000ULL,
               "empty source id has exposure");
    const auto absent = (std::as_const(s.adapter()).*access(FromEntryTag{}))("");
    ce::require(!absent.has_value(), "empty from_entry returns no named-cohort quantity");
    const auto named = (std::as_const(s.adapter()).*access(FromEntryTag{}))("filters");
    ce::require(named.has_value(), "nonempty from_entry returns a named-cohort quantity");
    equal_bits(*named, s.expected("filters"), "named optional follows the same oracle");
    s.clear_all();
    (void)s.ensure("filters");
    s.append("filters", handle(50));
    s.put("filters", handle(50), 0.1);
}

void canonical_position_owners(Subject& s, const std::string& id) {
    const auto& cohorts = std::as_const(s.adapter()).*access(CohortsTag{});
    const auto found = cohorts.find(id);
    ce::require(found != cohorts.end(), "position witness names real canonical facts");
    const auto& roster = found->second.origins;
    std::set<std::uint64_t> incarnations;
    for (const auto& h : s.model.at(id).origins) incarnations.insert(h.incarnation);
    std::map<std::size_t, std::uint64_t> owners;
    for (const auto incarnation : incarnations) {
        roster.visit_incarnation_positions(incarnation, [&](std::size_t p) {
            ce::require(p >= roster.base() && p < roster.size(), "index names a retained position");
            ce::require(roster[p].incarnation == incarnation, "position index names canonical owner");
            ce::require(owners.emplace(p, incarnation).second,
                        "canonical real roster positions have exactly one owner");
            return true;
        });
    }
    ce::require(owners.size() == roster.size() - roster.base(),
                "position index covers every retained canonical occurrence");
}

void s4_cohort_exposure_positions_and_ties() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject s(store);
    (void)s.ensure("positions");
    s.append("positions", handle(1)); // Two releasable historical rows.
    s.append("positions", handle(2));
    const auto a = handle(30), b = handle(20);
    s.append("positions", a);
    s.append("positions", b);
    s.append("positions", a); // Real roster [dead,dead,A,B,A], not a forged index.
    s.put("positions", a, 0.1);
    s.put("positions", b, 0.2);
    s.put("positions", handle(5), 0x1.fffffffffffffp1023); // Orphan must remain absent.
    equal_bits(s.adapter().source_unclosed_qty_for("positions"), 0x3fd999999999999aULL,
               "A contributes at both positions");
    canonical_position_owners(s, "positions");
    s.retire_single_cohort("positions", 2);
    s.put("positions", a, 0.0); // Key PRESENCE pins retirement, even when filtered.
    s.retire_single_cohort("positions", 2);
    s.put("positions", a, ce::from_bits(0x7ff8000000000001ULL));
    s.retire_single_cohort("positions", 2);
    s.put("positions", a, 0.3);
    equal_bits(s.adapter().source_unclosed_qty_for("positions"), 0x3fe999999999999aULL,
               "one unit update changes both retained A occurrences");
    s.erase("positions", a);
    s.retire_single_cohort("positions", 3); // B now pins; first A position is gone.
    s.put("positions", a, 0.3);
    equal_bits(s.adapter().source_unclosed_qty_for("positions"), 0x3fe0000000000000ULL,
               "only the retained A occurrence regains units: .2+.3=.5");
    canonical_position_owners(s, "positions");
    // Healthy append cannot give two incarnations the same position.
    // Ties therefore use the PURE specification corpus and analytic pins,
    // never a fabricated engine position-key edit or a claimed restore seam.
    bool found_tie = false;
    for (const auto& c : ce::adversarial_corpus()) {
        if (c.name != "position_tie_filter_before_add") continue;
        ce::require(ce::prepare({c}).size() == 1, "equal-position specification pins agree");
        found_tie = true;
    }
    ce::require(found_tie, "tied-position helper case is present");
}

ps::PlacementSnapshot deferred_exit(const std::string& from_entry) {
    ps::PlacementSnapshot row;
    row.family = ps::PineOrderFamily::ExitLimit;
    row.source_id = "exit";
    row.from_entry = from_entry;
    row.opening = false;
    row.deferred_cohort = true;
    row.is_long = true;
    row.qty_percent = 100.0;
    row.exit_levels.limit = 200.0;
    return row;
}

no::Request pending_request(no::CohortHandle cohort) {
    no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "exit", ""};
    request.trigger = no::Limit{200.0};
    request.owner = no::BindCohort{cohort};
    return request;
}

void install_pending(Subject& s, const no::Request& request, const ps::PlacementSnapshot& snapshot) {
    // Existing queue 0 is the actual pending-bracket owner.
    s.adapter().fixture_queue_state(0, nullptr, nullptr, {}, {});
    ce::require(s.adapter().pending_intent_view().size() == 0, "pending fixture clear is real");
    s.adapter().fixture_queue_state(0, &request, &snapshot, {}, {});
    ce::require(s.adapter().pending_intent_view().size() == 1, "pending population Q is exactly one");
}

void check_reservation(const pf_pending_order_v1_t& row, const std::string& id,
                       std::uint64_t expected) {
    ce::require(std::strcmp(row.from_entry, id.c_str()) == 0, "copy_v1 selected the named exit");
    ce::require(row.quantity_reservation_present == (expected ? 1U : 0U),
                "reservation presence follows finite-positive oracle exposure");
    equal_bits(row.quantity_reservation_units, expected, "copy_v1 reservation units");
    equal_bits(row.quantity_reservation_basis_units, expected, "copy_v1 reservation basis units");
}

QueryCost measure_pending(Subject& s, ScratchStore& store, Rig& rig,
                          const std::string& id, std::size_t n, std::size_t phase) {
    const auto expected = s.expected(id);
    const auto& view = s.adapter().pending_intent_view();
    ce::require(view.size() == 1, "measured copy has fixed Q=1");
    QueryGuard guard(store, rig);
    pf_pending_order_v1_t warm{}, actual{};
    int warm_status = -1, status = -1;
    std::size_t warm_peak = 0, peak = 0;
    {
        QueryAllocations window;
        warm_status = view.copy_v1(0, &warm);
        warm_peak = window.growth();
    }
    ce::require(warm_status >= 0, "warm copy_v1 executes");
    check_reservation(warm, id, expected);
    guard.start(warm_peak);
    {
        QueryAllocations window;
        status = view.copy_v1(0, &actual);
        peak = window.growth();
    }
    ce::require(status >= 0, "measured copy_v1 executes");
    check_reservation(actual, id, expected);
    const auto cost = guard.finish(
        peak, "named-pending-exposure-bound", n, s.model.at(id).units.size(), phase);
    s.canonical_matches(id);
    return cost;
}

void s4_named_pending_exposure_bound_red() {
    std::array<std::array<QueryCost, 4>, 2> exposure_reads{}, copy_reads{};
    const std::array<std::size_t, 2> sizes{{17, 65}};
    std::uint64_t materialization_allowance = 0;
    for (std::size_t sample = 0; sample < sizes.size(); ++sample) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject s(store, false); // No exposure pre-query in fixture setup or edits.
        const auto handles = populate(s, "cohort", sizes[sample]);
        const auto cohort = s.edit("cohort", CohortEdit::lookup).first;
        const auto request = pending_request(cohort);
        const auto snapshot = deferred_exit("cohort");
        install_pending(s, request, snapshot);
        // Fixed fixture-size allowance, derived from encoded input, NEVER
        // observed green reads: Q=1 performs two bracket-row decodes (selected
        // row and sibling). Every nonempty primitive consumes >=1 encoded
        // byte and at most two 256-byte chunk pieces for these short fields.
        // A piece find/read has <=128 page references at ordered/handle
        // depths <=3 (the existing s4 bound uses <=112 for a scalar). Add
        // 1024 for fixed queue-size/offset reads. This deliberately loose v1
        // allowance is independent of U; E retains its separate 1024 ceiling.
        ws::Save count;
        count(request, snapshot, std::string{}, std::uint64_t{0});
        const auto fixed_allowance = 2 * 2 * 128 * count.position() + 1024;
        if (sample == 0) materialization_allowance = fixed_allowance;
        ce::require(fixed_allowance == materialization_allowance,
                    "copy materialization allowance cannot grow with U");
        const auto initial = s.expected("cohort");
        for (std::size_t phase = 0; phase < 4; ++phase) {
            if (phase == 1) s.put("cohort", handles[handles.size() / 2], 3.5);
            if (phase == 2) s.clear_units("cohort");
            if (phase == 3) s.put("cohort", handles.back(), 0.001);
            if (phase == 1)
                ce::require(s.expected("cohort") != initial, "named reservation is value-sensitive");
            // Companion measurement of the SAME E facade, separately charged
            // from total copy. No unavailable internal tracing seam is invented.
            exposure_reads[sample][phase] =
                measure_exposure(s, store, rig, "cohort", sizes[sample], phase);
            copy_reads[sample][phase] =
                measure_pending(s, store, rig, "cohort", sizes[sample], phase);
        }
        // Empty from_entry does not reserve even when the empty-id cohort
        // itself holds units. This is a real copy_v1 control outside the bound.
        (void)s.ensure("");
        s.append("", handle(999999));
        s.put("", handle(999999), 0.5);
        const auto global = deferred_exit("");
        no::Request global_request{no::Reduce{no::ExplicitUnits{1.0}}, "exit", ""};
        global_request.trigger = no::Limit{200.0};
        install_pending(s, global_request, global);
        pf_pending_order_v1_t row{};
        ce::require(s.adapter().pending_intent_view().copy_v1(0, &row) >= 0,
                    "empty-from-entry pending control executes");
        check_reservation(row, "", 0);
    }
    // Report all samples before the expected base cost failure.
    std::printf("fixed-v1-materialization-allowance=%llu plus E=%llu\n",
                static_cast<unsigned long long>(materialization_allowance),
                static_cast<unsigned long long>(exposure_read_ceiling));
    std::fflush(stdout);
    enforce_exposure_bounds(exposure_reads);
    for (const auto& sample : copy_reads)
        for (const auto& cost : sample)
            for (const auto reads : cost.reads)
                ce::require(reads <= materialization_allowance + exposure_read_ceiling,
                            "warm/measured named copy_v1 fits fixed v1 materialization + E allowance");
}

struct TestCase { const char* name; void (*body)(); };
const TestCase cases[] = {
    {"s4_cohort_exposure_read_bound_red", &s4_cohort_exposure_read_bound_red},
    {"s4_cohort_exposure_seeded_bits", &s4_cohort_exposure_seeded_bits},
    {"s4_cohort_exposure_middle_delete_bits", &s4_cohort_exposure_middle_delete_bits},
    {"s4_cohort_exposure_filters_and_regain_bits", &s4_cohort_exposure_filters_and_regain_bits},
    {"s4_cohort_exposure_positions_and_ties", &s4_cohort_exposure_positions_and_ties},
    {"s4_named_pending_exposure_bound_red", &s4_named_pending_exposure_bound_red}
};
} // namespace

int main(int argc, char** argv) {
    // Do not reset live_bytes or disable recording between query windows:
    // engine frames can survive a read and be released by a later mutation.
    global_allocation::recording = true;
    bool selected = false;
    int failures = 0;
    for (const auto& test : cases) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0) continue;
        selected = true;
        try {
            ce::require_round_to_nearest();
            test.body();
            std::printf("%s: PASS\n", test.name);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "%s: FAIL: %s\n", test.name, error.what());
        }
    }
    if (!selected) std::fprintf(stderr, "unknown cohort-exposure case\n");
    return selected && failures == 0 ? 0 : 1;
}
