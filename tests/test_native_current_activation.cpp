#include "../src/native_execution_consumer.hpp"
#include "native_current_fixture.hpp"

using namespace r4_test;

namespace {
struct ActivationHost : Host {
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
};

void refused_unchanged(ActivationHost& host, no::RequestHandle handle,
                       NativeCurrentRefusal expected) {
    const auto before = host.native_continuation_hash();
    const auto count = host.native_events(0).size();
    CHECK(host.consumer().activate_current_stop_limit(host, handle) == expected);
    CHECK(host.native_continuation_hash() == before);
    CHECK(host.native_events(0).size() == count);
}

void current_activation(bool prepared, bool buy, NativePriceGrid grid,
                        NativeGridRounding rounding) {
    ActivationHost host;
    host.consumer().set_direct_mutation(!prepared);
    auto settings = spec("current-activation");
    settings.price_grid = grid;
    settings.grid_rounding = rounding;
    // C=100 and tick=.01: an equality stop is reached; one tick farther in
    // the stop direction is not. The limit two ticks away cannot fill here.
    const double sign = buy ? 1 : -1;
    const double stop = 100;
    const double limit = (100/settings.price_tick - sign*2)*settings.price_tick;
    refused_unchanged(host, {}, NativeCurrentRefusal::NoExecutionContext);
    host.calculation = [&](Host& base) {
        auto& h = static_cast<ActivationHost&>(base);
        auto request = tx(sign);
        request.trigger = no::StopLimit{stop, limit};
        const auto target = put(h, request);
        const auto unrelated = put(h, request);
        auto missed = request;
        missed.trigger = no::StopLimit{stop + sign*settings.price_tick, limit};
        const auto unreached = put(h, missed);
        auto pure_limit = request;
        pure_limit.trigger = no::Limit{limit};
        const auto limit_target = put(h, pure_limit);
        auto pure_stop = request;
        pure_stop.trigger = no::Stop{stop};
        const auto stop_target = put(h, pure_stop);
        const auto market_target = put(h, tx(sign));

        refused_unchanged(h, {}, NativeCurrentRefusal::InvalidHandle);
        refused_unchanged(h, limit_target, NativeCurrentRefusal::UnsupportedRequest);
        refused_unchanged(h, stop_target, NativeCurrentRefusal::UnsupportedRequest);
        refused_unchanged(h, market_target, NativeCurrentRefusal::UnsupportedRequest);
        refused_unchanged(h, unreached, NativeCurrentRefusal::UnsupportedRequest);
        const auto pending = h.consumer().request_core().hold_live(unreached);
        REQUIRE(pending);
        CHECK(std::holds_alternative<no::StopLimitPending>(pending->trigger_state));

        const auto before = events<no::ActivatedEvent>(h).size();
        const auto hash_before = h.native_continuation_hash();
        REQUIRE(!h.consumer().activate_current_stop_limit(h, target));
        const auto live = h.consumer().request_core().hold_live(target);
        REQUIRE(live);
        CHECK(std::holds_alternative<no::StopLimitLive>(live->trigger_state));
        const auto other = h.consumer().request_core().hold_live(unrelated);
        REQUIRE(other);
        CHECK(std::holds_alternative<no::StopLimitPending>(other->trigger_state));
        const auto activated = events<no::ActivatedEvent>(h);
        REQUIRE(activated.size() == before + 1);
        CHECK(activated.back().kind == no::ActivationKind::StopLimit);
        CHECK(activated.back().cursor.point.provenance == NativePriceProvenance::CurrentExecution);
        CHECK(activated.back().cursor.point.ordinal > live->birth().acceptance_ordinal);
        CHECK(h.native_continuation_hash() != hash_before);
        CHECK(events<no::ExecutionAppliedEvent>(h).empty());
        CHECK(h.physical_position().signed_units == 0);

        const auto once = h.native_continuation_hash();
        const auto journal = h.native_events(0).size();
        // Exercise the host forwarder too: an already-live target is a no-op.
        CHECK(!h.activate_current_stop_limit(target));
        CHECK(h.native_continuation_hash() == once);
        CHECK(h.native_events(0).size() == journal);
        CHECK(events<no::ActivatedEvent>(h).size() == before + 1);
        for (const auto handle : {target, unrelated, unreached, limit_target,
                                  stop_target, market_target})
            h.cancel(handle);
        refused_unchanged(h, target, NativeCurrentRefusal::NotWorking);
    };
    run(host, settings, {100});
    completed(host);
    CHECK(host.notified.empty());
}

void ordinary_confirmed_close_stays_refused(bool prepared) {
    no::WorkingRequestCore core({"ordinary-close", 1});
    std::uint64_t incarnation = 1, ordinal = 1;
    auto request = tx(1);
    request.trigger = no::StopLimit{100, 99};
    const auto accepted = core.submit(request, 0, incarnation, ordinal);
    REQUIRE(accepted.handle);
    no::MatchCursor cursor;
    cursor.point.ordinal = ordinal++;
    cursor.point.effective_time_ms = 1000;
    cursor.point.provenance = NativePriceProvenance::AfterCalculationClose;
    cursor.point.path_phase = NativePathPhase::Close;
    cursor.point.completion = NativeCompletionKind::Confirmed;
    const auto size = core.history().size();
    const auto next = ordinal;
    const no::ActivateStopLimit hit{cursor, 100};
    if (prepared) {
        const auto result = core.prepare_trigger(*accepted.handle, hit,
            no::DriverEligibilityClass::ConfirmedAfterCalculationClose, ordinal);
        REQUIRE(std::holds_alternative<no::NoChange>(result));
        CHECK(std::get<no::NoChange>(result).reason == no::NoChangeReason::NotEligible);
    } else {
        const auto result = core.apply_trigger(*accepted.handle, hit,
            no::DriverEligibilityClass::ConfirmedAfterCalculationClose, ordinal);
        REQUIRE(std::holds_alternative<no::NoChange>(result));
        CHECK(std::get<no::NoChange>(result).reason == no::NoChangeReason::NotEligible);
    }
    CHECK(core.history().size() == size);
    CHECK(ordinal == next);
    CHECK(std::holds_alternative<no::StopLimitPending>(
        core.find_live(*accepted.handle)->trigger_state));
}

void quantized_reachedness(bool prepared, bool buy, NativePriceGrid grid,
                          NativeGridRounding rounding) {
    ActivationHost host;
    host.consumer().set_direct_mutation(!prepared);
    auto settings = spec("quantized-current-activation");
    settings.price_grid = grid;
    settings.grid_rounding = rounding;
    const double sign = buy ? 1 : -1;
    // Raw C=100+sign*.006 misses stop=100+sign*.01 by .004.
    // Half-up reaches the next tick after .005; directional reaches it
    // after leaving 100. Only the trigger-quantizing mode may activate.
    const double quote = 100 + sign*settings.price_tick*0.6;
    const double stop = (100/settings.price_tick + sign)*settings.price_tick;
    host.calculation = [&](Host& base) {
        auto& h = static_cast<ActivationHost&>(base);
        auto request = tx(sign);
        request.trigger = no::StopLimit{stop, 100 - sign*settings.price_tick*2};
        const auto target = put(h, request);
        if (grid == NativePriceGrid::QuantizeFillsAndTriggers) {
            REQUIRE(!h.consumer().activate_current_stop_limit(h, target));
            const auto row = h.consumer().request_core().hold_live(target);
            REQUIRE(row);
            CHECK(std::holds_alternative<no::StopLimitLive>(row->trigger_state));
            CHECK(events<no::ActivatedEvent>(h).size() == 1);
        } else {
            refused_unchanged(h, target, NativeCurrentRefusal::UnsupportedRequest);
            const auto row = h.consumer().request_core().hold_live(target);
            REQUIRE(row);
            CHECK(std::holds_alternative<no::StopLimitPending>(row->trigger_state));
            CHECK(events<no::ActivatedEvent>(h).empty());
        }
        h.cancel(target);
    };
    run(host, settings, {quote});
    completed(host);
}
} // namespace

int main() {
    for (const bool prepared : {false, true}) {
        for (const bool buy : {false, true})
            for (const auto grid : {NativePriceGrid::None, NativePriceGrid::QuantizeFills,
                                    NativePriceGrid::QuantizeFillsAndTriggers})
                for (const auto rounding : {NativeGridRounding::HalfUp, NativeGridRounding::Directional}) {
                    test("current stop-limit activation", [&] {
                        current_activation(prepared, buy, grid, rounding);
                    });
                    test("current stop-limit grid reachedness", [&] {
                        quantized_reachedness(prepared, buy, grid, rounding);
                    });
                }
        test("ordinary confirmed close refuses activation", [&] {
            ordinary_confirmed_close_stays_refused(prepared);
        });
    }
    return failures ? 1 : 0;
}
