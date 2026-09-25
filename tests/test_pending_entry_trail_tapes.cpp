/*
 * test_pending_entry_trail_tapes.cpp — H-MEASURE (rows E5 pending-entry arm
 * and E14 anchored-leg seed).
 *
 * Every tape here issues strategy.exit(trail_points=n, trail_offset=k) on the
 * SIGNAL bar, together with the MARKET strategy.entry it names, so the exit
 * is placed while its entry is still pending; the entry fills at the next
 * bar's open. The Pine adapter queues such an exit and submits its leg at the
 * parent's fill, where exit() builds the Trail with its arm threshold and a
 * best_seed. It is never the kernel's anchored child: that child cannot carry
 * the seed (anchorable_relative_exit). None of the 23 E5 / E9 / E14 tapes
 * measured this shape: every one of them issued its exit once the entry had
 * filled.
 * The tapes are `lab tv` exports on NYSE:F 15m (sub-cent prints),
 * tests/fixtures/pending_entry_trail (README.md there lists them).
 *
 * 1. The arm (lane E5's pending-entry claim). On every event the bar that arms
 *    the trail has a raw extreme strictly inside the activation's tick cell
 *    (two more tapes issue a LIMIT entry instead, which fills mid-bar one or
 *    two bars later; seven of their twelve arms land on the rest of that bar):
 *    its tick IS the activation, the raw price is not. TradingView exits on
 *    that bar at activation -/+ 1 tick, 28 of 28 (market entries 8 + 8, 11 of
 *    them on the fill bar itself; limit entries 6 + 6), so the pending-entry trail arms on the
 *    tick-quantized path exactly as lane E5 measured for a filled entry. The
 *    fill point's leg arms there too (source_trail_arm_level in exit()) and
 *    books TradingView's bar and price, 28 of 28.
 *
 * 2. The running best (lane E14's open measurement). Same shape, but the prints
 *    after the arm land between the two stops a best can give: TradingView's
 *    exit is the stop of a best that starts AT the activation (13 of 13), the
 *    reading lane E14 measured for a filled entry. The fill point's leg names
 *    that start as its Trail::best_seed, and the engine books TradingView's
 *    bar and price, 13 of 13. Lane H-MEASURE recorded these 13 as divergences:
 *    the adapter then adopted the kernel's anchored child, which names no
 *    seed, so its best started at the raw arm print, half a tick or less short
 *    of the activation, and it exited 1 to 24 bars later. Lane PAR-ORDERS
 *    flipped them: a seeded definition is not anchorable.
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

#ifndef PINEFORGE_PENDING_TRAIL_FIXTURE_DIR
#error "PINEFORGE_PENDING_TRAIL_FIXTURE_DIR must name tests/fixtures/pending_entry_trail"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTick = 0.01;
constexpr int kTimeout = 25;       // bars after the fill bar, then strategy.close

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

#include "fixtures/pending_entry_trail/bars.inc"

enum class Question { Arm, RunningBest };

constexpr double kOffsetOne[] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

struct Probe {
    const char* slug;
    bool is_long;
    const double* trail_points;
    const double* trail_offsets;
    const double* limit_levels;  // nullptr: a MARKET entry
    const FeedBar* bars;
    int entries;
    int bars_per_entry;          // the signal bar .. past the timeout close's fill
    Question question;
};

const Probe kProbes[] = {
    {"hm-orders-h4-f-long-pending-arm", true, kH4LongPoints, kH4LongOffsets, nullptr,
     &kH4Long[0][0], 8, 28, Question::Arm},
    {"hm-orders-h4-f-short-pending-arm", false, kH4ShortPoints, kH4ShortOffsets, nullptr,
     &kH4Short[0][0], 8, 28, Question::Arm},
    {"hm-orders-h4-f-long-pending-limit-arm", true, kH4LimitLongPoints, kOffsetOne,
     kH4LimitLongLevels, &kH4LimitLong[0][0], 6, 30, Question::Arm},
    {"hm-orders-h4-f-short-pending-limit-arm", false, kH4LimitShortPoints, kOffsetOne,
     kH4LimitShortLevels, &kH4LimitShort[0][0], 6, 30, Question::Arm},
    {"hm-orders-h3-f-long-pending-seed", true, kH3LongPoints, kH3LongOffsets, nullptr,
     &kH3Long[0][0], 8, 28, Question::RunningBest},
    {"hm-orders-h3-f-short-pending-seed", false, kH3ShortPoints, kH3ShortOffsets, nullptr,
     &kH3Short[0][0], 5, 28, Question::RunningBest},
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
    std::ifstream in(std::string(PINEFORGE_PENDING_TRAIL_FIXTURE_DIR) + "/" + slug
                     + "/tv_trades.csv");
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

// Bar 0 is the signal bar: the entry (market, or a limit that fills later) and
// its trailing exit are issued there together. `kTimeout` bars after the fill bar strategy.close takes
// what is left.
class TapeHost : public pineforge::source::PineStrategyHost {
public:
    TapeHost(bool is_long, double trail_points, double trail_offset, double limit)
        : is_long_(is_long), trail_points_(trail_points), trail_offset_(trail_offset),
          limit_(limit) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 100.0;
        config.commission_value = 0.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.slippage = 0;
        config.pyramiding = 1;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        syminfo_mintick_ = kTick;
    }

    void on_source_bar(const Bar&) override {
        const char* id = is_long_ ? "L" : "S";
        if (bar_index_ == 0) {
            strategy_entry(id, is_long_, limit_);
            strategy_exit(is_long_ ? "LX" : "SX", id, kNaN, kNaN, trail_points_, trail_offset_);
        }
        const double units = physical_position().signed_units;
        if (units != 0.0 && entry_bar_ < 0) entry_bar_ = bar_index_;
        if (units != 0.0 && entry_bar_ >= 0 && bar_index_ - entry_bar_ >= kTimeout)
            strategy_close(id, "timeout");
    }

    source::PineExecutionAdapter::AnchoredRelativeStats anchored() const {
        return adapter_.anchored_relative_stats();
    }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    std::int64_t entry_time(int i) const { return closed_trade_entry_time(i); }
    std::int64_t exit_time(int i) const { return closed_trade_exit_time(i); }

private:
    bool is_long_;
    double trail_points_;
    double trail_offset_;
    double limit_;
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

// TradingView's tick of a print: floor(p / mintick + 0.5).
double tick_of(double price) { return std::floor(price / kTick + 0.5) * kTick; }

int bar_of(const std::vector<Bar>& bars, std::int64_t ms) {
    for (std::size_t i = 0; i < bars.size(); ++i)
        if (bars[i].timestamp == ms) return static_cast<int>(i);
    return -1;
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

// What the kernel recorded for the exit label: the accepted request (an
// anchored Trail, and the best_seed it names) and its TrailArm.
struct ExitRecord {
    bool anchored_trail = false;
    std::optional<double> best_seed;
    std::optional<int> arm_bar;
    std::optional<double> arm_reached;
};

ExitRecord exit_record(const TapeHost& host, const char* label) {
    ExitRecord record;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<native_order::AcceptedEvent>(&*event.command)) {
            if (accepted->request().label != label) continue;
            if (const auto* t = std::get_if<native_order::Trail>(&accepted->request().trigger)) {
                record.anchored_trail = record.anchored_trail
                    || std::holds_alternative<native_order::FromOwnerFill>(accepted->request().anchor);
                if (t->best_seed) record.best_seed = t->best_seed;
            }
        } else if (const auto* act = std::get_if<native_order::ActivatedEvent>(&*event.command)) {
            if (act->definition->request.label != label
                || act->kind != native_order::ActivationKind::TrailArm) {
                continue;
            }
            record.arm_bar = act->cursor.point.interval_index;
            record.arm_reached = act->reached_price;
        }
    }
    return record;
}

// ── every tape trade, replayed ────────────────────────────────────────

void test_tapes_replay(Question question) {
    for (const Probe& probe : kProbes) {
        if (probe.question != question) continue;
        std::printf("-- %s: %d TradingView trades (%s) --\n", probe.slug, probe.entries,
                    question == Question::Arm ? "the arm" : "the running best");
        const auto tape = read_tape(probe.slug);
        CHECK(static_cast<int>(tape.size()) == probe.entries);
        if (static_cast<int>(tape.size()) != probe.entries) continue;
        const double sign = probe.is_long ? 1.0 : -1.0;
        const char* label = probe.is_long ? "LX" : "SX";
        int matched = 0;
        for (int e = 0; e < probe.entries; ++e) {
            const auto bars = entry_bars(probe, e);
            const double limit = probe.limit_levels ? probe.limit_levels[e] : kNaN;
            TapeHost host(probe.is_long, probe.trail_points[e], probe.trail_offsets[e], limit);
            host.fixture_retain_all_events();  // read after the run (V19-B)
            host.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(host.last_error().empty());
            CHECK(host.trade_count() == 1);
            if (host.trade_count() != 1) continue;
            const TapeTrade& tv = tape[static_cast<std::size_t>(e)];
            // The entry: a market fill at the open of the bar after the signal,
            // or the limit's own level on the later bar whose path crosses it
            // (never at that bar's open: no gap).
            CHECK(host.entry_time(0) == tv.entry_ms);
            CHECK(same_price(host.entry_price(0), tv.entry_price));
            const int fill_bar = bar_of(bars, tv.entry_ms);
            if (probe.limit_levels) {
                CHECK(fill_bar >= 1 && same_price(tv.entry_price, limit));
                if (fill_bar >= 1)
                    CHECK(!same_price(bars[static_cast<std::size_t>(fill_bar)].open, limit));
            } else {
                CHECK(fill_bar == 1 && same_price(tv.entry_price, bars[1].open));
            }
            // The shape under test: no anchored child (the definition is not
            // anchorable), so the fill point submits the leg exit() builds,
            // whose running best starts AT the activation. The fill print is
            // short of it on every trade here, so the seed is the activation.
            const auto stats = host.anchored();
            CHECK(stats.anchored == 0 && stats.adopted == 0 && stats.withdrawn == 0);
            const ExitRecord record = exit_record(host, label);
            // TradingView's exit is the trail's (never the timeout), on the bar
            // that armed it or later, at the stop of a best that starts AT the
            // activation: activation -/+ k ticks.
            const int k = static_cast<int>(probe.trail_offsets[e]);
            const double activation = tv.entry_price + sign * probe.trail_points[e] * kTick;
            CHECK(!record.anchored_trail);
            CHECK(record.best_seed && same_price(*record.best_seed, activation));
            const int tv_bar = bar_of(bars, tv.exit_ms);
            CHECK(tv.exit_signal == label);
            CHECK(tv_bar >= 1);
            CHECK(same_price(tv.exit_price, activation - sign * k * kTick));
            // The arm: the kernel's TrailArm lands on a bar whose raw extreme
            // stops inside the activation's tick cell (its tick is the
            // activation, the raw price is not): a quantized arm.
            CHECK(record.arm_bar.has_value());
            if (record.arm_bar && *record.arm_bar >= 0
                && *record.arm_bar < static_cast<int>(bars.size())) {
                const Bar& arm = bars[static_cast<std::size_t>(*record.arm_bar)];
                const double extreme = probe.is_long ? arm.high : arm.low;
                CHECK(same_price(tick_of(extreme), activation));
                CHECK(probe.is_long ? extreme < activation : extreme > activation);
                if (question == Question::Arm) CHECK(*record.arm_bar == tv_bar);
                else CHECK(*record.arm_bar <= tv_bar);
            }
            const bool matches = host.exit_time(0) == tv.exit_ms
                && same_price(host.exit_price(0), tv.exit_price);
            CHECK(host.exit_time(0) == tv.exit_ms);
            CHECK(same_price(host.exit_price(0), tv.exit_price));
            if (matches) {
                ++matched;
            } else {
                std::printf("        trade %d: engine exit %s @%.10g, TradingView %s @%.10g\n",
                            e + 1, utc(host.exit_time(0)).c_str(), host.exit_price(0),
                            utc(tv.exit_ms).c_str(), tv.exit_price);
            }
        }
        std::printf("        %d of %d trades match TradingView bar and price\n", matched,
                    probe.entries);
    }
}

}  // namespace

int main() {
    test_tapes_replay(Question::Arm);
    test_tapes_replay(Question::RunningBest);
    std::printf("\n%s pending-entry trail tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
