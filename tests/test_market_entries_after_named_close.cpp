#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double omitted = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1704067200000LL;
constexpr std::int64_t quarter_hour = 900000;
constexpr int bar_count = 8;

struct Case {
    const char* name;
    bool seed_long;
    const char* first;
    const char* second;
    double seed_qty;
    double default_qty;
    double second_qty;
    bool two_slots;
    bool cancel_first;
};

struct Fill {
    std::string id;
    std::string comment;
    std::int64_t time;
    double opened;
    double closed;
};

class Book final : public source::PineStrategyHost {
public:
    explicit Book(const Case& c) : c(c) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.pyramiding = c.two_slots ? 2 : 1;
        config.default_qty_value = c.default_qty;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.001);
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar& bar) override {
        ++callbacks;
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("base", c.seed_long, omitted, omitted, c.seed_qty);
        if (i == 2) {
            seed = native_open_lots(bar.close);
            strategy_close("base", "c1", omitted, omitted, false, 1);
            strategy_entry(c.first, !c.seed_long);
            // A single close followed by two entries with room for both;
            // a rule that simply deletes other IDs behind a close is wrong.
            if (!c.two_slots)
                strategy_close("base", "c2", omitted, omitted, false, 2);
            if (c.cancel_first) strategy_cancel(c.first);
            strategy_entry(c.second, !c.seed_long, omitted, omitted, c.second_qty);
        }
        if (i == 3) {
            after = native_open_lots(bar.close);
            pending_after = pending_order_count();
            if (c.two_slots) strategy_close_all();
            else {
                strategy_close(c.first, "", omitted, omitted, false, 3);
                strategy_close(c.second, "", omitted, omitted, false, 4);
            }
        }
        if (!c.two_slots && i == 4) strategy_close_all();
    }

    const Case& c;
    int callbacks = 0;
    int pending_after = -1;
    std::vector<NativeOpenLot> seed, after;
};

template<class Actual, class Expected>
bool check_value(const std::string& where, const Actual& actual, const Expected& expected) {
    if (actual == expected) return true;
    std::cerr << "FAIL " << where << ": expected " << expected << ", got " << actual << '\n';
    return false;
}

int run(const Case& c) {
    std::array<Bar, bar_count> bars{};
    for (int i = 0; i < bar_count; ++i) {
        const double p = 100.0 + i;
        bars[i] = {p, p + 2, p - 1, p + 1, 1000, start + i * quarter_hour};
    }
    Book book(c);
    book.run(bars.data(), bar_count, "15", "15", false);
    const double sign = c.seed_long ? 1.0 : -1.0;
    const double seed_qty = std::isnan(c.seed_qty) ? c.default_qty : c.seed_qty;
    if (!book.last_error().empty() || book.callbacks != bar_count || book.seed.size() != 1 ||
        book.seed.front().entry_label != "base" || book.seed.front().signed_units != sign * seed_qty) {
        std::cerr << "NOT COMPARED " << c.name << ": expected " << bar_count
                  << " callbacks and base lot " << sign * seed_qty << "; got "
                  << book.callbacks << " callbacks, " << book.seed.size()
                  << " seed lots";
        for (const auto& lot : book.seed)
            std::cerr << " [" << lot.entry_label << ": " << lot.signed_units << ']';
        std::cerr << ", run error '" << book.last_error() << "'\n";
        return 2;
    }

    // Check quantities and ownership at the fill, before the cleanup close can
    // hide an extra entry. The fill stream distinguishes c1 from c2 even if
    // they have the same label and the two trades end at the same net size.
    const std::string label = std::string(c.name) + ": ";
    const auto first_time = bars[3].timestamp;
    const auto exit_time = first_time + quarter_hour;
    const double second_qty = std::isnan(c.second_qty) ? c.default_qty : c.second_qty;
    const char* survivor = c.cancel_first ? c.second : c.first;
    const double survivor_qty = c.cancel_first ? second_qty : c.default_qty;
    const std::string close_id = c.two_slots ? "__close__" : "__close__" + std::string(survivor);
    std::vector<Fill> fills;
    int second_fills = 0;
    for (const auto& event : book.native_events(0)) {
        if (!event.command) continue;
        const auto* e = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!e) continue;
        fills.push_back({e->request().label, e->request().comment, e->interval_open_ms(),
                         e->opened_units, e->closed_units});
        if (e->request().label == c.second) ++second_fills;
    }
    bool ok = check_value(label + c.second + " entry fills", second_fills,
                          c.two_slots || c.cancel_first ? 1 : 0);
    ok &= check_value(label + "working orders after fills", book.pending_after, 0);
    ok &= check_value(label + "open lots after fills", book.after.size(), c.two_slots ? 2U : 1U);
    if (!book.after.empty()) {
        ok &= check_value(label + "first lot ID", book.after[0].entry_label, survivor);
        ok &= check_value(label + "first lot units", book.after[0].signed_units, -sign * survivor_qty);
    }
    if (c.two_slots && book.after.size() >= 2) {
        ok &= check_value(label + "second lot ID", book.after[1].entry_label, c.second);
        ok &= check_value(label + "second lot units", book.after[1].signed_units, -sign * second_qty);
    }

    std::vector<Fill> wanted{
        {"base", "", bars[1].timestamp, sign * seed_qty, 0},
        {"__close__base", "c1", bars[3].timestamp, 0, seed_qty},
        {survivor, "", first_time, -sign * survivor_qty, 0},
    };
    if (c.two_slots)
        wanted.push_back({c.second, "", first_time, -sign * second_qty, 0});
    wanted.push_back({close_id, "", exit_time, 0, survivor_qty + (c.two_slots ? second_qty : 0)});
    ok &= check_value(label + "applied fills", fills.size(), wanted.size());
    for (std::size_t i = 0; i < fills.size(); ++i) {
        const auto& f = fills[i];
        std::cout << label << "fill " << i << ' ' << f.time << ' ' << f.id << " ["
                  << f.comment << "] opened=" << f.opened << " closed=" << f.closed << '\n';
        if (i >= wanted.size()) continue;
        const auto& w = wanted[i];
        const auto prefix = label + "fill " + std::to_string(i) + " ";
        ok &= check_value(prefix + "ID", f.id, w.id);
        if (w.id == "__close__base") ok &= check_value(prefix + "comment", f.comment, w.comment);
        ok &= check_value(prefix + "epoch", f.time, w.time);
        ok &= check_value(prefix + "opened", f.opened, w.opened);
        ok &= check_value(prefix + "closed", f.closed, w.closed);
    }
    ok &= check_value(label + "closed trades", book.closed_trade_count(), c.two_slots ? 3U : 2U);
    for (int i = 0; i < book.trade_count(); ++i) {
        const auto t = book.get_trade(i);
        const auto prefix = label + "trade " + std::to_string(i) + " ";
        const bool base = i == 0;
        ok &= check_value(prefix + "entry ID", t.entry_id, base ? "base" : i == 1 ? survivor : c.second);
        ok &= check_value(prefix + "exit ID", t.exit_id, base ? "__close__base" : close_id);
        ok &= check_value(prefix + "side", t.is_long, base ? c.seed_long : !c.seed_long);
        ok &= check_value(prefix + "quantity", t.qty, base ? seed_qty : i == 1 ? survivor_qty : second_qty);
        ok &= check_value(prefix + "entry epoch", t.entry_time, base ? bars[1].timestamp : first_time);
        ok &= check_value(prefix + "exit epoch", t.exit_time, base ? bars[3].timestamp : exit_time);
    }
    ok &= check_value(label + "final position", book.physical_position().signed_units, 0.0);
    ok &= check_value(label + "final working requests", book.native_working_requests().size(), 0U);
    return ok ? 0 : 1;
}
} // namespace

int main() {
    // The first case uses default quantities for all three entries. Swapping
    // IDs and direction separates first-in-line from a preference for a label;
    // a larger base must not be added to the first entry after it has closed.
    const Case cases[] = {
        {"one slot", true, "x", "y", omitted, 1, omitted, false, false},
        {"one slot, mirrored", false, "y", "x", 5, 2.5, 4, false, false},
        {"released slot", true, "x", "y", 3, 2, 4.5, false, true},
        {"two free slots", true, "x", "y", 3, 2, 4.5, true, false},
    };
    int status = 0;
    for (const auto& c : cases) {
        try {
            const int result = run(c);
            if (result > status) status = result;
        } catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << c.name << ": " << e.what() << '\n';
            status = 2;
        }
    }
    return status;
}
