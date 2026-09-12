#include <pineforge/native_calendar.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

using namespace pineforge::native_calendar;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto _a = (actual);                                              \
        const auto _e = (expected);                                            \
        if (_a != _e) {                                                        \
            std::printf("  FAIL  %s:%d  %s == %s  (got %lld, want %lld)\n",    \
                        __FILE__, __LINE__, #actual, #expected,                \
                        static_cast<long long>(_a), static_cast<long long>(_e)); \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

// Independent America/New_York zoneinfo epochs (NY-DST-ORACLE.json and the
// same Python zoneinfo source). Not taken from native_calendar or from
// calendar_day_open_local_ms.
constexpr int64_t kNyMar8Midnight = 1741410000000;     // 2025-03-08 00:00 EST 24h control
constexpr int64_t kNyMar9Midnight = 1741496400000;     // 2025-03-09 00:00 EST 23h
constexpr int64_t kNyMar10Midnight = 1741579200000;    // 2025-03-10 00:00 EDT
constexpr int64_t kNyNov1Midnight = 1761969600000;     // 2025-11-01 00:00 EDT 24h control
constexpr int64_t kNyNov2Midnight = 1762056000000;     // 2025-11-02 00:00 EDT 25h
constexpr int64_t kNyNov3Midnight = 1762146000000;     // 2025-11-03 00:00 EST
constexpr int64_t kNyMar8_1700 = 1741471200000;        // 2025-03-08 17:00 EST
constexpr int64_t kNyMar9_1700 = 1741554000000;        // 2025-03-09 17:00 EDT
constexpr int64_t kNyMar8_1800 = 1741474800000;        // 2025-03-08 18:00 EST
constexpr int64_t kNyMar8_2200 = 1741489200000;        // 2025-03-08 22:00 EST
constexpr int64_t kNyMar9_0300 = 1741503600000;        // 2025-03-09 03:00 EDT (first >= 02:00)
constexpr int64_t kNyMar9_0030 = 1741498200000;        // 2025-03-09 00:30 EST
constexpr int64_t kNyMar9_1200 = 1741536000000;        // 2025-03-09 12:00 EDT
constexpr int64_t kNyMar9_0159 = 1741503540000;        // 2025-03-09 01:59 EST
constexpr int64_t kNyNov2_0030 = 1762057800000;        // 2025-11-02 00:30 EDT
constexpr int64_t kNyNov2_1200 = 1762102800000;        // 2025-11-02 12:00 EST
constexpr int64_t kNyNov2_0100Early = 1762059600000;   // 2025-11-02 01:00 EDT fold=0
constexpr int64_t kNyNov2_0100Late = 1762063200000;    // 2025-11-02 01:00 EST fold=1
constexpr int64_t kNyNov2_0130Early = 1762061400000;   // 2025-11-02 01:30 EDT
constexpr int64_t kNyNov2_0200 = 1762066800000;        // 2025-11-02 02:00 EST
constexpr int64_t kNyMar7_0930 = 1741357800000;        // Fri 2025-03-07 09:30 EST
constexpr int64_t kNyMar7_1600 = 1741381200000;        // Fri 2025-03-07 16:00 EST
constexpr int64_t kNyMar10_0930 = 1741613400000;       // Mon 2025-03-10 09:30 EDT
constexpr int64_t kNyJun9_0930 = 1749475800000;
constexpr int64_t kNyJun9_1000 = 1749477600000;
constexpr int64_t kNyJun10_0930 = 1749562200000;
constexpr int64_t kNyJun10_1000 = 1749564000000;
constexpr int64_t kNyJun10_1100 = 1749567600000;
constexpr int64_t kNyJun10_1130 = 1749569400000;
constexpr int64_t kNyJun10_1200 = 1749571200000;
constexpr int64_t kNyJun10_1300 = 1749574800000;
constexpr int64_t kNyJun10_1330 = 1749576600000;
constexpr int64_t kNyJun10_1600 = 1749585600000;
constexpr int64_t kNyJun13_0930 = 1749821400000;
constexpr int64_t kNyJun13_1600 = 1749844800000;
constexpr int64_t kNyJun16_0930 = 1750080600000;
constexpr int64_t kNyJun7_1000 = 1749304800000;        // Saturday
constexpr int64_t kNyJun8_1800 = 1749420000000;
constexpr int64_t kNyJun9_1700 = 1749502800000;
constexpr int64_t kNyJun9_1800 = 1749506400000;
constexpr int64_t kNyJan1 = 1735707600000;
constexpr int64_t kNyJan2 = 1735794000000;
constexpr int64_t kNyJan3 = 1735880400000;
constexpr int64_t kNyJan6 = 1736139600000;
constexpr int64_t kNyJan13 = 1736744400000;
constexpr int64_t kNyJan20 = 1737349200000;
constexpr int64_t kNyJan13_0930 = 1736778600000;
constexpr int64_t kNyJan20_0930 = 1737383400000;
constexpr int64_t kNyFeb15 = 1739595600000;
constexpr int64_t kNyMar15 = 1742011200000;
constexpr int64_t kNyApr1 = 1743480000000;
constexpr int64_t kNyApr15 = 1744689600000;
constexpr int64_t kNy1969Dec30 = -154800000;
constexpr int64_t kNy1969Dec31 = -68400000;

// Independent UTC civil epochs (calendar-codex-audit/independent-oracle.json
// and the same datetime(timezone.utc) source).
constexpr int64_t kUtcJun9_1300 = 1749474000000;
constexpr int64_t kUtcJun9_1800 = 1749492000000;
constexpr int64_t kUtcJun10_095959 = 1749549599000;
constexpr int64_t kUtcJun10_1000 = 1749549600000;
constexpr int64_t kUtcJun10_1100 = 1749553200000;
constexpr int64_t kUtcJun10_1130 = 1749555000000;
constexpr int64_t kUtcJun10_1230 = 1749558600000;
constexpr int64_t kUtcJun10_1300 = 1749560400000;
constexpr int64_t kUtcJun10_1700 = 1749574800000;
constexpr int64_t kUtcJun7_1800 = 1749319200000;
constexpr int64_t kNyJun8_1300 = 1749402000000;       // Sunday 13:00 EDT
constexpr int64_t kNyJun8_1330 = 1749403800000;       // Sunday 13:30 EDT
constexpr int64_t kNyJun9_1130 = 1749483000000;       // Monday 11:30 EDT
constexpr int64_t kNyJun7_1330 = 1749317400000;       // Saturday 13:30 EDT
constexpr int64_t kUtcJun9_0900 = 1749459600000;
constexpr int64_t kUtcJun9_1430 = 1749479400000;
constexpr int64_t kUtcJun9_1530 = 1749483000000;
constexpr int64_t kUtcJun9_1600 = 1749484800000;
constexpr int64_t kUtcJun10_0900 = 1749546000000;
constexpr int64_t kNyMar9_0930 = 1741527000000;
constexpr int64_t kNyMar9_1600 = 1741550400000;
constexpr int64_t kNyMar10_0245 = 1741589100000;  // Mon 02:45 EDT, after DST
constexpr int64_t kUtcJun8_1200 = 1749384000000;       // Sunday 2025-06-08 12:00
constexpr int64_t kUtcJun8_0930 = 1749375000000;
constexpr int64_t kUtcJun9_0930 = 1749461400000;
constexpr int64_t kUtcJun7_1200 = 1749297600000;       // Saturday 2025-06-07 12:00
constexpr int64_t kUtcJun7_0930 = 1749288600000;
constexpr int64_t kUtcDec31_2023_1200 = 1704024000000;  // Sunday
constexpr int64_t kUtcDec31_2023_0930 = 1704015000000;
constexpr int64_t kUtcJan1_2024_0930 = 1704101400000;
constexpr int64_t kUtcDec25_2023_0930 = 1703496600000;
constexpr int64_t kNyMar9_0230_resolved = 1741503600000;  // first representable ≥ 02:30
constexpr int64_t kNyMar10_0230 = 1741588200000;
constexpr int64_t kUtcMar8_0000 = 1741392000000;   // 2025-03-08 00:00 UTC
constexpr int64_t kUtcJun10_0000 = 1749513600000;
constexpr int64_t kUtcJun10_0000_plus0530 = 1749493800000;  // 2025-06-10 00:00 UTC+05:30
constexpr int64_t kUtcJun10_0000_gmt_minus4 = 1749528000000;  // 2025-06-10 00:00 GMT-4
constexpr int64_t kUtcJun10_0000_taipei = 1749484800000;  // 2025-06-10 00:00 UTC+8
constexpr int64_t kUtcJun10_0000_japan = 1749481200000;  // 2025-06-10 00:00 JST (UTC+9)
constexpr int64_t kCstMar8Midnight = 1741413600000;      // 2025-03-08 00:00 CST (UTC-6)

static Timeframe must_tf(const char* s) {
    auto tf = parse_timeframe(s);
    CHECK(tf.has_value());
    return tf ? *tf : Timeframe{};
}

static SessionCalendar must_cal(const char* session, const char* tz) {
    auto cal = parse_session(session, tz);
    CHECK(cal.has_value());
    return cal ? *cal : SessionCalendar{};
}

static NativeInterval must_iv(const SessionCalendar& cal, const Timeframe& tf, int64_t ms) {
    auto iv = interval_containing(cal, tf, ms);
    CHECK(iv.has_value());
    return iv ? *iv : NativeInterval{};
}

static NativeInterval must_iv2(const SessionCalendar& cal,
                               const Timeframe& script,
                               const Timeframe& input,
                               int64_t ms) {
    auto iv = interval_containing(cal, script, input, ms);
    CHECK(iv.has_value());
    return iv ? *iv : NativeInterval{};
}

static void test_timeframe_forms() {
    std::printf("test_timeframe_forms\n");
    const struct {
        const char* text;
        TimeframeUnit unit;
        int count;
    } ok[] = {
        {"15S", TimeframeUnit::Second, 15},
        {"1", TimeframeUnit::Minute, 1},
        {"5", TimeframeUnit::Minute, 5},
        {"60", TimeframeUnit::Minute, 60},
        {"240", TimeframeUnit::Minute, 240},
        {"D", TimeframeUnit::Day, 1},
        {"1D", TimeframeUnit::Day, 1},
        {"2D", TimeframeUnit::Day, 2},
        {"W", TimeframeUnit::Week, 1},
        {"1W", TimeframeUnit::Week, 1},
        {"2W", TimeframeUnit::Week, 2},
        {"M", TimeframeUnit::Month, 1},
        {"1M", TimeframeUnit::Month, 1},
        {"3M", TimeframeUnit::Month, 3},
    };
    for (const auto& c : ok) {
        auto tf = parse_timeframe(c.text);
        CHECK(tf.has_value());
        if (!tf) continue;
        CHECK(tf->unit() == c.unit);
        CHECK_EQ(tf->count(), c.count);
        CHECK(tf->literal() == c.text);
        CHECK(tf->valid());
    }
    // Equal-literal identity: 1D is not rewritten to D.
    CHECK(must_tf("1D").literal() == "1D");
    CHECK(must_tf("D").literal() == "D");
    CHECK(!(must_tf("1D") == must_tf("D")));

    const char* bad[] = {"", " ", " 5", "5 ", "01", "01D", "0", "0D", "S", "1H",
                         "4H", "1d", "2X", "H", "7m", "99999999999999999999",
                         "9223372036854775807", "9223372036854775808",
                         "92233720368547758080", "2147483648"};
    for (const char* s : bad) CHECK(!parse_timeframe(s));
    CHECK(parse_timeframe("2147483647").has_value());
}

static void test_pairings() {
    std::printf("test_pairings\n");
    auto p = [](const char* a, const char* b) {
        return compatibility(must_tf(a), must_tf(b)).pairing;
    };
    auto f = [](const char* a, const char* b) {
        return compatibility(must_tf(a), must_tf(b)).group_factor;
    };
    CHECK(p("D", "D") == TimeframePairing::Passthrough);
    CHECK(p("2D", "2D") == TimeframePairing::Passthrough);
    CHECK(p("W", "W") == TimeframePairing::Passthrough);
    CHECK(p("2W", "2W") == TimeframePairing::Passthrough);
    CHECK(p("M", "M") == TimeframePairing::Passthrough);
    CHECK(p("3M", "3M") == TimeframePairing::Passthrough);
    CHECK(p("1D", "D") == TimeframePairing::SameUnitMultiple);
    CHECK_EQ(f("1D", "D"), 1);
    CHECK(p("1", "5") == TimeframePairing::SameUnitMultiple);
    CHECK_EQ(f("1", "5"), 5);
    CHECK(p("D", "2D") == TimeframePairing::SameUnitMultiple);
    CHECK(p("M", "3M") == TimeframePairing::SameUnitMultiple);
    CHECK_EQ(f("M", "3M"), 3);
    CHECK(p("15S", "1") == TimeframePairing::FixedDivisible);
    CHECK_EQ(f("15S", "1"), 4);
    CHECK(p("1", "D") == TimeframePairing::FixedToCalendar);
    CHECK(p("60", "W") == TimeframePairing::FixedToCalendar);
    CHECK(p("5", "M") == TimeframePairing::FixedToCalendar);
    CHECK(p("D", "W") == TimeframePairing::CalendarToCalendar);
    CHECK(p("D", "M") == TimeframePairing::CalendarToCalendar);
    CHECK(p("W", "M") == TimeframePairing::CalendarToCalendar);
    CHECK(p("2D", "W") == TimeframePairing::CalendarToCalendar);
    CHECK(p("2D", "M") == TimeframePairing::CalendarToCalendar);
    CHECK(p("2W", "3M") == TimeframePairing::CalendarToCalendar);
    CHECK(p("5", "7") == TimeframePairing::IndivisibleFixed);
    CHECK(p("2D", "3D") == TimeframePairing::IndivisibleFixed);
    CHECK(p("60", "1") == TimeframePairing::ScriptFiner);
    CHECK(p("W", "D") == TimeframePairing::ScriptFiner);
    CHECK(p("M", "W") == TimeframePairing::ScriptFiner);
    CHECK(p("3M", "M") == TimeframePairing::ScriptFiner);
    // Baseline tf_ratio("2880","D")==-2 and tf_ratio("14D","W")==-2 (input
    // seconds exceed target). 2D→W stays CalendarToCalendar (tf_ratio==3).
    // 7D→W equal 604800s (tf_ratio==1); 8D→W is -2. 6D→W is tf_ratio==1
    // because 604800/518400 truncates; month targets stay calendar (tf_ratio
    // ==-1) and are not refused with an invented month length.
    CHECK(p("2880", "D") == TimeframePairing::ScriptFiner);
    CHECK(p("14D", "W") == TimeframePairing::ScriptFiner);
    CHECK(p("8D", "W") == TimeframePairing::ScriptFiner);
    CHECK(p("7D", "W") == TimeframePairing::CalendarToCalendar);
    CHECK(p("6D", "W") == TimeframePairing::CalendarToCalendar);
    CHECK(p("1440", "D") == TimeframePairing::FixedToCalendar);
    CHECK(p("14D", "M") == TimeframePairing::CalendarToCalendar);
    CHECK(p("2880", "M") == TimeframePairing::FixedToCalendar);
    CHECK_EQ(compatibility(must_tf("1S"), must_tf("2147483647")).group_factor,
             128849018820LL);

    CHECK(stream_compatibility(must_tf("M"), must_tf("M")).pairing
          == TimeframePairing::StreamMonthlyInputRefused);
    CHECK(stream_compatibility(must_tf("3M"), must_tf("3M")).pairing
          == TimeframePairing::StreamMonthlyInputRefused);
    CHECK(stream_compatibility(must_tf("D"), must_tf("W")).pairing
          == TimeframePairing::CalendarToCalendar);
    CHECK(stream_compatibility(must_tf("2D"), must_tf("2D")).pairing
          == TimeframePairing::Passthrough);
    // Legacy quirk (not native): tf_to_seconds("1H")==60; tf_ratio monthly -1
    // collapsing equal M/3M batch. Native rejects "1H" and keeps M/M passthrough.
}

static void test_session_parse() {
    std::printf("test_session_parse\n");
    auto a = must_cal("24x7", "America/New_York");
    CHECK(a.all_day());
    CHECK_EQ(a.origin_minutes(), 0);
    auto b = must_cal("", "UTC");
    CHECK(b.all_day());
    auto rth = must_cal("0930-1600", "America/New_York");
    CHECK_EQ(rth.origin_minutes(), 9 * 60 + 30);
    CHECK_EQ(static_cast<int>(rth.windows().size()), 1);
    CHECK_EQ(rth.windows()[0].end_minutes(), 16 * 60);
    CHECK(!rth.windows()[0].wraps());
    auto multi = must_cal("0930-1130,1300-1600", "America/New_York");
    CHECK_EQ(multi.origin_minutes(), 9 * 60 + 30);
    CHECK_EQ(static_cast<int>(multi.windows().size()), 2);
    CHECK_EQ(multi.windows()[0].end_minutes(), 11 * 60 + 30);
    CHECK_EQ(multi.windows()[1].end_minutes(), 16 * 60);
    auto ovn = must_cal("1800-1700", "America/New_York");
    CHECK_EQ(ovn.origin_minutes(), 18 * 60);
    CHECK(ovn.windows()[0].wraps());
    auto fx = must_cal("1700-1700", "America/New_York");
    CHECK(fx.windows()[0].full_day());
    CHECK_EQ(fx.origin_minutes(), 17 * 60);
    auto allday2400 = must_cal("0000-2400", "UTC");
    CHECK_EQ(allday2400.windows()[0].end_minutes(), 1440);
    auto masked = must_cal("0930-1600:23456", "America/New_York");
    CHECK((masked.day_mask() & (1u << 1)) == 0);  // Sunday off
    CHECK((masked.day_mask() & (1u << 2)) != 0);  // Monday on
    auto spaced = must_cal("0930-1130, 1300-1600", "America/New_York");
    CHECK_EQ(static_cast<int>(spaced.windows().size()), 2);
    CHECK(!parse_session("0930", "UTC"));
    CHECK(!parse_session("0930-1600:8", "UTC"));
    CHECK(!parse_session("1H-2H", "UTC"));
}

static void test_multi_window_last_close() {
    std::printf("test_multi_window_last_close\n");
    auto cal = must_cal("0930-1130,1300-1600", "America/New_York");
    auto d = must_tf("D");
    CHECK(in_session(cal, kNyJun10_1000));
    CHECK(!in_session(cal, kNyJun10_1200));
    CHECK(in_session(cal, kNyJun10_1330));
    auto iv = must_iv(cal, d, kNyJun10_1000);
    CHECK_EQ(iv.open_ms, kNyJun10_0930);
    CHECK_EQ(iv.last_traded_close_ms, kNyJun10_1600);
    CHECK(iv.last_traded_close_ms != kNyJun10_1130);
}

static void test_overnight_origin_1800() {
    std::printf("test_overnight_origin_1800\n");
    auto cal = must_cal("1800-1700", "America/New_York");
    auto d = must_tf("D");
    CHECK(!in_session(cal, kNyJun9_1700));
    CHECK(in_session(cal, kNyJun8_1800));
    auto iv = must_iv(cal, d, kNyJun8_1800);
    CHECK_EQ(iv.open_ms, kNyJun8_1800);
    CHECK_EQ(iv.last_traded_close_ms, kNyJun9_1700);
    CHECK_EQ(iv.next_period_open_ms, kNyJun9_1800);
    CHECK(iv.open_ms != kNyJun9_1700);
    // Day mask applies to the trading date (Monday), not Sunday's civil weekday.
    auto masked = must_cal("1800-1700:23456", "America/New_York");
    CHECK(in_session(masked, kNyJun8_1800));
    auto miv = must_iv(masked, d, kNyJun8_1800);
    CHECK_EQ(miv.open_ms, kNyJun8_1800);
    auto week = must_iv(cal, must_tf("W"), kNyJun10_1000);
    CHECK_EQ(week.open_ms, kNyJun8_1800);
    // Pine 17:00 day-stamp on 1800-1700 is a legacy/source label, not native origin.
}

static void test_full_day_and_2359() {
    std::printf("test_full_day_and_2359\n");
    auto wrap0 = must_cal("0000-0000", "America/New_York");
    auto d = must_tf("D");
    auto iv = must_iv(wrap0, d, kNyJun10_0930);
    CHECK_EQ(iv.open_ms, 1749528000000);  // 2025-06-10 00:00 EDT (zoneinfo)
    CHECK_EQ(iv.last_traded_close_ms, 1749614400000);  // 2025-06-11 00:00 EDT
    auto h2359 = must_cal("0000-2359", "America/New_York");
    auto iv2 = must_iv(h2359, d, kNyJun10_0930);
    CHECK_EQ(iv2.open_ms, 1749528000000);
    CHECK_EQ(iv2.last_traded_close_ms, 1749614340000);  // 2025-06-10 23:59 EDT
    auto utc = must_cal("24x7", "UTC");
    auto u = must_iv(utc, d, 1749528000000);
    CHECK_EQ(u.open_ms, 1749513600000);  // 2025-06-10 00:00 UTC
}

static void test_weekday_mask_and_gaps() {
    std::printf("test_weekday_mask_and_gaps\n");
    auto cal = must_cal("0930-1600:23456", "America/New_York");
    auto d = must_tf("D");
    CHECK(in_session(cal, kNyJun9_1000));
    CHECK(!in_session(cal, kNyJun7_1000));
    auto fri = must_iv(cal, d, kNyJun13_0930);
    CHECK_EQ(fri.last_traded_close_ms, kNyJun13_1600);
    CHECK_EQ(fri.next_input_open_ms, kNyJun16_0930);
    CHECK(fri.next_input_open_ms != fri.last_traded_close_ms);
}

static void test_rth_friday_vs_monday() {
    std::printf("test_rth_friday_vs_monday\n");
    auto cal = must_cal("0930-1600:23456", "America/New_York");
    auto w = must_tf("W");
    auto d = must_tf("D");
    auto week = must_iv2(cal, w, d, kNyJun10_1000);
    CHECK_EQ(week.open_ms, kNyJun9_0930);
    CHECK_EQ(week.last_traded_close_ms, kNyJun13_1600);
    CHECK_EQ(week.next_period_open_ms, kNyJun16_0930);
    CHECK_EQ(week.next_input_open_ms, kNyJun16_0930);
    CHECK(week.last_traded_close_ms != week.next_period_open_ms);

    auto fri_d = must_iv2(cal, d, must_tf("1"), kNyJun13_1600 - 60000);
    CHECK_EQ(fri_d.last_traded_close_ms, kNyJun13_1600);
    CHECK_EQ(fri_d.next_input_open_ms, kNyJun16_0930);

    // Same split across the 2025 spring DST weekend (Fri EST close, Mon EDT open).
    auto dst_week = must_iv2(cal, w, d, kNyMar7_1600 - 60000);
    CHECK_EQ(must_iv(cal, d, kNyMar7_1600 - 60000).open_ms, kNyMar7_0930);
    CHECK_EQ(dst_week.last_traded_close_ms, kNyMar7_1600);
    CHECK_EQ(dst_week.next_period_open_ms, kNyMar10_0930);
    CHECK_EQ(dst_week.next_input_open_ms, kNyMar10_0930);
    CHECK(dst_week.last_traded_close_ms != dst_week.next_period_open_ms);
}

static void test_dst_oracles() {
    std::printf("test_dst_oracles\n");
    auto cal = must_cal("24x7", "America/New_York");
    auto d = must_tf("D");
    auto sat_s = must_iv(cal, d, kNyMar8Midnight);
    CHECK_EQ(sat_s.open_ms, kNyMar8Midnight);
    CHECK_EQ(sat_s.last_traded_close_ms, kNyMar9Midnight);
    CHECK_EQ(sat_s.last_traded_close_ms - sat_s.open_ms, 86400000);

    auto sun_s = must_iv(cal, d, kNyMar9Midnight);
    CHECK_EQ(sun_s.open_ms, kNyMar9Midnight);
    CHECK_EQ(sun_s.last_traded_close_ms, kNyMar10Midnight);
    CHECK_EQ(sun_s.last_traded_close_ms - sun_s.open_ms, 82800000);

    auto sat_f = must_iv(cal, d, kNyNov1Midnight);
    CHECK_EQ(sat_f.open_ms, kNyNov1Midnight);
    CHECK_EQ(sat_f.last_traded_close_ms, kNyNov2Midnight);
    CHECK_EQ(sat_f.last_traded_close_ms - sat_f.open_ms, 86400000);

    auto sun_f = must_iv(cal, d, kNyNov2Midnight);
    CHECK_EQ(sun_f.open_ms, kNyNov2Midnight);
    CHECK_EQ(sun_f.last_traded_close_ms, kNyNov3Midnight);
    CHECK_EQ(sun_f.last_traded_close_ms - sun_f.open_ms, 90000000);

    auto fx = must_cal("1700-1700", "America/New_York");
    auto c3 = must_iv(fx, d, kNyMar8_1700);
    CHECK_EQ(c3.open_ms, kNyMar8_1700);
    CHECK_EQ(c3.last_traded_close_ms, kNyMar9_1700);
    CHECK_EQ(c3.last_traded_close_ms - c3.open_ms, 82800000);

    auto ovn = must_cal("1800-1700", "America/New_York");
    auto ov = must_iv(ovn, d, kNyMar8_1800);
    CHECK_EQ(ov.open_ms, kNyMar8_1800);
    CHECK_EQ(ov.last_traded_close_ms, kNyMar9_1700);
}

static void test_overnight_spanning_gap() {
    std::printf("test_overnight_spanning_gap\n");
    auto cal = must_cal("2200-0200", "America/New_York");
    auto d = must_tf("D");
    auto iv = must_iv(cal, d, kNyMar8_2200);
    CHECK_EQ(iv.open_ms, kNyMar8_2200);
    CHECK_EQ(iv.last_traded_close_ms, kNyMar9_0300);
    CHECK(in_session(cal, kNyMar8_2200));
    CHECK(!in_session(cal, kNyMar9_0300));
}

static void test_fold_and_gap_resolve() {
    std::printf("test_fold_and_gap_resolve\n");
    auto gap = resolve_civil("America/New_York", 2025, 3, 9, 2, 0, 0);
    CHECK(gap.has_value());
    if (gap) {
        CHECK(gap->kind == CivilKind::Gap);
        CHECK_EQ(gap->epoch_ms, kNyMar9_0300);
    }
    auto fold = resolve_civil("America/New_York", 2025, 11, 2, 1, 0, 0);
    CHECK(fold.has_value());
    if (fold) {
        CHECK(fold->kind == CivilKind::Fold);
        CHECK_EQ(fold->epoch_ms, kNyNov2_0100Early);
        CHECK(fold->epoch_ms != kNyNov2_0100Late);
    }
    auto early = resolve_civil("America/New_York", 2025, 11, 2, 1, 30, 0);
    CHECK(early.has_value());
    if (early) CHECK_EQ(early->epoch_ms, kNyNov2_0130Early);

    auto a = must_cal("0000-0100", "America/New_York");
    auto b = must_cal("0100-0200", "America/New_York");
    auto d = must_tf("D");
    auto ia = must_iv(a, d, kNyNov2Midnight);
    auto ib = must_iv(b, d, kNyNov2_0100Early);
    CHECK_EQ(ia.last_traded_close_ms, kNyNov2_0100Early);
    CHECK_EQ(ib.open_ms, kNyNov2_0100Early);
    CHECK_EQ(ia.last_traded_close_ms, ib.open_ms);
    CHECK_EQ(ib.last_traded_close_ms, kNyNov2_0200);
}

static void test_call_order_independence() {
    std::printf("test_call_order_independence\n");
    auto cal = must_cal("24x7", "America/New_York");
    auto d = must_tf("D");
    // Witness instants: helper calendar_day_open_local_ms returns different
    // midnights for 00:30 vs 12:00 on the same transition day. Native must not.
    auto a = must_iv(cal, d, kNyMar9_0030);
    auto b = must_iv(cal, d, kNyMar9_1200);
    CHECK_EQ(a.open_ms, kNyMar9Midnight);
    CHECK_EQ(b.open_ms, kNyMar9Midnight);
    CHECK_EQ(a.open_ms, b.open_ms);
    auto c = must_iv(cal, d, kNyNov2_1200);
    auto e = must_iv(cal, d, kNyNov2_0030);
    CHECK_EQ(c.open_ms, kNyNov2Midnight);
    CHECK_EQ(e.open_ms, kNyNov2Midnight);

    auto r1 = resolve_civil("America/New_York", 2025, 3, 9, 0, 0, 0);
    auto r2 = resolve_civil("America/New_York", 2025, 11, 2, 0, 0, 0);
    auto r3 = resolve_civil("America/New_York", 2025, 3, 9, 0, 0, 0);
    CHECK(r1 && r2 && r3);
    if (r1 && r3) CHECK_EQ(r1->epoch_ms, r3->epoch_ms);
    if (r1) CHECK_EQ(r1->epoch_ms, kNyMar9Midnight);
    if (r2) CHECK_EQ(r2->epoch_ms, kNyNov2Midnight);
}

static void test_consecutive_continuity() {
    std::printf("test_consecutive_continuity\n");
    auto cal = must_cal("24x7", "America/New_York");
    auto d = must_tf("D");
    auto sat = must_iv(cal, d, kNyMar8Midnight);
    auto sun = must_iv(cal, d, kNyMar9Midnight);
    auto mon = must_iv(cal, d, kNyMar10Midnight);
    CHECK_EQ(sat.last_traded_close_ms, sun.open_ms);
    CHECK_EQ(sat.next_period_open_ms, sun.open_ms);
    CHECK_EQ(sun.last_traded_close_ms, mon.open_ms);
    CHECK_EQ(sun.next_period_open_ms, mon.open_ms);
    CHECK(sat.open_ms < sun.open_ms);
    CHECK(sun.open_ms < mon.open_ms);

    auto nsat = must_iv(cal, d, kNyNov1Midnight);
    auto nsun = must_iv(cal, d, kNyNov2Midnight);
    auto nmon = must_iv(cal, d, kNyNov3Midnight);
    CHECK_EQ(nsat.last_traded_close_ms, nsun.open_ms);
    CHECK_EQ(nsun.last_traded_close_ms, nmon.open_ms);
}

static void test_fixed_1m_across_dst() {
    std::printf("test_fixed_1m_across_dst\n");
    auto cal = must_cal("24x7", "America/New_York");
    auto m1 = must_tf("1");
    auto iv = must_iv(cal, m1, kNyMar9_0159);
    CHECK_EQ(iv.open_ms, kNyMar9_0159);
    CHECK_EQ(iv.last_traded_close_ms, kNyMar9_0300);
    CHECK_EQ(iv.last_traded_close_ms - iv.open_ms, 60000);
}

static void test_stable_anchors() {
    std::printf("test_stable_anchors\n");
    auto cal = must_cal("24x7", "America/New_York");
    auto two_d = must_tf("2D");
    auto two_w = must_tf("2W");
    auto three_m = must_tf("3M");
    // 2D: Jan 2 and Jan 3 share a pair; Jan 1 is the previous pair.
    // Querying only the later day does not shift the open.
    CHECK(period_key(cal, two_d, kNyJan2) == period_key(cal, two_d, kNyJan3));
    CHECK(period_key(cal, two_d, kNyJan1) != period_key(cal, two_d, kNyJan2));
    auto d2 = must_iv(cal, two_d, kNyJan2);
    auto d3 = must_iv(cal, two_d, kNyJan3);
    CHECK_EQ(d2.open_ms, d3.open_ms);
    CHECK_EQ(d2.open_ms, kNyJan2);
    // 2W: Jan 13 and Jan 20 share a pair; Jan 6 does not.
    CHECK(period_key(cal, two_w, kNyJan13) == period_key(cal, two_w, kNyJan20));
    CHECK(period_key(cal, two_w, kNyJan6) != period_key(cal, two_w, kNyJan13));
    auto w13 = must_iv(cal, two_w, kNyJan13);
    auto w20 = must_iv(cal, two_w, kNyJan20);
    CHECK_EQ(w13.open_ms, w20.open_ms);
    CHECK_EQ(w13.open_ms, kNyJan13);
    // 3M: Jan/Feb/Mar share January; April starts the next quarter.
    CHECK(period_key(cal, three_m, kNyJan1) == period_key(cal, three_m, kNyFeb15));
    CHECK(period_key(cal, three_m, kNyFeb15) == period_key(cal, three_m, kNyMar15));
    CHECK(period_key(cal, three_m, kNyApr15) != period_key(cal, three_m, kNyFeb15));
    auto q1 = must_iv(cal, three_m, kNyFeb15);
    auto q1b = must_iv(cal, three_m, kNyMar15);
    auto q2 = must_iv(cal, three_m, kNyApr15);
    CHECK_EQ(q1.open_ms, q1b.open_ms);
    CHECK_EQ(q1.open_ms, kNyJan1);
    CHECK_EQ(q2.open_ms, kNyApr1);

    auto rth = must_cal("0930-1600:23456", "America/New_York");
    auto wr13 = must_iv(rth, two_w, kNyJan13_0930);
    auto wr20 = must_iv(rth, two_w, kNyJan20_0930);
    CHECK_EQ(wr13.open_ms, wr20.open_ms);
    CHECK_EQ(wr13.open_ms, kNyJan13_0930);

    // Dates before the named origins keep floor-toward-inf grouping.
    CHECK(period_key(cal, two_d, kNy1969Dec30) == period_key(cal, two_d, kNy1969Dec31));
    auto week_ord = session_week_ordinal(cal, kNy1969Dec31);
    auto day_ord = session_day_ordinal(cal, kNy1969Dec31);
    CHECK(week_ord.has_value() && *week_ord <= 0);
    CHECK(day_ord.has_value() && *day_ord < 0);
}

static void test_fixed_clip_and_lunch() {
    std::printf("test_fixed_clip_and_lunch\n");
    auto cal = must_cal("0930-1130,1300-1600", "America/New_York");
    auto h1 = must_tf("60");
    auto morning = must_iv(cal, h1, kNyJun10_1100);
    CHECK_EQ(morning.open_ms, kNyJun10_0930 + 3600000);  // 10:30
    CHECK_EQ(morning.last_traded_close_ms, kNyJun10_1130);
    CHECK_EQ(morning.next_input_open_ms, kNyJun10_1300);
    CHECK(in_session(cal, morning.next_input_open_ms));
    CHECK(morning.next_input_open_ms != kNyJun10_0930 + 3 * 3600000);  // not 12:30
    CHECK(!in_session(cal, kNyJun10_1200));
    auto after = must_iv(cal, h1, kNyJun10_1330);
    CHECK(after.open_ms <= kNyJun10_1330);
    CHECK(after.last_traded_close_ms > kNyJun10_1330);
    auto last = must_iv(cal, h1, kNyJun10_1600 - 60000);
    CHECK_EQ(last.last_traded_close_ms, kNyJun10_1600);
}

static void test_codex_audit_regressions() {
    std::printf("test_codex_audit_regressions\n");
    auto d = must_tf("D");

    // P1-1 split overnight: second window is on the origin cycle, not the
    // origin's civil date. Origin stays 18:00, not the earliest clock 00:00.
    auto split = must_cal("1800-2300,0000-1700", "UTC");
    CHECK_EQ(split.origin_minutes(), 18 * 60);
    CHECK(in_session(split, kUtcJun10_1000));
    auto split_iv = must_iv(split, d, kUtcJun10_1000);
    CHECK_EQ(split_iv.open_ms, kUtcJun9_1800);
    CHECK_EQ(split_iv.last_traded_close_ms, kUtcJun10_1700);
    auto split_mask = must_cal("1800-2300,0000-1700:23456", "UTC");
    CHECK(in_session(split_mask, kUtcJun10_1000));
    CHECK(!in_session(split_mask, kUtcJun7_1800));

    // P1-1 reversed declaration: origin is 13:00, morning window is the next
    // cycle coverage, not dropped.
    auto rev = must_cal("1300-1600,0930-1130", "UTC");
    CHECK_EQ(rev.origin_minutes(), 13 * 60);
    CHECK(rev.origin_minutes() != 9 * 60 + 30);
    CHECK(in_session(rev, kUtcJun10_1000));
    auto rev_iv = must_iv(rev, d, kUtcJun10_1000);
    CHECK_EQ(rev_iv.open_ms, kUtcJun9_1300);
    CHECK_EQ(rev_iv.last_traded_close_ms, kUtcJun10_1130);

    // P1-2: 1S at 09:59:59 before a 3h break still yields an interval; next
    // input jumps to 13:00 rather than walking 8000 buckets.
    auto sec = must_tf("1S");
    auto brk = must_cal("0930-1000,1300-1600", "UTC");
    auto before = interval_containing(brk, sec, kUtcJun10_095959);
    CHECK(before.has_value());
    if (before) {
        CHECK_EQ(before->open_ms, kUtcJun10_095959);
        CHECK_EQ(before->last_traded_close_ms, kUtcJun10_1000);
        CHECK_EQ(before->next_input_open_ms, kUtcJun10_1300);
        CHECK(in_session(brk, before->next_input_open_ms));
    }

    // P1-3: next_input skips the closed lunch; 13:00 reopen is off the 60m
    // origin grid (12:30). Nominal open of the 13:00 query may be 12:30.
    auto lunch = must_cal("0930-1130,1300-1600", "UTC");
    auto h1 = must_tf("60");
    auto at11 = must_iv(lunch, h1, kUtcJun10_1100);
    CHECK_EQ(at11.next_input_open_ms, kUtcJun10_1300);
    CHECK(in_session(lunch, at11.next_input_open_ms));
    CHECK(at11.next_input_open_ms != kUtcJun10_1230);
    CHECK(!in_session(lunch, kUtcJun10_1230));
    auto at13 = must_iv(lunch, h1, kUtcJun10_1300);
    CHECK_EQ(at13.open_ms, kUtcJun10_1230);
    CHECK_EQ(at13.eligible_open_ms, kUtcJun10_1300);
    CHECK(in_session(lunch, at13.eligible_open_ms));
    CHECK(!in_session(lunch, at13.open_ms));

    // Mask applies once to the cycle. Sunday 13:30 belongs to the cycle whose
    // last coverage is Monday 11:30 (trading date Monday, :23456 allows).
    auto rev_mask = must_cal("1300-1600,0930-1130:23456", "America/New_York");
    CHECK(in_session(rev_mask, kNyJun8_1330));
    CHECK(in_session(rev_mask, kNyJun9_1000));
    CHECK(!in_session(rev_mask, kNyJun7_1330));
    auto sun_iv = must_iv(rev_mask, d, kNyJun8_1330);
    CHECK_EQ(sun_iv.open_ms, kNyJun8_1300);
    CHECK_EQ(sun_iv.last_traded_close_ms, kNyJun9_1130);
    auto mon_iv = must_iv(rev_mask, d, kNyJun9_1000);
    CHECK_EQ(mon_iv.open_ms, kNyJun8_1300);
    CHECK_EQ(mon_iv.last_traded_close_ms, kNyJun9_1130);

    // Overlap across the origin: 0900-1500 contributes both [13:00,15:00) on
    // the origin date and [09:00,13:00) the next morning. Shift-one-day would
    // drop the afternoon overlap.
    auto ovl = must_cal("1300-1600,0900-1500", "UTC");
    CHECK_EQ(ovl.origin_minutes(), 13 * 60);
    CHECK(in_session(ovl, kUtcJun9_1300));
    CHECK(in_session(ovl, kUtcJun9_1430));
    CHECK(in_session(ovl, kUtcJun9_1530));
    CHECK(!in_session(ovl, kUtcJun9_1600));
    CHECK(in_session(ovl, kUtcJun10_0900));
    CHECK(in_session(ovl, kUtcJun10_1000));
    auto ovl_iv = must_iv(ovl, d, kUtcJun9_1430);
    CHECK_EQ(ovl_iv.open_ms, kUtcJun9_1300);
    CHECK_EQ(ovl_iv.last_traded_close_ms, kUtcJun10_1300);
    auto ovl_next = must_iv(ovl, d, kUtcJun10_1300);
    CHECK_EQ(ovl_next.open_ms, kUtcJun10_1300);

    auto perm = must_cal("1300-1600,1100-1400,0900-1500", "UTC");
    CHECK_EQ(perm.origin_minutes(), 13 * 60);
    CHECK(in_session(perm, kUtcJun9_1430) == in_session(ovl, kUtcJun9_1430));
    CHECK(in_session(perm, kUtcJun10_1000) == in_session(ovl, kUtcJun10_1000));
    CHECK_EQ(must_iv(perm, d, kUtcJun9_1430).last_traded_close_ms,
             must_iv(ovl, d, kUtcJun9_1430).last_traded_close_ms);

    auto swapped = must_cal("0900-1500,1300-1600", "UTC");
    CHECK_EQ(swapped.origin_minutes(), 9 * 60);
    auto swapped_iv = must_iv(swapped, d, kUtcJun9_1430);
    CHECK_EQ(swapped_iv.open_ms, kUtcJun9_0900);
    CHECK(swapped_iv.open_ms != kUtcJun9_1300);

    // DST gap collapsing both ends of Sunday 0200-0245 omits that instance
    // only; 0930-1600 remains, and Monday 02:00-02:45 still covers the
    // Sunday cycle's pre-origin morning.
    auto gapw = must_cal("0930-1600,0200-0245", "America/New_York");
    CHECK(in_session(gapw, kNyMar9_1200));
    CHECK(!in_session(gapw, kNyMar9_0300));
    auto gap_iv = must_iv(gapw, d, kNyMar9_1200);
    CHECK_EQ(gap_iv.open_ms, kNyMar9_0930);
    CHECK_EQ(gap_iv.last_traded_close_ms, kNyMar10_0245);
    CHECK(gap_iv.last_traded_close_ms != kNyMar9_1600);
}

static void test_checked_public_values() {
    std::printf("test_checked_public_values\n");
    Timeframe def_tf;
    CHECK(!def_tf.valid());
    CHECK_EQ(def_tf.count(), 0);
    auto d = must_tf("D");
    CHECK(compatibility(def_tf, d).pairing == TimeframePairing::Invalid);
    CHECK(compatibility(d, def_tf).pairing == TimeframePairing::Invalid);
    CHECK(stream_compatibility(def_tf, d).pairing == TimeframePairing::Invalid);
    auto utc = must_cal("24x7", "UTC");
    CHECK(!period_key(utc, def_tf, 0).has_value());
    CHECK(!session_day_ordinal(SessionCalendar{}, 0).has_value());

    auto moved_from = must_tf("D");
    auto moved_to = std::move(moved_from);
    CHECK(moved_to.valid());
    CHECK(moved_to.literal() == "D");
    CHECK(!moved_from.valid());
    CHECK_EQ(moved_from.count(), 0);
    CHECK(compatibility(moved_from, must_tf("2D")).pairing == TimeframePairing::Invalid);
    CHECK(!period_key(utc, moved_from, 0).has_value());
    CHECK(compatibility(moved_to, must_tf("D")).pairing == TimeframePairing::Passthrough);

    SessionCalendar def_cal;
    CHECK(!def_cal.valid());
    CHECK_EQ(def_cal.day_mask(), 0);
    CHECK(!in_session(def_cal, 1749605400000LL));
    CHECK(!interval_containing(def_cal, must_tf("D"), 1749605400000LL).has_value());
    CHECK(!interval_containing(def_cal, must_tf("1000000D"), 1749549600000LL).has_value());

    auto moved_cal_from = must_cal("0930-1600", "UTC");
    auto moved_cal_to = std::move(moved_cal_from);
    CHECK(moved_cal_to.valid());
    CHECK(!moved_cal_from.valid());
    CHECK(!in_session(moved_cal_from, 1749605400000LL));
    CHECK(!interval_containing(moved_cal_from, must_tf("1000000D"), 1749549600000LL).has_value());
    CHECK(in_session(moved_cal_to, 1749553200000LL));

    SessionWindow def_win;
    CHECK(!def_win.valid());

    auto large = must_tf("1000000D");
    CHECK(interval_containing(utc, large, 1749549600000LL).has_value());
    CHECK(period_key(utc, large, 1749549600000LL).has_value());
}

static void test_empty_cycle_identity() {
    std::printf("test_empty_cycle_identity\n");
    auto rth = must_cal("0930-1600:23456", "UTC");
    auto d = must_tf("D");
    auto w = must_tf("W");
    auto mo = must_tf("M");

    CHECK(!in_session(rth, kUtcJun8_1200));
    auto sun_d = must_iv(rth, d, kUtcJun8_1200);
    CHECK_EQ(sun_d.open_ms, kUtcJun8_0930);
    CHECK_EQ(sun_d.last_traded_close_ms, kUtcJun8_0930);
    CHECK_EQ(sun_d.next_period_open_ms, kUtcJun9_0930);
    CHECK(sun_d.open_ms <= kUtcJun8_1200 && kUtcJun8_1200 < sun_d.next_period_open_ms);
    CHECK_EQ(sun_d.next_input_open_ms, kUtcJun9_0930);
    CHECK(period_key(rth, d, kUtcJun8_1200).has_value());
    CHECK_EQ(*period_key(rth, d, kUtcJun8_1200), 20247);
    CHECK_EQ(*period_key(rth, w, kUtcJun8_1200), 2892);
    CHECK_EQ(*period_key(rth, mo, kUtcJun8_1200), 24305);
    auto sun_w = must_iv(rth, w, kUtcJun8_1200);
    CHECK(sun_w.open_ms <= kUtcJun8_1200 && kUtcJun8_1200 < sun_w.next_period_open_ms);
    CHECK(sun_w.open_ms < kUtcJun9_0930);

    CHECK(!in_session(rth, kUtcJun7_1200));
    auto sat_d = must_iv(rth, d, kUtcJun7_1200);
    CHECK_EQ(sat_d.open_ms, kUtcJun7_0930);
    CHECK_EQ(sat_d.last_traded_close_ms, kUtcJun7_0930);
    CHECK_EQ(sat_d.next_period_open_ms, kUtcJun8_0930);
    CHECK(sat_d.open_ms <= kUtcJun7_1200 && kUtcJun7_1200 < sat_d.next_period_open_ms);
    CHECK(sat_d.next_period_open_ms - sat_d.open_ms < 2 * 86400000);
    CHECK_EQ(*period_key(rth, d, kUtcJun7_1200), 20246);
    CHECK_EQ(*period_key(rth, w, kUtcJun7_1200), 2892);

    CHECK(!in_session(rth, kUtcDec31_2023_1200));
    auto yend_d = must_iv(rth, d, kUtcDec31_2023_1200);
    CHECK_EQ(yend_d.open_ms, kUtcDec31_2023_0930);
    CHECK_EQ(yend_d.next_period_open_ms, kUtcJan1_2024_0930);
    CHECK(yend_d.open_ms <= kUtcDec31_2023_1200 && kUtcDec31_2023_1200 < yend_d.next_period_open_ms);
    CHECK_EQ(*period_key(rth, d, kUtcDec31_2023_1200), 19722);
    CHECK_EQ(*period_key(rth, w, kUtcDec31_2023_1200), 2817);
    CHECK_EQ(*period_key(rth, mo, kUtcDec31_2023_1200), 24287);
    auto yend_w = must_iv(rth, w, kUtcDec31_2023_1200);
    CHECK_EQ(yend_w.open_ms, kUtcDec25_2023_0930);
    CHECK(yend_w.open_ms <= kUtcDec31_2023_1200 && kUtcDec31_2023_1200 < yend_w.next_period_open_ms);
    auto yend_m = must_iv(rth, mo, kUtcDec31_2023_1200);
    CHECK(yend_m.open_ms <= kUtcDec31_2023_1200 && kUtcDec31_2023_1200 < yend_m.next_period_open_ms);
    CHECK(yend_m.next_period_open_ms == kUtcJan1_2024_0930);

    auto gap = must_cal("0230-0245", "America/New_York");
    CHECK(!in_session(gap, kNyMar9_1200));
    auto gap_d = must_iv(gap, d, kNyMar9_1200);
    CHECK_EQ(gap_d.open_ms, kNyMar9_0230_resolved);
    CHECK_EQ(gap_d.last_traded_close_ms, kNyMar9_0230_resolved);
    CHECK_EQ(gap_d.next_period_open_ms, kNyMar10_0230);
    CHECK(gap_d.open_ms <= kNyMar9_1200 && kNyMar9_1200 < gap_d.next_period_open_ms);
    CHECK(gap_d.open_ms != kNyMar10_0230);
    CHECK_EQ(*period_key(gap, d, kNyMar9_1200), 20156);
    CHECK_EQ(*period_key(gap, w, kNyMar9_1200), 2879);
    CHECK_EQ(*period_key(gap, mo, kNyMar9_1200), 24302);
}

static void test_timezone_acceptance() {
    std::printf("test_timezone_acceptance\n");
    CHECK(timezone_accepted(""));
    CHECK(timezone_accepted("UTC"));
    CHECK(timezone_accepted("GMT"));
    CHECK(timezone_accepted("Etc/UTC"));
    CHECK(timezone_accepted("Etc/GMT"));
    CHECK(timezone_accepted("America/New_York"));
    CHECK(timezone_accepted("Asia/Taipei"));
    CHECK(timezone_accepted("UTC+05:30"));
    CHECK(timezone_accepted("GMT-4"));
    CHECK(timezone_accepted("UTC+0"));
    CHECK(timezone_accepted("EST5EDT,M3.2.0,M11.1.0"));
    CHECK(timezone_accepted("CST6CDT,M3.2.0/2,M11.1.0/2"));
    CHECK(timezone_accepted("GMT0BST,M3.5.0/1,M10.5.0"));
    CHECK(timezone_accepted("<-05>5<-04>,M3.2.0,M11.1.0"));
    CHECK(timezone_accepted("US/Eastern"));
    CHECK(timezone_accepted("Japan"));
    CHECK(timezone_accepted("GB"));
    CHECK(timezone_accepted(":America/New_York"));

    CHECK(!timezone_accepted("No/Such_PineForge_Zone"));
    CHECK(!timezone_accepted("NoSuch_PineForge_Zone"));
    CHECK(!timezone_accepted("UTC+24:00"));
    CHECK(!timezone_accepted("GMT+24:00"));
    CHECK(!timezone_accepted("UTC+01:99"));
    CHECK(!timezone_accepted("UTC+5:3"));
    CHECK(!timezone_accepted("UTC+5:30:00"));
    CHECK(!timezone_accepted(" "));
    CHECK(!timezone_accepted("\tUTC"));
    CHECK(!timezone_accepted(std::string_view("UTC\0x", 5)));
    CHECK(!timezone_accepted("UTC+99999999999999999999"));
    CHECK(!timezone_accepted("UTC+"));
    CHECK(!timezone_accepted("UTC++5"));
    CHECK(!timezone_accepted("/usr/share/zoneinfo/UTC"));
    CHECK(!timezone_accepted("../UTC"));
    CHECK(!timezone_accepted("America/../Etc/UTC"));
    CHECK(!timezone_accepted("zone.tab"));
    CHECK(!timezone_accepted("iso3166.tab"));
    CHECK(!timezone_accepted("leapseconds"));
    CHECK(!timezone_accepted("+VERSION"));
    CHECK(!timezone_accepted(":"));

    CHECK(!parse_session("", "No/Such_PineForge_Zone"));
    CHECK(!parse_session("24x7", "UTC+24:00"));
    CHECK(!parse_session("0930-1600", "UTC+01:99"));
    CHECK(!parse_session("", " "));
    CHECK(parse_session("", "").has_value());
    CHECK(parse_session("24x7", "UTC+05:30").has_value());
    CHECK(parse_session("24x7", "EST5EDT,M3.2.0,M11.1.0").has_value());

    auto utc_mid = resolve_civil("UTC", 2025, 6, 10, 0, 0, 0);
    CHECK(utc_mid.has_value());
    CHECK_EQ(utc_mid->epoch_ms, kUtcJun10_0000);
    auto empty_mid = resolve_civil("", 2025, 6, 10, 0, 0, 0);
    CHECK(empty_mid.has_value());
    CHECK_EQ(empty_mid->epoch_ms, kUtcJun10_0000);
    auto off = resolve_civil("UTC+05:30", 2025, 6, 10, 0, 0, 0);
    CHECK(off.has_value());
    CHECK_EQ(off->epoch_ms, kUtcJun10_0000_plus0530);
    auto gmt = resolve_civil("GMT-4", 2025, 6, 10, 0, 0, 0);
    CHECK(gmt.has_value());
    CHECK_EQ(gmt->epoch_ms, kUtcJun10_0000_gmt_minus4);
    auto taipei = resolve_civil("Asia/Taipei", 2025, 6, 10, 0, 0, 0);
    CHECK(taipei.has_value());
    CHECK_EQ(taipei->epoch_ms, kUtcJun10_0000_taipei);
    auto posix = resolve_civil("EST5EDT,M3.2.0,M11.1.0", 2025, 3, 8, 0, 0, 0);
    CHECK(posix.has_value());
    CHECK_EQ(posix->epoch_ms, kNyMar8Midnight);
    auto posix_time = resolve_civil("CST6CDT,M3.2.0/2,M11.1.0/2", 2025, 3, 8, 0, 0, 0);
    CHECK(posix_time.has_value());
    CHECK_EQ(posix_time->epoch_ms, kCstMar8Midnight);
    auto eastern = resolve_civil("US/Eastern", 2025, 3, 8, 0, 0, 0);
    CHECK(eastern.has_value());
    CHECK_EQ(eastern->epoch_ms, kNyMar8Midnight);
    auto japan = resolve_civil("Japan", 2025, 6, 10, 0, 0, 0);
    CHECK(japan.has_value());
    CHECK_EQ(japan->epoch_ms, kUtcJun10_0000_japan);
    auto quoted = resolve_civil("<-05>5<-04>,M3.2.0,M11.1.0", 2025, 3, 8, 0, 0, 0);
    CHECK(quoted.has_value());
    CHECK_EQ(quoted->epoch_ms, kNyMar8Midnight);

    CHECK(!resolve_civil("No/Such_PineForge_Zone", 2025, 6, 10, 0, 0, 0));
    CHECK(!resolve_civil("UTC+24:00", 2025, 6, 10, 0, 0, 0));
    CHECK(!resolve_civil("UTC+01:99", 2025, 6, 10, 0, 0, 0));
    CHECK(!resolve_civil(" ", 2025, 6, 10, 0, 0, 0));
}

struct TzdirGuard {
    bool had = false;
    std::string old;
    TzdirGuard() {
        if (const char* v = std::getenv("TZDIR")) {
            had = true;
            old = v;
        }
    }
    void set(const char* v) { ::setenv("TZDIR", v, 1); }
    void clear() { ::unsetenv("TZDIR"); }
    ~TzdirGuard() {
        if (had) ::setenv("TZDIR", old.c_str(), 1);
        else ::unsetenv("TZDIR");
    }
};

static bool copy_file_bytes(const char* src, const char* dst) {
    const int in = ::open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    const int out = ::open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        return false;
    }
    char buf[4096];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            ok = false;
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(out, buf + off, static_cast<size_t>(n - off));
            if (w <= 0) {
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    ::close(in);
    ::close(out);
    return ok;
}

static const char* system_nonutc_tzif() {
    static const char* cands[] = {
        "/var/db/timezone/zoneinfo/Asia/Taipei",
        "/usr/share/zoneinfo/Asia/Taipei",
    };
    for (const char* p : cands) {
        const int fd = ::open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char mag[4] = {};
        const ssize_t n = ::read(fd, mag, 4);
        ::close(fd);
        if (n == 4 && std::memcmp(mag, "TZif", 4) == 0) return p;
    }
    return nullptr;
}

// Child process: libc mktime under the given TZ/TZDIR. Does not touch the
// parent's ScopedTimezone cache.
static int64_t libc_civil_ms(const char* tzdir, const char* tz, int y, int mo, int d) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        ::close(fds[0]);
        if (tzdir == nullptr) ::unsetenv("TZDIR");
        else ::setenv("TZDIR", tzdir, 1);
        ::setenv("TZ", tz, 1);
        ::tzset();
        std::tm tm {};
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_isdst = 0;
        const time_t t = ::mktime(&tm);
        const int64_t ms = (t == static_cast<time_t>(-1))
                               ? static_cast<int64_t>(-1)
                               : static_cast<int64_t>(t) * 1000;
        const ssize_t w = ::write(fds[1], &ms, sizeof(ms));
        (void)w;
        ::_exit(0);
    }
    ::close(fds[1]);
    int64_t ms = -1;
    ssize_t got = 0;
    char* p = reinterpret_cast<char*>(&ms);
    while (got < static_cast<ssize_t>(sizeof(ms))) {
        const ssize_t n = ::read(fds[0], p + got, sizeof(ms) - static_cast<size_t>(got));
        if (n <= 0) break;
        got += n;
    }
    ::close(fds[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    return got == static_cast<ssize_t>(sizeof(ms)) ? ms : static_cast<int64_t>(-1);
}

static std::string independent_zoneinfo_root() {
    char buf[PATH_MAX];
#if defined(__APPLE__)
    if (::realpath("/var/db/timezone/zoneinfo", buf) != nullptr) return buf;
    if (::realpath("/usr/share/zoneinfo", buf) != nullptr) return buf;
#elif defined(__GLIBC__)
    if (const char* env = std::getenv("TZDIR"); env != nullptr && env[0] != '\0') {
        if (::realpath(env, buf) == nullptr) return {};
        struct stat st {};
        if (::stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return {};
        return buf;
    }
    if (::realpath("/usr/share/zoneinfo", buf) != nullptr) return buf;
#else
    if (::realpath("/usr/share/zoneinfo", buf) != nullptr) return buf;
#endif
    return {};
}

static std::string independent_tzif_path(const char* name) {
    const std::string root = independent_zoneinfo_root();
    if (root.empty()) return {};
    const std::string full = root + "/" + name;
    char buf[PATH_MAX];
    if (::realpath(full.c_str(), buf) == nullptr) return {};
    const std::string res(buf);
    if (res != root && res.rfind(root + "/", 0) != 0) return {};
    const int fd = ::open(res.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char mag[4] = {};
    const ssize_t n = ::read(fd, mag, 4);
    ::close(fd);
    if (n != 4 || std::memcmp(mag, "TZif", 4) != 0) return {};
    return res;
}

static void check_identity(std::string_view zone,
                           TimezoneSourceKind kind,
                           std::string_view definition,
                           const char* path_or_null) {
    auto d = timezone_identity_descriptor(zone);
    CHECK(d.has_value());
    if (!d) return;
    CHECK(d->valid());
    CHECK_EQ(static_cast<long long>(d->semantics_version),
             static_cast<long long>(TimezoneIdentityDescriptor::kSemanticsVersion));
    CHECK_EQ(static_cast<long long>(d->kind), static_cast<long long>(kind));
    CHECK(d->input == zone);
    CHECK(d->effective_definition == definition);
    if (path_or_null == nullptr) {
        CHECK(d->resource_paths.empty());
    } else {
        CHECK(d->resource_paths.size() == 1);
        if (!d->resource_paths.empty()) CHECK(d->resource_paths.front() == path_or_null);
    }
}

static void test_timezone_identity() {
    std::printf("test_timezone_identity\n");
    CHECK(!TimezoneIdentityDescriptor{}.valid());
    CHECK(!timezone_identity_descriptor("No/Such_PineForge_Zone"));
    CHECK(!timezone_identity_descriptor("UTC+24:00"));
    CHECK(!timezone_identity_descriptor(" "));

    const std::string root = independent_zoneinfo_root();
    CHECK(!root.empty());
    const std::string utc_path = independent_tzif_path("UTC");
    const std::string ny_path = independent_tzif_path("America/New_York");
    const std::string eastern_path = independent_tzif_path("US/Eastern");
    const std::string japan_path = independent_tzif_path("Japan");
    const std::string posixrules_path = independent_tzif_path("posixrules");
    const std::string gmt_path = independent_tzif_path("GMT");
    CHECK(!ny_path.empty());
    CHECK(!eastern_path.empty());
    CHECK(!japan_path.empty());
    CHECK(!posixrules_path.empty());
    CHECK(independent_tzif_path("FOO5BAR").empty());

    check_identity("", TimezoneSourceKind::Utc, "UTC",
                   utc_path.empty() ? nullptr : utc_path.c_str());
    check_identity("UTC", TimezoneSourceKind::Utc, "UTC",
                   utc_path.empty() ? nullptr : utc_path.c_str());
    check_identity("GMT", TimezoneSourceKind::Utc, "UTC",
                   utc_path.empty() ? nullptr : utc_path.c_str());
    check_identity("Etc/UTC", TimezoneSourceKind::Utc, "UTC",
                   utc_path.empty() ? nullptr : utc_path.c_str());
    check_identity("UTC+0", TimezoneSourceKind::Utc, "UTC",
                   utc_path.empty() ? nullptr : utc_path.c_str());
    if (!utc_path.empty() && !gmt_path.empty()) {
        CHECK(utc_path != gmt_path);
        auto gmt = timezone_identity_descriptor("GMT");
        CHECK(gmt.has_value());
        CHECK(gmt->resource_paths.size() != 1 || gmt->resource_paths.front() != gmt_path);
    }

    auto ny = timezone_identity_descriptor("America/New_York");
    CHECK(ny.has_value());
    CHECK(ny->valid());
    CHECK(ny->kind == TimezoneSourceKind::Tzfile);
    CHECK(ny->effective_definition == "America/New_York");
    CHECK(ny->zoneinfo_root == root);
    CHECK(ny->resource_paths.size() == 1);
    CHECK(ny->resource_paths.front() == ny_path);

    auto eastern = timezone_identity_descriptor("US/Eastern");
    CHECK(eastern.has_value());
    CHECK(eastern->kind == TimezoneSourceKind::Tzfile);
    CHECK(eastern->effective_definition == "US/Eastern");
    CHECK(eastern->resource_paths.size() == 1);
    CHECK(eastern->resource_paths.front() == eastern_path);

    auto colon = timezone_identity_descriptor(":America/New_York");
    CHECK(colon.has_value());
    CHECK(colon->kind == TimezoneSourceKind::Tzfile);
    CHECK(colon->effective_definition == "America/New_York");
    CHECK(colon->resource_paths == ny->resource_paths);

    auto japan = timezone_identity_descriptor("Japan");
    CHECK(japan.has_value());
    CHECK(japan->kind == TimezoneSourceKind::Tzfile);
    CHECK(japan->resource_paths.front() == japan_path);

    check_identity("UTC+05:30", TimezoneSourceKind::FixedOffset, "UTC-5:30", nullptr);
    check_identity("GMT-4", TimezoneSourceKind::FixedOffset, "UTC+4", nullptr);
    check_identity("FOO5", TimezoneSourceKind::FixedOffset, "FOO5", nullptr);

    check_identity("EST5EDT,M3.2.0,M11.1.0", TimezoneSourceKind::PosixExplicit,
                   "EST5EDT,M3.2.0,M11.1.0", nullptr);
    check_identity("CST6CDT,M3.2.0/2,M11.1.0/2", TimezoneSourceKind::PosixExplicit,
                   "CST6CDT,M3.2.0/2,M11.1.0/2", nullptr);
    check_identity("<-05>5<-04>,M3.2.0,M11.1.0", TimezoneSourceKind::PosixExplicit,
                   "<-05>5<-04>,M3.2.0,M11.1.0", nullptr);

    const std::string est5edt_path = independent_tzif_path("EST5EDT");
    CHECK(!est5edt_path.empty());
    check_identity("EST5EDT", TimezoneSourceKind::Tzfile, "EST5EDT", est5edt_path.c_str());

    CHECK(timezone_accepted("FOO5BAR"));
    check_identity("FOO5BAR", TimezoneSourceKind::PosixDefaultDst, "FOO5BAR",
                   posixrules_path.c_str());
    CHECK(timezone_accepted("EST5EDT4"));
    check_identity("EST5EDT4", TimezoneSourceKind::PosixDefaultDst, "EST5EDT4",
                   posixrules_path.c_str());
}

static void test_timezone_tzdir_root() {
    std::printf("test_timezone_tzdir_root\n");
    TzdirGuard env;
    env.clear();

    // Digit-free so a missing tzfile is not a POSIX offset spec (PfTzdirP1
    // would be std+1 on this libc). Must not be a UTC TZif copy.
    const char kFake[] = "PfTzdirProbe/Zone";
    CHECK(timezone_accepted("America/New_York"));
    CHECK(!timezone_accepted(kFake));
    const auto ny = resolve_civil("America/New_York", 2025, 3, 8, 0, 0, 0);
    CHECK(ny.has_value());
    CHECK_EQ(ny->epoch_ms, kNyMar8Midnight);

    const char* src = system_nonutc_tzif();
    CHECK(src != nullptr);
    if (src == nullptr) return;

    char fake_root_tmpl[] = "/tmp/pf-tzdir-XXXXXX";
    char* fake_root = ::mkdtemp(fake_root_tmpl);
    CHECK(fake_root != nullptr);
    if (fake_root == nullptr) return;
    std::string zone_dir = std::string(fake_root) + "/PfTzdirProbe";
    CHECK(::mkdir(zone_dir.c_str(), 0700) == 0);
    std::string zone_path = zone_dir + "/Zone";
    CHECK(copy_file_bytes(src, zone_path.c_str()));

    char empty_tmpl[] = "/tmp/pf-tzdir-empty-XXXXXX";
    char* empty_root = ::mkdtemp(empty_tmpl);
    CHECK(empty_root != nullptr);
    const std::string missing =
        std::string("/tmp/pf-tzdir-missing-") + std::to_string(::getpid());

#if defined(__GLIBC__)
    const bool libc_reads_tzdir = true;
#else
    const bool libc_reads_tzdir = false;
#endif

    env.set(fake_root);
    if (libc_reads_tzdir) {
        CHECK(timezone_accepted(kFake));
        CHECK(parse_session("24x7", kFake).has_value());
        const auto fake_civil = resolve_civil(kFake, 2025, 6, 10, 0, 0, 0);
        CHECK(fake_civil.has_value());
        CHECK_EQ(fake_civil->epoch_ms, kUtcJun10_0000_taipei);
        CHECK(!timezone_accepted("America/New_York"));
        CHECK(!parse_session("24x7", "America/New_York"));
        CHECK(!resolve_civil("America/New_York", 2025, 3, 8, 0, 0, 0));
        CHECK(!timezone_identity_descriptor("America/New_York"));
        auto fake_id = timezone_identity_descriptor(kFake);
        CHECK(fake_id.has_value());
        CHECK(fake_id->kind == TimezoneSourceKind::Tzfile);
        char fake_real[PATH_MAX];
        CHECK(::realpath(zone_path.c_str(), fake_real) != nullptr);
        CHECK(fake_id->resource_paths.size() == 1);
        CHECK(fake_id->resource_paths.front() == fake_real);
        CHECK(timezone_accepted("FOO5BAR"));
        CHECK(!timezone_identity_descriptor("FOO5BAR"));
        CHECK_EQ(libc_civil_ms(fake_root, kFake, 2025, 6, 10), kUtcJun10_0000_taipei);
        CHECK_EQ(libc_civil_ms(fake_root, "America/New_York", 2025, 3, 8), kUtcMar8_0000);
    } else {
        CHECK(!timezone_accepted(kFake));
        CHECK(!parse_session("24x7", kFake));
        CHECK(!resolve_civil(kFake, 2025, 6, 10, 0, 0, 0));
        CHECK(!timezone_identity_descriptor(kFake));
        CHECK(timezone_accepted("America/New_York"));
        CHECK(parse_session("24x7", "America/New_York").has_value());
        const auto ny_fake_dir = resolve_civil("America/New_York", 2025, 3, 8, 0, 0, 0);
        CHECK(ny_fake_dir.has_value());
        CHECK_EQ(ny_fake_dir->epoch_ms, kNyMar8Midnight);
        auto ny_id = timezone_identity_descriptor("America/New_York");
        CHECK(ny_id.has_value());
        CHECK(ny_id->kind == TimezoneSourceKind::Tzfile);
        CHECK(ny_id->resource_paths.front() == independent_tzif_path("America/New_York"));
        CHECK(timezone_accepted("FOO5BAR"));
        auto foo_id = timezone_identity_descriptor("FOO5BAR");
        CHECK(foo_id.has_value());
        CHECK(foo_id->kind == TimezoneSourceKind::PosixDefaultDst);
        CHECK(foo_id->resource_paths.front() == independent_tzif_path("posixrules"));
        CHECK_EQ(libc_civil_ms(fake_root, kFake, 2025, 6, 10), kUtcJun10_0000);
        CHECK_EQ(libc_civil_ms(fake_root, "America/New_York", 2025, 3, 8), kNyMar8Midnight);
    }
    // A UTC TZif would not discriminate; the copied file is Asia/Taipei.
    CHECK(kUtcJun10_0000_taipei != kUtcJun10_0000);

    env.set(empty_root);
    if (libc_reads_tzdir) {
        CHECK(!timezone_accepted("America/New_York"));
        CHECK(!timezone_identity_descriptor("America/New_York"));
        CHECK_EQ(libc_civil_ms(empty_root, "America/New_York", 2025, 3, 8), kUtcMar8_0000);
    } else {
        CHECK(timezone_accepted("America/New_York"));
        CHECK(timezone_identity_descriptor("America/New_York").has_value());
        CHECK_EQ(libc_civil_ms(empty_root, "America/New_York", 2025, 3, 8), kNyMar8Midnight);
    }

    env.set(missing.c_str());
    if (libc_reads_tzdir) {
        CHECK(!timezone_accepted("America/New_York"));
        CHECK(!timezone_identity_descriptor("America/New_York"));
        CHECK_EQ(libc_civil_ms(missing.c_str(), "America/New_York", 2025, 3, 8),
                 kUtcMar8_0000);
    } else {
        CHECK(timezone_accepted("America/New_York"));
        CHECK(timezone_identity_descriptor("America/New_York").has_value());
        CHECK_EQ(libc_civil_ms(missing.c_str(), "America/New_York", 2025, 3, 8),
                 kNyMar8Midnight);
    }

    env.set("");
    CHECK(timezone_accepted("America/New_York"));

#if defined(__GLIBC__)
    {
        char saved_cwd[PATH_MAX];
        const bool have_cwd = ::getcwd(saved_cwd, sizeof(saved_cwd)) != nullptr;
        CHECK(have_cwd);
        if (have_cwd && ::chdir(fake_root) == 0) {
            env.set(".");
            CHECK(timezone_accepted(kFake));
            const auto rel = resolve_civil(kFake, 2025, 6, 10, 0, 0, 0);
            CHECK(rel.has_value());
            CHECK_EQ(rel->epoch_ms, kUtcJun10_0000_taipei);
            CHECK(::chdir(saved_cwd) == 0);
        }
    }
#endif

    env.clear();
    CHECK(timezone_accepted("America/New_York"));
    CHECK(!timezone_accepted(kFake));
    const auto ny_restored = resolve_civil("America/New_York", 2025, 3, 8, 0, 0, 0);
    CHECK(ny_restored.has_value());
    CHECK_EQ(ny_restored->epoch_ms, kNyMar8Midnight);

    ::unlink(zone_path.c_str());
    ::rmdir(zone_dir.c_str());
    ::rmdir(fake_root);
    ::rmdir(empty_root);
}

int main() {
    test_timeframe_forms();
    test_pairings();
    test_session_parse();
    test_multi_window_last_close();
    test_overnight_origin_1800();
    test_full_day_and_2359();
    test_weekday_mask_and_gaps();
    test_rth_friday_vs_monday();
    test_dst_oracles();
    test_overnight_spanning_gap();
    test_fold_and_gap_resolve();
    test_call_order_independence();
    test_consecutive_continuity();
    test_fixed_1m_across_dst();
    test_stable_anchors();
    test_fixed_clip_and_lunch();
    test_codex_audit_regressions();
    test_checked_public_values();
    test_empty_cycle_identity();
    test_timezone_acceptance();
    test_timezone_identity();
    test_timezone_tzdir_root();
    std::printf("test_native_calendar: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
