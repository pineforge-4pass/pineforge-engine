// Read the old stop *before* the market reversal matches. After a successful
// reversal its owner is gone, so an absent stop or a later missed fill says
// nothing about whether the opening funding check suspended it.
#include <pineforge/exit_leg_lifecycle.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
using Phase = NativePathPhase;
using Event = std::uint64_t;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double capital = 3030, entry_price = 60, signal = 64, seed = 1.5;
constexpr double point_value = 4, fx = 1.25, step = 0.5, tick = 0.125;
constexpr double stop = 58, high = 67, low = 56;
constexpr int entry_bar = 1, arm_bar = 2, signal_bar = 3, decision_bar = 4, last_bar = 5;

struct Scenario {
    const char* name;
    std::optional<QtyType> type;
    double amount;
    double open;
    double margin_pct;
};

double equity_at(double price) {
    return capital + seed * (price - entry_price) * point_value * fx;
}

// The new side is an opening, after closing the seed. No fees or slippage.
// Derive it from the input money and the fill quote, never a host sizing read.
double contracts(const Scenario& s, double price) {
    if (!s.type) return 0;
    const double money = *s.type == QtyType::PERCENT_OF_EQUITY
        ? equity_at(price) * s.amount / 100 : s.amount;
    const double units = *s.type == QtyType::FIXED ? s.amount : money / (price * point_value * fx);
    return step * std::floor(units / step);
}

double required(const Scenario& s, double price) {
    return contracts(s, price) * price * point_value * fx * s.margin_pct / 100;
}

std::array<Bar, last_bar + 1> feed(double open) {
    constexpr std::int64_t start = 1'767'225'600'000LL; // 2026-01-01 00:00 UTC
    const auto bar = [&](int i, double o, double h, double l, double c) -> Bar {
        return {o, h, l, c, 240, start + 60'000LL * i};
    };
    return {{bar(0,60,61,59,60), bar(1,60,61,59,60), bar(2,60,64,59,64),
             bar(3,64,65,63,64), bar(4,open,high,low,64), bar(5,64,65,63,64)}};
}

void need(bool condition, const char* evidence) {
    if (!condition) throw std::runtime_error(evidence);
}

struct Book {
    NativeCoordinate point;
    double quote, exposure, equity;
    std::vector<NativeOpenLot> lots;
    std::vector<NativeWorkingRequest> working;
    std::vector<pf_pending_order_v1_t> pending;

    const pf_pending_order_v1_t* order(const char* id) const {
        const pf_pending_order_v1_t* found = nullptr;
        for (const auto& p : pending) if (std::strcmp(p.id, id) == 0) {
            need(!found, "ambiguous pending order ID");
            found = &p;
        }
        return found;
    }

    const no::Request& live_request(const pf_pending_order_v1_t& p) const {
        for (const auto& w : working)
            if (w.definition->handle.incarnation == p.incarnation && w.definition->request.label == p.id)
                return w.definition->request;
        throw std::runtime_error("pending ID/incarnation is not a working native request");
    }
};

class Host final : public source::PineStrategyHost {
public:
    explicit Host(const Scenario& s) : s_(s) {
        source::PineStrategyConfig c;
        c.calc_on_order_fills = true;
        c.initial_capital = capital;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 3; // both entries supply their own quantity
        c.margin_long = c.margin_short = s.margin_pct;
        configure_pine_strategy(c);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(tick);
        set_syminfo_pointvalue(point_value);
        set_syminfo_metadata("qty_step", step);
        need(set_account_currency_fx_scalar(fx), "scalar FX setup");
        set_margin_call_enabled(false);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const auto p = current_execution_point();
        if (!p || p->decision.coordinate.provenance != NativePriceProvenance::Calculation) return;
        const int i = p->decision.coordinate.interval_index;
        if (i < 0 || i > last_bar || pine_bar_index() != i) {
            wrong_bar = true;
            return;
        }
        ++calculations[i];
        if (i == 0) strategy_entry("L", true, na, na, seed, "", "", 0, static_cast<int>(QtyType::FIXED));
        if (i == arm_bar) strategy_exit("X", "L", na, stop);
        if (i == signal_bar) {
            standing = book(*p); // before submitting R
            if (s_.type) strategy_entry("R", false, na, na, s_.amount, "", "", 0, static_cast<int>(*s_.type));
        }
        if (i == decision_bar) after = book(*p);
    }

    // COOF's ordinary script-state checkpoint. There are no script variables
    // to roll back here. The open hook has finished its policy; the kernel
    // has not matched the open. This observer only copies public readbacks.
    void snapshot_script_state() override {
        const auto p = current_execution_point();
        if (p && p->decision.coordinate.interval_index == decision_bar
            && p->decision.coordinate.path_phase == Phase::Open
            && p->decision.coordinate.provenance == NativePriceProvenance::ModeledOHLCOpen) {
            ++checkpoints;
            opening = book(*p);
        }
    }

    bool wrong_bar = false;
    int checkpoints = 0;
    std::array<int, last_bar + 1> calculations{};
    std::optional<Book> standing, opening, after;

private:
    const Scenario& s_;
    Book book(const NativeCurrentPointView& p) const {
        Book b{p.decision.coordinate, p.price, physical_position().signed_units,
               native_marked_equity(p.price), native_open_lots(p.price), native_working_requests(), {}};
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0 && row.struct_version == PF_PENDING_ORDER_STRUCT_VERSION
                 && row.size == sizeof(row), "pending read failed");
            b.pending.push_back(row);
        }
        return b;
    }
};

struct Tape {
    std::vector<no::ExecutionAppliedEvent> fills;
    std::vector<no::MatchRejectedEvent> refusals;
    std::vector<no::CancelledEvent> ends;
    std::vector<NativeDriverPoint> path;

    Event waypoint(Phase phase, double price) const {
        Event found = 0;
        for (const auto& p : path) if (p.coordinate.path_phase == phase && p.raw_price == price) {
            need(!found, "ambiguous driver phase");
            found = p.coordinate.ordinal;
        }
        need(found != 0, "missing driver phase or wrong price");
        return found;
    }
};

Tape read_tape(const Host& h) {
    Tape t;
    for (const auto& e : h.native_events(0)) {
        if (e.driver && e.driver->coordinate.interval_index == decision_bar) t.path.push_back(*e.driver);
        if (!e.command) continue;
        if (auto* f = std::get_if<no::ExecutionAppliedEvent>(&*e.command)) t.fills.push_back(*f);
        if (auto* r = std::get_if<no::MatchRejectedEvent>(&*e.command)) t.refusals.push_back(*r);
        if (auto* c = std::get_if<no::CancelledEvent>(&*e.command)) t.ends.push_back(*c);
    }
    return t;
}

bool fill_is(const no::ExecutionAppliedEvent& e, const char* id, int bar, Phase phase,
             double price, double opened, double closed) {
    return e.request().label == id && e.interval_index() == bar && e.cursor.point.path_phase == phase
        && e.resolved_price == price && e.opened_units == opened && e.closed_units == closed && e.terminal;
}

const pf_pending_order_v1_t& attached_stop(const Book& b, Event parent) {
    const auto* x = b.order("X");
    need(x && x->incarnation && x->created_bar == arm_bar && std::strcmp(x->from_entry, "L") == 0
         && x->legs_definition_value_present && x->legs_definition_stop_price == stop
         && x->legs_target_incarnation == x->incarnation && !x->legs_retirement0_present
         && std::holds_alternative<no::Stop>(b.live_request(*x).trigger), "working X stop attached to L");
    need(b.lots.size() == 1 && b.lots[0].entry_label == "L" && b.lots[0].entry_incarnation == parent
         && b.lots[0].cycle == x->legs_target_owner && b.exposure == seed, "X owner and seed exposure");
    return *x;
}

// Return true only for an otherwise completely identified funded reversal
// whose still-working X bears a suspension caused by this exact open.
bool compare(const Scenario& s) {
    const auto bars = feed(s.open);
    Host h(s);
    h.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!h.last_error().empty()) throw std::runtime_error(h.last_error());
    const auto state = h.native_state();
    need(state.kind == NativeLifecycleKind::Completed && state.completion == NativeCompletion::BatchComplete
         && state.spec && state.spec->account_fx == fx
         && state.spec->point_value == point_value && state.spec->quantity_grid == step, "completed run and sizing inputs");
    for (int n : h.calculations) need(n == 1, "missing or repeated ordinary bar calculation");
    need(!h.wrong_bar && h.checkpoints == 1 && h.standing && h.opening && h.after, "missing or ambiguous observation");
    const auto& prior = *h.standing;
    const auto& open = *h.opening;
    const auto& after = *h.after;
    const auto tape = read_tape(h);
    const Event o = tape.waypoint(Phase::Open, s.open);
    const Event hi = tape.waypoint(Phase::High, high);
    const Event lo = tape.waypoint(Phase::Low, low);
    need(prior.point.ordinal < o && o == open.point.ordinal && o < hi && hi < lo && lo < after.point.ordinal
         && prior.quote == signal && open.quote == s.open && prior.equity == equity_at(signal)
         && open.equity == equity_at(s.open), "signal, pre-match open, high, stop-crossing low, close");
    need(!tape.fills.empty() && fill_is(tape.fills.front(), "L", entry_bar, Phase::Open, entry_price, seed, 0)
         && tape.fills.front().ordinal < prior.point.ordinal, "seed fill ID/bar/phase/price/size");
    const auto parent = tape.fills.front().handle().incarnation;
    const auto& before = attached_stop(prior, parent);
    const auto& x = attached_stop(open, parent);
    need(!before.legs_suspension_present && before.incarnation == x.incarnation
         && !prior.order("R"), "same X was armed before the reversal signal");
    const bool suspended = x.legs_suspension_present != 0;
    if (suspended) {
        const auto leg = static_cast<unsigned>(exit_legs::Leg::Stop);
        const bool includes_stop = (x.legs_suspension_legs_count > 0 && x.legs_suspension_legs_item0 == leg)
            || (x.legs_suspension_legs_count > 1 && x.legs_suspension_legs_item1 == leg)
            || (x.legs_suspension_legs_count > 2 && x.legs_suspension_legs_item2 == leg);
        need(includes_stop && x.legs_suspension_cause_event == o && x.legs_suspension_cause_bar == decision_bar
             && x.legs_suspension_cause_phase == static_cast<unsigned>(exit_legs::Phase::Observation)
             && x.legs_suspension_cause_domain == static_cast<unsigned>(exit_legs::Domain::FillRecalc)
             && !x.legs_suspension_hold_present && !x.legs_suspension_replacement_present
             && x.legs_suspension_window_present, "X suspension must be this open's observation, not a hold or replacement");
    }

    const auto* r = open.order("R");
    const double units = contracts(s, s.open);
    const double cost = required(s, s.open);
    const bool funded = cost <= equity_at(signal);
    if (s.type) {
        need(required(s, signal) <= equity_at(signal) && funded == (cost <= equity_at(s.open)),
             "placement must fit; signal and marked equity must agree on open funding");
        need(r && r->incarnation && r->is_long == 0 && r->created_bar == signal_bar
             && r->created_position_side == static_cast<int>(PositionSide::LONG)
             && r->qty_type == static_cast<int>(*s.type) && r->qty == s.amount
             && std::holds_alternative<no::Market>(open.live_request(*r).trigger), "working opposite market R with specified amount and type");
    } else {
        need(!r, "unexpected R in bracket-only control");
    }
    std::cout << "OBS " << s.name << " bar=" << decision_bar << "/Open price=" << open.quote << " L=" << open.exposure
              << " X#" << x.incarnation << " R#" << (r ? r->incarnation : 0)
              << " equity=" << open.equity << " units=" << units << " required=" << cost
              << " suspended=" << suspended << " open-event=" << o << " cause=" << x.legs_suspension_cause_event
              << " fills=" << tape.fills.size() << " refusals=" << tape.refusals.size()
              << " cancels=" << tape.ends.size() << " close-position=" << after.exposure << '\n';
    for (const auto& f : tape.fills)
        std::cout << "  fill " << f.request().label << '#' << f.handle().incarnation << " event=" << f.ordinal
                  << " bar=" << f.interval_index() << " phase=" << static_cast<int>(f.cursor.point.path_phase)
                  << " price=" << f.resolved_price << " opened=" << f.opened_units << " closed=" << f.closed_units << '\n';
    for (const auto& reject : tape.refusals)
        std::cout << "  reject " << reject.request().label << '#' << reject.handle().incarnation
                  << " event=" << reject.ordinal << " bar=" << reject.cursor.point.interval_index
                  << " phase=" << static_cast<int>(reject.cursor.point.path_phase)
                  << " reason=" << static_cast<int>(reject.reason) << '\n';
    for (const auto& end : tape.ends)
        std::cout << "  cancel " << end.request().label << '#' << end.handle().incarnation << " event=" << end.ordinal
                  << " reason=" << static_cast<int>(end.reason) << " high-event=" << hi << '\n';

    need(!after.order("R") && h.physical_position().signed_units == after.exposure, "terminal R and final physical exposure");
    if (!r) {
        need(!suspended && tape.fills.size() == 2 && tape.refusals.empty() && tape.ends.empty()
             && fill_is(tape.fills[1], "X", decision_bar, Phase::Low, stop, 0, seed)
             && tape.fills[1].handle().incarnation == x.incarnation && tape.fills[1].ordinal > lo
             && after.exposure == 0 && !after.order("X"), "bracket-only control: X must actually fill on the low");
    } else if (!funded) {
        need(tape.fills.size() == 1 && tape.refusals.size() == 2 && tape.ends.empty(), "decline control: two refusals, no closing fill");
        const auto& rev = tape.refusals[0];
        const auto& leg = tape.refusals[1];
        need(rev.request().label == "R" && rev.handle().incarnation == r->incarnation
             && rev.cursor.point.interval_index == decision_bar && rev.cursor.point.path_phase == Phase::Open
             && rev.reason == no::MatchRejectReason::HostPrecommit && o < rev.ordinal && rev.ordinal < hi
             && rev.attempted_terms && rev.attempted_terms->shape == no::OpeningShape::ReverseTo
             && rev.attempted_terms->units == units && rev.attempted_terms->resolved_price == s.open,
             "decline control: R really refused at open for the independently sized reverse-to");
        need(suspended && leg.request().label == "X" && leg.handle().incarnation == x.incarnation
             && leg.cursor.point.interval_index == decision_bar && leg.cursor.point.path_phase == Phase::Low
             && leg.reason == no::MatchRejectReason::HostPrecommit && lo < leg.ordinal && leg.ordinal < after.point.ordinal
             && after.exposure == seed && after.lots.size() == 1 && after.lots[0].entry_incarnation == parent,
             "decline control: suspended X refused on the low; original L remains");
    } else {
        need(tape.fills.size() == 2 && tape.refusals.empty() && tape.ends.size() == 1, "funded control: seed, reversal, bracket cancellation");
        const auto& rev = tape.fills[1];
        const auto& end = tape.ends.front();
        need(fill_is(rev, "R", decision_bar, Phase::Open, s.open, -units, seed)
             && rev.handle().incarnation == r->incarnation && o < rev.ordinal && rev.ordinal < hi
             && after.exposure == -units && after.lots.size() == 1 && after.lots[0].entry_label == "R"
             && after.lots[0].entry_incarnation == r->incarnation, "funded R must actually close L and open the sized short");
        need(end.request().label == "X" && end.handle().incarnation == x.incarnation
             && (end.reason == no::CancelReason::OwnerGone || end.reason == no::CancelReason::User)
             && rev.ordinal < end.ordinal && end.ordinal < hi && !after.order("X"),
             "old X must end after the reversal, before the trigger excursion");
        return suspended;
    }
    return false;
}
} // namespace

int main() {
    const Scenario cash{"cash", QtyType::CASH, 2420, 64, 50};
    const Scenario fixed{"fixed", QtyType::FIXED, contracts(cash, cash.open), cash.open, cash.margin_pct};
    const Scenario controls[] = {
        {"stop-only", std::nullopt, 0, 65, 50}, fixed,
        {"cash-declined", QtyType::CASH, 6200, 65, 50},
        {"cash-margin-rescue", QtyType::CASH, 6200, 65, 0.125},
        {"percent-small", QtyType::PERCENT_OF_EQUITY, 15, 64, 50},
        {"percent-lower-margin", QtyType::PERCENT_OF_EQUITY, 40, 64, 20},
    };
    const Scenario candidates[] = {
        cash, {"percent", QtyType::PERCENT_OF_EQUITY, 40, 64, 50},
        {"percent-equity-basis", QtyType::PERCENT_OF_EQUITY, 42, 64, 50},
        {"cash-level-open", QtyType::CASH, 6200, 64, 50},
    };
    std::vector<std::string> premature;
    bool comparable = true;
    const auto run = [&](const Scenario& s, bool control) {
        try {
            if (compare(s)) {
                need(!control, "funded control suspended; the funding check is not isolated");
                premature.emplace_back(s.name);
            }
        } catch (const std::exception& e) {
            comparable = false;
            std::cerr << "NOT COMPARED " << s.name << ": " << e.what() << '\n';
        }
    };
    for (const auto& s : controls) run(s, true);
    for (const auto& s : candidates) run(s, false);
    if (!comparable) return 2;
    if (!premature.empty()) {
        std::cerr << "FAIL a funded reversal suspended its bracket: working X suspended at bar 4/Open before funded R filled:";
        for (const auto& name : premature) std::cerr << ' ' << name;
        std::cerr << '\n';
        return 1;
    }
    std::cout << "PASS funded reversals left X armed until settlement; decline and touch controls passed\n";
}
