#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
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
constexpr std::int64_t start = 1'736'683'200'000LL, hour = 3'600'000;
constexpr int charts = 7, per_hour = 6;

enum class Calls { Once, Between, Before, Sum, TwoIds, Reopen, Reopened };
struct Row {
    const char* name;
    Calls calls;
    bool magnified, held_long;
    const char* held() const { return held_long ? "L" : "S"; }
    const char* next() const {
        if (calls == Calls::Reopen || calls == Calls::Reopened) return held();
        return held_long ? "S" : "L";
    }
    bool split() const { return calls == Calls::Sum; }
    bool check_drop() const {
        return magnified && (calls == Calls::Between || calls == Calls::Before
                             || calls == Calls::Sum);
    }
};

std::vector<Bar> feed(bool finer) {
    std::vector<Bar> bars;
    for (int i = 0; i < charts; ++i) {
        const double open = 100 + i;
        const auto time = start + i * hour;
        if (finer) {
            for (int j = 0; j < per_hour; ++j) {
                const double p = open + 0.25 * j;
                bars.push_back({p, p + 0.25, p - 0.25, p + 0.25, 1, time + j * (hour / per_hour)});
            }
        } else {
            bars.push_back({open, open + 1.5, open - 0.25, open + 1.5, per_hour, time});
        }
    }
    return bars;
}

class Strategy final : public source::PineStrategyHost {
public:
    explicit Strategy(Row row) : row(row) {
        source::PineStrategyConfig c; // fixed one, FIFO, no fees, next open, no fill recalculation
        c.pyramiding = row.calls == Calls::TwoIds ? 2 : 1;
        configure_pine_strategy(c);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
        fixture_retain_all_events();
    }

    std::vector<std::int64_t> callbacks;
    double seed = omitted;
    bool saw_first_close = false;
    int before_rows = -1, after_rows = -1;
    std::uint64_t events_during_close = 0, finish_ordinal = 0;

private:
    Row row;

    void second_close() {
        const auto events = native_events(0);
        for (const auto& e : events) {
            if (!e.command) continue;
            const auto* a = std::get_if<native_order::AcceptedEvent>(&*e.command);
            if (a && a->request().label == std::string("__close__") + row.held()
                && a->request().comment == "first") saw_first_close = true;
        }
        const auto cursor = events.empty() ? 0 : events.back().ordinal;
        before_rows = static_cast<int>(adapter_.fixture_pending_snapshots().size());
        strategy_close(row.held(), "second", omitted, omitted, false, 2);
        after_rows = static_cast<int>(adapter_.fixture_pending_snapshots().size());
        events_during_close = native_events(cursor).size();
    }

    void on_source_bar(const Bar& b) override {
        callbacks.push_back(b.timestamp);
        switch (pine_bar_index()) {
        case 0:
            strategy_entry(row.held(), row.held_long, omitted, omitted,
                           row.split() || row.calls == Calls::TwoIds ? 2 : omitted);
            break;
        case 1:
            if (row.calls == Calls::TwoIds)
                strategy_entry("B", row.held_long);
            break;
        case 2:
            seed = live_position_size();
            strategy_close(row.held(), "first", row.split() ? 1 : omitted, omitted, false, 1);
            if (row.calls == Calls::TwoIds) {
                strategy_close("B", "second", omitted, omitted, false, 2);
                break;
            }
            if (row.calls == Calls::Sum)
                strategy_close(row.held(), "other unit", 1, omitted, false, 4);
            if (row.calls == Calls::Before || row.calls == Calls::Sum) second_close();
            strategy_entry(row.next(), !row.held_long);
            if (row.calls == Calls::Between || row.calls == Calls::Reopen)
                second_close();
            break;
        case 3:
            // At this calculation the new lot under the reused ID is already open.
            if (row.calls == Calls::Reopened)
                strategy_close(row.next(), "reopened", omitted, omitted, false, 2);
            break;
        case 4:
            if (row.calls == Calls::TwoIds || row.calls == Calls::Reopen || row.calls == Calls::Reopened)
                break;
            strategy_close(row.next(), "finish", omitted, omitted, false, 3);
            for (const auto& p : adapter_.fixture_pending_snapshots())
                if (p.snapshot.family == source::PineOrderFamily::Close && p.snapshot.comment == "finish")
                    finish_ordinal = p.snapshot.command_ordinal;
            break;
        }
    }
};

struct Leg {
    const char* id;
    bool is_long;
    int entry, exit;
    double qty;
    std::string exit_id;
};

// Commands at a chart close trade at the next hour's open, 100 + bar.
// A reversing entry closes the old side before opening its own unit.
std::vector<Leg> trades(Row r) {
    const std::string first = std::string("__close__") + r.held();
    std::vector<Leg> legs{{r.held(), r.held_long, 1, 3, r.calls == Calls::TwoIds ? 2.0 : 1.0, first}};
    if (r.calls == Calls::TwoIds) {
        legs.push_back({"B", r.held_long, 2, 3, 1, "__close__B"});
        return legs;
    }
    if (r.split())
        legs.push_back({r.held(), r.held_long, 1, 3, 1, first});
    const int exit = r.calls == Calls::Reopen ? 3 : r.calls == Calls::Reopened ? 4 : 5;
    legs.push_back({r.next(), !r.held_long, 3, exit, 1, std::string("__close__") + r.next()});
    return legs;
}

int check(Row row) {
    Strategy host(row);
    const auto bars = feed(row.magnified);
    host.run(bars.data(), static_cast<int>(bars.size()), row.magnified ? "10" : "60", "60",
             row.magnified, 4, MagnifierDistribution::ENDPOINTS);
    if (!host.last_error().empty()) {
        std::cerr << "NOT COMPARED " << row.name << ": " << host.last_error() << '\n';
        return 2;
    }
    ReportC report{};
    host.fill_report(&report);
    const int sub_bars = row.magnified ? per_hour * charts : 0;
    const bool path = report.bar_magnifier_enabled == static_cast<int>(row.magnified)
        && report.input_bars_processed == static_cast<std::int64_t>(bars.size())
        && report.script_bars_processed == charts && report.magnifier_sub_bars_total == sub_bars
        && report.magnifier_sample_ticks_total == 4 * sub_bars;
    BacktestEngine::free_report(&report);
    std::vector<std::int64_t> hours;
    for (int i = 0; i < charts; ++i) hours.push_back(start + i * hour);
    const double units = row.calls == Calls::TwoIds ? 3 : row.split() ? 2 : 1;
    if (!path || host.callbacks != hours || host.seed != (row.held_long ? units : -units)
        || (row.check_drop() && !host.saw_first_close)) {
        std::cerr << "NOT COMPARED " << row.name << ": path=" << path << " callbacks="
                  << host.callbacks.size() << " seed=" << host.seed
                  << " first close in journal=" << host.saw_first_close << '\n';
        return 2;
    }
    bool ok = true;
    const auto equal = [&](const std::string& field, const auto& got, const auto& want) {
        if (got == want) return;
        std::cerr << "FAIL " << row.name << ": " << field << " expected " << want << ", got " << got << '\n';
        ok = false;
    };
    const auto wanted = trades(row);
    const int count = host.trade_count();
    equal("trade count", count, static_cast<int>(wanted.size()));
    for (int i = 0; i < std::min(count, static_cast<int>(wanted.size())); ++i) {
        const auto t = host.get_trade(i);
        const auto& w = wanted[i];
        const std::string n = "trade " + std::to_string(i + 1) + " ";
        equal(n + "entry bar", t.entry_bar_index, w.entry);
        equal(n + "exit bar", t.exit_bar_index, w.exit);
        equal(n + "entry price", t.entry_price, 100 + w.entry);
        equal(n + "exit price", t.exit_price, 100 + w.exit);
        equal(n + "entry time", t.entry_time, hours[w.entry]);
        equal(n + "exit time", t.exit_time, hours[w.exit]);
        equal(n + "quantity", t.qty, w.qty);
        equal(n + "side", t.is_long, w.is_long);
        equal(n + "entry ID", t.entry_id, w.id);
        equal(n + "exit ID", t.exit_id, w.exit_id);
        equal(n + "open at end", t.open_at_end, false);
    }
    equal("final position", host.live_position_size(), 0.0);
    if (row.check_drop()) {
        // The dropped statement leaves the preceding orders alone at the call,
        // emits no command event and takes no ordinal from the next real order.
        const int preceding = row.calls == Calls::Before ? 1 : 2;
        equal("rows before second close", host.before_rows, preceding);
        equal("rows after second close", host.after_rows, preceding);
        equal("events during second close", host.events_during_close, 0U);
        equal("finish ordinal", host.finish_ordinal, row.calls == Calls::Sum ? 5U : 4U);
    }
    if (ok) std::cout << "PASS " << row.name << '\n';
    return ok ? 0 : 1;
}
} // namespace

int main() {
    const Row rows[] = {
        {"lower/short/close-entry-close", Calls::Between, true, false},
        {"lower/long/close-entry-close", Calls::Between, true, true},
        {"lower/short/close-close-entry", Calls::Before, true, false},
        {"lower/long/close-close-entry", Calls::Before, true, true},
        {"control/lower/short/close-entry", Calls::Once, true, false},
        {"control/lower/long/close-entry", Calls::Once, true, true},
        {"control/chart/short/close-entry", Calls::Once, false, false},
        {"control/chart/long/close-entry", Calls::Once, false, true},
        {"control/chart/short/close-entry-close", Calls::Between, false, false},
        {"control/chart/long/close-entry-close", Calls::Between, false, true},
        {"control/chart/short/close-close-entry", Calls::Before, false, false},
        {"control/chart/long/close-close-entry", Calls::Before, false, true},
        {"lower/two-partials-close-entry", Calls::Sum, true, false},
        {"control/lower/distinct-ids", Calls::TwoIds, true, true},
        {"control/lower/reopen-then-close", Calls::Reopen, true, false},
        {"control/lower/close-after-reopen-filled", Calls::Reopened, true, false},
    };
    int status = 0;
    for (const auto& row : rows) {
        try {
            status = std::max(status, check(row));
        } catch (const std::exception& e) {
            std::cerr << "NOT COMPARED " << row.name << ": " << e.what() << '\n';
            status = 2;
        }
    }
    return status;
}
