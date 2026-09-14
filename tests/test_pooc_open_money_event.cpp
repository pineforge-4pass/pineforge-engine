// Literal broker schedules from independently covered TV controls. No feed,
// strategy indicator, historical backtest, verifier or grading loop is loaded.
// Positive slippage: the one-unit money event is at next O. COOF closes the
// survivor there; ordinary close-calc waits until C. A funded book has no event.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; \
    std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
const double na = std::numeric_limits<double>::quiet_NaN();
constexpr double qty = 86654.03;
constexpr double equity = 98434.64537859998;
bool near(double a, double b, double epsilon = 1e-8) {
    return std::abs(a - b) < epsilon;
}

struct Seen {
    double qty, equity, cursor;
    bool recalc, at_open, raw_point;
};
enum class Guard { None, Pending, Fee, Risk, Fx, Pyramiding, Raw };

class Probe : public pineforge::source::PineStrategyHost {
    Guard guard_;
    bool cycle_, default_entry_;
public:
    std::vector<Seen> seen;
    explicit Probe(bool coof, bool funded, int slip = 2,
                   Guard guard = Guard::None, bool cycle = false,
                   bool default_entry = false, int pyramid_limit = 0)
        : guard_(guard), cycle_(cycle), default_entry_(default_entry) {
        initial_capital_ = equity - qty * (2 - slip) * 0.00001
                           + (funded ? 0.001 : 0.0);
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        margin_long_ = margin_short_ = 100;
        pyramiding_ = pyramid_limit;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0;
        slippage_ = slip;
        qty_step_ = 0.01;
        syminfo_.pointvalue = 1;
        set_syminfo_mintick(0.00001);
        process_orders_on_close_ = true;
        calc_on_order_fills_ = coof;
        if (guard == Guard::Fee) commission_value_ = 1e-12;
        if (guard == Guard::Risk) adapter_.cap = 100;
        if (guard == Guard::Pyramiding) pyramiding_ = 2;
        if (guard == Guard::Fx) {
            const int64_t times[] = {1000};
            const double rates[] = {1.0};
            CHECK(set_account_currency_fx_series(times, rates, 1));
        }
    }
    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            if (guard_ == Guard::Raw) strategy_order("Owned", true, qty);
            else strategy_entry("Owned", true, na, na,
                                default_entry_ ? na : qty, "ENTRY");
            if (guard_ == Guard::Pending)
                strategy_exit("Resting", "Owned", 2.0, na);
        }
        if (bar_index_ == 1 && cycle_ && position_side_ == PositionSide::FLAT
            && trades_.size() == 2) {
            strategy_entry("Next", true, na, na, 10.0, "NEXT");
        }
        if (bar_index_ == 1 && position_side_ != PositionSide::FLAT) {
            const bool recalc = coof_fill_recalc_active_;
            const double mark = recalc ? coof_cursor_price_ : bar.close;
            seen.push_back({signed_position_size(),
                current_equity() + open_profit(mark), mark,
                recalc, coof_recalc_at_bar_open_, coof_cursor_is_bar_point_});
            strategy_close("", "SURVIVOR");
        }
    }
    const std::vector<Trade>& rows() const { return trades_; }
    double remaining() const { return position_qty_; }
    void literal_shortfall_at_entry() {
        // Deliberately seed a post-admission book with a real cash deficit.
        // Its existing immediate entry-budget restore must not be deferred.
        bar_index_ = 0;
        current_bar_ = {1.13593, 1.13593, 1.13593, 1.13593, 1, 1000};
        initial_capital_ = qty * 1.13595 - 0.001;
        position_side_ = PositionSide::LONG;
        position_qty_ = qty;
        position_entry_price_ = 1.13595;
        position_entry_time_ = 1000;
        position_open_bar_ = 0;
        position_entry_count_ = 1;
        PyramidEntry entry{};
        entry.qty = qty; entry.price = 1.13595; entry.time = 1000;
        entry.entry_id = "Owned"; entry.entry_bar_index = 0;
        entry.entry_path_position = 3.0; entry.entry_incarnation = 1;
        entry.entry_commission_account = 0;
        entry.pooc_terminal_market_entry = true;
        pyramid_entries_.push_back(entry);
        opening_obligations_.replace(broker::OpeningReceipt::check(
                {position_cycle_seq_, broker_fill_event_seq_, 0, bar_index_, current_bar_.timestamp}, 1.13593));
        process_margin_call(current_bar_);
    }
};

const std::vector<Bar> bars = {
    {1.13575, 1.13644, 1.13558, 1.13593, 1, 1000},
    {1.13592, 1.13754, 1.13582, 1.13735, 1, 2000},
    {1.13735, 1.13754, 1.13692, 1.13698, 1, 3000},
};

void covered_control(bool coof, bool funded, int slip = 2, int pyramid_limit = 0) {
    Probe p(coof, funded, slip, Guard::None, false, false, pyramid_limit);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    CHECK(near(p.remaining(), 0));
    const bool fire = !funded;
    CHECK(p.rows().size() == (fire ? 2u : 1u));
    double total = 0;
    for (const auto& row : p.rows()) {
        total += row.qty;
        CHECK(row.entry_time == 1000);
        CHECK(near(row.entry_price, 1.13593 + slip * 0.00001));
        CHECK(row.exit_time == 2000);
        CHECK(row.entry_id == "Owned");
    }
    CHECK(near(total, qty)); // Negative controls must actually have entered.
    CHECK(p.seen.size() == 1);
    if (p.rows().size() != (fire ? 2u : 1u) || p.seen.size() != 1) return;
    const auto& script = p.seen[0];
    CHECK(near(script.qty, qty - (fire ? 1.0 : 0.0)));
    CHECK(script.recalc == (coof && fire));
    CHECK(script.at_open == (coof && fire));
    if (fire) {
        const auto& call = p.rows().front();
        CHECK(call.exit_comment == "Margin call");
        CHECK(call.exit_id == "__margin_call__");
        CHECK(call.qty == 1.0);
        CHECK(near(call.exit_price, 1.13590));
        CHECK(near(call.pnl, -0.00005, 1e-10));
        CHECK(near(call.max_runup, 0.0, 1e-10));
        CHECK(near(call.max_drawdown, 0.00005, 1e-10));
        if (coof) {
            CHECK(script.raw_point);
            CHECK(near(script.cursor, 1.13592, 1e-12));
            CHECK(near(script.equity, 98432.04573769997, 1e-7));
        }
    }
    const auto& close = p.rows().back();
    CHECK(close.exit_comment == "SURVIVOR");
    CHECK(near(close.exit_price,
        coof && fire ? 1.13590 : 1.13735 - slip * 0.00001));
    if (coof && fire) CHECK(near(close.max_runup, 0.0, 1e-10));
}

void next_orders_keep_waypoint_order() {
    Probe p(true, false, 2, Guard::None, true);
    p.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(p.last_error().empty());
    CHECK(p.rows().size() == 3);
    CHECK(near(p.remaining(), 0));
    if (p.rows().size() != 3) return;
    CHECK(p.rows()[0].exit_comment == "Margin call");
    CHECK(near(p.rows()[0].exit_price, 1.13590));
    CHECK(near(p.rows()[1].exit_price, 1.13590));
    CHECK(near(p.rows()[1].max_runup, 0.0));
    const auto& next = p.rows()[2];
    CHECK(next.entry_id == "Next");
    CHECK(next.qty == 10);
    CHECK(next.entry_time == 2000 && next.exit_time == 2000);
    CHECK(near(next.entry_price, 1.13584)); // next low + slippage, never O again
    CHECK(near(next.exit_price, 1.13752));  // following high - slippage
}

void compatibility_scopes() {
    for (bool coof : {false, true}) {
        for (Guard guard : {Guard::Pending, Guard::Fee, Guard::Risk,
                            Guard::Fx, Guard::Pyramiding, Guard::Raw}) {
            Probe p(coof, false, 2, guard);
            p.run(bars.data(), static_cast<int>(bars.size()));
            if (guard == Guard::Fx && coof) {
                // Existing unsupported combination: no admitted position, so
                // it is not counted as a liquidation compatibility control.
                CHECK(p.last_error().find("does not support calc_on_order_fills")
                      != std::string::npos);
                CHECK(p.rows().empty());
                continue;
            }
            CHECK(p.last_error().empty());
            double total = 0;
            for (const auto& row : p.rows()) {
                total += row.qty;
                // An existing opening-budget event may still fire at entry C.
                // None of these unproven classes acquires the new next-O event.
                if (row.exit_comment == "Margin call") CHECK(row.exit_time == 1000);
            }
            CHECK(near(total, qty));
            CHECK(near(p.remaining(), 0));
        }
    }
}

void opening_only_and_real_deficit() {
    for (bool coof : {false, true}) {
        auto later = bars;
        later[1].open = 1.13600; // exact money at O; a later point has the deficit
        later[1].low = 1.13592;
        Probe p(coof, false);
        p.run(later.data(), static_cast<int>(later.size()));
        CHECK(p.rows().size() == 1); // exclusion guard, not a new TV later-path claim
        if (p.rows().size() == 1) {
            CHECK(p.rows()[0].exit_comment == "SURVIVOR");
            CHECK(near(p.rows()[0].exit_price, 1.13733));
            CHECK(near(p.rows()[0].qty, qty));
        }
        Probe shortfall(coof, false);
        shortfall.literal_shortfall_at_entry();
        CHECK(shortfall.rows().size() == 1);
        if (shortfall.rows().size() == 1) {
            CHECK(shortfall.rows()[0].exit_comment == "Margin call");
            CHECK(shortfall.rows()[0].exit_time == 1000);
            CHECK(shortfall.rows()[0].qty == 1);
        }
    }
    Probe default_funded(true, true, 2, Guard::None, false, true);
    default_funded.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(default_funded.last_error().empty());
    CHECK(default_funded.rows().size() == 1);
    if (default_funded.rows().size() == 1) {
        CHECK(near(default_funded.rows()[0].qty, qty));
        CHECK(near(default_funded.rows()[0].exit_price, 1.13733));
        CHECK(default_funded.rows()[0].exit_comment == "SURVIVOR");
    }
}
}

int main() {
    for (int pyramid_limit : {0, 1}) {
        for (bool coof : {false, true}) {
            covered_control(coof, false, 2, pyramid_limit);
            covered_control(coof, true, 2, pyramid_limit);
            covered_control(coof, true, 0, pyramid_limit);
        }
    }
    next_orders_keep_waypoint_order();
    compatibility_scopes();
    opening_only_and_real_deficit();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
