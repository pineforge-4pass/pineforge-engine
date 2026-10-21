// Full-engine RED/GREEN fixture. Against the BASE header closure the same
// fixture runs the existing owning route and fails the output-history/heap
// assertions. It does not pass merely because a new API is unavailable.
#include <pineforge/native_host.hpp>
#if __has_include(<pineforge/report_output.hpp>)
#include <pineforge/report_output.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#endif
#include "cr150_hpo_heap.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kHour = 3600000;
constexpr std::uint64_t kSeed = 1469598103934665603ULL;
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); } } while (0)

std::uint64_t word(std::uint64_t hash, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) {
        hash ^= (value >> shift) & 255;
        hash *= 1099511628211ULL;
    }
    return hash;
}
std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}
std::vector<Bar> feed(int count) {
    std::vector<Bar> result(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + (i % 31) * .25 + (i % 3) * .125;
        result[static_cast<std::size_t>(i)] = {
            price, price + 2.0, price - 1.0, price + (i % 5 - 2) * .125,
            double(i % 101 + 1), kStart + std::int64_t(i) * kHour};
    }
    return result;
}
std::uint64_t input_witness(const std::vector<Bar>& bars) {
    // Independent complete input scan. Host below folds the six named values.
    // Bar's public layout is six 8-byte words and contains no padding.
    static_assert(sizeof(Bar) == 48);
    std::uint64_t result = kSeed;
    for (const auto& bar : bars) {
        std::array<std::uint64_t, 6> values{};
        std::memcpy(values.data(), &bar, sizeof bar);
        for (const auto value : values) result = word(result, value);
    }
    return result;
}

struct Storage {
    std::uint64_t equity = 0, trades = 0, optional = 0;
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    static void observe(void* context, const report::StorageEventV1& event) noexcept {
        auto& self = *static_cast<Storage*>(context);
        if (event.kind == report::StorageKindV1::EquityCurve) ++self.equity;
        else if (event.kind == report::StorageKindV1::TradeArray) ++self.trades;
        else ++self.optional;
    }
#endif
};

class Fixture final : public NativeStrategyHost {
public:
    Fixture(int count, bool trade) : count_(count), trade_(trade) {}
    int calculations = 0;
    std::uint64_t witness = kSeed;
    std::uint64_t sentinels = 0;
    std::uint64_t accepted_commands = 0;
    std::int64_t last_time = 0;
    double indicator = 0.0;

    void on_native_run_begin() override {
        calculations = 0; witness = kSeed; sentinels = 0; accepted_commands = 0;
        last_time = 0; indicator = 0.0;
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        const int index = calculations++;
        witness = word(witness, bits(bar.open));
        witness = word(witness, bits(bar.high));
        witness = word(witness, bits(bar.low));
        witness = word(witness, bits(bar.close));
        witness = word(witness, bits(bar.volume));
        witness = word(witness, static_cast<std::uint64_t>(bar.timestamp));
        indicator = .25 * bar.close + .75 * indicator;
        last_time = bar.timestamp;
        if (index == 0 && bar.high > bar.low) sentinels |= 1;
        if (index == count_ / 2 && bar.volume > 0) sentinels |= 2;
        if (index == count_ - 1 && indicator > 0) sentinels |= 4;
        if (!trade_) return;
        const int phase = index % 16;
        const double side = (index / 16) % 2 ? -1.0 : 1.0;
        native_order::Request request;
        if (phase == 0 || phase == 1) {
            request.intent = native_order::Transact{side * (phase == 0 ? 2.0 : 1.0)};
            request.label = "open";
        } else if (phase == 5) {
            request.intent = native_order::Flatten{};
            request.label = "close";
        } else return;
        if (submit(request).status != native_order::SubmitStatus::Accepted)
            throw std::runtime_error("HPO fixture command was refused");
        ++accepted_commands;
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        sink.s("cr150-hpo-fixture/v1");
        sink.i(count_); sink.b(trade_); sink.i(calculations);
        sink.u(witness); sink.u(sentinels); sink.u(accepted_commands);
        sink.i(last_time); sink.d(indicator);
    }
private:
    int count_;
    bool trade_;
};

NativeRunSpec specification() {
    NativeRunSpec spec;
    spec.identity = {"cr150-hpo-fixture", 1};
    spec.input_tf = "60"; spec.script_tf = "60";
    spec.tickerid = "TEST:HPO"; spec.timezone = "UTC"; spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0; spec.account_fx = 1.0; spec.price_tick = .125;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.fee_kind = NativeFeeKind::CashPerExecution; spec.fee_value = .125;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    spec.event_retention = NativeEventRetention::Window;
    return spec;
}

struct Snapshot {
    pf_metrics_t metrics{};
    double net_profit = 0.0;
    std::uint64_t trades = 0, completed_trades = 0, points = 0;
    std::int64_t input = 0, script = 0;
};

void select_summary(Fixture& host, Storage& storage) {
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    report::OutputOptionsV1 options;
    options.mode = report::OutputModeV1::Summary;
    options.storage_context = &storage;
    options.storage_observer = Storage::observe;
    host.set_report_output_v1(options);
    detail::install_hpo_scratch_manifest_v1(
        host, detail::derive_hpo_scratch_manifest(detail::HpoScratchSpec{}));
#else
    // BASE behavioral control: execute the actual legacy path. The checks
    // below still run and reject its resident history/copies.
    (void)host; (void)storage;
#endif
}
void select_legacy(Fixture& host) {
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    report::OutputOptionsV1 options;
    options.mode = report::OutputModeV1::LegacyOwningArrays;
    host.set_report_output_v1(options);
#else
    (void)host;
#endif
}
Snapshot frozen(Fixture& host, Storage& storage) {
    Snapshot result;
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    const auto value = host.report_summary_v1();
    CHECK(value.mode == report::OutputModeV1::Summary);
    CHECK(value.version == 1);
    result.net_profit = value.net_profit;
    result.trades = value.total_trades;
    result.completed_trades = value.completed_trades;
    result.points = value.equity_point_count;
    result.input = value.input_bars_processed;
    result.script = value.script_bars_processed;
    std::memcpy(&result.metrics, &value.metrics, sizeof result.metrics);
    (void)storage;
#else
    ReportC value{};
    host.fill_report(&value);
    result.net_profit = value.net_profit;
    result.trades = static_cast<std::uint64_t>(value.total_trades);
    result.points = static_cast<std::uint64_t>(value.equity_curve_len);
    result.input = value.input_bars_processed;
    result.script = value.script_bars_processed;
    std::memcpy(&result.metrics, &value.metrics, sizeof result.metrics);
    for (int i = 0; i < value.trades_len; ++i)
        result.completed_trades += !value.trades[i].open_at_end;
    // These real arrays are an observable lower bound on BASE's allocations.
    storage.equity += value.equity_curve != nullptr;
    storage.trades += value.trades != nullptr;
    BacktestEngine::free_report(&value);
#endif
    return result;
}

void complete(const Fixture& host, const std::vector<Bar>& bars) {
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_error().empty());
    CHECK(host.calculations == static_cast<int>(bars.size()));
    CHECK(host.witness == input_witness(bars));
    CHECK(host.sentinels == 7);
    CHECK(host.last_time == bars.back().timestamp);
}

struct ScalarEvidence {
    std::int64_t peak = 0;
    std::array<std::uint64_t, 6> manifest{};
};
ScalarEvidence scalar(int count, bool trade = false) {
    const auto bars = feed(count); // The sole shared-input allowance.
    const auto base = cr150_hpo_heap::live.load();
    cr150_hpo_heap::peak.store(base);
    Fixture host(count, trade);
    Storage storage;
    CHECK(host.configure_native(specification()).status == NativeSetupStatus::Applied);
    select_summary(host, storage);
    host.run(bars.data(), count);
    complete(host, bars);
    const auto result = frozen(host, storage);
    CHECK(result.input == count);
    CHECK(result.script == count);
    CHECK(result.points == static_cast<std::uint64_t>(count));
    CHECK(result.trades == (trade ? std::uint64_t(count / 16) * 2 : 0));
    CHECK(host.accepted_commands == (trade ? std::uint64_t(count / 16) * 3 : 0));
    CHECK(result.metrics.all.num_trades == static_cast<int>(result.trades));
    CHECK(storage.equity == 0);
    CHECK(storage.trades == 0);
    CHECK(storage.optional == 0);
    ScalarEvidence evidence;
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    const auto manifest = detail::installed_hpo_scratch_manifest_v1(host);
    evidence.manifest = {manifest.root_bytes, manifest.entry_bytes, manifest.cohort_bytes,
                         manifest.transaction_bytes, manifest.resident_bytes, manifest.version};
    CHECK(evidence.manifest == (std::array<std::uint64_t, 6>{
        65536, 65536, 65536, 65536, 262144, 1}));
    const auto before = cr150_hpo_heap::allocations.load();
    const auto callbacks = host.calculations;
    const auto again = frozen(host, storage);
    CHECK(cr150_hpo_heap::allocations.load() == before);
    CHECK(host.calculations == callbacks);
    CHECK(std::memcmp(&result.metrics, &again.metrics, sizeof result.metrics) == 0);
    CHECK(bits(result.net_profit) == bits(again.net_profit));
#endif
    evidence.peak = cr150_hpo_heap::peak.load() - base;
    return evidence;
}

void same_trade(const pf_trade_t& row, const TradeC& reference) {
#define SAME(field) CHECK(std::memcmp(&row.field, &reference.field, sizeof row.field) == 0)
    SAME(entry_time); SAME(exit_time); SAME(entry_price); SAME(exit_price);
    SAME(pnl); SAME(pnl_pct); SAME(is_long); SAME(max_runup); SAME(max_drawdown);
    SAME(qty); SAME(commission); SAME(entry_bar_index); SAME(exit_bar_index); SAME(open_at_end);
#undef SAME
}

void identical_trades_and_metrics() {
    constexpr int count = 6008; // 250 civil days, 376 two-lot round trips.
    const auto bars = feed(count);
    Fixture reference(count, true);
    CHECK(reference.configure_native(specification()).status == NativeSetupStatus::Applied);
    select_legacy(reference);
    reference.run(bars.data(), count);
    complete(reference, bars);
    CHECK(reference.accepted_commands == 1128);
    ReportC expected{};
    reference.fill_report(&expected);
    CHECK(expected.total_trades == 752);
    CHECK(expected.trades_len == 752);
    CHECK(expected.equity_curve_len == count);
    bool tied_exits = false;
    for (int i = 1; i < expected.trades_len; ++i)
        tied_exits |= expected.trades[i - 1].exit_time == expected.trades[i].exit_time;
    CHECK(tied_exits);
    CHECK(std::isfinite(expected.metrics.equity.sharpe_monthly));
    CHECK(std::isfinite(expected.metrics.equity.sortino_monthly));

    Fixture summary(count, true);
    Storage storage;
    CHECK(summary.configure_native(specification()).status == NativeSetupStatus::Applied);
    select_summary(summary, storage);
    summary.run(bars.data(), count);
    complete(summary, bars);
    CHECK(summary.accepted_commands == 1128);
    const auto actual = frozen(summary, storage);
    CHECK(actual.trades == 752);
    CHECK(actual.completed_trades == 752);
    CHECK(actual.points == count);
    CHECK(bits(actual.net_profit) == bits(expected.net_profit));
    CHECK(std::memcmp(&actual.metrics, &expected.metrics, sizeof actual.metrics) == 0);
    CHECK(summary.native_continuation_hash() == reference.native_continuation_hash());
    CHECK(summary.broker_state_hash() == reference.broker_state_hash());
#ifdef PINEFORGE_HAS_REPORT_OUTPUT_V1
    const auto& source = summary.completed_trade_source_v1();
    CHECK(source.size() == 752);
    if (source.size() != static_cast<std::uint64_t>(expected.trades_len))
        throw std::runtime_error("HPO completed trade source cardinality differs");
    // A page deliberately splits each pair of tied exit times. No whole
    // result collector is used by the summary consumer.
    std::array<report::TradeRowV1, 3> page{};
    for (std::uint64_t from = 0; from < source.size();) {
        const auto n = source.read_page(from, page.data(), page.size());
        CHECK(n > 0 && n <= page.size() && n <= source.size() - from);
        if (n == 0 || n > page.size() || n > source.size() - from) break;
        for (std::size_t i = 0; i < n; ++i)
            same_trade(page[i].value, expected.trades[from + i]);
        from += n;
    }
#else
    ReportC legacy{};
    summary.fill_report(&legacy);
    CHECK(legacy.trades_len == expected.trades_len);
    for (int i = 0; i < legacy.trades_len && i < expected.trades_len; ++i) {
        pf_trade_t row{};
        static_assert(sizeof row == sizeof(TradeC));
        std::memcpy(&row, &legacy.trades[i], sizeof row);
        same_trade(row, expected.trades[i]);
    }
    BacktestEngine::free_report(&legacy);
#endif
    CHECK(storage.equity == 0);
    CHECK(storage.trades == 0);
    CHECK(storage.optional == 0);
    BacktestEngine::free_report(&expected);
}
}

int main() {
    try {
        (void)scalar(256); // Seed process-global calendar/runtime caches.
        const auto small = scalar(1024);
        const auto large = scalar(262144);
        CHECK(small.manifest == large.manifest);
        // One additional complete fixed cache reservation is conservative
        // slack; it cannot hide BASE's >=24*262144 retained equity bytes.
        CHECK(large.peak <= small.peak + 262144);
        identical_trades_and_metrics();
        // 196608 commands, 131072 trades, 1048576 observations; all exceed
        // the resident windows. A capacity refusal fails complete()/counts.
        const auto few_trades = scalar(1024, true);
        const auto many_trades = scalar(1048576, true);
        CHECK(few_trades.manifest == many_trades.manifest);
        CHECK(many_trades.peak <= few_trades.peak + 262144);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HPO execution fixture: %s\n", error.what());
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
