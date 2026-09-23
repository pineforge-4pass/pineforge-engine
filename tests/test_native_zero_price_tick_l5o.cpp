// A38 pure-native witness: a zero PriceTick is an unquantized run.  The same
// stop and tape retain their raw level at tick zero and snap at a positive
// tick through the generic BacktestEngine price helpers.
#include <pineforge/native_host.hpp>

#include "../src/native_matching.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

constexpr std::int64_t kTime = 1736121600000LL;
constexpr double kRawStop = 99.875;
int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);              \
        }                                                                      \
    } while (0)

struct Host final : NativeStrategyHost {
    no::RequestHandle opening;
    no::RequestHandle stop;

    void on_native_run_begin() override {
        no::Request parent{no::Transact{1.0}, "opening", ""};
        const auto parent_result = submit(parent);
        CHECK(parent_result.status == no::SubmitStatus::Accepted);
        CHECK(parent_result.handle.has_value());
        if (!parent_result.handle) return;
        opening = *parent_result.handle;

        no::Request child{no::Reduce{no::OwnerOpenedUnits{}}, "raw-stop", ""};
        child.owner = no::WaitForApplied{opening};
        child.trigger = no::Stop{kRawStop};
        const auto child_result = submit(child);
        CHECK(child_result.status == no::SubmitStatus::Accepted);
        CHECK(child_result.handle.has_value());
        if (child_result.handle) stop = *child_result.handle;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        double resolved = facts.default_resolved_price;
        if (facts.price_kind == no::NativeCandidatePriceKind::TriggerLevel
            && facts.trigger_level) {
            resolved = native_matching::grid_round_directional(
                level_on_price_grid(*facts.trigger_level), syminfo_mintick_,
                /*up=*/facts.is_buy);
        }
        return {resolved, std::nullopt, no::OpeningShape::Transact};
    }

    double nearest(double value) const { return round_to_mintick(value); }
    double level(double value) const { return level_on_price_grid(value); }
};

NativeRunSpec specification(double tick) {
    NativeRunSpec spec;
    // Reads its whole event record once the run has ended (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    spec.identity = {"l5o-zero-price-tick", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "N";
    spec.tickerid = "TEST:N";
    spec.type = "futures";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "A38 zero price tick";
    spec.volumetype = "contracts";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = tick;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    return spec;
}

std::vector<no::ExecutionAppliedEvent> fills_for(
        const Host& host, const no::RequestHandle& handle) {
    std::vector<no::ExecutionAppliedEvent> result;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*row.command);
        if (applied && applied->handle() == handle) result.push_back(*applied);
    }
    return result;
}

std::uint64_t run_case(double tick, double expected_fill) {
    Host host;
    const auto setup = host.configure_native(specification(tick));
    CHECK(setup.status == NativeSetupStatus::Applied);
    CHECK(setup.validation.ok());
    if (setup.status != NativeSetupStatus::Applied) return host.native_continuation_hash();

    const auto configured_hash = host.native_continuation_hash();

    const Bar tape{100.0, 100.125, 99.625, 99.75, 1.0, kTime};
    host.run(&tape, 1);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.nearest(kRawStop) == (tick == 0.0 ? kRawStop : 100.0));
    CHECK(host.level(kRawStop) == kRawStop);
    const auto fills = fills_for(host, host.stop);
    CHECK(fills.size() == 1);
    if (fills.size() == 1) {
        CHECK(fills[0].raw_price == kRawStop);
        CHECK(fills[0].resolved_price == expected_fill);
    }
    CHECK(host.trade_count() == 1);
    if (host.trade_count() == 1) CHECK(host.get_trade(0).exit_price == expected_fill);
    return configured_hash;
}

}  // namespace

int main() {
    const auto raw_hash = run_case(0.0, kRawStop);
    const auto snapped_hash = run_case(0.25, 99.75);
    CHECK(raw_hash != snapped_hash);

    Host negative_zero;
    const auto negative_setup = negative_zero.configure_native(specification(-0.0));
    CHECK(negative_setup.status == NativeSetupStatus::Applied);
    CHECK(negative_setup.validation.ok());
    if (negative_setup.status == NativeSetupStatus::Applied) {
        CHECK(std::signbit(negative_zero.native_state().spec->price_tick));
        CHECK(raw_hash != negative_zero.native_continuation_hash());
    }

    auto negative = specification(-0.25);
    const auto refusal = validate_native_run_spec(negative);
    CHECK(refusal.error == NativeRunSpecError::NotFinitePositive);
    CHECK(refusal.field == NativeRunSpecField::PriceTick);

    std::printf("A38 native zero price tick: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
