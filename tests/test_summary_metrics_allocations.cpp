// O1W R01/R02 growth regression. On 583c2217d2 this same unit deliberately
// exercises the existing FULL route: both populations execute, then the
// report-byte equality and resident-curve bounds fail. The supervisor must
// record that remote RED before accepting the new mode's GREEN.
#include <pineforge/native_host.hpp>
#include <pineforge/metrics_stream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace allocation_census {
bool enabled = false;
std::size_t bytes = 0;
std::size_t calls = 0;
void record(std::size_t size) {
    if (enabled) { bytes += size; ++calls; }
}
}

void* operator new(std::size_t size) {
    if (void* ptr = std::malloc(size ? size : 1)) {
        allocation_census::record(size);
        return ptr;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<std::size_t>(alignment), size ? size : 1) != 0)
        throw std::bad_alloc();
    allocation_census::record(size);
    return ptr;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { std::free(ptr); }

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

using pineforge::Bar;
using pineforge::ReportC;

std::uint64_t feed_witness(std::uint64_t hash, const Bar& bar) {
    std::uint64_t close_bits;
    std::memcpy(&close_bits, &bar.close, sizeof(close_bits));
    hash ^= static_cast<std::uint64_t>(bar.timestamp);
    hash *= 1099511628211ULL;
    hash ^= close_bits;
    return hash * 1099511628211ULL;
}

class Host final : public pineforge::NativeStrategyHost {
public:
    std::int64_t callbacks = 0, last_timestamp = 0;
    std::uint64_t witness = 1469598103934665603ULL;
    std::size_t peak_capacity = 0;
    bool edit_presentation = false;
    void on_native_run_begin() override {
        callbacks = 0;
        witness = 1469598103934665603ULL;
        peak_capacity = equity_curve_.capacity();
    }
    void on_native_bar(const Bar& bar, const pineforge::NativeDecisionContext&) override {
        ++callbacks;
        last_timestamp = bar.timestamp;
        witness = feed_witness(witness, bar);
        peak_capacity = std::max(peak_capacity, equity_curve_.capacity());
    }
    void present_report(ReportC* report) const override {
        if (edit_presentation && report->equity_curve_len > 0) {
            auto& last = report->equity_curve[report->equity_curve_len - 1];
            last.equity -= 17.25;
            last.open_profit = -17.25;
        }
    }
    std::size_t capacity() const { return equity_curve_.capacity(); }
    bool no_curve_region() const { return report_sink_equity_scratch_region().descriptor == -1; }
};

void configure(Host& host, std::uint64_t run_number) {
    pineforge::NativeRunSpec spec;
    spec.identity = {"o1w-metrics", run_number};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:O1W_METRICS";
    spec.timezone = "America/New_York";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    CHECK(host.configure_native(spec).status == pineforge::NativeSetupStatus::Applied);
}

std::vector<Bar> feed(std::size_t count) {
    std::vector<Bar> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double price = 100.0 + static_cast<double>(i % 19) * 0.125;
        result.push_back({price, price + 1.0, price - 1.0, price + 0.25, 1.0,
                          1704067200000LL + static_cast<std::int64_t>(i) * 60000LL});
    }
    return result;
}

struct Measurement { std::size_t bytes, calls, curve_capacity; };

Measurement run(std::size_t count, bool summary, bool presentation, Host* reuse = nullptr,
                std::uint64_t run_number = 1) {
    // Test-only FULL oracle and shared immutable input are prepared before
    // counting report allocations. No input-copy cost is called a report cost.
    const auto bars = feed(count);
    Host full, fresh;
    Host& candidate = reuse ? *reuse : fresh;
    full.edit_presentation = candidate.edit_presentation = presentation;
    configure(full, 1);
    configure(candidate, run_number);
#ifdef PINEFORGE_HAS_EQUITY_METRICS_FOLD_V1
    CHECK(candidate.set_equity_report_mode(summary ? pineforge::ReportMode::SUMMARY_ONLY
                                                   : pineforge::ReportMode::FULL) == 0);
#endif
    full.run(bars.data(), static_cast<int>(bars.size()));
    candidate.run(bars.data(), static_cast<int>(bars.size()));
    std::uint64_t expected_witness = 1469598103934665603ULL;
    for (const auto& bar : bars) expected_witness = feed_witness(expected_witness, bar);
    // Assert actual population/consumption BEFORE measuring or bounding memory.
    CHECK(candidate.last_run_status() == 0);
    CHECK(candidate.last_error().empty());
    CHECK(candidate.callbacks == static_cast<std::int64_t>(count));
    CHECK(candidate.last_timestamp == bars.back().timestamp);
    CHECK(candidate.witness == expected_witness);

    ReportC oracle{};
    full.fill_report(&oracle);
    CHECK(oracle.script_bars_processed == static_cast<std::int64_t>(count));
    CHECK(oracle.equity_curve_len == static_cast<std::int64_t>(count));
    CHECK(oracle.total_trades == 0);
    CHECK(oracle.equity_curve != nullptr);

    ReportC report{};
    allocation_census::bytes = allocation_census::calls = 0;
    allocation_census::enabled = true;
    candidate.fill_report(&report);
    allocation_census::enabled = false;
    const Measurement measured{allocation_census::bytes, allocation_census::calls,
                               std::max(candidate.peak_capacity, candidate.capacity())};
    CHECK(report.script_bars_processed == static_cast<std::int64_t>(count));
    CHECK(report.total_trades == 0 && report.trades_len == 0 && report.trades == nullptr);
    if (summary) {
        CHECK(report.equity_curve == nullptr && report.equity_curve_len == 0);
        CHECK(candidate.no_curve_region());
        CHECK(measured.curve_capacity <= 1);
    }
#ifdef PINEFORGE_HAS_EQUITY_METRICS_FOLD_V1
    if (summary) {
        using namespace pineforge::metrics;
        CHECK(candidate.equity_logical_point_count() == static_cast<std::int64_t>(count));
        const auto* first = candidate.equity_summary_snapshot();
        CHECK(first != nullptr);
        if (first) {
            CHECK(first->point_count == static_cast<std::int64_t>(count));
            CHECK(first->observation_count == static_cast<std::int64_t>(count));
            CHECK(candidate.equity_summary_ratios_ready() == !first->needs_second_pass());
            EquityMetricsSecondPass second(*first);
            for (std::int64_t i = 0; i < oracle.equity_curve_len; ++i) {
                const auto& point = oracle.equity_curve[i];
                second.fold(point, equity_month_key(point.time_ms, "America/New_York"));
            }
            CHECK(candidate.install_equity_ratio_result(second.finish()));
            Host::free_report(&report);
            report = {};
            candidate.fill_report(&report);
            CHECK(candidate.equity_summary_ratios_ready());
            CHECK(std::memcmp(&report.metrics, &oracle.metrics, 768) == 0);
        }
    } else
#endif
    {
        CHECK(std::memcmp(&report.metrics, &oracle.metrics, 768) == 0);
    }
    Host::free_report(&report);
    Host::free_report(&oracle);
    return measured;
}

Measurement trade_finish(std::int64_t count) {
    using namespace pineforge::metrics;
    TradeStatsAccumulator prefix(TradeFilter::ALL, 10000.0);
    pineforge::TradeC row{};
    row.pnl = 1.25;
    row.pnl_pct = 0.125;
    row.commission = 0.01;
    row.is_long = 1;
    std::int64_t folded = 0;
    allocation_census::bytes = allocation_census::calls = 0;
    allocation_census::enabled = true;
    for (; folded < count; ++folded) prefix.fold(row);
    allocation_census::enabled = false;
    CHECK(folded == count);
    CHECK(prefix.finish().num_trades == count);
    CHECK(allocation_census::bytes == 0 && allocation_census::calls == 0);
    allocation_census::bytes = allocation_census::calls = 0;
    allocation_census::enabled = true;
    auto copy = prefix;
    const auto stats = copy.finish();
    allocation_census::enabled = false;
    const Measurement result{allocation_census::bytes, allocation_census::calls, 0};
    CHECK(stats.num_trades == count);
    return result;
}
} // namespace

int main() {
    const auto small = run(1000, true, false);
    const auto large = run(100000, true, false);
    CHECK(small.bytes == large.bytes);
    CHECK(small.calls == large.calls);
    CHECK(small.curve_capacity == large.curve_capacity);
    const auto full_small = run(1000, false, false);
    const auto full_large = run(100000, false, false);
    CHECK(full_large.bytes >= full_small.bytes + 99000 * sizeof(pf_equity_point_t));
    run(1, true, false);
    run(2, true, true);
    run(1000, true, true);
    Host reused;
    run(100000, false, false, &reused, 1);
    const auto reused_summary = run(1000, true, false, &reused, 2);
    CHECK(reused_summary.curve_capacity == small.curve_capacity);
    const auto trade_small = trade_finish(1000);
    const auto trade_large = trade_finish(100000);
    CHECK(trade_small.bytes == trade_large.bytes && trade_small.calls == trade_large.calls);
    std::printf("summary report allocations: 1000=%zu/%zu, 100000=%zu/%zu; failures=%d\n",
                small.bytes, small.calls, large.bytes, large.calls, failures);
    return failures ? 1 : 0;
}
