#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/command_scratch_cache.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge::detail;

constexpr std::size_t kMinimumCachedPageBytes = CachedScratchBacking::kMinPageBytes;
constexpr std::size_t kDocumentedFixedObjectBytes = CachedScratchBacking::kFixedObjectBytes;
constexpr std::size_t kDocumentedMemoryBytes =
    3 * (CachedScratchBacking::kCacheBytes
         + (CachedScratchBacking::kCacheBytes / kMinimumCachedPageBytes)
               * sizeof(ScratchPageId)
         + kDocumentedFixedObjectBytes);
constexpr std::size_t kMiddleCachedPageBytes = CachedScratchBacking::kMinPageBytes * 8;
constexpr std::size_t kLargeCachedPageBytes = CachedScratchBacking::kMinPageBytes * 128;
// At the default cache size the documented bound is 198 KiB; at any other size
// the same formula holds and only the figure differs.
static_assert(CachedScratchBacking::kCacheBytes != 64 * 1024 || kDocumentedMemoryBytes == 202752,
              "the documented default command-scratch bound is 198 KiB");
static_assert(CachedScratchBacking::kCommandScratchMemoryBytes == kDocumentedMemoryBytes,
              "the command-scratch bound must include the slot table and fixed object");

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

std::size_t slot_count_for(std::size_t page_size) {
    return page_size >= kMinimumCachedPageBytes
            && page_size <= CachedScratchBacking::kCacheBytes
        ? CachedScratchBacking::kCacheBytes / page_size : 0;
}

void check_slot_table_allocation(std::size_t page_size) {
    const auto slot_count = slot_count_for(page_size);
    require(slot_count != 0, "the allocation row uses a cached page geometry");
    if (page_size == CachedScratchBacking::kMinPageBytes)
        require(slot_count == CachedScratchBacking::kMaxSlots,
                "the minimum page geometry uses the maximum slot count");

    // The inner backing holds exactly kInnerPages pages (one), so page id
    // `slot_count - 1` is inside it only when it is page 0, that is only when the
    // cache has a single slot (a page as large as the cache). Otherwise the write
    // is beyond the inner extent and the inner refuses it with programming_error.
    constexpr std::size_t kInnerPages = 1;
    std::vector<std::byte> storage(kInnerPages * page_size);
    std::vector<std::byte> page(page_size);
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page_size);
    AllocationCapture observed;
    ScratchStatus highest_slot_status = ScratchStatus::ok;
    {
        AllocationObservation observation(observed);
        CachedScratchBacking cache(std::move(inner));
        highest_slot_status = cache.write_page(
            static_cast<ScratchPageId>(slot_count - 1), bytes(page));
    }

    require(observed.count == 2, "cache construction allocates exactly two arrays");
    require(observed.sizes[0] == CachedScratchBacking::kCacheBytes,
            "the page buffer allocation uses kCacheBytes");
    require(observed.sizes[1] == slot_count * sizeof(ScratchPageId),
            "the slot table allocation uses the wrapped page slot count");
    // Expectation derived per geometry from the page index, not a fixed status:
    // page id slot_count - 1 is in range of the one-page inner iff slot_count - 1 < kInnerPages.
    const ScratchStatus expected_highest_slot_status =
        slot_count - 1 < kInnerPages ? ScratchStatus::ok : ScratchStatus::programming_error;
    require(highest_slot_status == expected_highest_slot_status,
            "the highest slot write status follows whether its page is inside the inner backing");
}

struct ExpectedRead {
    ScratchStatus status;
    bool hit;
};

class ReferenceModel {
public:
    ReferenceModel(std::size_t page_size, std::size_t capacity_pages)
        : page_size_(page_size),
          capacity_pages_(capacity_pages),
          bytes_(page_size * capacity_pages),
          slot_count_(slot_count_for(page_size)),
          page_ids_(slot_count_, scratch_no_page),
          cached_(slot_count_ * page_size) {}

    std::uint64_t page_count() const noexcept { return extent_; }

    void seed(ScratchPageId id, ScratchBytes source) {
        require(id < capacity_pages_, "reference seed fits its backing");
        write_backing(id, source);
    }

    ScratchStatus write(ScratchPageId id, ScratchBytes source) {
        if (id >= capacity_pages_) return ScratchStatus::programming_error;
        if (slot_count_ != 0) page_ids_[slot(id)] = scratch_no_page;
        write_backing(id, source);
        if (slot_count_ != 0) remember(id, source.data);
        return ScratchStatus::ok;
    }

    ExpectedRead read(ScratchPageId id, ScratchBuffer destination) {
        if (slot_count_ != 0 && id < extent_) {
            const auto index = slot(id);
            if (page_ids_[index] == id) {
                std::memcpy(destination.data, cached_.data() + index * page_size_, page_size_);
                return {ScratchStatus::ok, true};
            }
        }
        if (id >= extent_) return {ScratchStatus::not_found, false};
        const auto source = bytes_.data() + static_cast<std::size_t>(id) * page_size_;
        std::memcpy(destination.data, source, page_size_);
        if (slot_count_ != 0) remember(id, source);
        return {ScratchStatus::ok, false};
    }

    const std::vector<std::byte>& storage() const noexcept { return bytes_; }

private:
    std::size_t slot(ScratchPageId id) const noexcept {
        return static_cast<std::size_t>(id % slot_count_);
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
        page_ids_[slot(id)] = id;
    }

    const std::size_t page_size_;
    const std::size_t capacity_pages_;
    std::vector<std::byte> bytes_;
    std::uint64_t extent_ = 0;
    const std::size_t slot_count_;
    std::vector<ScratchPageId> page_ids_;
    std::vector<std::byte> cached_;
};

void prime_stale_slot(std::size_t page_size) {
    if (slot_count_for(page_size) == 0) return;
    std::vector<std::byte> storage(page_size * 2);
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{storage.data(), storage.size()}, page_size);
    const auto seeded = image(page_size, 901);
    require(inner->write_page(0, bytes(seeded)) == ScratchStatus::ok,
            "prime backing accepts its first page");
    {
        CachedScratchBacking cache(std::move(inner));
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
    const auto expected_read = expected.read(id, buffer(expected_bytes));
    const auto before_logical = logical.reads;
    const auto before_physical = physical.reads;
    const auto status = actual.read_page(id, buffer(actual_bytes));
    require(status == expected_read.status, "read status follows the reference model");
    require(actual_bytes == expected_bytes, "read bytes follow the reference model");
    require(logical.reads == before_logical + 1, "each logical read is charged once");
    require(physical.reads == before_physical + (expected_read.hit ? 0 : 1),
            expected_read.hit ? "reference hit avoids the inner read"
                              : "reference miss reaches the inner backing");
    require(actual.page_count() == expected.page_count(),
            "read does not change the logical extent");
}

void exercise_geometry(std::size_t page_size) {
    const auto slot_count = slot_count_for(page_size);
    const auto highest_slot = slot_count == 0
        ? ScratchPageId{0} : static_cast<ScratchPageId>(slot_count - 1);
    const auto collision = slot_count == 0
        ? ScratchPageId{2}
        : static_cast<ScratchPageId>(slot_count + highest_slot);
    const auto slot_zero_collision = slot_count == 0
        ? ScratchPageId{1} : static_cast<ScratchPageId>(slot_count);
    const auto seeded_page = collision + 2;
    const auto absent_page = seeded_page + 1;
    const auto capacity_pages = static_cast<std::size_t>(absent_page + 1);
    require(capacity_pages <= static_cast<std::size_t>(-1) / page_size,
            "test backing geometry fits its byte vector");

    prime_stale_slot(page_size);

    std::vector<std::byte> actual_storage(page_size * capacity_pages);
    IoCounts physical;
    auto inner = std::make_unique<FixedMemoryScratchBacking>(
        ScratchBuffer{actual_storage.data(), actual_storage.size()}, page_size,
        physical.observer());
    ReferenceModel expected(page_size, capacity_pages);
    const auto seed = image(page_size, 7);
    require(inner->write_page(seeded_page, bytes(seed)) == ScratchStatus::ok,
            "real in-memory backing accepts its sparse seed");
    expected.seed(seeded_page, bytes(seed));
    physical = {};

    IoCounts logical;
    {
        CachedScratchBacking actual(std::move(inner), logical.observer());

        // The first read must miss even when the allocator hands the cache a
        // block previously used for a different backing. The second read is a
        // model-derived hit.
        check_read(actual, expected, logical, physical, 0, page_size, 11);
        check_read(actual, expected, logical, physical, 0, page_size, 12);

        // highest_slot reaches the final valid table entry. collision then
        // exercises the header's id modulo slot_count mapping.
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
    }

    require(actual_storage == expected.storage(),
            "final inner bytes match the independent reference model");
    require(logical.failures == physical.failures,
            "logical and physical failure charges agree");
}
} // namespace

int main() {
    try {
        check_slot_table_allocation(CachedScratchBacking::kMinPageBytes);
        check_slot_table_allocation(kMiddleCachedPageBytes);
        check_slot_table_allocation(kLargeCachedPageBytes);
        exercise_geometry(512);
        exercise_geometry(4 * 1024);
        exercise_geometry(64 * 1024);
        exercise_geometry(64 * 1024 + 1);
        std::puts("test_command_scratch_cache_slot_table: model-derived hits, misses and bytes");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_command_scratch_cache_slot_table: %s\n", error.what());
        return 1;
    }
}
