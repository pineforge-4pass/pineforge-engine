// R5 lane PERF-K1: every calendar-derived value a bare native host observes,
// value by value, over twelve fixed scenarios.
//
// Canonical slot labels (the NativeRunSpec default) resolve each bar's input
// and script intervals through native_calendar, and the session-day facts of
// every decision read the same calendar (R5 lane F5). Those intervals become
// the coordinates of every driver point, and every driver point is folded
// into the continuation digest. Lane PERF-K1 changes HOW the kernel resolves
// them -- a memo of the run calendar's session days instead of a fresh
// resolution per lookup -- and must not change WHAT they are: not one
// interval field, coordinate, session-day flag, series bucket interval or
// driver row.
//
// So this witness is data. Each scenario folds, in delivery order, every
// NativeInputContext, every NativeDecisionContext of a bar open, a bar and a
// print, every NativeTimeframeBarContext, and then every driver row of
// native_events() and every closed trade, into one FNV-1a value; the value,
// with the callback and row counts, is pinned below as observed on engine
// main fc7aad62 (wave F), before the lane. It says nothing about cost.
//
// The scenarios cover what the calendar resolves: a UTC year end, a masked
// America/New_York session across the November clock change with holes in
// its feed, an Asia/Tokyo lunch break, Europe/London's March change, a POSIX
// rule zone with an overnight session, a fixed offset, a daily script over
// hourly input (calendar periods), a declared hourly series, FeedTolerant
// labels over an aggregating pairing, a FeedTolerant stream crossing a closed
// night, a canonical stream crossing one, and a stream of prints with quiet
// slots, a session close and a partial final slot.
//
// One more check is relational, not pinned: a host reconfigured from
// America/New_York to Asia/Tokyo mid-life must see, bar for bar, exactly the
// intervals and session-day facts a fresh Tokyo host sees -- the calendar the
// kernel resolved before must not answer for the one it was given.
//
// Portability. The continuation digest folds the zone's tzdata CONTENT (lane
// E23), which differs between machines that agree on every rule this witness
// reads, so it is not pinned here; the byte-identity battery compares it on
// one machine. What is folded depends only on the zones' rules over
// 2024-2025, and every price is an exact binary fraction, so each value is
// the same on every host.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// fc7aad62 library with -DPINEFORGE_K1_HARVEST (which prints the observed
// values as the initializers below instead of checking them). Rebuild them the
// same way; never edit one by hand to make a run pass.
//
// Source-free: this TU links the generic kernel alone and runs in the
// kernel-only profile.

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;

// FNV-1a 64 over explicit little-endian words, so the value does not depend
// on the host's byte order.
struct Fold {
    std::uint64_t h = 1469598103934665603ULL;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void flag(bool v) { byte(v ? 1 : 0); }
    void real(double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u64(bits);
    }
    void text(const std::string& s) {
        u64(s.size());
        for (char c : s) byte(static_cast<std::uint8_t>(c));
    }
    void interval(const native_calendar::NativeInterval& iv) {
        i64(iv.open_ms);
        i64(iv.eligible_open_ms);
        i64(iv.last_traded_close_ms);
        i64(iv.next_period_open_ms);
        i64(iv.next_input_open_ms);
    }
    void coordinate(const NativeCoordinate& c) {
        u64(c.ordinal);
        i64(c.interval_index);
        i64(c.input_interval_index);
        i64(c.open_ms);
        i64(c.eligible_open_ms);
        i64(c.last_traded_close_ms);
        i64(c.next_period_open_ms);
        i64(c.next_input_open_ms);
        i64(c.effective_time_ms);
        i64(c.source_price_time_ms);
        byte(static_cast<std::uint8_t>(c.provenance));
        byte(static_cast<std::uint8_t>(c.path_phase));
        byte(static_cast<std::uint8_t>(c.completion));
    }
    void decision(const NativeDecisionContext& d) {
        coordinate(d.coordinate);
        i64(d.decision_floor_ms);
        interval(d.input_interval);
        interval(d.script_interval);
        i64(d.sub_index);
        i64(d.sub_count);
        flag(d.is_terminal_sub_bar);
        flag(d.in_session);
        flag(d.opens_session_day);
        flag(d.closes_session_day);
        flag(d.closes_session_day_open_ended);
        i64(d.sub_bar_open_ms);
        i64(d.script_bar_open_ms);
    }
    void bar(const Bar& b) {
        real(b.open);
        real(b.high);
        real(b.low);
        real(b.close);
        real(b.volume);
        i64(b.timestamp);
    }
};

// Every calendar-derived value the kernel hands a host, folded in delivery
// order. Every `period` bars the host flips a one-unit position with a market
// request, so the run also books fills and trades.
struct Observer final : NativeStrategyHost {
    Fold fold;
    int period = 0;
    std::uint64_t bars = 0;
    std::uint64_t opens = 0;
    std::uint64_t inputs = 0;
    std::uint64_t series = 0;
    std::uint64_t prints = 0;

    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        ++inputs;
        fold.byte(1);
        fold.bar(bar);
        fold.interval(context.input_interval);
        fold.interval(context.script_interval);
        fold.i64(context.input_index);
        fold.flag(context.completes_script_interval);
    }
    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        ++series;
        fold.byte(2);
        fold.bar(bar);
        fold.u64(context.subscription);
        fold.interval(context.interval);
        fold.byte(static_cast<std::uint8_t>(context.completion));
        fold.i64(context.delivered_at_ms);
    }
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext& context) override {
        ++opens;
        fold.byte(3);
        fold.bar(bar);
        fold.decision(context);
    }
    void on_native_tick(const Bar& bar, const NativeTickContext& context) override {
        ++prints;
        fold.byte(4);
        fold.bar(bar);
        fold.decision(context.decision);
        fold.u64(context.sequence);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++bars;
        fold.byte(5);
        fold.bar(bar);
        fold.decision(context);
        if (period <= 0 || bars % static_cast<std::uint64_t>(period) != 0) return;
        no::Request request;
        if (physical_position().signed_units == 0.0) {
            request.intent = no::Transact{1.0};
        } else {
            request.intent = no::Flatten{};
        }
        request.label = "k1";
        submit_market(request);
    }
};

// The observer's callbacks, then every driver row and every closed trade.
struct Observed {
    std::uint64_t digest = 0;
    std::uint64_t bars = 0;
    std::uint64_t opens = 0;
    std::uint64_t inputs = 0;
    std::uint64_t series = 0;
    std::uint64_t prints = 0;
    std::uint64_t driver_rows = 0;
    std::uint64_t other_rows = 0;
    int trades = 0;
    std::string error;
};

Observed finish(Observer& host) {
    Observed out;
    Fold& fold = host.fold;
    for (const NativeMarketEvent& event : host.native_events(0)) {
        if (!event.driver) {
            ++out.other_rows;
            continue;
        }
        ++out.driver_rows;
        const NativeDriverPoint& point = *event.driver;
        fold.byte(6);
        fold.u64(event.ordinal);
        fold.coordinate(point.coordinate);
        fold.real(point.raw_price);
        fold.flag(point.sequence.has_value());
        fold.u64(point.sequence.value_or(0));
        fold.flag(point.matching);
        fold.flag(point.excursion);
    }
    out.trades = host.trade_count();
    for (int i = 0; i < out.trades; ++i) {
        const Trade& trade = host.get_trade(i);
        fold.byte(7);
        fold.i64(trade.entry_time);
        fold.i64(trade.exit_time);
        fold.real(trade.entry_price);
        fold.real(trade.exit_price);
        fold.real(trade.qty);
    }
    out.error = host.last_error();
    fold.byte(8);
    fold.byte(static_cast<std::uint8_t>(host.native_state().kind));
    fold.text(out.error);
    out.digest = fold.h;
    out.bars = host.bars;
    out.opens = host.opens;
    out.inputs = host.inputs;
    out.series = host.series;
    out.prints = host.prints;
    return out;
}

// A deterministic walk in quarter steps: every price is an exact binary
// fraction, so every folded double is reproducible to the bit.
Bar bar_at(std::int64_t timestamp, int index) {
    const double base = 100.0 + 0.25 * static_cast<double>((index * 7) % 23);
    Bar bar{};
    bar.open = base;
    bar.close = base + 0.25 * static_cast<double>((index % 5) - 2);
    bar.high = (bar.open > bar.close ? bar.open : bar.close) + 0.5;
    bar.low = (bar.open < bar.close ? bar.open : bar.close) - 0.5;
    bar.volume = 1.0 + static_cast<double>(index % 3);
    bar.timestamp = timestamp;
    return bar;
}

// `count` bars `step` apart from `first`, skipping every timestamp `skip`
// rejects.
template <typename Skip>
std::vector<Bar> ladder(std::int64_t first, std::int64_t step, int count, Skip skip) {
    std::vector<Bar> bars;
    int index = 0;
    for (std::int64_t ts = first; static_cast<int>(bars.size()) < count; ts += step) {
        if (skip(ts)) continue;
        bars.push_back(bar_at(ts, index++));
    }
    return bars;
}

std::vector<Bar> ladder(std::int64_t first, std::int64_t step, int count) {
    return ladder(first, step, count, [](std::int64_t) { return false; });
}

NativeRunSpec spec_for(const char* key, const char* timezone, const char* session,
                       const char* input_tf, const char* script_tf) {
    NativeRunSpec spec;
    // Reads its whole event record once the run has ended (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    spec.identity = {key, 1};
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.ticker = "K1";
    spec.tickerid = "TEST:K1";
    spec.type = "stock";
    spec.currency = "USD";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    return spec;
}

Observed batch(const NativeRunSpec& spec, const std::vector<Bar>& bars, int period) {
    Observer host;
    host.period = period;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
    return finish(host);
}

// Warm up on the first `warmup` bars, push the rest one by one.
Observed bar_stream(const NativeRunSpec& spec, const std::vector<Bar>& bars, int warmup,
                    int period) {
    Observer host;
    host.period = period;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(host.stream_begin(bars.data(), warmup, spec.input_tf, spec.script_tf));
    for (std::size_t i = static_cast<std::size_t>(warmup); i < bars.size(); ++i) {
        CHECK(host.stream_push_bar(bars[i]));
    }
    CHECK(host.stream_end(false));
    return finish(host);
}

// America/New_York wall clock on the two dates the scenarios use, as UTC.
// 2024-11-01 (EDT, UTC-4) .. 2024-11-03 02:00 EDT, EST (UTC-5) after it.
constexpr std::int64_t kNyFri0930Edt = 1730467800000LL;   // 2024-11-01 09:30 EDT
constexpr std::int64_t kNyMon0930Est = 1730730600000LL;   // 2024-11-04 09:30 EST

// America/New_York local minute-of-day of `ms` in November 2024.
int ny_minute(std::int64_t ms) {
    const std::int64_t change = 1730613600000LL;  // 2024-11-03 06:00 UTC = 01:00 EST
    const std::int64_t offset = ms < change ? -4 * kHour : -5 * kHour;
    const std::int64_t local = ms + offset;
    return static_cast<int>(((local % kDay) + kDay) % kDay / kMinute);
}

struct Scenario {
    const char* name;
    Observed (*run)();
};

// 1. UTC 24x7, 5m, across 2024-12-31 -> 2025-01-01.
Observed utc_year_end() {
    const auto bars = ladder(1735588800000LL, 5 * kMinute, 1200);
    return batch(spec_for("k1-utc", "UTC", "24x7", "5", "5"), bars, 37);
}

// 2. A masked RTH America/New_York session, 5m, Friday 2024-11-01 across the
// weekend and the 2024-11-03 clock change to Wednesday, with holes in the
// feed: the lunch half hour of every day and the whole Saturday.
Observed ny_masked_dst() {
    const auto bars = ladder(kNyFri0930Edt, 5 * kMinute, 1400, [](std::int64_t ts) {
        const int minute = ny_minute(ts);
        const bool lunch = minute >= 12 * 60 && minute < 12 * 60 + 30;
        const bool saturday = ts >= 1730433600000LL + kDay && ts < 1730433600000LL + 2 * kDay;
        return lunch || saturday;
    });
    return batch(spec_for("k1-ny", "America/New_York", "0930-1600:23456", "5", "5"), bars, 29);
}

// 3. Asia/Tokyo with a lunch break, 1m, Monday 2025-01-06 08:00 JST on.
Observed tokyo_lunch() {
    const auto bars = ladder(1736118000000LL, kMinute, 1200);
    return batch(spec_for("k1-tokyo", "Asia/Tokyo", "0900-1130,1230-1500", "1", "1"), bars, 41);
}

// 4. Europe/London 15m across the 2025-03-30 clock change.
Observed london_dst() {
    const auto bars = ladder(1743148800000LL, 15 * kMinute, 400);
    return batch(spec_for("k1-london", "Europe/London", "0800-1630", "15", "15"), bars, 17);
}

// 5. A POSIX rule zone and an overnight session, hourly, across 2025-03-09.
Observed posix_overnight() {
    const auto bars = ladder(1741388400000LL, kHour, 140);
    return batch(spec_for("k1-posix", "EST5EDT,M3.2.0,M11.1.0", "1800-1700", "60", "60"), bars,
                 7);
}

// 6. A fixed offset, 5m.
Observed fixed_offset() {
    const auto bars = ladder(1736135100000LL, 5 * kMinute, 900);
    return batch(spec_for("k1-fixed", "UTC+05:30", "0915-1530", "5", "5"), bars, 23);
}

// 7. A daily script over hourly input (calendar periods) on the masked RTH
// session, across the clock change.
Observed ny_daily_over_hourly() {
    const auto bars = ladder(kNyFri0930Edt, kHour, 300);
    return batch(spec_for("k1-daily", "America/New_York", "0930-1600:23456", "60", "D"), bars, 3);
}

// 8. A declared hourly series over 5m RTH input, in-session bars only.
Observed ny_hourly_series() {
    auto spec = spec_for("k1-series", "America/New_York", "0930-1600", "5", "5");
    NativeTimeframeSubscription hourly;
    hourly.tf = "60";
    spec.subscriptions.push_back(hourly);
    const auto bars = ladder(kNyMon0930Est, 5 * kMinute, 234, [](std::int64_t ts) {
        const int minute = ny_minute(ts);
        return minute < 9 * 60 + 30 || minute >= 16 * 60;
    });
    return batch(spec, bars, 19);
}

// 9. FeedTolerant labels over an aggregating 5 -> 15 pairing, UTC.
Observed tolerant_aggregating() {
    auto spec = spec_for("k1-tolerant", "UTC", "24x7", "5", "15");
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    const auto bars = ladder(1735588800000LL, 5 * kMinute, 600);
    return batch(spec, bars, 11);
}

// RTH 15m bars, Monday 2024-11-04 from 09:30 EST, in-session only.
std::vector<Bar> rth_quarters(int count) {
    return ladder(kNyMon0930Est, 15 * kMinute, count, [](std::int64_t ts) {
        const int minute = ny_minute(ts);
        return minute < 9 * 60 + 30 || minute >= 16 * 60;
    });
}

// 10. A FeedTolerant 15 -> 15 stream crossing the closed night.
Observed tolerant_stream_night() {
    auto spec = spec_for("k1-tstream", "America/New_York", "0930-1600", "15", "15");
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    const auto bars = rth_quarters(34);
    return bar_stream(spec, bars, 20, 5);
}

// 11. A canonical 5 -> 5 stream crossing the closed night.
Observed canonical_stream_night() {
    const auto spec = spec_for("k1-cstream", "America/New_York", "0930-1600", "5", "5");
    const auto bars = ladder(kNyMon0930Est, 5 * kMinute, 110, [](std::int64_t ts) {
        const int minute = ny_minute(ts);
        return minute < 9 * 60 + 30 || minute >= 16 * 60;
    });
    return bar_stream(spec, bars, 66, 9);
}

// 12. Prints on a 1 -> 5 RTH stream: warmup to Monday 15:49, prints every
// 20 s with a quiet 15:52-15:54, the clock past the close, prints again on
// Tuesday from 09:30, and the partial final slot finalized at the end.
Observed tick_stream() {
    Observer host;
    host.period = 4;
    const auto spec = spec_for("k1-ticks", "America/New_York", "0930-1600", "1", "5");
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const std::int64_t monday_1500 = kNyMon0930Est + 330 * kMinute;
    const auto warmup = ladder(monday_1500, kMinute, 50);
    CHECK(host.stream_begin(warmup.data(), static_cast<int>(warmup.size()), "1", "5"));
    std::uint64_t sequence = 0;
    int index = 0;
    const auto print = [&](std::int64_t ts) {
        const double price = 100.0 + 0.25 * static_cast<double>((index++ * 5) % 17);
        CHECK(host.stream_push_tick(TradeTick{ts, ++sequence, price, 1.0}));
    };
    for (std::int64_t ts = monday_1500 + 50 * kMinute + 5000; ts < monday_1500 + 60 * kMinute;
         ts += 20'000) {
        const std::int64_t minute = (ts - monday_1500) / kMinute;
        if (minute >= 52 && minute < 55) continue;
        print(ts);
    }
    CHECK(host.stream_advance_time(monday_1500 + 90 * kMinute));
    const std::int64_t tuesday_0930 = kNyMon0930Est + kDay;
    for (std::int64_t ts = tuesday_0930 + 10'000; ts < tuesday_0930 + 13 * kMinute;
         ts += 20'000) {
        print(ts);
    }
    CHECK(host.stream_end(true));
    return finish(host);
}

// The calendar-derived facts of every bar, without the event ordinals and the
// decision floor a reused host carries over from its earlier runs.
struct Facts final : NativeStrategyHost {
    Fold fold;
    std::uint64_t bars = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        ++bars;
        fold.bar(bar);
        fold.interval(context.input_interval);
        fold.interval(context.script_interval);
        fold.i64(context.coordinate.open_ms);
        fold.i64(context.coordinate.eligible_open_ms);
        fold.i64(context.coordinate.last_traded_close_ms);
        fold.i64(context.coordinate.next_period_open_ms);
        fold.i64(context.coordinate.next_input_open_ms);
        fold.flag(context.in_session);
        fold.flag(context.opens_session_day);
        fold.flag(context.closes_session_day);
        fold.flag(context.closes_session_day_open_ended);
        fold.i64(context.sub_bar_open_ms);
        fold.i64(context.script_bar_open_ms);
    }
};

void a_reconfigured_calendar_answers_for_itself() {
    const auto bars = ladder(kNyMon0930Est, 5 * kMinute, 600);
    auto new_york = spec_for("k1-swap", "America/New_York", "0930-1600:23456", "5", "5");
    auto tokyo = spec_for("k1-swap", "Asia/Tokyo", "0900-1130,1230-1500", "5", "5");
    tokyo.identity.run_number = 2;

    Facts reused;
    CHECK(reused.configure_native(new_york).status == NativeSetupStatus::Applied);
    reused.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(reused.last_error().empty());
    reused.fold = Fold{};
    reused.bars = 0;
    CHECK(reused.configure_native(tokyo).status == NativeSetupStatus::Applied);
    reused.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(reused.last_error().empty());

    Facts fresh;
    CHECK(fresh.configure_native(tokyo).status == NativeSetupStatus::Applied);
    fresh.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(fresh.last_error().empty());

    CHECK(fresh.bars == bars.size());
    CHECK(reused.bars == fresh.bars);
    CHECK(reused.fold.h == fresh.fold.h);
}

const Scenario kScenarios[] = {
    {"utc_year_end", utc_year_end},
    {"ny_masked_dst", ny_masked_dst},
    {"tokyo_lunch", tokyo_lunch},
    {"london_dst", london_dst},
    {"posix_overnight", posix_overnight},
    {"fixed_offset", fixed_offset},
    {"ny_daily_over_hourly", ny_daily_over_hourly},
    {"ny_hourly_series", ny_hourly_series},
    {"tolerant_aggregating", tolerant_aggregating},
    {"tolerant_stream_night", tolerant_stream_night},
    {"canonical_stream_night", canonical_stream_night},
    {"tick_stream", tick_stream},
};

struct Pinned {
    const char* name;
    std::uint64_t digest;
    std::uint64_t bars;
    std::uint64_t opens;
    std::uint64_t inputs;
    std::uint64_t series;
    std::uint64_t prints;
    std::uint64_t driver_rows;
    std::uint64_t other_rows;
    int trades;
};

#ifndef PINEFORGE_K1_HARVEST
// K-IDX Option A re-pins the v19 coordinate witness once.
const Pinned kPinned[] = {
    // PINNED-BEGIN
    {"utc_year_end", 0xb941690ef38812a2ULL, 1200, 1200, 1200, 0, 0, 4800, 112, 16},
    {"ny_masked_dst", 0xa01f619809a8a8bcULL, 1400, 1400, 1400, 0, 0, 5600, 168, 24},
    {"tokyo_lunch", 0x2ec04b88881e49acULL, 1200, 1200, 1200, 0, 0, 4800, 101, 14},
    {"london_dst", 0xfa652480d4640d2eULL, 400, 400, 400, 0, 0, 1600, 80, 11},
    {"posix_overnight", 0xc07057fe81564f56ULL, 140, 140, 140, 0, 0, 560, 67, 9},
    {"fixed_offset", 0x484ca66d57dc239dULL, 900, 900, 900, 0, 0, 3600, 136, 19},
    // expectation corrected (KERNEL-EDGE, v19): the calendar-closed final
    // daily bucket is calculated at batch end, including its last trade.
    // expectation corrected (INT25): digest 0xfa85a7f9a92aea39 -> 0xe4365dfe9a898fbd,
    // harvested once on the INT25 tree (PINEFORGE_K1_HARVEST): KERNEL-EDGE's
    // final calendar-closed bucket and K-IDX's script-bar coordinate fold meet
    // in this scenario, and each lane pinned a tree without the other. The
    // calculated bars, opens, inputs, series, prints, driver and other rows
    // and trades did not move.
    {"ny_daily_over_hourly", 0xe4365dfe9a898fbdULL, 13, 13, 300, 0, 0, 52, 14, 2},
    {"ny_hourly_series", 0x284026ee6aa32186ULL, 234, 234, 234, 21, 0, 936, 42, 6},
    {"tolerant_aggregating", 0xfa838ffaa5666c82ULL, 200, 200, 600, 0, 0, 800, 63, 9},
    {"tolerant_stream_night", 0x8aa785e43e2916d0ULL, 34, 34, 34, 0, 0, 136, 21, 3},
    {"canonical_stream_night", 0x5f0cc60fc521a1a2ULL, 110, 110, 110, 0, 0, 440, 42, 6},
    {"tick_stream", 0x24a2c575b74ac52aULL, 14, 10, 50, 0, 60, 103, 10, 1},
    // PINNED-END
};
#endif

}  // namespace

int main() {
#ifdef PINEFORGE_K1_HARVEST
    for (const Scenario& scenario : kScenarios) {
        const Observed o = scenario.run();
        if (!o.error.empty()) {
            std::fprintf(stderr, "%s: last_error %s\n", scenario.name, o.error.c_str());
        }
        std::printf("    {\"%s\", 0x%016llxULL, %llu, %llu, %llu, %llu, %llu, %llu, %llu, %d},\n",
                    scenario.name, static_cast<unsigned long long>(o.digest),
                    static_cast<unsigned long long>(o.bars),
                    static_cast<unsigned long long>(o.opens),
                    static_cast<unsigned long long>(o.inputs),
                    static_cast<unsigned long long>(o.series),
                    static_cast<unsigned long long>(o.prints),
                    static_cast<unsigned long long>(o.driver_rows),
                    static_cast<unsigned long long>(o.other_rows), o.trades);
    }
    return failures == 0 ? 0 : 1;
#else
    constexpr std::size_t kCount = sizeof kScenarios / sizeof kScenarios[0];
    CHECK(sizeof kPinned / sizeof kPinned[0] == kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        const Scenario& scenario = kScenarios[i];
        const Pinned& pinned = kPinned[i];
        CHECK(std::strcmp(scenario.name, pinned.name) == 0);
        const Observed o = scenario.run();
        const bool same = o.digest == pinned.digest && o.bars == pinned.bars
            && o.opens == pinned.opens && o.inputs == pinned.inputs
            && o.series == pinned.series && o.prints == pinned.prints
            && o.driver_rows == pinned.driver_rows && o.other_rows == pinned.other_rows
            && o.trades == pinned.trades;
        if (!same) {
            std::fprintf(stderr,
                         "%s: observed 0x%016llx bars=%llu opens=%llu inputs=%llu series=%llu "
                         "prints=%llu driver=%llu other=%llu trades=%d error='%s'\n",
                         scenario.name, static_cast<unsigned long long>(o.digest),
                         static_cast<unsigned long long>(o.bars),
                         static_cast<unsigned long long>(o.opens),
                         static_cast<unsigned long long>(o.inputs),
                         static_cast<unsigned long long>(o.series),
                         static_cast<unsigned long long>(o.prints),
                         static_cast<unsigned long long>(o.driver_rows),
                         static_cast<unsigned long long>(o.other_rows), o.trades,
                         o.error.c_str());
        }
        CHECK(same);
    }
    a_reconfigured_calendar_answers_for_itself();
    if (failures == 0) {
        std::printf("test_native_calendar_hash_witness: %d checks, %zu scenarios ok, a "
                    "reconfigured calendar answers for itself\n", checks, kCount);
    }
    return failures == 0 ? 0 : 1;
#endif
}
