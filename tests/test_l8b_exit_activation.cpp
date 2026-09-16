#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

void review_literal_selector() {
    using namespace compat::pine;
    ExitActivationContext context;
    context.cycle = 7;
    context.bar_index = 5;
    context.position_open_bar = 5;
    context.direction = 1;
    context.cursor_price = 110.0;
    context.fill_recalc = true;
    context.scheduler = true;
    context.after_first_open_fill = true;
    context.recalc_leg = 0;
    context.current_fill = 19;

    ExitActivationRequest from_fill;
    from_fill.requested_trailing = false;
    from_fill.full_quantity = true;
    from_fill.from_fill = true;
    from_fill.has_from_entry = true;
    from_fill.birth_reach = HistoricalBirthReach::ExtremeWaypoints;
    const auto later = select_exit_activation(from_fill, kNaN, 105.0, context);
    CHECK(later.evidence().has_value());
    CHECK(later.continues_at_later_open());
    CHECK(later.resolve(7, 5).limit_first_bar == 5);

    ExitActivationRequest chart = from_fill;
    chart.from_fill = false;
    chart.birth_reach = HistoricalBirthReach::Standard;
    const auto ordinary = select_exit_activation(chart, kNaN, 105.0, context);
    CHECK(ordinary.evidence().has_value());
    CHECK(!ordinary.continues_at_later_open());
    CHECK(ordinary.resolve(7, 5).limit_first_bar == 6);

    context.position_open_bar = 4;
    CHECK(!select_exit_activation(from_fill, kNaN, 105.0, context)
               .evidence().has_value());
}

void first_high_recross_selector() {
    using namespace compat::pine;
    ExitActivationContext context;
    context.cycle = 9;
    context.bar_index = 7;
    context.position_open_bar = 7;
    context.direction = 1;
    context.cursor_price = 105.0;
    context.fill_recalc = true;
    context.scheduler = true;
    context.stream_idle = true;
    context.recalc_leg = 1;
    context.at_extreme = true;
    context.historical_point = 1;
    context.current_fill = 23;
    context.bar = {100, 105, 90, 101, 1, 7'000};
    context.position_entry_count = 1;
    context.position_quantity = 1.0;
    context.pyramiding = 0;
    context.lot_count = 1;
    context.first_lot_id = "E";
    context.first_lot_incarnation = 42;
    context.market_recalc_incarnation = 42;
    context.market_recalc_fill = 23;
    context.pending_empty = true;
    context.pointvalue = 1.0;
    context.account_fx = 1.0;
    context.fx_series_empty = true;
    context.bar_path_high_first = true;
    context.tick_high = 105.0;

    ExitActivationRequest request;
    request.full_quantity = true;
    request.has_from_entry = true;
    request.birth_reach = HistoricalBirthReach::ExtremeWaypoints;
    request.from_entry = "E";
    request.quantity = 1.0;
    const auto selected = select_exit_activation(request, kNaN, 103.0, context);
    CHECK(selected.evidence().has_value());
    CHECK(selected.evidence()->limit_continuation.has_value());
    CHECK(selected.evidence()->limit_continuation->cause
          == LimitContinuationCause::FirstHighRecross);
    CHECK(selected.resolve(9, 7).limit_first_bar == 7);
}

class LaterSameOpenRoute final : public source::PineStrategyHost {
public:
    LaterSameOpenRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 4;
        config.calc_on_order_fills = true;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0 && !seeded_) {
            seeded_ = true;
            strategy_entry("E", true, kNaN, kNaN, 1.0);
            return;
        }
        if (pine_bar_index() != 1) return;
        if (broker_fill_event_seq_ == 1 && !same_open_add_) {
            same_open_add_ = true;
            strategy_entry("A", true, kNaN, kNaN, 1.0);
            return;
        }
        if (broker_fill_event_seq_ < 2 || exit_issued_) return;
        exit_issued_ = true;
        strategy_exit("X", "E", 99.0, kNaN);
        const auto& view = pending_intent_view();
        for (int index = 0; index < view.size(); ++index) {
            pf_pending_order_v1_t candidate{};
            if (view.copy_v1(index, &candidate) == 0
                && std::strcmp(candidate.id, "X") == 0) {
                mirror_ = candidate;
                captured_ = true;
                break;
            }
        }
    }

    bool captured() const noexcept { return captured_; }
    const pf_pending_order_v1_t& mirror() const noexcept { return mirror_; }

private:
    bool seeded_ = false;
    bool same_open_add_ = false;
    bool exit_issued_ = false;
    bool captured_ = false;
    pf_pending_order_v1_t mirror_{};
};

void native_route_later_same_open() {
    LaterSameOpenRoute route;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 112, 94, 100, 1, 2'000},
        {100, 100, 100, 100, 1, 3'000},
    };
    route.run(bars, 3);
    CHECK(route.last_error().empty());
    CHECK(route.captured());
    if (!route.captured()) return;
    const auto& row = route.mirror();
    CHECK(row.birth_cause == static_cast<std::int32_t>(OrderBirthCause::FillEvaluation));
    CHECK(row.pine_birth_reach
          == static_cast<std::int32_t>(compat::pine::HistoricalBirthReach::ExtremeWaypoints));
    CHECK(row.pine_exit_activation_present == 1U);
    CHECK(row.pine_exit_activation_limit_continuation_present == 1U);
    CHECK(row.pine_exit_activation_limit_continuation_cause
          == static_cast<std::int32_t>(compat::pine::LimitContinuationCause::LaterSameOpen));
    CHECK(row.pine_exit_activation_entry_bar_at_birth == 1);
    CHECK(row.leg_activation_limit_first_bar == 1);
}
} // namespace

int main() {
    review_literal_selector();
    first_high_recross_selector();
    native_route_later_same_open();
    std::printf("L8b exit activation: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
