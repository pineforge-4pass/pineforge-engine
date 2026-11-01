// LAB-O1X5: real X25 child selections, X26 paired-close selections, and X27
// flat owner/ended-source selections, at increasing populations. The source
// call, its selected rows and the bytes simultaneously alive are measured.
// Compile this same TU on 21ba62bd: its vector snapshots exceed kScratchBytes.
//
// No baseline run or after-run was performed on the executor Mac. The
// supervisor must record fail-before, pass-after and the full parity result.
// Versioned object payloads, E13 visitors and the retained native openings
// owner remain integration requests in docs/o1inv-a7-cx-handoff-LAB-x5.md.
#include <pineforge/source/pine_adapter.hpp>

#if __has_include(<pineforge/source/exit_scratch.hpp>)
#define PF_HAS_EXIT_SCRATCH 1
#include <pineforge/source/exit_scratch.hpp>
#include "../src/source/pine_command_scratch_production.hpp"
#else
#define PF_HAS_EXIT_SCRATCH 0
#endif

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Handle = native_order::RequestHandle;
using Family = source::PineOrderFamily;

int checks = 0;
int failures = 0;
const char* scenario = "";
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, scenario, #value); \
} } while (0)

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct Children { friend auto access(Children); };
struct Cohorts { friend auto access(Cohorts); };
struct CohortSlots { friend auto access(CohortSlots); };
#if PF_HAS_EXIT_SCRATCH
struct CancelText { friend auto access(CancelText); };
struct Relative { friend auto access(Relative); };
struct ScratchOwner { friend auto access(ScratchOwner); };
struct ScratchState { friend auto access(ScratchState); };
template struct Access<CancelText, static_cast<void (Adapter::*)(
    const source::detail::ScratchByteSource&)>(&Adapter::cancel_exit_orders_for_full_close)>;
template struct Access<Relative, &Adapter::pending_relative_exits_>;
template struct Access<ScratchOwner, &Adapter::exit_scratch_>;
template struct Access<ScratchState, &pineforge::detail::CommandScratchStore::state_>;
#endif
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<Children, &Adapter::per_entry_child_legs>;
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<CohortSlots, &Adapter::cohort_opened_slot_count_>;

constexpr std::size_t kScratchBytes = 1024;
std::size_t peak_bytes = 0;
void allocation(std::size_t) {
    peak_bytes = std::max(peak_bytes, global_allocation::live_bytes);
}
struct Measure {
    Measure() {
        CHECK(!global_allocation::recording);
        CHECK(global_allocation::live_bytes == 0);
        peak_bytes = 0;
        global_allocation::on_recorded = allocation;
        global_allocation::recording = true;
    }
    ~Measure() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        std::printf("%s: peak=%zu bound=%zu\n", scenario, peak_bytes, kScratchBytes);
        CHECK(peak_bytes <= kScratchBytes);
        CHECK(global_allocation::live_bytes == 0);
    }
};

#if PF_HAS_EXIT_SCRATCH
struct WorkspaceCharge {
    std::size_t bytes = 0;
    WorkspaceCharge() {
        CHECK(!global_allocation::recording);
        CHECK(global_allocation::live_bytes == 0);
        global_allocation::recording = true;
    }
    void finish() {
        bytes = global_allocation::live_bytes;
        global_allocation::recording = false;
        global_allocation::live_bytes = 0;
    }
};

struct ScratchFixture {
    // Production disk backing, charged separately from the two 512-byte
    // RESIDENT cache pages. Each test gets a new logical empty backing.
    static inline std::size_t fixed_workspace_bytes = 0;
    static inline std::uint64_t fixed_resident_bytes = 0;
    native_order::RunIdentity run{native_order::SessionKey{"exit-scratch"}, 1};
    WorkspaceCharge workspace;
    source::detail::ExitScratchOrderStats order;
    std::unique_ptr<pineforge::detail::ScratchBacking> backing =
        source::detail::make_command_scratch_backing(512);
    pineforge::detail::CommandScratchStore store{
        {2, 512, 1, 1, source::detail::exit_scratch_key_order(&order),
         pineforge::detail::ScratchOpenMode::create_empty}, backing.get()};
    source::detail::ExitScratchContext context{store, run};
    std::size_t max_scope_rows = 0;
    std::uint64_t max_store_rows = 0;
    std::size_t lookup_calls = 0;
    std::size_t lookup_rows = 0;
    std::size_t max_lookup_rows = 0;
    std::uint64_t query_page_reads = 0;
    std::uint64_t max_query_page_reads = 0;

    ScratchFixture() {
        workspace.finish();
        CHECK(backing->kind() == pineforge::detail::ScratchBackingKind::disk);
        CHECK(store.status() == pineforge::detail::ScratchStatus::ok);
        CHECK(workspace.bytes != 0);
        if (fixed_workspace_bytes == 0) fixed_workspace_bytes = workspace.bytes;
        CHECK(workspace.bytes == fixed_workspace_bytes);
        if (fixed_resident_bytes == 0) fixed_resident_bytes = store.stats().resident_bytes;
        CHECK(store.stats().resident_bytes == fixed_resident_bytes);
        context.observation = this;
        context.observe_rows = [](void* opaque, std::uint64_t, std::size_t rows) noexcept {
            auto& fixture = *static_cast<ScratchFixture*>(opaque);
            fixture.max_scope_rows = std::max(fixture.max_scope_rows, rows);
            fixture.max_store_rows = std::max(fixture.max_store_rows,
                                              fixture.store.record_count());
        };
        context.observe_lookup = [](void* opaque, std::size_t rows) noexcept {
            auto& fixture = *static_cast<ScratchFixture*>(opaque);
            ++fixture.lookup_calls;
            fixture.lookup_rows += rows;
            fixture.max_lookup_rows = std::max(fixture.max_lookup_rows, rows);
        };
    }
    ~ScratchFixture() {
        CHECK(store.record_count() == 0);
        CHECK(!store.transaction_active());
    }
    void bind(Adapter& adapter) {
        adapter.bind_exit_scratch(context);
        CHECK(&source::detail::exit_scratch(adapter) == &context);
    }
    void population(std::size_t rows) const {
        CHECK(max_scope_rows >= rows);
        CHECK(max_store_rows >= rows);
        CHECK(backing->page_count() > 2);
        CHECK(store.stats().resident_bytes == fixed_resident_bytes);
        CHECK(max_lookup_rows <= 1);
        std::printf("%s: rows=%zu workspace=%zu total-heap-bound=%zu backing-pages=%llu "
                    "lookups=%zu candidates=%zu max-per-query=%zu comparisons=%llu "
                    "query-page-reads=%llu max-query-page-reads=%llu\n",
            scenario, rows, workspace.bytes, workspace.bytes + kScratchBytes,
            static_cast<unsigned long long>(backing->page_count()),
            lookup_calls, lookup_rows, max_lookup_rows,
            static_cast<unsigned long long>(order.comparisons),
            static_cast<unsigned long long>(query_page_reads),
            static_cast<unsigned long long>(max_query_page_reads));
    }
    template<class Query> auto query(Query read, std::size_t text_bytes = 0) {
        const auto comparisons = order.comparisons;
        const auto calls = lookup_calls;
        const auto candidates = lookup_rows;
        const auto before = store.stats();
        const auto result = read();
        CHECK(lookup_calls == calls + 1);
        CHECK(lookup_rows - candidates <= 1);
        std::uint64_t levels = 1;
        for (auto rows = store.record_count(); rows > 1; rows = (rows + 1) / 2) ++levels;
        CHECK(order.comparisons - comparisons <= 8 * levels);
        const auto after = store.stats();
        const auto reads = after.page_reads - before.page_reads;
        query_page_reads += reads;
        max_query_page_reads = std::max(max_query_page_reads, reads);
        CHECK(after.page_writes == before.page_writes);
        CHECK(after.syncs == before.syncs);
        // A seek's binary comparisons read a radix path, an index slot and
        // the key's blob pages. Point/read validation adds two radix paths.
        // 512-byte pages have 472-byte blob payloads; comparison reads use
        // 128-byte chunks. Charge those costs explicitly, independent of N.
        const auto key_bytes = source::detail::exit_scratch_key_bytes + text_bytes;
        const auto chunks = 1 + (text_bytes + 127) / 128;
        const auto pages = (key_bytes + 471) / 472;
        const auto per_comparison = levels + 2 + chunks + 2 * pages;
        CHECK(reads <= (8 * levels + 2) * per_comparison
                     + 4 * (levels + chunks * (pages + 1)));
        return result;
    }
};
#else
struct ScratchFixture {
    void bind(Adapter&) {}
    void population(std::size_t) const {}
};
#endif

Handle handle(std::uint64_t incarnation) {
    return {{native_order::SessionKey{"exit-scratch"}, 1}, incarnation};
}

struct EmptyHost final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

native_order::ExecutionAppliedEvent event() {
    auto definition = std::make_shared<native_order::RequestDefinition>();
    definition->handle = handle(1000000);
    definition->request.intent = native_order::Transact{1.0};
    native_order::ExecutionAppliedEvent result;
    result.definition = definition;
    result.terminal = false;
    return result;
}

#if PF_HAS_EXIT_SCRATCH
struct IoChargeLog {
    // Fixed fixture: P=512, two cache slots, 1024 backing pages, at most 65 rows.
    // Keys (32 bytes) and incarnations (8 bytes) each occupy one 56-byte cell.
    // Ordered leaves hold (512-16-32)/8=58 handles; handle leaves hold
    // (512-16-16)/32=15, with radix fanout (512-16-16)/8=60.
    // Thus both trees have height <=2, with at most one ordered separator.
    static_assert(pineforge::detail::scratch_min_page_size == 512);
    static_assert(source::detail::kExitScopeEraseBatch == 64);
    static constexpr std::size_t max_population = source::detail::kExitScopeEraseBatch + 1;
    // A cache rd costs <=2 charges (dirty eviction + read); wr costs <=4
    // (that fetch + pool read + journal write). A fresh wr_page costs <=1.
    // descend: root/leaf counts + child read, then <=1+6+1 comparisons,
    // each reading an index slot, two handle pages and one stored-key cell.
    static constexpr std::size_t comparisons = 1 + 6 + 1;
    static constexpr std::size_t descend_reads = 3 + comparisons * (1 + 2 + 1);
    // hset insertion: two new pages, four rd, four wr. Erase: four rd,
    // six wr, including the empty handle leaf and its parent slot/count.
    static constexpr std::size_t handle_insert_charges = 2 + 4 * 2 + 4 * 4;
    static constexpr std::size_t handle_erase_charges = 4 * 2 + 6 * 4;
    // Ordered append split: rd leaf, wr leaf, two fresh pages. Ordered erase:
    // four rd + six wr, plus separator hget, cell free and possible hset.
    static constexpr std::size_t index_insert_charges = 2 + 4 + 2;
    static constexpr std::size_t index_erase_charges =
        4 * 2 + 6 * 4 + 2 * 2 + (2 + 4) + handle_erase_charges;
    // Buffered commit: journal-tail read/write, <=2 dirty cache writes,
    // one superblock write. begin and create_empty issue no backing calls.
    static constexpr std::size_t commit_charges = 2 + 2 + 1;
    static constexpr std::size_t insert_charges =
        2 * descend_reads + 2 * (1 + 4) + handle_insert_charges
        + index_insert_charges + commit_charges;
    // Consumption is idle/clean: lower_bound adds two rd after descend,
    // and reading key words and value each adds two handle rd + one cell rd.
    static constexpr std::size_t consume_charges = descend_reads + 2 + 2 * (2 + 1);
    // Teardown seeks may evict dirty pages. Erase adds a stored probe read
    // per comparison, initial hget, leaf_entry, and one separator-name read.
    static constexpr std::size_t scope_seek_charges = 2 * (descend_reads + 2 + 2 + 1);
    static constexpr std::size_t erase_charges =
        2 * (descend_reads + comparisons + 2 + 1 + 1)
        + 2 * (2 + 4) + handle_erase_charges + index_erase_charges;
    // 119 + 43 + 80 + 212 + 5 = 459 charges/row, including teardown.
    // Empty-scope lookups/destructors add zero: 459 * (64+1) = 29835.
    static constexpr std::size_t capacity = max_population
        * (insert_charges + consume_charges + scope_seek_charges
           + erase_charges + commit_charges);
    std::array<pineforge::detail::ScratchIoCharge, capacity> entries{};
    std::size_t count = 0;
    bool overflow = false;

    static void charge(void* opaque,
                       const pineforge::detail::ScratchIoCharge& value) noexcept {
        auto& log = *static_cast<IoChargeLog*>(opaque);
        if (log.count == log.entries.size()) {
            log.overflow = true;
            return;
        }
        log.entries[log.count++] = value;
    }
};

bool same_io_charge(const pineforge::detail::ScratchIoCharge& left,
                    const pineforge::detail::ScratchIoCharge& right) {
    return left.kind == right.kind
        && left.operation == right.operation
        && left.page == right.page
        && left.bytes == right.bytes
        && left.status == right.status;
}

// Observe the first consumer comparison after the unconditional live-handle
// snapshot. Read fixed Core counters only; do not perform I/O, invoke another
// consumer, or install a row/I/O observer in this construction fixture.
struct ConstructionCounter {
    pineforge::detail::scratch_impl::Core* core = nullptr;
    IoChargeLog* charge_log = nullptr;
    std::uint64_t population = 0;
    std::uint64_t sequence_before = 0;
    std::uint64_t handles_before = 0;
    std::uint64_t publications = 0;
    std::uint64_t inserted = 0;
    std::uint64_t records = 0;
    std::size_t charge_count = 0;
    bool captured = false;
    bool consumer_idle = false;

    pineforge::detail::ScratchKeyOrder order() noexcept {
        const auto original = source::detail::exit_scratch_key_order();
        return {original.id, this,
            [](void* opaque, const source::detail::ScratchByteSource& left,
               const source::detail::ScratchByteSource& right, int& result) noexcept {
                auto& counter = *static_cast<ConstructionCounter*>(opaque);
                if (counter.core && !counter.captured && counter.population != 0
                        && counter.core->cur_.record_count == counter.population) {
                    // Each distinct ordinal allocates one fresh handle. The
                    // final put increments record_count after its comparisons;
                    // reaching this population identifies the first consumer,
                    // even if a broken builder left its transaction open.
                    counter.captured = true;
                    counter.consumer_idle = !counter.core->active_;
                    counter.publications = counter.core->cur_.seq - counter.sequence_before;
                    counter.inserted = counter.core->cur_.next_handle - counter.handles_before;
                    counter.records = counter.core->cur_.record_count;
                    counter.charge_count = counter.charge_log
                        ? counter.charge_log->count : 0;
                }
                const auto compare = source::detail::exit_scratch_key_order();
                return compare.compare(compare.context, left, right, result);
            }};
    }
};

void construction_publications() {
    namespace pd = pineforge::detail;
    namespace sd = source::detail;
    scenario = "bounded handle snapshot publications";
    // The insertion policy is the fixed 64-row batch constant shared with
    // teardown. It is not a page capacity: at P=512 the store's ordered leaf has
    // (512 - 16 trailer bytes - 32 header bytes) / 8 = 58 entries.
    constexpr std::size_t batch = sd::kExitScopeEraseBatch;
    static_assert(batch == 64, "construction acceptance uses a fixed 64-row batch");
    constexpr std::size_t page_size = pd::scratch_min_page_size;
    constexpr std::size_t backing_pages = 1024;
    for (const std::size_t population : {std::size_t{0}, std::size_t{1}, batch, batch + 1}) {
        std::array<std::byte, backing_pages * page_size> bytes{};
        pd::FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, page_size);
        ConstructionCounter counter;
        pd::CommandScratchStore store{
            {2, page_size, 1, 1, counter.order(), pd::ScratchOpenMode::create_empty}, &backing};
        CHECK(!backing.has_io_observer());
        CHECK(store.status() == pd::ScratchStatus::ok);
        CHECK(!store.has_io_observer());
        auto* state = store.*access(ScratchState{});
        CHECK(state != nullptr);
        if (!state || store.status() != pd::ScratchStatus::ok) continue;
        counter.core = state;
        counter.population = population;
        counter.sequence_before = state->cur_.seq;
        counter.handles_before = state->cur_.next_handle;
        sd::ExitScratchContext context{store, handle(1).run};
        EmptyHost host;
        Adapter adapter(host);
        adapter.bind_exit_scratch(context);
        auto& live = adapter.*access(Live{});
        for (std::size_t i = 0; i < population; ++i)
            live.push_back(handle(population - i));
        // No placements, cohorts or pending requests: only the generic flat
        // live-handle snapshot creates records. Its consumer finds no placement
        // and cannot cancel, submit, or build another nonempty selection.
        CHECK(!context.observe_rows && !context.observe_lookup);
        CHECK(!store.transaction_active()); // Applied entry
        adapter.on_applied(event(), {});
        CHECK(!store.transaction_active()); // Applied return and scope teardown
        CHECK(!context.teardown_failure.pending());
        CHECK(store.record_count() == 0);
        CHECK(state->cur_.next_handle - counter.handles_before == population);
        if (population == 0) {
            CHECK(!counter.captured);
            counter.publications = state->cur_.seq - counter.sequence_before;
            counter.inserted = state->cur_.next_handle - counter.handles_before;
        } else {
            CHECK(counter.captured);
            CHECK(counter.consumer_idle);
            CHECK(counter.records == population);
        }
        // Successful buffered commit_impl publishes one alternate superblock
        // and increments seq once. Sampled before consumption/erase, this is
        // construction work only; teardown publications are checked separately.
        const auto expected = population / batch + (population % batch != 0);
        CHECK(counter.inserted == population);
        CHECK(counter.publications == expected);
        CHECK(state->cur_.seq - counter.sequence_before
            == counter.publications + expected);
        std::array<std::byte, page_size> page{};
        std::uint64_t digest = 0;
        CHECK(store.state_digest({page.data(), page.size()}, digest) == pd::ScratchStatus::ok);
        CHECK(!store.transaction_active()); // hash boundary
        std::printf("%s: rows=%zu inserted=%llu publications=%llu expected=%zu\n",
            scenario, population, static_cast<unsigned long long>(counter.inserted),
            static_cast<unsigned long long>(counter.publications), expected);
    }
}

void observed_construction_publications() {
    namespace pd = pineforge::detail;
    namespace sd = source::detail;
    scenario = "observed handle snapshot publications";
    constexpr std::size_t batch = sd::kExitScopeEraseBatch;
    constexpr std::size_t page_size = pd::scratch_min_page_size;
    constexpr std::size_t backing_pages = 1024;
    for (const std::size_t population : {std::size_t{0}, std::size_t{1}, batch, batch + 1}) {
        std::array<std::byte, backing_pages * page_size> bytes{};
        IoChargeLog actual_log;
        pd::FixedMemoryScratchBacking backing(
            {bytes.data(), bytes.size()}, page_size,
            {&actual_log, &IoChargeLog::charge});
        ConstructionCounter counter;
        counter.charge_log = &actual_log;
        pd::CommandScratchStore store{
            {2, page_size, 1, 1, counter.order(), pd::ScratchOpenMode::create_empty}, &backing};
        CHECK(backing.has_io_observer());
        CHECK(store.has_io_observer());
        CHECK(store.status() == pd::ScratchStatus::ok);
        auto* state = store.*access(ScratchState{});
        CHECK(state != nullptr);
        if (!state || store.status() != pd::ScratchStatus::ok) continue;
        counter.core = state;
        counter.population = population;
        counter.sequence_before = state->cur_.seq;
        counter.handles_before = state->cur_.next_handle;
        const auto actual_start = actual_log.count;
        sd::ExitScratchContext context{store, handle(1).run};
        EmptyHost host;
        Adapter adapter(host);
        adapter.bind_exit_scratch(context);
        auto& live = adapter.*access(Live{});
        for (std::size_t i = 0; i < population; ++i)
            live.push_back(handle(population - i));
        CHECK(!context.observe_rows && !context.observe_lookup);
        CHECK(!store.transaction_active());
        adapter.on_applied(event(), {});
        CHECK(!store.transaction_active());
        CHECK(store.record_count() == 0);
        CHECK(state->cur_.next_handle - counter.handles_before == population);
        if (population == 0) {
            CHECK(!counter.captured);
            counter.publications = state->cur_.seq - counter.sequence_before;
            counter.inserted = state->cur_.next_handle - counter.handles_before;
            counter.charge_count = actual_log.count;
        } else {
            CHECK(counter.captured);
            CHECK(counter.consumer_idle);
            CHECK(counter.records == population);
        }
        CHECK(counter.inserted == population);
        CHECK(counter.publications == population);
        CHECK(counter.charge_count >= actual_start);

        // This is the unchanged per-row path: each scalar append invokes
        // exit_scratch_edit, so every row begins and publishes independently.
        std::array<std::byte, backing_pages * page_size> expected_bytes{};
        IoChargeLog expected_log;
        pd::FixedMemoryScratchBacking expected_backing(
            {expected_bytes.data(), expected_bytes.size()}, page_size,
            {&expected_log, &IoChargeLog::charge});
        ConstructionCounter expected_counter;
        expected_counter.charge_log = &expected_log;
        expected_counter.population = population;
        pd::CommandScratchStore expected_store{
            {2, page_size, 1, 1, expected_counter.order(),
             pd::ScratchOpenMode::create_empty}, &expected_backing};
        CHECK(expected_backing.has_io_observer());
        CHECK(expected_store.has_io_observer());
        CHECK(expected_store.status() == pd::ScratchStatus::ok);
        expected_counter.core = expected_store.*access(ScratchState{});
        CHECK(expected_counter.core != nullptr);
        sd::ExitScratchContext expected_context{expected_store, handle(1).run};
        const auto expected_start = expected_log.count;
        std::size_t expected_end = expected_start;
        {
            // The flat Applied path first creates an empty owner-selection
            // scope, performs its empty ordered lookup, then creates an empty
            // ended-source scope before the live-handle snapshot.
            sd::ExitScratchRows<std::string> exit_owners(expected_context);
            exit_owners.ordered_text(
                [](const sd::ExitScratchText&) { return true; }, true);
            sd::ExitScratchRows<std::string> ended_sources(expected_context);
            sd::ExitScratchRows<Handle> rows(expected_context);
            for (std::size_t i = 0; i < population; ++i)
                rows.push_back(handle(population - i));
            rows.finish();
            expected_end = expected_log.count;
            if (!rows.empty()) {
                // The adapter samples at the first consumer comparison,
                // after descend has already read index/handle pages. Reach
                // that same boundary in this independent scalar reference.
                (void)rows.front();
                CHECK(expected_counter.captured);
                CHECK(expected_counter.consumer_idle);
                expected_end = expected_counter.charge_count;
            }
        }
        if (population == 0) expected_end = expected_log.count;
        CHECK(!expected_store.transaction_active());
        CHECK(!actual_log.overflow);
        CHECK(!expected_log.overflow);
        CHECK(expected_end >= expected_start);
        if (counter.charge_count >= actual_start && expected_end >= expected_start) {
            const auto actual_count = counter.charge_count - actual_start;
            const auto expected_count = expected_end - expected_start;
            CHECK(actual_count == expected_count);
            if (actual_count == expected_count) {
                for (std::size_t index = 0; index < actual_count; ++index) {
                    CHECK(same_io_charge(actual_log.entries[actual_start + index],
                                         expected_log.entries[expected_start + index]));
                }
            }
        }
    }
}

void batch_visibility_boundaries() {
    namespace pd = pineforge::detail;
    namespace sd = source::detail;
    scenario = "batched row visibility boundaries";
    constexpr std::size_t page_size = pd::scratch_min_page_size;
    constexpr std::size_t backing_pages = 1024;
    std::array<std::byte, backing_pages * page_size> bytes{};
    pd::FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, page_size);
    pd::CommandScratchStore store{
        {2, page_size, 1, 1, sd::exit_scratch_key_order(),
         pd::ScratchOpenMode::create_empty}, &backing};
    CHECK(!backing.has_io_observer());
    CHECK(store.status() == pd::ScratchStatus::ok);
    if (store.status() != pd::ScratchStatus::ok) return;
    auto* state = store.*access(ScratchState{});
    CHECK(state != nullptr);
    if (!state) return;
    const auto sequence_before = state->cur_.seq;
    {
        sd::ExitScratchContext context{store, handle(1).run};
        sd::ExitScratchBatchRows<Handle> rows(context);
        try {
            rows.push_back(handle(2));
            rows.push_back(handle(1));
        } catch (const sd::ExitScratchError& error) {
            CHECK(false);
            CHECK(error.status() == pd::ScratchStatus::programming_error);
            if (store.transaction_active())
                CHECK(store.rollback() == pd::ScratchStatus::ok);
            return;
        }
        CHECK(rows.size() == 2);
        CHECK(store.transaction_active());
        CHECK(state->cur_.seq == sequence_before);
        CHECK(rows[0] == handle(2));
        CHECK(rows[1] == handle(1));
        rows.finish();
        CHECK(!store.transaction_active());
        CHECK(state->cur_.seq == sequence_before + 1);
        CHECK(rows[0] == handle(2));
        CHECK(rows[1] == handle(1));
    }
    CHECK(!store.transaction_active());
    CHECK(store.record_count() == 0);
}

namespace pd = pineforge::detail;
namespace sd = source::detail;

struct RowFailureObservation {
    const std::type_info* type = nullptr;
    std::string message;
    RunFailureValue failure;
    std::optional<pd::ScratchStatus> status;
    std::optional<std::error_code> error_code;
};

template<class Operation>
RowFailureObservation observe_row_failure(Operation operation) {
    RowFailureObservation result;
    try {
        operation();
    } catch (const std::exception& error) {
        result.type = &typeid(error);
        result.message = error.what();
        result.failure = classify_run_failure(error);
        if (const auto* scratch = dynamic_cast<const sd::ExitScratchError*>(&error))
            result.status = scratch->status();
        if (const auto* io = dynamic_cast<const std::system_error*>(&error))
            result.error_code = io->code();
    }
    return result;
}

void compare_row_failures(const RowFailureObservation& actual,
                          const RowFailureObservation& expected) {
    CHECK(expected.type != nullptr);
    CHECK(actual.type != nullptr);
    if (actual.type && expected.type) CHECK(*actual.type == *expected.type);
    CHECK(actual.message == expected.message);
    CHECK(actual.status == expected.status);
    CHECK(actual.error_code == expected.error_code);
    CHECK(actual.failure.code == expected.failure.code);
    CHECK(bool(actual.failure.args) == bool(expected.failure.args));
    if (actual.failure.args && expected.failure.args)
        CHECK(*actual.failure.args == *expected.failure.args);
}

class RowFaultBacking final : public pd::ScratchBacking {
public:
    explicit RowFaultBacking(pd::FixedMemoryScratchBacking& inner)
        : ScratchBacking(inner.kind(), inner.page_size()), inner_(inner) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return last_failed_ && typed_ ? &failure_ : nullptr;
    }
    void arm(bool typed) noexcept {
        fail_publication_ = true;
        typed_ = typed;
    }
    std::size_t faults = 0;
    pd::ScratchPageId failed_page = pd::scratch_no_page;

protected:
    pd::ScratchStatus do_read_page(pd::ScratchPageId page,
                                   pd::ScratchBuffer destination) const noexcept override {
        last_failed_ = false;
        return inner_.read_page(page, destination);
    }
    pd::ScratchStatus do_write_page(pd::ScratchPageId page,
                                    pd::ScratchBytes bytes) noexcept override {
        last_failed_ = false;
        // commit_impl publishes to next.seq & 1, so only pages 0 and 1
        // identify publication writes. Other writes still exercise the real
        // backing, including dirty evictions while comparing keys.
        if (fail_publication_ && page < 2) {
            fail_publication_ = false; // One refused write; rollback is serviceable.
            ++faults;
            failed_page = page;
            last_failed_ = true;
            return pd::ScratchStatus::io_error;
        }
        return inner_.write_page(page, bytes);
    }
    pd::ScratchStatus do_sync(pd::ScratchDurability durability) noexcept override {
        last_failed_ = false;
        return inner_.sync(durability);
    }

private:
    pd::FixedMemoryScratchBacking& inner_;
    pd::ReportScratchIoError failure_{ENOSPC, "exit scratch backing write"};
    bool fail_publication_ = false;
    bool typed_ = false;
    mutable bool last_failed_ = false;
};

struct PartialBatchFixture {
    // Same backing geometry as the construction fixtures; no observers, so
    // the bounded-batch path is exercised rather than its per-row fallback.
    static constexpr std::size_t page_size = pd::scratch_min_page_size;
    std::array<std::byte, 1024 * page_size> bytes{};
    pd::FixedMemoryScratchBacking memory{{bytes.data(), bytes.size()}, page_size};
    RowFaultBacking backing{memory};
    pd::CommandScratchStore store{
        {2, page_size, 1, 1, sd::exit_scratch_key_order(),
         pd::ScratchOpenMode::create_empty}, &backing};
    sd::ExitScratchContext context{store, handle(1).run};

    PartialBatchFixture() {
        CHECK(store.status() == pd::ScratchStatus::ok);
        CHECK(!store.has_io_observer());
        CHECK(!context.observe_rows && !context.observe_lookup);
    }
    void check_clean() const {
        CHECK(!store.transaction_active());
        CHECK(!context.teardown_failure.pending());
        CHECK(store.record_count() == 0);
    }
};

enum class PartialBatchFailure { identity, commit, teardown };

template<sd::ExitScratchRowMode Mode>
RowFailureObservation partial_batch_failure(PartialBatchFailure fault,
        std::size_t committed, std::size_t pending, bool typed = false) {
    PartialBatchFixture fixture;
    RowFailureObservation result;
    const auto cleanup = observe_row_failure([&] {
        sd::ExitScratchRows<Handle, Mode> rows(fixture.context);
        for (std::size_t i = 0; i < committed; ++i) rows.push_back(handle(i + 1));
        rows.finish();
        // Both modes attempt the same input rows. For a commit fault the
        // per-row reference fails while appending its last row; the batch
        // fails when finish publishes those already staged rows.
        const auto prepared = fault == PartialBatchFailure::commit
                && Mode == sd::ExitScratchRowMode::per_row ? pending - 1 : pending;
        for (std::size_t i = 0; i < prepared; ++i)
            rows.push_back(handle(committed + i + 1));
        CHECK(rows.size() == committed + prepared);
        CHECK(fixture.store.transaction_active()
            == (Mode == sd::ExitScratchRowMode::bounded_batch));
        if (fault == PartialBatchFailure::teardown)
            fixture.context.teardown_failure.remember(pd::ScratchStatus::corrupt, fixture.store);
        result = observe_row_failure([&] {
            switch (fault) {
            case PartialBatchFailure::identity: {
                auto foreign = handle(committed + prepared + 1);
                ++foreign.run.run_number;
                rows.push_back(foreign);
                break;
            }
            case PartialBatchFailure::commit:
                fixture.backing.arm(typed);
                if constexpr (Mode == sd::ExitScratchRowMode::per_row)
                    rows.push_back(handle(committed + pending));
                else rows.finish();
                break;
            case PartialBatchFailure::teardown:
                if constexpr (Mode == sd::ExitScratchRowMode::per_row)
                    rows.push_back(handle(committed + prepared + 1));
                else rows.finish();
                break;
            }
        });
        CHECK(result.type != nullptr);
        // Check before destruction too: a destructor-only fix cannot hide
        // an active transaction from the caller that catches this failure.
        CHECK(!fixture.store.transaction_active());
        CHECK(rows.size() == committed + prepared);
        // Rollback restores the last batch boundary. In the per-row reference
        // all preceding successful appends are already committed.
        const auto survivors = Mode == sd::ExitScratchRowMode::bounded_batch
            ? committed : committed + prepared;
        CHECK(fixture.store.record_count() == survivors);
        if (!fixture.store.transaction_active()) {
            // Inspect the store directly: the failed builder is abandoned,
            // and the teardown test deliberately still has a pending failure.
            pd::ScratchHandle current = 0;
            auto status = fixture.store.first(current);
            std::size_t visited = 0;
            for (; visited < survivors && status == pd::ScratchStatus::ok; ++visited) {
                sd::ExitScratchKeyWords key{};
                std::uint64_t incarnation = 0;
                CHECK(fixture.store.read(current, pd::ScratchField::key, 0,
                    {reinterpret_cast<std::byte*>(key.data()), sizeof(key)})
                    == pd::ScratchStatus::ok);
                CHECK((key == sd::ExitScratchKeyWords{rows.scope(), 0, 0, visited}));
                CHECK(fixture.store.read(current, pd::ScratchField::value, 0,
                    {reinterpret_cast<std::byte*>(&incarnation), sizeof(incarnation)})
                    == pd::ScratchStatus::ok);
                CHECK(incarnation == handle(visited + 1).incarnation);
                status = fixture.store.next(current, current);
            }
            CHECK(visited == survivors);
            CHECK(status == pd::ScratchStatus::end);
        }
        if (fault == PartialBatchFailure::commit) {
            CHECK(fixture.backing.faults == 1); // The armed failure is one shot.
            CHECK(fixture.backing.failed_page != pd::scratch_no_page);
            CHECK(fixture.backing.failed_page < 2);
        }
        if (fault == PartialBatchFailure::teardown) {
            CHECK(fixture.context.teardown_failure.pending());
            // Clear only this deliberately injected prior failure, after
            // observing rollback, so the ordinary scope erase can run.
            fixture.context.teardown_failure = sd::ExitScratchTeardownFailure{};
        } else CHECK(!fixture.context.teardown_failure.pending());
    });
    CHECK(cleanup.type == nullptr);
    // The relation differs just after rollback, but scope destruction erases
    // every surviving row in either mode. It must leave the context reusable.
    fixture.check_clean();
    return result;
}

void partial_batch_errors() {
    constexpr auto per_row = sd::ExitScratchRowMode::per_row;
    constexpr auto batched = sd::ExitScratchRowMode::bounded_batch;
    constexpr auto batch = sd::kExitScopeEraseBatch;
    for (const std::size_t committed : {std::size_t{0}, batch}) {
        for (const std::size_t pending : {std::size_t{17}, batch - 1}) {
            for (const auto fault : {PartialBatchFailure::identity, PartialBatchFailure::teardown}) {
                scenario = fault == PartialBatchFailure::identity
                    ? "partial batch foreign-run rollback" : "partial batch teardown rollback";
                // Derive exception type, status, code, arguments and message
                // from the unchanged per-row path with the same failed input.
                const auto expected = partial_batch_failure<per_row>(fault, committed, pending);
                const auto actual = partial_batch_failure<batched>(fault, committed, pending);
                compare_row_failures(actual, expected);
            }
            scenario = "partial batch backing commit failure";
            for (bool typed : {false, true}) {
                const auto expected = partial_batch_failure<per_row>(
                    PartialBatchFailure::commit, committed, pending, typed);
                const auto actual = partial_batch_failure<batched>(
                    PartialBatchFailure::commit, committed, pending, typed);
                compare_row_failures(actual, expected);
            }
        }
    }
}

void unfinished_batch_cleanup() {
    scenario = "unfinished batch scope cleanup";
    const std::runtime_error caller_failure("snapshot caller failure");
    const auto expected = observe_row_failure([&] { throw caller_failure; });
    for (bool unwinding : {false, true}) {
        PartialBatchFixture fixture;
        const auto actual = observe_row_failure([&] {
            sd::ExitScratchBatchRows<Handle> rows(fixture.context);
            for (std::size_t i = 0; i < sd::kExitScopeEraseBatch - 1; ++i)
                rows.push_back(handle(i + 1));
            CHECK(fixture.store.transaction_active());
            if (unwinding) throw caller_failure;
        });
        if (unwinding) compare_row_failures(actual, expected);
        else CHECK(actual.type == nullptr);
        fixture.check_clean();
    }
}

void partial_batch_boundary() {
    scenario = "partial batch final publication and move";
    PartialBatchFixture fixture;
    auto* state = fixture.store.*access(ScratchState{});
    CHECK(state != nullptr);
    if (!state) return;
    const auto sequence_before = state->cur_.seq;
    constexpr auto population = sd::kExitScopeEraseBatch - 1;
    const auto cleanup = observe_row_failure([&] {
        std::optional<sd::ExitScratchBatchRows<Handle>> moved;
        {
            sd::ExitScratchBatchRows<Handle> rows(fixture.context);
            for (std::size_t i = 0; i < population; ++i)
                rows.push_back(handle(population - i));
            CHECK(rows.size() == population);
            CHECK(state->cur_.seq == sequence_before);
            CHECK(fixture.store.transaction_active());
            moved.emplace(std::move(rows));
        } // The moved-from builder must not roll back the transferred batch.
        CHECK(fixture.store.transaction_active());
        CHECK(moved->size() == population);
        for (std::size_t i = 0; i < population; ++i)
            CHECK((*moved)[i] == handle(population - i));
        moved->finish();
        CHECK(!fixture.store.transaction_active());
        const auto publications = population / sd::kExitScopeEraseBatch
            + (population % sd::kExitScopeEraseBatch != 0);
        CHECK(state->cur_.seq == sequence_before + publications);
        moved->finish();
        CHECK(state->cur_.seq == sequence_before + publications);
        for (std::size_t i = 0; i < population; ++i)
            CHECK((*moved)[i] == handle(population - i));
    });
    CHECK(cleanup.type == nullptr);
    fixture.check_clean();
}
#endif

void child_rows(std::size_t population) {
    scenario = "X25 child snapshot";
    ScratchFixture scratch;
    Adapter adapter;
    scratch.bind(adapter);
    auto& placements = adapter.*access(Placements{});
    auto& live = adapter.*access(Live{});
    std::vector<Handle> expected;
    for (std::size_t i = 0; i < population; ++i) {
        const auto selected = handle(3 * (population - i));
        source::PlacementSnapshot row;
        row.source_id = "exit";
        row.per_entry_origin = 10 + i % 3;
        placements.try_emplace(selected.incarnation, row);
        live.push_back(selected);
        expected.push_back(selected);
    }
    source::PlacementSnapshot unrelated;
    unrelated.source_id = "other";
    unrelated.per_entry_origin = 10;
    placements.try_emplace(1000001, unrelated);
    live.push_back(handle(1000001));
    live.push_back(handle(1000002)); // absent placement is not selected
    {
        Measure measure;
        {
            const auto selected = (adapter.*access(Children{}))("exit", 0);
            CHECK(selected.size() == expected.size());
            std::size_t index = 0;
            for (const auto& row : selected) CHECK(row == expected[index++]);
        }
    }
    scratch.population(population);
    {
        const auto selected = (adapter.*access(Children{}))("exit", 0);
        // Mutation can allocate the placement owner's erased-row metadata;
        // that persistent sibling state is outside the scratch measurement.
        placements.erase(expected.front().incarnation);
        CHECK(selected.front() == expected.front());
    }
}

void paired_closes(std::size_t population) {
    scenario = "X26 paired-close snapshot";
    ScratchFixture scratch;
    EmptyHost host;
    Adapter adapter(host);
    scratch.bind(adapter);
    const auto applied = event();
    auto& placements = adapter.*access(Placements{});
    auto& live = adapter.*access(Live{});
    source::PlacementSnapshot opening;
    opening.family = Family::Entry;
    opening.opening = true;
    opening.source_id = "parent";
    placements.try_emplace(applied.handle().incarnation, opening);
    live.push_back(applied.handle());
    for (std::size_t i = 0; i < population; ++i) {
        source::PlacementSnapshot close;
        close.family = Family::Close;
        close.paired_reversal_parent = applied.handle();
        placements.try_emplace(i + 1, close);
        live.push_back(handle(i + 1));
    }
    CHECK(live.size() == population + 1);
    {
        Measure measure;
        // Outside a native command callback, cancel rejects before changing
        // the book. The real paired-close selection has already completed.
        // This isolates snapshot bytes from retained command-event storage.
        bool command_boundary = false;
        try {
            adapter.on_applied(applied, {});
#if PF_HAS_EXIT_SCRATCH
        } catch (const source::detail::ExitScratchError&) {
            CHECK(false);
#endif
        } catch (const std::exception&) {
            command_boundary = true;
        }
        CHECK(command_boundary);
    }
    scratch.population(population);
    CHECK(live.size() == population + 1);
    CHECK(host.physical_position().signed_units == 0.0);
    CHECK(host.physical_position().lot_count == 0);
    for (std::size_t i = 0; i < population; ++i)
        CHECK(placements.at(i + 1).paired_reversal_parent == applied.handle());
}

void flat_owners(std::size_t population, std::size_t width = 0) {
    scenario = width ? "X27 flat owner page readers" : "X27 flat owner snapshot";
    ScratchFixture scratch;
    EmptyHost host;
    Adapter adapter(host);
    scratch.bind(adapter);
    auto& placements = adapter.*access(Placements{});
    auto& live = adapter.*access(Live{});
    std::vector<std::string> owners;
    for (std::size_t i = 0; i < population; ++i) {
        owners.push_back(std::string(width, 'x') + "owner-" + std::to_string(population - i));
        source::PlacementSnapshot row;
        row.family = Family::ExitLimit;
        row.source_id = "exit";
        row.from_entry = owners.back();
        row.pending_bound_exit = true; // this flat preserves pending owners
        placements.try_emplace(i + 1, row);
        live.push_back(handle(i + 1));
    }
    const auto applied = event();
    {
        Measure measure;
        adapter.on_applied(applied, {});
    }
    scratch.population(population);
    CHECK(live.size() == population);
    for (std::size_t i = 0; i < population; ++i) {
        const auto& row = placements.at(i + 1);
        CHECK(row.from_entry == owners[i]);
        CHECK(row.pending_bound_exit);
    }
}

void ended_sources(std::size_t population) {
    scenario = "X27 ended-source snapshot";
    ScratchFixture scratch;
    EmptyHost host;
    Adapter adapter(host);
    scratch.bind(adapter);
    auto& cohorts = adapter.*access(Cohorts{});
    auto& placements = adapter.*access(Placements{});
    auto& live = adapter.*access(Live{});
    for (std::size_t i = 0; i < population; ++i) {
        const auto row = cohorts.emplace(
            "owner-" + std::to_string(i), decltype(cohorts.begin()->second){}).first;
        auto& cohort = row->second;
        cohort.opened.push_back(handle(i + 1));
        ++(adapter.*access(CohortSlots{}));
        cohorts.refresh_held(row);
        cohort.live_units_by_origin[i + 1] = 1.0;
        cohorts.refresh_held(row);
        source::PlacementSnapshot pending;
        pending.family = Family::ExitLimit;
        pending.source_id = "exit";
        pending.from_entry = "pending-" + std::to_string(i);
        pending.pending_bound_exit = true;
        placements.try_emplace(i + 1, pending);
        live.push_back(handle(i + 1));
    }
    CHECK(cohorts.size() == population);
    const auto applied = event();
    {
        Measure measure;
        adapter.on_applied(applied, {});
    }
    scratch.population(population);
    CHECK(live.size() == population);
    for (const auto& cohort : cohorts) {
        CHECK(cohort.second.opened.empty());
        CHECK(cohort.second.live_units_by_origin.empty());
    }
}

#if PF_HAS_EXIT_SCRATCH
struct ProductionScratchIo {
    std::uint64_t page_writes = 0;
    bool disk_only = true;
    pineforge::detail::ScratchIoObserver previous;

    ProductionScratchIo()
        : previous(source::detail::set_command_scratch_io_observer({
            this, [](void* opaque, const pineforge::detail::ScratchIoCharge& charge) noexcept {
                auto& io = *static_cast<ProductionScratchIo*>(opaque);
                io.disk_only = io.disk_only
                    && charge.kind == pineforge::detail::ScratchBackingKind::disk;
                if (charge.operation == pineforge::detail::ScratchIoOperation::write_page)
                    ++io.page_writes;
            }})) {}
    ~ProductionScratchIo() { source::detail::set_command_scratch_io_observer(previous); }
};

void production_begin_binding(std::size_t population) {
    scenario = "X25-X27 projected run store and lifecycle";
    ProductionScratchIo io;
    EmptyHost host;
    Adapter adapter(host);
    source::StagedConfiguration staged;
    staged.syminfo.mintick = 0.25;
    staged.syminfo.pointvalue = 1.0;
    const auto spec = adapter.project(source::PineStrategyConfig{}, staged, NativeBeginArgs{});
    auto& context = source::detail::exit_scratch(adapter);
    CHECK(context.run == spec.identity);
    CHECK(context.store.status() == pineforge::detail::ScratchStatus::ok);
    CHECK(context.store.record_count() == 0);
    CHECK(context.next_scope == 0);
    const auto resident = context.store.stats().resident_bytes;
    CHECK(resident == ScratchFixture::fixed_resident_bytes);
    std::weak_ptr<source::detail::ExitScratchContext> owner =
        adapter.*access(ScratchOwner{});
    adapter.bind_exit_scratch(context);
    CHECK(!owner.expired());
    CHECK(&source::detail::exit_scratch(adapter) == &context);
    std::size_t selected_rows = 0;
    context.observation = &selected_rows;
    context.observe_rows = [](void* opaque, std::uint64_t, std::size_t count) noexcept {
        auto& peak = *static_cast<std::size_t*>(opaque);
        peak = std::max(peak, count);
    };
    auto& placements = adapter.*access(Placements{});
    auto& live = adapter.*access(Live{});
    for (std::size_t i = 0; i < population; ++i) {
        source::PlacementSnapshot row;
        row.source_id = "exit";
        row.per_entry_origin = i + 1;
        const auto incarnation = population - i;
        placements.try_emplace(incarnation, row);
        live.push_back({spec.identity, incarnation});
    }
    const auto writes_before = context.store.stats().page_writes;
    const auto charged_writes_before = io.page_writes;
    {
        Measure measure;
        const auto selected = (adapter.*access(Children{}))("exit", 0);
        CHECK(selected.size() == population);
        for (std::size_t i = 0; i < population; ++i) {
            CHECK(selected[i].run == spec.identity);
            CHECK(selected[i].incarnation == population - i);
        }
        source::detail::ExitScratchRows<std::uint64_t> nested(context);
        nested.push_back(population);
        CHECK(nested.front() == population);
        CHECK(selected.front().incarnation == population);
    }
    CHECK(selected_rows == population);
    CHECK(context.store.record_count() == 0);
    CHECK(context.store.stats().backing_pages > 2);
    CHECK(context.store.stats().resident_bytes == resident);
    CHECK(io.page_writes > charged_writes_before);
    CHECK(io.page_writes - charged_writes_before
        == context.store.stats().page_writes - writes_before);
    CHECK(io.disk_only);
    std::printf("%s: rows=%zu resident=%llu backing-pages=%llu\n", scenario, population,
        static_cast<unsigned long long>(resident),
        static_cast<unsigned long long>(context.store.stats().backing_pages));
    adapter.reset_for_run();
    CHECK(owner.expired());
    bool cleared = false;
    try {
        (void)source::detail::exit_scratch(adapter);
    } catch (const source::detail::ExitScratchError& error) {
        CHECK(error.status() == pineforge::detail::ScratchStatus::programming_error);
        cleared = true;
    }
    CHECK(cleared);
    const auto next = adapter.project(source::PineStrategyConfig{}, staged, NativeBeginArgs{});
    auto& restarted = source::detail::exit_scratch(adapter);
    CHECK(restarted.run == next.identity);
    CHECK(restarted.run != spec.identity);
    CHECK(restarted.store.record_count() == 0);
    CHECK(restarted.next_scope == 0);
    CHECK(restarted.store.stats().resident_bytes == resident);
    owner = adapter.*access(ScratchOwner{});
    adapter.bind(host);
    CHECK(owner.expired());
}

void binding_and_status() {
    scenario = "production scratch binding and errors";
    ScratchFixture scratch;
    Adapter bound;
    scratch.bind(bound);
    Adapter unbound;
    bool missing = false;
    try {
        (void)source::detail::exit_scratch(unbound);
    } catch (const source::detail::ExitScratchError& error) {
        CHECK(error.status() == pineforge::detail::ScratchStatus::programming_error);
        missing = true;
    }
    CHECK(missing);
    CHECK(pineforge::detail::ScratchStatus::ok == scratch.store.begin());
    bool active = false;
    try {
        unbound.bind_exit_scratch(scratch.context);
    } catch (const source::detail::ExitScratchError& error) {
        CHECK(error.status() == pineforge::detail::ScratchStatus::programming_error);
        active = true;
    }
    CHECK(active);
    CHECK(pineforge::detail::ScratchStatus::ok == scratch.store.rollback());
    scratch.bind(unbound);
    for (const auto status : {pineforge::detail::ScratchStatus::io_error,
                              pineforge::detail::ScratchStatus::corrupt}) {
        bool reported = false;
        try {
            source::detail::exit_scratch_check(status);
        } catch (const source::detail::ExitScratchError& error) {
            CHECK(error.status() == status);
            reported = true;
        }
        CHECK(reported);
    }
}

void indexed_queries(std::size_t population) {
    scenario = "X25/X27 ordered family and sibling indexes";
    ScratchFixture scratch;
    Adapter adapter;
    scratch.bind(adapter);
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < population; ++i)
        keys.push_back(std::string(1537, 'x') + std::to_string(population - i));
    const std::string absent = keys.front() + "missing";
    {
        Measure measure;
        source::detail::ExitScratchRows<std::uint64_t> rows(adapter);
        source::detail::ExitScratchIndex families(scratch.context);
        source::detail::ExitScratchIndex text(scratch.context);
        source::detail::ExitScratchIndex members(scratch.context);
        for (std::size_t i = 0; i < population; ++i) {
            rows.push_back(i);
            families.put(i + 1, i);
            text.put(keys[i], i);
            members.put(i % 7, i, i);
        }
        CHECK(scratch.store.record_count() == 4 * population);
        for (std::size_t i = 0; i < population; ++i) {
            CHECK(scratch.query([&] { return families.find(i + 1); }) == i);
            CHECK(scratch.query([&] { return text.find(keys[i]); }, keys[i].size()) == i);
        }
        CHECK(scratch.query([&] { return families.find(population + 1); })
            == source::detail::ExitScratchIndex::absent);
        CHECK(scratch.query([&] { return text.find(absent); }, absent.size())
            == source::detail::ExitScratchIndex::absent);
        families.put(population, 1000000);
        CHECK(scratch.query([&] { return families.find(population); }) == 1000000);
        CHECK(scratch.store.record_count() == 4 * population);
        for (std::uint64_t family = 0; family < 7; ++family) {
            std::uint64_t next = family;
            std::size_t visited = 0;
            members.each(family, [&](std::uint64_t ordinal) {
                CHECK(ordinal == next);
                CHECK(rows[ordinal] == ordinal);
                next += 7;
                ++visited;
                // A cancellation/replacement may create and destroy another
                // scope; this ordered reader must keep its exact membership.
                source::detail::ExitScratchRows<Handle> nested(adapter);
                nested.push_back(handle(ordinal + 1));
                CHECK(nested.front() == handle(ordinal + 1));
                return true;
            });
            CHECK(visited == (population + 6 - family) / 7);
        }
    }
    CHECK(scratch.lookup_calls == 2 * population + 3);
    scratch.population(population);
}

void family_removal(std::size_t population) {
    scenario = "X25 family removal order and digest";
    ScratchFixture scratch;
    Adapter adapter;
    scratch.bind(adapter);
    for (const bool parked : {false, true}) {
        source::BracketRoster actual;
        source::BracketRoster oracle;
        std::vector<Handle> doomed;
        for (std::uint64_t i = 1; i <= population; ++i) {
            actual.push_back(handle(i));
            oracle.push_back(handle(i));
            if (i % 5 == 0) {
                actual.push_back(handle(i));
                oracle.push_back(handle(i));
            }
            if (!parked || (i != 1 && i % 7 != 0)) {
                doomed.push_back(handle(i));
                if (i % 5 == 0) doomed.push_back(handle(i));
            }
        }
        auto foreign = handle(4);
        foreign.run.run_number = 2;
        actual.push_back(foreign);
        oracle.push_back(foreign);
        if (parked) {
            const auto erased = [](std::uint64_t incarnation) {
                return incarnation == 1 || incarnation % 7 == 0;
            };
            actual.settle(erased);
            oracle.settle(erased);
        }
        auto expected = actual.members();
        expected.erase(std::remove_if(expected.begin(), expected.end(),
            [&](const Handle& value) {
                return std::find(doomed.begin(), doomed.end(), value) != doomed.end();
            }), expected.end());
        oracle.remove_all(doomed);
        const auto remove = [&] {
            source::detail::ExitScratchRows<Handle> selected(adapter);
            for (const auto& value : doomed) selected.push_back(value);
            actual.remove_selected(selected);
        };
        if (parked) {
            // Joining the roster's retained parked bytes is existing durable
            // owner storage, measured by its own row rather than X25 scratch.
            remove();
        } else {
            Measure measure;
            remove();
        }
        CHECK(actual.members() == expected);
        CHECK(actual.members() == oracle.members());
        CHECK(actual.settled_count() == oracle.settled_count());
        CHECK(actual.settled_digest() == oracle.settled_digest());
        CHECK(actual.holds(foreign));
        for (std::uint64_t i = 1; i <= population; ++i)
            CHECK(actual.count(i) == oracle.count(i));
    }
    scratch.population(population);
}

void paged_text_and_cancellation(std::size_t population) {
    scenario = "X26/X27 frozen text lookup and cancellation";
    ScratchFixture scratch;
    Adapter adapter;
    scratch.bind(adapter);
    auto& pending = adapter.*access(Relative{});
    std::vector<std::string> input;
    for (std::size_t i = 0; i < population; ++i) {
        std::string key(1537, 'x');
        key[128] = '\0';
        key[1024] = static_cast<char>(0x80 + i % 64);
        key += std::to_string(population - i);
        input.push_back(key);
        pending.emplace_back();
        pending.back().from_entry = key;
    }
    auto expected = input;
    std::stable_sort(expected.begin(), expected.end());
    const auto missing = input.front() + "missing";
    {
        Measure measure;
        source::detail::ExitScratchRows<std::string> frozen(adapter);
        for (const auto& key : input) frozen.push_back(key);
        frozen.push_back(input.front()); // exact duplicate sorts once below
        for (const auto& key : input)
            CHECK(scratch.query([&] { return frozen.contains(key); }, key.size()));
        CHECK(!scratch.query([&] { return frozen.contains(missing); }, missing.size()));
        std::size_t visited = 0;
        frozen.ordered_text([&](const source::detail::ExitScratchText& key) {
            CHECK(key == expected[visited]);
            const auto before = pending.size();
            (adapter.*access(CancelText{}))(key);
            CHECK(pending.size() + 1 == before);
            CHECK(key == expected[visited]); // its source row was erased
            source::detail::ExitScratchRows<std::uint64_t> nested(adapter);
            nested.push_back(visited);
            CHECK(key == expected[visited]); // after page eviction/mutation
            ++visited;
            return true;
        }, true);
        CHECK(visited == population);
        CHECK(pending.empty());
        CHECK(frozen.front() == input.front());
    }
    scratch.population(population);
}

void stable_external_order(std::size_t population) {
    scenario = "shared selection order and nested snapshot";
    ScratchFixture scratch;
    struct Row { std::uint64_t rank; std::uint64_t origin; };
    std::vector<Row> expected;
    for (std::uint64_t i = 0; i < population; ++i)
        expected.push_back({i % 7, population - i});
    const auto rank = [](const Row& a, const Row& b) { return a.rank < b.rank; };
    std::stable_sort(expected.begin(), expected.end(), rank);
    {
        Measure measure;
        {
            source::detail::ExitScratchRows<Row> rows(scratch.context);
            for (std::uint64_t i = 0; i < population; ++i)
                rows.push_back({i % 7, population - i});
            std::size_t position = 0;
            std::size_t key_reads = 0;
            rows.ordered([&](const Row& row) { ++key_reads; return row.rank; },
                [&](const Row& row, std::size_t) {
                CHECK(row.rank == expected[position].rank);
                CHECK(row.origin == expected[position].origin);
                ++position;
                source::detail::ExitScratchRows<std::uint64_t> nested(scratch.context);
                nested.push_back(row.origin);
                CHECK(nested.front() == row.origin);
                CHECK(rows.size() == expected.size());
                return true;
            });
            CHECK(position == expected.size());
            CHECK(key_reads == population);
        }
    }
    scratch.population(population);
}
#endif
} // namespace

int main(int argc, char** argv) {
    const std::string row = argc == 1 ? "all" : argv[1];
    if (row != "all" && row != "x25" && row != "x26" && row != "x27") {
        std::fprintf(stderr, "expected all, x25, x26, or x27\n");
        return 2;
    }
#if PF_HAS_EXIT_SCRATCH
    if (row == "all") {
        construction_publications();
        batch_visibility_boundaries();
        observed_construction_publications();
        partial_batch_errors();
        unfinished_batch_cleanup();
        partial_batch_boundary();
    }
#endif
    for (const auto population : {17U, 129U, 513U}) {
        if (row == "all" || row == "x25") child_rows(population);
        if (row == "all" || row == "x26") paired_closes(population);
        if (row == "all" || row == "x27") {
            flat_owners(population);
            flat_owners(population, 4096);
            ended_sources(population);
        }
#if PF_HAS_EXIT_SCRATCH
        production_begin_binding(population);
        if (row == "all" || row == "x25") {
            stable_external_order(population);
            family_removal(population);
        }
        if (row == "all" || row == "x25" || row == "x27") indexed_queries(population);
        if (row == "all" || row == "x26" || row == "x27")
            paged_text_and_cancellation(population);
#endif
    }
#if PF_HAS_EXIT_SCRATCH
    binding_and_status();
#endif
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
