#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr double held = 3;

enum class Action { Held, AfterExit, Explicit, Cancel, Market };
enum class Closer { ProtectiveStop, MarketClose, RawStop, LimitExit };
struct Row {
    const char* name;
    bool buy;
    Action action;
    int percent;
    bool rounded_budget = false;
    Closer closer = Closer::ProtectiveStop;

    int sign() const { return buy ? 1 : -1; }
    double stop() const { return rounded_budget ? 25'000'000'000.0 : buy ? 30 : 18; }
    double mark() const { return stop() - sign() * 6; }
    const char* close_label() const { return closer == Closer::MarketClose ? "__close__Base" : "Guard"; }
    double close_price() const {
        if (closer == Closer::MarketClose) return mark();
        if (closer == Closer::LimitExit) return mark() - sign();
        return stop() + sign() * (action == Action::Held ? 2 : -3);
    }
    double capital() const { return rounded_budget ? 6 * stop() - 40 : 360; }
    double placing_equity() const {
        return action == Action::Market ? capital() - held * std::abs(close_price() - mark()) : capital();
    }

    // Full margin, whole contracts, no fees. A resting buy above the close
    // normally funds. In the rounded case 149999999960 rounds to 150000000000
    // for sizing: six at the stop. At the close those six cost 149999999964.
    double own() const {
        if (rounded_budget) return 6;
        if (action == Action::Explicit) return 20;
        if (action == Action::Market) return std::floor(placing_equity() * percent / 100 / close_price());
        return std::floor(capital() * percent / 100 / stop());
    }
    bool declined() const { return action != Action::Market && own() * mark() > capital(); }
    double expected_position() const {
        if (action == Action::Cancel || closer != Closer::ProtectiveStop) return 0;
        if (action == Action::Market) return sign() * own();
        const double from_flat = action == Action::Held ? 0 : held;
        return sign() * (from_flat + (declined() ? 0 : own())) + 0.0;
    }
};

Bar bar(int i, double o, double h, double l, double c) {
    return {o, h, l, c, 100, 1'800'000'000'000LL + i * 60'000LL};
}

std::array<Bar, 7> prices(const Row& r) {
    std::array<Bar, 7> bars{};
    const double m = r.mark();
    for (int i = 0; i < 4; ++i) bars[i] = bar(i, m, m, m, m);
    const bool guard_only = r.action == Action::Cancel || r.action == Action::Market;
    const double c = guard_only ? r.close_price() : r.stop();
    // The rounded buy declines at placement. One tick lower, its six units
    // fit at the open, so the guard stays live for the rise to the buy stop.
    const double o = r.rounded_budget && r.action == Action::AfterExit ? m - 1 : m;
    bars[4] = r.buy ? bar(4, o, c + 1, o - 1, c) : bar(4, o, o + 1, c - 1, c);
    bars[5] = bar(5, c, c, c, c);
    bars[6] = r.action != Action::Cancel ? bar(6, c, c, c, c)
        : r.buy ? bar(6, c, r.stop() + 1, c, r.stop()) : bar(6, c, c, r.stop() - 1, r.stop());
    return bars;
}

struct Pending {
    int count = 0;
    std::uint64_t id = 0;
    int side = -1, close_only = -1;
    double carry = na, own = na, equity = na, mark = na;
    bool sized(const Row& r) const {
        return count == 1 && own == r.own() && close_only == r.declined()
            && equity == r.placing_equity() && mark == (r.action == Action::Market ? r.close_price() : r.mark());
    }
};

struct Fill {
    std::string label;
    int bar;
    std::uint64_t id;
    double price, closed, opened;
    bool is(const char* name, int at, double px, double cl, double op) const {
        return label == name && bar == at && price == px && closed == cl && opened == op;
    }
};

class Probe final : public source::PineStrategyHost {
public:
    explicit Probe(const Row& row) : r(row) {
        source::PineStrategyConfig c;
        c.initial_capital = r.capital();
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = r.percent;
        c.margin_long = c.margin_short = 100;
        c.pyramiding = 0;
        configure_pine_strategy(c);
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1);
        set_syminfo_pointvalue(1);
        set_syminfo_metadata("qty_step", 1);
        fixture_retain_all_events();
    }

    Pending observe() {
        Pending p;
        for (int i = 0; i < pending_order_count(); ++i) {
            pf_pending_order_v1_t order{};
            if (observe_pending_copy_v1(i, &order) != 0) { ++read_errors; continue; }
            if (std::strcmp(order.id, "Flip") != 0) continue;
            ++p.count;
            if (static_cast<bool>(order.is_long) != r.buy || !std::isnan(order.limit_price)
                || (r.action == Action::Market ? !std::isnan(order.stop_price) : order.stop_price != r.stop())) {
                ++read_errors;
            }
            p.id = order.incarnation;
            p.side = order.created_position_side;
            p.carry = order.tv_carry_qty;
            p.own = r.action == Action::Explicit ? order.qty
                : r.action == Action::Market ? order.frozen_default_qty : order.default_stop_placement_qty;
            p.close_only = order.affordability_close_only;
            p.equity = order.sizing_equity;
            p.mark = order.sizing_mark;
        }
        return p;
    }

    void on_source_bar(const Bar& b) override {
        last = pine_bar_index();
        if (last == 0) strategy_entry("Base", !r.buy, na, na, held);
        if (last == 1 && r.closer == Closer::ProtectiveStop)
            strategy_exit("Guard", "Base", na, r.close_price());
        if (last == 2 && r.action != Action::Market) {
            placed = live_position_size();
            strategy_entry("Flip", r.buy, na, r.stop(), r.action == Action::Explicit ? r.own() : na);
            call = observe(); // The priced reversal is still queued inside this callback.
        }
        if (last == 3) {
            before_path = live_position_size();
            if (r.action != Action::Market) working = observe();
            // Flip is already resting. Use the same label and stop price for
            // the raw order as for the protective exit. The limit's bracket
            // also has a stop, two ticks beyond Flip and outside the bar.
            if (r.closer == Closer::MarketClose) strategy_close("Base");
            if (r.closer == Closer::RawStop) strategy_order("Guard", r.buy, held, na, r.close_price());
            if (r.closer == Closer::LimitExit)
                strategy_exit("Guard", "Base", r.close_price(), r.stop() + 2 * r.sign());
        }
        if (last == 4 && r.action == Action::Market) {
            placed = live_position_size();
            strategy_entry("Flip", r.buy);
            call = observe();
        }
        if (last == 5 && r.action == Action::Cancel) {
            before_cancel = observe();
            cancel_book = live_position_size();
            strategy_cancel("Flip");
            after_cancel = observe();
        }
        if (last == (r.action == Action::Cancel ? 6 : 4))
            reached = r.buy ? b.high >= r.stop() : b.low <= r.stop();
        if (last == 6) end = live_position_size();
    }

    void on_native_applied_event(const native_order::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        fills.push_back({e.request().label, e.interval_index(), e.handle().incarnation,
                         e.resolved_price, e.closed_units, e.opened_units});
        if (e.request().label == r.close_label()) {
            const auto& trigger = e.request().trigger;
            if ((r.closer == Closer::MarketClose && !std::holds_alternative<native_order::Market>(trigger))
                || (r.closer == Closer::RawStop && !std::holds_alternative<native_order::Stop>(trigger))
                || (r.closer == Closer::LimitExit && !std::holds_alternative<native_order::Limit>(trigger)))
                ++read_errors;
            guard_book = live_position_size();
            if (r.action != Action::Market) at_exit = observe();
        }
    }

    const Row& r;
    int last = -1, read_errors = 0;
    bool reached = false;
    double placed = na, before_path = na, guard_book = na, cancel_book = na, end = na;
    Pending call, working, at_exit, before_cancel, after_cancel;
    std::vector<Fill> fills;
};

int check(const Row& r) {
    Probe p(r);
    const auto bars = prices(r);
    p.run(bars.data(), static_cast<int>(bars.size()), "1", "1");

    const bool market = r.action == Action::Market, cancel = r.action == Action::Cancel;
    const bool guard_first = r.action != Action::Held;
    const double seed = -r.sign() * held;
    const bool complete = p.native_state().kind == NativeLifecycleKind::Completed
        && p.last_error().empty() && p.last == 6;
    const bool seeded = !p.fills.empty() && p.fills[0].is("Base", 1, r.mark(), 0, seed)
        && p.before_path == seed && p.placed == (market ? 0 : seed);
    const auto placed_side = static_cast<int>(r.buy ? PositionSide::SHORT : PositionSide::LONG);
    const bool submitted = p.call.sized(r) && p.call.carry == 0
        && (market || (p.call.id == 0 && p.working.sized(r) && p.working.id != 0
                       && p.working.side == placed_side && p.working.carry == held));
    const auto same_working_order = [&](const Pending& q) {
        return q.sized(r) && q.id == p.working.id && q.side == placed_side && q.carry == held;
    };
    const bool guarded = guard_first
        ? p.fills.size() >= 2 && p.fills[1].is(r.close_label(), 4, r.close_price(), held, 0)
            && p.guard_book == 0 && (market || same_working_order(p.at_exit))
        : std::isnan(p.guard_book);
    // In a red run, a missing fill alone does not prove the stop fired.
    // Its terms receipt identifies the actual order, bar, price and units.
    double terms = na;
    if (complete && !market && p.working.id != 0) {
        for (const auto& event : p.native_events(0)) {
            if (!event.command) continue;
            const auto* t = std::get_if<native_order::TermsResolvedEvent>(&*event.command);
            if (t && t->handle().incarnation == p.working.id && t->cursor.point.interval_index == 4
                && t->input.terms.resolved_price == r.stop() && t->input.terms.units)
                terms = *t->input.terms.units;
        }
    }
    const bool other_closer = r.closer != Closer::ProtectiveStop;
    const bool setup = complete && seeded && submitted && guarded && !p.read_errors
        && (!other_closer || r.declined())
        && (market || p.reached) && (market || cancel || std::isfinite(terms))
        && (!cancel || (p.cancel_book == 0 && same_working_order(p.before_cancel)));

    const std::size_t prefix = guard_first ? 2 : 1;
    const Fill* flip = p.fills.size() > prefix ? &p.fills[prefix] : nullptr;
    const double expected = r.expected_position();
    const bool settled = cancel ? p.after_cancel.count == 0 && p.fills.size() == prefix
        : other_closer ? terms == 0 && p.fills.size() == prefix
        : flip && p.fills.size() == prefix + 1
            && flip->is("Flip", market ? 5 : 4, market ? r.close_price() : r.stop(),
                        guard_first ? 0 : held, expected)
            && (market || flip->id == p.working.id);
    const bool ok = settled && p.end == expected;
    std::printf("%s %s: placed=%g call=%g working=%g flat_carry=%g own=%g close_only=%d "
                "equity=%.17g mark=%.17g terms=%g cancel=%d->%d fills=%zu end=%g expected=%g",
                !setup ? "NOT COMPARED" : ok ? "PASS" : "FAIL", r.name, p.placed,
                p.call.carry, p.working.carry, p.at_exit.carry, p.call.own, p.call.close_only,
                p.call.equity, p.call.mark, terms, p.before_cancel.count, p.after_cancel.count,
                p.fills.size(), p.end + 0.0, expected);
    for (const auto& f : p.fills)
        std::printf(" %s@%d:%.17g/%g/%g", f.label.c_str(), f.bar, f.price, f.closed, f.opened);
    if (!setup) {
        std::printf(" last=%d completed=%d error='%s' read_errors=%d before_path=%g guard_book=%g "
                    "cancel_book=%g reached=%d", p.last, complete, p.last_error().c_str(), p.read_errors,
                    p.before_path, p.guard_book, p.cancel_book, p.reached);
        for (const auto& q : {p.call, p.working, p.at_exit, p.before_cancel, p.after_cancel})
            std::printf(" [rows=%d id=%llu side=%d carry=%g own=%g flag=%d]", q.count,
                        static_cast<unsigned long long>(q.id), q.side, q.carry, q.own, q.close_only);
    }
    std::printf("\n");
    return !setup ? 2 : ok ? 0 : 1;
}
} // namespace

int main() {
    const Row controls[] = {
        {"held sell", false, Action::Held, 100}, {"held buy", true, Action::Held, 100, true},
        {"funded sell", false, Action::AfterExit, 50}, {"funded buy", true, Action::AfterExit, 50},
        {"explicit sell", false, Action::Explicit, 100}, {"cancelled sell", false, Action::Cancel, 100},
        {"market sell", false, Action::Market, 50}
    };
    for (const Row& r : controls)
        if (const int rc = check(r)) return rc;
    int status = 0;
    for (const Row& r : {Row{"declined sell", false, Action::AfterExit, 100},
                         Row{"declined buy", true, Action::AfterExit, 100, true},
                         Row{"after market close", false, Action::AfterExit, 100, false, Closer::MarketClose},
                         Row{"after raw stop", false, Action::AfterExit, 100, false, Closer::RawStop},
                         Row{"after limit exit", false, Action::AfterExit, 100, false, Closer::LimitExit}}) {
        const int rc = check(r);
        if (rc == 2) return rc;
        if (rc) status = rc;
    }
    return status;
}
