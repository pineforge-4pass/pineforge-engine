// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Round16 Hariss F: original TV rows 114/118/363, source and feed pinned in
// r16-20260906/readback-receipt.json. Cloud Run diagnostic captures the new
// stop/limit prices. The broker tests its tick close against the raw level:
// Sep3 C11.575 ->11.58 skips L11.576782; Sep8 C11.695 ->11.70 reaches
// S11.698693; Apr23 C12.495 ->12.50 reaches S12.496973. Existing resting
// levels miss those bars; the newly reissued close-time exit owns the fill.
// Four synthetic bars isolate each event, without loading strategy/feed data.
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
static int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { \
    ++failed; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
} } while (0)

namespace {
constexpr double N = std::numeric_limits<double>::quiet_NaN();
bool near(double a, double b) { return std::abs(a-b) < 1e-8; }
enum class Guard { None, FreshId, Competing, Partial, Long, Coof,
                   NonPooc, Slip, Fx, EntryBar };

struct Panel {
    double entry, old_stop, old_limit, new_stop, new_limit;
    double next_stop, next_limit, expected_exit;
    int expected_bar;
    std::vector<Bar> bars;
};

Panel panel(int n) {
    if (n == 0) return {11.69,11.747887,11.574226,11.746609,11.576782,
        11.743280,11.583440,11.58,3,{
        {11.69,11.69,11.69,11.69,1,1000},
        {11.615,11.615,11.595,11.595,1,2000},
        {11.595,11.595,11.575,11.575,1,3000},
        {11.58,11.58,11.575,11.575,1,4000}}};
    if (n == 1) return {11.64,11.700131,11.519738,11.698693,11.522614,
        11.69900,11.52200,11.70,2,{
        {11.64,11.64,11.64,11.64,1,1000},
        {11.685,11.70,11.685,11.69,1,2000},
        {11.69,11.70,11.68,11.695,1,3000},
        {11.695,11.695,11.66,11.665,1,4000}}};
    return {12.41,12.500586,12.228828,12.496973,12.236054,
        12.49700,12.23600,12.50,2,{
        {12.41,12.41,12.41,12.41,1,1000},
        {12.46,12.48,12.45,12.48,1,2000},
        {12.48,12.50,12.48,12.495,1,3000},
        {12.50,12.515,12.47,12.48,1,4000}}};
}

class CloseTickProbe : public pineforge::source::PineStrategyHost {
public:
    CloseTickProbe(Panel data, Guard guard = Guard::None, bool unbound = false)
        : p_(std::move(data)), guard_(guard), unbound_(unbound) {
        initial_capital_ = 100000;
        margin_long_ = margin_short_ = 100;
        pyramiding_ = 0;
        qty_step_ = 1;
        syminfo_.pointvalue = 1;
        set_syminfo_mintick(.01);
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = .05;
        process_orders_on_close_ = guard != Guard::NonPooc;
        calc_on_order_fills_ = guard == Guard::Coof;
        slippage_ = guard == Guard::Slip ? 1 : 0;
        account_currency_fx_ = guard == Guard::Fx ? 2 : 1;
    }
    void on_source_bar(const Bar& b) override {
        const int seed_bar = guard_ == Guard::EntryBar ? 2 : 0;
        if (bar_index_ == seed_bar && position_side_ == PositionSide::FLAT
            && trades_.empty())
            strategy_entry("E", guard_ == Guard::Long, N, N, 1);
        if (bar_index_ >= 1 && position_side_ != PositionSide::FLAT) {
            if (bar_index_ == 2 && guard_ == Guard::FreshId)
                strategy_cancel("X");
            if (guard_ == Guard::Competing)
                strategy_order("Idle", true, 1, N, 1000);
            const double stop = bar_index_ == 1 ? p_.old_stop
                : (bar_index_ == 2 ? p_.new_stop : p_.next_stop);
            const double limit = bar_index_ == 1 ? p_.old_limit
                : (bar_index_ == 2 ? p_.new_limit : p_.next_limit);
            // The real source issues both directional brackets each close.
            // This other parent never opened in this position cycle.
            if (unbound_)
                strategy_exit("Opposite", "Other", limit, stop);
            strategy_exit("X", "E", limit, stop, N, N, N,
                guard_ == Guard::Partial ? 50 : 100, "X");
        }
        seen_close = b.close;
    }
    double remaining() const { return position_qty_; }
    uint64_t fills() const { return broker_fill_event_seq_; }
    double seen_close = N;
private:
    Panel p_;
    Guard guard_;
    bool unbound_;
};

void positive(int n, bool unbound) {
    const auto d = panel(n);
    CloseTickProbe p(d, Guard::None, unbound);
    for (int repeat = 0; repeat < 2; ++repeat) {
        p.run(d.bars.data(), d.bars.size());
        CHECK(p.last_error().empty());
        CHECK(p.trade_count() == 1);
        CHECK(p.fills() == 2);
        CHECK(near(p.seen_close, d.bars.back().close));
        if (p.trade_count() != 1) continue;
        const auto& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == d.expected_bar);
        CHECK(near(t.entry_price, d.entry));
        CHECK(near(t.exit_price, d.expected_exit));
        CHECK(near(t.qty, 1));
        CHECK(!t.is_long);
        CHECK(t.exit_id == "X");
        CHECK(near(t.commission, (d.entry+d.expected_exit)*.0005));
        CHECK(near(t.pnl, d.entry-d.expected_exit-t.commission));
    }
}

// Signatures are compared with the unchanged parent's matching headers/lib.
// These excluded synthetic inputs characterize existing behavior only.
void guards() {
    for (Guard g : {Guard::FreshId, Guard::Competing, Guard::Partial,
                    Guard::Long, Guard::Coof, Guard::NonPooc, Guard::Slip,
                    Guard::Fx, Guard::EntryBar}) {
        for (int n = 0; n < 3; ++n) {
            const auto d = panel(n);
            CloseTickProbe p(d, g);
            p.run(d.bars.data(), d.bars.size());
            CHECK(p.last_error().empty());
            std::printf("guard %d panel %d trades %d fills %llu remaining %.9f",
                static_cast<int>(g), n, p.trade_count(),
                static_cast<unsigned long long>(p.fills()), p.remaining());
            for (int i = 0; i < p.trade_count(); ++i) {
                const auto& t = p.get_trade(i);
                std::printf(" | %d,%d,%.9f,%.9f,%.9f,%.9f",
                    t.entry_bar_index,t.exit_bar_index,t.entry_price,t.exit_price,t.qty,t.pnl);
            }
            std::puts("");
        }
    }
}
} // namespace

int main(int argc, char**) {
    if (argc == 1) for (int i = 0; i < 3; ++i) {
        positive(i, false);
        positive(i, true);
    }
    guards();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
