#include "exit_lifecycle_fixture.hpp"
// Literal native readiness contracts. No Pine, external tapes, or grader.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); } } while (0)

class MarginBook : public pineforge::source::PineStrategyHost {
public:
    MarginBook() {
        initial_capital_ = 1000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 50;
        pyramiding_ = 10;
        qty_step_ = 1;
        current_bar_ = {100, 100, 100, 100, 1, 0};
    }
    void on_source_bar(const Bar&) override {}
    PendingOrder& child() {
        for (auto& order : pending_orders_) if (order.id == "X") return order;
        throw std::logic_error("missing native bracket");
    }
    void exercise(int mode) {
        strategy_entry("E", true, missing, missing, 20);
        ++bar_index_;
        current_bar_ = {100, 100, 100, 100, 1, 60000};
        process_pending_orders(current_bar_);
        CHECK(position_qty_ == 20 && position_cycle_seq_ == 1);
        strategy_exit("X", "E", missing, 110);
        // A valid dormant state is the fixture precondition. The originating
        // rejection is separate from this activation/risk settlement contract.
        lifecycle_fixture::suspend(child());
        const bool ready = mode == 1;
        const bool foreign = mode == 2;
        child().leg_activation.bind({foreign ? 99 : position_cycle_seq_, ready ? 2 : 5, 5});
        ++bar_index_;
        current_bar_ = {95, 95, 95, 95, 1, 120000};
        process_margin_call(current_bar_);
        // Equity900 < margin950. The existing risk rule liquidates
        // 4 * floor((950-900)/0.5/95) = 4 units, independently of the bracket.
        CHECK(!trades_.empty());
        if (trades_.empty()) return;
        CHECK(trades_[0].exit_id == "__margin_call__");
        CHECK(trades_[0].qty == 4 && trades_[0].exit_price == 95);
        CHECK(trades_[0].exit_bar_index == 2);
        if (ready) {
            CHECK(position_qty_ == 0 && trades_.size() == 2);
            if (trades_.size() != 2) return;
            CHECK(trades_[1].exit_id == "X" && trades_[1].qty == 16);
            CHECK(trades_[1].exit_price == 95 && trades_[1].exit_bar_index == 2);
            return;
        }
        CHECK(position_qty_ == 16 && trades_.size() == 1);
        CHECK(!child().legs.dormant()); // metadata revival is independent
        CHECK(child().leg_activation.bounds()->stop_first_bar == 5);
        for (int bar = 3; bar <= 5; ++bar) {
            bar_index_ = bar;
            current_bar_ = {95, 95, 95, 95, 1, bar * 60000LL};
            process_pending_orders(current_bar_);
            CHECK(position_qty_ == (bar < 5 || foreign ? 16 : 0));
        }
        if (foreign) {
            CHECK(trades_.size() == 1);
            child().leg_activation.bind({position_cycle_seq_, 5, 5});
            bar_index_ = 6;
            current_bar_ = {95, 95, 95, 95, 1, 360000};
            process_pending_orders(current_bar_);
        }
        CHECK(position_qty_ == 0 && trades_.size() == 2);
        if (trades_.size() != 2) return;
        CHECK(trades_[1].exit_id == "X" && trades_[1].qty == 16);
        CHECK(trades_[1].exit_price == 95);
        CHECK(trades_[1].exit_bar_index == (foreign ? 6 : 5));
    }
};

enum class GapCase { HeldStop, ReadyLimit, HeldWithTrail, ReadyStop,
                     BothHeld, ForeignWithTrail, BothReady, LimitOnly };
class PrearmedFrame : public pineforge::source::PineStrategyHost {
public:
    PrearmedFrame() {
        initial_capital_ = 100000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 0;
        slippage_ = 2;
        syminfo_mintick_ = 0.01;
        current_bar_ = {100, 100, 100, 100, 1, 0};
    }
    void on_source_bar(const Bar&) override {}
    void exercise(GapCase mode) {
        const bool trail = mode == GapCase::HeldWithTrail || mode == GapCase::ForeignWithTrail;
        strategy_entry("E", true, missing, missing, 1);
        strategy_exit("X", "E", mode == GapCase::HeldStop ? 150 : 90,
                      mode == GapCase::LimitOnly ? missing : 110,
                      trail ? 1000 : missing, trail ? 1 : missing);
        const auto frame = pending_orders_;
        CHECK(frame.size() == 2);
        if (frame.size() != 2) return;
        pending_orders_.resize(1);
        ++bar_index_;
        current_bar_ = {100, 100, 100, 100, 1, 60000};
        process_pending_orders(current_bar_);
        CHECK(position_qty_ == 1 && position_side_ == PositionSide::LONG);
        CHECK(std::abs(position_entry_price_ - 100.02) < 1e-9);
        // Explicit post-parent/pre-compaction snapshot: retain the actual
        // parent identity with no executable remainder. This targets the real
        // matching route, not public placement chronology.
        pending_orders_ = frame;
        pending_orders_[0].qty = 0;
        const bool stop_ready = mode == GapCase::ReadyStop || mode == GapCase::BothReady;
        const bool limit_ready = mode == GapCase::HeldStop || mode == GapCase::ReadyLimit
                              || mode == GapCase::BothReady || mode == GapCase::LimitOnly;
        pending_orders_[1].leg_activation.bind({
            mode == GapCase::ForeignWithTrail ? 99 : position_cycle_seq_,
            stop_ready ? 1 : 5, limit_ready ? 1 : 5});
        process_pending_orders(current_bar_);
        const bool filled = mode == GapCase::ReadyLimit || mode == GapCase::ReadyStop
                         || mode == GapCase::BothReady || mode == GapCase::LimitOnly;
        CHECK(position_qty_ == (filled ? 0 : 1));
        CHECK(trades_.size() == (filled ? 1u : 0u));
        if (filled && trades_.size() == 1) {
            const bool limit = mode == GapCase::ReadyLimit || mode == GapCase::LimitOnly;
            CHECK(trades_[0].exit_id == "X" && trades_[0].qty == 1);
            // Ready limits use the unslipped100 open; stop precedence uses
            // 99.98. A held stop cannot borrow a ready sibling's permission.
            CHECK(std::abs(trades_[0].exit_price - (limit ? 100 : 99.98)) < 1e-9);
        }
    }
};

class ChartPointBook : public pineforge::source::PineStrategyHost {
    bool long_side_;
    bool stop_leg_;
    bool armed_ = false;
    int first_bar_;
public:
    ChartPointBook(bool long_side, bool stop_leg, int first_bar)
        : long_side_(long_side), stop_leg_(stop_leg), first_bar_(first_bar) {
        initial_capital_ = 100000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        pyramiding_ = 0;
        calc_on_order_fills_ = true;
        syminfo_mintick_ = 0.01;
    }
    double level() const { return long_side_ == stop_leg_ ? 9.90 : 10.26; }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", long_side_, missing, missing, 1);
        if (bar_index_ != 1 || !coof_fill_recalc_active_ || armed_) return;
        armed_ = true;
        strategy_exit("X", "E", stop_leg_ ? missing : level(), stop_leg_ ? level() : missing);
        for (auto& order : pending_orders_) if (order.id == "X") {
            order.leg_activation.bind({position_cycle_seq_, first_bar_, first_bar_});
        }
    }
    void exercise() {
        // The raw extremes do not reach9.90/10.26, but their chart tick
        // projections do. Test all long/short stop/limit combinations.
        const Bar bars[] = {
            {10, 10, 10, 10, 1, 0}, {10, 10, 10, 10, 1, 60000},
            {10, 10.256, 9.904, 10, 1, 120000},
            {10, 10, 10, 10, 1, 180000}, {10, 10, 10, 10, 1, 240000},
            {10, 10.256, 9.904, 10, 1, 300000},
        };
        run(bars, 6);
        CHECK(last_error().empty());
        CHECK(trades_.size() == 1);
        if (trades_.size() != 1) return;
        CHECK(trades_[0].exit_id == "X" && trades_[0].qty == 1);
        CHECK(trades_[0].exit_bar_index == first_bar_);
        CHECK(std::abs(trades_[0].exit_price - level()) < 1e-9);
    }
};
}
int main() {
    for (int mode = 0; mode < 3; ++mode) {
        try { MarginBook book; book.exercise(mode); }
        catch (const std::exception& e) { ++failures; std::fprintf(stderr, "margin: %s\n", e.what()); }
    }
    for (int mode = 0; mode < 8; ++mode) {
        try { PrearmedFrame book; book.exercise(static_cast<GapCase>(mode)); }
        catch (const std::exception& e) { ++failures; std::fprintf(stderr, "prearmed: %s\n", e.what()); }
    }
    for (bool long_side : {false, true}) {
        for (bool stop_leg : {false, true}) {
            for (int first_bar : {2, 5}) {
                try { ChartPointBook book(long_side, stop_leg, first_bar); book.exercise(); }
                catch (const std::exception& e) { ++failures; std::fprintf(stderr, "chart point: %s\n", e.what()); }
            }
        }
    }
    std::printf("native activation routes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
