/*
 * test_coof_recalc_security_tapes.cpp -- lane INT28-FIX, rule RC.
 *
 * TradingView runs a calc_on_order_fills recalculation of a historical bar on
 * the whole bar: a recalculation reads the bar's close, high and low, and a
 * request.security on a timeframe finer than the chart reads the bar's own
 * slice of it, as the bar's close calculation does. Three read-out probes
 * (tests/fixtures/coof_recalc_security, lab tv exports on BINANCE:ETHUSDT.P 15)
 * close a position in a recalculation with a comment naming what that
 * recalculation read, so each read-out exit row of a tape carries one
 * recalculation's view:
 *   int28fix-rc   the open fill's recalculation: close, high, low and the
 *                 lookahead_off 5m close are the bar's own;
 *   int28fix-rc2  the recalculation of a take-profit filled on the bar's path
 *                 (half the position; the read-out closes the rest): the same;
 *   int28fix-rc3  the open fill's recalculation: a lookahead_on 5m site reads
 *                 the bar's first 5m bar, a 5m request.security_lower_tf array
 *                 all three.
 *
 * The engine fed a bar's slice of the auxiliary 1m feed at the bar's close
 * callback, after the bar's fill recalculations, so a recalculation read the
 * previous chart bar's finer values. It now feeds the slice at the bar's first
 * recalculation before the close callback, rebasing the script-state
 * checkpoint on it; a recalculation after the close callback (a fill the close
 * books, strategy.close(immediately = true)) finds the slice that callback
 * fed and feeds nothing (the fourth probe: a double feed would step the 5m
 * site twice, and on the run's last bar would find its routing cleared).
 *
 * The ports register the probes' sites over the corpus 1m bars as the
 * auxiliary feed (bars.inc), as the verifier's split-feed route runs them, and
 * checkpoint the sites' values and the 5m site's completed-bar count the way a
 * generated strategy checkpoints its request.security values for
 * calc_on_order_fills. Every callback is recorded: every callback of a bar
 * must read that bar's own 5m close and one completed 5m bar more than three
 * per earlier chart bar. The probes' 5m RSI and 60m close (int28fix-rc) are
 * left out: the RSI reads TradingView's whole 5m history, and the 60m bar the
 * first cell reads closes before the replayed bars.
 *
 * Fail-before: see the lane report (on the commit before rule RC every
 * recalculation reads the previous bar's slice; with RC fed at every first
 * recalculation, the late-close rows double-feed and the last one fails).
 */

#include <pineforge/pineforge.h>
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
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

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the recalculation read-out tapes need the auxiliary security feed"
#endif
#ifndef PINEFORGE_COOF_RECALC_SECURITY_FIXTURE_DIR
#error "PINEFORGE_COOF_RECALC_SECURITY_FIXTURE_DIR must name tests/fixtures/coof_recalc_security"
#endif

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};

// kEthRc15 (chart) and kEthRc1 (auxiliary): BINANCE:ETHUSDT.P, 2025-04-11.
#include "fixtures/coof_recalc_security/bars.inc"

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr std::int64_t kMinute = 60'000;
constexpr int kS5 = 0;   // request.security("5", close), lookahead_off
constexpr int kOn5 = 1;  // request.security("5", close), lookahead_on
constexpr int kArr = 2;  // request.security_lower_tf("5", close)

long long ticks(double price) { return std::isnan(price) ? -1 : std::llround(price / kTick); }

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::int64_t at(int hour, int minute) {
    return ((days_from_civil(2025, 4, 11) * 24 + hour) * 60 + minute) * kMinute;
}

std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

std::vector<Bar> to_bars(const FeedBar* rows, std::size_t n) {
    std::vector<Bar> out;
    for (std::size_t i = 0; i < n; ++i) {
        Bar b{};
        b.timestamp = rows[i].ts;
        b.open = rows[i].open; b.high = rows[i].high; b.low = rows[i].low;
        b.close = rows[i].close; b.volume = rows[i].volume;
        out.push_back(b);
    }
    return out;
}

// "key=value" out of a read-out comment; NaN when absent or "na".
double field(const std::string& comment, const std::string& key) {
    const std::string tag = key + "=";
    std::istringstream words(comment);
    std::string word;
    while (words >> word) {
        if (word.rfind(tag, 0) != 0) continue;
        const std::string value = word.substr(tag.size());
        return value == "na" ? kNaN : std::stod(value);
    }
    return kNaN;
}

struct TapeTrade {
    std::int64_t entry_ms = -1, exit_ms = -1;
    long long entry_ticks = -1, exit_ticks = -1, qty = -1;
    std::string exit_signal;
};

// The tape's trades in trade-number order.
std::vector<TapeTrade> load_tape(const std::string& tape) {
    std::ifstream in(std::string(PINEFORGE_COOF_RECALC_SECURITY_FIXTURE_DIR) + "/" + tape
                     + "/tv_trades.csv");
    std::map<int, TapeTrade> by_number;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string value;
        while (std::getline(fields, value, ',')) cell.push_back(value);
        if (cell.size() < 6) continue;
        TapeTrade& t = by_number[std::stoi(cell[0])];
        if (cell[1].rfind("Entry", 0) == 0) {
            t.entry_ms = tape_ms(cell[2]);
            t.entry_ticks = ticks(std::stod(cell[4]));
            t.qty = std::llround(std::stod(cell[5]) * 1e4);
        } else {
            t.exit_ms = tape_ms(cell[2]);
            t.exit_ticks = ticks(std::stod(cell[4]));
            t.exit_signal = cell[3];
        }
    }
    std::vector<TapeTrade> out;
    for (const auto& [number, t] : by_number) out.push_back(t);
    return out;
}

enum class Probe { ReadOut, MidBar, Sites, LateClose };

// What one callback read, in ticks (the lower_tf array's size as is).
struct Read {
    std::int64_t time = 0;
    double size = 0.0;
    long long close = -1, high = -1, low = -1, s5 = -1, on5 = -1, a0 = -1, a_last = -1;
    long long completed = 0;
    int n = 0;
};

// The probes (fixtures/.../int28fix-rc*/strategy.pine), their sites as the
// generated strategy registers and evaluates them, and the late-close probe.
class RecalcProbe final : public source::PineStrategyHost {
public:
    std::vector<Read> reads;

    explicit RecalcProbe(Probe probe) : probe_(probe) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = probe == Probe::MidBar ? 2.0 : 1.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 0.0001);
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(kS5, "5", input_tf_, false, false);
        if (probe_ == Probe::Sites) {
            register_security_eval(kOn5, "5", input_tf_, true, false);
            register_security_lower_tf_eval(kArr, "5", input_tf_);
        }
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id == kS5) {
            live_.s5 = bar.close;
            if (is_complete) ++live_.completed;
        } else if (sec_id == kOn5) {
            live_.on5 = bar.close;
        } else if (sec_id == kArr) {
            if (security_lower_tf_sub_bar_index(kArr) == 0) live_.arr.clear();
            live_.arr.push_back(bar.close);
        }
    }

    void clear_security(int sec_id) override {
        if (sec_id == kS5) live_.s5 = kNaN;
        if (sec_id == kOn5) live_.on5 = kNaN;
        if (sec_id == kArr) live_.arr.clear();
    }

    // The state a generated strategy checkpoints for calc_on_order_fills:
    // its request.security values and the state of their contexts.
    void snapshot_script_state() override { saved_ = live_; }
    void restore_script_state() override { live_ = saved_; }
    void commit_script_state() override { saved_ = live_; }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        const double size = signed_position_size();
        Read r;
        r.time = t;
        r.size = size;
        r.close = ticks(current_bar_.close);
        r.high = ticks(current_bar_.high);
        r.low = ticks(current_bar_.low);
        r.s5 = ticks(live_.s5);
        r.on5 = ticks(live_.on5);
        r.completed = live_.completed;
        r.n = static_cast<int>(live_.arr.size());
        r.a0 = live_.arr.empty() ? -1 : ticks(live_.arr.front());
        r.a_last = live_.arr.empty() ? -1 : ticks(live_.arr.back());
        reads.push_back(r);

        switch (probe_) {
        case Probe::ReadOut:
        case Probe::Sites:
            if (t == at(0, 15)) strategy_entry("RA", true, kNaN, kNaN, kNaN, "RA");
            if (t == at(2, 15)) strategy_entry("RB", false, kNaN, kNaN, kNaN, "RB");
            if (size != 0.0) strategy_close("", "read-out", kNaN, kNaN, false);
            break;
        case Probe::MidBar:
            if (t == at(0, 15)) strategy_entry("RA", true, kNaN, kNaN, kNaN, "RA");
            if (t == at(2, 15)) strategy_entry("RB", false, kNaN, kNaN, kNaN, "RB");
            if (size == 2.0)
                strategy_exit("RA-TP", "RA", position_entry_price_ + 1, kNaN, kNaN, kNaN, kNaN,
                              50.0, "RA tp");
            if (size == -2.0)
                strategy_exit("RB-TP", "RB", position_entry_price_ - 1, kNaN, kNaN, kNaN, kNaN,
                              50.0, "RB tp");
            if (std::fabs(size) == 1.0) strategy_close("", "read-out", kNaN, kNaN, false);
            if (t == at(1, 0) || t == at(3, 0)) {
                strategy_cancel_all();
                strategy_close("", "cleanup", kNaN, kNaN, false);
            }
            break;
        case Probe::LateClose:
            // Each entry is closed at the close of a later bar that fills
            // nothing before it, immediately: that fill's recalculation runs
            // after the bar's close callback. The second is the run's last bar.
            if (t == at(0, 15)) strategy_entry("LA", true, kNaN, kNaN, kNaN, "LA");
            if (t == at(1, 0) && size > 0.0) strategy_close("", "late", kNaN, kNaN, true);
            if (t == at(2, 30)) strategy_entry("LB", false, kNaN, kNaN, kNaN, "LB");
            if (t == at(3, 0) && size < 0.0) strategy_close("", "late", kNaN, kNaN, true);
            break;
        }
    }

private:
    struct State {
        double s5 = kNaN, on5 = kNaN;
        std::vector<double> arr;
        long long completed = 0;
    };
    Probe probe_;
    State live_, saved_;
};

struct Run {
    std::vector<Read> reads;
    std::vector<Trade> trades;
    std::string error;
};

Run run(Probe probe) {
    const auto chart = to_bars(kEthRc15, std::size(kEthRc15));
    const auto aux = to_bars(kEthRc1, std::size(kEthRc1));
    RecalcProbe host(probe);
    const auto handle = static_cast<pf_strategy_t>(&host);
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
    host.set_trade_start_time(chart.front().timestamp);
    host.run(chart.data(), static_cast<int>(chart.size()), "15", "15", false);
    Run out;
    out.reads = host.reads;
    out.error = host.last_error();
    for (int i = 0; i < host.trade_count(); ++i) out.trades.push_back(host.get_trade(i));
    return out;
}

// Every callback of chart bar i reads that bar's own slice, fed once: its 5m
// close is the bar's close (the chart bars are the 1m bars' aggregate) and the
// 5m site completed three bars per chart bar through bar i.
int check_reads_whole_bar(const char* name, const std::vector<Read>& reads) {
    int checked = 0;
    for (const Read& r : reads) {
        const std::int64_t index = (r.time - kEthRc15[0].ts) / (15 * kMinute);
        const bool own_close = r.s5 == r.close;
        const bool fed_once = r.completed == 3 * (index + 1);
        if (!own_close || !fed_once) {
            std::printf("  %s: bar %lld (size %g) reads 5m close %lld, bar close %lld, "
                        "5m bars completed %lld, expected %lld\n",
                        name, static_cast<long long>(index), r.size, r.s5, r.close,
                        r.completed, static_cast<long long>(3 * (index + 1)));
        }
        CHECK(own_close);
        CHECK(fed_once);
        ++checked;
    }
    return checked;
}

// The engine's trades are the tape's, bar, price and quantity.
void check_trades(const char* tape_name, const std::vector<TapeTrade>& tape,
                  const std::vector<Trade>& trades) {
    CHECK(trades.size() == tape.size());
    for (std::size_t i = 0; i < trades.size() && i < tape.size(); ++i) {
        const Trade& e = trades[i];
        const TapeTrade& t = tape[i];
        const bool same = e.entry_time == t.entry_ms && e.exit_time == t.exit_ms
            && ticks(e.entry_price) == t.entry_ticks && ticks(e.exit_price) == t.exit_ticks
            && std::llround(e.qty * 1e4) == t.qty;
        if (!same) {
            std::printf("  %s trade %zu: engine %lld@%lld -> %lld@%lld q%lld, tape "
                        "%lld@%lld -> %lld@%lld q%lld\n",
                        tape_name, i, static_cast<long long>(e.entry_time),
                        ticks(e.entry_price), static_cast<long long>(e.exit_time),
                        ticks(e.exit_price), std::llround(e.qty * 1e4),
                        static_cast<long long>(t.entry_ms), t.entry_ticks,
                        static_cast<long long>(t.exit_ms), t.exit_ticks, t.qty);
        }
        CHECK(same);
    }
}

// The read of the callback that placed a read-out exit: the one on the exit's
// bar holding `held` units of the position.
const Read* read_at(const std::vector<Read>& reads, std::int64_t time, double held) {
    for (const Read& r : reads)
        if (r.time == time && std::fabs(r.size) == held) return &r;
    return nullptr;
}

void test_open_fill_readout() {
    std::printf("-- int28fix-rc: the open fill's recalculation reads the whole bar\n");
    const auto tape = load_tape("int28fix-rc");
    CHECK(tape.size() == 2);
    const Run r = run(Probe::ReadOut);
    CHECK(r.error.empty());
    check_trades("int28fix-rc", tape, r.trades);
    for (const TapeTrade& t : tape) {
        const Read* e = read_at(r.reads, t.entry_ms, 1.0);
        CHECK(e != nullptr);
        if (e == nullptr) continue;
        CHECK(e->close == ticks(field(t.exit_signal, "c")));
        CHECK(e->high == ticks(field(t.exit_signal, "h")));
        CHECK(e->low == ticks(field(t.exit_signal, "l")));
        CHECK(e->s5 == ticks(field(t.exit_signal, "s5")));
    }
    CHECK(check_reads_whole_bar("int28fix-rc", r.reads) > 0);
}

void test_mid_bar_fill_readout() {
    std::printf("-- int28fix-rc2: a take-profit fill's recalculation reads the whole bar\n");
    const auto tape = load_tape("int28fix-rc2");
    CHECK(tape.size() == 4);
    const Run r = run(Probe::MidBar);
    CHECK(r.error.empty());
    check_trades("int28fix-rc2", tape, r.trades);
    int readouts = 0;
    for (const TapeTrade& t : tape) {
        if (t.exit_signal.find("c=") == std::string::npos) continue;
        ++readouts;
        const Read* e = read_at(r.reads, t.exit_ms, 1.0);
        CHECK(e != nullptr);
        if (e == nullptr) continue;
        CHECK(e->close == ticks(field(t.exit_signal, "c")));
        CHECK(e->high == ticks(field(t.exit_signal, "h")));
        CHECK(e->low == ticks(field(t.exit_signal, "l")));
        CHECK(e->s5 == ticks(field(t.exit_signal, "s5")));
    }
    CHECK(readouts == 2);
    CHECK(check_reads_whole_bar("int28fix-rc2", r.reads) > 0);
}

void test_site_kinds_readout() {
    std::printf("-- int28fix-rc3: a lookahead_on site and a lower_tf array read the bar's own slice\n");
    const auto tape = load_tape("int28fix-rc3");
    CHECK(tape.size() == 2);
    const Run r = run(Probe::Sites);
    CHECK(r.error.empty());
    check_trades("int28fix-rc3", tape, r.trades);
    for (const TapeTrade& t : tape) {
        const Read* e = read_at(r.reads, t.entry_ms, 1.0);
        CHECK(e != nullptr);
        if (e == nullptr) continue;
        const bool same = e->on5 == ticks(field(t.exit_signal, "on5"))
            && e->n == static_cast<int>(field(t.exit_signal, "n"))
            && e->a0 == ticks(field(t.exit_signal, "a0"))
            && e->a_last == ticks(field(t.exit_signal, "aL"));
        if (!same) {
            std::printf("  int28fix-rc3 %lld: engine on5=%lld n=%d a0=%lld aL=%lld, tape %s\n",
                        static_cast<long long>(t.entry_ms), e->on5, e->n, e->a0, e->a_last,
                        t.exit_signal.c_str());
        }
        CHECK(same);
    }
    CHECK(check_reads_whole_bar("int28fix-rc3", r.reads) > 0);
}

void test_recalculation_after_the_close_feeds_nothing() {
    std::printf("-- a fill the close callback books recalculates on the slice that callback fed\n");
    const Run r = run(Probe::LateClose);
    CHECK(r.error.empty());
    if (!r.error.empty()) std::printf("  run error: %s\n", r.error.c_str());
    // Both immediate closes filled on their own bar, and each fill ran a
    // recalculation after the bar's close callback (a second callback there).
    CHECK(r.trades.size() == 2);
    if (r.trades.size() == 2) {
        CHECK(r.trades[0].exit_time == at(1, 0));
        CHECK(r.trades[1].exit_time == at(3, 0));
    }
    for (const std::int64_t bar : {at(1, 0), at(3, 0)}) {
        int callbacks = 0;
        for (const Read& read : r.reads) callbacks += read.time == bar ? 1 : 0;
        CHECK(callbacks == 2);
    }
    CHECK(check_reads_whole_bar("late-close", r.reads) > 0);
}

}  // namespace

int main() {
    test_open_fill_readout();
    test_mid_bar_fill_readout();
    test_site_kinds_readout();
    test_recalculation_after_the_close_feeds_nothing();
    std::printf("\n%s calc_on_order_fills recalculation security tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
