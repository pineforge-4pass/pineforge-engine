// What another symbol's feed costs a Pine host in memory (lane AR-R4). A
// feed's bars are 56 bytes each (a 48-byte Bar and an 8-byte close) and a
// multi-symbol study installs millions of them per strategy handle, so the
// number of copies the host and the kernel hold at once is the figure a
// caller sizes its workers by. Every allocation of this binary goes through
// the replacement operator new below, which counts the live heap and its
// peak; a scenario reads the peak as copies of the installed feed's bytes.
//   1. Installing a feed holds one copy: judging it makes none of its own.
//   2. A run, from its configure to its end, holds at most kRunCopies.
//   3. The same with an inert intrabar path (the magnifier off with a
//      non-default sample count), whose input preflight copies the spec.
//   4. A handle run again after a door (a column installed after the first
//      run) reads exactly what a fresh handle with the same data reads, and
//      holds at most kRerunCopies.
//   5. A configure the kernel refuses (a reused handle's session key
//      changed) leaves the installed feed whole: a column of the feed's own
//      length is still accepted afterwards.
// The values a site reads are checked bar by bar in every scenario, so a
// copy saved can never be a value lost.

#include "native_instrument_feed_fixture.hpp"

#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace live_heap {

std::atomic<long long> live{0};
std::atomic<long long> peak{0};
constexpr std::size_t kHeader = alignof(std::max_align_t);

void note(long long delta) {
    const long long now = live.fetch_add(delta) + delta;
    long long seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
}

void* take(std::size_t n) {
    void* raw = std::malloc(n + kHeader);
    if (raw == nullptr) return nullptr;
    *static_cast<std::size_t*>(raw) = n;
    note(static_cast<long long>(n));
    return static_cast<char*>(raw) + kHeader;
}

void give(void* p) {
    if (p == nullptr) return;
    void* raw = static_cast<char*>(p) - kHeader;
    note(-static_cast<long long>(*static_cast<std::size_t*>(raw)));
    std::free(raw);
}

void restart_peak() { peak.store(live.load()); }

}  // namespace live_heap

void* operator new(std::size_t n) {
    if (void* p = live_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = live_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return live_heap::take(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return live_heap::take(n); }
void operator delete(void* p) noexcept { live_heap::give(p); }
void operator delete[](void* p) noexcept { live_heap::give(p); }
void operator delete(void* p, std::size_t) noexcept { live_heap::give(p); }
void operator delete[](void* p, std::size_t) noexcept { live_heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { live_heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { live_heap::give(p); }

namespace {
using namespace instrument_fixture;

constexpr int kFeedBars = 200000;
constexpr int kChartBars = 64;
// One copy of the feed: kFeedBars bars of 48 bytes and closes of 8.
constexpr double kCopyBytes = 56.0 * kFeedBars;
// A fresh handle: the one copy the kernel's run spec holds, lent by the host.
constexpr double kRunCopies = 1.25;
// A handle run again: the completed run's spec and the store copied back
// from it, one column (8 of 56 bytes) heavier.
constexpr double kRerunCopies = 2.25;

const std::string kKey = "SYN:F";
const std::int64_t kStart = utc_ms(2025, 1, 1);

struct Row {
    double close = na<double>();
    double time = na<double>();
    double column = na<double>();
};

// One request site of another symbol, in the shape codegen emits: the
// payload writes the site's value, the body records it per chart bar.
class OneSite final : public source::PineStrategyHost {
public:
    std::vector<Row> rows;
    Row value;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        value = Row{};
        register_security_eval(0, kKey, "1", input_tf_, false, false, false);
    }
    void evaluate_security(int, const Bar& bar, bool) override {
        value.close = bar.close;
        value.time = static_cast<double>(bar.timestamp);
        value.column = security_column_value(0, "x");
    }
    void clear_security(int) override { value = Row{}; }
    void on_source_bar(const Bar&) override { rows.push_back(value); }
};

struct Feed {
    std::vector<Bar> bars;
    std::vector<std::int64_t> close_ms;
    std::vector<double> column;
};

Feed make_feed() {
    Feed feed;
    feed.bars.reserve(kFeedBars);
    feed.close_ms.reserve(kFeedBars);
    feed.column.reserve(kFeedBars);
    for (int k = 0; k < kFeedBars; ++k) {
        const std::int64_t open = kStart + k * kMinute;
        feed.bars.push_back(make_bar(open, k, 50.0));
        feed.close_ms.push_back(open + kMinute);
        feed.column.push_back(0.5 * k - 7.0);
    }
    return feed;
}

// The chart covers the feed's last kChartBars minutes, so the whole feed is
// handed over (its history on the first chart bar).
std::vector<Bar> make_chart() {
    std::vector<Bar> chart;
    for (int j = 0; j < kChartBars; ++j)
        chart.push_back(make_bar(kStart + (kFeedBars - kChartBars + j) * kMinute, j, 100.0));
    return chart;
}

void prime(OneSite& pine) {
    pine.set_syminfo_timezone("Etc/UTC");
    pine.set_syminfo_session("24x7");
    pine.set_syminfo_type("crypto");
    pine.set_syminfo_string("tickerid", "SYN:BASE");
    pine.set_syminfo_string("ticker", "BASE");
}

bool install(OneSite& pine, const Feed& feed) {
    return pine.set_symbol_feed(kKey, "1", feed.bars.data(), feed.close_ms.data(), kFeedBars);
}

bool install_column(OneSite& pine, const Feed& feed) {
    return pine.set_symbol_feed_column(kKey, "1", "x", feed.column.data(), kFeedBars);
}

bool run(OneSite& pine, const std::vector<Bar>& chart, int samples = 4) {
    pine.rows.clear();
    pine.run(chart.data(), static_cast<int>(chart.size()), "1", "1", false, samples,
             MagnifierDistribution::ENDPOINTS);
    if (!pine.last_error().empty()) std::printf("  run error: %s\n", pine.last_error().c_str());
    return pine.last_error().empty();
}

double copies_since(long long base) {
    return static_cast<double>(live_heap::peak.load() - base) / kCopyBytes;
}

bool same_value(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return a == b;
}

// Chart bar j reads the feed bar that opened with it: that bar closes with
// the chart bar, so it is the last one visible with lookahead off.
void check_rows(const OneSite& pine, const Feed& feed, bool with_column) {
    CHECK(pine.rows.size() == static_cast<std::size_t>(kChartBars));
    if (pine.rows.size() != static_cast<std::size_t>(kChartBars)) return;
    int mismatches = 0;
    for (int j = 0; j < kChartBars; ++j) {
        const std::size_t at = static_cast<std::size_t>(kFeedBars - kChartBars + j);
        const Row& row = pine.rows[static_cast<std::size_t>(j)];
        const bool equal = same_value(row.close, feed.bars[at].close)
            && same_value(row.time, static_cast<double>(feed.bars[at].timestamp))
            && same_value(row.column, with_column ? feed.column[at] : na<double>());
        if (!equal && mismatches++ < 3)
            std::printf("  chart bar %d: close %.6g time %.0f column %.6g\n", j, row.close,
                        row.time, row.column);
        CHECK(equal);
    }
}

void install_and_run() {
    scenario = "copies: install, then a run";
    const Feed feed = make_feed();
    const std::vector<Bar> chart = make_chart();
    OneSite pine;
    prime(pine);
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install(pine, feed));
    const double held = static_cast<double>(live_heap::live.load() - base) / kCopyBytes;
    const double install_peak = copies_since(base);
    live_heap::restart_peak();
    CHECK(run(pine, chart));
    const double run_peak = copies_since(base);
    std::printf("  installed %.3f, install peak %.3f, run peak %.3f copies\n", held,
                install_peak, run_peak);
    CHECK(held > 0.99 && held < 1.01);
    CHECK(install_peak < 1.01);
    CHECK(run_peak > 0.99);
    CHECK(run_peak < kRunCopies);
    check_rows(pine, feed, false);
    // The kernel's run spec still names the feed, bar for bar, after the run.
    const auto view = pine.native_state();
    CHECK(view.spec != nullptr && view.spec->instrument_feeds.size() == 1
          && view.spec->instrument_feeds[0].bars.size() == static_cast<std::size_t>(kFeedBars));
}

void inert_intrabar_path() {
    scenario = "copies: a run with an inert intrabar path";
    const Feed feed = make_feed();
    const std::vector<Bar> chart = make_chart();
    OneSite pine;
    prime(pine);
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install(pine, feed));
    live_heap::restart_peak();
    CHECK(run(pine, chart, 6));
    const double run_peak = copies_since(base);
    std::printf("  run peak %.3f copies\n", run_peak);
    CHECK(run_peak < kRunCopies);
    check_rows(pine, feed, false);
}

void rerun_after_a_door() {
    scenario = "copies: a run again after a door";
    const Feed feed = make_feed();
    const std::vector<Bar> chart = make_chart();
    OneSite again;
    prime(again);
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install(again, feed));
    CHECK(run(again, chart));
    check_rows(again, feed, false);
    live_heap::restart_peak();
    CHECK(install_column(again, feed));
    CHECK(run(again, chart));
    const double rerun_peak = copies_since(base);
    std::printf("  rerun peak %.3f copies\n", rerun_peak);
    CHECK(rerun_peak < kRerunCopies);
    check_rows(again, feed, true);

    OneSite fresh;
    prime(fresh);
    CHECK(install(fresh, feed));
    CHECK(install_column(fresh, feed));
    CHECK(run(fresh, chart));
    check_rows(fresh, feed, true);
    CHECK(fresh.rows.size() == again.rows.size());
    for (std::size_t j = 0; j < fresh.rows.size() && j < again.rows.size(); ++j) {
        CHECK(same_value(fresh.rows[j].close, again.rows[j].close)
              && same_value(fresh.rows[j].time, again.rows[j].time)
              && same_value(fresh.rows[j].column, again.rows[j].column));
    }
    // A third run with nothing installed in between reads the same again.
    CHECK(run(again, chart));
    check_rows(again, feed, true);
}

void refused_configure_keeps_the_feed() {
    scenario = "copies: a refused configure keeps the feed";
    const Feed feed = make_feed();
    const std::vector<Bar> chart = make_chart();
    OneSite pine;
    prime(pine);
    CHECK(install(pine, feed));
    CHECK(run(pine, chart));
    check_rows(pine, feed, false);
    // Another zone changes the session key, which the kernel's configure
    // refuses on a reused handle: the refusal comes after the spec is formed.
    pine.set_syminfo_timezone("UTC");
    pine.rows.clear();
    pine.run(chart.data(), static_cast<int>(chart.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    std::printf("  refused: %s\n", pine.last_error().c_str());
    const auto state = pine.native_state();
    CHECK(state.kind == NativeLifecycleKind::Failed
          && state.failure.code == NativeFailureCode::Contract
          && state.failure.operation == NativeFailureOperation::Configure);
    CHECK(pine.rows.empty());
    // The refused configure moved the completed run's spec, bars included,
    // into the failed state.
    CHECK(state.spec != nullptr && state.spec->instrument_feeds.size() == 1
          && state.spec->instrument_feeds[0].bars.size() == static_cast<std::size_t>(kFeedBars));
    // The store still holds every bar: a column of the feed's length fits.
    CHECK(install_column(pine, feed));
}

}  // namespace

int main() {
    install_and_run();
    inert_intrabar_path();
    rerun_after_a_door();
    refused_configure_keeps_the_feed();
    std::printf("test_symbol_feed_copies: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
