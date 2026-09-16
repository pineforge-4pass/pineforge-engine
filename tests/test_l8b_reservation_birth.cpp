#include <pineforge/compat/pine/reservation_expansion.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

void reservation_selector_literals() {
    compat::pine::ReservationGrowthCandidate clean;
    clean.incarnation = 41;
    clean.market_entry = true;
    clean.is_long = true;
    clean.created_position_side = PositionSide::LONG;
    clean.created_bar = 3;
    CHECK(compat::pine::select_reservation_growth_sources(
              {clean}, "", true, false, 100.0, 3, PositionSide::LONG).size() == 1);

    auto fill_born = clean;
    fill_born.from_fill = true;
    CHECK(compat::pine::select_reservation_growth_sources(
              {fill_born}, "", true, false, 100.0, 3,
              PositionSide::LONG).empty());

    auto wrong_side_at_birth = clean;
    wrong_side_at_birth.created_position_side = PositionSide::SHORT;
    CHECK(compat::pine::select_reservation_growth_sources(
              {wrong_side_at_birth}, "", true, false, 100.0, 3,
              PositionSide::LONG).empty());
}

class FillBornReservationRoute final : public source::PineStrategyHost {
public:
    FillBornReservationRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 6;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = true;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0 && !seeded_) {
            seeded_ = true;
            strategy_entry("seed", true, kNaN, kNaN, 1.0);
            strategy_entry("A", true, kNaN, 105.0, 1.0);
            return;
        }
        if (pine_bar_index() != 1) return;
        if (broker_fill_event_seq_ >= 2 && !second_add_) {
            second_add_ = true;
            strategy_entry("B", true, kNaN, kNaN, 1.0);
            strategy_exit("X", "", 120.0, kNaN, kNaN, kNaN, kNaN, 100.0);
            for (const auto& candidate : adapter_.fixture_pending_snapshots()) {
                if (candidate.snapshot.source_id == "X") {
                    birth_cause_ = candidate.snapshot.birth.cause();
                    birth_reach_ = candidate.snapshot.birth_reach;
                    reservation_present_ =
                        candidate.snapshot.reservation_expansion.capture().has_value();
                    tracks_bound_adds_ =
                        candidate.snapshot.pooc_global_full_exit_tracks_bound_adds;
                    captured_ = true;
                    break;
                }
            }
            return;
        }
    }

    bool captured() const noexcept { return captured_; }
    OrderBirthCause birth_cause() const noexcept { return birth_cause_; }
    compat::pine::HistoricalBirthReach birth_reach() const noexcept {
        return birth_reach_;
    }
    bool reservation_present() const noexcept { return reservation_present_; }
    bool tracks_bound_adds() const noexcept { return tracks_bound_adds_; }
    std::uint64_t fills() const noexcept { return broker_fill_event_seq_; }

private:
    bool seeded_ = false;
    bool second_add_ = false;
    bool captured_ = false;
    OrderBirthCause birth_cause_ = OrderBirthCause::Unattributed;
    compat::pine::HistoricalBirthReach birth_reach_ =
        compat::pine::HistoricalBirthReach::Standard;
    bool reservation_present_ = false;
    bool tracks_bound_adds_ = false;
};

void fill_born_add_is_not_a_reservation_population() {
    FillBornReservationRoute route;
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 110, 90, 100, 1, 2'000},
        {100, 100, 100, 100, 1, 3'000},
    };
    route.run(bars, 3);
    CHECK(route.last_error().empty());
    CHECK(route.captured());
    if (!route.captured()) {
        std::fprintf(stderr, "reservation diagnostic: fills=%llu pending=%d\n",
                     static_cast<unsigned long long>(route.fills()),
                     route.pending_order_count());
        return;
    }
    CHECK(route.birth_cause() == OrderBirthCause::FillEvaluation);
    CHECK(route.birth_reach()
          == compat::pine::HistoricalBirthReach::ExtremeWaypoints);
    CHECK(!route.reservation_present());
    CHECK(!route.tracks_bound_adds());
}
} // namespace

int main() {
    reservation_selector_literals();
    fill_born_add_is_not_a_reservation_population();
    std::printf("L8b reservation/birth: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
