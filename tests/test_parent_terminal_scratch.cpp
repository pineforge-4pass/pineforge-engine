// X08: the actual waiting population and every descendant are drained in the
// base queue's order, while the drain's additional resident bytes stay fixed.
// The production exercise also compiles on 21ba62bd; there the byte assertion
// fails because Item rows survive in the queue. Only the store audit is gated.
#include "../src/native_execution_consumer.hpp"
#include "native_order_full_fold.hpp"
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    using Seeds = std::vector<std::pair<native_order::EventId, native_order::RequestHandle>>;

    static native_order::WorkingRequestCore& core(NativeExecutionConsumer& c) {
        return c.requests_;
    }
    static bool failed(const NativeExecutionConsumer& c) noexcept { return c.failed(); }
    static void note_setup(NativeExecutionConsumer& c, std::size_t first = 0) {
        c.note_install(native_order::EventRange{first, c.requests_.history_end() - first});
    }
    static void drain(NativeExecutionConsumer& c, BacktestEngine& engine, const Seeds& seeds) {
        c.drain_dependency_queue(engine, seeds, NativeFailureOperation::Mutation);
    }
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    static auto stats(const NativeExecutionConsumer& c) { return c.dependency_scratch_stats_; }
#endif

    // Frozen 21ba62bd algorithm, used only as an ordering/hash oracle.
    static void reference(NativeExecutionConsumer& c, BacktestEngine& engine,
                          const Seeds& seeds) {
        struct Item {
            std::uint64_t cause_ordinal;
            native_order::EventId cause;
            native_order::RequestHandle child;
            std::uint64_t child_priority;
        };
        std::vector<Item> queue;
        const auto less = [](const Item& a, const Item& b) {
            if (a.cause_ordinal != b.cause_ordinal) return a.cause_ordinal < b.cause_ordinal;
            return a.child_priority < b.child_priority;
        };
        const auto enqueue = [&](const native_order::EventId& cause,
                                 const native_order::RequestHandle& parent, std::size_t first) {
            auto children = c.requests_.waiting_children(parent);
            for (auto& child : children) {
                const auto* live = c.requests_.find_live(child);
                const auto priority = live ? live->priority() : child.incarnation;
                queue.push_back({cause.ordinal, cause, std::move(child), priority});
            }
            if (first < queue.size())
                std::sort(queue.begin() + static_cast<std::ptrdiff_t>(first), queue.end(), less);
        };
        for (const auto& seed : seeds) enqueue(seed.first, seed.second, queue.size());
        std::sort(queue.begin(), queue.end(), less);
        for (std::size_t index = 0; index < queue.size() && !c.failed(); ++index) {
            const auto cause = queue[index].cause;
            const auto child = queue[index].child;
            auto step = c.direct_mutation_
                ? c.core_step(c.requests_.apply_parent_terminal(cause, child,
                                                                c.next_timeline_ordinal_))
                : c.core_step(c.requests_.prepare_parent_terminal(cause, child,
                                                                  c.next_timeline_ordinal_));
            if (const auto* error = std::get_if<native_order::PreparationError>(&step)) {
                c.fail_preparation(engine, *error, NativeFailureOperation::Mutation);
                return;
            }
            if (std::holds_alternative<native_order::NoChange>(step)) continue;
            if (!c.settle_step(engine, std::move(step), NativeFailureOperation::Mutation,
                               cause.ordinal)) return;
            if (!c.requests_.find_live(child) && c.requests_.last_ordinal() != 0) {
                auto next = cause;
                next.ordinal = c.requests_.last_ordinal();
                enqueue(next, child, index + 1);
            }
        }
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

namespace {
using namespace pineforge;
namespace no = native_order;
using Probe = NativeExecutionConsumerProbe;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);  \
    }                                                                          \
} while (false)

#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
struct ScratchFixture {
    using Status = detail::ScratchStatus;
    static constexpr std::size_t page_size = 512;
    std::vector<std::byte> storage = std::vector<std::byte>(64u * 1024u * 1024u);
    detail::ScratchKeyOrder order = NativeExecutionConsumer::dependency_scratch_order();
    std::uint64_t comparisons = 0, reads = 0, writes = 0, syncs = 0;
    static Status compare(void* context, const detail::ScratchByteSource& a,
                          const detail::ScratchByteSource& b, int& result) noexcept {
        auto& self = *static_cast<ScratchFixture*>(context);
        ++self.comparisons;
        return self.order.compare(self.order.context, a, b, result);
    }
    static void charge(void* context, const detail::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<ScratchFixture*>(context);
        CHECK(io.kind == detail::ScratchBackingKind::test_memory);
        if (io.operation == detail::ScratchIoOperation::read_page) ++self.reads;
        if (io.operation == detail::ScratchIoOperation::write_page) ++self.writes;
        if (io.operation == detail::ScratchIoOperation::sync) ++self.syncs;
    }
    detail::FixedMemoryScratchBacking backing{
        {storage.data(), storage.size()}, page_size, {this, charge}};
    detail::CommandScratchStore store{
        {2, page_size, 1, 1, {order.id, this, compare}, detail::ScratchOpenMode::create_empty},
        &backing};
    const std::uint64_t resident = store.stats().resident_bytes;

    std::uint64_t digest() {
        std::array<std::byte, page_size> page{};
        std::uint64_t result = 0;
        CHECK(store.state_digest({page.data(), page.size()}, result) == Status::ok);
        return result;
    }
    void check_empty() {
        CHECK(store.status() == Status::ok);
        CHECK(!store.transaction_active());
        CHECK(store.record_count() == 0);
        const auto stats = store.stats();
        CHECK(stats.resident_bytes == resident && resident > 0);
        static std::uint64_t fixed_resident = 0;
        if (!fixed_resident) fixed_resident = resident;
        CHECK(resident == fixed_resident);
        CHECK(stats.page_reads == reads && stats.page_writes == writes && stats.syncs == syncs);
    }
    void check_queries(std::size_t population) {
        CHECK(store.begin() == Status::ok);
        using Key = std::array<std::uint64_t, 4>;
        detail::ScratchSpanSource empty(detail::ScratchBytes{});
        const auto refused_before = global_allocation::refused;
        global_allocation::refusing = true;
        for (std::size_t index = 0; index < population; ++index) {
            const Key key{0, index * 2, 0, 0};
            detail::ScratchSpanSource source(
                {reinterpret_cast<const std::byte*>(key.data()), sizeof key});
            detail::ScratchHandle handle = detail::scratch_no_handle;
            CHECK(store.put(source, empty, handle) == Status::ok);
        }
        CHECK(store.record_count() == population);
        CHECK(store.stats().backing_pages > 2);
        std::size_t levels = 0;
        for (auto count = population; count; count >>= 1) ++levels;
        std::uint64_t maximum = 0;
        for (std::size_t index = 0; index < population; ++index) {
            const auto probe_index = (index * 73) % population;
            const Key key{0, probe_index * 2 + 1, 0, 0};
            detail::ScratchSpanSource source(
                {reinterpret_cast<const std::byte*>(key.data()), sizeof key});
            detail::ScratchHandle handle = detail::scratch_no_handle;
            const auto before = comparisons;
            const auto status = store.lower_bound(source, handle);
            CHECK(status == (probe_index + 1 == population ? Status::end : Status::ok));
            maximum = std::max(maximum, comparisons - before);
            if (status == Status::ok) {
                Key actual{};
                CHECK(store.read(handle, detail::ScratchField::key, 0,
                    {reinterpret_cast<std::byte*>(actual.data()), sizeof actual}) == Status::ok);
                CHECK((actual == Key{0, (probe_index + 1) * 2, 0, 0}));
            } else CHECK(handle == detail::scratch_no_handle);
        }
        CHECK(maximum > 0 && maximum <= 8 * (levels + 1));
        CHECK(store.rollback() == Status::ok);
        global_allocation::refusing = false;
        CHECK(global_allocation::refused == refused_before);
        check_empty();
    }
};
#endif

std::size_t peak_bytes = 0;
void record_bytes(std::size_t) {
    peak_bytes = std::max(peak_bytes, global_allocation::live_bytes);
}

std::size_t reserve_bytes(no::WorkingRequestCore& core, std::size_t expected_events) {
    struct RestoreRecorder {
        const bool recording = global_allocation::recording;
        const std::size_t live = global_allocation::live_bytes;
        const std::size_t peak = peak_bytes;
        void (*const callback)(std::size_t) = global_allocation::on_recorded;
        ~RestoreRecorder() {
            global_allocation::recording = recording;
            global_allocation::live_bytes = live;
            global_allocation::on_recorded = callback;
            peak_bytes = peak;
        }
    } restore;
    const auto before = global_allocation::live_bytes;
    peak_bytes = before;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    core.reserve(expected_events);
    return peak_bytes - before;
}

struct Host final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
};

enum class Shape { Wide, Deep, Tree, ManySeeds };

// Mirrors JournalStore's private constants; the event value itself is public.
constexpr std::size_t kJournalMaxResident = 16384;
constexpr std::size_t kJournalChunk = 32;
constexpr std::size_t kJournalSpillPageBytes = 32768;
// src/native_order.cpp:1582: std::array<Cached, 4>; :1567 has four read buffers.
// exercise_reload_control counts the decoded storage slots through history_at.
constexpr std::size_t kJournalCacheSlots = 4;
constexpr std::size_t kDrainBytes = 4096;

void print_reserve(std::size_t population, Shape shape, bool direct, std::size_t reserve_peak) {
    std::printf("X08 reserve population=%zu shape=%d direct=%d reserve_peak=%zu "
                "event_bytes=%zu slot_bytes=%zu cap_slot_bytes=%zu chunk_pointer_bytes=%zu\n",
                population, static_cast<int>(shape), direct, reserve_peak,
                sizeof(no::CommandEvent), sizeof(std::optional<no::CommandEvent>),
                kJournalMaxResident * sizeof(std::optional<no::CommandEvent>),
                (kJournalMaxResident / kJournalChunk + 4) * sizeof(std::unique_ptr<std::byte>));
}

Probe::Seeds populate(NativeExecutionConsumer& consumer, std::size_t population, Shape shape,
                      std::size_t* reserve_peak = nullptr) {
    auto& core = Probe::core(consumer);
    core.reset(no::RunIdentity{"x08-scratch", 1});
    const auto root_count = shape == Shape::ManySeeds ? population : 2;
    // Retained command output is not queue scratch. Allocate it before the
    // measured drain so the census charges only the drain and one mutation.
    const auto expected_events = 2 * population + 2 * root_count + 64;
    if (reserve_peak) *reserve_peak = reserve_bytes(core, expected_events);
    else core.reserve(expected_events);
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;
    std::vector<no::RequestHandle> roots;
    for (std::size_t index = 0; index < root_count; ++index) {
        const auto accepted = core.submit(no::Request{no::Transact{1.0}, "root", ""},
                                          0, incarnation, ordinal);
        CHECK(accepted.status == no::SubmitStatus::Accepted);
        if (!accepted.handle) return {};
        roots.push_back(*accepted.handle);
    }
    std::vector<no::RequestHandle> children;
    for (std::size_t index = 0; index < population; ++index) {
        const auto parent = index < 2 || shape == Shape::Wide || shape == Shape::ManySeeds
            ? roots[index % root_count]
            : children[shape == Shape::Deep ? index - 2 : (index - 2) / 2];
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "child", ""};
        no::WaitForApplied wait;
        wait.parent = parent;
        request.owner = wait;
        const auto accepted = core.submit(request, 0, incarnation, ordinal);
        CHECK(accepted.status == no::SubmitStatus::Accepted);
        if (!accepted.handle) return {};
        children.push_back(*accepted.handle);
    }
    CHECK(core.live().size() == population + roots.size());
    std::size_t waiting = 0;
    for (const auto& live : core.live())
        if (std::holds_alternative<no::Wait>(live.authority)) ++waiting;
    CHECK(waiting == population);

    // Priority must be the enqueued book priority, not handle incarnation.
    no::Request repriced = core.find_live(children.front())->request();
    no::ReplaceOptions options;
    options.keep_handle = true;
    const auto replaced = core.replace(children.front(), repriced, 0, incarnation, ordinal,
                                        std::nullopt, options);
    CHECK(replaced.status == no::ReplaceStatus::Replaced);
    CHECK(core.find_live(children.front())->priority() > children.back().incarnation);

    Probe::Seeds causes;
    for (const auto& root : roots) {
        const auto cancelled = core.cancel(root, ordinal);
        CHECK(cancelled.status == no::CancelStatus::Cancelled);
        causes.push_back({no::EventId{core.identity(), cancelled.event_ordinal}, root});
    }
    CHECK(core.live().size() == population);
    Probe::note_setup(consumer);
    // Unsorted causes, a duplicate, and a real NoTransition before the right
    // parent cause exercise the same membership as the original queue.
    if (shape == Shape::ManySeeds) {
        const auto first = causes.front();
        std::reverse(causes.begin(), causes.end());
        causes.push_back({first.first, roots[1]});
        causes.push_back(first);
        CHECK(causes.size() == population + 2);
        return causes;
    }
    return {causes[1], causes[0], {causes[0].first, roots[1]}, causes[0]};
}

void exercise(std::size_t population, Shape shape, bool direct) {
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    ScratchFixture scratch;
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    scratch.check_queries(population);
    const auto scratch_digest = scratch.digest();
#endif
    Host actual;
    Host expected;
    auto& consumer = actual.consumer();
    auto& oracle = expected.consumer();
    consumer.set_direct_mutation(direct);
    oracle.set_direct_mutation(direct);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    consumer.set_dependency_scratch(scratch.store);
#endif
    std::size_t reserve_peak = 0;
    const auto seeds = populate(consumer, population, shape, &reserve_peak);
    print_reserve(population, shape, direct, reserve_peak);
    const auto reference_seeds = populate(oracle, population, shape);
    CHECK(!seeds.empty());
    CHECK(!reference_seeds.empty());
    if (seeds.empty() || reference_seeds.empty()) return;
    const auto before = consumer.request_core().history_end();
    Probe::reference(oracle, expected, reference_seeds);

#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    const auto comparisons_before = scratch.comparisons;
    const auto writes_before = scratch.writes;
#endif
    peak_bytes = 0;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    Probe::drain(consumer, actual, seeds);
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;

#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(scratch.comparisons > comparisons_before && scratch.writes > writes_before);
    const auto census = Probe::stats(consumer);
    CHECK(census.book_rows == population && census.waiting_rows == population);
    CHECK(census.seed_rows == seeds.size());
    CHECK(census.popped_rows >= population && census.popped_rows == census.queued_rows);
    CHECK(census.edge_rows >= population);
    CHECK(census.edge_rows <= 3 * population + seeds.size());
    CHECK(census.lookups <= census.edge_rows + census.queued_rows + population + seeds.size() + 1);
    std::size_t levels = 0;
    for (auto count = population + census.queued_rows; count; count >>= 1) ++levels;
    CHECK(census.max_lookup_reads <= 16 * (levels + 1));
    const auto operations = population + census.queued_rows + census.popped_rows + census.lookups;
    CHECK(scratch.comparisons - comparisons_before <= 8 * (levels + 1) * operations);
    std::printf("X08 indexed=%llu edges=%llu queued=%llu seeks=%llu max_seek_reads=%llu resident=%llu\n",
        static_cast<unsigned long long>(census.waiting_rows),
        static_cast<unsigned long long>(census.edge_rows),
        static_cast<unsigned long long>(census.queued_rows),
        static_cast<unsigned long long>(census.lookups),
        static_cast<unsigned long long>(census.max_lookup_reads),
        static_cast<unsigned long long>(scratch.resident));
#endif
    CHECK(!Probe::failed(consumer));
    CHECK(!Probe::failed(oracle));
    CHECK(consumer.request_core().live().empty());
    CHECK(consumer.request_core().history_end() - before == population);
    std::size_t drained = 0;
    for (std::size_t index = before; index < consumer.request_core().history_end(); ++index) {
        const auto* row = std::get_if<no::CancelledEvent>(
            &consumer.request_core().history_at(index));
        CHECK(row && row->reason == no::CancelReason::OwnerGone && row->cause);
        if (row) ++drained;
        full_fold::Fold a;
        full_fold::Fold b;
        full_fold::fold(a, consumer.request_core().history_at(index));
        full_fold::fold(b, oracle.request_core().history_at(index));
        CHECK(a.h == b.h);
    }
    CHECK(drained == population);
    CHECK(full_fold::core_value(consumer.request_core())
          == full_fold::core_value(oracle.request_core()));
    CHECK(consumer.continuation_hash() == oracle.continuation_hash());
    // No population-scaled allowance: this includes the peak live bytes of
    // the prepared mutation when direct mutation is disabled.
    CHECK(peak_bytes <= kDrainBytes);
    std::printf("X08 population=%zu shape=%d direct=%d peak=%zu bound=%zu\n",
                population, static_cast<int>(shape), direct, peak_bytes, kDrainBytes);
    const auto finished_hash = consumer.continuation_hash();
    const auto finished_history = consumer.request_core().history_end();
    Probe::drain(consumer, actual, seeds);
    CHECK(!Probe::failed(consumer));
    CHECK(consumer.continuation_hash() == finished_hash);
    CHECK(consumer.request_core().history_end() == finished_history);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(Probe::stats(consumer).book_rows == 0);
    CHECK(Probe::stats(consumer).queued_rows == 0);
#endif
}

std::size_t exercise_beyond_cap(std::size_t population, bool direct) {
    // Wide needs only two roots and tests the same journal capacity as Deep
    // and Tree. Avoid the oracle's per-child book scans at these populations.
    constexpr auto shape = Shape::Wide;
    constexpr std::size_t root_count = 2;
    CHECK(2 * population + 2 * root_count + 64 > kJournalMaxResident);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    ScratchFixture scratch;
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    const auto scratch_digest = scratch.digest();
#endif
    Host actual;
    auto& consumer = actual.consumer();
    consumer.set_direct_mutation(direct);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    consumer.set_dependency_scratch(scratch.store);
#endif
    std::size_t reserve_peak = 0;
    const auto seeds = populate(consumer, population, shape, &reserve_peak);
    print_reserve(population, shape, direct, reserve_peak);
    CHECK(reserve_peak >= kJournalMaxResident * sizeof(std::optional<no::CommandEvent>));
    CHECK(!seeds.empty());
    if (seeds.empty()) return reserve_peak;
    const auto before = consumer.request_core().history_end();
    // One acceptance per child/root, one re-price and two root cancellations.
    // Cross the actual retained-event limit too, rather than just reserving it.
    CHECK(before == population + root_count + 1 + root_count);
    CHECK(before + population > kJournalMaxResident);

    // Reserve's marked chunks are recycled through JournalStore::spare during
    // these one-event appends, not freed against this separate recording window.
    peak_bytes = 0;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    Probe::drain(consumer, actual, seeds);
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    CHECK(peak_bytes <= kDrainBytes);
    std::printf("X08 population=%zu shape=%d direct=%d peak=%zu bound=%zu\n",
                population, static_cast<int>(shape), direct, peak_bytes, kDrainBytes);

    CHECK(!Probe::failed(consumer));
    CHECK(consumer.request_core().live().empty());
    CHECK(consumer.request_core().history_end() - before == population);
    std::size_t drained = 0;
    for (std::size_t index = before; index < consumer.request_core().history_end(); ++index) {
        const auto* row = std::get_if<no::CancelledEvent>(
            &consumer.request_core().history_at(index));
        CHECK(row && row->reason == no::CancelReason::OwnerGone && row->cause);
        if (row) ++drained;
    }
    CHECK(drained == population);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    const auto census = Probe::stats(consumer);
    CHECK(census.book_rows == population && census.waiting_rows == population);
    CHECK(census.seed_rows == seeds.size());
    CHECK(census.popped_rows >= population && census.popped_rows == census.queued_rows);
    CHECK(census.edge_rows >= population);
    CHECK(census.edge_rows <= 3 * population + seeds.size());
    CHECK(census.lookups <= census.edge_rows + census.queued_rows + population + seeds.size() + 1);
    std::size_t levels = 0;
    for (auto count = population + census.queued_rows; count; count >>= 1) ++levels;
    CHECK(census.max_lookup_reads <= 16 * (levels + 1));
#endif
    const auto finished_hash = consumer.continuation_hash();
    const auto finished_history = consumer.request_core().history_end();
    Probe::drain(consumer, actual, seeds);
    CHECK(!Probe::failed(consumer));
    CHECK(consumer.continuation_hash() == finished_hash);
    CHECK(consumer.request_core().history_end() == finished_history);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(Probe::stats(consumer).book_rows == 0);
    CHECK(Probe::stats(consumer).queued_rows == 0);
#endif
    return reserve_peak;
}

void exercise_reload_control(bool direct, const no::Request& request, const no::Request& filler,
                             std::size_t reload_budget_bytes) {
    constexpr auto page_count = kJournalCacheSlots + 2;
    constexpr auto definition_count = page_count * kJournalChunk;
    no::WorkingRequestCore core{no::RunIdentity{"x08-reload-control", 1}};
    core.reset(no::RunIdentity{"x08-reload-control", 1});
    core.reserve(kJournalMaxResident + definition_count);
    std::uint64_t incarnation = 1, ordinal = 1;
    for (std::size_t index = 0; index < definition_count; ++index)
        CHECK(core.submit(request, 0, incarnation, ordinal).status == no::SubmitStatus::Accepted);
    CHECK(core.history_end() == definition_count);
    CHECK(core.live().size() == definition_count);
    std::array<std::uintptr_t, definition_count> resident_addresses{};
    std::array<std::uint64_t, definition_count> values{};
    for (std::size_t index = 0; index < definition_count; ++index) {
        const auto& event = core.history_at(index);
        resident_addresses[index] = reinterpret_cast<std::uintptr_t>(&event);
        values[index] = full_fold::value(event);
    }
    while (core.history_end() < kJournalMaxResident)
        CHECK(core.submit(filler, 0, incarnation, ordinal).status == no::SubmitStatus::Rejected);

    // The next append spills [0,32); every further 32 appends spills another
    // chunk (src/native_order.cpp:1679-1717). Do not read any spilled row until
    // recording: all six distinct pages must be cold. Below, verify that each
    // entire chunk moved into one contiguous decoded CommandEvent vector.
    std::array<std::size_t, page_count> page_positions{};
    for (std::size_t page = 0; page < page_count; ++page) {
        page_positions[page] = page * kJournalChunk;
        const auto spill_end = kJournalMaxResident + page_positions[page] + 1;
        while (core.history_end() < spill_end)
            CHECK(core.submit(filler, 0, incarnation, ordinal).status == no::SubmitStatus::Rejected);
        CHECK(core.history_end() == spill_end);
    }
    CHECK(core.history_base() == 0);
    CHECK(core.history_end() == kJournalMaxResident + (page_count - 1) * kJournalChunk + 1);
    std::array<std::uintptr_t, page_count> decoded_addresses{};
    std::size_t spilled_pages = 0, distinct_reloads = 0, observed_slots = 0;
    const auto allocations_before = global_allocation::allocations;
    peak_bytes = 0;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    for (std::size_t page = 0; page < page_count; ++page) {
        const auto first = page_positions[page];
        decoded_addresses[page] = reinterpret_cast<std::uintptr_t>(&core.history_at(first));
        bool full_spilled_page = true;
        for (std::size_t offset = 0; offset < kJournalChunk; ++offset) {
            const auto position = first + offset;
            const auto& event = core.history_at(position);
            const auto address = reinterpret_cast<std::uintptr_t>(&event);
            const bool moved = address != resident_addresses[position];
            const bool contiguous =
                address == decoded_addresses[page] + offset * sizeof(no::CommandEvent);
            const bool accepted = std::holds_alternative<no::AcceptedEvent>(event);
            const bool unchanged = full_fold::value(event) == values[position];
            CHECK(moved && contiguous && accepted && unchanged);
            full_spilled_page = full_spilled_page && moved && contiguous && accepted && unchanged;
        }
        if (full_spilled_page) {
            ++spilled_pages;
            const auto previous = page_positions.begin() + static_cast<std::ptrdiff_t>(page);
            if (std::find(page_positions.begin(), previous, first) == previous) ++distinct_reloads;
        }
        const auto previous = decoded_addresses.begin() + static_cast<std::ptrdiff_t>(page);
        if (std::find(decoded_addresses.begin(), previous, decoded_addresses[page]) == previous)
            ++observed_slots;
    }
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    const auto measured_reload_peak = peak_bytes;
    const auto allocations = global_allocation::allocations - allocations_before;
    CHECK(spilled_pages == page_count);
    CHECK(distinct_reloads == spilled_pages && distinct_reloads > kJournalCacheSlots);
    CHECK(observed_slots == kJournalCacheSlots);
    CHECK(allocations >= distinct_reloads * kJournalChunk);
    CHECK(measured_reload_peak >= kJournalCacheSlots * kJournalChunk * sizeof(no::RequestDefinition));
    CHECK(measured_reload_peak <= reload_budget_bytes);
    std::printf("X08 reload_control direct=%d spilled_pages=%zu distinct_reloads=%zu "
                "observed_slots=%zu measured_peak=%zu budget=%zu allocations=%zu\n",
                direct, spilled_pages, distinct_reloads, observed_slots,
                measured_reload_peak, reload_budget_bytes, allocations);
}

void exercise_reload(bool direct) {
    constexpr std::size_t population = 8191;
    const no::Request root_request{no::Transact{1.0}, "root", ""};
    const no::Request child_request{no::Reduce{no::ExplicitUnits{1.0}}, "child", ""};
    const no::Request filler{no::Transact{0.0}, "filler", ""};
    // Conservative make_shared control block plus rounding allowance: two
    // pointer widths and a size_t, rounded to max_align_t; no private STL type.
    // Definitions are allocated by src/native_order.cpp:1285, once per event.
    constexpr auto alignment = alignof(std::max_align_t);
    constexpr auto control_block_bytes =
        (2 * sizeof(void*) + sizeof(std::size_t) + alignment - 1) / alignment * alignment;
    constexpr auto decoded_events = kJournalCacheSlots * kJournalChunk;
    constexpr auto definition_bytes =
        decoded_events * (sizeof(no::RequestDefinition) + control_block_bytes);
    // archive_string (:1205-1215) resizes fresh strings. Bound both text fields
    // by these actual case strings' largest capacities, including terminators,
    // even where short-string storage is already included in sizeof(definition).
    const auto label_capacity = std::max({root_request.label.capacity(),
                                         child_request.label.capacity(), filler.label.capacity()});
    const auto comment_capacity = std::max({root_request.comment.capacity(),
                                           child_request.comment.capacity(), filler.comment.capacity()});
    const auto string_bytes =
        decoded_events * (label_capacity + 1 + comment_capacity + 1) * sizeof(std::string::value_type);
    // The first page has 31 Accepted and one Cancelled event, each with one
    // definition. Independent/WaitForApplied owners and BookTransaction/Wait
    // authority with PendingNone contain no opening handles or adjustment ids.
    // The control pages contain only Accepted/Independent events, also no ids.
    // archive_vector (src/native_order.cpp:1235-1246) therefore resizes no ids.
    constexpr std::size_t opening_ids_per_event = 0, adjustment_ids_per_event = 0;
    constexpr auto id_vector_bytes = decoded_events *
        (opening_ids_per_event * sizeof(no::RequestHandle)
         + adjustment_ids_per_event * sizeof(std::uint64_t));
    // prepare_spill (:1595-1602) reserves all event vectors and buffers and,
    // via ExtFile::prepare (:979-982), the first arena chunk/table entry BEFORE
    // recording. The drain can still grow the data extent: bound one existing
    // page plus a full fixed resident cap of appends (enforced below), each
    // chunk spilling at most kSpillPageBytes for these short, full-page rows.
    // The page directory stays within its prepared first arena chunk.
    constexpr auto max_spill_bytes =
        (1 + kJournalMaxResident / kJournalChunk) * kJournalSpillPageBytes;
    constexpr std::size_t arena_chunk_bytes = detail::ScratchArena::kChunkBytes;
    constexpr auto max_arena_chunks = (max_spill_bytes + arena_chunk_bytes - 1) / arena_chunk_bytes;
    // src/scratch_arena.cpp:295-301 doubles the uint32_t table capacity:
    // at most five growths, 1 -> 2 -> 4 -> 8 -> 16 -> 32 (17 chunks needed).
    // Sum every allocation, including replaced tables, to bound the live peak.
    constexpr auto arena_chunk_table_bytes = [] {
        std::size_t bytes = 0;
        for (std::size_t next = 2; next / 2 < max_arena_chunks; next *= 2)
            bytes += next * sizeof(std::uint32_t);
        return bytes;
    }();
    // Fixed production caps and fixed case payloads only: no population or
    // retained-window length contributes to this byte budget.
    const auto reload_budget_bytes =
        definition_bytes + string_bytes + id_vector_bytes + arena_chunk_table_bytes;
    CHECK(population < kJournalMaxResident);
    // A fifth full decoded page must exceed this SAME budget even counting
    // only its definitions, so growing the cache cannot hide in the allowances.
    CHECK(reload_budget_bytes <
          (kJournalCacheSlots + 1) * kJournalChunk * sizeof(no::RequestDefinition));
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    ScratchFixture scratch;
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    const auto scratch_digest = scratch.digest();
#endif
    Host actual;
    auto& consumer = actual.consumer();
    consumer.set_direct_mutation(direct);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    consumer.set_dependency_scratch(scratch.store);
#endif
    auto& core = Probe::core(consumer);
    core.reset(no::RunIdentity{"x08-reload", 1});
    core.reserve(kJournalMaxResident + population + 64);
    std::uint64_t incarnation = 1, ordinal = 1;
    std::array<no::RequestHandle, 2> roots;
    for (auto& root : roots) {
        const auto accepted = core.submit(root_request, 0, incarnation, ordinal);
        CHECK(accepted.status == no::SubmitStatus::Accepted && accepted.handle);
        if (!accepted.handle) return;
        root = *accepted.handle;
    }
    Probe::Seeds seeds;
    std::size_t cause_position = 0;
    for (std::size_t index = 0; index < population; ++index) {
        no::Request request = child_request;
        no::WaitForApplied wait;
        wait.parent = roots[index == 0 ? 0 : 1];
        request.owner = wait;
        const auto accepted = core.submit(request, 0, incarnation, ordinal);
        CHECK(accepted.status == no::SubmitStatus::Accepted);
        if (index == 0) {
            // Both roots and the first waiting child precede this cause:
            // position 3 is in the first spilled page, unlike populate().
            cause_position = core.history_end();
            const auto cancelled = core.cancel(roots[0], ordinal);
            CHECK(cancelled.status == no::CancelStatus::Cancelled);
            seeds.push_back({no::EventId{core.identity(), cancelled.event_ordinal}, roots[0]});
        }
    }
    const auto cancelled = core.cancel(roots[1], ordinal);
    CHECK(cancelled.status == no::CancelStatus::Cancelled);
    seeds.push_back({no::EventId{core.identity(), cancelled.event_ordinal}, roots[1]});
    CHECK(core.live().size() == population);
    CHECK(cause_position == 3 && cause_position < kJournalChunk);
    const auto cause_address = reinterpret_cast<std::uintptr_t>(&core.history_at(cause_position));
    const auto cause_value = full_fold::value(core.history_at(cause_position));
    Probe::note_setup(consumer);

    // These rejected commands add history without more waiting children.
    // Fold each new row while it is resident; after the first spill, neither
    // note_setup's event range nor its incremental fold revisits the cause.
    while (core.history_end() <= kJournalMaxResident) {
        const auto first = core.history_end();
        CHECK(core.submit(filler, 0, incarnation, ordinal).status == no::SubmitStatus::Rejected);
        Probe::note_setup(consumer, first);
    }
    const auto before = core.history_end();
    CHECK(before == kJournalMaxResident + 1);
    CHECK(core.history_base() == 0);
    const auto allocations_before = global_allocation::allocations;
    peak_bytes = 0;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    // apply/prepare_parent_terminal must event_at() the cold spilled cause.
    Probe::drain(consumer, actual, seeds);
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    std::printf("X08 reload population=%zu direct=%d cause_position=%zu peak=%zu bound=%zu "
                "allocations=%zu definition_bytes=%zu\n",
                population, direct, cause_position, peak_bytes, kDrainBytes,
                global_allocation::allocations - allocations_before, sizeof(no::RequestDefinition));
    const auto measured_reload_peak = peak_bytes;
    std::printf("X08 reload_budget population=%zu direct=%d measured_peak=%zu budget=%zu "
                "slots=%zu definition_bytes=%zu\n",
                population, direct, measured_reload_peak, reload_budget_bytes,
                kJournalCacheSlots, sizeof(no::RequestDefinition));
    // Only reload uses its computed byte budget; kDrainBytes remains 4096
    // for every other drain (and is retained in the diagnostic above).
    CHECK(measured_reload_peak <= reload_budget_bytes);
    CHECK(!Probe::failed(consumer));
    CHECK(core.live().empty());
    CHECK(core.history_end() - before == population);
    CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(cause_position)) != cause_address);
    CHECK(full_fold::value(core.history_at(cause_position)) == cause_value);
    std::size_t early = 0, late = 0;
    for (std::size_t index = before; index < core.history_end(); ++index) {
        const auto* row = std::get_if<no::CancelledEvent>(&core.history_at(index));
        CHECK(row && row->reason == no::CancelReason::OwnerGone && row->cause);
        if (!row || !row->cause) continue;
        if (row->cause->ordinal == seeds[0].first.ordinal) ++early;
        else if (row->cause->ordinal == seeds[1].first.ordinal) ++late;
        else CHECK(false);
    }
    CHECK(early == 1 && late == population - 1);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(Probe::stats(consumer).book_rows == population);
    CHECK(Probe::stats(consumer).waiting_rows == population);
    CHECK(Probe::stats(consumer).seed_rows == seeds.size());
#endif
    const auto finished_hash = consumer.continuation_hash();
    const auto finished_history = core.history_end();
    Probe::drain(consumer, actual, seeds);
    CHECK(!Probe::failed(consumer));
    CHECK(consumer.continuation_hash() == finished_hash);
    CHECK(core.history_end() == finished_history);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
#endif
    exercise_reload_control(direct, root_request, filler, reload_budget_bytes);
}

no::SubmitResult submit_limit_case(no::WorkingRequestCore& core, const no::Request& request,
                                  std::uint64_t& incarnation, std::uint64_t& ordinal, bool direct) {
    if (!direct) return core.submit(request, 0, incarnation, ordinal);
    auto installed = core.apply_submit(request, no::CommandContext{}, incarnation, ordinal);
    ++ordinal;
    if (installed.result.status == no::SubmitStatus::Accepted) ++incarnation;
    return std::move(installed.result);
}

void exercise_limits(bool direct) {
    // Match SaveAr, not sizeof(Request): each variant has a uint32_t tag,
    // strings have uint64_t lengths, empty alternatives still write sizeof(T),
    // and each absent optional / present definition writes a one-byte flag.
    // These accepted requests have no predecessor, root or kept binding.
    static_assert(std::is_trivially_copyable_v<no::RequestHandle>
                  && std::is_trivially_copyable_v<no::Birth>);
    constexpr auto tag = sizeof(std::uint32_t);
    constexpr auto flag = sizeof(unsigned char);
    constexpr auto request_bytes =
        tag + sizeof(no::Transact) + 2 * sizeof(std::uint64_t)
        + tag + sizeof(no::Market) + tag + sizeof(no::ImmediateRemaining)
        + tag + sizeof(no::Independent) + tag + sizeof(no::NoGroup)
        + tag + sizeof(no::Absolute);
    constexpr auto accepted_bytes =
        tag + sizeof(std::uint64_t) + flag + sizeof(no::RequestHandle) + request_bytes
        + sizeof(no::Birth) + flag + sizeof(no::RequestOrigin) + flag
        + sizeof(std::uint64_t) + flag + sizeof(no::CommandSurface);
    static_assert(accepted_bytes < kJournalSpillPageBytes);
    constexpr auto under_label = kJournalSpillPageBytes - accepted_bytes - 1;
    constexpr auto exact_label = kJournalSpillPageBytes - accepted_bytes;
    constexpr auto over_label = kJournalSpillPageBytes - accepted_bytes + 1;
    const no::Request filler{no::Transact{0.0}, "filler", ""};
    {
        no::WorkingRequestCore core{no::RunIdentity{"x08-limits-fit", 1}};
        core.reset(no::RunIdentity{"x08-limits-fit", 1});
        core.reserve(kJournalMaxResident + 64);
        std::uint64_t incarnation = 1, ordinal = 1;
        const no::Request under{no::Transact{1.0}, std::string(under_label, 'u'), ""};
        const no::Request exact{no::Transact{1.0}, std::string(exact_label, 'e'), ""};
        CHECK(submit_limit_case(core, under, incarnation, ordinal, direct).status
              == no::SubmitStatus::Accepted);
        CHECK(submit_limit_case(core, exact, incarnation, ordinal, direct).status
              == no::SubmitStatus::Accepted);
        while (core.history_end() < kJournalMaxResident)
            CHECK(submit_limit_case(core, filler, incarnation, ordinal, direct).status
                  == no::SubmitStatus::Rejected);
        std::array<std::uint64_t, kJournalChunk + 1> values{}, ordinals{};
        std::array<std::uintptr_t, kJournalChunk + 1> addresses{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto& event = core.history_at(index);
            values[index] = full_fold::value(event);
            ordinals[index] = std::visit([](const auto& row) { return row.ordinal; }, event);
            addresses[index] = reinterpret_cast<std::uintptr_t>(&event);
        }
        // Successive appends must spill pages [0,1), [1,2), then [2,32).
        // The first two pages cannot include even the following event's tag.
        for (std::size_t step = 0; step < 3; ++step) {
            CHECK(submit_limit_case(core, filler, incarnation, ordinal, direct).status
                  == no::SubmitStatus::Rejected);
            CHECK(core.history_end() == kJournalMaxResident + step + 1);
            CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(step)) != addresses[step]);
            if (step < 2)
                CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(step + 1))
                      == addresses[step + 1]);
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            CHECK(full_fold::value(core.history_at(index)) == values[index]);
            const auto* found = core.event_at(no::EventId{core.identity(), ordinals[index]});
            CHECK(found && full_fold::value(*found) == values[index]);
        }
        const auto& under_read = std::get<no::AcceptedEvent>(core.history_at(0));
        const auto& exact_read = std::get<no::AcceptedEvent>(core.history_at(1));
        CHECK(under_read.request().label == under.label);
        CHECK(exact_read.request().label == exact.label);

        // Retire at a short-page boundary, inside the remaining chunk, then
        // through every spilled row. A new spill reuses page number zero.
        CHECK(core.retire_history(ordinals[0]) == 1);
        CHECK(core.history_base() == 1);
        CHECK(!core.event_at(no::EventId{core.identity(), ordinals[0]}));
        CHECK(full_fold::value(core.history_at(1)) == values[1]);
        CHECK(core.retire_history(ordinals[15]) == 15);
        CHECK(core.history_base() == 16);
        CHECK(full_fold::value(core.history_at(16)) == values[16]);
        CHECK(core.retire_history(ordinals[kJournalChunk - 1]) == kJournalChunk - 16);
        CHECK(core.history_base() == kJournalChunk);
        CHECK(full_fold::value(core.history_at(kJournalChunk)) == values[kJournalChunk]);
        for (std::size_t index = 0; index < kJournalChunk - 2; ++index)
            CHECK(submit_limit_case(core, filler, incarnation, ordinal, direct).status
                  == no::SubmitStatus::Rejected);
        CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(kJournalChunk))
              != addresses[kJournalChunk]);
        CHECK(full_fold::value(core.history_at(kJournalChunk)) == values[kJournalChunk]);
        const auto* found = core.event_at(no::EventId{core.identity(), ordinals[kJournalChunk]});
        CHECK(found && full_fold::value(*found) == values[kJournalChunk]);
    }
    {
        no::WorkingRequestCore core{no::RunIdentity{"x08-limits-refuse", 1}};
        core.reset(no::RunIdentity{"x08-limits-refuse", 1});
        core.reserve(kJournalMaxResident + 64);
        std::uint64_t incarnation = 1, ordinal = 1;
        const no::Request over{no::Transact{1.0}, std::string(over_label, 'o'), ""};
        const auto accepted = submit_limit_case(core, over, incarnation, ordinal, direct);
        CHECK(accepted.status == no::SubmitStatus::Accepted);
        while (core.history_end() < kJournalMaxResident)
            CHECK(submit_limit_case(core, filler, incarnation, ordinal, direct).status
                  == no::SubmitStatus::Rejected);
        const auto before = full_fold::core_value(core);
        const auto before_end = core.history_end();
        const auto before_incarnation = incarnation, before_ordinal = ordinal;
        std::array<std::uintptr_t, kJournalChunk> addresses{};
        for (std::size_t index = 0; index < addresses.size(); ++index)
            addresses[index] = reinterpret_cast<std::uintptr_t>(&core.history_at(index));
        // The event is already resident; the next append reaches the spill
        // refusal in reserve_plan/seal_direct. Retry also must publish nothing.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            bool refused = false;
            try {
                (void)submit_limit_case(core, filler, incarnation, ordinal, direct);
            } catch (const detail::ScratchIoError& error) {
                refused = true;
                CHECK(error.code() == std::error_code(EFBIG, std::generic_category()));
                CHECK(std::string(error.what()).find(
                          "native o1 journal event exceeds the spill page byte bound")
                      != std::string::npos);
            }
            CHECK(refused);
            CHECK(core.history_end() == before_end && core.history_base() == 0);
            CHECK(incarnation == before_incarnation && ordinal == before_ordinal);
            CHECK(full_fold::core_value(core) == before);
            for (std::size_t index = 0; index < addresses.size(); ++index)
                CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(index)) == addresses[index]);
        }
        // Removing only the oversized event leaves a partial resident chunk.
        // The following appends must still spill/read its surviving prefix.
        const auto survivor = full_fold::value(core.history_at(1));
        CHECK(core.retire_history(accepted.event_ordinal) == 1);
        for (unsigned index = 0; index < 2; ++index)
            CHECK(submit_limit_case(core, filler, incarnation, ordinal, direct).status
                  == no::SubmitStatus::Rejected);
        CHECK(core.history_base() == 1 && core.history_end() == before_end + 2);
        CHECK(reinterpret_cast<std::uintptr_t>(&core.history_at(1)) != addresses[1]);
        CHECK(full_fold::value(core.history_at(1)) == survivor);
    }
    std::printf("X08 limits direct=%d page_bytes=%zu accepted_fixed_bytes=%zu "
                "under_label=%zu exact_label=%zu over_label=%zu\n",
                direct, kJournalSpillPageBytes, accepted_bytes, under_label, exact_label, over_label);
}

void invalid_cause(bool direct) {
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    ScratchFixture scratch;
    const auto scratch_digest = scratch.digest();
#endif
    Host actual;
    Host expected;
    auto& consumer = actual.consumer();
    auto& oracle = expected.consumer();
    consumer.set_direct_mutation(direct);
    oracle.set_direct_mutation(direct);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    consumer.set_dependency_scratch(scratch.store);
#endif
    auto seeds = populate(consumer, 64, Shape::Wide);
    auto reference_seeds = populate(oracle, 64, Shape::Wide);
    seeds.resize(1);
    reference_seeds.resize(1);
    seeds.front().first.ordinal = reference_seeds.front().first.ordinal = 0;
    Probe::reference(oracle, expected, reference_seeds);
    Probe::drain(consumer, actual, seeds);
    CHECK(Probe::failed(consumer) && Probe::failed(oracle));
    CHECK(consumer.request_core().live().size() == 64);
    CHECK(full_fold::core_value(consumer.request_core())
          == full_fold::core_value(oracle.request_core()));
    CHECK(consumer.continuation_hash() == oracle.continuation_hash());
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
#endif
}
}  // namespace

int main() {
    for (const bool direct : {true, false}) {
        exercise(64, Shape::Wide, direct);
        exercise(512, Shape::Wide, direct);
        exercise(512, Shape::Deep, direct);
        exercise(512, Shape::Tree, direct);
        exercise(64, Shape::ManySeeds, direct);
        exercise(512, Shape::ManySeeds, direct);
        // The smallest adjacent populations whose 2*N + 5 actual events spill;
        // their reserve requests are 16448 and 16450, both above 16384.
        // Without the cap, these requests need different numbers of chunks.
        const auto reserve_peak_at_cap = exercise_beyond_cap(8190, direct);
        const auto reserve_peak_beyond_cap = exercise_beyond_cap(8191, direct);
        CHECK(reserve_peak_at_cap > 0);
        CHECK(reserve_peak_at_cap == reserve_peak_beyond_cap);
        exercise_reload(direct);
        exercise_limits(direct);
        invalid_cause(direct);
    }
    std::printf("%s X08 parent-terminal scratch (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
