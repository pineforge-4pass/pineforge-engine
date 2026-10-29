// SPDX-License-Identifier: Apache-2.0
// Count the production accumulator's staging of the equity curve for the
// exact-ratio routine, independently of cumulative output bytes and of the
// routine's own history walks: after warmup an input may store at most three
// points (the previous held point, the appended point and the presented tail).
#include "../src/live_report.hpp"
#include "../runner/report.hpp"
#include "live_report_document_oracle.hpp"
#include "native_report_truth_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::live;
using namespace r4_test;

// One previous held point + one appended point + one presented-tail write.
// This bounds logical point stores into reusable ratio storage, excluding
// warmup and allocator relocations (which must remain amortized linear).
constexpr std::uint64_t staging_per_input = 3;

std::vector<Bar> tape(int count) {
    std::uint64_t seed = 0x243f6a8885a308d3ULL;
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const double price = 95.003 + static_cast<double>(seed % 2101) * 0.0137;
        bars.push_back({price, price + 0.31, price - 0.23, price + 0.019, 3.0,
                       1706659200000LL + static_cast<std::int64_t>(i) * 86400000LL});
    }
    return bars;
}

void orders(Host& host) {
    const auto phase = (host.calculations - 1) % 8;
    if (phase == 2) put(host, tx(1.0));
    if (phase == 6 && host.physical_position().signed_units != 0.0)
        put(host, flat());
}

struct BatchReport {
    ReportC value{};
    explicit BatchReport(const BacktestEngine& engine) { engine.fill_report(&value); }
    ~BatchReport() { BacktestEngine::free_report(&value); }
    const pf_report_t& c() const { return *reinterpret_cast<const pf_report_t*>(&value); }
};

void population(int inputs) {
    const auto bars = tape(inputs + 3);
    auto config = l2_fixture::report_spec("live-report-curve-staging");
    config.input_tf = config.script_tf = "1D";
    config.report_policy = NativeReportPolicy::KernelRecorded;
    config.report_open_position_at_end = false;
    config.fee_value = 0.173;
    config.chart_timezone = "America/New_York";

    Host actual, reference;
    actual.calculation = reference.calculation = orders;
    REQUIRE(actual.configure_native(config).status == NativeSetupStatus::Applied);
    REQUIRE(reference.configure_native(config).status == NativeSetupStatus::Applied);
    actual.set_broker_state_hash_recording(true);
    reference.set_broker_state_hash_recording(true);
    REQUIRE(actual.stream_begin(bars.data(), 3, "1D", "1D"));
    REQUIRE(reference.stream_begin(bars.data(), 3, "1D", "1D"));

    LiveReportAccumulator carried; // Default interval 1: no deferred ratios.
    ReportDeltas cumulative(true), batch_deltas(true);
    std::uint64_t warmup_points = 0, staged_at_warmup = 0;
    std::uint64_t previous_points = 0, full_copy_baseline = 0, max_staged = 0;
    std::uint64_t cumulative_bytes = 0, delta_bytes = 0;
    bool saw_closed_trade = false;
    for (int i = 0; i <= inputs; ++i) {
        if (i) {
            REQUIRE(actual.stream_push_bar(bars[static_cast<std::size_t>(i + 2)]));
            REQUIRE(reference.stream_push_bar(bars[static_cast<std::size_t>(i + 2)]));
        }
        const auto hash = actual.stream_state_hash();
        const auto before = carried.work();
        const auto patch = carried.update(actual, static_cast<std::uint64_t>(i));
        REQUIRE(carried.work().fallback_report_reads == 0);
        CHECK(actual.stream_state_hash() == hash);
        CHECK(reference.stream_state_hash() == hash);
        CHECK(!patch.interval_ratios);
        CHECK(carried.work().warmup_ratio_recomputations == 1);
        CHECK(carried.work().ratio_recomputations == static_cast<std::uint64_t>(i));

        if (!i) {
            warmup_points = patch.equity.logical_length;
            REQUIRE(warmup_points > 0);
            staged_at_warmup = carried.work().ratio_points_supplied;
        } else {
            // Establish the population from actual patches, not a mock or an
            // assumed warmup length. Every live input appends exactly one row.
            REQUIRE(patch.equity.logical_length == previous_points + 1);
            REQUIRE(patch.equity.rows.size() == 2);
            full_copy_baseline += patch.equity.logical_length;
            max_staged = std::max(max_staged,
                carried.work().ratio_points_supplied - before.ratio_points_supplied);
        }
        previous_points = patch.equity.logical_length;

        // The batch engine is independent of the accumulator being counted.
        // Keep the exact-ratio and byte contracts even when staging changes.
        const auto staged_before_batch = carried.work().ratio_points_supplied;
        BatchReport batch(reference);
        CHECK(carried.work().ratio_points_supplied == staged_before_batch);
        const auto delta = cumulative.update(patch, "curve-staging", i, hash);
        CHECK(delta == batch_deltas.update(batch.c(), "curve-staging", i, hash));
        const auto full = cumulative.full_json();
        CHECK(full == native_report_document(batch.c(), "curve-staging", i, hash).dump());
        saw_closed_trade |= batch.value.metrics.all.num_trades > 0;
        if (i) {
            cumulative_bytes += static_cast<std::uint64_t>(full.size()) + 1;
            delta_bytes += static_cast<std::uint64_t>(delta.size());
        }
    }

    const auto n = static_cast<std::uint64_t>(inputs);
    const auto staged = carried.work().ratio_points_supplied - staged_at_warmup;
    const auto bound = staging_per_input * n;
    REQUIRE(saw_closed_trade);
    // On today's code staged equals this quadratic baseline. Do not assert
    // that equality: the same row must become green after suffix staging.
    REQUIRE(full_copy_baseline == n * warmup_points + n * (n + 1) / 2);
    std::cout << "curve_staging n=" << n
              << " warmup_points=" << warmup_points
              << " staged_points=" << staged
              << " full_copy_baseline=" << full_copy_baseline
              << " max_staged_per_input=" << max_staged
              << " bound=" << bound
              << " cumulative_document_bytes=" << cumulative_bytes
              << " delta_payload_bytes=" << delta_bytes
              << '\n' << std::flush;
    // Nonfatal checks retain raw count lines for BOTH populations on RED.
    CHECK(staged <= bound);
    CHECK(max_staged <= staging_per_input);
}
} // namespace

int main() {
    r4_test::test("curve staging n=64", [] { population(64); });
    r4_test::test("curve staging n=640", [] { population(640); });
    return r4_test::failures ? 1 : 0;
}
