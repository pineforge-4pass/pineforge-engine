/*
 * test_trail_activation_tick_reach.cpp — R5 follow-up lane E9.
 *
 * Lane E5 left one of its ten tapes failing (its report, "(f) Findings" 1):
 * the ONE-SHOT control of its ETH probes, a strategy.exit with trail_offset 0
 * whose trail_price is SUB-TICK — the entry fill + 0.004 on a 0.01 tick
 * (tests/fixtures/trail_activation_tick_reach, `lab tv`, ws-report-v1,
 * rangeProof covered; README.md there lists the tapes). TradingView exits 5 of
 * the 8 trades on the first bar whose path reaches the level, at the tick PAST
 * it (fill 2550.85, level 2550.854, exit @2550.86; 2500.40, 2450.34, 4769.74,
 * 3310.01 alike); the other 3 never reach it and close at the timeout. The
 * short twin (fill - 0.004, the entries of lane E5's ETH shorts) exits 4 of 8
 * at fill - 0.01 (1791.82, 2589.99, 4463.40, 2155.56).
 *
 * The adapter rests that one-shot as a generic Limit at the half-tick boundary
 * where the tick-quantized path first reaches the level
 * (source_trigger_threshold: 2550.855, the per-kind arm lanes R7 and E5 pin),
 * but it booked the crossing at the level snapped TOWARD the position — the
 * stop-style directional snap, 2550.85 — a price that limit forbids. The
 * kernel refused the terms (MatchRejectedEvent, InvalidTerms: the generic
 * limit-or-better check of NativeExecutionConsumer::consume_matched_request)
 * and the exit never happened: those trades closed at the timeout.
 *
 * Ruling: a one-shot trail's activation is reached on the quantized path, so
 * the price it books is the tick that path first reaches — the activation
 * rounded AWAY from the position (a long's sell exit up, a short's buy exit
 * down), the grid point its resting threshold stands for. On the grid that is
 * the activation itself, which is why no on-grid pin moves.
 *
 * E5's other open question: does a PLACEMENT close that sits inside the
 * activation's tick cell count as already reached? NYSE:F 15m (sub-cent
 * prints), seven longs and one short with trail_points n: the close of the bar
 * whose open filled the entry — where strategy.exit is issued — is half a tick
 * short of the activation and quantizes onto it (11.295 -> 11.30, 11.575 ->
 * 11.58, 13.055 -> 13.06, 14.135 -> 14.14, 13.705 -> 13.71, 13.385 -> 13.39,
 * 11.565 -> 11.57; short 14.415 -> 14.41), and the next bar never reaches the
 * activation's half-tick boundary on its raw path. TradingView exits every
 * trade on that next bar, at its open, with trail_offset 1 and 0 alike (the
 * tapes are identical): the placement close is one more print of the
 * quantized path, so it has reached the activation, and the trail's running
 * best starts at the activation (offset 1 books activation -/+ 1 tick, not
 * close -/+ 1 tick). The adapter read that close raw, left the trail dormant
 * and exited one to twenty bars later.
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

#ifndef PINEFORGE_E9_FIXTURE_DIR
#error "PINEFORGE_E9_FIXTURE_DIR must name tests/fixtures/trail_activation_tick_reach"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

// The ETH entries lane E5 exported its offset-trail tapes over; the one-shot
// tapes ran on the same eight per side.
#include "fixtures/offset_trail_arm/bars.inc"
// The NYSE:F placement-cell entries.
#include "fixtures/trail_activation_tick_reach/bars.inc"

// One tape of the fixture: its side, its strategy.exit arguments and the
// entries it was run over (bars.inc rows).
struct Probe {
    const char* slug;
    bool is_long;
    double mintick;
    double qty;
    int timeout;                 // bars after the entry bar, then strategy.close
    const double* trail_points;  // per entry; nullptr: absent
    double trail_offset;
    double trail_delta;          // trail_price = entry fill + delta; NaN: absent
    const FeedBar* bars;
    int entries;
    int bars_per_entry;
};

constexpr double kFordLongCellPoints[] = {3.0, 3.0, 1.0, 1.0, 5.0, 4.0, 3.0};
constexpr double kFordShortCellPoints[] = {2.0};

const Probe kProbes[] = {
    {"e5-eth-long-oneshot-p004", true, 0.01, 1.0, 16, nullptr, 0.0, 0.004,
     &kEthLong[0][0], 8, 19},
    {"e9-eth-short-oneshot-m004", false, 0.01, 1.0, 16, nullptr, 0.0, -0.004,
     &kEthShort[0][0], 8, 19},
    {"e9-f-long-cell-off1", true, 0.01, 100.0, 20, kFordLongCellPoints, 1.0, kNaN,
     &kFordLongCell[0][0], 7, 23},
    {"e9-f-long-cell-oneshot", true, 0.01, 100.0, 20, kFordLongCellPoints, 0.0, kNaN,
     &kFordLongCell[0][0], 7, 23},
    {"e9-f-short-cell-off1", false, 0.01, 100.0, 20, kFordShortCellPoints, 1.0, kNaN,
     &kFordShortCell[0][0], 1, 23},
    {"e9-f-short-cell-oneshot", false, 0.01, 100.0, 20, kFordShortCellPoints, 0.0, kNaN,
     &kFordShortCell[0][0], 1, 23},
};

// ── the tape ──────────────────────────────────────────────────────────

struct TapeTrade {
    std::int64_t entry_ms = 0;
    double entry_price = kNaN;
    std::int64_t exit_ms = 0;
    double exit_price = kNaN;
    std::string exit_signal;
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
    std::ifstream in(std::string(PINEFORGE_E9_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
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
        if (!entry) t.exit_signal = cell[3];
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

double trail_points_of(const Probe& probe, int entry) {
    return probe.trail_points ? probe.trail_points[entry] : kNaN;
}

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

// TradingView's tick of a print: floor(p / mintick + 0.5).
double tick_of(double price, double mintick) {
    return std::floor(price / mintick + 0.5) * mintick;
}

// The tick the quantized path first reaches at a level: the level itself on
// the grid, else the grid point past it away from the position.
double reach_tick(double level, double mintick, bool is_long) {
    const double nearest = tick_of(level, mintick);
    if (same_price(nearest, level)) return nearest;
    if (is_long) return nearest > level ? nearest : nearest + mintick;
    return nearest < level ? nearest : nearest - mintick;
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

// What the kernel recorded for the one-shot exit: the Limit it rests as, a
// refused match with the terms it was offered, and the fill it booked.
struct OneShotRecord {
    std::optional<native_order::Limit> limit;
    std::optional<native_order::MatchRejectReason> rejected;
    std::optional<double> rejected_price;
    std::optional<double> filled_price;
    std::optional<std::int64_t> filled_bar_ms;
};

OneShotRecord one_shot_record(const TapeHost& host, const char* exit_label) {
    OneShotRecord record;
    std::optional<native_order::RequestHandle> exit;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
            if (accepted->request().label != exit_label) continue;
            if (const auto* l = std::get_if<native_order::Limit>(&accepted->request().trigger)) {
                exit = accepted->handle();
                record.limit = *l;
            }
        } else if (const auto* refused =
                       std::get_if<native_order::MatchRejectedEvent>(&*event.command)) {
            if (exit && refused->handle() == *exit) {
                record.rejected = refused->reason;
                if (refused->attempted_terms)
                    record.rejected_price = refused->attempted_terms->resolved_price;
            }
        } else if (const auto* applied =
                       std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
            if (exit && applied->handle() == *exit && applied->closed_units != 0.0) {
                record.filled_price = applied->resolved_price;
                record.filled_bar_ms = applied->cursor.point.open_ms;
            }
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
            TapeHost host(probe, trail_points_of(probe, e));
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

// ── 2. the kernel's record: the resting limit and the booked price ────

// The one-shot rests as a plain Limit at the outermost price whose tick is the
// tick the quantized path must reach (the half-tick boundary, unchanged by
// this lane). Every exit TradingView books through the trail is that tick, on
// TradingView's exit bar, and the kernel accepts it because it lies on the
// limit's own side of the boundary; no match is refused.
void test_booked_price_is_the_reach_tick() {
    for (const Probe& probe : kProbes) {
        if (std::isnan(probe.trail_delta)) continue;
        std::printf("-- %s: the booked price is the tick past the sub-tick level --\n",
                    probe.slug);
        const auto tape = read_tape(probe.slug);
        if (static_cast<int>(tape.size()) != probe.entries) { CHECK(false); continue; }
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            TapeHost host(probe, trail_points_of(probe, e));
            host.run(bars.data(), static_cast<int>(bars.size()));
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            const double tick = probe.mintick;
            const double level = tv.entry_price + probe.trail_delta;
            const double reach = reach_tick(level, tick, probe.is_long);
            // The setup: a level strictly inside a tick cell.
            CHECK(!same_price(tick_of(level, tick), level));
            CHECK(probe.is_long ? reach > level : reach < level);
            const OneShotRecord record = one_shot_record(host, probe.is_long ? "LX" : "SX");
            CHECK(record.limit.has_value());
            if (!record.limit) continue;
            const double limit = record.limit->price;
            const double outward = std::nextafter(
                limit, probe.is_long ? -std::numeric_limits<double>::infinity()
                                     : std::numeric_limits<double>::infinity());
            CHECK(!record.limit->fill_through);
            CHECK(same_price(tick_of(limit, tick), reach));
            CHECK(!same_price(tick_of(outward, tick), reach));
            CHECK(!record.rejected.has_value());
            if (record.rejected) {
                std::printf("        entry %d: match refused (reason %d), offered @%.17g against "
                            "the resting limit %.17g\n",
                            e + 1, static_cast<int>(*record.rejected),
                            record.rejected_price ? *record.rejected_price : kNaN, limit);
            }
            const bool trail_exit = tv.exit_signal == (probe.is_long ? "LX" : "SX");
            if (!trail_exit) {
                // Never reached inside the timeout: no fill of the one-shot.
                CHECK(!record.filled_price.has_value());
                continue;
            }
            CHECK(same_price(tv.exit_price, reach));
            CHECK(record.filled_price.has_value());
            if (!record.filled_price) continue;
            CHECK(same_price(*record.filled_price, reach));
            CHECK(probe.is_long ? *record.filled_price >= limit : *record.filled_price <= limit);
            CHECK(record.filled_bar_ms && *record.filled_bar_ms == tv.exit_ms);
        }
    }
}

// ── 3. a placement close inside the activation's tick cell has reached it ──

// What the kernel recorded for the exit's legs: a resting one-shot Limit, a
// generic Trail and its arm threshold, and the bar a leg closed the trade on.
struct PlacementRecord {
    bool one_shot_limit = false;
    bool trail = false;
    std::optional<double> trail_arm;
    std::optional<std::int64_t> close_bar_ms;
};

PlacementRecord placement_record(const TapeHost& host, const char* exit_label) {
    PlacementRecord record;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
            if (accepted->request().label != exit_label) continue;
            if (std::holds_alternative<native_order::Limit>(accepted->request().trigger))
                record.one_shot_limit = true;
            if (const auto* t = std::get_if<native_order::Trail>(&accepted->request().trigger)) {
                record.trail = true;
                record.trail_arm = t->arm_price;
            }
        } else if (const auto* applied =
                       std::get_if<native_order::ExecutionAppliedEvent>(&*event.command)) {
            if (applied->request().label == exit_label && applied->closed_units != 0.0)
                record.close_bar_ms = applied->cursor.point.open_ms;
        }
    }
    return record;
}

// On every NYSE:F entry the placement close — the entry bar's close, where
// strategy.exit is issued — stops half a tick short of the activation and its
// tick IS the activation, while the next bar never reaches the activation's
// half-tick boundary on its raw path; TradingView exits on that next bar, at
// its open, and with trail_offset 1 at activation -/+ 1 tick. So the adapter
// arms the trail at placement: the generic Trail is accepted with no arm
// threshold (the kernel arms it at its first print), with or without an
// offset, instead of a dormant one-shot Limit or an arm the next bar never
// reaches, and an exit leg closes the trade on that next bar.
void test_placement_close_in_the_cell_has_reached() {
    for (const Probe& probe : kProbes) {
        if (!probe.trail_points) continue;
        std::printf("-- %s: the placement close's tick reaches the activation --\n",
                    probe.slug);
        const auto tape = read_tape(probe.slug);
        if (static_cast<int>(tape.size()) != probe.entries) { CHECK(false); continue; }
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            TapeHost host(probe, probe.trail_points[e]);
            host.run(bars.data(), static_cast<int>(bars.size()));
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            const double tick = probe.mintick;
            const double activation = tv.entry_price
                + (probe.is_long ? 1.0 : -1.0) * probe.trail_points[e] * tick;
            // bars[0] places the entry, bars[1] fills it at its open and issues
            // the exit at its close, bars[2] is the next bar.
            const double placement = bars[1].close;
            const Bar& next = bars[2];
            CHECK(bars[1].timestamp == tv.entry_ms);
            CHECK(probe.is_long ? placement < activation : placement > activation);
            CHECK(same_price(tick_of(placement, tick), activation));
            const double next_tick = tick_of(probe.is_long ? next.high : next.low, tick);
            CHECK(probe.is_long ? next_tick < activation - 0.5 * tick
                                : next_tick > activation + 0.5 * tick);
            CHECK(tv.exit_ms == next.timestamp);
            CHECK(same_price(tv.exit_price, tick_of(next.open, tick)));
            if (probe.trail_offset >= 1.0) {
                CHECK(same_price(tv.exit_price, activation
                    + (probe.is_long ? -1.0 : 1.0) * probe.trail_offset * tick));
            }
            const PlacementRecord record = placement_record(host, probe.is_long ? "LX" : "SX");
            CHECK(!record.one_shot_limit);
            CHECK(record.trail);
            CHECK(!record.trail_arm.has_value());
            CHECK(record.close_bar_ms && *record.close_bar_ms == next.timestamp);
            if (record.one_shot_limit || record.trail_arm) {
                std::printf("        entry %d: placement %.10g short of activation %.10g left %s\n",
                            e + 1, placement, activation,
                            record.one_shot_limit ? "a resting one-shot Limit"
                                                  : "a Trail waiting for its arm");
            }
        }
    }
}

}  // namespace

int main() {
    test_tapes_replay();
    test_booked_price_is_the_reach_tick();
    test_placement_close_in_the_cell_has_reached();
    std::printf("\n%s trail activation tick reach: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
