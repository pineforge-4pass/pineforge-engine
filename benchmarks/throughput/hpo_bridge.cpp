// No storage/metric implementation here: delegate to the shared lane contracts.
#include "hpo_bridge.hpp"

#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_output.hpp>
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<pf_report_summary_v1>);
static_assert(sizeof(pf_report_summary_v1) == 824);
static_assert(offsetof(pf_report_summary_v1, metrics) == 56);
static_assert(sizeof(pf_hpo_scratch_manifest_v1) == 48);
static_assert(sizeof(pf_hpo_scratch_manifest_v1)
              == sizeof(pineforge::detail::HpoScratchManifest));
static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar));
#define HPO_BAR_OFFSET(field) static_assert(offsetof(pf_bar_t, field) == offsetof(pineforge::Bar, field))
HPO_BAR_OFFSET(open); HPO_BAR_OFFSET(high); HPO_BAR_OFFSET(low);
HPO_BAR_OFFSET(close); HPO_BAR_OFFSET(volume); HPO_BAR_OFFSET(timestamp);
#undef HPO_BAR_OFFSET

namespace {
pineforge::BacktestEngine* engine(pf_strategy_t handle) {
    return static_cast<pineforge::BacktestEngine*>(handle);
}
}

extern "C" {

std::uint32_t pf_report_output_version_v1() { return 1; }

int strategy_set_report_output_mode_v1(pf_strategy_t handle, std::uint32_t mode) {
    if (!handle || mode > 2) return -1;
    try {
        pineforge::report::OutputOptionsV1 options;
        options.mode = static_cast<pineforge::report::OutputModeV1>(mode);
        engine(handle)->set_report_output_v1(options);
        return 0;
    } catch (...) {
        return -1;
    }
}

int strategy_set_hpo_scratch_manifest_v1(
        pf_strategy_t handle, const pf_hpo_scratch_manifest_v1* manifest) {
    if (!handle || !manifest || manifest->struct_size != sizeof(*manifest)
        || manifest->version != 1) return -1;
    try {
        const pineforge::detail::HpoScratchManifest request{
            manifest->struct_size, manifest->version,
            manifest->root_bytes, manifest->entry_bytes, manifest->cohort_bytes,
            manifest->transaction_bytes, manifest->resident_bytes};
        pineforge::detail::install_hpo_scratch_manifest_v1(*engine(handle), request);
        return 0;
    } catch (...) {
        return -1;
    }
}

int strategy_hpo_scratch_manifest_v1(
        pf_strategy_t handle, pf_hpo_scratch_manifest_v1* out) {
    if (!handle || !out || out->struct_size != sizeof(*out) || out->version != 1)
        return -1;
    try {
        const auto value = pineforge::detail::installed_hpo_scratch_manifest_v1(*engine(handle));
        const pf_hpo_scratch_manifest_v1 result{
            value.struct_size, value.version, value.root_bytes, value.entry_bytes,
            value.cohort_bytes, value.transaction_bytes, value.resident_bytes};
        std::memcpy(out, &result, sizeof(result));
        return 0;
    } catch (...) {
        return -1;
    }
}

int strategy_run_hpo_v1(pf_strategy_t handle, const pf_bar_t* bars, int count) {
    if (!handle) return -1;
    try {
        // This dedicated entry cannot reach a generated wrapper that insists
        // on a non-NULL report. It borrows the sole shared array through run().
        engine(handle)->run(reinterpret_cast<const pineforge::Bar*>(bars), count);
        return engine(handle)->last_run_status() == 0 && engine(handle)->last_error().empty()
            ? 0 : -1;
    } catch (const std::exception& error) {
        try { pineforge::note_run_failure(*engine(handle), "strategy_run_hpo_v1", error); }
        catch (...) {}
        return -1;
    } catch (...) {
        try { pineforge::note_run_failure_unknown(*engine(handle), "strategy_run_hpo_v1"); }
        catch (...) {}
        return -1;
    }
}

int strategy_report_summary_v1(pf_strategy_t handle, pf_report_summary_v1* out) {
    if (!handle || !out || out->struct_size != sizeof(*out) || out->version != 1)
        return -1;
    try {
        const auto* host = dynamic_cast<const pineforge::NativeStrategyHost*>(engine(handle));
        if (!host || host->native_state().kind != pineforge::NativeLifecycleKind::Completed)
            return -1;
        const auto value = engine(handle)->report_summary_v1();
        if (value.version != 1 || value.mode != pineforge::report::OutputModeV1::Summary
            || value.total_trades > std::uint64_t{std::numeric_limits<std::int64_t>::max()}
            || value.equity_point_count > std::uint64_t{std::numeric_limits<std::int64_t>::max()})
            return -1;
        pf_report_summary_v1 result{};
        result.struct_size = sizeof(result);
        result.version = value.version;
        result.output_mode = static_cast<std::uint32_t>(value.mode);
        result.complete = 1;
        result.total_trades = static_cast<std::int64_t>(value.total_trades);
        result.equity_point_count = static_cast<std::int64_t>(value.equity_point_count);
        result.input_bars_processed = value.input_bars_processed;
        result.script_bars_processed = value.script_bars_processed;
        result.net_profit = value.net_profit;
        // Preserve every metric byte, including NaN payloads and signed zero.
        std::memcpy(&result.metrics, &value.metrics, sizeof(result.metrics));
        std::memcpy(out, &result, sizeof(result));
        return 0;
    } catch (...) {
        return -1;
    }
}

}
