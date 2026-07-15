// test_chart_ema_na_warmup — pins the opt-in KI-55 chart-EMA warmup flag.
//
// ``chart_ema_na_warmup`` is an independent, default-off run flag carried
// through the syminfo-metadata channel.  While chart strategy code executes,
// it makes newly used ta::EMA instances latch TradingView's built-in warmup
// shape (na for length-1 values, then an SMA seed).  request.security keeps
// its own ``security_range_start_na_warmup`` scope and must not inherit this
// chart choice.

// This fixture covers every engine-owned chart on_bar dispatch path: normal,
// calc_on_order_fills (ordinary + fill recalc), magnifier, and streaming.  It
// also proves that the thread-local selector is restored when on_bar throws.

#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond, tag) do {                                                   \
    if (!(cond)) {                                                              \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__);                  \
        ++failures;                                                             \
    }                                                                           \
} while (0)

bool exact_or_both_na(double lhs, double rhs) {
    if (is_na(lhs) && is_na(rhs)) return true;
    if (is_na(lhs) || is_na(rhs)) return false;
    return lhs == rhs;
}

std::vector<Bar> flat_bars(int count, int64_t step_ms = 60'000) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double price = 10.0 * static_cast<double>(i + 1);
        bars.push_back(Bar{price, price, price, price, 1.0,
                           static_cast<int64_t>(i + 1) * step_ms});
    }
    return bars;
}

class EmaValueHarness final : public BacktestEngine {
public:
    ta::EMA ema{3};
    std::vector<bool> flags;
    std::vector<double> values;

    void on_bar(const Bar& bar) override {
        flags.push_back(ta::ema_na_warmup_flag());
        values.push_back(ema.compute(bar.close));
    }
};

void test_default_off_on_and_disable_zero() {
    const auto bars = flat_bars(3);

    ta::ema_na_warmup_flag() = false;
    EmaValueHarness off;
    off.run(bars.data(), static_cast<int>(bars.size()));
    const double expected_off[] = {10.0, 15.0, 22.5};
    CHECK(off.flags.size() == 3, "default-off: one chart dispatch per bar");
    CHECK(std::all_of(off.flags.begin(), off.flags.end(),
                      [](bool value) { return !value; }),
          "default-off: chart scope exposes false");
    for (std::size_t i = 0; i < off.values.size() && i < 3; ++i) {
        CHECK(exact_or_both_na(off.values[i], expected_off[i]),
              "default-off: EMA keeps src-seed recursion");
    }
    CHECK(!ta::ema_na_warmup_flag(),
          "default-off: chart dispatch restores ambient false");

    EmaValueHarness on;
    on.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    on.run(bars.data(), static_cast<int>(bars.size()));
    const double expected_on[] = {na<double>(), na<double>(), 20.0};
    CHECK(on.flags.size() == 3, "flag-on: one chart dispatch per bar");
    CHECK(std::all_of(on.flags.begin(), on.flags.end(),
                      [](bool value) { return value; }),
          "flag-on: chart scope exposes true");
    for (std::size_t i = 0; i < on.values.size() && i < 3; ++i) {
        CHECK(exact_or_both_na(on.values[i], expected_on[i]),
              "flag-on: EMA na-warms then SMA-seeds");
    }
    CHECK(!ta::ema_na_warmup_flag(),
          "flag-on: chart dispatch restores ambient false");

    EmaValueHarness disabled;
    disabled.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    disabled.set_syminfo_metadata("chart_ema_na_warmup", 0.0);
    disabled.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(std::all_of(disabled.flags.begin(), disabled.flags.end(),
                      [](bool value) { return !value; }),
          "disable=0: later metadata value turns chart warmup off");
    CHECK(disabled.values.size() == 3
              && exact_or_both_na(disabled.values.front(), 10.0),
          "disable=0: EMA returns to src-seed behavior");
}

class DispatchHarness final : public BacktestEngine {
public:
    std::vector<bool> flags;
    std::vector<bool> realtime_flags;
    bool placed = false;

    explicit DispatchHarness(bool coof = false) {
        calc_on_order_fills_ = coof;
    }

    void on_bar(const Bar&) override {
        const bool flag = ta::ema_na_warmup_flag();
        flags.push_back(flag);
        if (barstate_islast_) realtime_flags.push_back(flag);
        if (calc_on_order_fills_ && bar_index_ == 0 && !placed) {
            placed = true;
            strategy_entry("L", true);
        }
    }
};

void test_coof_dispatches_are_scoped() {
    ta::ema_na_warmup_flag() = false;
    DispatchHarness strat(/*coof=*/true);
    strat.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    const auto bars = flat_bars(3);
    strat.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(strat.last_error().empty(), "COOF: run succeeds");
    CHECK(strat.flags.size() > bars.size(),
          "COOF: fixture exercised at least one fill recalculation");
    CHECK(std::all_of(strat.flags.begin(), strat.flags.end(),
                      [](bool value) { return value; }),
          "COOF: ordinary and fill-recalc chart dispatches expose true");
    CHECK(!ta::ema_na_warmup_flag(), "COOF: ambient flag restored");
}

void test_magnifier_dispatch_is_scoped() {
    ta::ema_na_warmup_flag() = false;
    DispatchHarness strat;
    strat.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    const auto bars = flat_bars(4);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "1", "2", /*bar_magnifier=*/true, 4,
              MagnifierDistribution::ENDPOINTS);

    CHECK(strat.last_error().empty(), "magnifier: run succeeds");
    CHECK(strat.flags.size() == 2,
          "magnifier: one chart dispatch per completed 2m bar");
    CHECK(std::all_of(strat.flags.begin(), strat.flags.end(),
                      [](bool value) { return value; }),
          "magnifier: chart dispatch exposes true");
    CHECK(!ta::ema_na_warmup_flag(), "magnifier: ambient flag restored");
}

void test_streaming_dispatch_is_scoped() {
    ta::ema_na_warmup_flag() = false;
    DispatchHarness strat;
    strat.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    const auto warmup = flat_bars(2);
    CHECK(strat.stream_begin(warmup.data(), static_cast<int>(warmup.size()),
                             "1", "1"),
          "streaming: warmup begins");
    CHECK(strat.stream_push_tick(TradeTick{180'010, 1, 35.0, 1.0}),
          "streaming: realtime tick accepted");
    CHECK(strat.stream_advance_time(240'000),
          "streaming: realtime chart bar finalized");

    CHECK(strat.flags.size() >= 3,
          "streaming: warmup and realtime chart dispatches both ran");
    CHECK(std::all_of(strat.flags.begin(), strat.flags.end(),
                      [](bool value) { return value; }),
          "streaming: every chart dispatch exposes true");
    CHECK(!strat.realtime_flags.empty()
              && std::all_of(strat.realtime_flags.begin(),
                             strat.realtime_flags.end(),
                             [](bool value) { return value; }),
          "streaming: direct realtime dispatch exposes true");
    CHECK(!ta::ema_na_warmup_flag(), "streaming: ambient flag restored");
    CHECK(strat.stream_end(false), "streaming: stream ends cleanly");
}

class IndependenceHarness final : public BacktestEngine {
public:
    std::vector<bool> chart_flags;
    std::vector<bool> security_flags;

    IndependenceHarness() {
        register_security_eval(0, "1", "1", /*lookahead_on=*/false,
                               /*gaps_on=*/false);
    }

    void evaluate_security(int sec_id, const Bar&, bool) override {
        if (sec_id == 0) {
            security_flags.push_back(ta::ema_na_warmup_flag());
        }
    }

    void on_bar(const Bar&) override {
        chart_flags.push_back(ta::ema_na_warmup_flag());
    }
};

void test_chart_and_security_flags_are_independent() {
    const auto bars = flat_bars(4);
    ta::ema_na_warmup_flag() = false;

    IndependenceHarness chart_only;
    chart_only.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    chart_only.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(!chart_only.chart_flags.empty()
              && std::all_of(chart_only.chart_flags.begin(),
                             chart_only.chart_flags.end(),
                             [](bool value) { return value; }),
          "independence: chart flag on inside on_bar");
    CHECK(!chart_only.security_flags.empty()
              && std::all_of(chart_only.security_flags.begin(),
                             chart_only.security_flags.end(),
                             [](bool value) { return !value; }),
          "independence: chart flag does not leak into security evaluator");

    IndependenceHarness security_only;
    security_only.set_syminfo_metadata("security_range_start_na_warmup", 1.0);
    security_only.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(!security_only.chart_flags.empty()
              && std::all_of(security_only.chart_flags.begin(),
                             security_only.chart_flags.end(),
                             [](bool value) { return !value; }),
          "independence: security flag does not leak into chart on_bar");
    CHECK(!security_only.security_flags.empty()
              && std::all_of(security_only.security_flags.begin(),
                             security_only.security_flags.end(),
                             [](bool value) { return value; }),
          "independence: existing security evaluator scope remains on");
    CHECK(!ta::ema_na_warmup_flag(), "independence: ambient flag restored");
}

class ThrowingHarness final : public BacktestEngine {
public:
    bool observed = false;

    void on_bar(const Bar&) override {
        observed = ta::ema_na_warmup_flag();
        throw std::runtime_error("chart warmup restoration probe");
    }
};

void test_thread_local_restored_after_exception() {
    const auto bars = flat_bars(1);

    ta::ema_na_warmup_flag() = false;
    ThrowingHarness enabled;
    enabled.set_syminfo_metadata("chart_ema_na_warmup", 1.0);
    enabled.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(enabled.observed, "exception: enabled chart body observes true");
    CHECK(!enabled.last_error().empty(), "exception: run records thrown error");
    CHECK(!ta::ema_na_warmup_flag(),
          "exception: enabled scope restores ambient false");

    ta::ema_na_warmup_flag() = true;
    ThrowingHarness disabled;
    disabled.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(!disabled.observed,
          "exception: disabled chart scope masks ambient true inside on_bar");
    CHECK(ta::ema_na_warmup_flag(),
          "exception: disabled scope restores ambient true");
    ta::ema_na_warmup_flag() = false;
}

}  // namespace

int main() {
    test_default_off_on_and_disable_zero();
    test_coof_dispatches_are_scoped();
    test_magnifier_dispatch_is_scoped();
    test_streaming_dispatch_is_scoped();
    test_chart_and_security_flags_are_independent();
    test_thread_local_restored_after_exception();

    if (failures != 0) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_chart_ema_na_warmup passed.\n");
    return 0;
}
