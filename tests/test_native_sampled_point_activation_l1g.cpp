// R4-D L1g generic sampled-point activation witness.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#ifndef PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V17
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

NativeRunSpec sampled_spec(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "L1G:SAMPLED";
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

class CrossingHost final : public NativeStrategyHost {
public:
    explicit CrossingHost(bool short_opening) : short_opening_(short_opening) {}

    void on_native_run_begin() override {
        no::Request opening;
        opening.intent = no::Transact{short_opening_ ? -1.0 : 1.0};
        opening.label = short_opening_ ? "short-opening" : "long-opening";
        CHECK(submit(opening).handle.has_value());

        no::Request resting;
        resting.intent = no::Reduce{no::ExplicitUnits{1.0}};
        resting.trigger = short_opening_ ? no::Trigger{no::Limit{95.0}}
                                        : no::Trigger{no::Stop{95.0}};
        resting.label = short_opening_ ? "buy-limit-gap" : "sell-stop-gap";
        CHECK(submit(resting).handle.has_value());
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

private:
    bool short_opening_ = false;
};

void run_crossing_case(bool short_opening, const char* key, const char* label) {
    CrossingHost host(short_opening);
    CHECK(host.configure_native(sampled_spec(key)).status == NativeSetupStatus::Applied);
    // ENDPOINTS samples this as 100.0, 100.5, 94.5, 96.0.  The 94.5 sample
    // is a one-price bar: the long stop and mirrored short limit gap through
    // their 95.0 levels and must retain the 94.5 tick price.
    const Bar bar{100.0, 100.5, 94.5, 96.0, 50.0, 60'000};
    host.run(&bar, 1, "1", "1", true, 4, MagnifierDistribution::ENDPOINTS);

    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    const auto filled = applied(host, label);
    CHECK(filled.has_value());
    if (filled) {
        near(filled->raw_price, 94.5);
        near(filled->resolved_price, 94.5);
        CHECK(filled->cursor.point.path_phase == NativePathPhase::Open);
        CHECK(filled->cursor.point.provenance == NativePriceProvenance::ModeledOHLCOpen);
    }
    if (!short_opening) {
        const auto activated = activation(host, label);
        CHECK(activated.has_value());
        if (activated) near(activated->reached_price, 94.5);
    }
    CHECK(host.trade_count() == 1);
}

}  // namespace

int main() {
    run_crossing_case(false, "sampled-stop", "sell-stop-gap");
    run_crossing_case(true, "sampled-limit", "buy-limit-gap");
    std::printf("R4-D L1g sampled activation: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
