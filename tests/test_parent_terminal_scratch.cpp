// X08: the actual waiting population and every descendant are drained in the
// base queue's order, while the drain's additional resident bytes stay fixed.
// The production exercise also compiles on 21ba62bd; there the byte assertion
// fails because Item rows survive in the queue. Only the store audit is gated.
#include "../src/native_execution_consumer.hpp"
#include "native_order_full_fold.hpp"

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    static void note_setup(NativeExecutionConsumer& c) {
        c.note_install(native_order::EventRange{0, c.requests_.history_end()});
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
    detail::ScratchStore store{
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

struct Host final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
};

enum class Shape { Wide, Deep, Tree, ManySeeds };

Probe::Seeds populate(NativeExecutionConsumer& consumer, std::size_t population, Shape shape) {
    auto& core = Probe::core(consumer);
    core.reset(no::RunIdentity{"x08-scratch", 1});
    const auto root_count = shape == Shape::ManySeeds ? population : 2;
    // Retained command output is not queue scratch. Allocate it before the
    // measured drain so the census charges only the drain and one mutation.
    core.reserve(2 * population + 2 * root_count + 64);
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
    const auto seeds = populate(consumer, population, shape);
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
    CHECK(!consumer.failed());
    CHECK(!oracle.failed());
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
    constexpr std::size_t kDrainBytes = 4096;
    CHECK(peak_bytes <= kDrainBytes);
    std::printf("X08 population=%zu shape=%d direct=%d peak=%zu bound=%zu\n",
                population, static_cast<int>(shape), direct, peak_bytes, kDrainBytes);
    const auto finished_hash = consumer.continuation_hash();
    const auto finished_history = consumer.request_core().history_end();
    Probe::drain(consumer, actual, seeds);
    CHECK(!consumer.failed());
    CHECK(consumer.continuation_hash() == finished_hash);
    CHECK(consumer.request_core().history_end() == finished_history);
#ifdef PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(Probe::stats(consumer).book_rows == 0);
    CHECK(Probe::stats(consumer).queued_rows == 0);
#endif
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
    CHECK(consumer.failed() && oracle.failed());
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
        invalid_cause(direct);
    }
    std::printf("%s X08 parent-terminal scratch (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
