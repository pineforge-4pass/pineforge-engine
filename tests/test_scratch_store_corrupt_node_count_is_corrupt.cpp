// D-01: malformed persisted pages must be refused before traversal or writes.
// Edit-only lane: the supervisor runs this on base, fixed head and mutation.
#include <pineforge/detail/scratch_store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge::detail;
constexpr std::size_t page_bytes = 512;
constexpr std::size_t records = 100; // exceeds the format's 58-handle leaf
constexpr std::uint32_t oversized_count = 0xFFFF;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t le(const std::byte* bytes, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[i])} << (8 * i);
    return value;
}

template<std::size_t N>
void encode(std::byte* bytes, std::uint64_t value) {
    for (std::size_t i = 0; i < N; ++i)
        bytes[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
}

std::array<std::byte, 8> number(std::uint64_t value) {
    std::array<std::byte, 8> result{};
    encode<8>(result.data(), value);
    return result;
}

ScratchStatus numeric_order(void*, const ScratchByteSource& a,
                           const ScratchByteSource& b, int& order) noexcept {
    std::array<std::byte, 8> left{}, right{};
    if (a.size() != left.size() || b.size() != right.size())
        return ScratchStatus::programming_error;
    auto status = a.read(0, {left.data(), left.size()});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != ScratchStatus::ok) return status;
    const auto av = le(left.data(), left.size()), bv = le(right.data(), right.size());
    order = av < bv ? -1 : (av > bv ? 1 : 0);
    return ScratchStatus::ok;
}

struct Fixture {
    // A fixed test backing, separate from the two-page resident cache.
    std::vector<std::byte> bytes = std::vector<std::byte>(1024 * page_bytes);
    FixedMemoryScratchBacking backing{{bytes.data(), bytes.size()}, page_bytes};
    ScratchStoreConfig config{2, page_bytes, 1, 1, {0xD001, nullptr, numeric_order},
                              ScratchOpenMode::create_empty};
    std::array<std::byte, 2 * page_bytes> superblocks{};

    Fixture() {
        require(backing.status() == ScratchStatus::ok, "fixture backing");
        {
            CommandScratchStore store(config, &backing);
            require(store.status() == ScratchStatus::ok, "create fixture");
            require(store.begin() == ScratchStatus::ok, "begin fixture");
            for (std::uint64_t i = 0; i < records; ++i) {
                const auto raw = number(i);
                ScratchSpanSource source({raw.data(), raw.size()});
                ScratchHandle handle = scratch_no_handle;
                require(store.put(source, source, handle) == ScratchStatus::ok, "populate fixture");
                require(handle == i + 1, "fixture issues consecutive handles");
            }
            require(store.commit(ScratchDurability::buffered) == ScratchStatus::ok, "commit fixture");
            require(store.record_count() == records, "fixture population");
        }
        for (std::size_t i = 0; i < superblocks.size(); ++i) superblocks[i] = bytes[i];
        // First commit publishes slot 1. Check its checksum independently,
        // then preserve both complete superblocks while changing a data page.
        const auto* super = bytes.data() + page_bytes;
        std::uint64_t checksum = 14695981039346656037ull;
        for (std::size_t i = 0; i < 18 * 8; ++i) {
            checksum ^= std::to_integer<unsigned char>(super[i]);
            checksum *= 1099511628211ull;
        }
        require(le(super + 18 * 8, 8) == checksum, "fixture has a valid superblock checksum");
        require(le(super + 12 * 8, 8) > 1, "fixture index actually split");
        config.mode = ScratchOpenMode::reopen;
    }

    std::byte* page(std::uint64_t id) {
        require(id >= 2 && id < backing.page_count(), "fixture data page id");
        return bytes.data() + static_cast<std::size_t>(id) * page_bytes;
    }

    std::byte* root(std::size_t field) {
        return page(le(bytes.data() + page_bytes + field * 8, 8));
    }

    std::byte* leftmost(std::size_t root_field, std::size_t height_field) {
        auto* result = root(root_field);
        const auto height = le(bytes.data() + page_bytes + height_field * 8, 8);
        for (std::uint64_t level = height; level > 1; --level)
            result = page(le(result + 16, 8));
        return result;
    }

    void corrupt_count(std::uint32_t type) {
        // Version-1 page types: index leaf/node = 2/3; handle leaf/node = 4/5.
        auto* target = type == 2 ? leftmost(11, 12)
                     : type == 3 ? root(11)
                     : type == 4 ? leftmost(9, 10) : root(9);
        require(le(target, 4) == type, "fixture reaches the intended page type");
        require(le(target + 4, 4) > 0, "fixture node is populated");
        encode<4>(target + 4, oversized_count);
        check_superblocks();
    }

    void check_superblocks() const {
        for (std::size_t i = 0; i < superblocks.size(); ++i)
            require(bytes[i] == superblocks[i], "corruption leaves the superblocks byte-identical");
    }
};

bool count_visit(void* context, const ScratchRecordView&) noexcept {
    ++*static_cast<std::size_t*>(context);
    return true;
}

void index_count_is_corrupt(std::uint32_t type) {
    Fixture fixture;
    fixture.corrupt_count(type);
    {
        CommandScratchStore store(fixture.config, &fixture.backing);
        require(store.status() == ScratchStatus::ok, "reopen accepts the valid superblock");
        std::array<std::byte, page_bytes> page{};
        std::size_t visits = 0;
        require(store.for_each({page.data(), page.size()}, count_visit, &visits)
                    == ScratchStatus::corrupt, "walk refuses oversized index count");
        require(visits == 0, "walk refuses the corrupt node before invoking a visitor");
    }
    {
        CommandScratchStore store(fixture.config, &fixture.backing);
        require(store.status() == ScratchStatus::ok, "reopen before seek");
        const auto raw = number(0); // routes through the leftmost index leaf
        ScratchSpanSource key({raw.data(), raw.size()});
        ScratchHandle result = 123456;
        require(store.lower_bound(key, result) == ScratchStatus::corrupt,
                "seek refuses oversized index count");
        require(result == 123456, "failed seek leaves output unchanged");
    }
    {
        CommandScratchStore store(fixture.config, &fixture.backing);
        require(store.status() == ScratchStatus::ok, "reopen before put");
        require(store.begin() == ScratchStatus::ok, "begin corrupt-count put");
        const auto raw = number(0);
        ScratchSpanSource key({raw.data(), raw.size()});
        ScratchHandle result = 123456;
        require(store.put(key, key, result) == ScratchStatus::corrupt,
                "put preserves the stored-byte corruption verdict");
        require(result == 123456, "failed put leaves output unchanged");
        require(store.commit(ScratchDurability::buffered) == ScratchStatus::programming_error,
                "corrupt-count put makes the transaction rollback-only");
        require(store.rollback() == ScratchStatus::ok, "rollback corrupt-count put");
    }
    {
        CommandScratchStore store(fixture.config, &fixture.backing);
        require(store.status() == ScratchStatus::ok, "reopen before erase");
        require(store.begin() == ScratchStatus::ok, "begin corrupt-count erase");
        require(store.erase(1) == ScratchStatus::corrupt,
                "erase preserves the stored-byte corruption verdict");
        require(store.record_count() == records, "failed erase preserves the population");
        require(store.commit(ScratchDurability::buffered) == ScratchStatus::programming_error,
                "corrupt-count erase makes the transaction rollback-only");
        require(store.rollback() == ScratchStatus::ok, "rollback corrupt-count erase");
    }
    fixture.check_superblocks();
}

void handle_count_is_corrupt(std::uint32_t type) {
    Fixture fixture;
    fixture.corrupt_count(type);
    CommandScratchStore store(fixture.config, &fixture.backing);
    require(store.status() == ScratchStatus::ok, "reopen before handle lookup");
    ScratchRecordInfo info;
    info.handle = 123456;
    require(store.point(1, info) == ScratchStatus::corrupt, "handle lookup refuses oversized count");
    require(info.handle == 123456, "failed lookup leaves output unchanged");
}

void cell_slot_is_corrupt() {
    Fixture fixture;
    auto* entry = fixture.leftmost(9, 10) + 16; // handle 1's entry
    const auto loc = le(entry + 8, 8);
    require(loc != 0 && (loc & 1) == 0, "fixture value uses a cell");
    constexpr std::size_t cells_per_page = (page_bytes - 16 - 16) / 64;
    const auto cell = ((loc >> 1) & ~std::uint64_t{0xFFFF}) | cells_per_page;
    encode<8>(entry + 8, cell << 1); // first impossible slot, even for a short read
    fixture.check_superblocks();
    CommandScratchStore store(fixture.config, &fixture.backing);
    require(store.status() == ScratchStatus::ok, "reopen before bad-cell read");
    std::array<std::byte, 8> output{};
    require(store.read(1, ScratchField::value, 0, {output.data(), output.size()})
                == ScratchStatus::corrupt, "blob read refuses an impossible cell slot");
}

ScratchStatus caller_corrupt(void*, const ScratchByteSource&, const ScratchByteSource&,
                             int&) noexcept {
    return ScratchStatus::corrupt;
}

void caller_failure_stays_programming_error() {
    Fixture fixture;
    fixture.config.order.compare = caller_corrupt;
    CommandScratchStore store(fixture.config, &fixture.backing);
    require(store.status() == ScratchStatus::ok, "reopen before comparator control");
    require(store.begin() == ScratchStatus::ok, "begin comparator control");
    const auto raw = number(0);
    ScratchSpanSource source({raw.data(), raw.size()});
    ScratchHandle result = 123456;
    require(store.put(source, source, result) == ScratchStatus::programming_error,
            "a caller comparator's corrupt status remains a programming error");
    require(result == 123456, "comparator failure leaves output unchanged");
    require(store.rollback() == ScratchStatus::ok, "rollback comparator control");
}
} // namespace

int main() {
    try {
        index_count_is_corrupt(2);
        index_count_is_corrupt(3);
        handle_count_is_corrupt(4);
        handle_count_is_corrupt(5);
        cell_slot_is_corrupt();
        caller_failure_stays_programming_error();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "D-01: %s\n", error.what());
        return 1;
    }
}
