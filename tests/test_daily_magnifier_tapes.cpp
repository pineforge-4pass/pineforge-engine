// The bar magnifier on a daily chart (R5 lane XAU-CAL), on TradingView's own
// tapes (tests/fixtures/daily_magnifier, `lab tv --no-note` exports of the
// lane's own synthetic scripts on OANDA:XAUUSD 1D, 2026-02-16 .. 04-01; the
// README names each).
//
// TradingView backtests a daily chart that declares use_bar_magnifier = true on
// its 60-minute intrabars (the help centre's table row for 1D,
// source::tradingview_intrabar_timeframe), and dates every order by the daily
// bar's own stamp: OANDA's 17:00 ET, 22:00 UTC before the 2026-03-08 switch to
// daylight time and 21:00 UTC after it, where the 1800-1700 session opens an
// hour later.
//
// 1. xc-dmag-dip-xau1d: whenever flat, a buy limit 0.2 % under the close with a
//    take-profit 0.4 % over it; whether a day books both depends on the order
//    of its extremes. Replayed on the day's hourly bars with the magnifier on
//    and the chart's daily bars installed as the run's daily feed, every one of
//    TradingView's 20 trades is the adapter's -- entry and exit bar and price,
//    and the entry comment spelling the placing bar's time and time_close. Its
//    twin declared off (xc-dmag-dip-off-xau1d), replayed on the daily bars
//    unmagnified, books TradingView's 18: the declaration decides 2 trades.
// 2. xc-dmag-xau1d and its -off twin: a market long with a +-0.4 % bracket.
//    Both books are TradingView's 31 trades; the two tapes are identical, as
//    no day of the window reaches the two legs in an order the daily bar's
//    rule misreads.
// 3. Without the daily feed the magnified run labels each day at the
//    session's 18:00 ET open: every trade and every comment is then an hour
//    off TradingView's, and nothing else differs -- the day-label rule
//    (NativeExecutionConsumer::prepare_day_labels) is what dates them.
//
// Source-bound (includes pineforge/source): release profile only.

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/str_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef PINEFORGE_DAILY_MAGNIFIER_FIXTURE_DIR
#error "PINEFORGE_DAILY_MAGNIFIER_FIXTURE_DIR must name tests/fixtures/daily_magnifier"
#endif

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "init";

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL [%s] line %d: %s\n", scenario, __LINE__, #expr); \
        }                                                                      \
    } while (0)

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};
#include "fixtures/daily_magnifier/bars.inc"

std::vector<Bar> bars_of(const FeedBar* rows, std::size_t n) {
    std::vector<Bar> out;
    for (std::size_t i = 0; i < n; ++i)
        out.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, rows[i].volume,
                       rows[i].ts});
    return out;
}

// One CSV record, double quotes honoured.
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

// One of TradingView's trades: its entry and exit rows.
struct TvTrade {
    std::int64_t entry_bar = 0, exit_bar = 0;
    double entry_price = 0.0, exit_price = 0.0;
    std::string entry_signal, exit_signal;
};

std::vector<TvTrade> tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_DAILY_MAGNIFIER_FIXTURE_DIR) + "/" + slug
                     + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);
    std::map<int, TvTrade> by_number;
    while (std::getline(in, line)) {
        const auto cell = fields(line);
        if (cell.size() < 5) continue;
        TvTrade& t = by_number[std::atoi(cell[0].c_str())];
        if (cell[1].rfind("Entry", 0) == 0) {
            t.entry_bar = tape_ms(cell[2]);
            t.entry_signal = cell[3];
            t.entry_price = std::atof(cell[4].c_str());
        } else {
            t.exit_bar = tape_ms(cell[2]);
            t.exit_signal = cell[3];
            t.exit_price = std::atof(cell[4].c_str());
        }
    }
    std::vector<TvTrade> out;
    for (const auto& entry : by_number) out.push_back(entry.second);
    return out;
}

enum class Script { Bracket, Dip };

// The synthetic scripts, line for line (fixtures' strategy.pine).
struct XauDaily final : source::PineStrategyHost {
    Script script;
    std::vector<std::int64_t> chart;   // every chart bar the host published
    explicit XauDaily(Script s) : script(s) {
        auto& cfg = fixture_configuration();
        cfg.initial_capital = 1000000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        initial_capital_ = cfg.initial_capital;
        set_syminfo_session("1800-1700");
        set_syminfo_timezone("America/New_York");
        set_syminfo_mintick(0.001);
    }
    std::string f(std::int64_t t) { return str_format_time(t, "MMdd-HHmm", "UTC"); }
    void on_source_bar(const Bar& bar) override {
        if (chart.empty() || chart.back() != bar.timestamp) chart.push_back(bar.timestamp);
        const std::string info = "t=" + f(current_bar_.timestamp) + "|tc=" + f(time_close());
        if (signed_position_size() != 0.0) return;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        if (script == Script::Bracket) {
            const double k = round_to_mintick(current_bar_.close * 0.004);
            strategy_entry("L", true, nan, nan, nan, info);
            strategy_exit("X", "L", current_bar_.close + k, current_bar_.close - k);
        } else {
            const double level = round_to_mintick(current_bar_.close * 0.998);
            const double target = round_to_mintick(current_bar_.close * 1.004);
            const double stop = round_to_mintick(level * 0.97);
            strategy_entry("L", true, level, nan, nan, info);
            strategy_exit("X", "L", target, stop);
        }
    }
    // The chart bar holding instant `t`.
    std::int64_t chart_of(std::int64_t t) const {
        auto it = std::upper_bound(chart.begin(), chart.end(), t);
        return it == chart.begin() ? std::int64_t{-1} : *(it - 1);
    }
};

struct Replay {
    int trades = 0;
    int equal = 0;          // bar, price and entry comment all TradingView's
    int hour_off = 0;       // the same but for bars and comments an hour later
};

bool same_price(double a, double b) { return std::abs(a - b) <= 1e-9; }

Replay replay(const char* slug, Script script, bool magnified, bool daily_feed) {
    const auto tv = tape(slug);
    XauDaily pine(script);
    const auto daily = bars_of(kXauDaily, std::size(kXauDaily));
    const auto hourly = bars_of(kXauHourly, std::size(kXauHourly));
    if (daily_feed)
        CHECK(pine.set_native_security_feed("D", daily.data(), static_cast<int>(daily.size())));
    if (magnified) {
        pine.run(hourly.data(), static_cast<int>(hourly.size()), "60", "1D", true);
    } else {
        pine.run(daily.data(), static_cast<int>(daily.size()), "1D", "1D", false);
    }
    CHECK(pine.last_error().empty());
    if (!pine.last_error().empty()) std::printf("  error: %s\n", pine.last_error().c_str());
    Replay out;
    out.trades = pine.trade_count();
    for (int i = 0; i < pine.trade_count() && i < static_cast<int>(tv.size()); ++i) {
        const Trade& t = pine.get_trade(i);
        const TvTrade& want = tv[static_cast<std::size_t>(i)];
        const bool prices = same_price(t.entry_price, want.entry_price)
            && same_price(t.exit_price, want.exit_price);
        const std::int64_t entry_bar = pine.chart_of(t.entry_time);
        const std::int64_t exit_bar = pine.chart_of(t.exit_time);
        if (prices && entry_bar == want.entry_bar && exit_bar == want.exit_bar
            && t.entry_comment == want.entry_signal) {
            ++out.equal;
        } else if (prices && entry_bar == want.entry_bar + 3600000
                   && exit_bar == want.exit_bar + 3600000) {
            ++out.hour_off;
        } else {
            std::printf("  trade %d: tape %lld %.3f -> %lld %.3f [%s], adapter %lld %.3f -> %lld %.3f [%s]\n",
                        i + 1, static_cast<long long>(want.entry_bar), want.entry_price,
                        static_cast<long long>(want.exit_bar), want.exit_price,
                        want.entry_signal.c_str(), static_cast<long long>(entry_bar), t.entry_price,
                        static_cast<long long>(exit_bar), t.exit_price, t.entry_comment.c_str());
        }
    }
    std::printf("  %s (%s%s): %d of %zu trades TradingView's, %d an hour late, %d booked\n", slug,
                magnified ? "magnified" : "daily bars", daily_feed ? ", daily feed" : "", out.equal,
                tv.size(), out.hour_off, out.trades);
    return out;
}

void test_dip_tapes() {
    scenario = "dip tapes";
    const auto on = tape("xc-dmag-dip-xau1d");
    const auto off = tape("xc-dmag-dip-off-xau1d");
    CHECK(on.size() == 20);
    CHECK(off.size() == 18);
    const Replay mag = replay("xc-dmag-dip-xau1d", Script::Dip, true, true);
    CHECK(mag.trades == 20);
    CHECK(mag.equal == 20);
    const Replay plain = replay("xc-dmag-dip-off-xau1d", Script::Dip, false, false);
    CHECK(plain.trades == 18);
    CHECK(plain.equal == 18);
}

void test_bracket_tapes() {
    scenario = "bracket tapes";
    const auto on = tape("xc-dmag-xau1d");
    const auto off = tape("xc-dmag-off-xau1d");
    CHECK(on.size() == 31);
    CHECK(off.size() == 31);
    bool same = on.size() == off.size();
    for (std::size_t i = 0; same && i < on.size(); ++i) {
        same = on[i].entry_bar == off[i].entry_bar && on[i].exit_bar == off[i].exit_bar
            && same_price(on[i].entry_price, off[i].entry_price)
            && same_price(on[i].exit_price, off[i].exit_price);
    }
    CHECK(same);
    const Replay mag = replay("xc-dmag-xau1d", Script::Bracket, true, true);
    CHECK(mag.trades == 31);
    CHECK(mag.equal == 31);
    const Replay plain = replay("xc-dmag-off-xau1d", Script::Bracket, false, false);
    CHECK(plain.trades == 31);
    CHECK(plain.equal == 31);
}

void test_without_the_daily_feed() {
    scenario = "without the daily feed";
    // The session's 18:00 ET open labels each day: the same trades, every one
    // an hour late.
    const Replay dip = replay("xc-dmag-dip-xau1d", Script::Dip, true, false);
    CHECK(dip.trades == 20);
    CHECK(dip.equal == 0);
    CHECK(dip.hour_off == 20);
    const Replay bracket = replay("xc-dmag-xau1d", Script::Bracket, true, false);
    CHECK(bracket.trades == 31);
    CHECK(bracket.equal == 0);
    CHECK(bracket.hour_off == 31);
}

}  // namespace

int main() {
    test_dip_tapes();
    test_bracket_tapes();
    test_without_the_daily_feed();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
