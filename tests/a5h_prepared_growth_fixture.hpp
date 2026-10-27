#pragma once

#include "a5h_growth_fixture.hpp"
#include <pineforge/source/pine_language_state.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

namespace history_growth_test {

class SourceRings : public pineforge::source::PineLanguageState {
public:
    void step(double value) {
        _src_open_.push(value); _src_high_.push(value + 1); _src_low_.push(value - 1);
        _src_close_.push(value); _src_volume_.push(100);
        _src_hl2_.push(value); _src_hlc3_.push(value);
        _src_ohlc4_.push(value); _src_hlcc4_.push(value);
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
    }
};

template<class Bound, class Make>
auto prepare(Bound& bound, Make make, int) -> decltype(bound.prepare(make), void()) {
    bound.prepare(make);
}
template<class Bound, class Make>
void prepare(Bound&, Make, long) {}

struct PreparedHistory {
    pineforge::Series<double> series{113}, saved_series{113};
    SourceRings source;
    pineforge::source::HeldHistory held, saved_held;
    pineforge::source::FirstCallBound<pineforge::ta::SMA> first;
    pineforge::ta::SMA sma{113};
    pineforge::ta::WMA wma{113};
    pineforge::ta::VWMA vwma{113};
    pineforge::ta::ALMA alma{113};
    pineforge::ta::SWMA swma;
    pineforge::ta::Mom mom{113};
    pineforge::ta::ROC roc{113};
    pineforge::ta::Rising rising{113};
    pineforge::ta::Falling falling{113};
    pineforge::ta::CCI cci{113};
    pineforge::ta::Variance variance{113};
    pineforge::ta::StdDev stddev{113, false};
    pineforge::ta::Dev dev{113};
    pineforge::ta::CMO cmo{113};
    pineforge::ta::MFI mfi{113};
    pineforge::ta::COG cog{113};
    pineforge::ta::PivotHigh high{17, 19};
    pineforge::ta::PivotLow low{17, 19};
    pineforge::ta::Median median{113};
    pineforge::ta::Mode mode{113};
    pineforge::ta::RCI rci{113};
    pineforge::ta::PercentRank rank{113};
    pineforge::ta::Linreg linreg{113};
    pineforge::ta::Correlation correlation{113};
    pineforge::ta::ValueWhen value_when{17};
    pineforge::ta::PercentileNearestRank percentile{113};
    pineforge::ta::Change change{113};
    pineforge::ta::PivotPointLevels pivots;
    static auto make_first() { return pineforge::ta::SMA(37); }
    PreparedHistory() { prepare(first, make_first, 0); }
};

inline int prepared_growth(std::size_t count) {
    try {
        PreparedHistory state;
        volatile double observed = 0;
        std::size_t allocations = 0;
        // Predetermined post-preparation bound: zero new resident bytes and
        // zero allocations. Constructors run before this family's windows.
        // No sampled resident value is used as a ceiling.
        require(global_allocation::live_bytes == 0, "prepared measurement starts dirty");
        auto step = [&](std::size_t bar, double value) {
            Window window;
            const auto before = global_allocation::allocations;
            state.series.push(value);
            state.saved_series = state.series;
            state.series = state.saved_series;
            for (int offset : {0, 1, 37, 112, 113}) {
                const auto available = std::min<std::size_t>(bar + 1, 113);
                const auto got = state.series[offset];
                if (static_cast<std::size_t>(offset) >= available)
                    require(pineforge::is_na(got), "prepared Series window na");
                else require(got == value_at(bar - offset), "prepared Series history");
            }
            state.source.step(value);
            state.held.write(static_cast<long long>(bar), value, 15001);
            state.saved_held = state.held;
            if (bar % 79 == 0) state.held = state.saved_held;
            observed = state.first.compute(PreparedHistory::make_first, value);
            observed = state.sma.compute(value);
            observed = state.wma.compute(value);
            observed = state.vwma.compute(value, 100.0);
            observed = state.alma.compute(value);
            observed = state.swma.compute(value);
            observed = state.mom.compute(value);
            observed = state.roc.compute(value);
            observed = state.rising.compute(value);
            observed = state.falling.compute(value);
            observed = state.cci.compute(value);
            observed = state.variance.compute(value);
            observed = state.stddev.compute(value);
            observed = state.dev.compute(value);
            observed = state.cmo.compute(value);
            observed = state.mfi.compute(value, 100.0);
            observed = state.cog.compute(value);
            observed = state.high.compute(value);
            observed = state.low.compute(value);
            observed = state.median.compute(value);
            observed = state.mode.compute(value);
            observed = state.rci.compute(value);
            observed = state.rank.compute(value);
            observed = state.linreg.compute(value, 0.0);
            observed = state.correlation.compute(value, value + 1.0);
            observed = state.value_when.compute(bar % 7 == 0, value, 0);
            observed = state.percentile.compute(value, 70.0);
            observed = state.change.compute(value, 113);
            const auto levels = state.pivots.compute(pineforge::ta::PivotLevelsType::Classic,
                true, true, value, value + 1, value - 1, value + 0.25);
            observed = levels[0];
            allocations += global_allocation::allocations - before;
        };
        drive(count, step);
        const auto resident_delta = global_allocation::live_bytes;
        std::printf("prepared_history bars=%zu allocations=%zu resident_added=%zu bound=0\n",
                    count, allocations, resident_delta);
        (void)observed;
        return allocations == 0 && resident_delta == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "prepared_history semantic/setup failure: %s\n", error.what());
        return 2;
    }
}

} // namespace history_growth_test
