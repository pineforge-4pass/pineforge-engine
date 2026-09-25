// test_adapter_security_route_conditions.cpp — R5 lane H-MEASURE (AUDIT4 §6 M20 / G2-23, X14).
//
// The Pine adapter declares its request.security sites to the kernel as
// NativeRunSpec::subscriptions
// (PineStrategyHost::declare_security_sites_to_kernel, lane R3b) unless one of
// the conditions of that predicate holds; then EVERY site of the run stays on
// the source evaluator. This unit takes each reachable condition once, on a
// site the predicate would otherwise route, and holds two facts:
//
//   1. the route: the site stays on the evaluator (the running spec names no
//      subscription and native_series_bar never answers), and the control
//      site without the condition is the kernel's;
//   2. where a NativeTimeframeSubscription can spell the same site, the paired
//      differential: the same site on the same bars through a bare kernel host
//      declaring it (native_series_bar(0).close on every script bar) against
//      the value the generated body reads on every chart bar. The chart bars
//      where the two differ are pinned exactly: none where the condition is a
//      conservative exclusion on these feeds, the Pine rule's own bars where
//      it is not.
//
// Conditions the predicate names that no run can trip on its own -- subsumed
// by an earlier test or behind a validate-time throw -- are listed in the
// ADR-0001 `subscriptions` row, not here. The stream (tick input) and the auxiliary
// feed are pinned by route only: the kernel refuses ticks while a series is
// declared (tests/test_native_htf_subscriptions_stream.cpp) and the auxiliary
// drive's differential is tests/test_native_auxiliary_feed_twin.cpp.

#include <pineforge/na.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                          \
        }                                                                      \
    } while (false)

std::int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kQuarter = 15 * kMinute;

// A 24x7 feed of `step`-minute bars from 2024-01-01 00:00Z + offset quarters.
std::vector<Bar> grid_bars(int n, int offset_quarters = 0, std::int64_t step = kQuarter) {
    const std::int64_t origin = utc_ms(2024, 1, 1) + offset_quarters * kQuarter;
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + i;
        Bar bar{};
        bar.open = open; bar.high = open + 2.0; bar.low = open - 1.0; bar.close = open + 0.5;
        bar.volume = static_cast<double>(i + 1);
        bar.timestamp = origin + static_cast<std::int64_t>(i) * step;
        bars.push_back(bar);
    }
    return bars;
}

// Regular-hours 15m bars, 09:30..15:45 America/New_York (EST), five weekdays.
std::vector<Bar> rth_bars(int days) {
    std::vector<Bar> bars;
    int day = 2, made = 0, i = 0;
    while (made < days) {
        const std::int64_t d0 = utc_ms(2024, 1, day);
        const int weekday = static_cast<int>(((d0 / 86400000) + 4) % 7);  // 0 = Sunday
        ++day;
        if (weekday == 0 || weekday == 6) continue;
        for (int k = 0; k < 26; ++k, ++i) {
            const double open = 100.0 + i;
            Bar bar{};
            bar.open = open; bar.high = open + 2.0; bar.low = open - 1.0; bar.close = open + 0.5;
            bar.volume = static_cast<double>(i + 1);
            bar.timestamp = d0 + (14 * 60 + 30) * kMinute + k * kQuarter;
            bars.push_back(bar);
        }
        ++made;
    }
    return bars;
}

struct Site {
    std::string tf;
    bool lookahead = false;
    bool gaps = false;
    bool heikinashi = false;
    bool lower_tf = false;
    int sec_id = 0;
};

// The adapter in the generated shape (tests/test_adapter_htf_relower.cpp):
// _req_sec_0 is the last evaluated bar's close; the body reads it per bar.
class GeneratedShape final : public source::PineStrategyHost {
public:
    Site site;
    double value = na<double>();
    std::vector<double> visible;
    bool series_seen = false;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        value = na<double>();
        visible.clear();
        series_seen = false;
        if (site.lower_tf) {
            register_security_lower_tf_eval(site.sec_id, site.tf, input_tf_);
        } else {
            register_security_eval(site.sec_id, site.tf, input_tf_, site.lookahead, site.gaps,
                                   site.heikinashi);
        }
    }
    void evaluate_security(int, const Bar& bar, bool) override { value = bar.close; }
    void clear_security(int) override { value = na<double>(); }
    void on_source_bar(const Bar&) override {
        visible.push_back(value);
        if (native_series_bar(0).has_value()) series_seen = true;
    }
    bool kernel_routed() const {
        const auto view = native_state();
        return view.spec != nullptr && !view.spec->subscriptions.empty();
    }
};

// The kernel alone, declaring the same series.
class KernelSeries final : public NativeStrategyHost {
public:
    std::vector<double> visible;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const auto bar = native_series_bar(0);
        visible.push_back(bar ? bar->close : na<double>());
    }
};

struct Shape {
    const char* input_tf = "15";
    const char* script_tf = "15";
    bool magnifier = false;
    const char* type = "crypto";
    const char* session = "24x7";
    const char* timezone = "UTC";
    double range_start_ms = 0.0;      // security_range_start_na_warmup
    bool lookahead_projection = false; // historical_security_lookahead_projection
};

void prime(GeneratedShape& pine, const Shape& shape) {
    pine.set_syminfo_timezone(shape.timezone);
    pine.set_syminfo_session(shape.session);
    pine.set_syminfo_type(shape.type);
    if (shape.range_start_ms > 0.0)
        pine.set_syminfo_metadata("security_range_start_na_warmup", shape.range_start_ms);
    if (shape.lookahead_projection)
        pine.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
}

void run_pine(GeneratedShape& pine, const std::vector<Bar>& bars, const Shape& shape) {
    pine.run(bars.data(), static_cast<int>(bars.size()), shape.input_tf, shape.script_tf,
             shape.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pine.last_error().empty());
}

NativeRunSpec kernel_spec(const std::vector<Bar>& bars, const Site& site, const Shape& shape) {
    NativeRunSpec spec;
    spec.identity = {std::string(shape.session) + "@" + shape.timezone, 1};
    spec.input_tf = shape.input_tf;
    spec.script_tf = shape.script_tf;
    spec.ticker = "SERIES";
    spec.tickerid = "TEST:SERIES";
    spec.type = shape.type;
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "security route differential";
    spec.volumetype = "base";
    spec.timezone = shape.timezone;
    spec.session = shape.session;
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    NativeTimeframeSubscription series;
    series.tf = site.tf;
    series.lookahead = site.lookahead;
    series.gaps = site.gaps;
    spec.subscriptions.push_back(series);
    if (shape.magnifier) {
        // The adapter's own projection of the magnifier (PineExecutionAdapter::project).
        if (std::string(shape.input_tf) == shape.script_tf) {
            IntrabarPath::synthesized path;
            spec.intrabar.value = path;
        } else {
            IntrabarPath::lower_tf path;
            path.bars = bars;
            path.tf = shape.input_tf;
            path.sample_eligibility = IntrabarPath::SampleEligibility::ContinuousSegments;
            spec.intrabar.value = path;
        }
    }
    return spec;
}

using Ranges = std::vector<std::pair<int, int>>;

Ranges differing(const std::vector<double>& a, const std::vector<double>& b) {
    Ranges out;
    const std::size_t n = std::min(a.size(), b.size());
    const auto differs = [&](std::size_t k) {
        return !((std::isnan(a[k]) && std::isnan(b[k])) || a[k] == b[k]);
    };
    for (std::size_t i = 0; i < n;) {
        if (!differs(i)) { ++i; continue; }
        std::size_t j = i;
        while (j + 1 < n && differs(j + 1)) ++j;
        out.push_back({static_cast<int>(i), static_cast<int>(j)});
        i = j + 1;
    }
    return out;
}

void print_ranges(const Ranges& ranges) {
    std::printf("    differing chart bars:");
    if (ranges.empty()) std::printf(" none");
    for (const auto& r : ranges) std::printf(" %d..%d", r.first, r.second);
    std::printf("\n");
}

// Route + differential for one condition.
void condition(const char* name, const std::vector<Bar>& bars, const Site& site,
               const Shape& shape, bool kernel_route, bool kernel_expressible,
               const Ranges& expected) {
    scenario = name;
    GeneratedShape pine;
    pine.site = site;
    prime(pine, shape);
    run_pine(pine, bars, shape);
    CHECK(pine.kernel_routed() == kernel_route);
    CHECK(pine.series_seen == kernel_route);
    if (!kernel_expressible) return;
    KernelSeries kernel;
    const auto applied = kernel.configure_native(kernel_spec(bars, site, shape));
    CHECK(applied.status == NativeSetupStatus::Applied);
    kernel.run(bars.data(), static_cast<int>(bars.size()), shape.input_tf, shape.script_tf,
               shape.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(kernel.last_error().empty());
    CHECK(kernel.visible.size() == pine.visible.size());
    const Ranges got = differing(pine.visible, kernel.visible);
    CHECK(got == expected);
    if (got != expected) {
        std::printf("  [%s]\n", name);
        print_ranges(got);
    }
}

// ---- 1. the control: a plain site on the chart timeframe is the kernel's ----

void test_controls() {
    condition("control: plain 60 on 15/15", grid_bars(64), Site{"60"}, Shape{}, true, true, {});
    condition("control: gaps_on 60 on 15/15", grid_bars(64), Site{"60", false, true}, Shape{},
              true, true, {});
}

// ---- 2. run-shape conditions ------------------------------------------------

void test_run_shape_conditions() {
    Shape aggregated;
    aggregated.script_tf = "60";
    // input_tf != script_tf (pine_strategy_host.cpp:1551): conservative on
    // every feed below -- the kernel's series is the evaluator's, bar for bar.
    condition("aggregated: 240 on 15 -> 60", grid_bars(128), Site{"240"}, aggregated, false, true, {});
    condition("aggregated: 60 on 15 -> 60", grid_bars(128), Site{"60"}, aggregated, false, true, {});
    Shape rth_aggregated = aggregated;
    rth_aggregated.type = "stock";
    rth_aggregated.session = "0930-1600";
    rth_aggregated.timezone = "America/New_York";
    condition("aggregated: D on RTH 15 -> 60", rth_bars(5), Site{"D"}, rth_aggregated, false, true, {});
    condition("aggregated: 120 on RTH 15 -> 60", rth_bars(5), Site{"120"}, rth_aggregated, false,
              true, {});

    // the bar magnifier (:1545): conservative on the same feeds.
    Shape magnified = aggregated;
    magnified.magnifier = true;
    condition("magnifier: 240 on 15 -> 60", grid_bars(128), Site{"240"}, magnified, false, true, {});
    Shape magnified_chart;
    magnified_chart.magnifier = true;
    condition("magnifier: 60 on 15 -> 15", grid_bars(64), Site{"60"}, magnified_chart, false, true, {});
    Shape rth_magnified = rth_aggregated;
    rth_magnified.magnifier = true;
    condition("magnifier: D on RTH 15 -> 60", rth_bars(5), Site{"D"}, rth_magnified, false, true, {});
    condition("magnifier: 120 on RTH 15 -> 60", rth_bars(5), Site{"120"}, rth_magnified, false,
              true, {});

    // KI-55 range-start cut (:1546): a range start at bar 6 (01:30Z) cuts the
    // hour in progress; the kernel, which has no cut, reads it from bar 3.
    const std::vector<Bar> hourly = grid_bars(64);
    Shape range_start;
    range_start.range_start_ms = static_cast<double>(hourly[6].timestamp);
    condition("KI-55 range start at bar 6", hourly, Site{"60"}, range_start, false, true, {{3, 10}});
    // ... and a cut at the first bar cuts nothing, route included.
    Shape range_start_first;
    range_start_first.range_start_ms = static_cast<double>(hourly[0].timestamp);
    condition("KI-55 range start at bar 0", hourly, Site{"60"}, range_start_first, false, true, {});

    // historical lookahead projection (:1546): a lookahead_off site is the
    // kernel's plain series; a lookahead_on site is the kernel's lookahead
    // rule except on the run's incomplete tail hour (bars 64, 65), which the
    // projection shows progressively and the kernel never completes.
    Shape projection;
    projection.lookahead_projection = true;
    condition("historical projection, lookahead_off", grid_bars(66), Site{"60"}, projection, false,
              true, {});
    condition("historical projection, lookahead_on", grid_bars(66), Site{"60", true}, projection,
              false, true, {{64, 65}});
}

// ---- 3. per-site conditions -------------------------------------------------

void test_site_conditions() {
    // lookahead_on (:1572) without the projection: the evaluator reads the
    // hour in progress on every bar that completes none (index % 4 != 3), the
    // kernel's lookahead rule the completed hour.
    Ranges progressive;
    for (int k = 0; k < 16; ++k) progressive.push_back({4 * k, 4 * k + 2});
    progressive.push_back({64, 65});
    condition("lookahead_on", grid_bars(66), Site{"60", true}, Shape{}, false, true, progressive);
    // ticker.heikinashi and request.security_lower_tf: no subscription spells them.
    condition("heikinashi", grid_bars(64), Site{"60", false, false, true}, Shape{}, false, false, {});
    condition("security_lower_tf", grid_bars(64), Site{"1", false, false, false, true}, Shape{},
              false, false, {});
    // OTC daily pins (:1578): a forex intraday run that starts mid-day has no
    // partial first D bar; the kernel delivers it at bar 71 and holds it until
    // the first full day completes at bar 167. From midnight nothing differs.
    Shape forex;
    forex.type = "forex";
    condition("forex D from 06:00Z", grid_bars(480, 24), Site{"D"}, forex, false, true, {{71, 166}});
    condition("forex D from 00:00Z", grid_bars(480), Site{"D"}, forex, false, true, {});
    condition("crypto D from 06:00Z (no pins: the kernel's)", grid_bars(480, 24), Site{"D"}, Shape{},
              true, true, {});
    // A sec_id that is not the site's index (:1567): structural only.
    Site second;
    second.tf = "60";
    second.sec_id = 1;
    condition("sec_id 1 without sec_id 0", grid_bars(64), second, Shape{}, false, true, {});
}

// ---- 4. the kernel's own refusal (:1584) --------------------------------------

void test_kernel_refusal() {
    scenario = "kernel refuses a non-divisible series";
    GeneratedShape pine;
    pine.site = Site{"25"};
    const std::vector<Bar> bars = grid_bars(64);
    prime(pine, Shape{});
    run_pine(pine, bars, Shape{});
    CHECK(!pine.kernel_routed());
    CHECK(!pine.series_seen);
    KernelSeries kernel;
    const auto applied = kernel.configure_native(kernel_spec(bars, Site{"25"}, Shape{}));
    CHECK(applied.status != NativeSetupStatus::Applied);
}

// ---- 5. route-only conditions -------------------------------------------------

void test_route_only_conditions() {
    // A stream (:1545): the kernel takes no tick while a series is declared.
    scenario = "stream";
    {
        GeneratedShape pine;
        pine.site = Site{"60"};
        prime(pine, Shape{});
        const std::vector<Bar> bars = grid_bars(32);
        CHECK(pine.stream_begin(bars.data(), 19, "15", "15"));
        TradeTick tick{};
        tick.timestamp = bars[19].timestamp;
        tick.sequence = 1;
        tick.price = bars[19].close;
        tick.quantity = 1.0;
        CHECK(pine.stream_push_tick(tick));
        CHECK(pine.stream_end(true));
        CHECK(pine.last_error().empty());
        CHECK(!pine.kernel_routed());
        CHECK(!pine.series_seen);
    }
    // The auxiliary finer feed (:1549).
    scenario = "auxiliary feed";
    {
        GeneratedShape pine;
        pine.site = Site{"60"};
        prime(pine, Shape{});
        const std::vector<Bar> chart = grid_bars(64);
        const std::vector<Bar> finer = grid_bars(192, 0, 5 * kMinute);
        CHECK(pine.set_aux_security_feed(finer.data(), static_cast<int>(finer.size()), "5"));
        run_pine(pine, chart, Shape{});
        CHECK(!pine.kernel_routed());
        CHECK(!pine.series_seen);
    }
}

}  // namespace

int main() {
    test_controls();
    test_run_shape_conditions();
    test_site_conditions();
    test_kernel_refusal();
    test_route_only_conditions();
    std::printf("adapter security route conditions: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
