// Standalone base-first witness of global-leg revival.
// Fix12 uses an ordinary next-open close of R, whose Applied trade removes
// its opened-origin slot before S's next callback (as measured in G1). The
// reversal/margin suffix is the measured F2 path shifted by three bars. Only
// base abe989918 APIs/hooks are used; no L19G source/header overlay is needed.
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
            // R is FIFO's older lot. The ordinary close fills at bar 4's
            // 100 open; consume_closed_trade_rows removes R's opened slot.
            strategy_close("R", "Release anchor", 40.0);
        }
        // The measured immediate-close version consumed R's units but kept
        // its opened slot, so S hit the cap of two. Continuous quantities do
        // not enable the ledger-count override. With the ordinary close's
        // normal trade drain, only the parent is counted here; S fills on 5.
        if (i == 4) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i == 8) {
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
    add(100.0, 100.2, 99.8, 100.0); // 3: parent fills, binds X; close R only
    add(100.0, 100.2, 99.8, 100.0); // 4: R closes; its slot drains; submit S
    add(100.0, 100.2, 99.8, 100.0); // 5: S fills; observe both lots before X
    add(100.0, 105.2, 99.8, 105.0); // 6: X closes its 40-unit parent at 105
    add(105.0, 105.2, 104.8, 105.0); // 7: sweep with empty-ID exposure zero
    add(105.0, 105.2, 104.8, 105.0); // 8: place all-in reversal / close pair
    add(160.0, 160.2, 159.8, 160.0); // 9: decline; suspend original global X
    add(160.0, 160.2, 159.8, 160.0); // 10: inspect the dormant original
    add(160.0, 175.0, 159.8, 175.0); // 11: chart margin checkpoint
    add(175.0, 175.0, 175.0, 175.0); // 12: sampled path's first 175 checkpoint
    add(175.0, 175.0, 175.0, 175.0); // 13: inspect original restoration
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
    CHECK(out.observations.size() == 14);
    CHECK(out.positions.size() == 14);
    CHECK(out.exposure.size() == 14);
    CHECK(out.cycles.size() == 14);
    CHECK(out.legs.size() == 14);
    if (out.positions.size() != 14 || out.exposure.size() != 14
        || out.cycles.size() != 14 || out.legs.size() != 14) return;

    // Measured base binding: R opens on 1; X is origin-free on 2; the empty
    // parent fills on 3. The ordinary close now removes R at bar 4's open.
    CHECK(out.positions[0] == 0.0 && out.positions[1] == -40.0);
    CHECK(out.positions[2] == -40.0 && out.exposure[2] == 0.0);
    CHECK(out.legs[2].present && out.legs[2].target != 0 && out.legs[2].origin == 0);
    CHECK(out.positions[3] == -80.0 && out.exposure[3] == 40.0);
    CHECK(out.legs[3].present && out.legs[3].origin != 0);
    CHECK(out.positions[4] == -40.0 && out.exposure[4] == 40.0);
    // After the normal trade drain, S is admitted on 4 and fills at 100 on 5.
    // This quiet bar MUST show two lots before X closes its FIFO parent on 6.
    CHECK(out.positions[5] == -80.0 && out.exposure[5] == 40.0);
    CHECK(out.legs[5].present && out.legs[5].origin == out.legs[3].origin);
    // S holds 40 in the SAME cycle at bar 7's post-parent-close sweep.
    CHECK(out.positions[7] == -40.0 && out.exposure[7] == 0.0);
    CHECK(out.cycles[2] != 0 && out.cycles[2] == out.cycles[5]);
    CHECK(out.cycles[5] == out.cycles[7]);
    CHECK(out.legs[7].present && !out.legs[7].dormant && !out.legs[7].withdrawn);
    CHECK(out.legs[7].qty == 40.0 && out.legs[7].stop == 105.0);
    CHECK(out.legs[7].predecessor == 0);
    CHECK(out.legs[3].origin == out.legs[7].origin);
    // Measured F2 suffix shifted +3: reversal on 8, gap decline on 9,
    // dormant original on 10, chart margin on 11 / sampled margin on 12.
    CHECK(out.positions[10] == -40.0);
    CHECK(out.cycles[7] == out.cycles[10]);
    CHECK(out.legs[10].present && out.legs[10].dormant);
    CHECK(!out.legs[10].withdrawn && !out.legs[10].restored);
    // The base run MUST revive original X; equal totals alone cannot pass.
    CHECK(out.legs[13].present && out.legs[13].restored);
    CHECK(out.legs[2].target == out.legs[3].target);
    CHECK(out.legs[3].target == out.legs[5].target);
    CHECK(out.legs[5].target == out.legs[7].target);
    CHECK(out.legs[7].target == out.legs[10].target);
    CHECK(out.legs[10].target == out.legs[13].target);
    CHECK(out.legs[7].origin == out.legs[13].origin);

    int through_x = 0;
    int margins = 0;
    int anchor_closes = 0;
    for (const auto& trade : out.trades) {
        if (trade.exit_id == "__margin_call__") {
            ++margins;
            // At 175: equity 9800 - 40*75 = 6800; maintenance 7000.
            // The retained engine's 4x shortfall slice is 4*200/175 = 32/7.
            CHECK(trade.entry_id == "S" && trade.entry_bar_index == 5);
            CHECK(trade.exit_bar_index == margin_bar && trade.exit_price == 175.0);
            CHECK(trade.qty == 32.0 / 7.0);
        } else if (trade.exit_id == "X") {
            ++through_x;
            CHECK(trade.entry_price == 100.0);
            if (through_x == 1) {
                // Bound parent opens on bar 3 and X closes it at 105 on bar 6.
                CHECK(trade.entry_id.empty() && trade.entry_bar_index == 3);
                CHECK(trade.entry_incarnation == out.legs[7].origin);
                CHECK(trade.exit_bar_index == 6 && trade.exit_price == 105.0);
                CHECK(trade.qty == 40.0);
            } else {
                // Original X then flattens S's post-margin remainder at 175.
                CHECK(trade.entry_id == "S" && trade.entry_bar_index == 5);
                CHECK(trade.entry_incarnation != out.legs[7].origin);
                CHECK(trade.exit_bar_index == margin_bar && trade.exit_price == 175.0);
                CHECK(trade.qty == 248.0 / 7.0);
            }
        } else {
            ++anchor_closes;
            // R opened on bar 1; the ordinary close uses bar 4's 100 open.
            CHECK(trade.entry_id == "R" && trade.entry_bar_index == 1);
            CHECK(trade.exit_bar_index == 4 && trade.qty == 40.0);
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
        // Same suffix as measured F2, shifted +3: its chart bar 8 becomes
        // 11; its first sampled 175 checkpoint (bar 9) becomes 12.
        const int margin_bar = magnifier ? 12 : 11;
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
