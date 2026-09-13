#include <pineforge/native_host.hpp>
#include <pineforge/execution_consumer.hpp>
#include <pineforge/pineforge.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>

#ifndef PINEFORGE_HAS_NATIVE_FX_CURVE_V1
#error "native FX curve C transport feature macro is required"
#endif

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    } \
} while (0)

static_assert(std::is_standard_layout_v<pf_native_fx_curve_v1>);
static_assert(std::is_same_v<decltype(pf_native_fx_curve_v1::struct_size), uint32_t>);
static_assert(std::is_same_v<decltype(pf_native_fx_curve_v1::n), uint32_t>);
static_assert(std::is_same_v<decltype(pf_native_fx_curve_v1::effective_from_ms),
                             const int64_t*>);
static_assert(std::is_same_v<decltype(pf_native_fx_curve_v1::account_per_quote),
                             const double*>);

class FxCurveHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

class LegacyEngine final : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}

    std::uint64_t continuation_hash_for_test() const {
        return execution_consumer().continuation_hash();
    }
};

class NativeButNotHost final : public BacktestEngine {
public:
    NativeButNotHost() : BacktestEngine(NativeConsumerBindTag{}) {}

    void on_bar(const Bar&) override {}

    std::uint64_t continuation_hash_for_test() const {
        return execution_consumer().continuation_hash();
    }
};

NativeRunSpec ready_spec(std::uint64_t run_number = 1) {
    NativeRunSpec spec;
    spec.identity = {"fx-curve-c-transport", run_number};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:FX";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

void check_unchanged(const FxCurveHost& host, NativeLifecycleKind kind,
                     std::uint64_t hash) {
    CHECK(host.native_state().kind == kind);
    CHECK(host.native_continuation_hash() == hash);
}

pf_native_fx_curve_v1 curve(const int64_t* timestamps, const double* rates,
                            std::uint32_t n) {
    return {static_cast<uint32_t>(sizeof(pf_native_fx_curve_v1)), n, timestamps, rates};
}

void c_entry_refusals_and_staging() {
    const int64_t timestamps[] = {0, 1000};
    const double rates[] = {1.0, 1.25};
    const auto valid = curve(timestamps, rates, 2);
    const pf_native_fx_curve_v1 clear = curve(nullptr, nullptr, 0);

    FxCurveHost unconfigured;
    const auto unconfigured_hash = unconfigured.native_continuation_hash();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&unconfigured), &valid) == -1);
    check_unchanged(unconfigured, NativeLifecycleKind::Unconfigured, unconfigured_hash);
    CHECK(strategy_configure_native_fx_curve_v1(nullptr, &valid) == -1);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&unconfigured), nullptr) == -1);
    check_unchanged(unconfigured, NativeLifecycleKind::Unconfigured, unconfigured_hash);

    LegacyEngine legacy;
    CHECK(!legacy.native_bound());
    const auto legacy_hash = legacy.continuation_hash_for_test();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&legacy), &valid) == -1);
    CHECK(!legacy.native_bound());
    CHECK(legacy.continuation_hash_for_test() == legacy_hash);

    NativeButNotHost non_host_native;
    CHECK(non_host_native.native_bound());
    const auto non_host_hash = non_host_native.continuation_hash_for_test();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&non_host_native), &valid) == -1);
    CHECK(non_host_native.native_bound());
    CHECK(non_host_native.continuation_hash_for_test() == non_host_hash);

    FxCurveHost host;
    CHECK(host.configure_native(ready_spec()).status == NativeSetupStatus::Applied);
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
    const auto before = host.native_continuation_hash();

    auto bad_size = valid;
    bad_size.struct_size -= 1;
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &bad_size) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const auto missing_timestamps = curve(nullptr, rates, 1);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &missing_timestamps) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const auto missing_rates = curve(timestamps, nullptr, 1);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &missing_rates) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const NativeFxCurve length_mismatch{{0, 1}, {1.0}};
    const auto mismatch = host.configure_native_fx_curve(length_mismatch);
    CHECK(mismatch.status == NativeSetupStatus::Failed);
    CHECK(mismatch.validation.error == NativeFxCurveError::LengthMismatch);
    CHECK(mismatch.validation.index == 0);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const int64_t repeated[] = {0, 0};
    const auto non_increasing = curve(repeated, rates, 2);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &non_increasing) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const double nonpositive[] = {1.0, 0.0};
    const auto zero_rate = curve(timestamps, nonpositive, 2);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &zero_rate) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    const double nan_rate[] = {1.0, std::numeric_limits<double>::quiet_NaN()};
    const auto nan_curve = curve(timestamps, nan_rate, 2);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &nan_curve) == -1);
    check_unchanged(host, NativeLifecycleKind::Ready, before);

    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &valid) == 0);
    CHECK(host.native_state().kind == NativeLifecycleKind::Ready);
    CHECK(host.native_continuation_hash() != before);
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&host), &clear) == 0);
    check_unchanged(host, NativeLifecycleKind::Ready, before);
}

void ready_only_and_configure_clear_rows() {
    const int64_t timestamps[] = {0, 1000};
    const double rates[] = {1.0, 1.25};
    const auto valid = curve(timestamps, rates, 2);
    const auto clear = curve(nullptr, nullptr, 0);

    FxCurveHost completed;
    CHECK(completed.configure_native(ready_spec()).status == NativeSetupStatus::Applied);
    completed.run(nullptr, 0);
    CHECK(completed.native_state().kind == NativeLifecycleKind::Completed);
    const auto completed_hash = completed.native_continuation_hash();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&completed), &valid) == -1);
    check_unchanged(completed, NativeLifecycleKind::Completed, completed_hash);

    FxCurveHost running;
    CHECK(running.configure_native(ready_spec()).status == NativeSetupStatus::Applied);
    const Bar warmup{100.0, 101.0, 99.0, 100.0, 1.0, 0};
    CHECK(running.stream_begin(&warmup, 1, "1", "1"));
    CHECK(running.native_state().kind == NativeLifecycleKind::Running);
    const auto running_hash = running.native_continuation_hash();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&running), &valid) == -1);
    check_unchanged(running, NativeLifecycleKind::Running, running_hash);
    CHECK(running.stream_end(false));

    FxCurveHost reused;
    CHECK(reused.configure_native(ready_spec()).status == NativeSetupStatus::Applied);
    const auto uncurved_hash = reused.native_continuation_hash();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&reused), &valid) == 0);
    CHECK(reused.native_continuation_hash() != uncurved_hash);
    reused.run(nullptr, 0);
    CHECK(reused.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(reused.configure_native(ready_spec(2)).status == NativeSetupStatus::Applied);
    CHECK(reused.native_state().kind == NativeLifecycleKind::Ready);
    const auto reconfigured_hash = reused.native_continuation_hash();
    CHECK(strategy_configure_native_fx_curve_v1(
              reinterpret_cast<pf_strategy_t>(&reused), &clear) == 0);
    check_unchanged(reused, NativeLifecycleKind::Ready, reconfigured_hash);
}

}  // namespace

int main() {
    c_entry_refusals_and_staging();
    ready_only_and_configure_clear_rows();
    std::printf("%d checks %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
