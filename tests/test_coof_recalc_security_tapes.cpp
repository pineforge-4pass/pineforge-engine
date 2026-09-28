/*
 * test_coof_recalc_security_tapes.cpp -- lane INT28-FIX, rule RC.
 *
 * TradingView runs a calc_on_order_fills recalculation of a historical bar on
 * the whole bar: the recalculation of an entry's open fill reads the bar's
 * close, high and low, and a request.security on a timeframe finer than the
 * chart reads the requested bar that closes with the chart bar, as the bar's
 * close calculation does. The probe (tests/fixtures/coof_recalc_security,
 * int28fix-rc: one lab tv export on BINANCE:ETHUSDT.P 15) closes each entry
 * in its open fill's recalculation with a comment naming what that
 * recalculation read; both closes fill at the entry's open, so each exit row
 * of the tape carries the read-out of one open fill's recalculation.
 *
 * The engine fed a bar's slice of the auxiliary 1m feed at the bar's close
 * callback, after the bar's fill recalculations, so the recalculation of the
 * open fill read the previous chart bar's 5m close (the fill's own open).
 *
 * The port registers the probe's 5m site over the corpus 1m bars as the
 * auxiliary feed (bars.inc), as the verifier's split-feed route runs it. The
 * probe's 5m RSI and 60m close are left out: the RSI reads TradingView's whole
 * 5m history, and the 60m bar the first cell reads closes before the replayed
 * bars.
 *
 * Fail-before: see the lane report (both read-outs name the previous bar's
 * close as the 5m close).
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
#error "the recalculation read-out tape needs the auxiliary security feed"
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

// What one recalculation read, in ticks: the chart bar's close, high and low
// and the 5m close.
struct Readout {
    long long close = -1, high = -1, low = -1, s5 = -1;
    bool operator==(const Readout& o) const {
        return close == o.close && high == o.high && low == o.low && s5 == o.s5;
    }
};

double field(const std::string& comment, const std::string& key) {
    const std::string tag = key + "=";
    std::istringstream words(comment);
    std::string word;
    while (words >> word)
        if (word.rfind(tag, 0) == 0) return std::stod(word.substr(tag.size()));
    return kNaN;
}

struct TapeExit {
    std::int64_t entry_ms = -1, exit_ms = -1;
    long long entry_ticks = -1, exit_ticks = -1;
    Readout read;
};

// entry signal -> its trade, with the read-out its exit's comment carries.
std::map<std::string, TapeExit> load_tape() {
    std::ifstream in(std::string(PINEFORGE_COOF_RECALC_SECURITY_FIXTURE_DIR)
                     + "/int28fix-rc/tv_trades.csv");
    std::map<int, std::pair<std::string, TapeExit>> by_number;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string value;
        while (std::getline(fields, value, ',')) cell.push_back(value);
        if (cell.size() < 5) continue;
        auto& [signal, t] = by_number[std::stoi(cell[0])];
        if (cell[1].rfind("Entry", 0) == 0) {
            signal = cell[3];
            t.entry_ms = tape_ms(cell[2]);
            t.entry_ticks = ticks(std::stod(cell[4]));
        } else {
            t.exit_ms = tape_ms(cell[2]);
            t.exit_ticks = ticks(std::stod(cell[4]));
            t.read = {ticks(field(cell[3], "c")), ticks(field(cell[3], "h")),
                      ticks(field(cell[3], "l")), ticks(field(cell[3], "s5"))};
        }
    }
    std::map<std::string, TapeExit> out;
    for (const auto& [number, row] : by_number) out[row.first] = row.second;
    return out;
}

// The probe (fixtures/.../int28fix-rc/strategy.pine), its 5m site as the
// generated strategy registers and evaluates it.
class RecalcProbe final : public source::PineStrategyHost {
public:
    struct Read {
        std::int64_t time;
        Readout read;
    };
    std::vector<Read> reads;

    RecalcProbe() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        cfg.initial_capital = 100000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        configure_pine_strategy(cfg);
        set_syminfo_metadata("qty_step", 0.0001);
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "5", input_tf_, false, false);  // close
    }

    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        if (sec_id == 0) s5_ = bar.close;
    }

    void clear_security(int sec_id) override {
        if (sec_id == 0) s5_ = kNaN;
    }

    void on_source_bar(const Bar&) override {
        const std::int64_t t = current_bar_.timestamp;
        if (t == at(0, 15)) strategy_entry("RA", true, kNaN, kNaN, kNaN, "RA");
        if (t == at(2, 15)) strategy_entry("RB", false, kNaN, kNaN, kNaN, "RB");
        if (signed_position_size() != 0.0) {
            reads.push_back({t, {ticks(current_bar_.close), ticks(current_bar_.high),
                                 ticks(current_bar_.low), ticks(s5_)}});
            strategy_close("", "read-out", kNaN, kNaN, false);
        }
    }

private:
    double s5_ = kNaN;
};

void show(const char* tag, const Readout& r) {
    std::printf("    %-7s c=%lld h=%lld l=%lld s5=%lld (ticks)\n", tag, r.close, r.high,
                r.low, r.s5);
}

}  // namespace

int main() {
    const auto tape = load_tape();
    CHECK(tape.size() == 2);
    const auto chart = to_bars(kEthRc15, std::size(kEthRc15));
    const auto aux = to_bars(kEthRc1, std::size(kEthRc1));

    RecalcProbe probe;
    const auto handle = static_cast<pf_strategy_t>(&probe);
    CHECK(strategy_set_aux_security_feed(
              handle, reinterpret_cast<const pf_bar_t*>(aux.data()),
              static_cast<int>(aux.size()), "1") == 0);
    probe.set_trade_start_time(chart.front().timestamp);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15", false);
    CHECK(probe.last_error().empty());

    std::printf("-- every trade is the tape's: closed at its entry's open\n");
    CHECK(probe.trade_count() == static_cast<int>(tape.size()));
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& t = probe.get_trade(i);
        const auto it = tape.find(t.entry_id);
        CHECK(it != tape.end());
        if (it == tape.end()) continue;
        CHECK(t.entry_time == it->second.entry_ms && t.exit_time == it->second.exit_ms);
        CHECK(ticks(t.entry_price) == it->second.entry_ticks);
        CHECK(ticks(t.exit_price) == it->second.exit_ticks);
    }

    std::printf("-- the open fill's recalculation reads the whole bar, its 5m close too\n");
    // Each entry's position is held by one callback only: its open fill's
    // recalculation, which closes it.
    CHECK(probe.reads.size() == tape.size());
    for (const char* signal : {"RA", "RB"}) {
        const auto it = tape.find(signal);
        CHECK(it != tape.end());
        if (it == tape.end()) continue;
        const TapeExit& tv = it->second;
        bool found = false;
        for (const auto& r : probe.reads) {
            if (r.time != tv.entry_ms) continue;
            found = true;
            CHECK(r.read == tv.read);
            if (!(r.read == tv.read)) {
                std::printf("  %s\n", signal);
                show("tape", tv.read);
                show("engine", r.read);
            }
            // The 5m bar read is the one closing with the chart bar.
            CHECK(tv.read.s5 == tv.read.close);
        }
        CHECK(found);
    }

    std::printf("\n%s calc_on_order_fills recalculation security tape: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
