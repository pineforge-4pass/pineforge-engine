#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/reservation_expansion.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

void generic_capture_contract() {
    ReservationExpansion expansion;
    CHECK(!expansion.capture().has_value());
    expansion.capture(50, 7, PositionSide::LONG, 10);
    CHECK(expansion.capture().has_value());
    CHECK(expansion.capture()->position_cycle == 7);
    CHECK(expansion.capture()->side == PositionSide::LONG);
    CHECK(!expansion.capture()->first_later_admission.has_value());
    CHECK(expansion.population_open());
    CHECK(expansion.owns_exposure(7, PositionSide::LONG));
    CHECK(!expansion.owns_exposure(8, PositionSide::LONG));
    CHECK(!expansion.owns_exposure(7, PositionSide::SHORT));
    CHECK(expansion.live_all(7, PositionSide::LONG));
    CHECK(!expansion.live_all(7, PositionSide::SHORT));
    double qty = 10;
    expansion.grow(qty, 7, PositionSide::LONG, 10, 7, PositionSide::LONG, 12, 1e-9);
    CHECK(qty == 12);
    expansion.close_population(51);
    CHECK(!expansion.population_open());
    CHECK(expansion.capture()->first_later_admission.has_value());
    CHECK(*expansion.capture()->first_later_admission == 51);
    CHECK(!expansion.live_all(7, PositionSide::LONG));
    qty = 10;
    expansion.grow(qty, 7, PositionSide::LONG, 10, 7, PositionSide::LONG, 14, 1e-9);
    CHECK(qty == 14);

    ReservationExpansion short_side;
    short_side.capture(70, 9, PositionSide::SHORT, 4);
    CHECK(short_side.population_open());
    CHECK(short_side.owns_exposure(9, PositionSide::SHORT));
    qty = 4;
    short_side.grow(qty, 9, PositionSide::SHORT, 4, 9, PositionSide::SHORT, 6, 1e-9);
    CHECK(qty == 6);
    short_side.close_population(71);
    CHECK(!short_side.population_open());

    ReservationGrowthSource source;
    CHECK(!source.reservation_owner().has_value());
    source.assign_capture(41, 50);
    CHECK(source.reservation_owner().has_value());
    CHECK(*source.reservation_owner() == 50);
    bool rejected = false;
    try { ReservationExpansion bad; bad.capture(0, 7, PositionSide::LONG, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationExpansion bad; bad.capture(1, 0, PositionSide::LONG, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationExpansion bad; bad.capture(1, 7, PositionSide::FLAT, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationExpansion bad; bad.capture(1, 7, PositionSide::LONG, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationGrowthSource bad; bad.assign_capture(0, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationGrowthSource bad; bad.assign_capture(1, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { ReservationGrowthSource bad; bad.assign_capture(1, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

class PublicRoute final : public pineforge::source::PineStrategyHost {
public:
    PublicRoute() {
        process_orders_on_close_ = true;
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        pyramiding_ = 3;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, nan(), nan(), 1);
        if (bar_index_ == 1) {
            strategy_entry("A", true, nan(), nan(), 1);
            strategy_exit("X", "", 110, nan(), nan(), nan(), nan(), 100, "global");
            rows = l4c_pending_orders();
        }
    }
    static double nan() { return std::numeric_limits<double>::quiet_NaN(); }
    std::vector<pineforge::source::L4cPendingOrder> rows;
};

void public_projection_contract() {
    PublicRoute route;
    const Bar bars[] = {{100,100,100,100,1,0}, {100,100,100,100,1,60000},
                        {100,111,99,100,1,120000}};
    route.run(bars, 3);
    CHECK(route.last_error().empty());
    CHECK(!route.rows.empty());
    bool found = false;
    for (const auto& row : route.rows) {
        if (row.id == "X") {
            found = true;
            CHECK(row.type == pineforge::source::L4cOrderType::EXIT);
            CHECK(row.qty_percent == 100);
            CHECK(row.reservation_expansion.present || std::isnan(row.qty));
            CHECK(row.from_entry.empty());
        }
    }
    CHECK(found);
}
} // namespace

int main() {
    generic_capture_contract();
    public_projection_contract();
    std::printf("reservation expansion: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
