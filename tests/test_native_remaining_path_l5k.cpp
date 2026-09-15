// A35 pure-native witnesses for births on the unconsumed suffix of an OHLC path.
// No generated source host or adapter participates.
#include "native_current_fixture.hpp"

#include <algorithm>
#include <array>

using namespace r4_test;

namespace {

std::vector<no::ExecutionAppliedEvent> fills_for(
        const Host& host, const no::RequestHandle& handle) {
    auto applied = events<no::ExecutionAppliedEvent>(host);
    applied.erase(std::remove_if(applied.begin(), applied.end(),
        [&](const no::ExecutionAppliedEvent& event) {
            return event.handle() != handle;
        }), applied.end());
    return applied;
}

void callback_born_stop_continues_inflight_segment() {
    Host host;
    no::RequestHandle parent;
    no::RequestHandle child;

    host.calculation = [&](Host& current) {
        if (current.calculations != 1) return;
        auto request = tx(-1.0, "mid-segment-parent");
        request.trigger = no::Limit{105.0};
        parent = put(current, request);
    };
    host.notification = [&](Host& current, const no::ExecutionAppliedEvent& event) {
        if (event.handle() != parent || child.incarnation != 0) return;
        auto request = reduce(1.0, "callback-born-stop");
        request.owner = no::BindOpening{parent, event.cycle_after};
        request.trigger = no::Stop{108.0};
        child = put(current, request);
    };

    REQUIRE(host.configure_native(spec("l5k-inflight-suffix")).status
            == NativeSetupStatus::Applied);
    const std::array<Bar, 2> bars{{
        {100.0, 100.0, 100.0, 100.0, 1.0, T},
        // Low-first path: O100 -> L99 -> H110 -> C105. The short parent
        // fills at 105 on the rising segment; its callback-born buy stop must
        // continue that segment from 105 and reach 108 before H110.
        {100.0, 110.0, 99.0, 105.0, 1.0, T + 60000},
    }};
    host.run(bars.data(), static_cast<int>(bars.size()));

    const auto parent_fills = fills_for(host, parent);
    const auto child_fills = fills_for(host, child);
    REQUIRE(parent_fills.size() == 1);
    REQUIRE(child_fills.size() == 1);
    CHECK(parent_fills[0].cursor.point.path_phase == NativePathPhase::High);
    CHECK(child_fills[0].cursor.point.path_phase == NativePathPhase::High);
    CHECK(child_fills[0].cursor.point.ordinal == parent_fills[0].cursor.point.ordinal);
    CHECK(child_fills[0].cursor.t > parent_fills[0].cursor.t);
    near(parent_fills[0].raw_price, 105.0);
    near(child_fills[0].raw_price, 108.0);
    CHECK(parent_fills[0].ordinal < child_fills[0].ordinal);
    CHECK(host.physical_position().signed_units == 0.0);
    completed(host);
}

void waiting_bracket_child_reaches_next_segment() {
    Host host;
    no::RequestHandle parent;
    no::RequestHandle child;

    host.calculation = [&](Host& current) {
        if (current.calculations != 1) return;
        auto opening = tx(1.0, "priced-parent");
        opening.trigger = no::Limit{95.0};
        parent = put(current, opening);

        no::Request bracket{no::Reduce{no::OwnerOpenedUnits{}},
                            "waiting-bracket-child", ""};
        bracket.owner = no::WaitForApplied{parent};
        bracket.trigger = no::Limit{96.0};
        child = put(current, bracket);
    };

    REQUIRE(host.configure_native(spec("l5k-next-segment-child")).status
            == NativeSetupStatus::Applied);
    const std::array<Bar, 2> bars{{
        {100.0, 100.0, 100.0, 100.0, 1.0, T},
        // High-first path: O100 -> H101 -> L90 -> C96. The parent fills at
        // 95 on segment Low; its materialized child reaches 96 on segment Close.
        {100.0, 101.0, 90.0, 96.0, 1.0, T + 60000},
    }};
    host.run(bars.data(), static_cast<int>(bars.size()));

    const auto parent_fills = fills_for(host, parent);
    const auto child_fills = fills_for(host, child);
    REQUIRE(parent_fills.size() == 1);
    REQUIRE(child_fills.size() == 1);
    CHECK(parent_fills[0].cursor.point.path_phase == NativePathPhase::Low);
    CHECK(child_fills[0].cursor.point.path_phase == NativePathPhase::Close);
    CHECK(child_fills[0].cursor.point.ordinal > parent_fills[0].cursor.point.ordinal);
    near(parent_fills[0].raw_price, 95.0);
    near(child_fills[0].raw_price, 96.0);
    CHECK(parent_fills[0].ordinal < child_fills[0].ordinal);
    CHECK(host.physical_position().signed_units == 0.0);
    completed(host);
}

}  // namespace

int main() {
    test("callback-born stop continues in-flight segment",
         callback_born_stop_continues_inflight_segment);
    test("waiting bracket child reaches next segment",
         waiting_bracket_child_reaches_next_segment);
    std::printf("L5k native remaining path: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
