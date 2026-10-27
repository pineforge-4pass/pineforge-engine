// CR-307 C0182. Expected Release wall time: 2k <5s, 8k <15s,
// 50k file <60s; 50k arena including directory/control rows <180s.
// These are estimates; lab must measure.
// Red on feae33694: compiles using size() as the resident observer, then
// peak=B exceeds 64. Orders end through HOST cancel; book never erases them.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "order_book_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

using namespace r4_test;
using namespace order_book_test;

namespace {
#if __has_include(<pineforge/detail/order_book_storage.hpp>)
// ScratchArenaStats exposes chunk/mapping counts, not directory capacity.
// Observe retained allocations instead, with arena/session setup outside the
// recording window. These literals are predictions, never production limits:
// 256 MiB / 1 MiB * sizeof(uint32_t) = 1,024 bytes at first open AND at B=50k.
struct Recording {
    const std::size_t baseline = global_allocation::live_bytes;
    Recording() {
        REQUIRE(!global_allocation::recording);
        global_allocation::recording = true;
    }
    ~Recording() { global_allocation::recording = false; }
    std::size_t bytes() const { return global_allocation::live_bytes - baseline; }
};

// Keep this witness compilable against the round-1 storage header as well:
// without the new policy, an ordinary table must fail the same byte checks.
template<class T> auto bounded_table(int) -> decltype(T(T::BackingPolicy::bounded)) {
    return T(T::BackingPolicy::bounded);
}
template<class T> T bounded_table(...) { return T{}; }

void resident_directory() {
    Arena arena;
    const detail::OrderBookEntry value{{no::RunIdentity{"cr307-directory", 1}, 7}, false};
    using Store = detail::OrderBookStorage<std::string>;
    Recording recording;
    {
        Store store;
        CHECK(recording.bytes() == 0); // still lazy
        store.put(key(0), value);
        REQUIRE(recording.bytes() == 1024);
        for (std::size_t i = 1; i < 50000; ++i) {
            store.put(key(i), value);
            REQUIRE(recording.bytes() == 1024);
        }
        REQUIRE(store.size() == 50000);
        CHECK(store.resident_size() == 64);
        // Exactly 1+50,000*(56+32+16) = 5,200,001 bytes => five chunks.
        CHECK(detail::ScratchArena::attached()->stats().bytes_total == 268435456);
        CHECK(detail::ScratchArena::attached()->stats().bytes_used == 5242880);
        for (std::size_t i = 50000; i-- != 0;) {
            const auto found = store.find(key(i));
            REQUIRE(found);
            CHECK(found->handle == value.handle);
        }
        CHECK(recording.bytes() == 1024);
        Store moved(std::move(store));
        CHECK(store.size() == 0 && moved.size() == 50000);
        store.put(key(50000), value); // moved-from reuse must reserve again
        CHECK(recording.bytes() == 2048);
        const auto first = moved.find(key(0));
        REQUIRE(first);
        CHECK(first->handle == value.handle);
        {
            Store copied(store); // independent bounded backing on copy
            CHECK(recording.bytes() == 3072);
            const auto found = copied.find(key(50000));
            REQUIRE(found);
            CHECK(found->handle == value.handle);
        }
        CHECK(recording.bytes() == 2048);
        store = std::move(moved); // releases the destination's directory
        CHECK(recording.bytes() == 1024);
        CHECK(store.size() == 50000 && moved.size() == 0);
        moved.put(key(50001), value); // assignment source remains bounded too
        CHECK(recording.bytes() == 2048);
    }
    CHECK(recording.bytes() == 0);
    CHECK(detail::ScratchArena::attached()->stats().bytes_used == 0);

    using Table = detail::DiskIdTable<std::array<std::uint64_t, 4>>;
    {
        auto table = bounded_table<Table>(0);
        table.insert(key(0), {});
        CHECK(recording.bytes() == 1024);
        table.clear();
        CHECK(recording.bytes() == 0);
        table.insert(key(1), {});
        CHECK(recording.bytes() == 1024); // clear retained the opt-in
        Table moved(std::move(table));
        moved.clear();
        CHECK(recording.bytes() == 0);
        moved.insert(key(2), {});
        CHECK(recording.bytes() == 1024); // move construction retained it
        table.insert(key(3), {});
        CHECK(recording.bytes() == 2048); // and kept the source reusable
        Table destination; // deliberately starts with the DEFAULT policy
        destination.insert(key(4), {});
        CHECK(recording.bytes() == 2052);
        destination = std::move(moved);
        CHECK(recording.bytes() == 2048);
        destination.clear();
        CHECK(recording.bytes() == 1024);
        destination.insert(key(5), {});
        CHECK(recording.bytes() == 2048); // assignment replaced its policy
        moved.insert(key(6), {});
        CHECK(recording.bytes() == 3072);
    }
    CHECK(recording.bytes() == 0);

    // Negative control: SAME arena and 32-byte value, default policy unchanged.
    // Its five used chunks grow the directory through capacities 1,2,4,8.
    // A one-key-only measurement would miss this additional retained heap.
    {
        Table ordinary;
        ordinary.insert(key(0), {});
        REQUIRE(recording.bytes() == 4);
        for (std::size_t i = 1; i < 50000; ++i) ordinary.insert(key(i), {});
        CHECK(ordinary.size() == 50000);
        CHECK(recording.bytes() == 32);
        CHECK(detail::ScratchArena::attached()->stats().bytes_used == 5242880);
    }
    CHECK(recording.bytes() == 0);
    std::printf("B=50000 arena directory payload: bounded=1024 default=32 "
                "(default first-key=4); budget=268435456\n");
}
#endif

void growth(std::size_t rounds, bool arena_mode) {
    std::unique_ptr<Arena> arena;
    if (arena_mode) arena = std::make_unique<Arena>();
    Host host;
    Book book(host);
    auto setup = spec("cr307-order-book-growth");
    setup.event_retention = NativeEventRetention::Window;
    REQUIRE(host.configure_native(setup).status == NativeSetupStatus::Applied);
    std::size_t count = 0, peak = 0;
    std::uint64_t first_incarnation = 0;
    const auto request = resting();
    host.calculation = [&](Host& base) {
        const auto out = book.submit_or_replace_outcome(key(count), request);
        REQUIRE(out.action == native_toolkit::OrderBookAction::SubmitAccepted);
        REQUIRE(out.handle());
        if (count == 0) first_incarnation = out.handle()->incarnation;
        CHECK(out.handle()->incarnation == first_incarnation + count);
        peak = std::max(peak, resident(book));
        CHECK(book.size() == count + 1);
        const auto cancelled = base.cancel(*out.handle()); // deliberately bypass book
        REQUIRE(cancelled.status == no::CancelStatus::Cancelled);
        peak = std::max(peak, resident(book));
        CHECK(base.native_working_requests().empty());
        CHECK(book.size() == count + 1);
        ++count;
    };
    // Inputs are fixed before execution; no history of keys/receipts is kept.
    std::vector<Bar> bars;
    bars.reserve(rounds);
    for (std::size_t i = 0; i < rounds; ++i)
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0, T + std::int64_t(i) * 60000});
    host.run(bars.data(), static_cast<int>(bars.size()));
    completed(host);
    REQUIRE(count == rounds);
    CHECK(peak <= kResidentLimit);
    CHECK(resident(book) <= kResidentLimit);
    CHECK(book.size() == rounds);
    CHECK(!book.empty());
    CHECK(host.native_working_requests().empty());
    CHECK(host.rows().size() == 0);
    // Exact full-handle readback, one key at a time, in both access orders.
    // All first-pass rows except at most 64 must come through cold scratch.
    for (std::size_t i = 0; i < rounds; ++i) {
        const no::RequestHandle expected{setup.identity, first_incarnation + i};
        CHECK(book.handle(key(i)) == expected);
    }
    for (std::size_t i = rounds; i-- != 0;) {
        const no::RequestHandle expected{setup.identity, first_incarnation + i};
        CHECK(book.handle(key(i)) == expected);
    }
    CHECK(resident(book) <= kResidentLimit);
    std::printf("B=%zu backend=%s peak=%zu logical=%zu resident=%zu limit=%zu\n",
                rounds, arena_mode ? "arena" : "file", peak, book.size(),
                resident(book), kResidentLimit);
}
}

int main(int argc, char** argv) {
    test("fresh keys ended through host", [&] {
        const std::string scale = argc > 1 ? argv[1] : "2000";
        REQUIRE(scale == "2000" || scale == "8000" || scale == "50000");
        const std::size_t rounds = scale == "2000" ? 2000 : scale == "8000" ? 8000 : 50000;
        REQUIRE(argc <= 3);
        REQUIRE(argc < 3 || std::string(argv[2]) == "arena");
        growth(rounds, argc == 3);
#if __has_include(<pineforge/detail/order_book_storage.hpp>)
        // Run once in the existing 50k arena CTest invocation. The frozen map
        // closure has no storage header; its growth witness above still runs
        // and fails peak<=64 rather than failing to compile on a new API.
        if (rounds == 50000 && argc == 3)
            test("resident directory at 50000 keys", resident_directory);
#endif
    });
    std::printf("OrderBook growth: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
