// R4-D L1g generic sampled-point birth-order witness.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19
#error "L1g sampled-point witnesses require the v17 native host surface"
#endif

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                         \
    ++checks;                                                                    \
    if (!(expr)) {                                                               \
        ++failures;                                                              \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                            \
} while (0)

void near(double actual, double expected) {
    CHECK(std::isfinite(actual));
    CHECK(std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected)));
}

NativeRunSpec sampled_spec() {
    NativeRunSpec spec;
    spec.identity = {"sampled-ordinal", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "L1G:ORDINAL";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10'000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    IntrabarPath::synthesized path;
    path.samples = 4;
    path.distribution = MagnifierDistribution::ENDPOINTS;
    spec.intrabar.value = std::move(path);
    return spec;
}

class SampleBornHost final : public NativeStrategyHost {
public:
    std::uint64_t birth_sample_ordinal = 0;
    std::uint64_t stop_acceptance_ordinal = 0;

    void on_native_run_begin() override {
        no::Request entry;
        entry.intent = no::Transact{1.0};
        entry.label = "sample-born-entry";
        CHECK(submit(entry).handle.has_value());
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext& context) override {
        if (event.request().label != "sample-born-entry") return;
        birth_sample_ordinal = context.coordinate.ordinal;
        no::Request stop;
        stop.intent = no::Reduce{no::ExplicitUnits{1.0}};
        stop.trigger = no::Stop{99.0};
        stop.label = "sample-born-stop";
        const auto submitted = submit(stop);
        CHECK(submitted.handle.has_value());
        stop_acceptance_ordinal = submitted.event_ordinal;
    }
};

std::optional<no::ExecutionAppliedEvent> applied(
        const NativeStrategyHost& host, const char* label) {
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            if (event->request().label == label) return *event;
        }
    }
    return std::nullopt;
}

std::optional<no::ActivatedEvent> activation(
        const NativeStrategyHost& host, const char* label) {
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* event = std::get_if<no::ActivatedEvent>(&*row.command)) {
            if (event->definition->request.label == label) return *event;
        }
    }
    return std::nullopt;
}

}  // namespace

int main() {
    SampleBornHost host;
    CHECK(host.configure_native(sampled_spec()).status == NativeSetupStatus::Applied);
    // Sample ordinals are [100, 101, 99, 100].  The applied callback at the
    // first 100 submits the stop.  It is not a same-sample resting retry:
    // the next sample (101) is eligible but does not reach 99, and the later
    // 99 sample activates/fills at the level.  This mirrors the bounded
    // same-point treatment in the legacy magnifier/COOF loops: ordinary
    // sample-born resting requests wait for a strictly later sample.
    const Bar bar{100.0, 101.0, 99.0, 100.0, 50.0, 60'000};
    host.run(&bar, 1, "1", "1", true, 4, MagnifierDistribution::ENDPOINTS);

    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.birth_sample_ordinal != 0);
    CHECK(host.stop_acceptance_ordinal > host.birth_sample_ordinal);
    const auto stop = applied(host, "sample-born-stop");
    const auto activated = activation(host, "sample-born-stop");
    CHECK(stop.has_value());
    CHECK(activated.has_value());
    if (stop) {
        CHECK(stop->cursor.point.ordinal > host.birth_sample_ordinal);
        CHECK(stop->cursor.point.ordinal > host.stop_acceptance_ordinal);
        CHECK(stop->cursor.point.effective_time_ms == 60'000);
        near(stop->raw_price, 99.0);
        near(stop->resolved_price, 99.0);
    }
    if (activated) {
        CHECK(activated->cursor.point.ordinal > host.birth_sample_ordinal);
        near(activated->reached_price, 99.0);
    }
    CHECK(host.trade_count() == 1);
    std::printf("R4-D L1g sampled ordinal: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
