#include "exit_lifecycle_fixture.hpp"
// R28 covered TV controls: integer short margin events, including a bracket
// revived after an opening declined reversal, precede close-time script state.
// Evidence: r28-killed-dynamic, r28-killed-explicit-child, r28-killed-funded,
// carried-half, opening-half and r28-unit-carried-high-state campaign tapes.
// The last one's TV CSV SHA is
// eb3a2e2fd74a9a56560b0525ff56126aa085d6117798fc49dfe0b8247cd14db1.
// These compact command fixtures use synthetic timestamps, fixed distances,
// and a short bar sequence; they do not run a corpus strategy or a verifier.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
constexpr double qnan = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }

enum class Mode { OPENING_HALF, CARRIED_HALF, DYNAMIC, EXPLICIT, UNIT };
class IntegerScript : public pineforge::source::PineStrategyHost {
public:
    Mode mode;
    double opening_view = qnan, carried_view = qnan, carried_average = qnan;
    double carried_balance = qnan;
    std::size_t carried_closed = 0;
    uint64_t explicit_child_incarnation = 0, filled_parent_child_incarnation = 0;
    explicit IntegerScript(Mode value, double capital = 10315.59)
        : mode(value) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        qty_step_ = 1.0;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        margin_short_ = 100.0;
        margin_long_ = mode == Mode::UNIT ? 50.0 : 100.0;
        pyramiding_ = 0;
    }
    void reverse(double distance) {
        const double average = signed_position_size() == 0.0
            ? qnan : position_entry_price_;
        strategy_entry("L", true);
        if (mode == Mode::EXPLICIT && bar_index_ == 2) {
            strategy_exit("XL", "L", 10.63, 10.49);
        } else {
            strategy_exit("XL", "L", average + 2.0 * distance, average - distance);
        }
        strategy_exit("XS", "S", average - 2.0 * distance, average + distance);
        strategy_close("S");
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("S", false, qnan, qnan, mode == Mode::UNIT ? 1.0 : 991.0);
        if (bar_index_ == 1) {
            opening_view = signed_position_size();
            if (mode == Mode::OPENING_HALF) strategy_close("S", "half", std::floor(-opening_view / 2.0));
            if (mode == Mode::DYNAMIC || mode == Mode::EXPLICIT) reverse(0.034233333969624);
        }
        if (bar_index_ == 2) {
            carried_view = signed_position_size();
            carried_average = carried_view == 0.0 ? qnan : position_entry_price_;
            carried_balance = current_equity();
            carried_closed = trades_.size();
            if (mode == Mode::CARRIED_HALF) strategy_close("S", "half", std::floor(-carried_view / 2.0));
            if (mode == Mode::DYNAMIC || mode == Mode::EXPLICIT) {
                reverse(0.040359524400365);
                if (mode == Mode::EXPLICIT) {
                    for (const auto& order : pending_orders_) {
                        if (order.id == "XL") explicit_child_incarnation = order.incarnation;
                    }
                }
            }
            if (mode == Mode::UNIT) strategy_entry("Observer", true, qnan, qnan, 1.0);
        }
        if (mode == Mode::EXPLICIT && bar_index_ == 3) {
            for (const auto& order : pending_orders_) {
                if (order.id == "XL") filled_parent_child_incarnation = order.incarnation;
            }
        }
        // The EXPLICIT child must survive on its original incarnation;
        // reissuing it here would mask a lost pending-parent bracket.
        if (signed_position_size() > 0.0 && bar_index_ >= 3 && mode == Mode::DYNAMIC) {
            strategy_exit("XL", "L", 10.63, 10.49);
        }
        if (bar_index_ == 5) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

const std::vector<Bar> bars = {
    {10.415, 10.415, 10.39, 10.395, 1, 1000},
    {10.395, 10.44, 10.38, 10.44, 1, 2000},
    {10.50, 10.56, 10.49, 10.54, 1, 3000},
    {10.535, 10.60, 10.535, 10.56, 1, 4000},
    {10.565, 10.61, 10.56, 10.605, 1, 5000},
    {10.605, 10.66, 10.605, 10.645, 1, 6000},
    {10.61, 10.61, 10.61, 10.61, 1, 7000},
};

void test_revival_precedes_replacement_script() {
    for (Mode mode : {Mode::DYNAMIC, Mode::EXPLICIT}) {
        IntegerScript engine(mode);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, -967.0));
        CHECK(near(engine.carried_view, 0.0));
        CHECK(std::isnan(engine.carried_average));
        CHECK(near(engine.carried_balance, 10159.91));
        CHECK(engine.carried_closed == 3);
        if (mode == Mode::EXPLICIT) {
            CHECK(engine.explicit_child_incarnation != 0);
            CHECK(engine.filled_parent_child_incarnation == engine.explicit_child_incarnation);
        }
        CHECK(engine.rows().size() == 4);
        if (engine.rows().size() != 4) continue;
        CHECK(engine.rows()[0].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[0].qty, 24.0));
        CHECK(near(engine.rows()[0].exit_price, 10.44));
        CHECK(engine.rows()[1].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[1].qty, 16.0));
        CHECK(engine.rows()[1].exit_time == 3000);
        CHECK(near(engine.rows()[1].exit_price, 10.56));
        CHECK(engine.rows()[2].exit_id == "XS");
        CHECK(near(engine.rows()[2].qty, 951.0));
        CHECK(engine.rows()[2].exit_time == 3000);
        CHECK(near(engine.rows()[2].exit_price, 10.56));
        CHECK(engine.rows()[3].entry_time == 4000);
        CHECK(near(engine.rows()[3].qty, 963.0));
        CHECK(near(engine.rows()[3].entry_price, 10.54));
        CHECK(engine.rows()[3].exit_id == "XL");
        CHECK(engine.rows()[3].exit_time == 6000);
        CHECK(near(engine.rows()[3].exit_price, 10.63));
    }
}

void test_partial_state_and_funded_control() {
    for (Mode mode : {Mode::OPENING_HALF, Mode::CARRIED_HALF}) {
        IntegerScript engine(mode);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, -967.0));
        CHECK(engine.rows().size() == (mode == Mode::OPENING_HALF ? 3 : 4));
        if (mode == Mode::CARRIED_HALF) CHECK(near(engine.carried_view, -951.0));
        bool found_half = false;
        for (const auto& trade : engine.rows()) {
            if (trade.exit_comment != "half") continue;
            found_half = true;
            CHECK(near(trade.qty, mode == Mode::OPENING_HALF ? 483.0 : 475.0));
        }
        CHECK(found_half);
    }
    IntegerScript funded(Mode::DYNAMIC, 11315.59);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.opening_view, -991.0));
    CHECK(near(funded.carried_view, -991.0));
    CHECK(funded.carried_closed == 0);
    CHECK(funded.rows().size() == 2);
    if (funded.rows().size() != 2) return;
    CHECK(funded.rows()[0].exit_time == 4000);
    CHECK(near(funded.rows()[0].qty, 991.0));
    CHECK(funded.rows()[1].entry_time == 4000);
    CHECK(funded.rows()[1].exit_time == 4000);
    CHECK(funded.rows()[1].exit_id == "XL");
    CHECK(near(funded.rows()[1].qty, 1060.0));
}

void test_one_unit_adverse_high() {
    for (double capital : {10.4, 10.5, 10.6}) {
        IntegerScript engine(Mode::UNIT, capital);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.opening_view, capital == 10.4 ? 0.0 : -1.0));
        CHECK(near(engine.carried_view, 0.0));
        CHECK(engine.carried_closed == 1);
        CHECK(engine.rows().size() == 2);
        if (engine.rows().size() != 2) continue;
        CHECK(engine.rows()[0].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[0].qty, 1.0));
        CHECK(engine.rows()[0].exit_time == (capital == 10.4 ? 2000 : 3000));
        CHECK(near(engine.rows()[0].exit_price,
                   capital == 10.4 ? 10.44 : (capital == 10.5 ? 10.50 : 10.56)));
    }
}

// Broker snapshots exercise the lifetime boundary independently of the
// strategy command sequence: an unknown/old/held/reissued/trailing dormant
// bracket must keep its old scheduler, as must integer STOP-origin books.
enum class Shape { CURRENT, UNKNOWN, OLD, FUTURE, HELD, OLD_HOLD, REISSUED, TRAIL,
                   FOREIGN, GLOBAL, UNPRICED, STOP_ORIGIN, OFF_GRID, BIG_STEP,
                   LIMIT_ONLY, PARTIAL, COARSE_FRACTIONAL, TRAIL_OFFSET, NAKED,
                   PENDING_ENTRY, INFINITE_PERCENT };
class DormantCheckpoint : public pineforge::source::PineStrategyHost {
public:
    explicit DormantCheckpoint(Shape shape) {
        initial_capital_ = 10000.0;
        current_bar_ = {100.0, 102.0, 99.0, 100.0, 1, 2000};
        bar_index_ = 1;
        position_open_bar_ = 0;
        position_side_ = PositionSide::SHORT;
        position_qty_ = 100.0;
        position_entry_price_ = 100.0;
        position_entry_time_ = 1000;
        position_entry_count_ = 1;
        position_cycle_seq_ = 1;
        qty_step_ = shape == Shape::BIG_STEP ? 2.0 : 1.0;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        PyramidEntry entry{};
        entry.price = 100.0;
        entry.qty = position_qty_;
        entry.time = 1000;
        entry.entry_id = "S";
        entry.entry_bar_index = 0;
        entry.entry_incarnation = 7;
        entry.ordinary_market_open = true;
        pyramid_entries_.push_back(entry);
        cycle_filled_entry_ids_.insert("S");
        PendingOrder order{};
        order.id = "XS";
        order.incarnation = 8; // explicit identity for this synthetic native checkpoint
        order.type = OrderType::EXIT;
        order.from_entry = "S";
        order.legs.set_limit_price(order.legs.set_trail_points(order.legs.set_trail_offset(order.qty = qnan)));
        order.qty_percent = 100.0;
        order.legs.set_stop_price(101.0);
        // Snapshot of the private opening-decline producer. The command
        // fixture above separately exercises that producer through orders.
        lifecycle_fixture::suspend(order);
        lifecycle_fixture::suspend(order, bar_index_);
        pending_orders_.push_back(order);
        auto& owned = pending_orders_.front();
        switch (shape) {
        case Shape::UNKNOWN: lifecycle_fixture::suspend(owned, std::nullopt); break;
        case Shape::OLD: lifecycle_fixture::suspend(owned, 0); break;
        case Shape::FUTURE: lifecycle_fixture::suspend(owned, 2); break;
        case Shape::HELD: lifecycle_fixture::suspend(owned, owned.legs.excluded_bar(), 1); break;
        case Shape::OLD_HOLD: lifecycle_fixture::suspend(owned, owned.legs.excluded_bar(), 0); break;
        case Shape::REISSUED: lifecycle_fixture::stage(owned); break;
        case Shape::TRAIL: owned.legs.set_trail_price(100.5); break;
        case Shape::FOREIGN: owned.from_entry = "Other"; break;
        case Shape::GLOBAL: owned.from_entry.clear(); break;
        case Shape::UNPRICED: owned.legs.set_stop_price(qnan); break;
        case Shape::STOP_ORIGIN:
            pyramid_entries_[0].ordinary_market_open = false;
            pyramid_entries_[0].ordinary_stop_open = true;
            break;
        case Shape::OFF_GRID: position_qty_ = pyramid_entries_[0].qty = 100.5; break;
        case Shape::LIMIT_ONLY: owned.legs.set_stop_price(qnan); owned.legs.set_limit_price(98.0); break;
        case Shape::PARTIAL:
            owned.qty = 50.0;
            owned.quantity_request.request(QuantityIntent::units(50.0));
            owned.quantity_request.reserve(50.0, 100.0);
            break;
        case Shape::COARSE_FRACTIONAL:
            qty_step_ = 1.5;
            position_qty_ = pyramid_entries_[0].qty = 100.5;
            break;
        case Shape::TRAIL_OFFSET: owned.legs.set_trail_offset(1.0); break;
        case Shape::PENDING_ENTRY:
            owned.type = OrderType::ENTRY;
            owned.legs.set_stop_price(110.0);
            lifecycle_fixture::restore(owned);
            break;
        case Shape::INFINITE_PERCENT: owned.qty_percent = INFINITY; break;
        case Shape::NAKED: pending_orders_.clear(); break;
        default: break;
        }
    }
    void on_source_bar(const Bar&) override {}
    void checkpoint() { process_short_margin_before_script(current_bar_); }
    void late_margin() { process_margin_call(current_bar_); }
    const std::vector<Trade>& rows() const { return trades_; }
    double quantity() const { return position_qty_; }
    std::size_t pending() const { return pending_orders_.size(); }
};

void test_dormant_lifetime_boundary() {
    for (Shape shape : {Shape::CURRENT, Shape::BIG_STEP}) {
        DormantCheckpoint owned(shape);
        owned.checkpoint();
        CHECK(owned.rows().size() == 2);
        CHECK(owned.quantity() == 0.0);
        CHECK(owned.pending() == 0);
        if (owned.rows().size() != 2) continue;
        CHECK(owned.rows()[0].exit_id == "__margin_call__");
        CHECK(near(owned.rows()[0].qty, shape == Shape::CURRENT ? 12.0 : 8.0));
        CHECK(owned.rows()[1].exit_id == "XS");
        CHECK(near(owned.rows()[0].exit_price, 102.0));
        CHECK(near(owned.rows()[1].exit_price, 102.0));
        owned.late_margin();
        CHECK(owned.rows().size() == 2);
    }
    for (Shape shape : {Shape::UNKNOWN, Shape::OLD, Shape::FUTURE, Shape::HELD,
                        Shape::OLD_HOLD, Shape::REISSUED, Shape::TRAIL,
                        Shape::FOREIGN, Shape::GLOBAL, Shape::UNPRICED,
                        Shape::STOP_ORIGIN, Shape::OFF_GRID, Shape::LIMIT_ONLY,
                        Shape::PARTIAL, Shape::COARSE_FRACTIONAL,
                        Shape::TRAIL_OFFSET, Shape::PENDING_ENTRY, Shape::INFINITE_PERCENT}) {
        DormantCheckpoint other(shape);
        const double before = other.quantity();
        other.checkpoint();
        CHECK(other.rows().empty());
        CHECK(other.quantity() == before);
        CHECK(other.pending() == 1);
    }
    DormantCheckpoint naked(Shape::NAKED);
    naked.checkpoint();
    CHECK(naked.rows().size() == 1);
    CHECK(near(naked.quantity(), 88.0));
    naked.late_margin();
    CHECK(naked.rows().size() == 1);
    CHECK(near(naked.quantity(), 88.0));
}
}

int main() {
    test_revival_precedes_replacement_script();
    test_partial_state_and_funded_control();
    test_one_unit_adverse_high();
    test_dormant_lifetime_boundary();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
