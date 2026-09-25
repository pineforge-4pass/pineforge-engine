/*
 * test_aggregated_entry_bar_index_tape.cpp — R5 lane H-MEASURE (AUDIT4 X14, F1(e)).
 *
 * The parity-visible half of lane F1 / K-IDX: a process_orders_on_close script
 * that READS the bar indices of its trades, replayed on an aggregated chart
 * against TradingView's own tape.
 *
 * AUDIT4 §3.2 (F1 e): the aggregated-index regressions were invisible to
 * parity because no aggregated corpus probe reads a bar index and none runs
 * process_orders_on_close (the only two aggregated probes,
 * ltf-bool-array-bull-majority-01 and ltf-numeric-float-ratio15-01, read
 * neither). tests/test_aggregated_path_regressions.cpp pins the pre-R4 engine
 * ab9714be; this row pins TradingView.
 *
 * The tape (tests/fixtures/aggregated_entry_bar_index/hm-f1e-ebi-pooc-eth,
 * `lab tv`, ws-report-v1, rangeProof covered, BINANCE:ETHUSDT.P 15m,
 * process_orders_on_close=true) enters at three fixed bar opens and closes
 * every position on a bar-index difference:
 *
 *   A long  01:00 UTC, closed once bar_index - strategy.opentrades.entry_bar_index(0) >= 1
 *   B short 05:00 UTC, closed once held >= 3
 *   C long  09:00 UTC, closed once held >= (last closed exit_bar_index - entry_bar_index) + 2
 *   D short entered when bar_index - strategy.closedtrades.exit_bar_index(2) == 4,
 *           closed once held >= 2
 *
 * and a three-hour "timeout" guard that TradingView never reaches. Each exit's
 * comment is "held<N>", the count TradingView's script computed, and the
 * tape's "Duration (bars)" is exit bar - entry bar: 1, 3, 5, 2.
 *
 * The same chart reaches the script three ways: the 15m bars fed as they are
 * (the control, input == script timeframe), and the corpus 1m bars they
 * aggregate, plain and under the bar magnifier. The chart and plain aggregated
 * paths answer the tape above field for field, excursions included; the
 * magnified path answers hm-f1e-ebi-pooc-eth-mag, the same script exported
 * with use_bar_magnifier=true, on every field but the excursions (TradingView's
 * magnifier moves trade 2's favorable excursion 0.40 -> 1.86, which the
 * engine's excursion model does not reproduce on any path; not this row's
 * subject: tests/test_e19_excursion_tape.cpp measures that model). Since K-IDX (ADR-0001 rule 2,
 * Option A) the kernel books lots, trade rows and NativeCoordinate::
 * interval_index in SCRIPT-bar space, so all three read chart bars. With the
 * kernel's pre-K-IDX input-slot booking (and no adapter re-stamp) the
 * aggregated paths read held = chart bar - input slot < 0: no held exit ever
 * fires, the guard closes A, B and C at their timeouts and D never enters.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_HM_F1E_FIXTURE_DIR
#error "PINEFORGE_HM_F1E_FIXTURE_DIR must name tests/fixtures/aggregated_entry_bar_index"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

#include "fixtures/aggregated_entry_bar_index/bars.inc"

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr const char* kSlug = "hm-f1e-ebi-pooc-eth";          // use_bar_magnifier=false
constexpr const char* kSlugMag = "hm-f1e-ebi-pooc-eth-mag";     // use_bar_magnifier=true

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t utc_ms(int y, int mo, int d, int h, int mi) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h) * 60 + mi) * kMinute;
}

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    return utc_ms(y, mo, d, h, mi) - 8 * kHour;
}

// ── the tape ──────────────────────────────────────────────────────────

struct TapeTrade {
    bool is_long = true;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double qty = 0.0;
    double net_pnl = 0.0;
    double favorable = 0.0;
    double adverse = 0.0;
    std::string entry_signal;
    std::string exit_signal;
    int duration_bars = -1;
};

std::vector<TapeTrade> read_tape(const char* slug) {
    std::ifstream in(std::string(PINEFORGE_HM_F1E_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 17) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        t.is_long = cell[1].find("long") != std::string::npos;
        (entry ? t.entry_ms : t.exit_ms) = tape_ms(cell[2]);
        (entry ? t.entry_signal : t.exit_signal) = cell[3];
        (entry ? t.entry_price : t.exit_price) = std::stod(cell[4]);
        t.qty = std::stod(cell[5]);
        t.net_pnl = std::stod(cell[7]);
        t.favorable = std::stod(cell[10]);
        t.adverse = std::stod(cell[12]);
        t.duration_bars = std::stoi(cell[16]);
    }
    return trades;
}

// ── the strategy, as the probe's Pine source runs it ─────────────────

struct Held {
    int bar = 0;
    std::string id;
    int held = 0;
};

class F1eHost final : public source::PineStrategyHost {
public:
    F1eHost() {
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        source::PineStrategyConfig config;
        config.initial_capital = 100000;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1;
        config.pyramiding = 1;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = true;
        config.margin_long = 100;
        config.margin_short = 100;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        // int lastDur = na
        // if strategy.closedtrades > 0
        //     lastDur := strategy.closedtrades.exit_bar_index(n - 1)
        //              - strategy.closedtrades.entry_bar_index(n - 1)
        const int closed = trade_count();
        const int last_duration = closed > 0
            ? closed_trade_exit_bar_index(closed - 1) - closed_trade_entry_bar_index(closed - 1)
            : 0;
        const std::int64_t t = current_bar_.timestamp;
        if (position_entry_count_ > 0) {
            // held = bar_index - strategy.opentrades.entry_bar_index(0)
            const int held = bar_index_ - open_trade_entry_bar_index(0);
            const std::string id = open_trade_entry_id(0);
            const int need = id == "A" ? 1 : id == "B" ? 3 : id == "D" ? 2 : last_duration + 2;
            held_.push_back({bar_index_, id, held});
            if (held >= need) {
                strategy_close(id, "held" + std::to_string(held));
            } else if (t - open_trade_entry_time(0) >= 3 * kHour) {
                strategy_close(id, "timeout");
            }
        } else if (t == utc_ms(2025, 6, 12, 1, 0)) {
            strategy_entry("A", true);
        } else if (t == utc_ms(2025, 6, 12, 5, 0)) {
            strategy_entry("B", false);
        } else if (t == utc_ms(2025, 6, 12, 9, 0)) {
            strategy_entry("C", true);
        } else if (closed == 3 && bar_index_ - closed_trade_exit_bar_index(2) == 4) {
            strategy_entry("D", false);
        }
    }

    // One booked row, as Pine's strategy.closedtrades.* accessors read it.
    struct Row {
        bool is_long = true;
        std::int64_t entry_ms = 0, exit_ms = 0;
        double entry_price = 0.0, exit_price = 0.0, qty = 0.0;
        double profit = 0.0, runup = 0.0, drawdown = 0.0;
        int entry_bar = 0, exit_bar = 0;
        std::string entry_id, exit_comment;
    };
    struct Run {
        std::vector<Row> closed;
        int open_count = 0;
        std::string open_id;
        std::int64_t open_entry_ms = 0;
        int open_entry_bar = 0;
        std::string error;
    };
    Run read() const {
        Run run;
        run.error = last_error();
        for (int i = 0; i < trade_count(); ++i) {
            Row row;
            row.is_long = trades_[static_cast<std::size_t>(i)].is_long;
            row.entry_ms = closed_trade_entry_time(i);
            row.exit_ms = closed_trade_exit_time(i);
            row.entry_price = closed_trade_entry_price(i);
            row.exit_price = closed_trade_exit_price(i);
            row.qty = closed_trade_size(i);
            row.profit = closed_trade_profit(i);
            row.runup = closed_trade_max_runup(i);
            row.drawdown = closed_trade_max_drawdown(i);
            row.entry_bar = closed_trade_entry_bar_index(i);
            row.exit_bar = closed_trade_exit_bar_index(i);
            row.entry_id = closed_trade_entry_id(i);
            row.exit_comment = closed_trade_exit_comment(i);
            run.closed.push_back(row);
        }
        run.open_count = position_entry_count_;
        if (run.open_count > 0) {
            run.open_id = open_trade_entry_id(0);
            run.open_entry_ms = open_trade_entry_time(0);
            run.open_entry_bar = open_trade_entry_bar_index(0);
        }
        return run;
    }

    std::vector<Held> held_;
};

enum class Path { Chart, Aggregated, Magnified };
const Path kPaths[] = {Path::Chart, Path::Aggregated, Path::Magnified};

const char* path_name(Path path) {
    switch (path) {
        case Path::Chart: return "chart 15 -> 15 (control)";
        case Path::Aggregated: return "aggregated 1 -> 15";
        case Path::Magnified: return "aggregated 1 -> 15, magnifier";
    }
    return "?";
}

template <std::size_t N>
std::vector<Bar> feed(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& row : rows) {
        Bar b{};
        b.timestamp = row.ts;
        b.open = row.open; b.high = row.high; b.low = row.low; b.close = row.close;
        b.volume = row.volume;
        bars.push_back(b);
    }
    return bars;
}

void run_path(F1eHost& host, Path path) {
    static const std::vector<Bar> fifteen = feed(kEth15);
    static const std::vector<Bar> one = feed(kEth1m);
    if (path == Path::Chart) {
        host.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
    } else {
        host.run(one.data(), static_cast<int>(one.size()), "1", "15", path == Path::Magnified);
    }
}

// The chart bar a UTC bar open is, counted from the fixture's first 15m bar
// (the fixture has no hole, so bar k opens k * 15 minutes after it).
int chart_bar_of(std::int64_t ms) {
    return static_cast<int>((ms - kEth15[0].ts) / (15 * kMinute));
}

std::string hhmm(std::int64_t ms) {
    const std::int64_t minutes = ms / kMinute;
    char text[16];
    std::snprintf(text, sizeof(text), "%02lld:%02lld",
                  static_cast<long long>(minutes % 1440 / 60),
                  static_cast<long long>(minutes % 60));
    return text;
}

// TradingView prints money to the cent.
bool cents(double engine, double tape) { return std::fabs(engine - tape) < 0.005 + 1e-9; }
bool same_price(double a, double b) { return std::fabs(a - b) <= 1e-9; }

void show(const char* tag, const F1eHost::Run& run) {
    std::printf("    %s: closed=%zu open=%d err='%s'\n", tag, run.closed.size(),
                run.open_count, run.error.c_str());
    for (std::size_t i = 0; i < run.closed.size(); ++i) {
        const F1eHost::Row& r = run.closed[i];
        std::printf("      [%zu] %s %s in %s @%.2f bar %d  out %s @%.2f bar %d  '%s'  dur %d\n", i,
                    r.entry_id.c_str(), r.is_long ? "L" : "S", hhmm(r.entry_ms).c_str(),
                    r.entry_price, r.entry_bar, hhmm(r.exit_ms).c_str(), r.exit_price,
                    r.exit_bar, r.exit_comment.c_str(), r.exit_bar - r.entry_bar);
    }
    if (run.open_count > 0) {
        std::printf("      open %s in %s bar %d\n", run.open_id.c_str(),
                    hhmm(run.open_entry_ms).c_str(), run.open_entry_bar);
    }
}

// ── 1. every tape row, on every path ──────────────────────────────────

void test_tape_rows() {
    const std::vector<TapeTrade> plain = read_tape(kSlug);
    const std::vector<TapeTrade> magnified = read_tape(kSlugMag);
    std::printf("-- %s: %zu TradingView trades; %s: %zu --\n", kSlug, plain.size(), kSlugMag,
                magnified.size());
    CHECK(plain.size() == 4);
    CHECK(magnified.size() == 4);
    if (plain.size() != 4 || magnified.size() != 4) return;
    for (const Path path : kPaths) {
        const bool mag = path == Path::Magnified;
        const std::vector<TapeTrade>& tape = mag ? magnified : plain;
        std::printf("test_tape_rows [%s] vs %s\n", path_name(path), mag ? kSlugMag : kSlug);
        F1eHost host;
        run_path(host, path);
        const F1eHost::Run run = host.read();
        CHECK(run.error.empty());
        CHECK(run.closed.size() == tape.size());
        CHECK(run.open_count == 0);
        bool path_ok = run.error.empty() && run.closed.size() == tape.size()
            && run.open_count == 0;
        const std::size_t rows = std::min(run.closed.size(), tape.size());
        for (std::size_t i = 0; i < rows; ++i) {
            const TapeTrade& tv = tape[i];
            const F1eHost::Row& r = run.closed[i];
            const bool ok = r.is_long == tv.is_long
                && r.entry_ms == tv.entry_ms && r.exit_ms == tv.exit_ms
                && same_price(r.entry_price, tv.entry_price)
                && same_price(r.exit_price, tv.exit_price)
                && r.qty == tv.qty
                && r.entry_id == tv.entry_signal && r.exit_comment == tv.exit_signal
                && cents(r.profit, tv.net_pnl)
                && (mag || (cents(r.runup, tv.favorable) && cents(-r.drawdown, tv.adverse)))
                // "Duration (bars)": the row counts chart bars (design AG1) ...
                && r.exit_bar - r.entry_bar == tv.duration_bars
                // ... and they are the chart bars the tape's instants open.
                && r.entry_bar == chart_bar_of(tv.entry_ms)
                && r.exit_bar == chart_bar_of(tv.exit_ms);
            CHECK(ok);
            if (!ok) {
                path_ok = false;
                std::printf("      row %zu: TradingView %s %s %s @%.2f bar %d -> %s @%.2f bar %d"
                            " '%s' dur %d pnl %.2f fe %.2f ae %.2f\n"
                            "              engine      %s %s %s @%.2f bar %d -> %s @%.2f bar %d"
                            " '%s' dur %d pnl %.4f fe %.4f ae %.4f\n",
                            i + 1, tv.entry_signal.c_str(), tv.is_long ? "L" : "S",
                            hhmm(tv.entry_ms).c_str(), tv.entry_price, chart_bar_of(tv.entry_ms),
                            hhmm(tv.exit_ms).c_str(), tv.exit_price, chart_bar_of(tv.exit_ms),
                            tv.exit_signal.c_str(), tv.duration_bars, tv.net_pnl, tv.favorable,
                            tv.adverse, r.entry_id.c_str(), r.is_long ? "L" : "S",
                            hhmm(r.entry_ms).c_str(), r.entry_price, r.entry_bar,
                            hhmm(r.exit_ms).c_str(), r.exit_price, r.exit_bar,
                            r.exit_comment.c_str(), r.exit_bar - r.entry_bar, r.profit, r.runup,
                            -r.drawdown);
            }
        }
        if (!path_ok) show(path_name(path), run);
    }
}

// ── 2. what the script read: held = bar_index - entry_bar_index(0) ────

// Every callback with a position open reads 1, 2, ... chart bars, the same
// sequence on every path: A 1; B 1..3; C 1..5; D 1..2.
void test_held_sequence() {
    const std::vector<int> expected = {1, 1, 2, 3, 1, 2, 3, 4, 5, 1, 2};
    for (const Path path : kPaths) {
        std::printf("test_held_sequence [%s]\n", path_name(path));
        F1eHost host;
        run_path(host, path);
        std::vector<int> held;
        for (const Held& h : host.held_) held.push_back(h.held);
        CHECK(held == expected);
        if (held != expected) {
            std::printf("      held:");
            for (std::size_t i = 0; i < host.held_.size() && i < 24; ++i)
                std::printf(" %s@%d=%d", host.held_[i].id.c_str(), host.held_[i].bar,
                            host.held_[i].held);
            std::printf("%s\n", host.held_.size() > 24 ? " ..." : "");
        }
    }
}

}  // namespace

int main() {
    test_tape_rows();
    test_held_sequence();
    std::printf("\n%s aggregated entry_bar_index tape: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
