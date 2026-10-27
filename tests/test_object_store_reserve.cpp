// CR417-RESERVE. This test is intentionally red on the pre-reservation head:
// scratch_defaults.hpp has no object reservation functions there.
#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchBacking;
using pineforge::detail::ScratchBackingKind;
using pineforge::detail::ScratchBuffer;
using pineforge::detail::ScratchBytes;
using pineforge::detail::ScratchFile;
using pineforge::detail::ScratchPageId;
using pineforge::detail::ScratchStatus;
using pineforge::detail::ScratchStore;
using pineforge::detail::o1::Shape;
using pineforge::detail::o1::Stats;
using pineforge::detail::o1::Store;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void reserve_rows() {
    using pineforge::detail::object_physical_bytes;
    using pineforge::detail::object_store_reserve_bytes;
    struct Row {
        std::uint64_t bars;
        std::uint64_t objects_per_execution;
        std::uint64_t record_bytes;
        std::uint64_t physical_expected;
        std::uint64_t expected;
    };
    // These values are hand-derived from the 56-byte cell payload, the 16-byte
    // header, 1024-byte object chunks (one record each, with its own key cell
    // and 50-byte index entry) and 63 cells per 4096-byte page:
    // bytes = ceil(cells * 4096 / 63) + records * 50, cells = records + value cells.
    const std::array<Row, 15> rows{{
        {0, 1, 0, 181, 0},
        {1, 0, 0, 181, 0},
        {1, 1, 0, 181, 181},     // 2 cells -> 131, + 50
        {1, 1, 40, 181, 181},
        {1, 1, 41, 246, 246},    // 3 cells -> 196, + 50
        {1, 1, 96, 246, 246},
        {1, 1, 97, 311, 311},    // 4 cells -> 261, + 50
        {1, 1, 1000, 1351, 1351},  // 20 cells -> 1301, + 50
        // Five records, each rounding its own value blob: chunk 0 holds the
        // 16-byte header and 1024 payload bytes (ceil(1040 / 56) = 19 cells),
        // chunks 1..3 hold 1024 bytes each (19 cells each), chunk 4 holds 904
        // bytes (ceil(904 / 56) = 17 cells): 5 key cells + 93 value cells =
        // 98 cells -> ceil(98 * 4096 / 63) = 6372, + 5 * 50 index bytes = 6622.
        {1, 1, 5000, 6622, 6622},
        {1, 2, 0, 181, 362},
        {1, 3, 0, 181, 543},
        {200000, 1, 0, 181, 36200000},
        {1000000, 1, 0, 181, 181000000},
        {1, 1, 0, 181, 181},
        {0, 1, 5000, 6622, 0},
    }};
    for (const auto& row : rows) {
        require(object_physical_bytes(row.record_bytes) == row.physical_expected,
                "object physical row");
        require(object_store_reserve_bytes(row.bars, row.objects_per_execution,
                                           row.record_bytes) == row.expected,
                "object reserve row");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    require(object_store_reserve_bytes(maximum / 2, 4, 0) == maximum,
            "reserve multiplication saturates");
    require(object_store_reserve_bytes(1, maximum, 0) == maximum,
            "unbounded execution sentinel");
    require(object_store_reserve_bytes(0, maximum, 0) == 0,
            "zero bars win over unbounded sentinel");

    const auto base = object_store_reserve_bytes(1, 1, 0);
    for (const std::uint64_t bars :
         std::initializer_list<std::uint64_t>{1, 2, 10, 200000, 1000000})
        require(object_store_reserve_bytes(bars, 1, 0) >= base,
                "reserve is monotone in bars");
    for (const std::uint64_t executions :
         std::initializer_list<std::uint64_t>{1, 2, 3, 100})
        require(object_store_reserve_bytes(1, executions, 0) >= base,
                "reserve is monotone in executions");
    for (const std::uint64_t bytes : std::initializer_list<std::uint64_t>{
             0, 40, 41, 96, 97, 1000, 5000})
        require(object_store_reserve_bytes(1, 1, bytes) >= base,
                "reserve is monotone in record bytes");

    // A host floor is applied by the caller. The pure function does not lower
    // or rewrite that floor.
    const auto stated_floor = std::uint64_t{1000000};
    require(base < stated_floor, "caller-owned floor remains external");
}

class ArenaBacking final : public ScratchBacking {
public:
    ArenaBacking()
        : ScratchBacking(ScratchBackingKind::disk, 4096),
          file_(ScratchFile::IoRaiser::auxiliary) {
        file_.set_store(ScratchStore::command);
        file_.open("pineforge-object-reserve");
    }

    std::uint64_t page_count() const noexcept override { return extent_pages_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (id >= extent_pages_) return ScratchStatus::not_found;
        try {
            file_.read(id * page_size(), destination.data, destination.size);
            return ScratchStatus::ok;
        } catch (...) {
            return ScratchStatus::io_error;
        }
    }

    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        try {
            file_.write(id * page_size(), source.data, source.size);
            if (id >= extent_pages_) extent_pages_ = id + 1;
            return ScratchStatus::ok;
        } catch (...) {
            return ScratchStatus::io_error;
        }
    }

    ScratchStatus do_sync(pineforge::detail::ScratchDurability) noexcept override {
        return ScratchStatus::ok;
    }

private:
    mutable ScratchFile file_;
    std::uint64_t extent_pages_ = 0;
};

class ArenaAttachment {
public:
    explicit ArenaAttachment(std::uint64_t bytes) {
        require(ScratchArena::attached() == nullptr, "slope starts without an arena");
        char path[] = "/tmp/pineforge-object-reserve-XXXXXX";
        fd_ = ::mkstemp(path);
        require(fd_ >= 0, "create slope arena");
        require(::unlink(path) == 0, "unlink slope arena");
        require(::ftruncate(fd_, static_cast<off_t>(bytes)) == 0,
                "size slope arena");
        ScratchArena::attach(fd_, bytes);
    }

    ~ArenaAttachment() {
        ScratchArena::detach();
        ::close(fd_);
    }

    ArenaAttachment(const ArenaAttachment&) = delete;
    ArenaAttachment& operator=(const ArenaAttachment&) = delete;

private:
    int fd_ = -1;
};

bool slope_row(std::uint64_t payload_bytes) {
    using pineforge::detail::kObjectArenaChunkBytes;
    using pineforge::detail::kObjectArenaDirectoryBytesPerChunk;
    using pineforge::detail::object_physical_bytes;
    constexpr std::uint64_t kN = 10000;
    constexpr std::uint64_t kArenaBytes = 256 * ScratchArena::kChunkBytes;
    ArenaAttachment attachment(kArenaBytes);
    ArenaBacking backing;
    Shape shape;
    auto store = Store::open(shape, backing);

    const Stats before = store->stats();
    for (std::uint64_t index = 0; index < kN; ++index)
        (void)store->create(payload_bytes, 1);
    const Stats middle = store->stats();
    for (std::uint64_t index = 0; index < kN; ++index)
        (void)store->create(payload_bytes, 1);
    const Stats after = store->stats();

    require(before.bytes_resident == middle.bytes_resident
                && middle.bytes_resident == after.bytes_resident,
            "resident workspace is flat across object counts");
    require(after.bytes_backing >= middle.bytes_backing
                && middle.bytes_backing >= before.bytes_backing,
            "backing grows monotonically");

    const auto expected_per_object = object_physical_bytes(payload_bytes);
    // 2 * kN objects were created in two halves of kN each.
    const auto expected_total = expected_per_object * 2 * kN;
    const auto directory_chunks =
        expected_total / kObjectArenaChunkBytes + 1;
    const auto directory_tolerance =
        directory_chunks * kObjectArenaDirectoryBytesPerChunk;
    const auto measured =
        (after.bytes_backing - before.bytes_backing) / (2 * kN);
    // The observed numbers are printed before any bound is checked so a
    // failing bound shows what the store really cost.
    std::fprintf(stderr,
                 "slope payload=%llu first_half_per_object=%llu "
                 "total_per_object=%llu formula=%llu resident=%llu\n",
                 static_cast<unsigned long long>(payload_bytes),
                 static_cast<unsigned long long>(
                     (middle.bytes_backing - before.bytes_backing) / kN),
                 static_cast<unsigned long long>(measured),
                 static_cast<unsigned long long>(expected_per_object),
                 static_cast<unsigned long long>(after.bytes_resident));
    // bytes_backing is a page-granularity upper-bound observation. The
    // formula is the host-reliant physical bound; only the separately named
    // four-byte-per-MiB arena directory is allowed as tolerance. The lower
    // half check prevents a vacuous bound.
    // Every payload is measured and printed before the verdict, so one failing
    // row does not hide the numbers of the others.
    bool within = true;
    if (!(after.bytes_backing - before.bytes_backing
              <= expected_total + directory_tolerance)) {
        std::fprintf(stderr, "slope payload=%llu: backing slope stays below physical bound: FAILED\n",
                     static_cast<unsigned long long>(payload_bytes));
        within = false;
    }
    if (!(after.bytes_backing - before.bytes_backing >=
          (expected_per_object / 2) * 2 * kN)) {
        std::fprintf(stderr, "slope payload=%llu: backing slope is not a vacuous bound: FAILED\n",
                     static_cast<unsigned long long>(payload_bytes));
        within = false;
    }
    if (!(measured <= expected_per_object + kObjectArenaDirectoryBytesPerChunk)) {
        std::fprintf(stderr, "slope payload=%llu: measured per-object slope: FAILED\n",
                     static_cast<unsigned long long>(payload_bytes));
        within = false;
    }
    return within;
}

void backing_slopes() {
    bool all_within = true;
    for (const std::uint64_t payload_bytes :
         std::initializer_list<std::uint64_t>{0, 16, 100, 1000, 5000})
        all_within = slope_row(payload_bytes) && all_within;
    require(all_within, "backing slope stays within the physical bound for every payload");
}

} // namespace

int main() {
    try {
        reserve_rows();
        backing_slopes();
        std::puts("test_object_store_reserve: rows and backing slopes passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_object_store_reserve: %s\n", error.what());
        return 1;
    }
}
