// A-7: test the production decoder against forged logical record contents,
// without corrupting a backing checksum or failing store setup. Including the
// implementation follows test_position_tail_summary_scale's TU idiom and
// reaches the anonymous decoder without adding a production test hook.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/source/pine_strategy_host.cpp"

#include <cstdio>

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
using Header = pineforge::TradeScratchHeader;

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
} } while (false)

std::size_t largest_allocation = 0;
void allocation(std::size_t bytes) {
    largest_allocation = std::max(largest_allocation, bytes);
}

struct Fixture {
    static constexpr std::size_t payload_bytes = 64;
    std::array<std::byte, 64 * 1024> memory{};
    pd::FixedMemoryScratchBacking backing{{memory.data(), memory.size()}, 512};
    pd::CommandScratchStore store{
        {2, 512, 1, 1, {0x5830365830370001ULL, nullptr, compare_trade_scratch},
         pd::ScratchOpenMode::create_empty}, &backing};
    pd::ScratchHandle handle = pd::scratch_no_handle;

    explicit Fixture(const Header& header) {
        CHECK(store.status() == pd::ScratchStatus::ok);
        const pineforge::TradeScratchKey key{1, 0, 0, 0};
        std::array<std::byte, sizeof(Header) + payload_bytes> value{};
        std::memcpy(value.data(), header.data(), sizeof header);
        for (std::size_t i = 0; i < payload_bytes; ++i)
            value[sizeof(Header) + i] = static_cast<std::byte>('a' + i % 26);
        const pd::ScratchSpanSource key_source{
            {reinterpret_cast<const std::byte*>(key.data()), sizeof key}};
        const pd::ScratchSpanSource value_source{{value.data(), value.size()}};
        CHECK(store.begin() == pd::ScratchStatus::ok);
        CHECK(store.put(key_source, value_source, handle) == pd::ScratchStatus::ok);
        CHECK(store.commit(pd::ScratchDurability::buffered) == pd::ScratchStatus::ok);
        pd::ScratchRecordInfo info{};
        CHECK(store.point(handle, info) == pd::ScratchStatus::ok);
        CHECK(info.value_bytes == value.size());
    }
};

void rejected(const Header& header, std::uint64_t forged_bytes) {
    Fixture fixture(header);
    const auto stored = scratch_read_trade(fixture.store, fixture.handle);
    CHECK(stored == header);
    Trade row{};
    bool typed = false;
    largest_allocation = 0;
    global_allocation::on_recorded = allocation;
    global_allocation::recording = true;
    try {
        scratch_trade_text(fixture.store, fixture.handle, stored, row);
    } catch (const RunFailureInfo& error) {
        typed = error.run_failure().code == RunFailureCode::engine_invariant;
    } catch (...) {
        // An uncoded length_error/bad_alloc is the defect, not a valid refusal.
    }
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    CHECK(typed);
    // A decoder must reject the complete header before resizing ANY string.
    CHECK(row.entry_id.empty() && row.entry_comment.empty());
    CHECK(row.exit_comment.empty() && row.exit_id.empty());
    // Exception bookkeeping may allocate; no request may reach the forged
    // string size. This bound comes from the corrupt header, not a measurement.
    CHECK(largest_allocation < forged_bytes);
}

void valid_record() {
    Header header{};
    header[19] = 17;
    header[20] = 13;
    header[21] = 7;
    header[22] = 27;
    Fixture fixture(header);
    Trade row{};
    scratch_trade_text(fixture.store, fixture.handle,
                       scratch_read_trade(fixture.store, fixture.handle), row);
    std::string expected;
    for (std::size_t i = 0; i < Fixture::payload_bytes; ++i)
        expected += static_cast<char>('a' + i % 26);
    CHECK(row.entry_id == expected.substr(0, 17));
    CHECK(row.entry_comment == expected.substr(17, 13));
    CHECK(row.exit_comment == expected.substr(30, 7));
    CHECK(row.exit_id == expected.substr(37, 27));
}
} // namespace

int main() {
    valid_record();
    constexpr std::uint64_t forged = 1024 * 1024;
    for (std::size_t field = 0; field < 4; ++field) {
        Header header{};
        if (field != 0) header[19] = 32;
        header[19 + field] = forged;
        rejected(header, forged);
        header[19 + field] = std::numeric_limits<std::uint64_t>::max();
        rejected(header, forged);
    }
    // Each length fits the payload on its own; their sum does not. A
    // per-field comparison against the original extent is insufficient.
    Header combined{};
    combined[19] = 40;
    combined[20] = 40;
    rejected(combined, forged);
    std::printf("%s test_trade_scratch_header_size_guard (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
