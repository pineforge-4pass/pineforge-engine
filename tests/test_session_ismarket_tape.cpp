/*
 * test_session_ismarket_tape.cpp — R5 wave H, lane H-MEASURE, row G2-36.
 *
 * Generated Pine code reads `session.ismarket` through the legacy
 * time-of-day predicate `pine_session_ismarket` (include/pineforge/
 * session_time.hpp, via PineStrategyHost's class-scope wrapper), while the
 * kernel answers the same question as a session-day fact
 * (NativeDecisionContext::in_session, which the Pine host also stores as
 * session_ismarket_ before every source callback). The two read a session's
 * day mask differently: the predicate tests each instant's own weekday, the
 * kernel tests the trading date of the session day the instant belongs to.
 *
 * TradingView's answer (tests/fixtures/session_ismarket, six `lab tv` tapes,
 * channel ws-report-v1, rangeProof covered): the probe reverses its position
 * at the close of EVERY chart bar (process_orders_on_close), so every entry is
 * one chart bar, dated at its open, and its Signal is the flag TradingView
 * evaluated there ("M1" in market, "M0" not). On all six tapes — CME_MINI:ES1!
 * 60 across both 2025 US DST switches and the Thanksgiving week, OANDA:EURUSD
 * 60, OANDA:XAUUSD 60 and BINANCE:ETHUSDT.P 60 — every one of the 1138 bars
 * is "M1", the Sunday-evening CME/FX opens included.
 *
 * What each reading gives on those bars, per host session string:
 *   1. the campaign's lane facts (pineforge-lab config/symbol-lanes-v1.json:
 *      ES1! "1700-1600" America/Chicago, EURUSD "1700-1700" and XAUUSD
 *      "1800-1700" America/New_York, 24x7 UTC for the crypto perp): the
 *      predicate, the kernel fact and TradingView agree on every bar;
 *   2. the same sessions spelled with TradingView's weekday mask ":23456":
 *      the kernel still agrees on every bar; the predicate reads each Sunday
 *      evening open (17:00 CT on ES1!, 17:00 / 18:00 ET on EURUSD / XAUUSD)
 *      as out of market, because the instant's weekday is Sunday while the
 *      session day it opens is Monday's;
 *   3. a 24-hour day spelled "0000-2400": the kernel reads every bar in
 *      session, the predicate none ("2400" is not a time of day it parses);
 *      the TradingView spelling "0000-0000" is the control both read as 24h.
 * The predicate's disagreements are pinned bar for bar, so a lane that routes
 * generated session.ismarket to the kernel fact moves exactly these rows.
 *
 * Bars: session.ismarket is a function of the bar's time and the symbol's
 * session and timezone only, so each tape is replayed on flat bars stamped at
 * the tape's own entry times (every bar TradingView held on that chart).
 */

#include <pineforge/bar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
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

#ifndef PINEFORGE_G236_FIXTURE_DIR
#error "PINEFORGE_G236_FIXTURE_DIR must name tests/fixtures/session_ismarket"
#endif

namespace {

constexpr std::int64_t kMinute = 60'000;

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" at a fixed UTC offset in hours -> UTC milliseconds.
std::int64_t utc_ms(int y, int mo, int d, int h, int mi, int offset_hours) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

struct TapeBar {
    std::int64_t ts = 0;   // the chart bar's open, UTC ms
    bool market = false;   // TradingView's session.ismarket on it
};

// The tape's Entry rows (times UTC+8, the lab tv rendering): one per chart
// bar, in time order; its Signal is the flag.
std::vector<TapeBar> read_tape(const char* slug) {
    std::ifstream in(std::string(PINEFORGE_G236_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::vector<TapeBar> bars;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 4 || cell[1].rfind("Entry", 0) != 0) continue;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        TapeBar bar;
        bar.ts = utc_ms(y, mo, d, h, mi, 8);
        bar.market = cell[3] == "M1";
        CHECK(cell[3] == "M1" || cell[3] == "M0");
        bars.push_back(bar);
    }
    // The export lists trades newest first on some channels; order by time.
    for (std::size_t i = 1; i < bars.size(); ++i)
        for (std::size_t j = i; j > 0 && bars[j - 1].ts > bars[j].ts; --j)
            std::swap(bars[j - 1], bars[j]);
    return bars;
}

Bar flat_bar(std::int64_t ts) {
    Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

struct Seen {
    std::int64_t ts = 0;
    bool generated = false;  // what generated `session.ismarket` evaluates
    bool kernel = false;     // the kernel's in-session fact the host stores
};

class IsMarketHost final : public source::PineStrategyHost {
public:
    IsMarketHost(const std::string& session, const std::string& timezone) {
        set_syminfo_session(session);
        set_syminfo_timezone(timezone);
    }
    void on_source_bar(const Bar&) override {
        // The call generated code emits for `session.ismarket`
        // (corpus generated.cpp): the class-scope wrapper over
        // session_time.hpp's predicate, at the presented bar's time.
        seen.push_back({current_bar_.timestamp,
                        pine_session_ismarket(syminfo_.session, syminfo_.timezone,
                                              current_bar_.timestamp),
                        session_ismarket_});
    }
    std::vector<Seen> seen;
};

// The kernel alone: a bare host reading NativeDecisionContext::in_session.
class BareHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar& bar, const NativeDecisionContext& ctx) override {
        stamps.push_back(bar.timestamp);
        in_session.push_back(ctx.in_session);
    }
    std::vector<std::int64_t> stamps;
    std::vector<bool> in_session;
};

int local_weekday(std::int64_t ts, const std::string& tz) {
    return pineforge::pine_dayofweek(ts, tz);  // 1 = Sunday
}

struct Case {
    const char* slug;
    const char* session;
    const char* timezone;
    int generated_misses;   // bars where the predicate differs from TradingView
    int sunday_misses;      // of which on a local Sunday
};

void replay(const Case& c) {
    const std::vector<TapeBar> tape = read_tape(c.slug);
    std::vector<Bar> bars;
    for (const TapeBar& t : tape) bars.push_back(flat_bar(t.ts));

    IsMarketHost pine(c.session, c.timezone);
    pine.run(bars.data(), static_cast<int>(bars.size()), "60", "60");

    NativeRunSpec spec;
    spec.identity.session_key = std::string("g236-") + c.slug;
    spec.identity.run_number = 1;
    spec.input_tf = "60"; spec.script_tf = "60";
    spec.ticker = "MOCK"; spec.tickerid = "TEST:MOCK"; spec.type = "futures";
    spec.currency = "USD"; spec.timezone = c.timezone; spec.session = c.session;
    spec.initial_capital = 1000000.0; spec.point_value = 1.0; spec.account_fx = 1.0;
    spec.price_tick = 0.01; spec.fee_kind = NativeFeeKind::Percent; spec.fee_value = 0.0;
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    BareHost bare;
    const bool configured = bare.configure_native(spec).status == NativeSetupStatus::Applied;
    CHECK(configured);
    if (configured) bare.run(bars.data(), static_cast<int>(bars.size()));

    int tv_market = 0, kernel_agree = 0, bare_agree = 0, generated_agree = 0;
    int generated_misses = 0, sunday_misses = 0;
    const bool aligned = pine.seen.size() == tape.size() && bare.stamps.size() == tape.size();
    CHECK(pine.last_error().empty());
    CHECK(bare.last_error().empty());
    CHECK(aligned);
    if (aligned) {
        for (std::size_t i = 0; i < tape.size(); ++i) {
            CHECK(pine.seen[i].ts == tape[i].ts);
            CHECK(bare.stamps[i] == tape[i].ts);
            tv_market += tape[i].market;
            kernel_agree += pine.seen[i].kernel == tape[i].market;
            bare_agree += bare.in_session[i] == tape[i].market;
            if (pine.seen[i].generated == tape[i].market) {
                ++generated_agree;
            } else {
                ++generated_misses;
                if (local_weekday(tape[i].ts, c.timezone) == 1) ++sunday_misses;
            }
        }
    }
    const int n = static_cast<int>(tape.size());
    std::printf("  %-28s %-17s %-17s bars=%d tv_M1=%d kernel==tv %d bare==tv %d generated==tv %d"
                " (misses %d, on a local Sunday %d)\n",
                c.slug, c.session, c.timezone, n, tv_market, kernel_agree, bare_agree,
                generated_agree, generated_misses, sunday_misses);
    if (!pine.last_error().empty()) std::printf("    pine last_error: %s\n", pine.last_error().c_str());
    if (!bare.last_error().empty()) std::printf("    bare last_error: %s\n", bare.last_error().c_str());
    // TradingView flags every chart bar of these regular-session charts.
    CHECK(tv_market == n);
    // The kernel fact is TradingView's on every bar, whatever the spelling.
    CHECK(kernel_agree == n);
    CHECK(bare_agree == n);
    // The predicate generated code calls: exactly the pinned misses.
    CHECK(generated_misses == c.generated_misses);
    CHECK(sunday_misses == c.sunday_misses);
}

}  // namespace

int main() {
    std::printf("TradingView session.ismarket vs the generated predicate vs the kernel fact\n");
    const Case cases[] = {
        // 1. the campaign's lane facts: everyone agrees.
        {"hm-g236-es1-60-dst-mar", "1700-1600", "America/Chicago", 0, 0},
        {"hm-g236-es1-60-dst-nov", "1700-1600", "America/Chicago", 0, 0},
        {"hm-g236-es1-60-thanksgiving", "1700-1600", "America/Chicago", 0, 0},
        {"hm-g236-eurusd-60-dst-mar", "1700-1700", "America/New_York", 0, 0},
        {"hm-g236-xauusd-60-dst-mar", "1800-1700", "America/New_York", 0, 0},
        {"hm-g236-eth-60-24x7", "24x7", "UTC", 0, 0},
        // 2. the same sessions with a weekday mask: the predicate misses
        //    every Sunday-evening open.
        {"hm-g236-es1-60-dst-mar", "1700-1600:23456", "America/Chicago", 14, 14},
        {"hm-g236-es1-60-dst-nov", "1700-1600:23456", "America/Chicago", 14, 14},
        {"hm-g236-es1-60-thanksgiving", "1700-1600:23456", "America/Chicago", 14, 14},
        {"hm-g236-eurusd-60-dst-mar", "1700-1700:23456", "America/New_York", 14, 14},
        {"hm-g236-xauusd-60-dst-mar", "1800-1700:23456", "America/New_York", 12, 12},
        // 3. a 24-hour day: "0000-2400" (predicate: never) and the
        //    TradingView spelling "0000-0000" (both: always).
        {"hm-g236-eth-60-24x7", "0000-2400", "UTC", 97, 24},
        {"hm-g236-eth-60-24x7", "0000-0000", "UTC", 0, 0},
    };
    for (const Case& c : cases) replay(c);
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
