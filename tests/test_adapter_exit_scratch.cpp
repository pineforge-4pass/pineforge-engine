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
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
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
#if PF_HAS_EXIT_SCRATCH
struct CancelText { friend auto access(CancelText); };
struct Relative { friend auto access(Relative); };
struct ScratchOwner { friend auto access(ScratchOwner); };
template struct Access<CancelText, static_cast<void (Adapter::*)(
    const source::detail::ScratchByteSource&)>(&Adapter::cancel_exit_orders_for_full_close)>;
template struct Access<Relative, &Adapter::pending_relative_exits_>;
template struct Access<ScratchOwner, &Adapter::exit_scratch_>;
#endif
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<Children, &Adapter::per_entry_child_legs>;
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

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
        auto& cohort = cohorts.emplace(
            "owner-" + std::to_string(i), decltype(cohorts.begin()->second){}).first->second;
        cohort.opened.push_back(handle(i + 1));
        cohort.live_units_by_origin[i + 1] = 1.0;
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
