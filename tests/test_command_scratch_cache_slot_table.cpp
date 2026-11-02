#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/command_scratch_cache.hpp"

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;

constexpr std::size_t kMinimumCachedPageBytes = 512;
constexpr std::size_t kDocumentedFixedObjectBytes = 2 * 1024;
constexpr std::size_t kCommandOwners = 5;
constexpr auto kDisabled = std::numeric_limits<std::size_t>::max();
static_assert(sizeof(CachedScratchBacking) <= kDocumentedFixedObjectBytes,
              "cache payload and metadata must not enlarge the fixed object");
static_assert(std::is_same<decltype(CachedScratchBacking::kCacheBytes),
                           const std::size_t>::value, "cache capacities use size_t");
static_assert(std::is_same<ScratchPageId, std::uint64_t>::value, "page ids remain 64-bit");

struct Geometry {
    std::size_t lines = 0;
    std::size_t storage_bytes = 0;
};

// Independent byte-budget formula: each complete payload page carries all
// shared line metadata; unused payload tail bytes remain in the slab charge.
Geometry geometry_for(std::size_t page_size, std::size_t payload_bytes) {
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (page_size < kMinimumCachedPageBytes || payload_bytes > maximum) return {};
    const auto lines = payload_bytes / page_size;
    const auto line_bytes = ReadThroughCache::line_bytes(page_size);
    if (lines == 0 || line_bytes == 0) return {};
    const auto metadata_bytes = line_bytes - page_size;
    if (metadata_bytes != 0 && lines > (maximum - payload_bytes) / metadata_bytes) return {};
    return {lines, payload_bytes + lines * metadata_bytes};
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct AllocationCapture {
    std::array<std::size_t, 4> sizes{};
    std::size_t count = 0;

    static AllocationCapture* current;

    static void record(std::size_t size) noexcept {
        if (current == nullptr) return;
        if (current->count < current->sizes.size())
            current->sizes[current->count] = size;
        ++current->count;
    }
};

AllocationCapture* AllocationCapture::current = nullptr;

struct AllocationObservation {
    explicit AllocationObservation(AllocationCapture& capture) {
        require(!global_allocation::recording && global_allocation::on_recorded == nullptr,
                "begin with allocation observation disabled");
        AllocationCapture::current = &capture;
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = &AllocationCapture::record;
        global_allocation::recording = true;
    }

    ~AllocationObservation() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        global_allocation::live_bytes = 0;
        AllocationCapture::current = nullptr;
    }
};

struct IoCounts {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t syncs = 0;
    std::uint64_t failures = 0;

    static void charge(void* context, const ScratchIoCharge& io) noexcept {
        auto& counts = *static_cast<IoCounts*>(context);
        if (io.operation == ScratchIoOperation::read_page) ++counts.reads;
        if (io.operation == ScratchIoOperation::write_page) ++counts.writes;
        if (io.operation == ScratchIoOperation::sync) ++counts.syncs;
        if (io.status != ScratchStatus::ok) ++counts.failures;
    }

    ScratchIoObserver observer() noexcept { return {this, &charge}; }
};

std::vector<std::byte> image(std::size_t size, unsigned seed) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::byte>((i * 17u + seed * 29u + 5u) & 0xffu);
    return bytes;
}

ScratchBytes bytes(const std::vector<std::byte>& value) {
    return {value.data(), value.size()};
}

ScratchBuffer buffer(std::vector<std::byte>& value) {
    return {value.data(), value.size()};
}

void check_slab_allocation(std::size_t page_size, std::size_t payload_bytes) {
    const auto expected = geometry_for(page_size, payload_bytes);
    require(CachedScratchBacking::cache_allocation_bytes(page_size, payload_bytes)
                == expected.storage_bytes,
            "the adapter's byte allowance follows the independent geometry formula");
    std::vector<std::byte> storage(page_size);
    const auto page = image(page_size, 3);
    std::vector<std::byte> output(page_size);
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page_size);
    AllocationCapture observed;
    {
        AllocationObservation observation(observed);
        {
            CachedScratchBacking cache(std::move(inner), {}, 0, {}, payload_bytes);
            require(sizeof(cache) <= kDocumentedFixedObjectBytes
                        && cache.cache_slot_count() == expected.lines
                        && cache.cache_payload_bytes() == (expected.lines ? payload_bytes : 0)
                        && cache.cache_metadata_bytes()
                            == (expected.lines ? expected.storage_bytes - payload_bytes : 0)
                        && cache.cache_storage_bytes() == expected.storage_bytes,
                    "fixed object and all charged slab components follow the geometry");
            require(cache.write_page(0, bytes(page)) == ScratchStatus::ok
                        && cache.read_page(0, buffer(output)) == ScratchStatus::ok
                        && output == page,
                    "allocation census exercises a real write and read");
            const auto stats = cache.read_cache_stats();
            require(stats.current_bytes == expected.lines * ReadThroughCache::line_bytes(page_size)
                        && stats.current_payload_bytes == expected.lines * page_size
                        && stats.current_metadata_bytes
                            == expected.lines * (ReadThroughCache::line_bytes(page_size) - page_size)
                        && stats.allocation_bypass_count == 0
                        && global_allocation::live_bytes == expected.storage_bytes,
                    "shared tags borrow the slab without a second allocation");
        }
        require(global_allocation::live_bytes == 0, "destruction releases the entire slab");
    }
    // The constructor has one optional byte-array expression. A disabled
    // geometry reaches none; tag construction and page I/O reach no allocator.
    require(observed.count == (expected.lines ? 1u : 0u),
            "each enabled cache owns exactly one construction-time allocation");
    if (expected.lines)
        require(observed.sizes[0] == expected.storage_bytes,
                "the single allocation includes payload slack and every shared line tag");
}

struct ExpectedRead {
    ScratchStatus status;
    bool hit;
};

class ReferenceModel {
public:
    ReferenceModel(std::size_t page_size, std::size_t capacity_pages, std::size_t payload_bytes)
        : page_size_(page_size),
          capacity_pages_(capacity_pages),
          bytes_(page_size * capacity_pages),
          lines_(geometry_for(page_size, payload_bytes).lines),
          tags_(lines_, scratch_no_page),
          cached_(lines_ * page_size) {}

    std::uint64_t page_count() const noexcept { return extent_; }

    void seed(ScratchPageId id, ScratchBytes source) {
        require(id < capacity_pages_, "reference seed fits its backing");
        write_backing(id, source);
    }

    ScratchStatus write(ScratchPageId id, ScratchBytes source) {
        if (id >= capacity_pages_) return ScratchStatus::programming_error;
        if (lines_ != 0) tags_[slot(id)] = scratch_no_page;
        write_backing(id, source);
        if (lines_ != 0) remember(id, source.data);
        return ScratchStatus::ok;
    }

    ExpectedRead read(ScratchPageId id, ScratchBuffer destination) {
        if (lines_ != 0 && id < extent_) {
            const auto index = slot(id);
            if (tags_[index] == id) {
                std::memcpy(destination.data, cached_.data() + index * page_size_, page_size_);
                return {ScratchStatus::ok, true};
            }
        }
        if (id >= extent_) return {ScratchStatus::not_found, false};
        const auto source = bytes_.data() + static_cast<std::size_t>(id) * page_size_;
        std::memcpy(destination.data, source, page_size_);
        if (lines_ != 0) remember(id, source);
        return {ScratchStatus::ok, false};
    }

    const std::vector<std::byte>& storage() const noexcept { return bytes_; }

private:
    std::size_t slot(ScratchPageId id) const noexcept {
        return static_cast<std::size_t>(id % lines_);
    }

    void write_backing(ScratchPageId id, ScratchBytes source) {
        const auto offset = static_cast<std::size_t>(id) * page_size_;
        if (id > extent_) {
            const auto old_end = static_cast<std::size_t>(extent_) * page_size_;
            std::fill(bytes_.begin() + old_end, bytes_.begin() + offset, std::byte{0});
        }
        std::memcpy(bytes_.data() + offset, source.data, page_size_);
        if (id >= extent_) extent_ = id + 1;
    }

    void remember(ScratchPageId id, const std::byte* source) {
        const auto offset = slot(id) * page_size_;
        std::memcpy(cached_.data() + offset, source, page_size_);
        tags_[slot(id)] = id;
    }

    const std::size_t page_size_;
    const std::size_t capacity_pages_;
    std::vector<std::byte> bytes_;
    std::uint64_t extent_ = 0;
    const std::size_t lines_;
    std::vector<ScratchPageId> tags_;
    std::vector<std::byte> cached_;
};

void prime_stale_slot(std::size_t page_size, std::size_t payload_bytes) {
    if (geometry_for(page_size, payload_bytes).lines == 0) return;
    std::vector<std::byte> storage(page_size * 2);
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page_size);
    const auto seeded = image(page_size, 901);
    require(inner->write_page(0, bytes(seeded)) == ScratchStatus::ok,
            "prime backing accepts its first page");
    {
        CachedScratchBacking cache(std::move(inner), {}, 0, {}, payload_bytes);
        require(cache.write_page(0, bytes(seeded)) == ScratchStatus::ok,
                "prime cache stores a page in its first slot");
    }
}

void check_write(CachedScratchBacking& actual, ReferenceModel& expected,
                 IoCounts& logical, IoCounts& physical, ScratchPageId id,
                 const std::vector<std::byte>& source) {
    const auto expected_status = expected.write(id, bytes(source));
    const auto before_logical = logical.writes;
    const auto before_physical = physical.writes;
    const auto status = actual.write_page(id, bytes(source));
    require(status == expected_status, "write status follows the reference model");
    require(logical.writes == before_logical + 1, "each logical write is charged once");
    require(physical.writes == before_physical + 1,
            "each nonresident cache write reaches the inner backing once");
    require(actual.page_count() == expected.page_count(),
            "write extent follows the reference model");
}

void check_read(CachedScratchBacking& actual, ReferenceModel& expected,
                IoCounts& logical, IoCounts& physical, ScratchPageId id,
                std::size_t page_size, unsigned seed) {
    auto actual_bytes = image(page_size, seed);
    auto expected_bytes = actual_bytes;
    const bool in_extent = id < expected.page_count();
    const auto expected_read = expected.read(id, buffer(expected_bytes));
    const auto before_logical = logical.reads;
    const auto before_physical = physical.reads;
    const auto before_stats = actual.read_cache_stats();
    const auto status = actual.read_page(id, buffer(actual_bytes));
    require(status == expected_read.status, "read status follows the reference model");
    require(actual_bytes == expected_bytes, "read bytes follow the reference model");
    require(logical.reads == before_logical + 1, "each logical read is charged once");
    require(physical.reads == before_physical + (expected_read.hit ? 0 : 1),
            expected_read.hit ? "reference hit avoids the inner read"
                              : "reference miss reaches the inner backing");
    const auto stats = actual.read_cache_stats();
    require(stats.get_calls == before_stats.get_calls + (in_extent ? 1 : 0)
                && stats.hits == before_stats.hits + (expected_read.hit ? 1 : 0)
                && stats.misses == before_stats.misses + (in_extent && !expected_read.hit ? 1 : 0),
            "cache hit and miss statistics agree with the independent model");
    require(actual.page_count() == expected.page_count(),
            "read does not change the logical extent");
}

void exercise_geometry(std::size_t page_size, std::size_t payload_bytes) {
    const auto lines = geometry_for(page_size, payload_bytes).lines;
    const auto highest_slot = lines == 0
        ? ScratchPageId{0} : static_cast<ScratchPageId>(lines - 1);
    const auto collision = lines == 0
        ? ScratchPageId{2}
        : static_cast<ScratchPageId>(lines + highest_slot);
    const auto slot_zero_collision = lines == 0
        ? ScratchPageId{1} : static_cast<ScratchPageId>(lines);
    const auto seeded_page = collision + 2;
    const auto absent_page = seeded_page + 1;
    const auto capacity_pages = static_cast<std::size_t>(absent_page + 1);
    require(capacity_pages <= static_cast<std::size_t>(-1) / page_size,
            "test backing geometry fits its byte vector");

    prime_stale_slot(page_size, payload_bytes);

    std::vector<std::byte> actual_storage(page_size * capacity_pages);
    IoCounts physical;
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{actual_storage.data(), actual_storage.size()}, page_size,
        physical.observer());
    ReferenceModel expected(page_size, capacity_pages, payload_bytes);
    const auto seed = image(page_size, 7);
    require(inner->write_page(seeded_page, bytes(seed)) == ScratchStatus::ok,
            "real in-memory backing accepts its sparse seed");
    expected.seed(seeded_page, bytes(seed));
    physical = {};

    IoCounts logical;
    {
        CachedScratchBacking actual(std::move(inner), logical.observer(), 0, {}, payload_bytes);

        // The first read must miss even when the allocator hands the cache a
        // block previously used for a different backing. The second read is a
        // model-derived hit.
        check_read(actual, expected, logical, physical, 0, page_size, 11);
        check_read(actual, expected, logical, physical, 0, page_size, 12);

        // Reach the final line and collide with it, independent of the
        // shared cache's private metadata layout.
        check_write(actual, expected, logical, physical, highest_slot,
                    image(page_size, 21));
        check_read(actual, expected, logical, physical, highest_slot, page_size, 22);
        check_write(actual, expected, logical, physical, collision,
                    image(page_size, 31));
        check_read(actual, expected, logical, physical, highest_slot, page_size, 32);
        check_read(actual, expected, logical, physical, collision, page_size, 33);

        check_write(actual, expected, logical, physical, slot_zero_collision,
                    image(page_size, 41));
        check_read(actual, expected, logical, physical, 0, page_size, 42);
        check_read(actual, expected, logical, physical, slot_zero_collision, page_size, 43);
        check_read(actual, expected, logical, physical, absent_page, page_size, 44);
        check_read(actual, expected, logical, physical, ScratchPageId{1} << 32, page_size, 45);
    }

    require(actual_storage == expected.storage(),
            "final inner bytes match the independent reference model");
    require(logical.failures == physical.failures,
            "logical and physical failure charges agree");
}

struct OneAllocationFailure {
    const std::size_t refused_before = global_allocation::refused;

    OneAllocationFailure() {
        require(!global_allocation::refusing && global_allocation::fail_after == kDisabled,
                "arm a single allocation failure after all test setup");
        global_allocation::fail_after = 0;
    }
    ~OneAllocationFailure() { global_allocation::fail_after = kDisabled; }
    void check() const {
        require(global_allocation::fail_after == kDisabled
                    && global_allocation::refused == refused_before + 1,
                "exactly the armed allocation failed, with no retry");
    }
};

void check_slab_failure_bypass(std::size_t page_size, std::size_t payload_bytes) {
    if (geometry_for(page_size, payload_bytes).lines == 0) return;
    std::vector<std::byte> storage(4 * page_size);
    const auto page = image(page_size, 67);
    std::vector<std::byte> output(page_size);
    IoCounts physical, logical;
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page_size, physical.observer());
    const auto allocations_before = global_allocation::allocations;
    OneAllocationFailure fault;
    // A fresh backing and a nonzero prefix cap would permit residency if
    // the optional slab succeeded. Its failure must disable that mode too.
    CachedScratchBacking cache(std::move(inner), logical.observer(), payload_bytes, {}, payload_bytes);
    fault.check();
    require(cache.cache_storage_bytes() == 0 && cache.cache_payload_bytes() == 0
                && cache.cache_metadata_bytes() == 0 && cache.cache_slot_count() == 0
                && cache.read_cache_stats().allocation_bypass_count == 1,
            "the failed slab is disclosed once, before any I/O");

    constexpr std::size_t reads = 3;
    global_allocation::refusing = true;
    bool okay = cache.write_page(0, bytes(page)) == ScratchStatus::ok;
    for (std::size_t i = 0; i < reads; ++i) {
        const auto status = cache.read_page(0, buffer(output));
        okay = status == ScratchStatus::ok && output == page && okay;
    }
    const auto sync_status = cache.sync(ScratchDurability::buffered);
    global_allocation::refusing = false;

    fault.check();
    const auto stats = cache.read_cache_stats();
    require(okay && sync_status == ScratchStatus::ok
                && logical.writes == 1 && physical.writes == 1
                && logical.reads == reads && physical.reads == reads
                && logical.syncs == 1 && physical.syncs == 1
                && logical.failures == 0 && physical.failures == 0
                && cache.page_count() == 1 && !cache.carried_failure(),
            "bypass preserves bytes, extent and every logical and backing I/O charge");
    require(global_allocation::allocations == allocations_before
                && stats.put_calls == 1 && stats.get_calls == reads
                && stats.hits == 0 && stats.misses == reads && stats.bypass_reads == reads
                && stats.allocation_bypass_count == 1 && stats.current_bytes == 0,
            "optional allocation failure permanently bypasses without an exception or retry");
}

ScratchKeyOrder numeric_order() noexcept {
    return {1, nullptr,
        [](void*, const ScratchByteSource& a, const ScratchByteSource& b,
           int& result) noexcept {
            std::uint64_t left = 0, right = 0;
            if (a.size() != sizeof left || b.size() != sizeof right)
                return ScratchStatus::programming_error;
            auto status = a.read(0, {reinterpret_cast<std::byte*>(&left), sizeof left});
            if (status != ScratchStatus::ok) return status;
            status = b.read(0, {reinterpret_cast<std::byte*>(&right), sizeof right});
            if (status != ScratchStatus::ok) return status;
            result = left < right ? -1 : right < left ? 1 : 0;
            return ScratchStatus::ok;
        }};
}

void check_required_workspace_failure() {
    constexpr auto page_size = scratch_min_page_size;
    const ScratchStoreConfig config{
        scratch_min_page_count, page_size, 1, 1, numeric_order(), ScratchOpenMode::create_empty};
    // A healthy control proves this geometry reaches the real required
    // workspace allocation and prepares the shared failure marker unarmed.
    std::array<std::byte, 16 * page_size> healthy_storage{};
    FixedMemoryScratchBacking healthy_backing(
        {healthy_storage.data(), healthy_storage.size()}, page_size);
    CommandScratchStore healthy(config, &healthy_backing);
    require(healthy.status() == ScratchStatus::ok && !healthy.carried_failure()
                && healthy.stats().resident_bytes > 0,
            "the control constructs a usable store with a required workspace");

    std::array<std::byte, 16 * page_size> failed_storage{};
    FixedMemoryScratchBacking failed_backing(
        {failed_storage.data(), failed_storage.size()}, page_size);
    OneAllocationFailure fault;
    CommandScratchStore failed(config, &failed_backing);
    fault.check();
    require(failed.status() == ScratchStatus::io_error && failed.carried_failure()
                && failed.page_size() == 0 && failed.record_count() == 0
                && !failed.transaction_active() && failed_backing.page_count() == 0,
            "a required workspace failure refuses the store without backing mutation");
    bool typed_memory_failure = false;
    try { rethrow_carried_scratch_failure(failed.carried_failure()); }
    catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        typed_memory_failure = typed
            && typed->run_failure().code == pineforge::RunFailureCode::out_of_memory;
    }
    require(typed_memory_failure, "required allocation failure preserves its typed out_of_memory");
    fault.check();
}
} // namespace

int main() {
    try {
        const auto default_geometry = geometry_for(kMinimumCachedPageBytes,
                                                   CachedScratchBacking::kCacheBytes);
        require(CachedScratchBacking::kCommandScratchMemoryBytes
                    == kCommandOwners * (default_geometry.storage_bytes + kDocumentedFixedObjectBytes),
                "the default bound charges five slabs and their fixed control allowances");
        // Fixed test budgets keep this row fast even if the build default is
        // raised to GiB sizes. The largest slab is only a 16 MiB payload plus
        // metadata; the same size_t constructor and object serve every case.
        constexpr std::array<std::size_t, 6> payloads{{
            0, 512, 64 * 1024, 1024 * 1024, 1024 * 1024 + 37, 16 * 1024 * 1024,
        }};
        constexpr std::array<std::size_t, 5> page_sizes{{256, 512, 4096, 65536, 65537}};
        for (const auto page_size : page_sizes) {
            for (const auto payload : payloads) {
                check_slab_allocation(page_size, payload);
                exercise_geometry(page_size, payload);
                check_slab_failure_bypass(page_size, payload);
            }
            // Unrepresentable allowances must perform direct I/O without
            // attempting enormous allocations or reporting an allocation fault.
            for (const auto payload : {
                    static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()),
                    std::numeric_limits<std::size_t>::max()}) {
                check_slab_allocation(page_size, payload);
                exercise_geometry(page_size, payload);
            }
        }
        check_required_workspace_failure();
        std::puts("test_command_scratch_cache_slot_table: shared slab, model reads and allocation failures");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        global_allocation::fail_after = kDisabled;
        std::fprintf(stderr, "test_command_scratch_cache_slot_table: %s\n", error.what());
        return 1;
    }
}
