// A39 P1-21/P1-22/P1-23/P1-24: pin the accepted generic diagnostics, the
// legacy mutation-refusal order, canonical-only delta refusal, and the active
// native settlement replacement for the deleted compatibility seams.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

using namespace pineforge;
namespace ex = pineforge::execution;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);           \
    }                                                                           \
} while (false)

NativeRunSpec spec_for(const std::string& key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "TEST:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = key;
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10'000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    return spec;
}

class NativeProbe final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    ex::Result settle(const ex::Action& action, double price) {
        ex::PhysicalExecutionContext context;
        context.effective_time_ms = 60'000;
        context.interval_index = 0;
        return settle_native_execution_at(
            action, ex::Fill{price, "active", "native", 7}, context);
    }
};

void accepted_generic_diagnostic_counters() {
    NativeProbe host;
    CHECK(host.configure_native(spec_for("diag-counters")).status
          == NativeSetupStatus::Applied);
    const Bar bars[] = {
        {100.0, 100.0, 100.0, 100.0, 1.0, 60'000},
        {101.0, 101.0, 101.0, 101.0, 1.0, 120'000},
        {102.0, 102.0, 102.0, 102.0, 1.0, 180'000},
    };
    host.run(bars, 3, "1", "1");
    ReportC report{};
    host.fill_report(&report);
    CHECK(report.input_bars_processed == 3);
    CHECK(report.script_bars_processed == 3);
    BacktestEngine::free_report(&report);
}

void realtime_fx_mutation_latches_before_the_api_refusal() {
    NativeProbe host;
    CHECK(host.configure_native(spec_for("fx-refusal-order")).status
          == NativeSetupStatus::Applied);
    const Bar warmup[] = {{100.0, 100.0, 100.0, 100.0, 1.0, 60'000}};
    CHECK(host.stream_begin(warmup, 1, "1", "1"));
    const std::int64_t times[] = {60'000};
    const double rates[] = {1.25};
    bool threw = false;
    try {
        (void)host.set_account_currency_fx_series(times, rates, 1);
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what())
            == "native host refuses source mutation: set_account_currency_fx_series";
    }
    CHECK(threw);
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(host.native_state().failure.code == NativeFailureCode::UnsupportedSource);
}

void active_native_settlement_keeps_the_legacy_literals() {
    NativeProbe host;
    const auto opened = host.settle(order_action::Transact{2.0}, 100.0);
    CHECK(opened.status == ex::Status::Applied);
    CHECK(opened.opened_units == 2.0);
    const auto closed = host.settle(ex::Flatten{}, 110.0);
    CHECK(closed.status == ex::Status::Applied);
    CHECK(closed.closed_units == 2.0);
    CHECK(host.physical_position().signed_units == 0.0);
}

} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "all" || selected == "p1-21")
        accepted_generic_diagnostic_counters();
    if (selected == "all" || selected == "p1-22")
        realtime_fx_mutation_latches_before_the_api_refusal();
    if (selected == "all" || selected == "p1-24")
        active_native_settlement_keeps_the_legacy_literals();
    std::printf("L8c kernel delta rulings: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
