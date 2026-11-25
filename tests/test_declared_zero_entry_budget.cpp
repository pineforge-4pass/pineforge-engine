#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

enum class Route { Market, Limit, Stop, AtClose, CloseFirst, EmptyClose, CloseAll,
                   CountedClose, DeferredSell, RearmedBuy, Replacement };
enum class Budget { Zero, Funded, BelowLot };

bool full_close(Route r) {
    return r == Route::CloseFirst || r == Route::EmptyClose || r == Route::CloseAll
        || r == Route::CountedClose;
}

std::vector<Bar> tape(Route r) {
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) {
        double price = full_close(r) ? 80 : 100;
        if (r == Route::Limit && i >= 3) price = 80;
        if (r == Route::Stop && i >= 3) price = 120;
        if (r == Route::Replacement && i >= 3) price = i == 3 ? 94 : 90;
        if (r == Route::DeferredSell) price = i == 1 ? 120 : 80;
        if (r == Route::RearmedBuy && i >= 3) price = 140;
        bars.push_back(flat(i, price));
    }
    if (r == Route::RearmedBuy) bars[2] = bar(2, 100, 126, 95, 110);
    return bars;
}

struct Pending {
    int count = 0;
    std::uint64_t incarnation = 0;
    double limit = missing;
};

class Probe final : public Host {
public:
    Probe(Amount a, Route r, Budget b)
        : Host(a, r == Route::AtClose || r == Route::CountedClose), route(r) {
        auto declaration = fixture_configuration();
        declaration.default_qty_type = type(a);
        declaration.default_qty_value = b == Budget::Zero ? 0
            : b == Budget::BelowLot ? (a == Amount::Cash ? 1 : 0.005)
            : (a == Amount::Cash ? 800 : 4);
        configure_pine_strategy(declaration);
        fixture_retain_all_events();
    }

    Pending pending(const char* id) {
        Pending result;
        for (int i = 0; i < pending_order_count(); ++i) {
            pf_pending_order_v1_t row{};
            if (pending_intent_view().copy_v1(i, &row) != 0) { readable = false; continue; }
            if (std::string(row.id) != id) continue;
            ++result.count;
            result.incarnation = row.incarnation;
            result.limit = row.limit_price;
        }
        return result;
    }

    void issue(bool buy, double limit = missing, double stop = missing) {
        ++calls;
        strategy_entry("budget", buy, limit, stop);
        at_call = pending("budget");
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        consecutive &= index == last_bar + 1;
        last_bar = index;
        if (last_bar == 0) {
            // Prove this run can place, read and cancel a real order even when
            // its declaration is zero. Remove it before the scenario starts.
            strategy_entry("witness", true, 20, missing, 1);
            witness = pending("witness");
            strategy_cancel("witness");
            started_flat = pending_order_count() == 0 && physical_position().signed_units == 0;
        }
        if (route == Route::CountedClose) set_pine_risk_max_intraday_filled_orders(20);
        if (last_bar == 1) {
            if (full_close(route)) strategy_entry("seed", true, missing, missing, 1);
            else if (route == Route::DeferredSell) {
                issue(false, missing, 124);
                strategy_entry("seed", true, missing, 75, 1);
            } else if (route == Route::RearmedBuy) {
                strategy_entry("seed", true, missing, missing, 1);
                strategy_exit("protect", "seed", missing, 96);
                issue(true, missing, 120);
            } else if (route == Route::Replacement) {
                strategy_entry("budget", true, 96, missing, 1);
            } else if (route == Route::Limit) issue(true, 90);
            else if (route == Route::Stop) issue(true, missing, 110);
            else issue(true);
        }
        if (last_bar == 2) {
            if (full_close(route)) {
                held = physical_position().signed_units;
                if (route == Route::CloseFirst) strategy_close_all();
                issue(false);
                if (route == Route::CloseAll) strategy_close_all();
                if (route == Route::EmptyClose || route == Route::CountedClose) strategy_close("");
            } else if (route == Route::Replacement) {
                prior = pending("budget");
                issue(true, 90);
            } else if (route == Route::Limit || route == Route::Stop) {
                resting = pending("budget");
            }
        }
    }

    const Route route;
    Pending witness, at_call, resting, prior;
    bool started_flat = false, readable = true, consecutive = true;
    int calls = 0;
    double held = missing;
};

struct Scenario {
    Route route;
    const char* name;
    int funded_handles, fill_bar;
    double price, cash_position, equity_position;
};

bool check(const Scenario& s, Amount a, Budget b, bool control_ok = true) {
    Probe p(a, s.route, b);
    const bool done = p.completed(tape(s.route));
    std::vector<std::uint64_t> handles;
    int witness_accepts = 0, witness_cancels = 0, replacements = 0;
    std::uint64_t replaced = 0, no_effect = 0;
    for (const auto& e : p.native_events(0)) {
        if (!e.command) continue;
        if (const auto* n = std::get_if<no::AcceptedEvent>(&*e.command)) {
            if (n->request().label == "budget") handles.push_back(n->handle().incarnation);
            if (n->request().label == "witness" && n->handle().incarnation == p.witness.incarnation)
                ++witness_accepts;
        } else if (const auto* r = std::get_if<no::ReplacedEvent>(&*e.command)) {
            if (r->successor_request().label == "budget") {
                ++replacements;
                replaced = r->predecessor().incarnation;
                if (!r->kept_handle()) handles.push_back(r->successor().incarnation);
            }
        } else if (const auto* c = std::get_if<no::CancelledEvent>(&*e.command)) {
            if (c->handle().incarnation == p.witness.incarnation) ++witness_cancels;
        } else if (const auto* n = std::get_if<no::NoEffectEvent>(&*e.command)) {
            if (n->request().label == "budget" && n->cursor.point.interval_index == 3)
                no_effect = n->handle().incarnation;
        }
    }
    const bool funded = b == Budget::Funded, zero = b == Budget::Zero;
    const bool replace = s.route == Route::Replacement, counted = s.route == Route::CountedClose;
    const bool sell = s.route == Route::DeferredSell, rearm = s.route == Route::RearmedBuy;
    const bool close = full_close(s.route);
    const int final_pending = p.pending_order_count();
    bool measured = control_ok && done && p.consecutive && p.calls == 1 && p.readable && p.started_flat
        && p.witness.count == 1 && p.witness.incarnation != 0 && p.witness.limit == 20
        && witness_accepts == 1 && witness_cancels == 1 && p.native_event_window_start() == 1;
    const auto* seed = p.only_fill("seed");
    const char* close_id = rearm ? "protect" : s.route == Route::EmptyClose || counted
        ? "__pine_close_all" : "__close__";
    const auto* closed = p.only_fill(close_id);
    if (close || sell || rearm) {
        measured &= seed && seed->bar == (counted ? 1 : 2) && seed->price == (rearm ? 100 : 80)
            && seed->opened == 1 && seed->closed == 0;
    }
    if (close || rearm) {
        measured &= seed && closed && closed->ordinal > seed->ordinal
            && closed->bar == (counted || rearm ? 2 : 3) && closed->price == (rearm ? 96 : 80)
            && closed->closed == 1 && closed->opened == 0 && (!close || p.held == 1);
    }
    if (counted) measured &= seed && seed->charged_slots == 1 && closed && closed->charged_slots == 2;
    if (replace) measured &= p.prior.count == 1 && p.prior.limit == 96 && p.prior.incarnation != 0;

    const bool keep_prior = replace && zero;
    const int expected_handles = zero ? (replace ? 1 : 0) : s.funded_handles;
    const double position = funded ? (a == Amount::Cash ? s.cash_position : s.equity_position)
                                   : (keep_prior || sell ? 1 : 0);
    const auto* fill = p.only_fill("budget");
    const std::size_t expected_fills = (close || rearm ? 2 : sell ? 1 : 0) + (funded || keep_prior);
    bool ok = handles.size() == static_cast<std::size_t>(expected_handles)
        && p.at_call.count == (zero && !replace ? 0 : 1)
        && replacements == (replace && funded ? 1 : 0)
        && final_pending == 0 && p.native_working_requests().empty()
        && p.fills.size() == expected_fills && p.live_position_size() == position;
    if (s.route == Route::Limit || s.route == Route::Stop)
        ok &= p.resting.count == p.at_call.count && p.resting.incarnation == p.at_call.incarnation
            && (zero || (!handles.empty() && handles.front() == p.resting.incarnation));
    if (replace) {
        ok &= !handles.empty() && handles.front() == p.prior.incarnation
            && p.at_call.incarnation == handles.back() && p.at_call.limit == (zero ? 96 : 90)
            && (zero || (replaced == p.prior.incarnation && handles.back() != replaced));
    }
    if (funded && s.funded_handles == 2 && !replace)
        ok &= handles.size() == 2 && handles.front() == p.at_call.incarnation
            && handles.front() != handles.back();
    if (funded || keep_prior) {
        ok &= fill && !handles.empty() && fill->incarnation == handles.back()
            && fill->bar == (keep_prior ? 3 : s.fill_bar) && fill->price == (keep_prior ? 94 : s.price)
            && fill->opened == position && fill->closed == (sell ? 1 : 0)
            && (!seed || fill->ordinal > seed->ordinal) && (!closed || fill->ordinal > closed->ordinal)
            && (!counted || (fill->charged_slots == 2 && fill->at_close(ClosePoint::Calculation)))
            && (s.route != Route::AtClose || fill->at_close(ClosePoint::AfterCalculation));
    } else {
        ok &= !fill;
        if (!zero) ok &= !handles.empty() && no_effect == handles.front();
    }
    std::printf("%s %s / %s / %s: handles=%zu pending=%d/%d fills=%zu position=%g\n",
                !measured ? "NOT COMPARED" : ok ? "PASS" : "FAIL", s.name, name(a),
                funded ? "funded" : zero ? "zero" : "below lot", handles.size(), p.at_call.count,
                final_pending, p.fills.size(), p.live_position_size());
    return measured && ok;
}
} // namespace

int main() {
    // 800 cash = 4% of 20,000. One contract costs twice the price; floor to
    // quarters. A percent stop freezes at its stop (at the signal if already
    // marketable); a cash stop spends its budget at the fill. The deferred
    // sell must spend one of its contracts closing the intervening long.
    const Scenario cases[] = {
        {Route::Market, "market", 1, 2, 100, 4, 4},
        {Route::Limit, "resting limit", 1, 3, 80, 5, 5},
        {Route::Stop, "resting stop", 1, 3, 120, 3.25, 3.5},
        {Route::AtClose, "bar close", 1, 1, 100, 4, 4},
        {Route::CloseFirst, "close first", 1, 3, 80, -5, -5},
        {Route::EmptyClose, "rebuilt after close", 2, 3, 80, -5, -5},
        {Route::CloseAll, "rebuilt after close_all", 2, 3, 80, -5, -5},
        {Route::CountedClose, "counted close", 2, 2, 80, -5, -5},
        {Route::DeferredSell, "deferred sell stop", 2, 2, 80, -4, -2.25},
        {Route::RearmedBuy, "rearmed buy stop", 2, 3, 140, 2.75, 3.25},
        {Route::Replacement, "same-ID limit", 2, 4, 90, 4.25, 4.25},
    };
    bool ok = true;
    for (const auto& s : cases) for (auto a : {Amount::Cash, Amount::Equity}) {
        const bool control = check(s, a, Budget::Funded);
        ok &= check(s, a, Budget::Zero, control);
        if (s.route == Route::Limit) ok &= check(s, a, Budget::BelowLot, control);
    }
    return ok ? 0 : 1;
}
