/*
 * test_offset_trail_quantized_arm.cpp — R5 follow-up lane E5.
 *
 * Lane P9 left one question open (its report, "Rulings" 2): does a trail WITH
 * a trailing offset arm on the raw price path or on the tick-quantized one?
 * The deleted exit-path resolver quantized it; the product armed the generic
 * Trail at the raw activation. TradingView's own tapes settle it
 * (tests/fixtures/offset_trail_arm/, `lab tv`, ws-report-v1, rangeProof
 * covered; README.md there lists them):
 *
 *   NYSE:F 15m, mintick 0.01, sub-cent prints. One strategy.exit per entry,
 *   trail_points n (the activation is the entry fill -/+ n ticks, on the
 *   grid), trail_offset 1. On the bar TradingView exits on, the raw extreme
 *   stops inside the activation's tick cell and only its tick-quantized value
 *   reaches the activation:
 *     e5-f-short-arm-qlow   lows 9.415 / 13.041 / 12.641 -> activations
 *                           9.41 / 13.04 / 12.64: exits @9.42 / 13.05 / 12.65
 *     e5-f-long-arm-qhigh   highs 11.899 / 13.049 / 13.419 -> 11.90 / 13.05 /
 *                           13.42: exits @11.89 / 13.04 / 13.41
 *   6 of 6 exit on that bar, so the arm is tested on the QUANTIZED path, like
 *   the one-shot's (e5-f-*-oneshot-*, trail_offset 0: the same six bars, at
 *   the activation, the rule test_trail_activation_tick_bar pins). Every
 *   booked exit is activation -/+ one tick although the raw extreme sits
 *   inside the cell: the running best starts at the activation.
 *
 *   BINANCE:ETHUSDT.P 15m, the corpus feed (on the grid). trail_price = entry
 *   fill + 0.004 / + 0.006 (longs), - 0.004 / - 0.006 (shorts), and
 *   trail_points 0.4, each with trail_offset 1, over 8 + 8 entries whose
 *   later bar touches the fill exactly: 0 of 32 arm on that bar; each arms on
 *   the first print a whole tick past the fill. The sub-tick LEVEL is not
 *   rounded to the tick; on an on-grid feed the quantized path is the raw one.
 *
 * Ruling: TradingView quantizes a trail's activation per order kind — the arm
 * on the quantized path, the running best and the trail stop on the raw path
 * (docs/design/native-feature-parity.md §3.6.2, B2). It is the Pine adapter's
 * spelling (ADR-0001 "Trail and tick conventions"), not a kernel option: the
 * generic Trail still compares its arm_price with the raw path, and the
 * adapter restates the arm as the half-tick boundary where the quantized path
 * first reaches the activation — the threshold its one-shot trail rests at —
 * and books the exit from a running best that is never short of the
 * activation. The kernel-visible half of this witness reads that arm and the
 * bar it fired on from NativeStrategyHost::native_events.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/native_order.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
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

#ifndef PINEFORGE_E5_FIXTURE_DIR
#error "PINEFORGE_E5_FIXTURE_DIR must name tests/fixtures/offset_trail_arm"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

#include "fixtures/offset_trail_arm/bars.inc"

// One strategy of the fixture: its tape, its side and its strategy.exit
// arguments, and the entries it was run over (bars.inc rows).
struct Probe {
    const char* slug;
    bool is_long;
    double mintick;
    double qty;
    int timeout;              // bars after the entry bar, then strategy.close
    const double* trail_points;   // per entry; nullptr: absent
    double trail_offset;
    double trail_delta;       // trail_price = entry fill + delta; NaN: absent
    const FeedBar* bars;
    int entries;
    int bars_per_entry;
};

constexpr double kFordShortPoints[] = {8.0, 5.0, 2.0};
constexpr double kFordLongPoints[] = {25.0, 2.0, 5.0};
constexpr double kEthPoints04[] = {0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4};

const Probe kProbes[] = {
    {"e5-f-short-arm-qlow", false, 0.01, 100.0, 20, kFordShortPoints, 1.0, kNaN,
     &kFordShort[0][0], 3, 23},
    {"e5-f-long-arm-qhigh", true, 0.01, 100.0, 20, kFordLongPoints, 1.0, kNaN,
     &kFordLong[0][0], 3, 23},
    {"e5-f-short-oneshot-qlow", false, 0.01, 100.0, 20, kFordShortPoints, 0.0, kNaN,
     &kFordShort[0][0], 3, 23},
    {"e5-f-long-oneshot-qhigh", true, 0.01, 100.0, 20, kFordLongPoints, 0.0, kNaN,
     &kFordLong[0][0], 3, 23},
    {"e5-eth-long-arm-p004", true, 0.01, 1.0, 16, nullptr, 1.0, 0.004,
     &kEthLong[0][0], 8, 19},
    {"e5-eth-long-arm-p006", true, 0.01, 1.0, 16, nullptr, 1.0, 0.006,
     &kEthLong[0][0], 8, 19},
    {"e5-eth-long-points-04", true, 0.01, 1.0, 16, kEthPoints04, 1.0, kNaN,
     &kEthLong[0][0], 8, 19},
    {"e5-eth-short-arm-m004", false, 0.01, 1.0, 16, nullptr, 1.0, -0.004,
     &kEthShort[0][0], 8, 19},
    {"e5-eth-short-arm-m006", false, 0.01, 1.0, 16, nullptr, 1.0, -0.006,
     &kEthShort[0][0], 8, 19},
};

// ── the tape ──────────────────────────────────────────────────────────

struct TapeTrade {
    std::int64_t entry_ms = 0;
    double entry_price = kNaN;
    std::int64_t exit_ms = 0;
    double exit_price = kNaN;
};

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

std::vector<TapeTrade> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_E5_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeTrade> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 5) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (trades.size() < n) trades.resize(n);
        TapeTrade& t = trades[n - 1];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        (entry ? t.entry_ms : t.exit_ms) = tape_ms(cell[2]);
        (entry ? t.entry_price : t.exit_price) = std::stod(cell[4]);
    }
    return trades;
}

// ── the strategy, as the probe's Pine source runs it ─────────────────

// Bar 0 places the entry; the exit is issued once, at the close of the bar
// whose open filled it; `timeout` bars later strategy.close takes what is left.
class TapeHost : public pineforge::source::PineStrategyHost {
public:
    TapeHost(const Probe& probe, double trail_points)
        : probe_(probe), trail_points_(trail_points) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = probe.qty;
        config.commission_value = 0.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.slippage = 0;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        syminfo_mintick_ = probe.mintick;
    }

    void on_source_bar(const Bar&) override {
        const char* id = probe_.is_long ? "L" : "S";
        const double units = physical_position().signed_units;
        if (bar_index_ == 0) strategy_entry(id, probe_.is_long);
        if (units != 0.0 && entry_bar_ < 0) {
            entry_bar_ = bar_index_;
            const double trail_price = std::isnan(probe_.trail_delta)
                ? kNaN : position_avg_price() + probe_.trail_delta;
            strategy_exit(probe_.is_long ? "LX" : "SX", id, kNaN, kNaN, trail_points_,
                          probe_.trail_offset, trail_price);
        }
        if (units != 0.0 && entry_bar_ >= 0 && bar_index_ - entry_bar_ >= probe_.timeout)
            strategy_close(id, "timeout");
    }

    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    std::int64_t entry_time(int i) const { return closed_trade_entry_time(i); }
    std::int64_t exit_time(int i) const { return closed_trade_exit_time(i); }

private:
    Probe probe_;
    double trail_points_;
    int entry_bar_ = -1;
};

std::vector<Bar> entry_bars(const Probe& probe, int entry) {
    std::vector<Bar> bars;
    const FeedBar* row = probe.bars + entry * probe.bars_per_entry;
    for (int i = 0; i < probe.bars_per_entry; ++i) {
        Bar b{};
        b.timestamp = row[i].ts;
        b.open = row[i].open; b.high = row[i].high;
        b.low = row[i].low; b.close = row[i].close;
        b.volume = 1000.0;
        bars.push_back(b);
    }
    return bars;
}

bool same_price(double a, double b) { return std::fabs(a - b) <= 1e-9; }

// TradingView's tick of a print: floor(p / mintick + 0.5) (the pins of
// test_stop_tick_rounding: 9.415 -> 9.41, 13.745 -> 13.75).
double tick_of(double price, double mintick) {
    return std::floor(price / mintick + 0.5) * mintick;
}

std::string utc(std::int64_t ms) {
    const std::int64_t minutes = ms / 60'000;
    char text[32];
    std::snprintf(text, sizeof(text), "day %lld %02lld:%02lld",
                  static_cast<long long>(minutes / 1440),
                  static_cast<long long>(minutes % 1440 / 60),
                  static_cast<long long>(minutes % 60));
    return text;
}

// What the kernel recorded for the exit's generic Trail: the arm it was
// accepted with and the bar its TrailArm activation fired on.
struct TrailRecord {
    std::optional<double> arm_price;
    std::optional<std::int64_t> arm_bar_ms;
    std::optional<double> arm_reached;
    std::optional<std::int64_t> close_bar_ms;
};

TrailRecord trail_record(const TapeHost& host) {
    TrailRecord record;
    std::optional<native_order::RequestHandle> trail;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
            if (const auto* t = std::get_if<native_order::Trail>(&accepted->request().trigger)) {
                trail = accepted->handle();
                record.arm_price = t->arm_price;
            }
        } else if (const auto* activated =
                       std::get_if<native_order::ActivatedEvent>(&*event.command)) {
            if (trail && activated->definition->handle == *trail
                && activated->kind == native_order::ActivationKind::TrailArm) {
                record.arm_bar_ms = activated->cursor.point.open_ms;
                record.arm_reached = activated->reached_price;
            }
        } else if (const auto* applied =
                       std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
            if (trail && applied->handle() == *trail && applied->closed_units != 0.0)
                record.close_bar_ms = applied->cursor.point.open_ms;
        }
    }
    return record;
}

// ── 1. every tape trade, replayed ─────────────────────────────────────

void test_tapes_replay() {
    for (const Probe& probe : kProbes) {
        std::printf("-- %s: %d TradingView trades --\n", probe.slug, probe.entries);
        const auto tape = read_tape(probe.slug);
        CHECK(static_cast<int>(tape.size()) == probe.entries);
        if (static_cast<int>(tape.size()) != probe.entries) continue;
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            TapeHost host(probe, probe.trail_points ? probe.trail_points[e] : kNaN);
            host.fixture_retain_all_events();  // read after the run (V19-B)
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() == 1);
            if (host.trade_count() != 1) continue;
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            CHECK(host.entry_time(0) == tv.entry_ms);
            CHECK(same_price(host.entry_price(0), tv.entry_price));
            CHECK(host.exit_time(0) == tv.exit_ms);
            CHECK(same_price(host.exit_price(0), tv.exit_price));
            if (host.exit_time(0) != tv.exit_ms || !same_price(host.exit_price(0), tv.exit_price)) {
                std::printf("        trade %d: engine exit %s @%.10g, TradingView %s @%.10g\n",
                            e + 1, utc(host.exit_time(0)).c_str(), host.exit_price(0),
                            utc(tv.exit_ms).c_str(), tv.exit_price);
            }
        }
    }
}

// ── 2. the kernel's record: the arm and the bar it fired on ───────────

// On every NYSE:F offset-trail entry, the bar TradingView exits on has a raw
// extreme short of the activation whose tick IS the activation. The generic
// Trail is accepted with its arm at the outermost price whose tick is the
// activation (the half-tick boundary, never the activation itself), the raw
// path crosses that arm on the exit bar (the TrailArm activation books the arm
// level) and the trail's close fills on that same bar.
void test_kernel_arm_is_the_quantized_boundary() {
    for (const Probe& probe : kProbes) {
        const bool ford_offset = probe.trail_points && probe.trail_offset >= 1.0
            && probe.bars_per_entry == 23;
        if (!ford_offset) continue;
        std::printf("-- %s: the generic Trail's arm is the activation's half-tick boundary --\n",
                    probe.slug);
        const auto tape = read_tape(probe.slug);
        if (static_cast<int>(tape.size()) != probe.entries) { CHECK(false); continue; }
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            TapeHost host(probe, probe.trail_points[e]);
            host.fixture_retain_all_events();  // read after the run (V19-B)
            host.run(bars.data(), static_cast<int>(bars.size()));
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            const double tick = probe.mintick;
            const double activation = tv.entry_price
                + (probe.is_long ? 1.0 : -1.0) * probe.trail_points[e] * tick;
            const Bar* exit_bar = nullptr;
            for (const Bar& b : bars)
                if (b.timestamp == tv.exit_ms) exit_bar = &b;
            CHECK(exit_bar != nullptr);
            if (!exit_bar) continue;
            // The tape's exit bar: raw short of the activation, quantized onto it.
            const double extreme = probe.is_long ? exit_bar->high : exit_bar->low;
            CHECK(probe.is_long ? extreme < activation : extreme > activation);
            CHECK(same_price(tick_of(extreme, tick), activation));
            const TrailRecord record = trail_record(host);
            CHECK(record.arm_price.has_value());
            if (!record.arm_price) continue;
            const double arm = *record.arm_price;
            const double outward = std::nextafter(
                arm, probe.is_long ? -std::numeric_limits<double>::infinity()
                                   : std::numeric_limits<double>::infinity());
            CHECK(arm != activation);
            CHECK(same_price(tick_of(arm, tick), activation));
            CHECK(!same_price(tick_of(outward, tick), activation));
            CHECK(record.arm_bar_ms && *record.arm_bar_ms == tv.exit_ms);
            CHECK(record.arm_reached && *record.arm_reached == arm);
            CHECK(record.close_bar_ms && *record.close_bar_ms == tv.exit_ms);
            if (!record.arm_bar_ms || *record.arm_bar_ms != tv.exit_ms) {
                std::printf("        entry %d: arm %.17g armed %s, TradingView's exit bar %s\n",
                            e + 1, arm,
                            record.arm_bar_ms ? utc(*record.arm_bar_ms).c_str() : "never",
                            utc(tv.exit_ms).c_str());
            }
        }
    }
}

// On the on-grid ETH feed the sub-tick level is restated the same way — the
// boundary of the tick past the fill, not of the fill's own tick — so the bar
// that touches the fill exactly never arms the trail.
void test_sub_tick_level_is_not_rounded_onto_the_fill() {
    for (const Probe& probe : kProbes) {
        if (probe.bars_per_entry != 19) continue;
        std::printf("-- %s: the arm is past the fill's tick --\n", probe.slug);
        const auto tape = read_tape(probe.slug);
        if (static_cast<int>(tape.size()) != probe.entries) { CHECK(false); continue; }
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            TapeHost host(probe, probe.trail_points ? probe.trail_points[e] : kNaN);
            host.fixture_retain_all_events();  // read after the run (V19-B)
            host.run(bars.data(), static_cast<int>(bars.size()));
            const TrailRecord record = trail_record(host);
            CHECK(record.arm_price.has_value());
            if (!record.arm_price) continue;
            const double fill = tape[static_cast<std::size_t>(e)].entry_price;
            const double next_tick = fill + (probe.is_long ? 1.0 : -1.0) * probe.mintick;
            CHECK(same_price(tick_of(*record.arm_price, probe.mintick), next_tick));
            // The touching bar exists and does not arm: the first activation
            // is a later bar, or none inside the timeout.
            bool touched = false;
            std::int64_t touch_ms = 0;
            for (std::size_t i = 2; i < bars.size() && !touched; ++i) {
                const double extreme = probe.is_long ? bars[i].high : bars[i].low;
                if (same_price(extreme, fill)) { touched = true; touch_ms = bars[i].timestamp; }
            }
            CHECK(touched);
            CHECK(!record.arm_bar_ms || *record.arm_bar_ms > touch_ms);
        }
    }
}

}  // namespace

int main() {
    test_tapes_replay();
    test_kernel_arm_is_the_quantized_boundary();
    test_sub_tick_level_is_not_rounded_onto_the_fill();
    std::printf("\n%s offset trail quantized arm: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
