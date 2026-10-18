// CR-150 L16 red witness: ddc9c94c9 has no make_magnifier_path_source.
// The lab must record that first diagnostic before building the implementation.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "native_current_fixture.hpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_bar_views.hpp>
#include <pineforge/source/magnifier_intrabars.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstring>
#include <memory>

namespace {
using namespace r4_test;
using pineforge::source::make_magnifier_path_source;
using pineforge::source::tradingview_magnifier_bars;

std::vector<Bar> minutes(int n, std::int64_t start = T, int stride = 1) {
    std::vector<Bar> result;
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + static_cast<double>(i % 31) * 0.125;
        result.push_back({p, p + 2, p - 1, p + 0.25, 1.0 + i % 7,
                          start + static_cast<std::int64_t>(i) * stride * 60000});
    }
    return result;
}

bool same_bar(const Bar& a, const Bar& b) {
    return std::memcmp(&a.open, &b.open, sizeof(double)) == 0
        && std::memcmp(&a.high, &b.high, sizeof(double)) == 0
        && std::memcmp(&a.low, &b.low, sizeof(double)) == 0
        && std::memcmp(&a.close, &b.close, sizeof(double)) == 0
        && std::memcmp(&a.volume, &b.volume, sizeof(double)) == 0
        && a.timestamp == b.timestamp;
}

std::vector<Bar> walk(const NativeLowerPathSource& source) {
    // Visitor is noexcept: reserve all space before entering it.
    std::vector<Bar> result;
    result.reserve(source.count());
    REQUIRE(source.for_each([](void* p, const Bar& bar) noexcept {
        static_cast<std::vector<Bar>*>(p)->push_back(bar);
        return true;
    }, &result));
    return result;
}

void equal_path(const std::vector<Bar>& feed, const std::string& input,
                const std::string& chart, const std::string& session = "24x7",
                const std::string& timezone = "UTC") {
    const auto vector = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                                 input, chart, session, timezone);
    auto source = make_magnifier_path_source(feed.data(), static_cast<int>(feed.size()),
                                            input, chart, session, timezone);
    REQUIRE(source != nullptr);
    REQUIRE(source->count() == vector.size());
    const auto streamed = walk(*source);
    REQUIRE(streamed.size() == vector.size());
    for (std::size_t i = 0; i < vector.size(); ++i) CHECK(same_bar(streamed[i], vector[i]));
    IntrabarPath::lower_tf lower;
    lower.tf = input;
    IntrabarPath owned, lent;
    lent.value = lower;
    lower.bars = vector;
    owned.value = std::move(lower);
    CHECK(pineforge::native_bar_views_v1::native_intrabar_path_digest(lent, source.get())
          == pineforge::native_run_spec_v3::native_intrabar_path_digest(owned));
    // Invalid fallback input is exercised by whole walks/digests only; the
    // kernel refuses it before a binary-search segment can be selected.
    if (!std::is_sorted(vector.begin(), vector.end(),
            [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; })) return;

    // Independent searches include repeated, overlapping and empty windows.
    for (std::int64_t begin = feed.empty() ? T : feed.front().timestamp;
         begin < (feed.empty() ? T + 60000 : feed.back().timestamp + 1800000);
         begin += 7 * 60000) {
        const auto end = begin + 15 * 60000;
        const auto segment = source->segment(begin, end);
        const auto first = std::lower_bound(vector.begin(), vector.end(), begin,
            [](const Bar& bar, std::int64_t t) { return bar.timestamp < t; });
        const auto last = std::lower_bound(first, vector.end(), end,
            [](const Bar& bar, std::int64_t t) { return bar.timestamp < t; });
        REQUIRE(segment.size == static_cast<std::size_t>(last - first));
        for (std::size_t i = 0; i < segment.size; ++i) CHECK(same_bar(segment.data[i], first[i]));
        const Bar* next = source->next_after_segment();
        CHECK((next == nullptr) == (last == vector.end()));
        if (next && last != vector.end()) CHECK(same_bar(*next, *last));
        CHECK(source->segment(begin, end).data == segment.data);
        const auto again = walk(*source); // must not overwrite either active buffer
        CHECK(again.size() == vector.size());
        CHECK(source->segment(begin, end).data == segment.data);
        CHECK(source->next_after_segment() == next);
        for (std::size_t i = 0; i < segment.size; ++i) CHECK(same_bar(segment.data[i], first[i]));
    }
}

void path_and_window_differential() {
    for (const char* chart : {"1", "5", "10", "15", "30", "60", "240", "1D", "3D", "1W"})
        equal_path(minutes(181), "1", chart);
    equal_path(minutes(180, T, 3), "3", "15"); // grid cannot tile: unchanged
    equal_path({}, "1", "15");
    equal_path(minutes(1, T + 14 * 60000), "1", "15"); // no owned slot: empty path
    equal_path(minutes(80, T, 31), "1", "15"); // missing owners / sparse input
    equal_path(minutes(75), "bad", "15");
    auto unordered = minutes(75);
    unordered.back().timestamp = unordered.front().timestamp;
    equal_path(unordered, "1", "15");
    // Session close truncation, overnight gaps and a late unresolved row:
    // the last case must fall back for the WHOLE input, never just its suffix.
    const auto day = T + 9 * 3600000 + 15 * 60000;
    equal_path(minutes(375, day), "1", "15", "0915-1530");
    auto broken = minutes(375, day);
    broken.push_back(minutes(1, T + 20 * 3600000).front());
    equal_path(broken, "1", "15", "0915-1530");
    equal_path(minutes(390, T + 14 * 3600000 + 30 * 60000), "1", "1D",
               "0930-1600", "America/New_York");
    // Split-session fallback still returns overlap rows in both windows.
    equal_path(minutes(390, T + 9 * 3600000 + 30 * 60000), "1", "60",
               "0930-1200,1300-1600");
    auto raw = minutes(75);
    std::uint64_t nan_bits = UINT64_C(0xfff8000000000042);
    std::memcpy(&raw[0].volume, &nan_bits, sizeof(nan_bits));
    raw[1].volume = -0.0;
    equal_path(raw, "1", "15");

    auto source = make_magnifier_path_source(nullptr, 0, "1", "15", "24x7", "UTC");
    CHECK(source->next_after_segment() == nullptr);
    CHECK(source->segment(T, T).size == 0);
    bool reversed = false, decreasing = false, null_visitor = false;
    try { (void)source->segment(T + 1, T); } catch (const std::logic_error&) { reversed = true; }
    try { (void)source->segment(T - 1, T); } catch (const std::logic_error&) { decreasing = true; }
    try { (void)source->for_each(nullptr, nullptr); } catch (const std::logic_error&) { null_visitor = true; }
    CHECK(reversed && decreasing && null_visitor);
    auto feed = minutes(75);
    source = make_magnifier_path_source(feed.data(), 75, "1", "15", "24x7", "UTC");
    int visits = 0;
    CHECK(!source->for_each([](void* p, const Bar&) noexcept { ++*static_cast<int*>(p); return false; }, &visits));
    CHECK(visits == 1);
    global_allocation::refusing = true;
    const auto cached_count = source->count();
    CHECK(source->count() == cached_count);
    global_allocation::refusing = false;

    const auto active = source->segment(T, T + 15 * 60000);
    const auto* next = source->next_after_segment();
    bool allocation_failed = false;
    global_allocation::refusing = true;
    try { (void)source->segment(T + 15 * 60000, T + 30 * 60000); }
    catch (const std::bad_alloc&) { allocation_failed = true; }
    global_allocation::refusing = false;
    CHECK(allocation_failed);
    CHECK(source->segment(T, T + 15 * 60000).data == active.data);
    CHECK(source->next_after_segment() == next);
    CHECK(source->segment(T + 15 * 60000, T + 30 * 60000).size > 0);

    IntrabarPath path;
    IntrabarPath::lower_tf lower;
    lower.tf = "1";
    path.value = lower;
    struct Nested {
        const IntrabarPath& path;
        const NativeLowerPathSource& source;
        bool visited = false, success = false;
    } nested{path, *source};
    const auto held = source->segment(T + 15 * 60000, T + 30 * 60000);
    REQUIRE(source->for_each([](void* context, const Bar&) noexcept {
        auto& state = *static_cast<Nested*>(context);
        if (!state.visited) {
            state.visited = true;
            try {
                (void)pineforge::native_bar_views_v1::native_intrabar_path_digest(state.path, &state.source);
                state.success = true;
            } catch (...) { return false; }
        }
        return true;
    }, &nested));
    CHECK(nested.visited && nested.success);
    CHECK(source->segment(T + 15 * 60000, T + 30 * 60000).data == held.data);
}

template<class Value> void bytes(std::string& out, const Value& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// Exact report value bytes (including raw pf_metrics_t), without pointers or
// TradeC/ReportC padding. This fixture has no trace/security records.
std::string report_bytes(const BacktestEngine& engine) {
    struct Report {
        ReportC c{};
        ~Report() { BacktestEngine::free_report(&c); }
    } report;
    engine.fill_report(&report.c);
    const auto& r = report.c;
    std::string out;
#define FIELD(name) bytes(out, r.name)
    FIELD(total_trades); FIELD(trades_len); FIELD(net_profit);
    FIELD(input_bars_processed); FIELD(script_bars_processed);
    FIELD(security_feeds_total); FIELD(security_eval_complete_total); FIELD(security_eval_partial_total);
    FIELD(magnifier_sub_bars_total); FIELD(magnifier_sample_ticks_total);
    FIELD(input_tf_seconds); FIELD(script_tf_seconds); FIELD(script_tf_ratio);
    FIELD(needs_aggregation); FIELD(bar_magnifier_enabled);
    FIELD(security_diag_len); FIELD(trace_len); FIELD(trace_names_len);
    FIELD(metrics); FIELD(equity_curve_len); FIELD(broker_state_hash_len);
#undef FIELD
    REQUIRE(r.security_diag_len == 0 && r.trace_len == 0 && r.trace_names_len == 0);
    for (int i = 0; i < r.trades_len; ++i) {
        const auto& t = r.trades[i];
#define FIELD(name) bytes(out, t.name)
        FIELD(entry_time); FIELD(exit_time); FIELD(entry_price); FIELD(exit_price);
        FIELD(pnl); FIELD(pnl_pct); FIELD(is_long); FIELD(max_runup); FIELD(max_drawdown);
        FIELD(qty); FIELD(commission); FIELD(entry_bar_index); FIELD(exit_bar_index); FIELD(open_at_end);
#undef FIELD
    }
    for (std::int64_t i = 0; i < r.equity_curve_len; ++i) {
        bytes(out, r.equity_curve[i].time_ms);
        bytes(out, r.equity_curve[i].equity);
        bytes(out, r.equity_curve[i].open_profit);
    }
    for (std::int64_t i = 0; i < r.broker_state_hash_len; ++i) bytes(out, r.broker_state_hash[i]);
    return out;
}

void execution_differential() {
    auto feed = minutes(150);
    auto s = spec("l16-path");
    s.script_tf = "15";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
    s.report_policy = NativeReportPolicy::KernelRecorded;
    IntrabarPath::lower_tf lower;
    lower.tf = "1";
    lower.sample_eligibility = IntrabarPath::SampleEligibility::ContinuousSegments;
    auto lent = s;
    lent.intrabar.value = lower;
    lower.bars = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                          "1", "15", "24x7", "UTC");
    s.intrabar.value = lower;
    Host vector, provider;
    std::vector<std::uint64_t> vector_hashes, provider_hashes;
    const auto install = [](Host& h, std::vector<std::uint64_t>& hashes) {
        h.calculation = [&hashes](Host& current) {
            put(current, current.calculations % 2 ? tx(1) : flat());
            hashes.push_back(current.native_continuation_hash());
        };
    };
    install(vector, vector_hashes);
    install(provider, provider_hashes);
    auto& consumer = NativeExecutionConsumer::bound(provider);
    consumer.stage_lower_path_source(make_magnifier_path_source(
        feed.data(), static_cast<int>(feed.size()), "1", "15", "24x7", "UTC"));
    REQUIRE(vector.configure_native(s).status == NativeSetupStatus::Applied);
    REQUIRE(provider.configure_native(lent).status == NativeSetupStatus::Applied);
    vector.run(feed.data(), static_cast<int>(feed.size()), "1", "15", true);
    provider.run(feed.data(), static_cast<int>(feed.size()), "1", "15", true);
    REQUIRE(vector.last_error().empty() && provider.last_error().empty());
    CHECK(!vector.rows().empty());
    CHECK(vector_hashes == provider_hashes);
    CHECK(vector.native_continuation_hash() == provider.native_continuation_hash());
    CHECK(report_bytes(vector) == report_bytes(provider));
    CHECK(consumer.bar_views().lower_path == nullptr);
    const auto after = provider.native_continuation_hash();
    feed.clear();
    feed.shrink_to_fit();
    CHECK(provider.native_continuation_hash() == after);
}

void preflight_chunk_boundaries() {
    const auto input = minutes(600);
    for (int mode = 0; mode < 4; ++mode) {
        auto path = input;
        if (mode == 0) path[256].timestamp = path[255].timestamp;
        if (mode == 1) path[255].high = path[255].low - 1;
        if (mode == 2) path[511].volume = -1;
        if (mode == 3) ++path[256].timestamp; // canonical label just past a batch boundary
        auto s = spec("l16-preflight");
        s.script_tf = "15";
        s.slot_label_policy = mode == 3 ? NativeSlotLabelPolicy::Canonical
                                       : NativeSlotLabelPolicy::FeedTolerant;
        s.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        IntrabarPath::lower_tf lower;
        lower.tf = "1";
        auto lent = s;
        lent.intrabar.value = lower;
        lower.bars = path;
        s.intrabar.value = lower;
        Host vector, provider;
        // Chart 10 selects unchanged input, so this is the same deliberately
        // invalid logical path for the provider and owned-vector preflights.
        NativeExecutionConsumer::bound(provider).stage_lower_path_source(
            make_magnifier_path_source(path.data(), static_cast<int>(path.size()),
                                       "1", "10", "24x7", "UTC"));
        REQUIRE(vector.configure_native(s).status == NativeSetupStatus::Applied);
        REQUIRE(provider.configure_native(lent).status == NativeSetupStatus::Applied);
        vector.run(input.data(), static_cast<int>(input.size()), "1", "15", true);
        provider.run(input.data(), static_cast<int>(input.size()), "1", "15", true);
        CHECK(!provider.last_error().empty());
        CHECK(vector.last_error() == provider.last_error());
        CHECK(vector.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(provider.native_state().kind == NativeLifecycleKind::Ready);
        CHECK(vector.calculations == 0 && provider.calculations == 0);
    }
}

void source_host_uses_provider() {
    struct Pine final : source::PineStrategyHost {
        int checks = 0;
        Pine() {
            source::PineStrategyConfig config;
            configure_pine_strategy(config);
            set_syminfo_session("24x7"); set_syminfo_timezone("UTC");
        }
        void on_source_bar(const Bar&) override {
            const auto views = NativeExecutionConsumer::bound(*this).bar_views();
            REQUIRE(views.lower_path != nullptr);
            REQUIRE(native_state().spec->intrabar.lower()->bars.empty());
            CHECK(views.lower_segment.size > 0);
            ++checks;
        }
    } host;
    const auto feed = minutes(75);
    host.run(feed.data(), static_cast<int>(feed.size()), "1", "15", true);
    CHECK(host.last_error().empty()); CHECK(host.checks == 5);
}

std::size_t peak_bytes = 0;
void allocation_peak(std::size_t) { peak_bytes = std::max(peak_bytes, global_allocation::live_bytes); }
std::size_t path_heap(int n) {
    const auto feed = minutes(n); // borrowed input is outside the measured window
    REQUIRE(global_allocation::live_bytes == 0);
    peak_bytes = 0;
    global_allocation::on_recorded = allocation_peak;
    global_allocation::recording = true;
    {
        auto source = make_magnifier_path_source(feed.data(), n, "1", "15", "24x7", "UTC");
        REQUIRE(source->for_each([](void*, const Bar&) noexcept { return true; }, nullptr));
        for (int i = 0; i < n; i += 15) {
            const auto begin = feed[static_cast<std::size_t>(i)].timestamp;
            (void)source->segment(begin, begin + 15 * 60000);
            (void)source->next_after_segment();
        }
    }
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    CHECK(global_allocation::live_bytes == 0);
    return peak_bytes;
}
void bounded_heap() {
    const auto small = path_heap(15000);
    const auto large = path_heap(150000);
    CHECK(large <= small + 64 * 1024);
}
} // namespace

int main() {
    test("L16 path/window differential", path_and_window_differential);
    test("L16 trades, hashes and report bytes", execution_differential);
    test("L16 preflight overlap boundaries", preflight_chunk_boundaries);
    test("L16 source projection wiring", source_host_uses_provider);
    test("L16 bounded provider heap", bounded_heap);
    std::printf("test_magnifier_path_source_l16: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
