// A-8: default construction into poisoned storage must not leak padding into
// the real raw-byte row codec, store value, or store digest.
#include <pineforge/source/pine_adapter.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;

template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct PerEntryRows { friend auto member(PerEntryRows); };
template struct Access<PerEntryRows, &Adapter::per_entry_exits_>;
using Exits = std::remove_reference_t<
    decltype(std::declval<Adapter&>().*member(PerEntryRows{}))>;
using Exit = Exits::value_type;
using Children = decltype(std::declval<Exit>().children);
using Row = Children::value_type;

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
} } while (false)

struct Fixture {
    std::array<std::byte, 64 * 1024> memory{};
    pd::FixedMemoryScratchBacking backing{{memory.data(), memory.size()}, 512};
    pd::CommandScratchStore store{
        {2, 512, 1, 1, sd::exit_scratch_key_order(),
         pd::ScratchOpenMode::create_empty}, &backing};
    sd::ExitScratchContext context{
        store, {pineforge::native_order::SessionKey{"row-bytes"}, 1}};

    std::uint64_t digest() const {
        std::array<std::byte, 512> page{};
        std::uint64_t result = 0;
        CHECK(store.state_digest({page.data(), page.size()}, result) == pd::ScratchStatus::ok);
        return result;
    }
    std::array<std::byte, sizeof(Row)> value() const {
        pd::ScratchHandle handle = pd::scratch_no_handle;
        CHECK(store.first(handle) == pd::ScratchStatus::ok);
        pd::ScratchRecordInfo info{};
        CHECK(store.point(handle, info) == pd::ScratchStatus::ok);
        CHECK(info.value_bytes == sizeof(Row));
        std::array<std::byte, sizeof(Row)> result{};
        CHECK(store.read(handle, pd::ScratchField::value, 0,
                         {result.data(), result.size()}) == pd::ScratchStatus::ok);
        return result;
    }
};

struct StoredRow {
    alignas(Row) std::array<std::byte, sizeof(Row)> bytes;
    Row* row;
    explicit StoredRow(unsigned char pattern) {
        std::memset(bytes.data(), pattern, bytes.size());
        // No value-initialization that might happen to clear padding. This is
        // the same default-member initialization as an ordinary Row variable.
        row = ::new (static_cast<void*>(bytes.data())) Row;
        row->origin = 17;
        row->fill_price = 100.125;
        row->units = 2.5;
        row->consumed = true;
    }
    ~StoredRow() { row->~Row(); }
};
} // namespace

int main() {
    static_assert(std::is_trivially_copyable_v<Row>, "exercise the raw-byte codec");
    StoredRow poisoned(0xA5);
    StoredRow zeroed(0);
    Fixture a;
    Fixture b;
    CHECK(a.store.status() == pd::ScratchStatus::ok);
    CHECK(b.store.status() == pd::ScratchStatus::ok);
    const auto empty = a.digest();
    CHECK(b.digest() == empty);
    {
        sd::ExitScratchRows<Row> left(a.context);
        sd::ExitScratchRows<Row> right(b.context);
        left.push_back(*poisoned.row);
        right.push_back(*zeroed.row);
        CHECK(left.size() == 1 && right.size() == 1);
        CHECK(a.store.record_count() == 1 && b.store.record_count() == 1);
        CHECK(a.value() == b.value());
        CHECK(a.digest() == b.digest());
        const auto decoded = left.front();
        CHECK(decoded.origin == 17);
        CHECK(decoded.fill_price == 100.125);
        CHECK(decoded.units == 2.5);
        CHECK(decoded.consumed);
    }
    CHECK(a.store.record_count() == 0 && b.store.record_count() == 0);
    std::printf("%s test_exit_scratch_row_bytes (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
