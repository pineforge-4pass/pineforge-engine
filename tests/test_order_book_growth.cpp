// CR-307 C0182. Expected Release wall time: 2k <5s, 8k <15s,
// 50k <60s per invocation (file or arena); estimates, lab must measure.
// Red on feae33694: compiles using size() as the resident observer, then
// peak=B exceeds 64. Orders end through HOST cancel; book never erases them.
#include "order_book_fixture.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace r4_test;
using namespace order_book_test;

namespace {
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
    });
    std::printf("OrderBook growth: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
