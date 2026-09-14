#include "placement_observation_fixture.hpp"
// R25 covered TV controls: a pure STOP filled at the opening point exposes
// its completed margin event to the script; an unhit pending entry survives.
// Compact command fixtures use synthetic timestamps, not historical replay.
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
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

const std::vector<Bar> bars = {
    {114643.19, 114781.21, 114555.0, 114555.0, 1, 1000},
    {114555.0, 114564.69, 114350.57, 114400.0, 1, 2000},
    {114400.01, 114600.94, 114378.99, 114454.93, 1, 3000},
    {114454.93, 114521.97, 114402.65, 114437.7, 1, 4000},
    {114437.71, 114657.0, 114437.7, 114514.05, 1, 5000},
    {114514.05, 114865.32, 114449.91, 114697.22, 1, 6000},
};

class StopBook : public pineforge::source::PineStrategyHost {
public:
    bool opposite, half_close, smaller, carried_half;
    double first_view = qnan;
    double carried_view = qnan;
    std::size_t first_closed = 0;
    StopBook(bool other = true, double capital = 9064.3344809999962,
             bool half = false, bool less = false, bool carry = false)
        : opposite(other), half_close(half), smaller(less), carried_half(carry) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 1) {
            first_view = signed_position_size();
            first_closed = trades_.size();
        }
        if (bar_index_ == 2) carried_view = signed_position_size();
        if (bar_index_ <= 1 && signed_position_size() == 0) {
            if (opposite) strategy_entry("Long", true, qnan, 117030.0, current_equity() / 117030.0);
            const double quantity = smaller ? 0.07911 : current_equity() / 114560.0;
            strategy_entry("Short", false, qnan, 114560.0, quantity);
        }
        if (signed_position_size() < 0) {
            strategy_exit("Exit Short", "Short", qnan, 117030.0);
            strategy_cancel("Long");
        }
        if (signed_position_size() > 0) {
            strategy_exit("Exit Long", "Long", qnan, 114560.0);
            strategy_cancel("Short");
        }
        if (half_close && bar_index_ == 1) strategy_close("Short", "half", qnan, 50.0);
        if (carried_half && bar_index_ == 2) strategy_close("Short", "carry half", qnan, 50.0);
        if (bar_index_ == 4) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_full_stop_liquidation_and_replacement() {
    for (bool opposite : {false, true}) {
        StopBook engine(opposite);
        engine.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(near(engine.first_view, 0));
        CHECK(engine.first_closed == 1);
        CHECK(engine.rows().size() == 3);
        if (engine.rows().size() != 3) continue;
        CHECK(engine.rows()[0].exit_id == "__margin_call__");
        CHECK(engine.rows()[0].exit_time == 2000);
        CHECK(near(engine.rows()[0].qty, 0.07912));
        CHECK(near(engine.rows()[0].exit_price, 114564.69));
        CHECK(engine.rows()[1].entry_time == 3000);
        CHECK(engine.rows()[1].exit_id == "__margin_call__");
        CHECK(near(engine.rows()[1].qty, 0.00064));
        CHECK(near(engine.rows()[1].entry_price, 114400.01));
        CHECK(engine.rows()[2].exit_time == 6000);
        CHECK(near(engine.rows()[2].qty, 0.07847));
        CHECK(near(engine.rows()[2].exit_price, 114514.05));
    }
}

void test_partial_and_no_opening_event() {
    StopBook partial(true, 11456.0, true);
    partial.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(partial.first_view, -0.09996));
    CHECK(partial.first_closed == 1);
    CHECK(partial.rows().size() == 3);
    if (partial.rows().size() == 3) {
        CHECK(partial.rows()[0].exit_id == "__margin_call__");
        CHECK(near(partial.rows()[0].qty, 0.00004));
        CHECK(partial.rows()[1].exit_comment == "half");
        CHECK(near(partial.rows()[1].qty, 0.04998));
        CHECK(near(partial.rows()[2].qty, 0.04998));
    }
    StopBook funded(true, 9064.3344809999962, false, true);
    funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(funded.first_view, -0.07911));
    CHECK(funded.first_closed == 0);
    CHECK(funded.rows().size() == 2);
    if (funded.rows().size() == 2) {
        CHECK(funded.rows()[0].exit_time == 3000);
        CHECK(near(funded.rows()[0].qty, 0.00016));
        CHECK(near(funded.rows()[1].qty, 0.07895));
    }
    // TV's carried-bar comment reads -0.07895 before the 50% close; it then
    // closes 0.03947 and retains 0.03948. The original STOP's open provenance
    // remains attached to the same physical lot across this partial.
    StopBook carried(true, 9064.3344809999962, false, true, true);
    carried.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(near(carried.carried_view, -0.07895));
    CHECK(carried.rows().size() == 3);
    if (carried.rows().size() == 3) {
        CHECK(carried.rows()[0].exit_time == 3000);
        CHECK(near(carried.rows()[0].qty, 0.00016));
        CHECK(carried.rows()[1].exit_time == 4000);
        CHECK(carried.rows()[1].exit_comment == "carry half");
        CHECK(near(carried.rows()[1].qty, 0.03947));
        CHECK(near(carried.rows()[2].qty, 0.03948));
    }
}

class PathAndLifetime : public pineforge::source::PineStrategyHost {
public:
    bool preserve;
    double first_view = qnan;
    PathAndLifetime(bool keep) : preserve(keep) {
        initial_capital_ = 9064.3344809999962;
        qty_step_ = 0.00001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            if (preserve) strategy_entry("Long", true, qnan, 117030.0, 0.01);
            strategy_entry("Short", false, qnan, preserve ? 114560.0 : 114500.0, 0.07912);
        }
        if (bar_index_ == 1) {
            first_view = signed_position_size();
            if (!preserve) strategy_close_all();
        }
        if (preserve && signed_position_size() > 0) strategy_close_all();
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

void test_prior_high_and_pending_entry_lifetime() {
    PathAndLifetime path(false);
    path.run(bars.data(), 3);
    CHECK(near(path.first_view, -0.07912));
    CHECK(path.rows().size() == 1);
    if (path.rows().size() == 1) {
        CHECK(path.rows()[0].entry_time == 2000);
        CHECK(near(path.rows()[0].entry_price, 114500.0));
        CHECK(near(path.rows()[0].qty, 0.07912));
        CHECK(path.rows()[0].exit_time == 3000);
    }
    std::vector<Bar> later = {bars[0], bars[1],
        {116900.0, 117040.0, 116890.0, 117010.0, 1, 3000},
        {116686.43, 116800.0, 116600.0, 116700.0, 1, 4000}};
    PathAndLifetime keep(true);
    keep.run(later.data(), static_cast<int>(later.size()));
    CHECK(near(keep.first_view, 0));
    CHECK(keep.rows().size() == 2);
    if (keep.rows().size() == 2) {
        CHECK(keep.rows()[0].exit_id == "__margin_call__");
        CHECK(keep.rows()[1].is_long);
        CHECK(keep.rows()[1].entry_time == 3000);
        CHECK(near(keep.rows()[1].entry_price, 117030.0));
        CHECK(near(keep.rows()[1].qty, 0.01));
    }
}

enum class Origin { STOP, MARKET, LIMIT, STOP_LIMIT, RAW_STOP, OCA_STOP,
                    REPLACED_STOP, REUSED_ID, ZERO_STOP, DECLINED_ADD };
class OriginBook : public pineforge::source::PineStrategyHost {
public:
    Origin mode;
    bool stop_origin = false, market_origin = false, final_stop = false;
    uint64_t first_incarnation = 0, final_incarnation = 0;
    explicit OriginBook(Origin value) : mode(value) {
        initial_capital_ = 1000.0;
        qty_step_ = 0.01;
        syminfo_mintick_ = 0.01;
        pyramiding_ = 0;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            if (mode == Origin::RAW_STOP) {
                strategy_order("S", false, 0.5, qnan, 101.0);
            } else {
                const bool market = mode == Origin::MARKET
                    || mode == Origin::ZERO_STOP || mode == Origin::DECLINED_ADD;
                const double limit = mode == Origin::LIMIT || mode == Origin::STOP_LIMIT ? 99.0 : qnan;
                const double stop = market || mode == Origin::LIMIT ? qnan : 101.0;
                if (mode == Origin::REPLACED_STOP)
                    strategy_entry("S", false, qnan, 99.0, 0.4);
                strategy_entry("S", false, limit, stop, 0.5, "",
                    mode == Origin::OCA_STOP ? "siblings" : "",
                    mode == Origin::OCA_STOP ? 1 : 0);
            }
        }
        if (bar_index_ == 1 && !pyramid_entries_.empty()) {
            const auto& entry = pyramid_entries_.front();
            stop_origin = entry.ordinary_stop_open;
            market_origin = entry.ordinary_market_open;
            first_incarnation = entry.entry_incarnation;
            if (mode == Origin::REUSED_ID) {
                strategy_close("S", "", qnan, qnan, true);
                strategy_entry("S", false, qnan, qnan, 0.5);
            }
            if (mode == Origin::ZERO_STOP || mode == Origin::DECLINED_ADD)
                strategy_entry("S", false, qnan, 101.0,
                    mode == Origin::ZERO_STOP ? 0.0 : 0.5);
        }
        if (bar_index_ == 2 && !pyramid_entries_.empty()) {
            final_stop = pyramid_entries_.front().ordinary_stop_open;
            final_incarnation = pyramid_entries_.front().entry_incarnation;
        }
    }
};

void test_origin_is_an_accepted_physical_stop_fill() {
    const Bar tape[] = {{100, 101, 99, 100, 1, 1000},
                        {100, 101, 99, 100, 1, 2000},
                        {100, 101, 99, 100, 1, 3000}};
    for (Origin mode : {Origin::STOP, Origin::MARKET, Origin::LIMIT,
                        Origin::STOP_LIMIT, Origin::RAW_STOP, Origin::OCA_STOP,
                        Origin::REPLACED_STOP, Origin::REUSED_ID,
                        Origin::ZERO_STOP, Origin::DECLINED_ADD}) {
        OriginBook engine(mode);
        engine.run(tape, 3);
        CHECK(engine.first_incarnation != 0);
        const bool pure_stop = mode == Origin::STOP || mode == Origin::REPLACED_STOP
            || mode == Origin::REUSED_ID;
        CHECK(engine.stop_origin == pure_stop);
        CHECK(engine.market_origin == (mode == Origin::MARKET
            || mode == Origin::ZERO_STOP || mode == Origin::DECLINED_ADD));
        if (mode == Origin::REUSED_ID) {
            CHECK(!engine.final_stop);
            CHECK(engine.first_incarnation != engine.final_incarnation);
        }
        if (mode == Origin::ZERO_STOP || mode == Origin::DECLINED_ADD) {
            CHECK(!engine.final_stop);
            CHECK(engine.first_incarnation == engine.final_incarnation);
        }
    }
}

// Exercise the checkpoint independently of the earlier order loop. A touched
// but deferred entry is still pending, so pending alone cannot prove unhit.
class PendingGuard : public pineforge::source::PineStrategyHost {
public:
    explicit PendingGuard(int scenario) {
        initial_capital_ = 50;
        current_bar_ = {100, 100.01, 99, 99.5, 1, 2000};
        bar_index_ = position_open_bar_ = 1;
        position_side_ = PositionSide::SHORT;
        position_qty_ = 0.5;
        position_entry_price_ = 100;
        position_entry_time_ = 2000;
        position_entry_count_ = 1;
        qty_step_ = syminfo_mintick_ = 0.01;
        PyramidEntry entry{};
        entry.price = 100;
        entry.qty = 0.5;
        entry.time = 2000;
        entry.entry_id = "S";
        entry.entry_bar_index = 1;
        entry.entry_incarnation = 7;
        entry.ordinary_stop_open = true;
        pyramid_entries_.push_back(entry);
        PendingOrder pending{};
        pending.id = "L";
        pending.type = OrderType::ENTRY;
        pending.is_long = true;
        pending.legs.set_limit_price(qnan);
        pending.legs.set_stop_price(103);
        pending.legs.set_trail_points(pending.legs.set_trail_price(pending.legs.set_trail_offset(qnan)));
        pending.qty = 0.1;
        pending.created_bar = 0;
        pending.incarnation = 8;
        switch (scenario) {
        case 1: pending.legs.set_stop_price(100.01); break;
        case 2: current_bar_.high = 100.006; pending.legs.set_stop_price(100.008); break;
        case 3: pending.oca_name = "siblings"; break;
        case 4: pending.oca_type = 1; break;
        case 5: pending.legs.set_limit_price(103); break;
        case 6: pending.stop_limit_activated = true; break;
        case 7: placement_fixture::prior_close_quantity(pending, 1.0); break;
        case 8: pending.created_position_side = PositionSide::SHORT; break;
        case 9: pending.created_bar = 1; break;
        case 10: pending.legs.set_trail_offset(1); break;
        case 11: pending.type = OrderType::MARKET; break;
        case 12: current_bar_.high = INFINITY; break;
        case 13: pending.legs.set_stop_price(INFINITY); break;
        case 14:
            pyramid_entries_[0].ordinary_stop_open = false;
            pyramid_entries_[0].ordinary_market_open = true;
            break;
        case 15: pending.birth = OrderBirth::fill_evaluation(0, 0, BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4), 100.0, 1, 1, 1); break;
        // A position-bound EXIT is distinct from the flat-born pending STOP.
        case 16: pending.type = OrderType::EXIT;
                 pending.created_position_side = PositionSide::SHORT; break;
        case 17: pending_orders_.push_back(pending); break;
        default: break;
        }
        pending_orders_.push_back(pending);
    }
    void on_source_bar(const Bar&) override {}
    void checkpoint() { process_short_margin_before_script(current_bar_); }
    std::size_t closed() const { return trades_.size(); }
    std::size_t pending() const { return pending_orders_.size(); }
    double quantity() const { return position_qty_; }
};

void test_only_proven_unhit_pending_entries_are_independent() {
    PendingGuard unhit(0);
    unhit.checkpoint();
    CHECK(unhit.closed() == 1);
    CHECK(near(unhit.quantity(), 0));
    CHECK(unhit.pending() == 1);
    for (int scenario = 1; scenario <= 17; ++scenario) {
        PendingGuard other(scenario);
        const auto count = other.pending();
        other.checkpoint();
        CHECK(other.closed() == 0);
        CHECK(near(other.quantity(), 0.5));
        CHECK(other.pending() == count);
    }
}
}
int main() {
    test_full_stop_liquidation_and_replacement();
    test_partial_and_no_opening_event();
    test_prior_high_and_pending_entry_lifetime();
    test_origin_is_an_accepted_physical_stop_fill();
    test_only_proven_unhit_pending_entries_are_independent();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
