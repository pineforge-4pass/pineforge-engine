#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace pineforge;
namespace no = native_order;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1704067200000LL, minute = 60000;

void need(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}

struct Case {
    const char* name;
    bool buy;
    double requested;  // fixed qty passed to strategy_entry
    double held;  // signed position at the gap
    bool placed_opposite = false;
    bool preopen_call = false;
};

class StopHost final : public source::PineStrategyHost {
public:
    explicit StopHost(Case c) : c(c) {
        source::PineStrategyConfig config;
        config.initial_capital = c.preopen_call ? 5000 : 10000;
        config.pyramiding = 2;
        config.slippage = c.preopen_call ? 0 : 1;
        config.close_entries_rule_any = true;
        if (c.preopen_call) {
            config.margin_long = config.margin_short = 50;
            config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            config.default_qty_value = 160;  // 1.6 * 5000 / 100 = 80
        }
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1.0);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        need(i == bars_seen++, "missing or repeated script bar");
        if (i == 0 && !c.placed_opposite) {
            need(live_position_size() == 0 && closed_trade_count() == 0,
                 "expected a fresh flat book when requesting the fixed stop");
            place_stop();
        }
        if (i == 1) {
            need(live_position_size() == 0, "expected a flat book before placing the held entry");
            if (!c.placed_opposite) check_stop(false);
            if (c.preopen_call)
                strategy_entry("Held", true, na, 100);  // marketable buy stop, next chart open
            else
                strategy_entry("Held", c.held > 0, na, na, 80);
        }
        if (!c.preopen_call && i == 2) {
            need(live_position_size() == c.held, "held entry was not on the book before the gap");
            if (c.placed_opposite) place_stop();
            // This exit belongs to the current cycle. Keep it unbound to the
            // physical book so only the adapter can cancel it at the reversal.
            strategy_exit("Guard", "", c.held > 0 ? 140 : 60, na);
        }
        if (!c.preopen_call && i == 3) {
            check_stop(c.placed_opposite);
            strategy_order("Park", true, 1, 10);
            const auto row = pending("Guard");
            need(row.from_entry[0] == '\0' && row.created_position_cycle_seq == held_cycle
                 && row.created_position_side == static_cast<int>(
                        c.held > 0 ? PositionSide::LONG : PositionSide::SHORT),
                 "expected the original global limit on the held cycle");
            for (const auto& w : native_working_requests()) {
                if (w.definition->handle.incarnation == row.incarnation)
                    guard = w.definition->handle;
                if (w.definition->request.label == "Park") park = w.definition->handle;
            }
            const auto w = native_working_request(guard);
            need(w && std::holds_alternative<no::Limit>(w->definition->request.trigger)
                 && park.incarnation != 0, "missing original global limit or resting parent");
            auto request = w->definition->request;
            request.owner = no::WaitForApplied{park, no::NativeArmVisibility::Working,
                                               no::NativeArmFirstMatch::AtArmPrint,
                                               no::NativeArmScope::Book};
            no::ReplaceOptions keep;
            keep.keep_handle = true;
            const auto replaced = replace(guard, request, keep);
            need(replaced.status == no::ReplaceStatus::Replaced && replaced.successor == guard
                 && native_working_request(guard), "global limit did not keep its working handle");
        }
    }

    int check() const {
        need(last_error().empty() && last_run_status() == 0
             && bars_seen == (c.preopen_call ? 4 : 6) && stop_fills == 1,
             "incomplete run or missing stop fill");
        const bool keep = position_after_stop() * c.held > 0;
        bool ok = true;
        if (c.buy || c.preopen_call) ok = margin_result.compare(c.name, "margin", keep);
        if (!c.preopen_call) ok = global_result.compare(c.name, "global", keep) && ok;
        return ok ? 0 : 1;
    }

private:
    double level() const { return c.buy ? 103 : 97; }
    double signed_request() const { return c.buy ? c.requested : -c.requested; }
    double position_after_stop() const {
        return c.placed_opposite ? signed_request() : c.held + signed_request();
    }
    void place_stop() { strategy_entry("Stop", c.buy, na, level(), c.requested); }

    pf_pending_order_v1_t pending(const char* id) const {
        const auto& view = pending_intent_view();
        pf_pending_order_v1_t found{};
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0, "could not read pending order");
            if (std::string(row.id) != id) continue;
            need(found.incarnation == 0 && row.incarnation != 0, "ambiguous pending order");
            found = row;
        }
        return found;
    }

    void check_stop(bool opposite) {
        const auto row = pending("Stop");
        need(row.stop_price == level() && row.is_long == c.buy && row.incarnation != 0
             && row.created_position_side == static_cast<int>(
                    opposite ? PositionSide::SHORT : PositionSide::FLAT),
             "wrong side or price on the accepted stop");
        for (const auto& w : native_working_requests()) {
            if (w.definition->handle.incarnation != row.incarnation) continue;
            const auto& req = w.definition->request;
            need(req.label == "Stop" && std::holds_alternative<no::Stop>(req.trigger),
                 "expected the original priced entry");
            if (!opposite) {
                const auto* tx = std::get_if<no::Transact>(&req.intent);
                need(tx && tx->signed_units == signed_request(),
                     "flat stop was not a fixed transaction");
            }
            need(stop.incarnation == 0 || stop == w.definition->handle, "stop handle changed");
            stop = w.definition->handle;
        }
        need(stop.incarnation == row.incarnation && native_working_request(stop),
             "stop was not working");
        for (const auto& p : adapter_.fixture_pending_snapshots())
            if (!p.staged && p.incarnation == stop.incarnation) {
                need(p.snapshot.family == source::PineOrderFamily::Entry
                     && p.snapshot.reverse_to == opposite, "unexpected placement intent");
                return;
            }
        throw std::runtime_error("missing accepted entry snapshot");
    }

    struct Observation {
        bool working, projected;
        int cancels;
        bool compare(const char* name, const char* order, bool keep) const {
            const bool ok = working == keep && projected == keep && cancels == int(!keep);
            (ok ? std::cout : std::cerr) << (ok ? "PASS " : "FAIL ") << name << ": " << order
                << " working/projected/user-cancels expected " << keep << '/' << keep << '/' << !keep
                << ", got " << working << '/' << projected << '/' << cancels << '\n';
            return ok;
        }
    };

    Observation observe(no::RequestHandle h, const char* label, std::uint64_t after) const {
        need(h.incarnation != 0, "missing handle before cleanup");
        Observation result{native_working_request(h).has_value(), pending(label).incarnation == h.incarnation, 0};
        for (const auto& e : native_events(after)) {
            if (!e.command) continue;
            const auto* end = std::get_if<no::CancelledEvent>(&*e.command);
            if (end && end->handle() == h && end->reason == no::CancelReason::User) ++result.cancels;
        }
        need(result.working || result.cancels != 0, "order did not reach cleanup live");
        return result;
    }

    void on_native_applied_event(const no::ExecutionAppliedEvent& fill,
                                 const NativeDecisionContext& ctx) override {
        if (fill.request().label == "Held") {
            need(fill.interval_index() == 2 && fill.closed_units == 0 && fill.opened_units == c.held
                 && fill.resolved_price == (c.preopen_call ? 100 : 99.75)
                 && fill.cycle_before == 0 && fill.cycle_after > 0, "wrong original lot");
            held_fill = fill.ordinal;
            held_cycle = fill.cycle_after;
            if (c.preopen_call) {
                // At 50 the long needs 2000 against equity 5000 + 80*(50-100)
                // = 1000. Four times floor(1000/25) is capped at the 80 held.
                const auto row = pending("__margin_preopen__Held");
                need(row.created_position_side == static_cast<int>(PositionSide::FLAT)
                     && std::string(row.from_entry) == "Held", "call was not staged for the long");
                for (const auto& w : native_working_requests()) {
                    if (w.definition->handle.incarnation != row.incarnation) continue;
                    const auto& req = w.definition->request;
                    const auto* trigger = std::get_if<no::Stop>(&req.trigger);
                    need(w.definition->birth.acceptance_ordinal < held_fill && trigger
                         && trigger->price == 50 && std::holds_alternative<no::BindCohort>(req.owner)
                         && row.qty == 80, "wrong pre-open call or owner");
                    margin = w.definition->handle;
                }
                need(margin.incarnation != 0, "no working margin call after the long opened");
            }
            return;
        }
        if (fill.request().label != "Stop") return;
        ++stop_fills;
        const double after = position_after_stop();
        const bool add = c.held < 0 && !c.buy;
        const bool same_side = after * c.held > 0;
        need(stop == fill.handle() && held_fill != 0 && held_fill < fill.ordinal
             && fill.interval_index() == (c.preopen_call ? 2 : 4)
             && ctx.sub_index == (c.preopen_call ? 1 : 0)
             && fill.cursor.point.path_phase == NativePathPhase::Open
             && fill.raw_price == (c.buy ? 104 : 96)
             && fill.closed_units == (add ? 0 : same_side ? c.requested : 80)
             && fill.opened_units == (add ? signed_request() : same_side ? 0 : after)
             && physical_position().signed_units == after && fill.cycle_before == held_cycle
             && (same_side ? fill.cycle_after == held_cycle : fill.cycle_after > held_cycle),
             "stop did not make the expected settlement at the gap");
        if (c.buy) {
            // At 125 the old short needs 10000; its equity is
            // 10000 + 80*(99.75-125) = 7980. Buy 4*floor(2020/125) = 64.
            for (const auto& e : native_events(held_fill)) {
                if (!e.command || e.ordinal >= fill.ordinal) continue;
                const auto* a = std::get_if<no::AcceptedEvent>(&*e.command);
                if (!a || a->request().label != source::kMarginCallLabel) continue;
                const auto* trigger = std::get_if<no::Stop>(&a->request().trigger);
                const auto* reduce = std::get_if<no::Reduce>(&a->request().intent);
                const auto* qty = reduce ? std::get_if<no::ExplicitUnits>(&reduce->size) : nullptr;
                need(margin.incarnation == 0 && a->definition->origin == no::RequestOrigin::Host
                     && trigger && trigger->price == 125 && qty && qty->units == 64,
                     "wrong margin order on the short");
                margin = a->handle();
            }
            need(margin.incarnation != 0, "old short had no call before the gap fill");
        }
        if (!c.preopen_call) {
            need(guard.incarnation != 0 && native_working_request(park),
                 "missing original global limit or its independent parent");
            global_result = observe(guard, "Guard", fill.ordinal);
        }
        if (c.buy || c.preopen_call)
            margin_result = observe(margin, c.preopen_call ? "__margin_preopen__Held"
                                                          : source::kMarginCallLabel, fill.ordinal);
    }

    Case c;
    no::RequestHandle stop{}, margin{}, guard{}, park{};
    std::uint64_t held_fill = 0;
    std::int64_t held_cycle = 0;
    int bars_seen = 0, stop_fills = 0;
    Observation margin_result{}, global_result{};
};
} // namespace

int main() {
    const Case cases[] = {
        {"buy stop requested flat", true, 82, -80},
        {"sell stop requested flat", false, 82, 80, false, true},
        {"buy stop requested short", true, 2, -80, true},
        {"sell stop adds to short", false, 2, -80},
        {"buy stop only reduces short", true, 78, -80},
    };
    int status = 0;
    for (const auto& c : cases) {
        try {
            StopHost host(c);
            if (c.preopen_call) {
                std::array<Bar, 8> bars{};
                for (int i = 0; i < 8; ++i)
                    bars[i] = {100, 100, 100, 100, 1000, start + i * minute};
                bars[5] = {96, 98, 50, 96, 1000, bars[5].timestamp};
                host.run(bars.data(), 8, "1", "2", true, 4, MagnifierDistribution::ENDPOINTS);
            } else {
                std::array<Bar, 6> bars{};
                for (int i = 0; i < 6; ++i)
                    bars[i] = {100, 100, 100, 100, 1000, start + i * minute};
                bars[4] = c.buy ? Bar{104, 125, 102, 104, 1000, bars[4].timestamp}
                                : Bar{96, 98, 94, 96, 1000, bars[4].timestamp};
                host.run(bars.data(), 6);
            }
            if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
            if (host.check() && status == 0) status = 1;
        } catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << c.name << ": " << e.what() << '\n';
            status = 2;
        }
    }
    return status;
}
