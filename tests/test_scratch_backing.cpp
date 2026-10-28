// Predicted first red on 6cfcff0696: the frozen closure has no
// pineforge/detail/scratch_backing.hpp (fatal error: file not found). The
// body needs only the header-defined backing, so it runs without the store.
// The complete body must be compiled there on the lab; no fail-before or
// passing execution is claimed by this edit-only lane.
#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_readback.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

const char* name_of(ScratchStatus status) {
    switch (status) {
    case ScratchStatus::ok: return "ok";
    case ScratchStatus::end: return "end";
    case ScratchStatus::cancelled: return "cancelled";
    case ScratchStatus::not_found: return "not_found";
    case ScratchStatus::programming_error: return "programming_error";
    case ScratchStatus::io_error: return "io_error";
    case ScratchStatus::corrupt: return "corrupt";
    }
    return "unknown";
}

void require_status(ScratchStatus got, ScratchStatus want, const char* message) {
    if (got == want) return;
    throw std::runtime_error(std::string(message) + ": got " + name_of(got)
                             + ", want " + name_of(want));
}

// Every attempted public operation is reported exactly once, errors included.
struct Charges {
    std::vector<ScratchIoCharge> list;
    static void on_charge(void* self, const ScratchIoCharge& charge) noexcept {
        static_cast<Charges*>(self)->list.push_back(charge);
    }
    ScratchIoObserver observer() { return ScratchIoObserver{this, &Charges::on_charge}; }
};

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i)
        out[i] = static_cast<std::byte>((i * 7u + seed * 31u + 3u) & 0xFFu);
    return out;
}

bool all_zero(const std::vector<std::byte>& bytes) {
    for (const auto byte : bytes)
        if (byte != std::byte{0}) return false;
    return true;
}

ScratchBytes view(const std::vector<std::byte>& bytes) {
    return ScratchBytes{bytes.data(), bytes.size()};
}

ScratchBuffer window(std::vector<std::byte>& bytes) {
    return ScratchBuffer{bytes.data(), bytes.size()};
}

// A backing that supports every durability level; it exists to prove that the
// medium kind is a constructor argument and flows into every charge.
class ProbeBacking final : public ScratchBacking {
public:
    ProbeBacking(ScratchBackingKind kind, std::size_t page_size, ScratchIoObserver observer)
        : ScratchBacking(kind, page_size, observer), bytes_(page_size * 4) {}
    std::uint64_t page_count() const noexcept override { return extent_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (id >= extent_) return ScratchStatus::not_found;
        std::memcpy(destination.data, bytes_.data() + static_cast<std::size_t>(id) * page_size(),
                    page_size());
        return ScratchStatus::ok;
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (id >= 4) return ScratchStatus::programming_error;
        std::memcpy(bytes_.data() + static_cast<std::size_t>(id) * page_size(), source.data,
                    page_size());
        if (id >= extent_) extent_ = id + 1;
        return ScratchStatus::ok;
    }
    ScratchStatus do_sync(ScratchDurability) noexcept override { return ScratchStatus::ok; }

private:
    std::vector<std::byte> bytes_;
    std::uint64_t extent_ = 0;
};

static_assert(std::is_same<ScratchHandle, std::uint64_t>::value, "handles are 64-bit");
static_assert(scratch_no_handle == 0, "handle zero is never a record");
static_assert(scratch_no_page == std::numeric_limits<ScratchPageId>::max(), "page sentinel");
static_assert(std::is_same<std::underlying_type<ScratchStatus>::type, std::uint32_t>::value,
              "status is a closed 32-bit enum");
static_assert(!std::is_default_constructible<ProbeBacking>::value
              && !std::is_default_constructible<FixedMemoryScratchBacking>::value,
              "a backing is never built without choosing its kind and page size");
static_assert(!std::is_copy_constructible<FixedMemoryScratchBacking>::value
              && !std::is_move_constructible<FixedMemoryScratchBacking>::value
              && !std::is_copy_assignable<FixedMemoryScratchBacking>::value
              && !std::is_move_assignable<FixedMemoryScratchBacking>::value,
              "a backing is exclusive and never copied or moved");
static_assert(std::is_abstract<ScratchBacking>::value && std::is_abstract<ScratchByteSource>::value
              && std::is_abstract<ScratchReadback>::value, "contract bases are interfaces");
static_assert(noexcept(std::declval<ScratchBacking&>().write_page(0, ScratchBytes{}))
              && noexcept(std::declval<const ScratchBacking&>().read_page(0, ScratchBuffer{}))
              && noexcept(std::declval<ScratchBacking&>().sync(ScratchDurability::buffered)),
              "no backing operation throws");

void extent_zero_fill_and_isolation() {
    constexpr std::size_t page = 512, pages = 8;
    std::vector<std::byte> storage(page * pages, std::byte{0xEE});
    FixedMemoryScratchBacking backing(ScratchBuffer{storage.data(), storage.size()}, page);
    require_status(backing.status(), ScratchStatus::ok, "fixture backing is serviceable");
    require(backing.kind() == ScratchBackingKind::test_memory, "memory backing discloses its medium");
    require(backing.page_size() == page && backing.capacity_pages() == pages
            && backing.page_count() == 0, "empty extent over a fixed capacity");

    auto image = pattern(page, 1);
    std::vector<std::byte> out(page, std::byte{0x11});
    require_status(backing.read_page(0, window(out)), ScratchStatus::not_found,
                   "a page at the extent is absent");

    require_status(backing.write_page(5, view(image)), ScratchStatus::ok, "write beyond the extent");
    require(backing.page_count() == 6, "write extends the extent to id + 1");
    for (ScratchPageId id = 0; id < 5; ++id) {
        std::fill(out.begin(), out.end(), std::byte{0x11});
        require_status(backing.read_page(id, window(out)), ScratchStatus::ok, "hole page reads");
        require(all_zero(out), "holes are zero-filled, never the storage's prior bytes");
    }
    require_status(backing.read_page(5, window(out)), ScratchStatus::ok, "written page reads");
    require(out == image, "written bytes round trip");

    // The backing copies on write and on read: neither side aliases the other.
    std::fill(image.begin(), image.end(), std::byte{0x5A});
    require_status(backing.read_page(5, window(out)), ScratchStatus::ok, "reread after source mutation");
    require(out != image, "mutating the source after write does not change the page");
    const auto expected = out;
    std::fill(out.begin(), out.end(), std::byte{0x00});
    std::vector<std::byte> again(page);
    require_status(backing.read_page(5, window(again)), ScratchStatus::ok, "reread after sink mutation");
    require(again == expected, "mutating the destination does not change the page");

    // An overwrite below the extent keeps it; the extent never shrinks.
    const auto low = pattern(page, 2);
    require_status(backing.write_page(2, view(low)), ScratchStatus::ok, "overwrite inside the extent");
    require(backing.page_count() == 6, "extent keeps its high-water mark");
    require_status(backing.read_page(2, window(again)), ScratchStatus::ok, "read the overwritten page");
    require(again == low, "overwrite landed");
    require_status(backing.read_page(3, window(again)), ScratchStatus::ok, "neighbour still a hole");
    require(all_zero(again), "neighbours of an overwrite are untouched");

    // Reads at or past the extent are not_found even inside the capacity;
    // a write past the capacity is a provisioning error and changes nothing.
    require_status(backing.read_page(6, window(again)), ScratchStatus::not_found, "past the extent");
    require_status(backing.write_page(pages, view(low)), ScratchStatus::programming_error,
                   "write past the fixed capacity");
    require(backing.page_count() == 6, "a refused write does not move the extent");
    require_status(backing.write_page(pages - 1, view(low)), ScratchStatus::ok, "last provisioned page");
    require(backing.page_count() == pages, "full extent");
}

void malformed_arguments_and_unserviceable_storage() {
    constexpr std::size_t page = 512;
    std::vector<std::byte> storage(page * 4);
    FixedMemoryScratchBacking backing(ScratchBuffer{storage.data(), storage.size()}, page);
    auto image = pattern(page, 3);
    std::vector<std::byte> out(page);

    require_status(backing.write_page(0, ScratchBytes{image.data(), page - 1}),
                   ScratchStatus::programming_error, "short source page");
    require_status(backing.write_page(0, ScratchBytes{image.data(), page + 1}),
                   ScratchStatus::programming_error, "long source page");
    require_status(backing.write_page(0, ScratchBytes{nullptr, page}),
                   ScratchStatus::programming_error, "null source page");
    require_status(backing.write_page(scratch_no_page, view(image)),
                   ScratchStatus::programming_error, "the sentinel id is not a page");
    require(backing.page_count() == 0, "refused writes leave the extent alone");
    require_status(backing.write_page(0, view(image)), ScratchStatus::ok, "a valid write");
    require_status(backing.read_page(0, ScratchBuffer{out.data(), page - 1}),
                   ScratchStatus::programming_error, "short destination");
    require_status(backing.read_page(0, ScratchBuffer{out.data(), page + 1}),
                   ScratchStatus::programming_error, "long destination");
    require_status(backing.read_page(0, ScratchBuffer{nullptr, page}),
                   ScratchStatus::programming_error, "null destination");
    require_status(backing.read_page(scratch_no_page, window(out)),
                   ScratchStatus::programming_error, "the sentinel id is not readable");

    // Storage that is not a whole number of pages, or a zero page size, is
    // refused as a fixture fault on every operation instead of dividing.
    FixedMemoryScratchBacking ragged(ScratchBuffer{storage.data(), page * 2 + 1}, page);
    require_status(ragged.status(), ScratchStatus::programming_error, "ragged storage");
    require_status(ragged.write_page(0, view(image)), ScratchStatus::programming_error, "ragged write");
    require_status(ragged.read_page(0, window(out)), ScratchStatus::programming_error, "ragged read");
    require(ragged.page_count() == 0 && ragged.capacity_pages() == 2, "ragged extent stays empty");
    FixedMemoryScratchBacking pageless(ScratchBuffer{storage.data(), page}, 0);
    require_status(pageless.status(), ScratchStatus::programming_error, "zero page size");
    require(pageless.capacity_pages() == 0, "zero page size has no capacity");
    require_status(pageless.write_page(0, view(image)), ScratchStatus::programming_error,
                   "zero page size write");
    FixedMemoryScratchBacking empty(ScratchBuffer{nullptr, 0}, page);
    require_status(empty.status(), ScratchStatus::programming_error, "no storage");
}

void durability_is_not_falsely_advertised() {
    constexpr std::size_t page = 512;
    std::vector<std::byte> storage(page * 2);
    FixedMemoryScratchBacking backing(ScratchBuffer{storage.data(), storage.size()}, page);
    require_status(backing.sync(ScratchDurability::buffered), ScratchStatus::ok, "buffered sync");
    require_status(backing.sync(ScratchDurability::data), ScratchStatus::programming_error,
                   "memory is not data-durable");
    require_status(backing.sync(ScratchDurability::full), ScratchStatus::programming_error,
                   "memory is not fully durable");
    require_status(backing.sync(static_cast<ScratchDurability>(77)), ScratchStatus::programming_error,
                   "an unknown durability level is refused before the medium");
}

void one_charge_per_attempted_operation() {
    constexpr std::size_t page = 512;
    std::vector<std::byte> storage(page * 8);
    Charges charges;
    FixedMemoryScratchBacking backing(ScratchBuffer{storage.data(), storage.size()}, page,
                                      charges.observer());
    auto image = pattern(page, 4);
    std::vector<std::byte> out(page);

    (void)backing.write_page(3, view(image));
    (void)backing.read_page(3, window(out));
    (void)backing.read_page(9, window(out));
    (void)backing.write_page(0, ScratchBytes{image.data(), page - 1});
    (void)backing.sync(ScratchDurability::buffered);
    (void)backing.sync(ScratchDurability::data);
    (void)backing.sync(static_cast<ScratchDurability>(77));

    struct Expect {
        ScratchIoOperation operation;
        ScratchPageId page;
        std::uint64_t bytes;
        ScratchStatus status;
    };
    const std::array<Expect, 7> expected{{
        {ScratchIoOperation::write_page, 3, page, ScratchStatus::ok},
        {ScratchIoOperation::read_page, 3, page, ScratchStatus::ok},
        {ScratchIoOperation::read_page, 9, page, ScratchStatus::not_found},
        {ScratchIoOperation::write_page, 0, page - 1, ScratchStatus::programming_error},
        {ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::ok},
        {ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::programming_error},
        {ScratchIoOperation::sync, scratch_no_page, 0, ScratchStatus::programming_error},
    }};
    require(charges.list.size() == expected.size(),
            "exactly one charge per attempt, failed and refused attempts included");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& got = charges.list[i];
        require(got.kind == ScratchBackingKind::test_memory, "the charge names the medium");
        require(got.operation == expected[i].operation, "charge operation");
        require(got.page == expected[i].page, "charge page");
        require(got.bytes == expected[i].bytes, "charge bytes are the REQUESTED bytes");
        require(got.status == expected[i].status, "charge carries the attempt's own status");
    }

    // An observer is optional: the same calls with none are legal and silent.
    FixedMemoryScratchBacking quiet(ScratchBuffer{storage.data(), storage.size()}, page);
    require_status(quiet.write_page(1, view(image)), ScratchStatus::ok, "no observer installed");
}

void medium_kind_is_an_explicit_construction_choice() {
    constexpr std::size_t page = 1024;
    const std::array<ScratchBackingKind, 3> kinds{{ScratchBackingKind::disk, ScratchBackingKind::memfd,
                                                   ScratchBackingKind::tmpfs}};
    const auto image = pattern(page, 5);
    for (const auto kind : kinds) {
        Charges charges;
        ProbeBacking backing(kind, page, charges.observer());
        require(backing.kind() == kind && backing.page_size() == page,
                "kind and page size are what the creator said");
        std::vector<std::byte> out(page);
        require_status(backing.write_page(1, view(image)), ScratchStatus::ok, "probe write");
        require_status(backing.read_page(1, window(out)), ScratchStatus::ok, "probe read");
        require(out == image, "probe round trip");
        require_status(backing.sync(ScratchDurability::full), ScratchStatus::ok, "probe full sync");
        require_status(backing.read_page(2, window(out)), ScratchStatus::not_found, "probe past extent");
        require_status(backing.write_page(4, view(image)), ScratchStatus::programming_error,
                       "probe exhaustion is a programming error, never a capacity code");
        require(charges.list.size() == 5, "five attempts, five charges");
        for (const auto& charge : charges.list)
            require(charge.kind == kind, "every charge carries the chosen medium, not a default");
        require(charges.list[2].operation == ScratchIoOperation::sync
                && charges.list[2].page == scratch_no_page && charges.list[2].bytes == 0,
                "a sync is charged with the sentinel page and zero bytes");
    }
    // The three kinds are distinct values: nothing can collapse them.
    require(kinds[0] != kinds[1] && kinds[1] != kinds[2] && kinds[0] != kinds[2]
            && ScratchBackingKind::test_memory != kinds[0], "four distinct media");
}

void byte_source_reads_exact_ranges() {
    const auto data = pattern(10, 6);
    ScratchSpanSource span(view(data));
    require(span.size() == 10, "size is the span size");

    std::vector<std::byte> out(10);
    require_status(span.read(0, window(out)), ScratchStatus::ok, "whole read");
    require(out == data, "whole read copies every byte");
    std::vector<std::byte> mid(4);
    require_status(span.read(3, window(mid)), ScratchStatus::ok, "inner range");
    require(std::equal(mid.begin(), mid.end(), data.begin() + 3), "inner range bytes");
    require_status(span.read(10, ScratchBuffer{nullptr, 0}), ScratchStatus::ok,
                   "a zero-length read at size() is valid even with no buffer");
    require_status(span.read(5, ScratchBuffer{out.data(), 0}), ScratchStatus::ok, "zero length inside");

    require_status(span.read(11, ScratchBuffer{out.data(), 0}), ScratchStatus::programming_error,
                   "offset past size()");
    std::vector<std::byte> three(3);
    require_status(span.read(8, window(three)), ScratchStatus::programming_error, "range past size()");
    require_status(span.read(0, ScratchBuffer{nullptr, 1}), ScratchStatus::programming_error,
                   "null nonempty destination");

    ScratchSpanSource empty(ScratchBytes{});
    require(empty.size() == 0, "an empty span is a valid empty source");
    require_status(empty.read(0, ScratchBuffer{nullptr, 0}), ScratchStatus::ok, "empty zero read");
    require_status(empty.read(0, ScratchBuffer{out.data(), 1}), ScratchStatus::programming_error,
                   "empty source has no byte to give");
    ScratchSpanSource hollow(ScratchBytes{nullptr, 4});
    require_status(hollow.read(0, ScratchBuffer{out.data(), 2}), ScratchStatus::programming_error,
                   "a span claiming bytes with no data is refused, not dereferenced");
}
} // namespace

int main() {
    try {
        extent_zero_fill_and_isolation();
        malformed_arguments_and_unserviceable_storage();
        durability_is_not_falsely_advertised();
        one_charge_per_attempted_operation();
        medium_kind_is_an_explicit_construction_choice();
        byte_source_reads_exact_ranges();
        std::puts("test_scratch_backing: extent, charges, explicit medium and byte sources");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_backing: %s\n", error.what());
        return 1;
    }
}
