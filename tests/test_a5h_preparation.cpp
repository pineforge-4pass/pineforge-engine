// RED on main's lazy/deque/per-result allocation paths. Allocation count is
// taken from the repository's complete global C++17 replacement.
#include "global_allocation_replacement.hpp"
#include <pineforge/series.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace {
volatile double observed = 0.0;
int failures = 0;

template<class Action>
void flat(const char* name, Action action) {
    const auto before = global_allocation::allocations;
    action();
    const auto delta = global_allocation::allocations - before;
    std::printf("history_preparation %s allocations=%zu\n", name, delta);
    if (delta != 0) ++failures; // predetermined zero after preparation
}

template<class T, class Step>
void exercise(const char* name, T object, Step step) {
    T saved(object);
    flat(name, [&] {
        for (int bar = 0; bar < 2048; ++bar) {
            saved = object;
            const double src = 100.0 + (bar * 37 % 509) * 0.25;
            observed = step(object, src, false);
            observed = step(object, src + 0.5, true);
            if (bar % 13 == 0) object = saved;
        }
    });
}
template<class T>
void exercise(const char* name, T object) {
    exercise(name, std::move(object), [](auto& t, double src, bool again) {
        return again ? t.recompute(src) : t.compute(src);
    });
}
}

int main() {
    using namespace pineforge;
    Series<double> series(113), saved(113);
    flat("Series first push/copy", [&] {
        series.push(17.0); saved = series; series = saved; observed = series[0];
    });
    source::HeldHistory held, saved_held;
    flat("HeldHistory first push/copy", [&] {
        for (int bar = 0; bar < 40000; ++bar) {
            held.write(bar, bar * 0.25, 15001);
            saved_held = held;
            if (bar % 17 == 0) held = saved_held;
        }
        observed = held.at(held.last_bar());
    });
    exercise("EMA positive control", ta::EMA(113));
    exercise("RMA positive control", ta::RMA(113));
    exercise("SMA", ta::SMA(113));
    exercise("WMA", ta::WMA(113));
    exercise("VWMA", ta::VWMA(113), [](auto& t, double src, bool again) {
        return again ? t.recompute(src, 100.0) : t.compute(src, 100.0);
    });
    exercise("ALMA", ta::ALMA(113));
    exercise("SWMA", ta::SWMA{});
    exercise("MOM", ta::Mom(113));
    exercise("ROC", ta::ROC(113));
    exercise("Rising", ta::Rising(113));
    exercise("Falling", ta::Falling(113));
    exercise("CCI", ta::CCI(113));
    exercise("Variance", ta::Variance(113));
    exercise("StdDev", ta::StdDev(113, false));
    exercise("Dev", ta::Dev(113));
    exercise("CMO", ta::CMO(113));
    exercise("MFI", ta::MFI(113), [](auto& t, double src, bool again) {
        return again ? t.recompute(src, 100.0) : t.compute(src, 100.0);
    });
    exercise("COG", ta::COG(113));
    exercise("PivotHigh", ta::PivotHigh(17, 19));
    exercise("PivotLow", ta::PivotLow(17, 19));
    exercise("Median", ta::Median(113));
    exercise("Mode", ta::Mode(113));
    exercise("RCI", ta::RCI(113));
    exercise("PercentRank", ta::PercentRank(113));
    exercise("Linreg", ta::Linreg(113), [](auto& t, double src, bool again) {
        return again ? t.recompute(src, 0.0) : t.compute(src, 0.0);
    });
    exercise("Correlation", ta::Correlation(113), [](auto& t, double src, bool again) {
        return again ? t.recompute(src, src + 1) : t.compute(src, src + 1);
    });
    exercise("ValueWhen", ta::ValueWhen(17), [](auto& t, double src, bool again) {
        return again ? t.recompute(true, src, 0) : t.compute(true, src, 0);
    });
    ta::PercentileNearestRank percentile(113);
    flat("percentile", [&] {
        for (int bar = 0; bar < 2048; ++bar) {
            observed = percentile.compute(bar % 37, 70.0);
            observed = percentile.recompute(bar % 41, 70.0);
        }
    });
    ta::Change change(113);
    flat("declared Change", [&] {
        for (int bar = 0; bar < 2048; ++bar) {
            observed = change.compute(bar * 0.25, 113);
            observed = change.recompute(bar * 0.25 + 0.5, 113);
        }
    });
    ta::PivotPointLevels pivots;
    flat("pivot results", [&] {
        for (int bar = 0; bar < 2048; ++bar) {
            const double src = bar * 0.25 + 100.0;
            const auto levels = pivots.compute(ta::PivotLevelsType::Classic, true, true,
                                               src, src + 1, src - 1, src + 0.5);
            observed = levels[0];
        }
    });
    return failures == 0 ? 0 : 1;
}
