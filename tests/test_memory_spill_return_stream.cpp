// Packed-return backing growth, replay and retirement are checked.
// Complete the actual implementation-only Fold/Prefix/PackedReturnStream.
// As in test_adapter_tree_ram, include the product unit
// verbatim. Its exported factory supplies the linked engine's real fold.
// No replacement implementation, private-keyword macro or product seam.
#include "../src/engine_metrics.cpp"
#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace memory_spill_return_stream {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace impl = pineforge::metrics::streaming_v1;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct PrefixMember { friend auto access(PrefixMember); };
struct StreamFile { friend auto access(StreamFile); };
struct Pending { friend auto access(Pending); };
struct PendingCount { friend auto access(PendingCount); };
struct Flushed { friend auto access(Flushed); };
template struct Access<PrefixMember, &impl::Fold::prefix_>;
template struct Access<StreamFile, &impl::PackedReturnStream::file_>;
template struct Access<Pending, &impl::PackedReturnStream::pending_>;
template struct Access<PendingCount, &impl::PackedReturnStream::pending_count_>;
template struct Access<Flushed, &impl::PackedReturnStream::flushed_>;

std::size_t checks = 0, failures = 0;
std::string trial_name;
struct PrerequisiteFailure {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [return-stream] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial_name.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) check(bool(expression), #expression, __LINE__)
#define MEM_NEED(expression) do { \
    if (!MEM_CHECK(expression)) throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void trial(const std::string& name, Action action) {
    trial_name = name;
    try { action(); }
    catch (const PrerequisiteFailure&) {} // Already counted; continue trials.
    catch (const std::exception& e) { check(false, e.what(), __LINE__); }
    catch (...) { check(false, "unexpected exception", __LINE__); }
}

constexpr double kCapital = 1000000.0;
constexpr std::int64_t kStart = 1736121600000LL;
// Source arithmetic: PackedReturnStream has one array<double,
// kScratchWindowBytes / sizeof(double)>; report_scratch.hpp sets 64*1024.
// Do not take an expected size from the observed array or its size constant.
constexpr std::size_t kWindowBytes = 64U * 1024U;
constexpr std::size_t kBatchReturns = kWindowBytes / sizeof(double);

NativeRunSpec spec() {
    NativeRunSpec out;
    out.identity = {"return-stream", 1};
    out.input_tf = out.script_tf = "1";
    out.ticker = "MOCK";
    out.tickerid = "TEST:RETURNS";
    out.type = "crypto";
    out.currency = "USD";
    out.timezone = "UTC";
    out.session = "24x7";
    out.initial_capital = kCapital;
    out.point_value = out.account_fx = 1.0;
    out.price_tick = 0.01;
    out.fee_value = 0.0;
    out.slippage_ticks = 0;
    out.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    out.report_policy = NativeReportPolicy::KernelRecordedAtHostMarks;
    out.close_execution = NativeCloseExecution::AfterCalculation;
    return out;
}

std::vector<Bar> tape(std::size_t n) {
    std::vector<Bar> bars;
    bars.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double price = 100.0 + static_cast<double>(i) / 64.0;
        bars.push_back({price, price, price, price, 1.0,
                       kStart + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

class ReturnHost final : public NativeStrategyHost {
public:
    bool expect_fold = true;
    std::size_t callbacks = 0, begins = 0;

    void on_native_run_begin() override {
        ++begins;
        callbacks = 0;
        MEM_CHECK(metrics_streaming_.points == 0);
        if (expect_fold) {
            MEM_NEED(metrics_streaming_.no_curve && metrics_streaming_.fold);
        } else {
            MEM_CHECK(!metrics_streaming_.no_curve);
            // A retained old fold has an incompatible held index. Stop this
            // run before append/amend can abort, AFTER recording the failed
            // packed-return retirement assertion; the harness still runs other trials.
            MEM_NEED(!metrics_streaming_.fold);
        }
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        if (callbacks++ == 0) {
            const auto accepted = submit({no::Transact{1.0}, "return-lot", ""});
            MEM_NEED(accepted.status == no::SubmitStatus::Accepted && accepted.handle);
        }
        MEM_NEED(mark_native_report_point(bar.timestamp));
    }
    auto& state() { return metrics_streaming_; }
    std::size_t open_lots() const { return pyramid_entries_.size(); }
    std::size_t curve_size() const { return equity_curve_.size(); }
};

void prepare(ReturnHost& host, const std::vector<Bar>& bars) {
    using namespace metrics;
    constexpr auto needs = kMetricsNeedSharpeBarV1 | kMetricsNeedSortinoBarV1;
    MEM_NEED(host.set_metrics_streaming_v1(StreamingSharpeV1::On, needs)
            == MetricsStreamingStatusV1::Ok);
    MetricsExtentPlanV1 plan;
    plan.present = 1;
    plan.extent.empty = 0;
    plan.extent.count = static_cast<std::int64_t>(bars.size());
    plan.extent.first_time_ms = bars.front().timestamp;
    plan.extent.last_time_ms = bars.back().timestamp;
    plan.attempt = 7;
    plan.input_generation = 11;
    plan.admitted_view = 13;
    plan.spec_identity = 17;
    MetricsPlainDeclarationV1 declaration;
    declaration.builtin_presenter = declaration.builtin_marks = declaration.builtin_scheduler = 1;
    MetricsPlainInputsV1 inputs;
    inputs.declaration = &declaration;
    inputs.module_contract_bound = inputs.historical_batch = inputs.equal_resolved_timeframes = 1;
    inputs.intrabar_none = inputs.builtin_host_marks = inputs.feed_tolerant_passthrough = 1;
    inputs.final_admitted_view_bound = 1;
    MetricsExtentReceiptV1 receipt;
    MEM_NEED(host.admit_metrics_plain_v1(inputs, plan, &receipt) == MetricsStreamingStatusV1::Ok);
    MEM_NEED(receipt.plain_subset == 1);
    MetricsStreamingConfigV1 config;
    config.streaming_sharpe = StreamingSharpeV1::On;
    config.metrics_needed = needs;
    config.curve_available = 0;
    config.deterministic_replay = 1;
    config.attempt = plan.attempt;
    config.input_generation = plan.input_generation;
    config.admitted_view = plan.admitted_view;
    config.spec_identity = plan.spec_identity;
    MEM_NEED(host.prepare_metrics_streaming_v1(config, plan) == MetricsStreamingStatusV1::Ok);
}

void run_stream(ReturnHost& host, const std::vector<Bar>& bars) {
    MEM_NEED(host.configure_native(spec()).status == NativeSetupStatus::Applied);
    prepare(host, bars);
    host.run(bars.data(), static_cast<int>(bars.size()));
    MEM_NEED(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.last_run_status() == 0 && host.last_error().empty());
    MEM_CHECK(host.callbacks == bars.size());
    MEM_CHECK(host.begins == 1 && host.open_lots() == 1);
    MEM_NEED(host.state().no_curve && host.state().fold);
    MEM_CHECK(host.curve_size() == 1);
    MEM_CHECK(host.state().points == static_cast<std::int64_t>(bars.size()));
}

impl::Prefix& prefix(ReturnHost& host) {
    auto* fold = dynamic_cast<impl::Fold*>(host.state().fold.get());
    MEM_NEED(fold);
    return fold->*access(PrefixMember{});
}

std::uint64_t file_bytes(const report_sink::ScratchFile& file) {
    struct stat info{};
    // Standalone, unattached file-mode fixture: do not substitute an arena
    // occupancy (chunk rounding) for the packed file's exact logical bytes.
    MEM_NEED(file.descriptor() >= 0);
    MEM_NEED(::fstat(file.descriptor(), &info) == 0 && info.st_size >= 0);
    return static_cast<std::uint64_t>(info.st_size);
}

bool same_bytes(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

struct Observation {
    bool complete = false;
    std::uint64_t returns = 0, bytes = 0;
    std::size_t resident_bytes = 0;
};

Observation growth(std::size_t n) {
    ReturnHost host;
    const auto bars = tape(n);
    run_stream(host, bars);
    auto& p = prefix(host);
    auto& stream = p.returns;
    const auto& file = stream.*access(StreamFile{});

    // Independent one-lot mark model: buy one unit at bar 0 close=100,
    // zero fees/slippage, unit point value and FX. Mark i has capital plus
    // close[i]-close[0]. Every previous equity is positive. The last point
    // is held editable, so Prefix contains n-1 marks and n-2 adjacent returns.
    // No production return generator or observed equity supplies this model.
    std::vector<double> expected;
    for (std::size_t i = 1; i + 1 < bars.size(); ++i) {
        const double before = kCapital + (bars[i - 1].close - bars[0].close);
        const double after = kCapital + (bars[i].close - bars[0].close);
        expected.push_back(after / before - 1.0);
    }
    const std::uint64_t count = expected.size();
    const std::uint64_t full_batches = count / kBatchReturns;
    const std::size_t remainder = count % kBatchReturns;
    MEM_CHECK(p.extent.count == static_cast<std::int64_t>(n - 1));
    MEM_CHECK(stream.count() == count && count == n - 2);
    // The pending array's extent is fixed by type.
    MEM_CHECK((stream.*access(Pending{})).size() * sizeof(double) == kWindowBytes);
    // These are BEFORE replay/finish can flush a partial batch. Exact disk
    // size and counters prove the automatic full-batch insert path ran.
    MEM_CHECK(full_batches > 0 && remainder > 0);
    MEM_CHECK(stream.*access(Flushed{}) == full_batches * kBatchReturns);
    MEM_CHECK(stream.*access(PendingCount{}) == remainder);
    MEM_CHECK(file_bytes(file) == full_batches * kWindowBytes);

    std::vector<double> actual;
    stream.replay([&](double value) { actual.push_back(value); });
    MEM_CHECK(actual.size() == count);
    MEM_CHECK(same_bytes(actual, expected));
    MEM_CHECK(stream.count() == count && stream.*access(Flushed{}) == count);
    MEM_CHECK(stream.*access(PendingCount{}) == 0);
    MEM_CHECK(file_bytes(file) == count * sizeof(double));

    // Repeated report reads/replay have no retire site. They neither cap the
    // file nor duplicate a return. The separately emitted held return is
    // counted by the receipt, but does not enter Prefix::returns.
    metrics::MetricsExtentReceiptV1 receipt;
    MEM_CHECK(host.metrics_extent_receipt_v1(&receipt) == metrics::MetricsStreamingStatusV1::Ok);
    MEM_CHECK(receipt.route == metrics::MetricsRatioRouteV1::Streaming);
    MEM_CHECK(receipt.witness == metrics::MetricsExtentWitnessV1::Match);
    MEM_CHECK(receipt.eligible_returns == n - 1);
    std::vector<double> again;
    stream.replay([&](double value) { again.push_back(value); });
    MEM_CHECK(same_bytes(again, expected));
    MEM_CHECK(stream.count() == count);
    MEM_CHECK(file_bytes(file) == count * sizeof(double));
    return {true, stream.count(), file_bytes(file),
            (stream.*access(Pending{})).size() * sizeof(double)};
}

void retirement(std::size_t n) {
    ReturnHost host;
    const auto bars = tape(n);
    run_stream(host, bars);
    auto* old_fold = host.state().fold.get();
    MEM_CHECK(prefix(host).returns.count() == n - 2);
    MEM_CHECK(file_bytes(prefix(host).returns.*access(StreamFile{}))
             == ((n - 2) / kBatchReturns) * kWindowBytes);
    // A consumed PLAIN declaration does not authorize the next run's fold.
    // Reconfigure the real host, but do not admit/prepare a second declaration.
    // Observe the old owner before begin, then its absence inside and after
    // the next begin. No access to the destroyed stream follows that boundary.
    auto next = spec();
    next.identity.run_number = 2;
    host.expect_fold = false;
    MEM_NEED(host.configure_native(next).status == NativeSetupStatus::Applied);
    MEM_CHECK(host.state().fold.get() == old_fold);
    host.run(bars.data(), 3);
    MEM_CHECK(host.begins == 2);
    MEM_CHECK(!host.state().fold);
    MEM_CHECK(!host.state().no_curve);
    MEM_CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.callbacks == 3 && host.curve_size() == 3);
}
} // namespace memory_spill_return_stream

int main() {
    using namespace memory_spill_return_stream;
    constexpr std::size_t n = 9000;
    Observation small, large;
    trial("growth bars=N", [&] { small = growth(n); });
    trial("growth bars=4N", [&] { large = growth(4 * n); });
    trial("compare N/4N: disk grows; resident window stays fixed", [&] {
        MEM_NEED(small.complete && large.complete);
        MEM_CHECK(large.returns - small.returns == 3 * n);
        MEM_CHECK(large.bytes - small.bytes == 3 * n * sizeof(double));
        // The resident window extent is fixed by type.
        MEM_CHECK(small.resident_bytes == kWindowBytes
                 && large.resident_bytes == small.resident_bytes);
    });
    // Independent trials: an I/O failure during growth replay must not
    // prevent the next-begin retirement assertion from being exercised.
    trial("next-begin retirement after N bars", [=] { retirement(n); });
    trial("next-begin retirement after 4N bars", [=] { retirement(4 * n); });
    std::printf("memory proof spill_return_stream: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
