#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/series.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_language_state.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace a5_history_fixture {

// Use the repository's COMPLETE C++17 replacement, including aligned,
// nothrow and sized forms. Requested retained C++ heap is the resident
// measure; no unimplemented allocator-charge counter is reported.
struct RecordingWindow {
    bool previous = global_allocation::recording;
    RecordingWindow() { global_allocation::recording = true; }
    ~RecordingWindow() { global_allocation::recording = previous; }
};
inline volatile double observed = 0.0;

template<class Step>
class HistoryHost final : public pineforge::NativeStrategyHost {
    Step& step_;
    std::size_t callbacks_ = 0;
public:
    explicit HistoryHost(Step& step) : step_(step) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"a5-history-preparation", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "HIST";
        spec.tickerid = "TEST:HIST";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.event_retention = pineforge::NativeEventRetention::Window;
        if (configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
            throw std::runtime_error("configure history preparation host");
    }
    void on_native_bar(const pineforge::Bar& bar, const pineforge::NativeDecisionContext&) override {
        step_(callbacks_++, bar.close);
    }
    std::size_t callbacks() const { return callbacks_; }
};

class SourceRings : public pineforge::source::PineLanguageState {
public:
    void step(double value) {
        _src_open_.push(value);
        _src_high_.push(value + 1.0);
        _src_low_.push(value - 1.0);
        _src_close_.push(value);
        _src_volume_.push(100.0);
        _src_hl2_.push(value);
        _src_hlc3_.push(value);
        _src_ohlc4_.push(value);
        _src_hlcc4_.push(value);

        coof_checkpoint_src_open_ = _src_open_;
        coof_checkpoint_src_high_ = _src_high_;
        coof_checkpoint_src_low_ = _src_low_;
        coof_checkpoint_src_close_ = _src_close_;
        coof_checkpoint_src_volume_ = _src_volume_;
        coof_checkpoint_src_hl2_ = _src_hl2_;
        coof_checkpoint_src_hlc3_ = _src_hlc3_;
        coof_checkpoint_src_ohlc4_ = _src_ohlc4_;
        coof_checkpoint_src_hlcc4_ = _src_hlcc4_;
        _src_close_ = coof_checkpoint_src_close_;
        observed = _src_close_[0];
    }
};

template <class Bound, class Make>
auto prepare_if_supported(Bound& bound, Make&& make, int)
    -> decltype(bound.prepare(std::forward<Make>(make)), void()) {
    bound.prepare(std::forward<Make>(make));
}

template <class Bound, class Make>
void prepare_if_supported(Bound&, Make&&, long) {}

template <class Action>
void require_flat(const char* name, Action&& action, int& failures) {
    const auto before = global_allocation::allocations;
    action();
    const auto added = global_allocation::allocations - before;
    std::printf("A5 %s allocations=%zu %s\n", name, added, added ? "RED" : "PASS");
    if (added != 0) ++failures;
}

template <class T, class Step>
void exercise_checkpointed_window(const char* name, T object, Step step, int& failures) {
    T checkpoint(object);
    require_flat(name, [&] {
        for (int i = 0; i < 2048; ++i) {
            checkpoint = object;
            observed = step(object, static_cast<double>((i * 17) % 101), false);
            observed = step(object, static_cast<double>((i * 19) % 103), true);
            if ((i % 11) == 0) object = checkpoint;
        }
    }, failures);
}

template <class T>
void exercise_checkpointed_window(const char* name, T object, int& failures) {
    exercise_checkpointed_window(name, std::move(object),
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(value) : window.compute(value);
        }, failures);
}

inline int run(std::size_t bars) {
    using namespace pineforge;

    RecordingWindow recording;
    int failures = 0;
    Series<double> series(113);
    Series<double> checkpoint(113);
    SourceRings source_rings;
    source::FirstCallBound<ta::SMA> first_call_bound;
    const auto make_sma = [] { return ta::SMA(37); };
    prepare_if_supported(first_call_bound, make_sma, 0);

    source::HeldHistory held;
    source::HeldHistory held_checkpoint;

    ta::SMA sma(113);
    ta::WMA wma(113);
    ta::VWMA vwma(113);
    ta::ALMA alma(113);
    ta::SWMA swma;
    ta::Mom mom(113);
    ta::ROC roc(113);
    ta::Rising rising(113);
    ta::Falling falling(113);
    ta::CCI cci(113);
    ta::Variance variance(113);
    ta::StdDev stddev(113, false);
    ta::Dev dev(113);
    ta::CMO cmo(113);
    ta::COG cog(113);
    ta::MFI mfi(113);
    ta::PivotHigh pivot_high(17, 19);
    ta::PivotLow pivot_low(17, 19);
    ta::Median median(113);
    ta::Mode mode(113);
    ta::RCI rci(113);
    ta::PercentRank percent_rank(113);
    ta::Linreg linreg(113);
    ta::Correlation correlation(113);
    ta::ValueWhen value_when(17);
    ta::PercentileNearestRank percentile(113);
    source::SeriesLowest series_lowest;
    source::SeriesLowest series_lowest_checkpoint;
    ta::Change change(113);
    ta::PivotPointLevels pivots;

    exercise_checkpointed_window("positive control EMA", ta::EMA(113), failures);
    exercise_checkpointed_window("positive control RMA", ta::RMA(113), failures);
    require_flat("U13 Series first push and checkpoint", [&] {
        series.push(42.0);
        checkpoint = series;
        series = checkpoint;
        observed = series[0];
    }, failures);

    require_flat("U14 source and fill checkpoint", [&] {
        source_rings.step(17.0);
    }, failures);

    require_flat("U16 FirstCallBound late execution", [&] {
        observed = first_call_bound.compute(make_sma, 17.0);
        observed = first_call_bound.recompute(make_sma, 18.0);
    }, failures);

    require_flat("U17 HeldHistory and checkpoint", [&] {
        for (int bar = 0; bar < 40000; ++bar) {
            held.write(bar, static_cast<double>(bar % 37), 15001);
            held_checkpoint = held;
            if ((bar % 13) == 0) held = held_checkpoint;
        }
        held.write(100000, 1.0, 15001);
        observed = held.at(held.last_bar());
    }, failures);

    exercise_checkpointed_window("U15 SMA Kahan", sma, failures);
    exercise_checkpointed_window("U15 WMA", wma, failures);
    exercise_checkpointed_window("U15 VWMA", vwma,
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(value, 100.0) : window.compute(value, 100.0);
        }, failures);
    exercise_checkpointed_window("U15 ALMA", alma, failures);
    exercise_checkpointed_window("U15 SWMA", swma, failures);
    exercise_checkpointed_window("U15 MOM", mom, failures);
    exercise_checkpointed_window("U15 ROC", roc, failures);
    exercise_checkpointed_window("U15 Rising", rising, failures);
    exercise_checkpointed_window("U15 Falling", falling, failures);
    exercise_checkpointed_window("U15 CCI", cci, failures);
    exercise_checkpointed_window("U15 Variance", variance, failures);
    exercise_checkpointed_window("U15 StdDev", stddev, failures);
    exercise_checkpointed_window("U15 Dev", dev, failures);
    exercise_checkpointed_window("U15 CMO checkpoint", cmo, failures);
    exercise_checkpointed_window("U15 MFI checkpoint", mfi,
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(value, 100.0) : window.compute(value, 100.0);
        }, failures);
    exercise_checkpointed_window("U15 COG", cog, failures);
    exercise_checkpointed_window("U15 PivotHigh", pivot_high, failures);
    exercise_checkpointed_window("U15 PivotLow", pivot_low, failures);
    exercise_checkpointed_window("U18 Median", median, failures);
    exercise_checkpointed_window("U18 Mode", mode, failures);
    exercise_checkpointed_window("U18 RCI", rci, failures);
    exercise_checkpointed_window("U18 PercentRank", percent_rank, failures);
    exercise_checkpointed_window("U18 Linreg", linreg,
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(value, 0.0) : window.compute(value, 0.0);
        }, failures);
    exercise_checkpointed_window("U18 Correlation", correlation,
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(value, value + 1.0) : window.compute(value, value + 1.0);
        }, failures);
    exercise_checkpointed_window("U18 ValueWhen", value_when,
        [](auto& window, double value, bool recompute) {
            return recompute ? window.recompute(true, value, 0) : window.compute(true, value, 0);
        }, failures);

    require_flat("U18 percentile workspace", [&] {
        for (int i = 0; i < 2048; ++i) {
            observed = percentile.compute(i % 37, 70.0);
            observed = percentile.recompute(i % 41, 70.0);
        }
    }, failures);

    require_flat("U16 declared Change bound", [&] {
        for (int i = 0; i < 2048; ++i) {
            observed = change.compute(i, 113);
            observed = change.recompute(i + 1.0, 113);
        }
    }, failures);

    require_flat("U19 fixed pivot results", [&] {
        for (int i = 0; i < 2048; ++i) {
            const auto levels = pivots.compute(ta::PivotLevelsType::Classic, true, true,
                                               10.0, 12.0, 8.0, 11.0);
            observed = levels[0];
            const auto free_levels = ta::pivot_point_levels("Classic", 12.0, 8.0, 11.0);
            observed = free_levels[0];
        }
    }, failures);

    const auto start_live = global_allocation::live_bytes;
    std::size_t history_allocations = 0;
    std::printf("A5 bars=%zu start_live=%zu\n", bars, start_live);

    auto step = [&](std::size_t bar, double value) {
        RecordingWindow window;
        const auto before = global_allocation::allocations;
        series.push(value);
        checkpoint = series;
        source_rings.step(value);
        held.write(static_cast<long long>(200000 + bar), value, 15001);
        held_checkpoint = held;
        observed = first_call_bound.compute(make_sma, value);
        observed = sma.compute(value);
        observed = wma.compute(value);
        observed = vwma.compute(value, 100.0);
        observed = alma.compute(value);
        observed = swma.compute(value);
        observed = mom.compute(value);
        observed = roc.compute(value);
        observed = rising.compute(value);
        observed = falling.compute(value);
        observed = cci.compute(value);
        observed = variance.compute(value);
        observed = stddev.compute(value);
        observed = dev.compute(value);
        observed = cmo.compute(value);
        observed = cog.compute(value);
        observed = mfi.compute(value, 100.0);
        observed = pivot_high.compute(value);
        observed = pivot_low.compute(value);
        observed = median.compute(value);
        observed = mode.compute(value);
        observed = rci.compute(value);
        observed = percent_rank.compute(value);
        observed = linreg.compute(value, 0.0);
        observed = correlation.compute(value, value + 1.0);
        observed = value_when.compute((bar % 7U) == 0U, value, 0);
        observed = percentile.compute(value, 70.0);
        observed = change.compute(value, 113);
        const auto levels = pivots.compute(ta::PivotLevelsType::Classic, true, true,
                                           value, value + 1.0, value - 1.0, value);
        observed = levels[0];
        if ((bar % 7U) == 0U) {
            ta::BarContextScope context(static_cast<long long>(bar), 0);
            observed = series_lowest.compute(value, 113.0);
            observed = series_lowest.recompute(value + 0.5, 113.0);
            series_lowest_checkpoint = series_lowest;
            if ((bar % 19U) == 0U) series_lowest = series_lowest_checkpoint;
        }
        history_allocations += global_allocation::allocations - before;
        if ((bar + 1U) == bars || bar + 1U == 100000U || bar + 1U == 1000000U) {
            const auto live = global_allocation::live_bytes;
            std::printf("A5 bars=%zu sample=%zu allocation_delta=%zu live=%zu prepared_live=%zu\n",
                        bars, bar + 1U, history_allocations, live, start_live);
            if (history_allocations != 0 || live != start_live) ++failures;
        }
    };
    // Only caller-owned OHLCV scales. Host setup/driver allocations are not
    // history-family allocations; every history call above has its own window.
    global_allocation::recording = false;
    std::vector<Bar> feed(bars);
    for (std::size_t i = 0; i < bars; ++i) {
        const double close = 100.0 + static_cast<double>((i * 37U) % 1021U) / 10.0;
        feed[i] = {close, close + 1.0, close - 1.0, close, 1000.0,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    HistoryHost<decltype(step)> host(step);
    host.run(feed.data(), static_cast<int>(bars), "1", "1");
    global_allocation::recording = true; // prepared objects are destroyed in their recording window
    if (!host.last_error().empty() || host.callbacks() != bars) {
        std::fprintf(stderr, "A5 history host: callbacks=%zu expected=%zu error=%s\n",
                     host.callbacks(), bars, host.last_error().c_str());
        ++failures;
    }

    std::printf("A5 bars=%zu failures=%d final_live=%zu\n",
                bars, failures, global_allocation::live_bytes);
    return failures == 0 ? 0 : 1;
}

} // namespace a5_history_fixture
