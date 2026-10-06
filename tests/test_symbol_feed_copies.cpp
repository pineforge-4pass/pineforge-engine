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
// copy saved can never be a value lost. Three more scenarios run a body that
// trades on two feeds that each carry a column, and compare the run's trades
// and per-bar broker_state_hash with a fresh handle's:
//   6. The same handle run again: both feeds and both columns come back from
//      the kernel's spec, by index.
//   7. A feed replaced under its key after a run, then a run again.
//   8. A run the kernel stops at its begin (an abort requested from the
//      run-begin callback, so the lent bytes sit in the failed state's
//      spec), then a run again.

#include "native_instrument_feed_fixture.hpp"

#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

// ---- two feeds with columns, and a body that trades on them ------------------

const std::string kKeyG = "SYN:G";

// The second symbol's bars, or the first symbol's after a replacement: every
// field differs from the first feed's, bar by bar.
Feed make_other_feed(int shift, double base, double column_scale) {
    Feed feed;
    feed.bars.reserve(kFeedBars);
    feed.close_ms.reserve(kFeedBars);
    feed.column.reserve(kFeedBars);
    for (int k = 0; k < kFeedBars; ++k) {
        const std::int64_t open = kStart + k * kMinute;
        feed.bars.push_back(make_bar(open, k + shift, base));
        feed.close_ms.push_back(open + kMinute);
        feed.column.push_back(column_scale * ((k * 37) % 101) - 11.0);
    }
    return feed;
}

class TwoSites final : public source::PineStrategyHost {
public:
    Row value_f, value_g;
    bool abort_at_begin = false;

    TwoSites() {
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_string("tickerid", "SYN:BASE");
        set_syminfo_string("ticker", "BASE");
        set_syminfo_mintick(0.01);
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }
    bool lent() const { return symbol_feeds_lent_; }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        value_f = Row{};
        value_g = Row{};
        register_security_eval(0, kKey, "1", input_tf_, false, false, false);
        register_security_eval(1, kKeyG, "1", input_tf_, false, false, false);
        if (abort_at_begin) {
            abort_at_begin = false;
            request_abort();
        }
    }
    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        Row& value = sec_id == 0 ? value_f : value_g;
        value.close = bar.close;
        value.time = static_cast<double>(bar.timestamp);
        value.column = security_column_value(sec_id, sec_id == 0 ? "x" : "y");
    }
    void clear_security(int sec_id) override { (sec_id == 0 ? value_f : value_g) = Row{}; }
    // Enters long or short on what both sites read, columns included, and
    // flattens later, so the trades depend on every byte the sites reach.
    void on_source_bar(const Bar&) override {
        if (std::isnan(value_f.close) || std::isnan(value_g.close)) return;
        const double column_f = std::isnan(value_f.column) ? 0.0 : value_f.column;
        const double column_g = std::isnan(value_g.column) ? 0.0 : value_g.column;
        const double signal = value_f.close - value_g.close + 0.03 * column_f - 0.02 * column_g;
        const int i = pine_bar_index();
        if (i % 6 == 1) strategy_entry("E", std::fmod(std::fabs(signal) * 10.0, 2.0) < 1.0);
        if (i % 6 == 4) strategy_close_all();
    }
};

// What a run left for a caller to compare: its trades, its per-bar broker
// hashes and the final one.
struct RunRecord {
    std::vector<std::string> trades;
    std::vector<std::uint64_t> hashes;
    std::uint64_t final_hash = 0;
    bool operator==(const RunRecord& other) const {
        return trades == other.trades && hashes == other.hashes && final_hash == other.final_hash;
    }
};

RunRecord record_of(TwoSites& pine) {
    RunRecord record;
    ReportC report{};
    pine.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& trade = report.trades[i];
        char line[192];
        std::snprintf(line, sizeof line, "%lld %lld %d %.17g %.17g %.17g %.17g",
                      static_cast<long long>(trade.entry_time),
                      static_cast<long long>(trade.exit_time), trade.is_long, trade.entry_price,
                      trade.exit_price, trade.qty, trade.pnl);
        record.trades.emplace_back(line);
    }
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        record.hashes.push_back(report.broker_state_hash[i]);
    BacktestEngine::free_report(&report);
    record.final_hash = pine.broker_state_hash();
    return record;
}

bool install_two(TwoSites& pine, const Feed& f, bool f_column, const Feed& g) {
    bool ok = pine.set_symbol_feed(kKey, "1", f.bars.data(), f.close_ms.data(), kFeedBars);
    if (f_column) ok = ok && pine.set_symbol_feed_column(kKey, "1", "x", f.column.data(), kFeedBars);
    ok = ok && pine.set_symbol_feed(kKeyG, "1", g.bars.data(), g.close_ms.data(), kFeedBars);
    return ok && pine.set_symbol_feed_column(kKeyG, "1", "y", g.column.data(), kFeedBars);
}

bool run_two(TwoSites& pine, const std::vector<Bar>& chart) {
    pine.run(chart.data(), static_cast<int>(chart.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    if (!pine.last_error().empty()) std::printf("  run error: %s\n", pine.last_error().c_str());
    return pine.last_error().empty();
}

// A fresh handle with the given data, run once: what every handle that holds
// the same data must report.
RunRecord fresh_record(const Feed& f, bool f_column, const Feed& g,
                       const std::vector<Bar>& chart) {
    TwoSites fresh;
    CHECK(install_two(fresh, f, f_column, g));
    CHECK(run_two(fresh, chart));
    return record_of(fresh);
}

void check_same(const RunRecord& got, const RunRecord& want) {
    CHECK(!want.trades.empty());
    CHECK(want.hashes.size() == static_cast<std::size_t>(kChartBars));
    CHECK(got.trades == want.trades);
    CHECK(got.hashes == want.hashes);
    CHECK(got.final_hash == want.final_hash);
    std::printf("  %zu trades, %zu bar hashes, final %016llx: %s\n", got.trades.size(),
                got.hashes.size(), static_cast<unsigned long long>(got.final_hash),
                got == want ? "equal to a fresh handle" : "DIFFERENT from a fresh handle");
}

void two_feeds_with_columns() {
    scenario = "copies: two feeds with columns, run again";
    const Feed f = make_feed();
    const Feed g = make_other_feed(7, 61.0, 0.25);
    const std::vector<Bar> chart = make_chart();
    TwoSites pine;
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install_two(pine, f, true, g));
    const double installed = static_cast<double>(live_heap::live.load() - base);
    live_heap::restart_peak();
    CHECK(run_two(pine, chart));
    const double run_peak = static_cast<double>(live_heap::peak.load() - base) / installed;
    CHECK(pine.lent());
    const RunRecord first = record_of(pine);
    live_heap::restart_peak();
    CHECK(run_two(pine, chart));
    const double rerun_peak = static_cast<double>(live_heap::peak.load() - base) / installed;
    std::printf("  run peak %.3f, rerun peak %.3f copies of both feeds\n", run_peak, rerun_peak);
    CHECK(run_peak < kRunCopies);
    CHECK(rerun_peak < kRerunCopies);
    const RunRecord want = fresh_record(f, true, g, chart);
    check_same(first, want);
    check_same(record_of(pine), want);
}

void feed_replaced_after_a_run() {
    scenario = "copies: a feed replaced after a run";
    const Feed f = make_feed();
    const Feed g = make_other_feed(7, 61.0, 0.25);
    const Feed replaced = make_other_feed(3, 47.5, 0.5);
    const std::vector<Bar> chart = make_chart();
    TwoSites pine;
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install_two(pine, f, true, g));
    const double installed = static_cast<double>(live_heap::live.load() - base);
    CHECK(run_two(pine, chart));
    const RunRecord before = record_of(pine);
    // The door replaces the first feed under its key (its column goes with
    // it); the store gets its bytes back from the kernel's spec first.
    live_heap::restart_peak();
    CHECK(pine.set_symbol_feed(kKey, "1", replaced.bars.data(), replaced.close_ms.data(),
                               kFeedBars));
    CHECK(!pine.lent());
    CHECK(run_two(pine, chart));
    const double peak = static_cast<double>(live_heap::peak.load() - base) / installed;
    // The completed run's spec, the store copied back from it, and the
    // incoming feed (56 of the 128 bytes a bar of both feeds holds).
    const double bound = 2.0 + 56.0 / 128.0 + 0.1;
    std::printf("  replace + run peak %.3f copies of both feeds (bound %.3f)\n", peak, bound);
    CHECK(peak < bound);
    const RunRecord after = record_of(pine);
    CHECK(after.hashes != before.hashes);
    check_same(after, fresh_record(replaced, false, g, chart));
}

void rerun_after_a_begin_failure() {
    scenario = "copies: a run again after the kernel stops a begin";
    const Feed f = make_feed();
    const Feed g = make_other_feed(7, 61.0, 0.25);
    const std::vector<Bar> chart = make_chart();
    TwoSites pine;
    live_heap::restart_peak();
    const long long base = live_heap::live.load();
    CHECK(install_two(pine, f, true, g));
    const double installed = static_cast<double>(live_heap::live.load() - base);
    pine.abort_at_begin = true;
    run_two(pine, chart);
    const auto state = pine.native_state();
    CHECK(state.kind == NativeLifecycleKind::Failed
          && state.failure.code == NativeFailureCode::Aborted);
    // The lent bytes sit in the failed state's spec, every bar of both feeds.
    CHECK(pine.lent());
    CHECK(state.spec != nullptr && state.spec->instrument_feeds.size() == 2
          && state.spec->instrument_feeds[0].bars.size() == static_cast<std::size_t>(kFeedBars)
          && state.spec->instrument_feeds[1].columns.size() == 1);
    live_heap::restart_peak();
    CHECK(run_two(pine, chart));
    const double peak = static_cast<double>(live_heap::peak.load() - base) / installed;
    std::printf("  rerun peak %.3f copies of both feeds\n", peak);
    CHECK(peak < kRerunCopies);
    check_same(record_of(pine), fresh_record(f, true, g, chart));
}

}  // namespace

int main() {
    install_and_run();
    inert_intrabar_path();
    rerun_after_a_door();
    refused_configure_keeps_the_feed();
    two_feeds_with_columns();
    feed_replaced_after_a_run();
    rerun_after_a_begin_failure();
    std::printf("test_symbol_feed_copies: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
