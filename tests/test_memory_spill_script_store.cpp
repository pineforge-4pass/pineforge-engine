// Script-store workspace, backing growth and retirement are checked.
// StoreImpl is implementation-only. Include its real implementation verbatim,
// then inspect its actual store_
// with the repository's explicit-instantiation member-pointer pattern.
// The static pineforge archive supplies the remaining implementation units.
// Bytes are unbounded by design at this layer (or bounded by an attached
// arena). Finite growth/retirement observations below do not assert a byte cap.
#include "../src/detail/o1_store.cpp"
#include <pineforge/array.hpp>
#include <pineforge/map.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace memory_spill_script_store {
namespace detail = pineforge::detail;
namespace o1 = detail::o1;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Chunks { friend auto access(Chunks); };
template struct Access<Chunks, &o1::StoreImpl::store_>;

std::size_t checks = 0;
std::size_t failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(bool passed, const char* file, int line, const char* expression) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [script-store] %s:%d: %s (trial=%s)\n",
                     file, line, expression, trial.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) \
    ::memory_spill_script_store::check(bool(expression), __FILE__, __LINE__, #expression)
// A failed ordinary assertion continues. Only an unsafe fixture prerequisite
// leaves its trial; the dispatcher still visits all later trials.
#define MEM_NEED(expression) do { \
    if (!MEM_CHECK(expression)) throw ::memory_spill_script_store::PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // The prerequisite already reported its row, expression and line.
    } catch (const std::exception& error) {
        MEM_CHECK(false && "unexpected exception");
        std::fprintf(stderr, "script-store exception detail: %s\n", error.what());
    } catch (...) {
        MEM_CHECK(false && "unknown exception");
    }
}

std::uint64_t chunks_for(std::uint64_t bytes, std::uint64_t chunk_bytes) {
    // Chunk zero exists even for an empty object.
    return std::max<std::uint64_t>(1, (bytes + chunk_bytes - 1) / chunk_bytes);
}

std::uint64_t expected_reported_workspace(const o1::Shape& shape) {
    // Independent layout arithmetic from StoreImpl's scratch-record format:
    // Core, page-slot metadata, hash heads, cache pages and six work pages.
    // StoreImpl::stats additionally reports two eight-entry, three-word tables.
    // Do not call required_bytes(), bucket_count(), or stats() for the oracle.
    const auto aligned = [](std::uint64_t bytes, std::uint64_t alignment) {
        return (bytes + alignment - 1) / alignment * alignment;
    };
    std::uint64_t buckets = 4;
    while (buckets < 2 * shape.resident_pages) buckets *= 2;
    return aligned(sizeof(detail::scratch_impl::Core), 16)
        + aligned(shape.resident_pages * sizeof(detail::scratch_impl::Core::Slot), 16)
        + aligned(buckets * sizeof(std::int32_t), 64)
        + (std::uint64_t{shape.resident_pages} + 6) * shape.page_bytes
        + 2 * 8 * 3 * sizeof(std::uint64_t);
}

struct Observation {
    std::uint64_t records = 0;
    std::uint64_t objects = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t reported_workspace = 0;
};

Observation script_strings(std::size_t bars) {
    o1::Shape shape;
    shape.resident_pages = 2; // Supported minimum; use the real spilling backing.
    auto holder = std::make_shared<detail::MapStoreHolder>(
        detail::make_script_scratch_backing(), shape);
    detail::MapStoreScope scope(holder);
    MEM_NEED(detail::map_current_store() == holder);
    auto* impl = dynamic_cast<o1::StoreImpl*>(holder->store);
    MEM_NEED(impl != nullptr);
    const auto& records = impl->*access(Chunks{});
    MEM_NEED(records && records->record_count() == 0);
    const auto resident = expected_reported_workspace(shape);
    MEM_CHECK(holder->store->stats().bytes_resident == resident);
    MEM_CHECK(holder->store->stats().max_live_objects == std::uint64_t{1024} * 1024);
    const auto initial_pages = holder->backing->page_count();
    const auto initial_writes = records->stats().page_writes;
    std::uint64_t peak_pages = 0;
    o1::Id root = o1::kNullId;
    Observation observation;
    {
        pineforge::ScriptArray<std::string> array(*detail::map_current_store()->store);
        root = array.id();
        MEM_CHECK(root != o1::kNullId && holder->store->alive(root));
        MEM_CHECK(records->record_count() == 1);
        std::vector<std::string> model;
        std::uint64_t capacity = 0;
        std::uint64_t payload_bytes = 0;
        std::uint64_t payload_chunks = 0;
        for (std::size_t bar = 0; bar != bars; ++bar) {
            // A simulated script callback retains one nonempty output per bar.
            // The model is test-owned, never populated from a store read.
            model.emplace_back(1400 + 59 * bar, static_cast<char>('a' + bar % 23));
            array.push_back(model.back()); // grow, encode string, push_cell.
            if (model.size() > capacity) {
                const auto wanted = std::max<std::uint64_t>(model.size(), 2 * capacity);
                // String array cells are eight-byte ids. Source reservation
                // rounds cells to 4096 bytes; its header is four uint64 words.
                capacity = ((wanted * sizeof(std::uint64_t) + 4095) / 4096 * 4096)
                         / sizeof(std::uint64_t);
            }
            payload_bytes += model.back().size();
            payload_chunks += chunks_for(model.back().size(), shape.chunk_bytes);
            const auto root_bytes = 4 * sizeof(std::uint64_t)
                                  + capacity * sizeof(std::uint64_t);
            // resize materializes all reserved root chunks; string encode
            // writes every payload byte. Each object has its own chunk zero.
            const auto expected_records = chunks_for(root_bytes, shape.chunk_bytes)
                                        + payload_chunks;
            MEM_CHECK(array.size() == model.size() && array.capacity() == capacity);
            MEM_CHECK(records->record_count() == expected_records);
            const auto stats = holder->store->stats();
            MEM_CHECK(stats.objects_live == model.size() + 1);
            MEM_CHECK(stats.bytes_live == root_bytes + payload_bytes);
            MEM_CHECK(stats.bytes_resident == resident);
        }
        std::vector<std::string> actual;
        for (std::size_t i = 0; i != model.size(); ++i)
            actual.push_back(array.get_raw(i));
        MEM_CHECK(actual == model); // Entire retained output, including old chunks.
        // Independent branch evidence: actual records plus physical backing
        // extent and write activity beyond the two-page resident window.
        MEM_CHECK(holder->backing->page_count() > initial_pages);
        MEM_CHECK(holder->backing->page_count() > shape.resident_pages);
        MEM_CHECK(records->stats().page_writes > initial_writes);
        MEM_CHECK(holder->store->stats().bytes_backing
                 == holder->backing->page_count() * std::uint64_t{shape.page_bytes});
        const auto stats = holder->store->stats();
        observation = {records->record_count(), stats.objects_live,
                       stats.bytes_live, stats.bytes_resident};
        peak_pages = holder->backing->page_count();
        array.clear(); // Releases string objects; keeps reserved parent cells.
        const auto root_bytes = 4 * sizeof(std::uint64_t)
                              + capacity * sizeof(std::uint64_t);
        MEM_CHECK(array.size() == 0 && array.capacity() == capacity);
        MEM_CHECK(records->record_count() == chunks_for(root_bytes, shape.chunk_bytes));
        MEM_CHECK(holder->store->stats().objects_live == 1);
        MEM_CHECK(holder->store->stats().bytes_live == root_bytes);
        // Retirement may write journal pages. It cannot reduce extent_, so
        // equality to the pre-cleanup size would be an unjustified claim.
        MEM_CHECK(holder->backing->page_count() >= peak_pages);
    } // destroy_tree -> release -> erase_chunk retires the parent records.
    MEM_CHECK(records->record_count() == 0);
    MEM_CHECK(!holder->store->alive(root));
    MEM_CHECK(holder->store->stats().objects_live == 0);
    MEM_CHECK(holder->store->stats().bytes_live == 0);
    MEM_CHECK(holder->backing->page_count() >= peak_pages);
    MEM_CHECK(holder->store->stats().bytes_resident == resident);
    return observation;
}

void growth_and_retirement() {
    constexpr std::size_t n = 24;
    const auto short_run = script_strings(n);
    const auto long_run = script_strings(4 * n);
    // Retained outputs grow with script operations, while the same open shape
    // has the same reported workspace. Per-step equalities above give the
    // exact counts/bytes; these comparisons alone are not the growth proof.
    MEM_CHECK(long_run.objects - short_run.objects == 3 * n);
    MEM_CHECK(long_run.records > short_run.records);
    MEM_CHECK(long_run.logical_bytes > short_run.logical_bytes);
    MEM_CHECK(long_run.reported_workspace == short_run.reported_workspace);
}

void explicit_object_limit(std::uint64_t limit) {
    o1::Shape shape;
    shape.resident_pages = 2;
    shape.max_live_objects = limit; // Explicit parameter, not a logical byte cap.
    auto holder = std::make_shared<detail::MapStoreHolder>(
        detail::make_script_scratch_backing(), shape);
    auto* impl = dynamic_cast<o1::StoreImpl*>(holder->store);
    MEM_NEED(impl != nullptr);
    const auto& records = impl->*access(Chunks{});
    MEM_NEED(records != nullptr);
    MEM_CHECK(holder->store->stats().max_live_objects == limit);
    std::vector<std::unique_ptr<pineforge::ScriptArray<double>>> roots;
    for (std::uint64_t i = 0; i != limit; ++i)
        roots.push_back(std::make_unique<pineforge::ScriptArray<double>>(*holder->store));
    // An empty numeric array writes only its four-word header: one chunk.
    MEM_CHECK(records->record_count() == limit);
    MEM_CHECK(holder->store->stats().objects_live == limit);
    MEM_CHECK(holder->store->stats().bytes_live == limit * 4 * sizeof(std::uint64_t));
    bool refused = false;
    try {
        roots.push_back(std::make_unique<pineforge::ScriptArray<double>>(*holder->store));
    } catch (const detail::ScratchCapacityError&) {
        refused = true;
    }
    MEM_CHECK(refused);
    MEM_CHECK(records->record_count() == limit);
    MEM_CHECK(holder->store->stats().objects_live == limit);
    // An actual retirement reopens one live-object slot; ids are not a
    // lifetime admission budget. Continue even when the refusal check failed.
    roots.erase(roots.begin());
    MEM_CHECK(records->record_count() == limit - 1);
    MEM_CHECK(holder->store->stats().objects_live == limit - 1);
    roots.push_back(std::make_unique<pineforge::ScriptArray<double>>(*holder->store));
    MEM_CHECK(records->record_count() == limit);
    MEM_CHECK(holder->store->stats().objects_live == limit);
    roots.clear();
    MEM_CHECK(records->record_count() == 0);
    MEM_CHECK(holder->store->stats().objects_live == 0);
    MEM_CHECK(holder->store->stats().bytes_live == 0);
}
} // namespace memory_spill_script_store

int main() {
    using namespace memory_spill_script_store;
    run("strings: N=24 and 4N=96, logical retirement and retained extent",
        growth_and_retirement);
    run("explicit object limit=3", [] { explicit_object_limit(3); });
    run("explicit object limit=7", [] { explicit_object_limit(7); });
    std::printf("memory proof spill_script_store: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
