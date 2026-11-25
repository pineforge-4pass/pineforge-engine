// BINANCE:BTCUSDT, 1h, 2024-01-01 through 2024-03-01. The reported
// percentage is per entry order; the closed trade rows are booked FIFO.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double no_price = std::numeric_limits<double>::quiet_NaN();
constexpr double entry_qty = 0.02, share = entry_qty / 2;
constexpr std::int64_t start = 1704067200000, hour = 3600000;
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

std::vector<Bar> feed() {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path()
                     / "fixtures/qty_step_mincontract/btc60/feed/bars.csv");
    std::string line;
    if (!std::getline(in, line) || line != "timestamp,open,high,low,close,volume")
        throw std::runtime_error("missing BTCUSDT hourly feed or wrong header");
    std::vector<Bar> bars;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        Bar b{};
        const auto comma = [&] { return row.get() == ','; };
        if (!(row >> b.timestamp) || !comma() || !(row >> b.open) || !comma()
            || !(row >> b.high) || !comma() || !(row >> b.low) || !comma()
            || !(row >> b.close) || !comma() || !(row >> b.volume) || !row.eof()
            || b.timestamp != start + static_cast<std::int64_t>(bars.size()) * hour
            || !(0 < b.low && b.low <= std::min(b.open, b.close)
                 && std::max(b.open, b.close) <= b.high && b.volume >= 0))
            throw std::runtime_error("invalid hourly bar " + std::to_string(bars.size()));
        bars.push_back(b);
    }
    if (!in.eof() || bars.size() != 1441 || !near(bars[89].open, 43915.70)
        || !near(bars[91].open, 44048.95) || !near(bars[137].open, 43872.35)
        || !near(bars[139].open, 43995.89))
        throw std::runtime_error("incomplete or different reported BTCUSDT window");
    return bars;
}

// The Applied observer reads the owner before the next bar retires the row.
template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct Placements { friend auto member(Placements); };
template struct Access<Placements, &source::PineExecutionAdapter::placement_>;

struct Fill {
    std::uint64_t handle, owner;
    int bar;
    double price, units;
};
class Replay final : public source::PineStrategyHost {
public:
    using source::PineStrategyHost::native_state;
    explicit Replay(bool partial) : partial_(partial) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg;
        cfg.initial_capital = 10000;
        cfg.pyramiding = 2;
        configure_pine_strategy(cfg);
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        set_syminfo_metadata("qty_step", 0.00001);
        set_syminfo_metadata("series_origin_ms", start);
    }
    void on_source_bar(const Bar& bar) override {
        ++bar_count;
        const double f = fast_.compute(bar.close), s = slow_.compute(bar.close);
        const bool up = cross_.compute(f, s);
        if (up || two_ago_) strategy_entry("Long", true, no_price, no_price, entry_qty);
        const double limit = signed_position_size() == 0 ? no_price : position_avg_price() * 1.01;
        if (partial_) strategy_exit("TP", "Long", limit, no_price, no_price, no_price, no_price, 50);
        else strategy_exit("TP", "Long", limit, no_price);
        if (under_.compute(f, s)) strategy_close("Long");
        two_ago_ = previous_;
        previous_ = up;
    }
    int bar_count = 0;
    std::vector<Fill> entries, targets;
private:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& e,
                                 const NativeDecisionContext&) override {
        if (e.request().label == "Long" && e.opened_units > 0)
            entries.push_back({e.handle().incarnation, 0, e.interval_index(),
                               e.resolved_price, e.opened_units});
        if (e.request().label == "TP" && e.closed_units > 0) {
            const auto& placements = adapter_.*member(Placements{});
            const auto it = placements.find(e.handle().incarnation);
            targets.push_back({e.handle().incarnation,
                it == placements.end() ? 0 : it->second.per_entry_origin,
                e.interval_index(), e.resolved_price, e.closed_units});
        }
    }
    bool partial_, previous_ = false, two_ago_ = false;
    ta::EMA fast_{10}, slow_{30};
    ta::Crossover cross_;
    ta::Crossunder under_;
};

struct Row { int entry, exit; double qty, price; bool tp, open; };
struct Target { int owner, exit; double price; };
struct Expected { std::vector<Row> rows; std::vector<Target> targets; };

// Read the orders directly from the bars. Market instructions take the
// next open; the sell limit standing from the prior close takes the first
// eligible tick or a better open. Its order owner and its FIFO lot can differ.
Expected expected(const std::vector<Bar>& bars, bool partial) {
    struct Lot { int entry; double left; bool spent; };
    std::vector<Lot> lots;
    Expected result;
    // Seed from the first period of closes in this window. The reference
    // does not call the host's EMA, crossover or execution code.
    const auto ema = [](long double& value, int length, int i, double close) -> long double {
        if (i < length) {
            value += close;
            if (i + 1 < length) return no_price;
            value /= length;
        } else value += (close - value) * (2.0L / (1 + length));
        return value;
    };
    long double fast = 0, slow = 0, last_diff = no_price;
    double limit = no_price, close_qty = 0;
    bool add = false, up1 = false, up2 = false;
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        const auto& b = bars[i];
        const auto book = [&](double qty, double price, bool tp, bool open = false) {
            for (auto& lot : lots) {
                const double take = std::min(qty, lot.left);
                if (take <= 0) continue;
                result.rows.push_back({lot.entry, i, take, price, tp, open});
                qty -= take;
                lot.left -= take;
            }
        };
        if (add) lots.push_back({i, entry_qty, false});
        if (close_qty > 0) book(close_qty, b.open, false);
        if (b.high >= limit) {
            const double price = std::max(b.open, limit);
            for (auto& lot : lots) {
                if (lot.spent || lot.left == 0) continue;
                result.targets.push_back({lot.entry, i, price});
                lot.spent = true;
                book(partial ? share : entry_qty, price, true);
            }
        }
        lots.erase(std::remove_if(lots.begin(), lots.end(),
            [](const Lot& lot) { return lot.left == 0; }), lots.end());
        double cost = 0, units = 0;
        for (const auto& lot : lots) {
            units += lot.left;
            cost += lot.left * bars[lot.entry].open;
        }
        const auto diff = ema(fast, 10, i, b.close) - ema(slow, 30, i, b.close);
        const bool up = diff > 0 && last_diff <= 0;
        add = (up || up2) && lots.size() < 2;
        close_qty = diff < 0 && last_diff >= 0 ? units : 0;
        limit = units > 0 ? std::ceil(cost / units * 1.01 * 100 - 1e-9) / 100 : no_price;
        up2 = up1;
        up1 = up;
        last_diff = diff;
        if (i + 1 == static_cast<int>(bars.size())) book(units, b.close, false, true);
    }
    return result;
}

std::string compare_rows(const Replay& host, const std::vector<Row>& rows,
                         const std::vector<Bar>& bars, bool targets_only = false) {
    std::size_t next = 0;
    const auto skip_market_rows = [&] {
        while (targets_only && next < rows.size() && !rows[next].tp) ++next;
    };
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const auto got = host.get_report_trade(i);
        if (targets_only && got.exit_id != "TP") continue;
        skip_market_rows();
        if (next == rows.size()) return "unexpected trade " + std::to_string(i + 1);
        const auto& want = rows[next++];
        if (got.entry_id != "Long" || !got.is_long || got.entry_bar_index != want.entry
            || !near(got.entry_price, bars[want.entry].open) || got.exit_bar_index != want.exit
            || !near(got.exit_price, want.price) || !near(got.qty, want.qty)
            || got.open_at_end != want.open
            || (!want.open && got.exit_id != (want.tp ? "TP" : "__close__Long")))
            return "first different trade: " + std::to_string(i + 1);
    }
    skip_market_rows();
    return next == rows.size() ? "" : "missing trade from entry bar " + std::to_string(rows[next].entry);
}

std::string check_targets(const Replay& host, const Expected& want) {
    if (host.targets.size() != want.targets.size()) return "wrong number of TP orders filled";
    // Each expected order must have a distinct fill; two equal amounts from
    // one order or two orders for the same opening do not satisfy the rule.
    std::set<std::uint64_t> handles;
    for (const auto& fill : host.targets)
        if (!fill.handle || !handles.insert(fill.handle).second) return "repeated TP order handle";
    for (const auto& target : want.targets) {
        const auto entry = std::find_if(host.entries.begin(), host.entries.end(),
            [&](const Fill& e) { return e.bar == target.owner && near(e.units, entry_qty); });
        if (entry == host.entries.end()) return "missing opening at bar " + std::to_string(target.owner);
        const auto matches = [&](const Fill& f) {
            return f.owner == entry->handle && f.bar == target.exit
                && near(f.units, share) && near(f.price, target.price);
        };
        if (std::count_if(host.targets.begin(), host.targets.end(), matches) != 1)
            return "expected one 0.01 TP for opening at bar " + std::to_string(target.owner);
    }
    return {};
}

bool published_pair(const Replay& host, int first, int second, int exit, double price,
                    const std::vector<Bar>& bars) {
    const auto entry = [&](int bar) {
        return std::count_if(host.entries.begin(), host.entries.end(), [&](const Fill& fill) {
            return fill.bar == bar && near(fill.units, entry_qty) && near(fill.price, bars[bar].open);
        }) == 1;
    };
    int matches = 0;
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto trade = host.get_trade(i);
        if (trade.exit_bar_index != exit || trade.exit_id != "TP") continue;
        if (!trade.is_long || trade.entry_id != "Long" || trade.entry_bar_index != first || !near(trade.qty, share)
            || !near(trade.entry_price, bars[first].open) || !near(trade.exit_price, price)) return false;
        ++matches;
    }
    return entry(first) && entry(second) && matches == 2;
}

int failures = 0;
constexpr const char* count_row = "control.percentage-report-count";
constexpr const char* order_row = "percentage.order-per-opening";
constexpr const char* first_pair = "percentage.jan04";
constexpr const char* second_pair = "percentage.jan07";
constexpr const char* fifo_row = "percentage.fifo-bookings";
constexpr const char* control_row = "control.full-exit-list";

void unavailable(const char* name, const std::string& why) {
    ++failures;
    std::cerr << "NOT COMPARED " << name << ": " << why << '\n';
}
void verdict(const char* name, const std::string& problem, const char* reason) {
    if (!problem.empty()) ++failures;
    std::cout << (problem.empty() ? "PASS " : "FAIL ") << name << ": " << reason << '\n';
    if (!problem.empty()) std::cout << "  " << problem << '\n';
}
void compare(const std::vector<Bar>& bars, bool partial) {
    const auto want = expected(bars, partial);
    const int count = partial ? 68 : 52;
    double pnl = 0;
    for (const auto& r : want.rows)
        if (!r.open) pnl += (r.price - bars[r.entry].open) * r.qty;
    const bool oracle_ready = static_cast<int>(want.rows.size()) == count && !want.targets.empty()
        && std::lround(pnl * 100) == (partial ? 12616 : 12623);
    const std::string oracle_error = "bar-derived rows=" + std::to_string(want.rows.size())
        + " closed profit=" + std::to_string(pnl) + "; expected "
        + (partial ? "68 and 126.16" : "52 and 126.23");
    Replay host(partial);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    if (!host.last_error().empty() || host.bar_count != static_cast<int>(bars.size())
        || host.entries.empty() || host.report_trade_count() == 0)
        throw std::runtime_error("incomplete strategy replay: " + host.last_error());
    const auto native = host.native_state();
    if (!native.spec || !native.spec->quantity_grid || *native.spec->quantity_grid != 0.00001)
        throw std::runtime_error("declared BTCUSDT quantity step was not applied");
    if (!partial) {
        if (!oracle_ready) unavailable(control_row, oracle_error);
        else verdict(control_row, compare_rows(host, want.rows, bars),
                     "all 52 trades match the full exit");
        return;
    }
    verdict(count_row, host.report_trade_count() == count ? "" : "got " + std::to_string(host.report_trade_count()),
            "68 closed and open rows");
    verdict(first_pair, published_pair(host, 89, 91, 91, 44354.86, bars)
            ? "" : "expected two FIFO halves at 44354.86", "two FIFO halves at 44354.86");
    verdict(second_pair, published_pair(host, 137, 139, 157, 44373.47, bars)
            ? "" : "expected two FIFO halves at 44373.47", "two FIFO halves at 44373.47");
    if (!oracle_ready) {
        unavailable(order_row, oracle_error);
        unavailable(fifo_row, oracle_error);
    } else {
        verdict(order_row, check_targets(host, want), "one 0.01 fill per eligible opening");
        verdict(fifo_row, compare_rows(host, want.rows, bars, true),
                "TP orders book against the oldest open units");
    }
}
} // namespace

int main() {
    for (bool partial : {true, false}) {
        try {
            compare(feed(), partial);
        } catch (const std::exception& e) {
            if (partial) {
                for (const char* name : {count_row, first_pair, second_pair, order_row, fifo_row})
                    unavailable(name, e.what());
            } else unavailable(control_row, e.what());
        }
    }
    return failures != 0;
}
