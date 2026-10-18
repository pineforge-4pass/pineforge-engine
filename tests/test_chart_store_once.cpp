// CR-150 L3: the chart is borrowed for a batch, owned once for stream
// warmup, and never read after the batch call returns. These checks use the
// pre-L3 public surface so the base fails on copies/lifetime, not a new API.
#include <pineforge/native_module.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace chart_heap {
std::atomic<long long> live{0};
std::atomic<long long> peak{0};
constexpr std::size_t kHeader = alignof(std::max_align_t);

void* take(std::size_t n) {
    void* raw = std::malloc(n + kHeader);
    if (!raw) return nullptr;
    *static_cast<std::size_t*>(raw) = n;
    const long long now = live.fetch_add(static_cast<long long>(n))
        + static_cast<long long>(n);
    long long seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
    return static_cast<char*>(raw) + kHeader;
}

void give(void* p) noexcept {
    if (!p) return;
    void* raw = static_cast<char*>(p) - kHeader;
    live.fetch_sub(static_cast<long long>(*static_cast<std::size_t*>(raw)));
    std::free(raw);
}
}  // namespace chart_heap

void* operator new(std::size_t n) {
    if (void* p = chart_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = chart_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return chart_heap::take(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return chart_heap::take(n); }
void operator delete(void* p) noexcept { chart_heap::give(p); }
void operator delete[](void* p) noexcept { chart_heap::give(p); }
void operator delete(void* p, std::size_t) noexcept { chart_heap::give(p); }
void operator delete[](void* p, std::size_t) noexcept { chart_heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { chart_heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { chart_heap::give(p); }

namespace {
using namespace pineforge;
int checks = 0;
int failures = 0;
const char* scenario = "";

#define CHECK(condition) do {                                                       \
    ++checks;                                                                      \
    if (!(condition)) {                                                            \
        ++failures;                                                                \
        std::fprintf(stderr, "%s:%d [%s] %s\n", __FILE__, __LINE__, scenario, #condition); \
    }                                                                              \
} while (0)

constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

std::vector<Bar> chart(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + static_cast<double>(i % 17);
        bars.push_back({p, p + 2.0, p - 2.0, p + 1.0, 10.0 + i,
                        kStart + i * kMinute});
    }
    return bars;
}

void capture_has_no_batch_copy() {
    scenario = "scheduler capture: borrow batch, own stream once";
    const auto bars = chart(200000);
    const long long copy = static_cast<long long>(bars.size() * sizeof(Bar));
    source::PineScheduler scheduler;
    NativeBeginArgs args{};
    args.bars = bars.data();
    args.n = static_cast<int>(bars.size());
    args.input_tf = "1";
    args.script_tf = "1";
    const long long base = chart_heap::live.load();
    chart_heap::peak.store(base);
    scheduler.capture_begin(args);
    const long long batch_peak = chart_heap::peak.load() - base;
    CHECK(scheduler.retained_input().data() == bars.data());
    CHECK(scheduler.retained_input().size() == bars.size());
    CHECK(batch_peak < copy / 8);

    args.is_stream = true;
    args.warmup_n = args.n;
    const long long stream_base = chart_heap::live.load();
    chart_heap::peak.store(stream_base);
    scheduler.capture_begin(args);
    const long long stream_peak = chart_heap::peak.load() - stream_base;
    CHECK(scheduler.retained_input().data() != bars.data());
    CHECK(scheduler.retained_input().size() == bars.size());
    CHECK(stream_peak >= copy);
    CHECK(stream_peak < copy + copy / 8);
    CHECK(scheduler.retained_input().back().close == bars.back().close);
    std::printf("  capture peak: batch %lld, stream %lld, one array %lld bytes\n",
                batch_peak, stream_peak, copy);

    args.is_stream = false;
    args.warmup_n = 0;
    scheduler.capture_begin(args);
    CHECK(scheduler.retained_input().data() == bars.data());
    // Changing from owned to borrowed must also release the old warmup.
    CHECK(chart_heap::live.load() - base < copy / 8);
}

class SourceProbe final : public source::PineStrategyHost {
public:
    std::vector<Bar> seen;
    int stop_at = -1;
    bool throw_at_stop = false;

    SourceProbe() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config;
        config.src_series_active = false;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar& bar) override {
        seen.push_back(bar);
        if (pine_bar_index() != stop_at) return;
        if (throw_at_stop) throw std::runtime_error("L3 callback failure");
        request_abort();
    }
    std::size_t retained_size() const { return scheduler_.retained_input().size(); }
    const Bar* retained_data() const { return scheduler_.retained_input().data(); }
    bool has_next() const { return scheduler_.next_source_bar(0).has_value(); }
    bool has_retained_after() const {
        return scheduler_.retained_input_from(0, kStart).has_value();
    }
    std::optional<Bar> broker_bar_at(std::int64_t timestamp) const {
        NativeDecisionContext context{};
        context.sub_bar_open_ms = timestamp;
        return scheduler_.broker_bar(*this, context);
    }
    std::uint64_t scheduler_hash() const {
        BrokerStateHashSink f;
        scheduler_.hash_state(f);
        return f.h;
    }
};

// Run through each existing overload, including an erased BacktestEngine*.
// Do not hash between the return and the retained-span assertions: cleanup
// must happen at run exit even if no caller ever asks for a hash/report.
void batch_lifetime(int overload, const char* script_tf, bool recording, bool gaps = false) {
    scenario = "batch end: null span, preserved consumed digests";
    SourceProbe pine;
    pine.set_broker_state_hash_recording(recording);
    auto bars = chart(30);
    // Missing input forces a boundary completion on the aggregated path.
    if (gaps) {
        for (std::size_t i = 2; i < bars.size(); ++i) bars[i].timestamp += 4 * kMinute;
    }
    const int n = static_cast<int>(bars.size());
    if (overload == 0) pine.run(bars.data(), n);
    if (overload == 1) pine.run(bars.data(), n, "1", script_tf);
    if (overload == 2) {
        std::unordered_map<std::string, std::string> inputs;
        SymInfo sym;
        sym.timezone = "UTC";
        sym.session = "24x7";
        sym.type = "crypto";
        pine.run(bars.data(), n, "1", script_tf, inputs, sym);
    }
    if (overload == 3) {
        BacktestEngine* erased = &pine;
        erased->run(bars.data(), n, "1", script_tf);
    }
    CHECK(pine.last_error().empty());
    CHECK(pine.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(!pine.seen.empty());
    if (std::string(script_tf) == "1") {
        CHECK(pine.seen.size() == bars.size());
        for (std::size_t i = 0; i < pine.seen.size() && i < bars.size(); ++i) {
            CHECK(pine.seen[i].timestamp == bars[i].timestamp);
            CHECK(pine.seen[i].open == bars[i].open);
            CHECK(pine.seen[i].high == bars[i].high);
            CHECK(pine.seen[i].low == bars[i].low);
            CHECK(pine.seen[i].close == bars[i].close);
            CHECK(pine.seen[i].volume == bars[i].volume);
        }
    }
    CHECK(pine.retained_data() == nullptr);
    CHECK(pine.retained_size() == 0);
    CHECK(!pine.has_next());
    CHECK(!pine.has_retained_after());
    const auto broker_bar = pine.broker_bar_at(kStart);
    CHECK(broker_bar.has_value());
    if (broker_bar && !pine.seen.empty())
        CHECK(broker_bar->timestamp == pine.seen.back().timestamp);
    const auto hash = strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine));
    if (recording) {
        ReportC report{};
        pine.fill_report(&report);
        CHECK(report.broker_state_hash_len > 0);
        if (report.broker_state_hash_len > 0)
            CHECK(hash == report.broker_state_hash[report.broker_state_hash_len - 1]);
        BacktestEngine::free_report(&report);
    }
    const auto scheduler_hash = pine.scheduler_hash();
    for (auto& bar : bars) bar = {};
    std::vector<Bar>().swap(bars);
    CHECK(strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine)) == hash);
    CHECK(pine.scheduler_hash() == scheduler_hash);
    CHECK(!pine.has_next());
    CHECK(!pine.has_retained_after());

    // A fresh begin must replace the span/count and restart every digest.
    const auto next = chart(12);
    pine.seen.clear();
    pine.run(next.data(), static_cast<int>(next.size()), "1", "1");
    CHECK(pine.last_error().empty());
    CHECK(pine.seen.size() == next.size());
    CHECK(pine.seen.back().close == next.back().close);
    CHECK(pine.retained_data() == nullptr);
}

void empty_and_refused_begins() {
    scenario = "empty batch and refusals leave no caller span";
    SourceProbe pine;
    pine.run(nullptr, 0, "1", "1");
    CHECK(pine.last_error().empty());
    CHECK(pine.retained_data() == nullptr);
    CHECK(pine.retained_size() == 0);
    CHECK(!pine.has_next());
    CHECK(!pine.has_retained_after());

    const auto bars = chart(8);
    pine.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(pine.last_error().empty());
    pine.run(nullptr, 2, "1", "1");
    CHECK(!pine.last_error().empty());
    CHECK(pine.retained_data() == nullptr);
    CHECK(pine.retained_size() == 0);

    // This changes the session key on a reused host. The refusal follows
    // capture_begin, before run_begin has reset the source counters.
    pine.set_syminfo_timezone("Etc/UTC");
    pine.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(!pine.last_error().empty());
    CHECK(pine.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(pine.retained_data() == nullptr);
    CHECK(pine.retained_size() == 0);
}

void failed_batch_releases_input(bool throws) {
    scenario = "aborted/failed batch releases its consumed prefix";
    SourceProbe pine;
    pine.stop_at = 3;
    pine.throw_at_stop = throws;
    auto bars = chart(20);
    pine.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(pine.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(!pine.seen.empty() && pine.seen.size() < bars.size());
    CHECK(pine.retained_data() == nullptr);
    CHECK(pine.retained_size() == 0);
    const auto hash = strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine));
    const auto scheduler_hash = pine.scheduler_hash();
    for (auto& bar : bars) bar = {};
    std::vector<Bar>().swap(bars);
    CHECK(strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine)) == hash);
    CHECK(pine.scheduler_hash() == scheduler_hash);
    CHECK(!pine.has_next());
    CHECK(!pine.has_retained_after());
}

// Use a bare native host so its begin callback observes only the wrapper's
// transport: both C entries must pass the original pointer. The source
// scheduler's independent check above proves warmup owns its one copy.
class NativeProbe : public NativeStrategyHost {
public:
    const Bar* expected = nullptr;
    bool same_pointer = false;
    int calls = 0;
    void prepare_native_begin(const NativeBeginArgs& args) override {
        same_pointer = args.bars == expected;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override { ++calls; }
};

NativeRunSpec spec() {
    NativeRunSpec s;
    s.identity = {"chart-store-once", 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.price_tick = 0.01;
    return s;
}

void c_wrappers_pass_the_original_array() {
    scenario = "native module batch and C stream borrow the entry array";
    const auto bars = chart(8);
    std::vector<pf_bar_t> c_bars;
    for (const auto& b : bars)
        c_bars.push_back({b.open, b.high, b.low, b.close, b.volume, b.timestamp});
    for (bool timeframes : {false, true}) {
        native_module::Module<NativeProbe> pine;
        CHECK(pine.configure_native(spec()).status == NativeSetupStatus::Applied);
        pine.expected = reinterpret_cast<const Bar*>(c_bars.data());
        native_module::run_batch<NativeProbe>(static_cast<BacktestEngine*>(&pine),
            c_bars.data(), static_cast<int>(c_bars.size()), "1", "1", timeframes, nullptr);
        CHECK(pine.last_error().empty());
        CHECK(pine.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(pine.same_pointer);
        CHECK(pine.calls == static_cast<int>(bars.size()));
    }
    NativeProbe stream;
    CHECK(stream.configure_native(spec()).status == NativeSetupStatus::Applied);
    stream.expected = reinterpret_cast<const Bar*>(c_bars.data());
    CHECK(strategy_stream_begin(static_cast<BacktestEngine*>(&stream), c_bars.data(),
                                static_cast<int>(c_bars.size()), "1", "1") == 0);
    CHECK(stream.same_pointer);
    CHECK(stream.calls == static_cast<int>(bars.size()));
    CHECK(strategy_stream_end(static_cast<BacktestEngine*>(&stream), 0) == 0);
}

void stream_keeps_owned_warmup() {
    scenario = "stream warmup survives caller release and realtime input";
    SourceProbe pine;
    auto bars = chart(8);
    const Bar last = bars.back();
    CHECK(strategy_stream_begin(static_cast<BacktestEngine*>(&pine),
        reinterpret_cast<const pf_bar_t*>(bars.data()), static_cast<int>(bars.size()),
        "1", "1") == 0);
    CHECK(pine.retained_size() == bars.size());
    CHECK(pine.retained_data() != bars.data());
    const auto hash = strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine));
    for (auto& bar : bars) bar = {};
    std::vector<Bar>().swap(bars);
    CHECK(strategy_broker_state_hash(static_cast<BacktestEngine*>(&pine)) == hash);
    CHECK(pine.retained_data()[7].close == last.close);
    const Bar next{110.0, 112.0, 109.0, 111.0, 30.0, last.timestamp + kMinute};
    CHECK(pine.stream_push_bar(next));
    CHECK(pine.seen.back().close == next.close);
    CHECK(pine.stream_end());
    CHECK(pine.retained_size() == 8);
}
}  // namespace

int main() {
    capture_has_no_batch_copy();
    for (bool recording : {false, true}) {
        for (int overload = 0; overload < 4; ++overload)
            batch_lifetime(overload, "1", recording);
        batch_lifetime(1, "5", recording);
        batch_lifetime(3, "5", recording);
        batch_lifetime(1, "5", recording, true);
    }
    empty_and_refused_begins();
    failed_batch_releases_input(false);
    failed_batch_releases_input(true);
    c_wrappers_pass_the_original_array();
    stream_keeps_owned_warmup();
    std::printf("test_chart_store_once: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
