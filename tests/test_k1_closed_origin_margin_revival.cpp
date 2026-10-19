// CR-150 L19G / read F2: preserve the retained engine's closed-origin leg
// revival through its same-id sibling after a declined reversal and margin call.
// Compare public results with every placement row retained. Keep the path
// controls: a missing fill, suspension, cohort pin or margin call is a failure.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <cmath>
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
    std::uint64_t origin = 0;
    double qty = 0.0;
};

struct Outcome {
    std::vector<std::uint64_t> observations;
    std::vector<double> positions;
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
        if (i == 0) strategy_entry("S", false, kNa, kNa, 40.0);
        if (i == 1) {
            // Bind X only to the first, already-open lot. The second S is
            // placed AFTER the exit call, and fills before X reaches 105.
            strategy_exit("X", "S", kNa, 105.0, kNa, kNa, kNa, 100.0, {}, 40.0);
            strategy_entry("S", false, kNa, kNa, 40.0);
        }
        if (i == 5) {
            // The 160 gap declines the frozen all-in long / close pair.
            // The surviving same-id short keeps the original cycle open.
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
        Leg leg;
        for (const auto& row : adapter_.*access(PlacementTag{})) {
            const auto& snapshot = row.second;
            if (snapshot.family != ps::PineOrderFamily::ExitStop
                || snapshot.source_id != "X" || snapshot.from_entry != "S"
                || snapshot.legs.target().incarnation == 0) continue;
            if (original_target_ == 0) original_target_ = snapshot.legs.target().incarnation;
            if (snapshot.legs.target().incarnation != original_target_) continue;
            CHECK(!leg.present);
            leg.present = true;
            leg.dormant = snapshot.legs.dormant();
            leg.restored = snapshot.restored_after_margin;
            leg.withdrawn = snapshot.legs.retired(exit_legs::Leg::Stop)
                && snapshot.legs.retired(exit_legs::Leg::Limit)
                && snapshot.legs.retired(exit_legs::Leg::Trail);
            leg.origin = snapshot.bracket_origin.incarnation;
            leg.qty = snapshot.requested_qty;
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
    Outcome out_;
    // Observation identity only; no script decision reads it.
    std::uint64_t original_target_ = 0;
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
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0);
    add(100.0, 100.2, 99.8, 100.0); // both same-id lots are now open
    add(100.0, 105.2, 99.8, 105.0); // X closes the first lot only
    add(105.0, 105.2, 104.8, 105.0); // the same-id sibling still needs the old leg
    add(105.0, 105.2, 104.8, 105.0); // place the reversal pair
    add(160.0, 160.2, 159.8, 160.0); // decline, below the short's margin level
    add(160.0, 160.2, 159.8, 160.0); // inspect the suspended retained leg
    add(160.0, 175.0, 159.8, 175.0); // a partial margin call, stop 105 reached
    add(175.0, 175.0, 175.0, 175.0);
    add(175.0, 175.0, 175.0, 175.0);
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

void closed_origin_revival_matches_retained_execution() {
    for (bool magnifier : {false, true}) for (bool recording : {false, true}) {
        // Distribution samples check margin at bar 9's first 175 print;
        // the chart path checks the high on bar 8 (retained-engine trace).
        const int expected_margin_bar = magnifier ? 9 : 8;
        const auto erased = run(false, magnifier, recording);
        const auto retained = run(true, magnifier, recording);
        CHECK(erased.error.empty());
        CHECK(retained.error.empty());
        CHECK(erased.positions.size() == 11);
        CHECK(retained.positions.size() == 11);
        if (erased.positions.size() != 11 || retained.positions.size() != 11) continue;
        CHECK(erased.positions[2] == -80.0);
        CHECK(retained.positions[2] == -80.0);
        CHECK(erased.positions[5] == -40.0);
        CHECK(retained.positions[5] == -40.0);
        // Retained trace, bar 5: S2's exposure still pins S1's original X.
        CHECK(erased.legs[5].present && !erased.legs[5].dormant);
        CHECK(retained.legs[5].present && !retained.legs[5].dormant);
        CHECK(!retained.legs[5].withdrawn);
        CHECK(retained.legs[5].origin != 0);
        CHECK(retained.legs[5].qty == 40.0);
        CHECK(retained.legs[7].present && retained.legs[7].dormant);
        CHECK(!retained.legs[7].withdrawn && !retained.legs[7].restored);
        CHECK(erased.positions[7] == -40.0);
        CHECK(retained.positions[7] == -40.0);

        int margin_calls = 0;
        int through_x = 0;
        for (const auto& trade : erased.trades) {
            if (trade.exit_id == "__margin_call__") {
                ++margin_calls;
                CHECK(trade.entry_id == "S");
                CHECK(trade.exit_bar_index == expected_margin_bar);
                CHECK(trade.qty > 0.0 && trade.qty < 40.0);
            }
            if (trade.exit_id == "X") {
                ++through_x;
                CHECK(trade.entry_id == "S");
                if (through_x == 1) {
                    // Retained trace, bar 3: the original X closes S1, 40 at 105.
                    CHECK(trade.entry_incarnation == retained.legs[5].origin);
                    CHECK(trade.entry_bar_index == 1);
                    CHECK(trade.qty == 40.0);
                    CHECK(trade.exit_price == 105.0);
                    CHECK(trade.exit_bar_index == 3);
                } else {
                    // Retained trace, margin: after the 32/7 slice, revived X
                    // closes S2's remaining 248/7 at 175 on the declared path.
                    CHECK(trade.entry_incarnation != retained.legs[5].origin);
                    CHECK(trade.entry_bar_index == 2);
                    CHECK(trade.qty == 248.0 / 7.0);
                    CHECK(trade.exit_price == 175.0);
                    CHECK(trade.exit_bar_index == expected_margin_bar);
                }
            }
        }
        CHECK(margin_calls > 0);
        // Retained trace: the original X, one margin slice, then revived X.
        CHECK(through_x == 2);
        CHECK(erased.trades.size() == 3);
        // Retained trace, after revival: S2 is flat; realized capital is
        // 10000 - (40 * 5) - (40 * 75) = 6800, including both S2 slices.
        CHECK(erased.open == 0.0);
        CHECK(erased.equity == 6800.0);
        CHECK(erased.equity > 0.0 && erased.equity < 9800.0);

        // F2 is closed only if retaining the dead leg changes no public result.
        // Do not exempt the reference from this check or harvest a new value.
        CHECK(erased.observations == retained.observations);
        CHECK(trade_digest(erased.trades) == trade_digest(retained.trades));
        CHECK(erased.open == retained.open);
        CHECK(erased.equity == retained.equity);
        std::printf("F2 magnifier=%d recording=%d: trades %zu/%zu, open %.17g/%.17g, "
                    "equity %.17g/%.17g, retained X restored=%d\n",
                    magnifier ? 1 : 0, recording ? 1 : 0,
                    erased.trades.size(), retained.trades.size(), erased.open, retained.open,
                    erased.equity, retained.equity, retained.legs[9].restored ? 1 : 0);
    }
}
} // namespace

int main() {
    closed_origin_revival_matches_retained_execution();
    std::printf("test_k1_closed_origin_margin_revival: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
