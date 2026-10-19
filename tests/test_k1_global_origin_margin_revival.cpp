// CR-150 L19G G2: standalone base-first witness of global-leg revival.
// The script, tape and exact controls are fix10's G2 case. This file needs
// only APIs/hooks already present on integration 366489c64, including its
// existing internal retention switch; no L19G source/header overlay is needed.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace ps = pineforge::source;

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct PlacementTag {
    using type = ps::PlacementTable ps::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &ps::PineExecutionAdapter::placement_>;
struct CycleTag {
    using type = std::int64_t ps::PineExecutionAdapter::*;
    friend type access(CycleTag);
};
template struct PrivateAccess<CycleTag, &ps::PineExecutionAdapter::current_position_cycle_>;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

struct Leg {
    bool present = false;
    bool dormant = false;
    bool restored = false;
    bool withdrawn = false;
    std::uint64_t target = 0;
    std::uint64_t origin = 0;
    std::uint64_t predecessor = 0;
    double qty = 0.0;
    double stop = kNa;
};

struct Outcome {
    std::vector<std::uint64_t> observations;
    std::vector<double> positions;
    std::vector<double> exposure;
    std::vector<std::int64_t> cycles;
    std::vector<Leg> legs;
    std::vector<Trade> trades;
    std::string error;
    double open = 0.0;
    double equity = 0.0;
};

class Host final : public ps::PineStrategyHost {
public:
    explicit Host(bool recording) {
        ps::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 2;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_margin_call_enabled(true);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar&) override {
        observe();
        const int i = pine_bar_index();
        // Arm global X in R's open cycle, before declaring the empty-ID
        // parent on a DIFFERENT signal bar. Its Applied callback binds X
        // without the same-bar re-arm that could replace the original target.
        if (i == 0) strategy_entry("R", false, kNa, kNa, 40.0);
        if (i == 1)
            strategy_exit("X", "", kNa, 105.0, kNa, kNa, kNa, 100.0, {}, 40.0);
        if (i == 2) strategy_entry("", false, kNa, kNa, 40.0);
        if (i == 3) {
            // R is FIFO's older lot: remove its 40 at this callback's 100
            // print so X later closes its bound parent while S holds the side.
            strategy_close("R", "Release anchor", 40.0, kNa, true);
            strategy_entry("S", false, kNa, kNa, 40.0);
        }
        if (i == 7) {
            strategy_entry("L", true);
            strategy_close("S", "Reverse to Long");
        }
    }

    void observe() {
        BrokerStateHashSink f;
        f.i(pine_bar_index());
        f.d(live_position_size());
        f.d(position_avg_price());
        f.d(current_equity());
        f.i(trade_count());
        const int pending = pending_order_count();
        f.i(pending);
        for (int i = 0; i < pending; ++i) {
            double stop = kNa, limit = kNa, trail = kNa;
            f.i(pending_order_effective_levels(i, &stop, &limit, &trail));
            f.d(stop); f.d(limit); f.d(trail);
        }
        out_.observations.push_back(f.h);
        out_.positions.push_back(live_position_size());
        out_.exposure.push_back(adapter_.source_unclosed_qty_for(""));
        out_.cycles.push_back(adapter_.*access(CycleTag{}));
        Leg leg;
        for (const auto& row : adapter_.*access(PlacementTag{})) {
            const auto& snapshot = row.second;
            if (snapshot.family != ps::PineOrderFamily::ExitStop
                || snapshot.source_id != "X" || !snapshot.from_entry.empty()
                || snapshot.legs.target().incarnation == 0) continue;
            if (original_target_ == 0) original_target_ = snapshot.legs.target().incarnation;
            if (snapshot.legs.target().incarnation != original_target_) continue;
            CHECK(!leg.present);
            leg.present = true;
            leg.target = snapshot.legs.target().incarnation;
            leg.origin = snapshot.bracket_origin.incarnation;
            leg.predecessor = snapshot.projection_predecessor;
            leg.qty = snapshot.requested_qty;
            leg.stop = snapshot.exit_levels.stop;
            leg.dormant = snapshot.legs.dormant();
            leg.restored = snapshot.restored_after_margin;
            leg.withdrawn = snapshot.legs.retired(exit_legs::Leg::Stop)
                && snapshot.legs.retired(exit_legs::Leg::Limit)
                && snapshot.legs.retired(exit_legs::Leg::Trail);
        }
        out_.legs.push_back(leg);
    }

    Outcome finish() {
        out_.error = last_error();
        out_.open = live_position_size();
        out_.equity = current_equity();
        for (int i = 0; i < trade_count(); ++i) out_.trades.push_back(get_trade(i));
        return out_;
    }

private:
    // Observation identity and outputs only; no script decision reads them.
    std::uint64_t original_target_ = 0;
    Outcome out_;
};

struct RetentionSwitch {
    explicit RetentionSwitch(bool retain) : previous(ps::detail::retain_retired_rows()) {
        ps::detail::set_retain_retired_rows(retain);
    }
    ~RetentionSwitch() { ps::detail::set_retain_retired_rows(previous); }
    bool previous;
};

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    const auto add = [&](double o, double h, double l, double c) {
        bars.push_back({o, h, l, c, 1.0,
            kStart + static_cast<std::int64_t>(bars.size()) * 86400000LL});
    };
    add(100.0, 100.2, 99.8, 100.0); // 0: submit R
    add(100.0, 100.2, 99.8, 100.0); // 1: R fills; arm origin-free global X
    add(100.0, 100.2, 99.8, 100.0); // 2: submit the later empty-ID parent
    add(100.0, 100.2, 99.8, 100.0); // 3: parent fills, binds X; close R, submit S
    add(100.0, 100.2, 99.8, 100.0); // 4: S fills; both lots hold 80 units
    add(100.0, 105.2, 99.8, 105.0); // 5: X closes its 40-unit parent at 105
    add(105.0, 105.2, 104.8, 105.0); // 6: sweep with empty-ID exposure zero
    add(105.0, 105.2, 104.8, 105.0); // 7: place all-in reversal / close pair
    add(160.0, 160.2, 159.8, 160.0); // 8: decline; suspend original global X
    add(160.0, 160.2, 159.8, 160.0); // 9: inspect the dormant original
    add(160.0, 175.0, 159.8, 175.0); // 10: chart margin checkpoint
    add(175.0, 175.0, 175.0, 175.0); // 11: sampled path's first 175 checkpoint
    add(175.0, 175.0, 175.0, 175.0); // 12: inspect original restoration
    return bars;
}

Outcome run(bool retain, bool magnifier, bool recording) {
    RetentionSwitch switch_for_run(retain);
    Host host(recording);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1D", "",
             magnifier, 4, MagnifierDistribution::ENDPOINTS);
    return host.finish();
}

std::uint64_t trade_digest(const std::vector<Trade>& trades) {
    BrokerStateHashSink f;
    f.u(trades.size());
    for (const auto& t : trades) {
        f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
        f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.b(t.is_long);
        f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.s(t.entry_id); f.s(t.exit_id);
        f.s(t.entry_comment); f.s(t.exit_comment); f.b(t.exit_from_bracket);
        f.d(t.max_runup); f.d(t.max_drawdown); f.d(t.commission);
        f.u(t.entry_incarnation); f.b(t.open_at_end);
        f.u(static_cast<std::uint64_t>(t.close_cause));
    }
    return f.h;
}

void check_path(const Outcome& out, int margin_bar) {
    CHECK(out.error.empty());
    CHECK(out.positions.size() == 13);
    CHECK(out.exposure.size() == 13);
    CHECK(out.cycles.size() == 13);
    CHECK(out.legs.size() == 13);
    if (out.positions.size() != 13 || out.exposure.size() != 13
        || out.cycles.size() != 13 || out.legs.size() != 13) return;

    // R's open cycle exists before X; the later empty-ID opening binds X.
    CHECK(out.legs[2].present && out.legs[2].target != 0 && out.legs[2].origin == 0);
    CHECK(out.positions[4] == -80.0 && out.exposure[4] == 40.0);
    CHECK(out.legs[4].present && out.legs[4].origin != 0);
    // Parent closed on bar 5; S still holds 40 in the SAME cycle at the sweep.
    CHECK(out.positions[6] == -40.0 && out.exposure[6] == 0.0);
    CHECK(out.cycles[2] == out.cycles[6]);
    CHECK(out.legs[6].present && !out.legs[6].dormant && !out.legs[6].withdrawn);
    CHECK(out.legs[6].qty == 40.0 && out.legs[6].stop == 105.0);
    CHECK(out.legs[6].predecessor == 0);
    CHECK(out.legs[4].origin == out.legs[6].origin);
    // The real reversal declines at 160 on bar 8, before any margin slice.
    CHECK(out.positions[9] == -40.0);
    CHECK(out.legs[9].present && out.legs[9].dormant);
    CHECK(!out.legs[9].withdrawn && !out.legs[9].restored);
    // The base run MUST revive original X; equal totals alone cannot pass.
    CHECK(out.legs[12].present && out.legs[12].restored);
    CHECK(out.legs[2].target == out.legs[4].target);
    CHECK(out.legs[4].target == out.legs[6].target);
    CHECK(out.legs[6].target == out.legs[9].target);
    CHECK(out.legs[9].target == out.legs[12].target);
    CHECK(out.legs[6].origin == out.legs[12].origin);

    int through_x = 0;
    int margins = 0;
    int anchor_closes = 0;
    for (const auto& trade : out.trades) {
        if (trade.exit_id == "__margin_call__") {
            ++margins;
            // At 175: equity 9800 - 40*75 = 6800; maintenance 7000.
            // The retained engine's 4x shortfall slice is 4*200/175 = 32/7.
            CHECK(trade.entry_id == "S" && trade.entry_bar_index == 4);
            CHECK(trade.exit_bar_index == margin_bar && trade.exit_price == 175.0);
            CHECK(trade.qty == 32.0 / 7.0);
        } else if (trade.exit_id == "X") {
            ++through_x;
            CHECK(trade.entry_price == 100.0);
            if (through_x == 1) {
                // Bound parent opens on bar 3 and X closes it at 105 on bar 5.
                CHECK(trade.entry_id.empty() && trade.entry_bar_index == 3);
                CHECK(trade.entry_incarnation == out.legs[6].origin);
                CHECK(trade.exit_bar_index == 5 && trade.exit_price == 105.0);
                CHECK(trade.qty == 40.0);
            } else {
                // Original X then flattens S's post-margin remainder at 175.
                CHECK(trade.entry_id == "S" && trade.entry_bar_index == 4);
                CHECK(trade.entry_incarnation != out.legs[6].origin);
                CHECK(trade.exit_bar_index == margin_bar && trade.exit_price == 175.0);
                CHECK(trade.qty == 248.0 / 7.0);
            }
        } else {
            ++anchor_closes;
            // R opened on bar 1; its immediate close uses bar 3's 100 print.
            CHECK(trade.entry_id == "R" && trade.entry_bar_index == 1);
            CHECK(trade.exit_bar_index == 3 && trade.qty == 40.0);
            CHECK(trade.entry_price == 100.0 && trade.exit_price == 100.0);
            CHECK(trade.pnl == 0.0);
        }
    }
    CHECK(through_x == 2 && margins == 1 && anchor_closes == 1);
    CHECK(out.trades.size() == 4);
    CHECK(out.open == 0.0);
    // R loses zero, the parent loses 40*5, and all of S loses 40*75.
    CHECK(out.equity == 10000.0 - 200.0 - 3000.0);
}

void global_original_revives_on_the_base_and_lane() {
    for (bool magnifier : {false, true}) for (bool recording : {false, true}) {
        // Chart margin sees bar 10's high; the sampled path first sees a
        // margin checkpoint at 175 on bar 11. Recording changes no decision.
        const int margin_bar = magnifier ? 11 : 10;
        const auto retained = run(true, magnifier, recording);
        const auto erased = run(false, magnifier, recording);
        check_path(retained, margin_bar);
        check_path(erased, margin_bar);
        CHECK(erased.observations == retained.observations);
        CHECK(trade_digest(erased.trades) == trade_digest(retained.trades));
        CHECK(erased.open == retained.open);
        CHECK(erased.equity == retained.equity);
        const Leg reference = retained.legs.empty() ? Leg{} : retained.legs.back();
        const Leg ordinary = erased.legs.empty() ? Leg{} : erased.legs.back();
        std::printf("G2 magnifier=%d recording=%d: original target=%llu/%llu "
                    "origin=%llu/%llu restored=%d/%d, trades=%zu/%zu, "
                    "open=%.17g/%.17g equity=%.17g/%.17g\n",
                    magnifier ? 1 : 0, recording ? 1 : 0,
                    static_cast<unsigned long long>(reference.target),
                    static_cast<unsigned long long>(ordinary.target),
                    static_cast<unsigned long long>(reference.origin),
                    static_cast<unsigned long long>(ordinary.origin),
                    reference.restored ? 1 : 0, ordinary.restored ? 1 : 0,
                    retained.trades.size(), erased.trades.size(), retained.open, erased.open,
                    retained.equity, erased.equity);
    }
}
} // namespace

int main() {
    global_original_revives_on_the_base_and_lane();
    std::printf("test_k1_global_origin_margin_revival: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
