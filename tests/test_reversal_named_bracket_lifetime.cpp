#include <pineforge/source/pine_strategy_host.hpp>

#include "waiting_exit_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = native_order;
constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr int flip = 6, close_bar = flip + 6, bars = close_bar + 2;
constexpr std::int64_t start = 1704067200000LL, period = 15 * 60000;

std::array<Bar, bars> prices() {
    std::array<Bar, bars> tape{};
    for (int i = 0; i < bars; ++i)
        tape[i] = {100, 101, 99, 100, 1000, start + i * period};
    tape[2] = {99, 102, 98, 101, 1000, tape[2].timestamp};
    tape[flip] = {103, 105, 97, 100, 1000, tape[flip].timestamp};
    for (int i = flip + 1; i < close_bar; ++i)
        tape[i] = {100, 112, 88, 100, 1000, tape[i].timestamp};
    tape[8] = {100, 115, 85, 100, 1000, tape[8].timestamp};
    tape[close_bar] = {87, 101, 86, 99, 1000, tape[close_bar].timestamp};
    return tape;
}

struct Checks {
    std::string name;
    bool ok = true;
    template<class A, class B>
    void equal(const std::string& field, const A& actual, const B& expected) {
        if (actual == expected) return;
        std::cerr << "FAIL " << name << ": " << field << " expected " << expected
                  << ", got " << actual << '\n';
        ok = false;
    }
};

void need(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

enum class Route { Attached, Held, Distinct, Fifo, Reissue, Replaced, ReplacedTwice, ReplacedDistinct,
                   ReplacedZero, ReplacedReissue, TwoLots, EndedStop, ReplacedFifo };
struct Book {
    int projected = 0, limits = 0, stops = 0, working = 0;
    std::string from;
    double limit = na, stop = na;
};
struct Fill {
    std::string id;
    int bar;
    double price, closed, opened;
    bool at_open;
};

class Host final : public source::PineStrategyHost {
public:
    Host(Route route, int side, int cap)
        : route(route), side(side),
          next_id(route == Route::Distinct || route == Route::ReplacedDistinct ? "Other" : "Entry") {
        source::PineStrategyConfig config;
        config.close_entries_rule_any = route != Route::Fifo && route != Route::ReplacedFifo;
        config.pyramiding = cap;
        config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        need(i == seen++, "missing or repeated chart bar");
        positions.at(i) = live_position_size();
        books.at(i) = book();
        if (i == 0 && replaces_entry()) {
            strategy_entry("Entry", side < 0, 100 + side * 6, na, 5);
            strategy_exit("Guard", "Entry", old_limit(), old_stop());
            if (route == Route::TwoLots) strategy_entry("First", side < 0, na, na, 2);
        }
        if (i == 1) {
            if (replaces_entry()) {
                for (const auto& w : native_working_requests())
                    if (w.definition->request.label == "Entry" &&
                        std::holds_alternative<no::Limit>(w.definition->request.trigger))
                        resting_entry = w.definition->handle;
                need(resting_entry.incarnation && books[i].limits == 1 && books[i].stops == 1,
                     "missing unfilled parent and its bracket before replacement");
            }
            if (route == Route::ReplacedTwice)
                strategy_entry("Entry", side < 0, 100 + side * 7, na, 5);
            strategy_entry("Entry", side < 0, na, na, 5);
        }
        if (!replaces_entry() && i == (route == Route::Held ? 3 : 1))
            strategy_exit("Guard", "Entry", 100 - side * 10, 100 + side * 10);
        if (i == flip - 1) {
            if (replaces_entry()) identify_legs();
            if (route == Route::TwoLots) {
                const auto lots = native_open_lots(100);
                need(lots.size() == 2 && lots[0].entry_label == "First" && lots[0].signed_units == -side * 2
                     && lots[1].entry_incarnation == filled_opener.incarnation
                     && lots[1].signed_units == -side * 5, "missing both lots before reversal");
            }
            if (route == Route::EndedStop) {
                // Cancelling the limit will end the stop inside the purge.
                auto request = native_working_request(old_stop_handle)->definition->request;
                request.owner = no::WaitForApplied{old_limit_handle, no::NativeArmVisibility::Working,
                    no::NativeArmFirstMatch::AtArmPrint, no::NativeArmScope::Book};
                no::ReplaceOptions keep;
                keep.keep_handle = true;
                const auto result = replace(old_stop_handle, request, keep);
                need(result.status == no::ReplaceStatus::Replaced && result.successor == old_stop_handle,
                     "could not make the original stop wait for its limit");
            }
            strategy_entry(next_id, side > 0, na, na, 3);
        }
        if (i == flip && reissues())
            strategy_exit("Guard", next_id, 100 + side * 14, 100 - side * 20);
        if (i == close_bar - 1) strategy_close(next_id);
    }

    void compare(Checks& c) const {
        need(last_run_status() == 0 && last_error().empty() && seen == bars, "incomplete run");
        const auto& before = books[flip - 1];
        const bool two = route == Route::TwoLots;
        const int opener = two ? 1 : 0;
        const int held = two ? 7 : 5;
        need(positions[flip - 1] == -side * held && before.limits == 1 && before.stops == 1
             && before.working == 2 && before.projected == 1
             && before.from == "Entry"
             && before.limit == old_limit() && before.stop == old_stop(),
             "the original lots and their two resting bracket legs are required");
        need(fills.size() >= std::size_t(opener + 2) && fills[opener].id == "Entry" && fills[opener + 1].id == next_id
             && fills[opener + 1].closed > 0 && fills[opener + 1].opened * side > 0 && saw_reversal,
             "no opening followed by an applied reversal of the original lot");
        need(!replaces_entry() || replacement_filled, "the market fill did not replace the resting entry");
        const bool reissue = reissues();
        const int end = reissue ? 8 : close_bar;
        const double end_price = reissue ? 100 + side * 14 : 87;
        const std::string end_id = reissue ? "Guard" : "__close__" + next_id;
        const auto fill = [&](int index, const std::string& id, int bar, double price,
                              double closed, double opened, bool at_open) {
            const auto& f = fills.at(index);
            const auto key = "fill " + std::to_string(index + 1) + " ";
            c.equal(key + "id", f.id, id);
            c.equal(key + "bar", f.bar, bar);
            c.equal(key + "price", f.price, price);
            c.equal(key + "closed units", f.closed, closed);
            c.equal(key + "signed opened units", f.opened, opened);
            c.equal(key + "at open", f.at_open, at_open);
        };
        if (two) fill(0, "First", 1, 100, 0, -side * 2, true);
        fill(opener, "Entry", 2, 99, 0, -side * 5, true);
        fill(opener + 1, next_id, flip, 103, held, side * 3, true);
        c.equal("working Guard legs at reversal", at_reversal.working, 0);
        c.equal("projected Guard orders at reversal", at_reversal.projected, 0);
        c.equal("applied fills", fills.size(), std::size_t(opener + 3));
        if (fills.size() >= std::size_t(opener + 3))
            fill(opener + 2, end_id, end, end_price, 3, 0, !reissue);
        if (replaces_entry() && route != Route::ReplacedDistinct) check_cancels(c);
        for (int i = 0; i < bars; ++i) {
            const std::string key = "bar " + std::to_string(i) + " ";
            const double units = i < 2 ? (two && i == 1 ? -side * 2 : 0)
                : i >= end ? 0 : i < flip ? -side * held : side * 3;
            c.equal(key + "signed position", positions[i], units);
            if (i < flip) continue;
            const bool fresh = reissue && i == flip + 1;
            c.equal(key + "working Guard legs", books[i].working, fresh ? 2 : 0);
            c.equal(key + "projected Guard orders", books[i].projected, fresh ? 1 : 0);
            if (fresh) {
                c.equal(key + "new owner", books[i].from, next_id);
                c.equal(key + "new limit", books[i].limit, 100 + side * 14);
                c.equal(key + "new stop", books[i].stop, 100 - side * 20);
            }
        }
        c.equal("closed trades", closed_trade_count(), two ? 3U : 2U);
        const auto trade = [&](int index, const std::string& entry, const std::string& exit,
                               bool is_long, double qty, int from, int to, double entry_price,
                               double exit_price, bool bracket) {
            if (static_cast<std::size_t>(index) >= closed_trade_count()) return;
            const Trade t = get_trade(index);
            const auto key = "trade " + std::to_string(index + 1) + " ";
            c.equal(key + "entry", t.entry_id, entry);
            c.equal(key + "exit", t.exit_id, exit);
            c.equal(key + "long", t.is_long, is_long);
            c.equal(key + "quantity", t.qty, qty);
            c.equal(key + "entry time", t.entry_time, start + from * period);
            c.equal(key + "exit time", t.exit_time, start + to * period);
            c.equal(key + "entry price", t.entry_price, entry_price);
            c.equal(key + "exit price", t.exit_price, exit_price);
            c.equal(key + "bracket exit", t.exit_from_bracket, bracket);
        };
        if (two) trade(0, "First", next_id, side < 0, 2, 1, flip, 100, 103, false);
        trade(opener, "Entry", next_id, side < 0, 5, 2, flip, 99, 103, false);
        trade(opener + 1, next_id, end_id, side > 0, 3, flip, end, 103, end_price, reissue);
    }

private:
    // On positive prints a sell stop at zero protects the long without
    // triggering; a buy limit at zero likewise rests on the short.
    double old_stop() const { return route == Route::ReplacedZero && side < 0 ? 0 : 100 + side * 10; }
    double old_limit() const { return route == Route::ReplacedZero && side > 0 ? 0 : 100 - side * 10; }
    bool replaces_entry() const {
        return route == Route::Replaced || route == Route::ReplacedTwice || route == Route::ReplacedDistinct
            || route == Route::ReplacedZero || route == Route::ReplacedReissue || route == Route::TwoLots
            || route == Route::EndedStop || route == Route::ReplacedFifo;
    }
    bool reissues() const { return route == Route::Reissue || route == Route::ReplacedReissue; }
    void identify_legs() {
        for (const auto& w : native_working_requests()) {
            const auto& r = w.definition->request;
            if (r.label != "Guard") continue;
            if (std::holds_alternative<no::Stop>(r.trigger)) old_stop_handle = w.definition->handle;
            if (std::holds_alternative<no::Limit>(r.trigger)) old_limit_handle = w.definition->handle;
        }
        need(old_stop_handle.incarnation && old_limit_handle.incarnation && old_stop_handle != old_limit_handle,
             "missing original stop or limit handle");
        int bound = 0;
        for (const auto& row : adapter_.fixture_pending_snapshots()) {
            if (row.incarnation == old_limit_handle.incarnation || row.incarnation == old_stop_handle.incarnation)
                bound += !row.staged && row.snapshot.bracket_origin == resting_entry
                    && !row.snapshot.pending_bound_exit && !row.snapshot.void_issue;
        }
        need(bound == 2, "both legs must still name the replaced entry order");
    }
    void check_cancels(Checks& c) const {
        int limit = 0, stop = 0, misses = 0;
        std::uint64_t limit_end = 0;
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* e = std::get_if<no::CancelledEvent>(&*row.command)) {
                if (e->handle() == old_limit_handle) {
                    ++limit;
                    c.equal("limit cancelled at reversal", e->ordinal > reversal_ordinal
                            && e->reason == no::CancelReason::User, true);
                    limit_end = e->ordinal;
                }
                if (e->handle() == old_stop_handle) {
                    ++stop;
                    const auto* wait = std::get_if<no::Wait>(&e->prior_authority);
                    c.equal("stop ended at reversal", e->ordinal > reversal_ordinal && (route == Route::EndedStop
                            ? wait && wait->parent == old_limit_handle && e->reason == no::CancelReason::OwnerGone
                                && limit_end && e->cause && e->cause->ordinal == limit_end
                            : e->reason == no::CancelReason::User), true);
                }
            } else if (const auto* e = std::get_if<no::NotWorkingEvent>(&*row.command)) {
                misses += e->target == old_stop_handle || e->target == old_limit_handle;
            }
        }
        c.equal("old limit cancellation count", limit, 1);
        c.equal("old stop cancellation count", stop, 1);
        c.equal("cancels of ended legs", misses, 0);
    }
    Book book() const {
        Book result;
        for (const auto& w : native_working_requests()) {
            const auto& r = w.definition->request;
            if (r.label != "Guard") continue;
            ++result.working;
            result.limits += std::holds_alternative<no::Limit>(r.trigger);
            result.stops += std::holds_alternative<no::Stop>(r.trigger);
        }
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            need(view.copy_v1(i, &row) == 0, "unreadable pending order");
            if (std::string(row.id) != "Guard") continue;
            ++result.projected;
            result.from = row.from_entry;
            result.limit = row.limit_price;
            result.stop = row.stop_price;
        }
        return result;
    }
    void on_native_applied_event(const no::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        fills.push_back({e.request().label, e.interval_index(), e.resolved_price,
                         e.closed_units, e.opened_units, e.cursor.point.path_phase == NativePathPhase::Open});
        if (replaces_entry() && e.request().label == "Entry" && e.closed_units == 0) {
            filled_opener = e.handle();
            replacement_filled = e.definition->root == resting_entry && e.definition->predecessor
                && (*e.definition->predecessor != resting_entry) == (route == Route::ReplacedTwice)
                && e.handle() != resting_entry && std::holds_alternative<no::Market>(e.request().trigger);
        }
        if (e.request().label == next_id && e.closed_units > 0 && e.opened_units * side > 0) {
            at_reversal = book();
            reversal_ordinal = e.ordinal;
            saw_reversal = true;
        }
    }
    const Route route;
    const int side;
    const std::string next_id;
    int seen = 0;
    bool saw_reversal = false;
    bool replacement_filled = false;
    no::RequestHandle resting_entry{}, filled_opener{}, old_stop_handle{}, old_limit_handle{};
    std::uint64_t reversal_ordinal = 0;
    Book at_reversal;
    std::array<double, bars> positions{};
    std::array<Book, bars> books{};
    std::vector<Fill> fills;
};

// A host keeps a priced successor working outside the source replacement
// book. Its two legs wait for it while another chain of the same ID fills.
class OtherChain final : public waiting_exit::Host {
public:
    explicit OtherChain(int side) : waiting_exit::Host(true), side(side) {}
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        need(i == callbacks++, "missing chart bar");
        if (i == 0) {
            strategy_entry("Entry", side > 0, 100 - side * 35, na, 3);
            guard_root = working("Entry");
            strategy_exit("Guard", "Entry", 100 + side * 25, 100 - side * 25);
            limit = working("Guard");
            stop = working<no::Stop>("Guard");
        }
        if (i == 1) {
            const auto original = native_working_request(guard_root);
            need(original.has_value(), "missing original priced entry");
            const auto result = replace(guard_root, original->definition->request);
            need(result.status == no::ReplaceStatus::Replaced && result.successor
                 && *result.successor != guard_root, "missing independent priced successor");
            guarded = *result.successor;
            wait_for(limit, guarded, true);
            wait_for(stop, guarded, true);
            strategy_entry("Entry", side < 0, 100 + side * 30, na, 5);
            for (const auto& w : native_working_requests())
                if (w.definition->request.label == "Entry" && w.definition->handle != guarded
                    && !w.definition->predecessor) other_root = w.definition->handle;
            need(other_root.incarnation && other_root != guard_root, "missing separate same-ID root");
        }
        if (i == 2) {
            strategy_entry("Entry", side < 0, na, na, 5);
            other_fill = working<no::Market>("Entry");
        }
        if (i == flip - 1) {
            const auto lots = native_open_lots(100);
            need(lots.size() == 1 && lots[0].entry_incarnation == other_fill.incarnation
                 && lots[0].entry_label == "Entry" && lots[0].signed_units == -side * 5
                 && guard_root.incarnation < other_fill.incarnation && working("Entry") == guarded,
                 "expected the other chain's lot beside Guard's resting parent");
            int bound = 0;
            for (const auto& row : adapter_.fixture_pending_snapshots()) {
                const auto& s = row.snapshot;
                if (row.incarnation != limit.incarnation && row.incarnation != stop.incarnation) continue;
                bound += !row.staged && !s.void_issue && !s.pending_bound_exit && s.bracket_origin == guard_root
                    && s.from_entry == "Entry" && s.exit_levels.limit == 100 + side * 25
                    && s.exit_levels.stop == 100 - side * 25;
            }
            need(bound == 2, "expected both resting legs bound to the earlier incarnation");
            strategy_entry("Entry", side > 0, na, na, 3);
        }
    }
    void compare(Checks& c) const {
        need(last_run_status() == 0 && last_error().empty() && callbacks == flip + 2
             && seeded && reversed, "missing identified fills from the two independent chains");
        c.equal("original working legs at reversal", kept, 2);
        c.equal("projected Guard orders at reversal", projected_at_flip, 1);
        c.equal("original working legs on following bar",
                int(bool(native_working_request(limit))) + int(bool(native_working_request(stop))), 2);
        c.equal("following position", physical_position().signed_units, side * 3);
        c.equal("closed trades", closed_trade_count(), 1U);
    }
private:
    void on_native_applied_event(const no::ExecutionAppliedEvent& e, const NativeDecisionContext&) override {
        if (e.handle() == other_fill) {
            seeded = e.definition->root == other_root && e.request().label == "Entry" && e.interval_index() == 3
                && e.resolved_price == 100 && e.opened_units == -side * 5 && e.closed_units == 0;
        }
        if (e.opened_units != side * 3 || e.closed_units != 5 || e.closed_trade_count != 1) return;
        const auto lot = get_trade(static_cast<int>(e.first_trade_index));
        reversed = e.interval_index() == flip && e.resolved_price == 103 && e.request().label == "Entry"
            && std::holds_alternative<no::Market>(e.request().trigger) && !e.definition->predecessor
            && native_working_request(guarded) && native_working_request(guarded)->definition->root == guard_root
            && other_root != guard_root && lot.entry_id == "Entry" && lot.entry_incarnation == other_fill.incarnation;
        kept = 0;
        for (const auto& h : {limit, stop}) {
            const auto w = native_working_request(h);
            const auto* wait = w ? std::get_if<no::WaitForApplied>(&w->definition->request.owner) : nullptr;
            kept += wait && wait->parent == guarded;
        }
        projected_at_flip = projected("Guard");
    }
    int side, callbacks = 0, kept = -1, projected_at_flip = -1;
    bool seeded = false, reversed = false;
    no::RequestHandle guard_root{}, guarded{}, other_root{}, other_fill{}, limit{}, stop{};
};
} // namespace

int main() {
    const auto tape = prices();
    int status = 0;
    for (int side : {-1, 1}) {
        Checks c{std::string("ANY other replacement chain") + (side < 0 ? " into short" : " into long")};
        try {
            OtherChain host(side);
            host.run(tape.data(), flip + 2, "15", "15", false);
            if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
            host.compare(c);
            if (c.ok) std::cout << "PASS " << c.name << '\n';
            else status = std::max(status, 1);
        } catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << c.name << ": " << e.what() << '\n';
            status = 2;
        }
    }
    struct Case { const char* name; Route route; int cap; };
    for (const auto c : {Case{"ANY replaced opener", Route::Replaced, 1},
                         Case{"ANY replaced opener zero level", Route::ReplacedZero, 1},
                         Case{"ANY replaced opener fresh exit", Route::ReplacedReissue, 1},
                         Case{"ANY replaced opener second lot", Route::TwoLots, 2},
                         Case{"ANY replaced opener dependent stop", Route::EndedStop, 1},
                         Case{"FIFO replaced opener", Route::ReplacedFifo, 1},
                         Case{"ANY replaced opener pyramid 2", Route::Replaced, 2},
                         Case{"ANY twice replaced opener", Route::ReplacedTwice, 1},
                         Case{"ANY replaced opener distinct entry", Route::ReplacedDistinct, 1},
                         Case{"ANY attached", Route::Attached, 1},
                         Case{"ANY attached pyramid 2", Route::Attached, 2},
                         Case{"ANY held", Route::Held, 1},
                         Case{"ANY distinct entry", Route::Distinct, 1},
                         Case{"FIFO attached", Route::Fifo, 1},
                         Case{"ANY reissued", Route::Reissue, 1}}) {
        for (int side : {-1, 1}) {
            Checks checks{std::string(c.name) + (side < 0 ? " into short" : " into long")};
            try {
                Host host(c.route, side, c.cap);
                host.run(tape.data(), bars, "15", "15", false);
                if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
                host.compare(checks);
                if (checks.ok) std::cout << "PASS " << checks.name << '\n';
                else status = std::max(status, 1);
            } catch (const std::exception& e) {
                std::cerr << "NOT COMPARED " << checks.name << ": " << e.what() << '\n';
                status = 2;
            }
        }
    }
    return status;
}
