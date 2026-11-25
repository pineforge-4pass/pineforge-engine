#include <pineforge/source/pine_strategy_host.hpp>

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

enum class Route { Attached, Held, Distinct, Fifo, Reissue, Replaced, ReplacedTwice, ReplacedDistinct };
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
        config.close_entries_rule_any = route != Route::Fifo;
        config.pyramiding = cap;
        config.margin_long = config.margin_short = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        need(i == seen++, "missing or repeated chart bar");
        positions.at(i) = live_position_size();
        books.at(i) = book();
        if (i == 0 && replaces_entry()) {
            strategy_entry("Entry", side < 0, 100 + side * 6, na, 5);
            strategy_exit("Guard", "Entry", 100 - side * 10, 100 + side * 10);
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
        if (i == flip - 1) strategy_entry(next_id, side > 0, na, na, 3);
        if (i == flip && route == Route::Reissue)
            strategy_exit("Guard", next_id, 100 + side * 14, 100 - side * 20);
        if (i == close_bar - 1) strategy_close(next_id);
    }

    void compare(Checks& c) const {
        need(last_run_status() == 0 && last_error().empty() && seen == bars, "incomplete run");
        const auto& before = books[flip - 1];
        need(positions[flip - 1] == -side * 5 && before.limits == 1 && before.stops == 1
             && before.working == 2 && before.projected == 1
             && before.from == "Entry"
             && before.limit == 100 - side * 10 && before.stop == 100 + side * 10,
             "the original five units and their two resting bracket legs are required");
        need(fills.size() >= 2 && fills[0].id == "Entry" && fills[1].id == next_id
             && fills[1].closed > 0 && fills[1].opened * side > 0 && saw_reversal,
             "no opening followed by an applied reversal of the original lot");
        need(!replaces_entry() || replacement_filled, "the market fill did not replace the resting entry");
        const bool reissue = route == Route::Reissue;
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
        fill(0, "Entry", 2, 99, 0, -side * 5, true);
        fill(1, next_id, flip, 103, 5, side * 3, true);
        c.equal("working Guard legs at reversal", at_reversal.working, 0);
        c.equal("projected Guard orders at reversal", at_reversal.projected, 0);
        c.equal("applied fills", fills.size(), 3U);
        if (fills.size() >= 3) fill(2, end_id, end, end_price, 3, 0, !reissue);
        for (int i = 0; i < bars; ++i) {
            const std::string key = "bar " + std::to_string(i) + " ";
            const double units = i < 2 || i >= end ? 0 : i < flip ? -side * 5 : side * 3;
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
        c.equal("closed trades", closed_trade_count(), 2U);
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
        trade(0, "Entry", next_id, side < 0, 5, 2, flip, 99, 103, false);
        trade(1, next_id, end_id, side > 0, 3, flip, end, 103, end_price, reissue);
    }

private:
    bool replaces_entry() const {
        return route == Route::Replaced || route == Route::ReplacedTwice || route == Route::ReplacedDistinct;
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
        if (fills.size() == 1 && replaces_entry()) {
            replacement_filled = e.definition->root == resting_entry && e.definition->predecessor
                && (*e.definition->predecessor != resting_entry) == (route == Route::ReplacedTwice)
                && e.handle() != resting_entry && std::holds_alternative<no::Market>(e.request().trigger);
        }
        if (e.request().label == next_id && e.closed_units > 0 && e.opened_units * side > 0) {
            at_reversal = book();
            saw_reversal = true;
        }
    }
    const Route route;
    const int side;
    const std::string next_id;
    int seen = 0;
    bool saw_reversal = false;
    bool replacement_filled = false;
    no::RequestHandle resting_entry{};
    Book at_reversal;
    std::array<double, bars> positions{};
    std::array<Book, bars> books{};
    std::vector<Fill> fills;
};
} // namespace

int main() {
    const auto tape = prices();
    int status = 0;
    struct Case { const char* name; Route route; int cap; };
    for (const auto c : {Case{"ANY replaced opener", Route::Replaced, 1},
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
