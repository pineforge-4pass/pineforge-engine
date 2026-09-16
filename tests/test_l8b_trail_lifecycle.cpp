#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", \
                             __FILE__, __LINE__, #expression); } } while (false)

bool retires_trail(double entry, double tick, double points,
                   int direction, double open) {
    exit_legs::Lifecycle lifecycle;
    lifecycle.set_prices({kNaN, 95.0, points, kNaN, 1.0, kNaN, kNaN});
    const auto operation = compat::pine::select_exit_suspension(
        lifecycle, {{7, 3, exit_legs::Domain::Ordinary,
                     exit_legs::Phase::Observation},
                    direction, entry, tick, open, kNaN, false, true});
    if (!operation) return false;
    const auto* suspension = std::get_if<exit_legs::Suspend>(&*operation);
    return suspension
        && std::find(suspension->retire.begin(), suspension->retire.end(),
                     exit_legs::Leg::Trail) != suspension->retire.end();
}

void review_literal_tick_snapping() {
    CHECK(retires_trail(10.11, 0.01, 21.0, -1, 9.9));
    CHECK(!retires_trail(100.0, 0.01, 7.54, 1, 100.076));
}

class EffectiveLevelsRoute final : public source::PineStrategyHost {
public:
    EffectiveLevelsRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, kNaN, kNaN, 1.0);
        if (pine_bar_index() != 1 || captured_) return;
        strategy_exit("X", "L", kNaN, kNaN, 3.4, 1.0, 105.0);
        const auto& view = pending_intent_view();
        for (int index = 0; index < view.size(); ++index) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(index, &row) != 0 || std::strcmp(row.id, "X") != 0)
                continue;
            captured_ = view.effective_levels(index, &stop_, &limit_, &trail_) == 0;
            break;
        }
    }

    bool captured() const noexcept { return captured_; }
    double trail() const noexcept { return trail_; }

private:
    bool captured_ = false;
    double stop_ = kNaN;
    double limit_ = kNaN;
    double trail_ = kNaN;
};

class UnresolvedLevelsRoute final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0 || captured_) return;
        strategy_entry("future", true, 90.0, kNaN, 1.0);
        strategy_exit("X", "future", kNaN, kNaN, 3.4, 1.0, 105.0);
        const auto& view = pending_intent_view();
        for (int index = 0; index < view.size(); ++index) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(index, &row) != 0 || std::strcmp(row.id, "X") != 0)
                continue;
            captured_ = view.effective_levels(index, &stop_, &limit_, &trail_) == 0;
            break;
        }
    }

    bool captured() const noexcept { return captured_; }
    double trail() const noexcept { return trail_; }

private:
    bool captured_ = false;
    double stop_ = kNaN;
    double limit_ = kNaN;
    double trail_ = kNaN;
};

SymInfo symbol() {
    SymInfo info;
    info.mintick = 0.05;
    info.pointvalue = 1.0;
    info.qty_step = 0.0;
    info.timezone = "UTC";
    info.session = "24x7";
    return info;
}

void native_effective_levels() {
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 100, 100, 100, 1, 2'000},
    };
    InputsMap inputs;
    EffectiveLevelsRoute resolved;
    resolved.run(bars, 2, "1", "1", inputs, symbol());
    CHECK(resolved.last_error().empty());
    CHECK(resolved.captured());
    CHECK(std::isfinite(resolved.trail()));
    CHECK(std::abs(resolved.trail() - 100.20) < 1e-12);

    UnresolvedLevelsRoute unresolved;
    unresolved.run(bars, 2, "1", "1", inputs, symbol());
    CHECK(unresolved.last_error().empty());
    CHECK(unresolved.captured());
    CHECK(std::isnan(unresolved.trail()));
}
} // namespace

int main() {
    review_literal_tick_snapping();
    native_effective_levels();
    std::printf("L8b trail lifecycle: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
