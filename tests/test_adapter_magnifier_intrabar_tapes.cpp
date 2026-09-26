// R5 lane MAG-INTRABAR: the bar magnifier's intrabars on TradingView's own
// tapes (tests/fixtures/magnifier_intrabars, `lab tv` exports of the lane's
// own synthetic scripts; the README names each).
//
// 1. TradingView's intrabars, printed by request.security_lower_tf inside two
//    or three chart bars per chart timeframe the population uses -- 1m (10S),
//    5m (30S), 15m on NSE:NIFTY (2), 30m (5), 1h (10), 4h (30), 1D (60) on
//    BINANCE:ETHUSDT.P and NYSE:F: each timeframe is the table's
//    (source::tradingview_intrabar_timeframe), each intrabar steps by it from
//    the session's own grid, and each is owned by the chart bar that holds its
//    last instant -- on NIFTY's 09:30 IST bar the first one opens at 09:29, the
//    session's last one closes at 15:30 after one minute, NYSE:F's last hour
//    is 15:30-16:00. Where the fixture carries the 1-minute bars, the
//    intrabars source::tradingview_magnifier_bars builds from them equal
//    TradingView's open, high, low and close, one for one.
// 2. Magnified bracket tapes: a market long every other chart bar, a +-K tick
//    bracket that often resolves inside one chart bar. Replayed through the
//    adapter on the 1-minute feed with the magnifier on, every exit is
//    TradingView's: bar and price. Walking the feed's own minutes (the
//    adapter before the lane) parts on 5 of 23 (ETH 15m), 5 of 11 (ETH 30m)
//    and 4 of 11 (NIFTY 15m) of these.
// 3. calc_on_order_fills cascades under the magnifier (15m ETH): at a signal
//    bar one market long, then every fill's recalculation adds one while
//    fewer than six are open, so each lot books the next fill point of
//    TradingView's magnified path -- on mi-coof-refill's :15 bars the chart
//    bar's own open once, then the straddling intrabar's open twice, its
//    first and second extreme, then the next intrabar's open (its close is
//    not a fill point); on the :00 bars of the same tape and the :30 bars of
//    mi-coof-refill-30 the first intrabar's open twice, and so on. Every lot
//    of the 72 cascades is TradingView's. Walking the feed's own minutes with
//    the chart bar's waypoint rule (the adapter before the lane) books none
//    of the 72.
//
// Source-bound (includes pineforge/source): release profile only.
#include "native_margin_hooks_fixture.hpp"

#include <pineforge/source/magnifier_intrabars.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PINEFORGE_MI_FIXTURE_DIR
#error "PINEFORGE_MI_FIXTURE_DIR must name tests/fixtures/magnifier_intrabars"
#endif

using namespace r4_test;
using namespace l4b_fixture;
using pineforge::source::tradingview_intrabar_timeframe;
using pineforge::source::tradingview_magnifier_bars;

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};
#include "fixtures/magnifier_intrabars/bars_1m.inc"

std::vector<Bar> bars_of(const FeedBar* rows, std::size_t n) {
    std::vector<Bar> out;
    for (std::size_t i = 0; i < n; ++i)
        out.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, 1.0, rows[i].ts});
    return out;
}

// One CSV record, double quotes honoured (the order comments hold commas).
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (char ch : line) {
        if (ch == '"') quoted = !quoted;
        else if (ch == ',' && !quoted) out.emplace_back();
        else if (ch != '\r' && ch != '\n') out.back().push_back(ch);
    }
    return out;
}

struct TapeRow { int trade; bool entry; std::int64_t time; std::string signal; double price; };

// "YYYY-MM-DD HH:MM" in UTC+8 (the exports' time zone) -> UTC ms.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    return ((days * 24 + h - 8) * 60 + mi) * 60000LL;
}

std::vector<TapeRow> tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_MI_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);
    std::vector<TapeRow> out;
    while (std::getline(in, line)) {
        const auto cell = fields(line);
        if (cell.size() < 5) continue;
        out.push_back({std::atoi(cell[0].c_str()), cell[1].rfind("Entry", 0) == 0,
                       tape_ms(cell[2]), cell[3], std::atof(cell[4].c_str())});
    }
    return out;
}

// ------------------------------------------------------------------ 1. intrabars
struct Printed {
    int n = -1;
    std::int64_t time_s = 0, close_s = 0;   // the chart bar's own time and time_close
    std::vector<double> t, e, o, h, l, c;
};

// "b0 n=.. t.. tc.." and "b0t0[..]" .. "b0c3[..]" (mkprobes.py) or "b0t[..]"
// (the NYSE:F probe), appended in order.
std::map<std::string, Printed> printed(const std::string& slug) {
    std::map<std::string, Printed> out;
    for (const TapeRow& row : tape(slug)) {
        if (!row.entry) continue;
        const std::string& s = row.signal;
        if (s.size() < 3 || s[0] != 'b') continue;
        const std::string tag = s.substr(0, 2);
        Printed& p = out[tag];
        if (s[2] == ' ') {
            long long n = 0, t = 0, tc = 0;
            std::sscanf(s.c_str() + 2, " n=%lld t%lld tc%lld", &n, &t, &tc);
            p.n = static_cast<int>(n);
            p.time_s = t;
            p.close_s = tc;
            continue;
        }
        const auto open = s.find('[');
        if (open == std::string::npos) continue;
        std::vector<double> values;
        std::stringstream in(s.substr(open + 1));
        std::string item;
        while (std::getline(in, item, ',')) {
            if (item.find_first_of("0123456789") == std::string::npos) continue;
            values.push_back(std::atof(item.c_str()));
        }
        std::vector<double>* dst = nullptr;
        switch (s[2]) {
        case 't': dst = &p.t; break;
        case 'e': dst = &p.e; break;
        case 'o': dst = &p.o; break;
        case 'h': dst = &p.h; break;
        case 'l': dst = &p.l; break;
        case 'c': dst = &p.c; break;
        default: break;
        }
        if (dst) dst->insert(dst->end(), values.begin(), values.end());
    }
    return out;
}

struct Probe {
    const char* slug;
    const char* chart;
    const char* intrabar;      // TradingView's table
    std::int64_t step_s;       // the intrabar's length, seconds
    std::int64_t unit_s;       // the unit the probe printed times in, seconds
    const char* session;
    const char* timezone;
    const FeedBar* bars;       // 1-minute bars of the printed chart bars, or null
    std::size_t n;
};

// Seconds since the UTC day's start of an instant printed in `unit_s` units.
std::int64_t day_seconds(double printed, std::int64_t unit_s) {
    return static_cast<std::int64_t>(std::llround(printed * static_cast<double>(unit_s)));
}

void intrabars_on_tapes() {
    const Probe probes[] = {
        {"mi-ltf-eth-1", "1", "10S", 10, 1, "24x7", "UTC", nullptr, 0},
        {"mi-ltf-eth-5", "5", "30S", 30, 1, "24x7", "UTC", nullptr, 0},
        {"mi-ltf-eth-30", "30", "5", 300, 60, "24x7", "UTC", kMiLtfEth30, std::size(kMiLtfEth30)},
        {"mi-ltf-eth-60", "60", "10", 600, 60, "24x7", "UTC", kMiLtfEth60, std::size(kMiLtfEth60)},
        {"mi-ltf-eth-240", "240", "30", 1800, 60, "24x7", "UTC", kMiLtfEth240, std::size(kMiLtfEth240)},
        {"mi-ltf-eth-1D", "1D", "60", 3600, 3600, "24x7", "UTC", nullptr, 0},
        {"mi-ltf-nifty-15", "15", "2", 120, 60, "0915-1530", "Asia/Kolkata", nullptr, 0},
        {"mi-ltf-f-1D", "1D", "60", 3600, 60, "0930-1600", "America/New_York", kMiLtfF1D,
         std::size(kMiLtfF1D)},
    };
    for (const Probe& probe : probes) {
        std::printf("-- intrabars %s (chart %s)\n", probe.slug, probe.chart);
        CHECK(tradingview_intrabar_timeframe(probe.chart) == probe.intrabar);
        const auto bars = printed(probe.slug);
        REQUIRE(bars.size() >= 2);
        std::vector<Bar> feed = probe.bars ? bars_of(probe.bars, probe.n) : std::vector<Bar>{};
        if (std::string(probe.slug) == "mi-ltf-nifty-15") {
            feed = bars_of(kMiLtfNiftyOpen, std::size(kMiLtfNiftyOpen));
            const auto close = bars_of(kMiLtfNiftyClose, std::size(kMiLtfNiftyClose));
            feed.insert(feed.end(), close.begin(), close.end());
        }
        const auto path = feed.empty() ? std::vector<Bar>{}
            : tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()), "1",
                                         probe.chart, probe.session, probe.timezone);
        for (const auto& [tag, p] : bars) {
            REQUIRE(p.n > 0);
            REQUIRE(p.t.size() == static_cast<std::size_t>(p.n));
            REQUIRE(p.e.size() == static_cast<std::size_t>(p.n));
            const std::int64_t open_s = p.time_s % 86400;
            const std::int64_t close_s = open_s + (p.close_s - p.time_s);
            std::printf("  %s: chart %lld..%lld s, %d intrabars from %lld s\n", tag.c_str(),
                        static_cast<long long>(open_s), static_cast<long long>(close_s), p.n,
                        static_cast<long long>(day_seconds(p.t.front(), probe.unit_s)));
            for (int k = 0; k < p.n; ++k) {
                std::int64_t start = day_seconds(p.t[static_cast<std::size_t>(k)], probe.unit_s);
                std::int64_t end = day_seconds(p.e[static_cast<std::size_t>(k)], probe.unit_s);
                if (end <= start) end += 86400;   // printed modulo the day
                if (start < open_s - probe.step_s) start += 86400;
                // The table's step on the session's grid; only the session's
                // last intrabar may be cut short by its close.
                CHECK(end - start == probe.step_s || (k + 1 == p.n && end - start < probe.step_s
                                                      && end == close_s));
                if (k > 0) {
                    CHECK(start == day_seconds(p.t[static_cast<std::size_t>(k - 1)], probe.unit_s)
                                       + probe.step_s
                          || start + 86400 == day_seconds(p.t[static_cast<std::size_t>(k - 1)],
                                                          probe.unit_s) + probe.step_s);
                }
                // Owned by the chart bar holding its last instant.
                CHECK(end > open_s && end <= close_s);
                CHECK(start > open_s - probe.step_s);
            }
            if (path.empty()) continue;
            // The builder's intrabars of this chart bar (its one-price open and
            // close bars aside) are TradingView's, value for value.
            std::vector<Bar> mine;
            const std::int64_t open_ms = p.time_s * 1000;
            const std::int64_t close_ms = p.close_s * 1000;
            for (const Bar& b : path)
                if (b.timestamp >= open_ms && b.timestamp < close_ms && b.volume > 0.0)
                    mine.push_back(b);
            CHECK(mine.size() == static_cast<std::size_t>(p.n));
            if (mine.size() != static_cast<std::size_t>(p.n) || p.o.size() != mine.size()) continue;
            for (std::size_t k = 0; k < mine.size(); ++k) {
                CHECK(std::abs(mine[k].open - p.o[k]) <= 1e-9);
                CHECK(std::abs(mine[k].high - p.h[k]) <= 1e-9);
                CHECK(std::abs(mine[k].low - p.l[k]) <= 1e-9);
                CHECK(std::abs(mine[k].close - p.c[k]) <= 1e-9);
            }
        }
    }
}

// ------------------------------------------------------------ 2. bracket tapes
struct BracketSide final : source::PineStrategyHost {
    std::set<std::int64_t> signal_bars;
    std::vector<std::int64_t> seen;
    double ticks = 0.0;
    BracketSide(const char* session, const char* timezone, double mintick, double k) : ticks(k) {
        auto& tv = fixture_configuration();
        tv.initial_capital = 1000000.0;
        tv.default_qty_type = static_cast<int>(QtyType::FIXED);
        tv.default_qty_value = 1.0;
        tv.commission_type = static_cast<int>(CommissionType::PERCENT);
        tv.commission_value = 0.0;
        tv.slippage = 0;
        tv.pyramiding = 1;
        initial_capital_ = tv.initial_capital;
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
        set_syminfo_mintick(mintick);
    }
    void on_source_bar(const Bar& bar) override {
        seen.push_back(bar.timestamp);
        if (physical_position().signed_units == 0.0 && signal_bars.count(bar.timestamp))
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, {}, kNaN, {}, ticks, ticks);
    }
};

struct Bracket {
    const char* slug;
    const char* chart;
    double ticks;
    const char* session;
    const char* timezone;
    double mintick;
    const FeedBar* bars;
    std::size_t n;
    int trades;                // replayed: every one but the range end's
};

void brackets_on_tapes() {
    const Bracket tapes[] = {
        {"mi-fx-eth-15", "15", 400, "24x7", "UTC", 0.01, kMiFxEth15, std::size(kMiFxEth15), 23},
        {"mi-fx-eth-30", "30", 600, "24x7", "UTC", 0.01, kMiFxEth30, std::size(kMiFxEth30), 11},
        {"mi-fx-nifty-15b", "15", 400, "0915-1530", "Asia/Kolkata", 0.05, kMiFxNifty15b,
         std::size(kMiFxNifty15b), 11},
    };
    for (const Bracket& fx : tapes) {
        std::printf("-- bracket tape %s\n", fx.slug);
        std::map<int, TapeRow> entries, exits;
        for (const TapeRow& row : tape(fx.slug)) (row.entry ? entries : exits)[row.trade] = row;
        REQUIRE(!entries.empty() && entries.size() == exits.size());
        const auto input = bars_of(fx.bars, fx.n);
        // The chart bars the adapter publishes, then the entries on the bar
        // before each tape entry (the script's bar_index % 2 cadence counts
        // TradingView's history, which this window does not hold).
        BracketSide probe(fx.session, fx.timezone, fx.mintick, fx.ticks);
        probe.run(input.data(), static_cast<int>(input.size()), "1", fx.chart, true);
        CHECK(probe.last_error().empty());
        const auto& chart = probe.seen;
        const auto chart_of = [&chart](std::int64_t t) {
            auto it = std::upper_bound(chart.begin(), chart.end(), t);
            return it == chart.begin() ? std::int64_t{-1} : *(it - 1);
        };
        BracketSide pine(fx.session, fx.timezone, fx.mintick, fx.ticks);
        for (const auto& [n, row] : entries) {
            const auto it = std::lower_bound(chart.begin(), chart.end(), row.time);
            if (it != chart.begin() && it != chart.end() && *it == row.time)
                pine.signal_bars.insert(*(it - 1));
        }
        pine.run(input.data(), static_cast<int>(input.size()), "1", fx.chart, true);
        CHECK(pine.last_error().empty());
        std::map<std::int64_t, const Trade*> by_entry;
        for (int i = 0; i < pine.trade_count(); ++i) {
            const Trade& t = pine.get_trade(i);
            by_entry[chart_of(t.entry_time)] = &t;
        }
        int replayed = 0;
        int equal = 0;
        const std::int64_t range_end = std::prev(exits.end())->second.time;
        for (const auto& [n, entry] : entries) {
            const TapeRow& exit = exits[n];
            if (exit.time >= range_end || exit.signal.empty()) continue;   // the range end
            ++replayed;
            const auto found = by_entry.find(entry.time);
            const bool same = found != by_entry.end()
                && std::abs(found->second->entry_price - entry.price) <= 1e-9
                && chart_of(found->second->exit_time) == exit.time
                && std::abs(found->second->exit_price - exit.price) <= 1e-9;
            if (same) {
                ++equal;
            } else {
                std::printf("  trade %d: tape %.4f -> %.4f at %lld, adapter %s\n", n, entry.price,
                            exit.price, static_cast<long long>(exit.time),
                            found == by_entry.end() ? "none" : "differs");
            }
        }
        std::printf("  %d of %d exits are TradingView's\n", equal, replayed);
        CHECK(replayed == fx.trades);
        CHECK(equal == replayed);
    }
}

// ---------------------------------------------------------- 3. refill cascades
struct RefillSide final : source::PineStrategyHost {
    RefillSide() {
        source::PineStrategyConfig config;
        config.calc_on_order_fills = true;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 10;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.01);
    }
    double lot_price(int index) const { return open_trade_entry_price(index); }
    // The script's cascade: the window starts at the signal bar.
    void on_source_bar(const Bar&) override {
        if (bar_index_ <= 1 && std::abs(physical_position().signed_units) < 6.0)
            strategy_entry("L" + std::to_string(physical_position().lot_count), true);
    }
};

void refill_cascades_on_tapes() {
    const struct { const char* slug; const FeedBar* bars; std::size_t n; } tapes[] = {
        {"mi-coof-refill", kMiCoofRefill, std::size(kMiCoofRefill)},
        {"mi-coof-refill-30", kMiCoofRefill30, std::size(kMiCoofRefill30)},
    };
    for (const auto& fx : tapes) {
        std::printf("-- refill tape %s\n", fx.slug);
        // Each cascade's lots by their L<k> id, keyed by the chart bar they
        // fill on.
        std::map<std::int64_t, std::map<int, double>> cascades;
        for (const TapeRow& row : tape(fx.slug)) {
            if (row.entry && row.signal.size() > 1 && row.signal[0] == 'L')
                cascades[row.time][std::atoi(row.signal.c_str() + 1)] = row.price;
        }
        const auto feed = bars_of(fx.bars, fx.n);
        int equal = 0;
        for (const auto& [bar, lots] : cascades) {
            std::vector<Bar> window;
            for (const Bar& b : feed)
                if (b.timestamp >= bar - 15 * 60000LL && b.timestamp < bar + 30 * 60000LL)
                    window.push_back(b);
            REQUIRE(window.size() == 45);
            RefillSide pine;
            pine.run(window.data(), static_cast<int>(window.size()), "1", "15", true);
            CHECK(pine.last_error().empty());
            const int count = pine.physical_position().lot_count;
            bool same = count == static_cast<int>(lots.size());
            std::string mine;
            for (int i = 0; i < count; ++i) {
                char text[32];
                std::snprintf(text, sizeof text, " %.2f", pine.lot_price(i));
                mine += text;
                const auto want = lots.find(i);
                same = same && want != lots.end()
                    && std::abs(want->second - pine.lot_price(i)) <= 1e-9;
            }
            if (same) {
                ++equal;
            } else {
                std::string theirs;
                for (const auto& [k, price] : lots) {
                    char text[32];
                    std::snprintf(text, sizeof text, " %.2f", price);
                    theirs += text;
                }
                std::printf("  cascade %lld: tape%s, adapter%s\n", static_cast<long long>(bar),
                            theirs.c_str(), mine.c_str());
            }
        }
        std::printf("  %d of %zu cascades are TradingView's\n", equal, cascades.size());
        CHECK(cascades.size() == 36);
        CHECK(equal == static_cast<int>(cascades.size()));
    }
}

}  // namespace

int main() {
    test("MAG-INTRABAR TradingView intrabars", intrabars_on_tapes);
    test("MAG-INTRABAR bracket tapes", brackets_on_tapes);
    test("MAG-INTRABAR refill cascades", refill_cascades_on_tapes);
    std::printf("test_adapter_magnifier_intrabar_tapes: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
