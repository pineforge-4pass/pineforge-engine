// R5 lane D2-D. The Pine adapter's chart-day key -- what the intraday day
// ledger, the filled-orders cap clock and the risk rules read at every bar
// open and fill -- keeps its last UTC day with the consumer that keeps the
// adapter's lookup state for the run (AdapterLookupIndex, R5 lane PERF-P7):
// a second of that day answers the kept key without the civil arithmetic. A
// chart timezone keeps its localtime_r path, untouched.
//
// The witness, on two hosts that differ only in the consumer's lookup cache
// (NativeExecutionConsumer::set_host_cache: off, no key is kept and every
// read computes, which is how 6c081f5d read every key):
//
//   * the zone battery: the key at every bar of bar sequences over the K1
//     zones -- the three UTC spellings, America/New_York, Europe/London,
//     Asia/Tokyo, a POSIX DST rule and a fixed offset -- stepped 1 m to 1 W
//     and by local midnights, Mondays and months, filtered through K1's
//     sessions (a regular day, a lunch break, an overnight day, a split day),
//     around every DST gap and overlap and year end, forward, backward and
//     shuffled, against chart_day_key's 6c081f5d body (transcribed below);
//     every civil-day boundary 1969-2101 to the millisecond, every day of
//     years 1-9999, the truncation of negative stamps and the +/-2^40-second
//     edge on each UTC spelling;
//   * the day-ledger runs: seeded strategies on UTC and zoned charts, with
//     no risk rule, an intraday loss rule (percent and cash), a
//     consecutive-loss-days rule and a filled-orders cap, 15 m bars across a
//     DST change and a month end, 60 m charts over 15 m input with the
//     magnifier off and on, and daily bars: at every source callback the
//     adapter's key against the transcription and the source layer's fold
//     (hash_host_extension: the day ledger, the risk state, every queue), then
//     the trades and the report's statistics -- each kept-key run equal to its
//     computed-key run, and every report's statistics equal to the
//     6c081f5d walk over the run's own equity curve
//     (tests/equity_stats_reference.hpp);
//   * the memo itself, which no key above can show -- it answers the key the
//     arithmetic answers -- read and planted in place (memo_witness): on each
//     UTC spelling a read keeps its whole floor day and key, and a key
//     planted over a day answers that day's first to last millisecond and no
//     second outside it; on each chart timezone nothing is kept and a
//     planted key is never answered; the computed host holds no cache.
#include <pineforge/metrics.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_host_reads.hpp"
#include "../src/timezone.hpp"
#include "equity_stats_reference.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
long long checks = 0;

std::map<int, long long> failed_lines;  // a failing CHECK's line -> its failures

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++checks;                                                               \
        if (!(cond)) {                                                          \
            ++failed_lines[__LINE__];                                           \
            if (++failures <= 25)                                               \
                std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while (0)

#if defined(__SANITIZE_ADDRESS__)
constexpr std::int64_t kStride = 15;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr std::int64_t kStride = 15;
#else
constexpr std::int64_t kStride = 1;
#endif
#else
constexpr std::int64_t kStride = 1;
#endif

constexpr std::int64_t kSecond = 1000;
constexpr std::int64_t kMinute = 60 * kSecond;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

// chart_day_key's body at 6c081f5d, verbatim, for the staged chart zone.
std::int64_t reference_day_key(const std::string& timezone, std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm fields{};
    const auto utc = [&]() {
        return ::gmtime_r(&seconds, &fields) != nullptr;
    };
    if (timezone.empty() || timezone == "UTC" || timezone == "Etc/UTC") {
        constexpr std::int64_t kCivilSpan = std::int64_t{1} << 40;
        const std::int64_t secs = static_cast<std::int64_t>(seconds);
        if (secs > -kCivilSpan && secs < kCivilSpan) {
            const std::int64_t days = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
            const std::int64_t z = days + 719468;
            const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const std::int64_t doe = z - era * 146097;
            const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const std::int64_t mp = (5 * doy + 2) / 153;
            const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
            const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
            return day * 100 + month;
        }
        if (!utc()) return std::numeric_limits<std::int64_t>::min();
    } else {
        try {
            tz_util::ScopedTimezone guard(timezone);
            if (::localtime_r(&seconds, &fields) == nullptr) {
                if (!utc()) return std::numeric_limits<std::int64_t>::min();
            }
        } catch (...) {
            if (!utc()) return std::numeric_limits<std::int64_t>::min();
        }
    }
    return static_cast<std::int64_t>(fields.tm_mday) * 100
        + static_cast<std::int64_t>(fields.tm_mon + 1);
}

bool utc_spelling(const std::string& zone) {
    return zone.empty() || zone == "UTC" || zone == "Etc/UTC";
}

// ── The key battery ──────────────────────────────────────────────────────
// A host whose adapter has staged `zone` (set_chart_timezone reaches the
// adapter at the next begin); its fixture read is chart_day_key itself.
class KeyHost final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    void stage(const std::string& zone, bool keep) {
        as_native_consumer(execution_consumer()).set_host_cache(keep);
        set_chart_timezone(zone);
        const Bar bars[] = {
            {100.0, 100.0, 100.0, 100.0, 1.0, 0},
            {100.0, 100.0, 100.0, 100.0, 1.0, 60'000},
        };
        run(bars, 2);
    }
    std::int64_t key(std::int64_t timestamp_ms) const noexcept {
        return fixture_chart_day_key(timestamp_ms);
    }
    bool holds_cache() const { return cache() != nullptr; }
    NativeHostCache* cache() const {
        return as_native_consumer(execution_consumer()).host_cache();
    }
    const void* adapter_address() const noexcept { return &adapter_; }
};

long long battery_keys = 0;
long long zoned_keys = 0;

struct Pair {
    std::string zone;
    KeyHost kept;
    KeyHost computed;
    explicit Pair(std::string z) : zone(std::move(z)) {
        kept.stage(zone, true);
        computed.stage(zone, false);
    }
    void at(std::int64_t ts) {
        const std::int64_t want = reference_day_key(zone, ts);
        const std::int64_t got = kept.key(ts);
        const std::int64_t plain = computed.key(ts);
        CHECK(got == want);
        CHECK(plain == want);
        if ((got != want || plain != want) && failures <= 25) {
            std::fprintf(stderr, "  zone '%s' ts_ms=%" PRId64 ": want %" PRId64
                         " kept %" PRId64 " computed %" PRId64 "\n",
                         zone.c_str(), ts, want, got, plain);
        }
        ++battery_keys;
        if (!utc_spelling(zone)) ++zoned_keys;
    }
    void walk(const std::vector<std::int64_t>& stamps, std::uint64_t seed) {
        for (std::int64_t ts : stamps) at(ts);
        for (auto it = stamps.rbegin(); it != stamps.rend(); ++it) at(*it);
        std::vector<std::int64_t> shuffled = stamps;
        std::uint64_t mix = seed | 1;
        for (std::size_t i = shuffled.size(); i > 1; --i) {
            mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
            std::swap(shuffled[i - 1], shuffled[mix % i]);
        }
        for (std::int64_t ts : shuffled) at(ts);
    }
};

// The local calendar fields of `ts` in `zone` (gmtime for a UTC spelling),
// used only to shape the bar sequences.
std::tm local_fields(const std::string& zone, std::int64_t ts) {
    const std::time_t seconds = static_cast<std::time_t>(ts / 1000);
    std::tm fields{};
    if (utc_spelling(zone)) {
        ::gmtime_r(&seconds, &fields);
    } else {
        tz_util::ScopedTimezone guard(zone);
        ::localtime_r(&seconds, &fields);
    }
    return fields;
}

// The instant of local midnight opening y-m-d in `zone` (mktime under the
// zone; timegm-equivalent for a UTC spelling).
std::int64_t local_midnight(const std::string& zone, int y, int m, int d) {
    std::tm fields{};
    fields.tm_year = y - 1900;
    fields.tm_mon = m - 1;
    fields.tm_mday = d;
    fields.tm_isdst = -1;
    if (utc_spelling(zone)) {
        tz_util::ScopedTimezone guard("UTC");
        return static_cast<std::int64_t>(::mktime(&fields)) * 1000;
    }
    tz_util::ScopedTimezone guard(zone);
    return static_cast<std::int64_t>(::mktime(&fields)) * 1000;
}

// K1's sessions, as local minute windows and a weekday rule, for shaping.
enum class Session { All, Regular, Lunch, Overnight, Split };

bool in_session(Session session, const std::tm& t) {
    const int minute = t.tm_hour * 60 + t.tm_min;
    const int weekday = t.tm_wday;  // 0 Sunday
    switch (session) {
    case Session::All: return true;
    case Session::Regular:  // 0930-1600:23456
        return weekday >= 1 && weekday <= 5 && minute >= 570 && minute < 960;
    case Session::Lunch:  // 0900-1130,1230-1500
        return (minute >= 540 && minute < 690) || (minute >= 750 && minute < 900);
    case Session::Overnight:  // 1800-1700:23456
        return (minute >= 1080 && weekday >= 0 && weekday <= 4)
            || (minute < 1020 && weekday >= 1 && weekday <= 5);
    case Session::Split:  // 0000-0230,0330-0600,1300-2400
        return minute < 150 || (minute >= 210 && minute < 360) || minute >= 780;
    }
    return true;
}

std::vector<std::int64_t> stepped(const std::string& zone, std::int64_t anchor,
                                  std::int64_t half_width, std::int64_t step,
                                  Session session) {
    std::vector<std::int64_t> out;
    const std::int64_t from = (anchor - half_width) / step * step;
    for (std::int64_t ts = from; ts <= anchor + half_width; ts += step) {
        if (session == Session::All || in_session(session, local_fields(zone, ts)))
            out.push_back(ts);
    }
    return out;
}

// 2024-03-10 07:00 UTC (NY spring), 2024-11-03 06:00 UTC (NY fall),
// 2025-03-30 01:00 UTC and 2025-10-26 01:00 UTC (London), 2025-03-09 07:00
// and 2025-11-02 06:00 UTC (the POSIX rule), 2025-01-01 00:00 JST, 00:00
// +05:30 and UTC: the K1 anchors.
constexpr std::int64_t kNySpring = 1710054000000LL;
constexpr std::int64_t kNyFall = 1730613600000LL;
constexpr std::int64_t kLondonSpring = 1743296400000LL;
constexpr std::int64_t kLondonFall = 1761440400000LL;
constexpr std::int64_t kPosixSpring = 1741503600000LL;
constexpr std::int64_t kPosixFall = 1762063200000LL;
constexpr std::int64_t kTokyoYearEnd = 1735657200000LL;
constexpr std::int64_t kIstYearEnd = 1735669800000LL;
constexpr std::int64_t kUtcYearEnd = 1735689600000LL;

void zone_battery() {
    struct Zone {
        const char* name;
        std::vector<std::int64_t> anchors;
    };
    const std::vector<Zone> zones = {
        {"", {kUtcYearEnd, kNySpring}},
        {"UTC", {kUtcYearEnd, kLondonFall}},
        {"Etc/UTC", {kUtcYearEnd, kPosixSpring}},
        {"America/New_York", {kNySpring, kNyFall}},
        {"Europe/London", {kLondonSpring, kLondonFall}},
        {"Asia/Tokyo", {kTokyoYearEnd}},
        {"EST5EDT,M3.2.0,M11.1.0", {kPosixSpring, kPosixFall}},
        {"UTC+05:30", {kIstYearEnd}},
    };
    struct Step {
        std::int64_t step;
        std::int64_t half_width;
    };
    const Step steps[] = {
        {kMinute, 2 * kDay}, {5 * kMinute, 4 * kDay}, {15 * kMinute, 9 * kDay},
        {kHour, 20 * kDay}, {4 * kHour, 60 * kDay}, {kDay, 400 * kDay},
        {7 * kDay, 1500 * kDay},
    };
    const Session sessions[] = {Session::All, Session::Regular, Session::Lunch,
                                Session::Overnight, Session::Split};
    std::uint64_t seed = 0x6a09e667f3bcc909ull;
    for (const Zone& zone : zones) {
        Pair pair(zone.name);
        for (std::int64_t anchor : zone.anchors) {
            for (const Step& step : steps) {
                for (Session session : sessions) {
                    // The daily and weekly grids are session-free.
                    if (step.step >= kDay && session != Session::All) continue;
                    const std::int64_t stride = utc_spelling(zone.name) ? 1 : kStride;
                    pair.walk(stepped(zone.name, anchor, step.half_width,
                                      step.step * stride, session), seed += 0x9e3779b9);
                }
            }
            // Charts at or above the day grain: local midnights, local
            // Mondays and local firsts of the month around the anchor.
            std::tm at = local_fields(zone.name, anchor);
            std::vector<std::int64_t> days;
            std::vector<std::int64_t> mondays;
            std::vector<std::int64_t> months;
            for (int d = -400; d <= 400; ++d) {
                const std::int64_t midnight =
                    local_midnight(zone.name, at.tm_year + 1900, at.tm_mon + 1, at.tm_mday + d);
                days.push_back(midnight);
                if (local_fields(zone.name, midnight).tm_wday == 1) mondays.push_back(midnight);
            }
            for (int m = -120; m <= 120; ++m) {
                months.push_back(local_midnight(zone.name, at.tm_year + 1900, at.tm_mon + 1 + m, 1));
            }
            pair.walk(days, seed += 0x9e3779b9);
            pair.walk(mondays, seed += 0x9e3779b9);
            pair.walk(months, seed += 0x9e3779b9);
        }
        // The kept host's memo is memo_witness's: holding a cache shows
        // nothing since D2-C, which adopts the index on every chart.
        CHECK(pair.computed.holds_cache() == false);
    }
}

// Every civil-day boundary 1969-2101 to the millisecond, every day of years
// 1-9999, negative truncation and the +/-2^40-second edge, on each UTC
// spelling, in order: each read moves the kept day or answers from it.
void utc_edges() {
    for (const char* spelling : {"", "UTC", "Etc/UTC"}) {
        Pair pair(spelling);
        const std::int64_t end_2101 = 4133980800LL * kSecond;
        for (std::int64_t day = -365; day * kDay <= end_2101; day += kStride) {
            for (std::int64_t delta : {-1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL, 999LL, 1000LL,
                                       1001LL, 43200000LL, 86399999LL}) {
                pair.at(day * kDay + delta);
            }
        }
        const std::int64_t first = -62135596800LL / 86400;  // 0001-01-01
        const std::int64_t last = 253402300799LL / 86400;   // 9999-12-31
        for (std::int64_t day = first; day <= last; day += 3 * kStride) pair.at(day * kDay + 43200000LL);
        for (std::int64_t ts = -5000; ts <= 5000; ts += 7) pair.at(ts);
        const std::int64_t edge = std::int64_t{1} << 40;
        for (std::int64_t secs : {edge - 86401, edge - 86400, edge - 1, edge, edge + 1,
                                  -edge - 1, -edge, -edge + 1, -edge + 86400, edge * 2,
                                  -edge * 2}) {
            pair.at(secs * kSecond);
            pair.at(secs * kSecond + (secs < 0 ? -999 : 999));
        }
    }
}

// ── The day-ledger runs ──────────────────────────────────────────────────
constexpr std::uint64_t kExecutionHash = 0x5eed1234abcd0d2dull;

enum class Rule { None, LossPercent, LossCash, LossDays, Cap, All };

struct RunCase {
    std::string zone;
    Rule rule;
    std::int64_t start;
    std::int64_t step;
    int bars;
    std::string script_tf;  // empty: the input's own
    bool magnifier;
    std::uint64_t seed;
};

class LedgerHost final : public source::PineStrategyHost {
public:
    LedgerHost(const RunCase& c, bool keep, bool read_keys)
        : case_(c), read_keys_(read_keys), mix_(c.seed | 1) {
        as_native_consumer(execution_consumer()).set_host_cache(keep);
        attach_pine_execution_adapter();
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_chart_timezone(c.zone);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.5;
        configure_pine_strategy(config);
        if (c.rule == Rule::LossPercent || c.rule == Rule::All)
            set_pine_risk_max_intraday_loss(1.5, true);
        if (c.rule == Rule::LossCash) set_pine_risk_max_intraday_loss(120.0, false);
        if (c.rule == Rule::LossDays || c.rule == Rule::All) set_pine_risk_max_cons_loss_days(2);
        if (c.rule == Rule::Cap || c.rule == Rule::All) set_pine_risk_max_intraday_filled_orders(5);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kExecutionHash);
    }

    void on_source_bar(const Bar& bar) override {
        if (read_keys_) {
            const std::int64_t got = fixture_chart_day_key(bar.timestamp);
            CHECK(got == reference_day_key(case_.zone, bar.timestamp));
            ++bar_keys;
        }
        BrokerStateHashSink f;
        hash_host_extension(f);
        const auto book = physical_position();
        f.d(book.signed_units);
        f.d(book.average_price);
        f.u(book.lot_count);
        f.i(pending_order_count());
        folds.push_back(f.h);
        script(bar);
    }

    std::vector<std::uint64_t> folds;
    long long bar_keys = 0;

private:
    std::uint64_t draw() {
        mix_ ^= mix_ << 13;
        mix_ ^= mix_ >> 7;
        mix_ ^= mix_ << 17;
        return mix_;
    }

    void script(const Bar& bar) {
        const std::uint64_t roll = draw() % 100;
        const double held = live_position_size();
        if (roll < 22) {
            const bool is_long = draw() % 2 == 0;
            strategy_entry(is_long ? "L" : "S", is_long);
        } else if (roll < 30 && held != 0.0) {
            strategy_close(held > 0.0 ? "L" : "S");
        } else if (roll < 40 && held != 0.0) {
            const double reach = 0.25 * static_cast<double>(1 + draw() % 12);
            const double limit = held > 0.0 ? bar.close + reach : bar.close - reach;
            const double stop = held > 0.0 ? bar.close - reach : bar.close + reach;
            strategy_exit("X", held > 0.0 ? "L" : "S", limit, stop);
        } else if (roll < 43) {
            strategy_close_all();
        }
    }

    RunCase case_;
    bool read_keys_;
    std::uint64_t mix_;
};

std::vector<Bar> tape(const RunCase& c) {
    std::vector<Bar> bars;
    std::uint64_t mix = c.seed * 0x9e3779b97f4a7c15ull + 7;
    double price = 400.0;
    for (int i = 0; i < c.bars; ++i) {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        const double move = 0.25 * static_cast<double>(static_cast<int>(mix % 41) - 20);
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        const double wick = 0.25 * static_cast<double>(1 + mix % 16);
        const double open = price;
        const double close = std::max(10.0, price + move);
        const double high = std::max(open, close) + wick;
        const double low = std::max(1.0, std::min(open, close) - wick);
        bars.push_back({open, high, low, close, 1.0, c.start + static_cast<std::int64_t>(i) * c.step});
        price = close;
    }
    return bars;
}

struct Outcome {
    std::string error;
    std::vector<std::uint64_t> folds;
    std::vector<Trade> trades;
    std::uint64_t final_hash = 0;
    pf_equity_stats_t stats{};
    std::vector<pf_equity_point_t> curve;
    double net_profit = 0.0;
    long long bar_keys = 0;
    int loss_closes = 0;
};

Outcome observe(const RunCase& c, bool keep, bool read_keys) {
    LedgerHost host(c, keep, read_keys);
    const std::vector<Bar> bars = tape(c);
    const std::string input_tf = c.step == kDay ? "D" : std::to_string(c.step / kMinute);
    if (c.script_tf.empty() && !c.magnifier) {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()), input_tf,
                 c.script_tf.empty() ? input_tf : c.script_tf, c.magnifier, 4,
                 MagnifierDistribution::ENDPOINTS);
    }
    Outcome out;
    out.error = host.last_error();
    out.folds = host.folds;
    for (int i = 0; i < host.trade_count(); ++i) {
        out.trades.push_back(host.get_trade(i));
        if (out.trades.back().exit_comment == source::kIntradayLossComment) ++out.loss_closes;
    }
    out.final_hash = host.broker_state_hash();
    ReportC report{};
    host.fill_report(&report);
    out.stats = report.metrics.equity;
    out.curve.assign(report.equity_curve, report.equity_curve + report.equity_curve_len);
    out.net_profit = report.net_profit;
    BacktestEngine::free_report(&report);
    out.bar_keys = host.bar_keys;
    return out;
}

bool same_trade(const Trade& a, const Trade& b) {
    return a.entry_time == b.entry_time && a.exit_time == b.exit_time
        && std::memcmp(&a.entry_price, &b.entry_price, sizeof(double)) == 0
        && std::memcmp(&a.exit_price, &b.exit_price, sizeof(double)) == 0
        && std::memcmp(&a.qty, &b.qty, sizeof(double)) == 0
        && std::memcmp(&a.pnl, &b.pnl, sizeof(double)) == 0
        && a.entry_id == b.entry_id && a.exit_id == b.exit_id
        && a.exit_comment == b.exit_comment && a.is_long == b.is_long;
}

long long ledger_runs = 0;
long long ledger_folds = 0;
long long loss_closes = 0;

void day_ledger_runs() {
    const std::int64_t mar_2024 = 1709251200000LL;   // 2024-03-01 00:00 UTC
    const std::int64_t jan_2023 = 1672531200000LL;   // 2023-01-01 00:00 UTC
    std::vector<RunCase> cases;
    std::uint64_t seed = 11;
    for (const char* zone : {"", "UTC", "Etc/UTC", "America/New_York", "Asia/Tokyo"}) {
        for (Rule rule : {Rule::None, Rule::LossPercent, Rule::LossCash, Rule::LossDays,
                          Rule::Cap, Rule::All}) {
            cases.push_back({zone, rule, mar_2024, 15 * kMinute,
                             static_cast<int>(4 * 96 * 11 / kStride), "", false, ++seed});
            cases.push_back({zone, rule, jan_2023, kDay, static_cast<int>(730 / kStride), "",
                             false, ++seed});
        }
        for (bool magnifier : {false, true}) {
            cases.push_back({zone, Rule::All, mar_2024, 15 * kMinute,
                             static_cast<int>(4 * 96 * 12 / kStride), "60", magnifier, ++seed});
        }
    }
    for (const RunCase& c : cases) {
        const Outcome kept = observe(c, true, false);
        const Outcome computed = observe(c, false, false);
        const Outcome read = observe(c, true, true);
        ++ledger_runs;
        CHECK(kept.error.empty());
        CHECK(computed.error.empty());
        if (!kept.error.empty()) std::fprintf(stderr, "  run error: %s\n", kept.error.c_str());
        CHECK(kept.folds == computed.folds);
        CHECK(kept.folds == read.folds);
        CHECK(kept.final_hash == computed.final_hash);
        CHECK(kept.trades.size() == computed.trades.size());
        for (std::size_t i = 0; i < kept.trades.size() && i < computed.trades.size(); ++i)
            CHECK(same_trade(kept.trades[i], computed.trades[i]));
        CHECK(equity_stats_reference::first_difference(kept.stats, computed.stats) == nullptr);
        CHECK(kept.curve.size() == computed.curve.size());
        CHECK(read.bar_keys == static_cast<long long>(read.folds.size()));
        // The report's statistics are the 6c081f5d walk over its own curve.
        const auto n = static_cast<std::int64_t>(kept.curve.size());
        const pf_equity_point_t* data = kept.curve.empty() ? nullptr : kept.curve.data();
        for (const char* tz : {c.zone.c_str(), "", "America/New_York"}) {
            const auto got = metrics::compute_equity_stats(data, n, 10000.0, tz, 400.0, 401.5,
                                                           n / 3, kept.net_profit);
            const auto want = equity_stats_reference::compute_equity_stats(
                data, n, 10000.0, tz, 400.0, 401.5, n / 3, kept.net_profit);
            CHECK(equity_stats_reference::first_difference(got, want) == nullptr);
        }
        ledger_folds += static_cast<long long>(kept.folds.size());
        loss_closes += kept.loss_closes;
    }
}

// ── The memo itself ──────────────────────────────────────────────────────
// Every check above holds a key to the value it must have, and the memo
// answers the value the arithmetic answers, so none of them can tell a key
// read from the memo from one computed again: INT23's mutant cd5, a memo
// never kept or read, passes them all. Nor can holding a cache: since R5
// lane D2-C the consumer holds the adapter's lookup index from every run's
// begin (take_run_facts), on a chart timezone too, where chart_day_key never
// reaches it. So the witness reads the memo in place and plants keys in it.
// AdapterLookupIndex is pine_adapter.cpp's own type. LookupIndexLayout
// repeats its members in order over the same base only to find where the
// last three -- day_lo, day_hi, day_key -- live; they are read and written
// as bytes at those offsets from the index's NativeHostCache base, never
// through the layout type. The offsets are held to the whole days UTC reads
// keep before anything is planted, so a layout that drifts from the index's
// fails there instead of writing over another member.
struct LookupIndexLayout final : source::detail::PineRunCache {
    struct CohortSides {
        std::size_t folded = 0;
        bool opened_long = false;
        bool opened_short = false;
    };
    std::uint64_t answers = 0;
    std::unordered_map<const void*, CohortSides> cohort_sides;
    std::uint64_t erased_seen = 0;
    std::uint64_t folded_high = 0;
    std::size_t rows_folded = 0;
    std::vector<std::uint64_t> unbound;
    std::array<std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>, 3> legs_by_origin;
    std::unordered_map<std::int32_t, std::vector<std::uint64_t>> immediate_closes_by_bar;
    std::int64_t day_lo = 1;
    std::int64_t day_hi = 0;
    std::int64_t day_key = 0;
    std::uint64_t answered() const noexcept override { return answers; }
};

// The memo: the whole seconds [lo, hi) it keeps and the key they answer. The
// default is an index's own before any read keeps a day.
struct DayMemo {
    std::int64_t lo = 1;
    std::int64_t hi = 0;
    std::int64_t key = 0;
    bool operator==(const DayMemo& other) const {
        return lo == other.lo && hi == other.hi && key == other.key;
    }
};

// The memo of the lookup index the host's consumer holds for the host's own
// adapter, in place; not held when the consumer holds no such index.
class MemoInPlace {
public:
    explicit MemoInPlace(const KeyHost& host) {
        NativeHostCache* const cache = host.cache();
        if (cache == nullptr || cache->kind() != &source::detail::kPineRunCacheKind) return;
        if (static_cast<const source::detail::PineRunCache*>(cache)->owner
            != host.adapter_address())
            return;
        if (std::strstr(typeid(*cache).name(), "AdapterLookupIndex") == nullptr) return;
        index_ = reinterpret_cast<unsigned char*>(cache);
    }
    bool held() const { return index_ != nullptr; }
    DayMemo read() const {
        DayMemo memo;
        std::memcpy(&memo.lo, index_ + offsets().lo, sizeof memo.lo);
        std::memcpy(&memo.hi, index_ + offsets().hi, sizeof memo.hi);
        std::memcpy(&memo.key, index_ + offsets().key, sizeof memo.key);
        return memo;
    }
    void plant(const DayMemo& memo) const {
        std::memcpy(index_ + offsets().lo, &memo.lo, sizeof memo.lo);
        std::memcpy(index_ + offsets().hi, &memo.hi, sizeof memo.hi);
        std::memcpy(index_ + offsets().key, &memo.key, sizeof memo.key);
    }

private:
    struct Offsets {
        std::ptrdiff_t lo;
        std::ptrdiff_t hi;
        std::ptrdiff_t key;
    };
    static const Offsets& offsets() {
        static const Offsets at = [] {
            const LookupIndexLayout layout;
            const auto* base = reinterpret_cast<const unsigned char*>(
                static_cast<const NativeHostCache*>(&layout));
            const auto of = [&](const std::int64_t& field) {
                return reinterpret_cast<const unsigned char*>(&field) - base;
            };
            return Offsets{of(layout.day_lo), of(layout.day_hi), of(layout.day_key)};
        }();
        return at;
    }
    unsigned char* index_ = nullptr;
};

// A key no day has: day 42 of month 42.
constexpr std::int64_t kPlantedKey = 4242;

long long memo_days_kept = 0;
long long memo_planted_answers = 0;
long long memo_computed_reads = 0;

// The whole floor day a UTC read at `ts` keeps, as chart_day_key keeps it:
// the stamp truncated to its second, then floored to its day.
DayMemo kept_day(std::int64_t ts) {
    const std::int64_t secs = ts / 1000;
    const std::int64_t day = secs / 86400 - (secs % 86400 < 0 ? 1 : 0);
    return {day * 86400, day * 86400 + 86400, reference_day_key("UTC", ts)};
}

bool memo_is(const MemoInPlace& memo, const DayMemo& want, const char* zone, std::int64_t ts) {
    const DayMemo got = memo.read();
    if (got == want) return true;
    if (failures < 25) {
        std::fprintf(stderr, "  zone '%s' after ts_ms=%" PRId64 ": memo [%" PRId64 ",%" PRId64
                     ") key %" PRId64 ", want [%" PRId64 ",%" PRId64 ") key %" PRId64 "\n",
                     zone, ts, got.lo, got.hi, got.key, want.lo, want.hi, want.key);
    }
    return false;
}

void memo_witness() {
    const std::int64_t mar_15 = 1710460800000LL;  // 2024-03-15 00:00 UTC
    const std::int64_t mar_20 = 1710892800000LL;  // 2024-03-20 00:00 UTC
    const std::int64_t dec_31_1969 = -kDay;       // 1969-12-31 00:00 UTC
    const std::int64_t read_at = mar_15 + kHour;
    const DayMemo planted{mar_20 / 1000, mar_20 / 1000 + 86400, kPlantedKey};
    bool offsets_held = false;
    // The UTC spellings first: their kept days hold the offsets the chart
    // timezones' plants use.
    for (const char* zone : {"", "UTC", "Etc/UTC", "America/New_York", "Europe/London",
                             "Asia/Tokyo", "EST5EDT,M3.2.0,M11.1.0", "UTC+05:30"}) {
        KeyHost kept;
        kept.stage(zone, true);
        KeyHost computed;
        computed.stage(zone, false);
        CHECK(kept.key(read_at) == reference_day_key(zone, read_at));
        CHECK(computed.key(read_at) == reference_day_key(zone, read_at));
        CHECK(computed.holds_cache() == false);
        const MemoInPlace memo(kept);
        CHECK(memo.held());  // the index D2-C adopts at the run's begin
        if (!memo.held()) continue;
        if (utc_spelling(zone)) {
            // Kept: each read keeps its whole floor day and key -- a day
            // before the epoch too, where the stamp truncates towards zero
            // and the day floors below it.
            bool kept_whole = memo_is(memo, kept_day(read_at), zone, read_at);
            CHECK(kept_whole);
            for (std::int64_t ts : {dec_31_1969 + 12 * kHour, read_at}) {
                if (!kept_whole) break;
                CHECK(kept.key(ts) == reference_day_key(zone, ts));
                kept_whole = memo_is(memo, kept_day(ts), zone, ts);
                CHECK(kept_whole);
            }
            if (!kept_whole) continue;  // never plant through offsets that do not hold
            offsets_held = true;
            memo_days_kept += 3;
            // Read: a key planted over a day answers its first to its last
            // millisecond, and those reads write nothing.
            memo.plant(planted);
            for (std::int64_t ts : {mar_20, mar_20 + 12 * kHour + 34567, mar_20 + kDay - 1}) {
                const bool answered = kept.key(ts) == kPlantedKey;
                CHECK(answered);
                if (answered) ++memo_planted_answers;
            }
            CHECK(memo_is(memo, planted, zone, mar_20));
            // ... and no second outside it: the first millisecond after and
            // the last before compute their own key and keep their own day.
            for (std::int64_t ts : {mar_20 + kDay, mar_20 - 1}) {
                memo.plant(planted);
                const bool computes = kept.key(ts) == reference_day_key(zone, ts);
                CHECK(computes);
                CHECK(memo_is(memo, kept_day(ts), zone, ts));
                if (computes) ++memo_computed_reads;
            }
        } else {
            // A chart timezone keeps nothing, and never answers a key planted
            // over the day it reads.
            CHECK(memo_is(memo, DayMemo{}, zone, read_at));
            if (!offsets_held) continue;
            const DayMemo over{mar_15 / 1000, mar_15 / 1000 + 86400, kPlantedKey};
            memo.plant(over);
            const bool computes = kept.key(read_at) == reference_day_key(zone, read_at);
            CHECK(computes);
            CHECK(memo_is(memo, over, zone, read_at));
            if (computes) ++memo_computed_reads;
        }
    }
}

}  // namespace

int main() {
    zone_battery();
    utc_edges();
    day_ledger_runs();
    memo_witness();
    std::printf("test_chart_day_memo: %lld keys against the 6c081f5d body (%lld on chart "
                "timezones), %lld day-ledger runs (%lld source folds, %lld intraday-loss closes) "
                "kept == computed\n",
                battery_keys, zoned_keys, ledger_runs, ledger_folds, loss_closes);
    std::printf("test_chart_day_memo: the memo in place: %lld UTC days kept whole, %lld reads "
                "answered by a key planted over their day, %lld reads outside it or on a chart "
                "timezone computed\n",
                memo_days_kept, memo_planted_answers, memo_computed_reads);
    CHECK(battery_keys > 3000000 / kStride);
    CHECK(zoned_keys > 100000 / kStride);
    CHECK(ledger_runs == 70);
    CHECK(loss_closes > 0);
    CHECK(memo_days_kept == 9);
    CHECK(memo_planted_answers == 9);
    CHECK(memo_computed_reads == 11);
    if (failures == 0) {
        std::printf("test_chart_day_memo: ok (%lld checks)\n", checks);
        return 0;
    }
    for (const auto& [line, count] : failed_lines)
        std::fprintf(stderr, "  line %d failed %lld times\n", line, count);
    std::fprintf(stderr, "test_chart_day_memo: %d of %lld checks failed\n", failures, checks);
    return 1;
}
