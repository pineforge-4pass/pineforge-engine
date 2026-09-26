// R5 lane MAG-INTRABAR: the intrabars TradingView's bar magnifier walks.
//
// TradingView's help centre maps a chart timeframe to the intrabar
// timeframe its magnifier reads ("What is bar magnifier backtesting mode",
// each row covering the chart timeframes up to the next): 1 -> 10S,
// 5 -> 30S, 10 -> 1, 15 -> 2, 30 -> 5, 60 -> 10, 240 -> 30, 1D -> 60,
// 3D -> 240, 1W -> 1D. The lane measured every row the population uses
// (tests/fixtures/magnifier_intrabars/README.md): request.security_lower_tf
// prints those intrabars -- the symbol's regular bars, anchored at the
// session's open, cut at its close, owned by the chart bar that holds their
// last minute -- and a magnified bracket tape per row matches a broker that
// walks them, with the chart bar's own open and close at either end.
//
// source::tradingview_magnifier_bars builds that path from the host's
// 1-minute feed for IntrabarPath::lower_tf:
//   1. the table, including the rows between the listed ones;
//   2. a 24x7 15m chart: 2-minute intrabars, the :14-:16 straddler owned by
//      the :15 bar behind a one-price bar at that bar's own open, and a
//      one-price bar at the :00 bar's own close for the minute it lost;
//   3. a session anchored at 09:15 (NSE): the 2-minute grid starts at the
//      session's open, and its last intrabar is cut at the close;
//   4. a 1D chart of a 09:30-16:00 session: seven hourly intrabars, the last
//      a half hour, no one-price bars;
//   5. the feed comes back unchanged where the table's intrabar is the
//      input's own timeframe (10m) or finer than it (1m, 5m), or cannot be
//      built from it (a 3-minute feed for 2-minute intrabars);
//   6. every stamp strictly increases and lies inside the chart bar that
//      owns it, which is how the kernel assigns lower bars.
//
// Source-bound (includes pineforge/source): release profile only.
#include <pineforge/source/magnifier_intrabars.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using pineforge::Bar;
using pineforge::source::tradingview_intrabar_timeframe;
using pineforge::source::tradingview_magnifier_bars;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(expr) do {                                                       \
    ++checks;                                                                  \
    if (!(expr)) { ++failures; std::printf("FAIL %d %s\n", __LINE__, #expr); } \
} while (0)

const std::int64_t kMin = 60000;

// 1-minute bars from `from` for `count` minutes: open = 100 + minute index,
// high = open + 0.5, low = open - 0.25, close = open + 0.1, volume 1.
std::vector<Bar> minutes(std::int64_t from, int count, int skip_every = 0) {
    std::vector<Bar> out;
    for (int i = 0; i < count; ++i) {
        if (skip_every > 0 && i % skip_every == skip_every - 1) continue;
        const double o = 100.0 + i;
        out.push_back({o, o + 0.5, o - 0.25, o + 0.1, 1.0, from + i * kMin});
    }
    return out;
}

bool same(const std::vector<Bar>& a, const std::vector<Bar>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].timestamp != b[i].timestamp || a[i].open != b[i].open || a[i].high != b[i].high
            || a[i].low != b[i].low || a[i].close != b[i].close || a[i].volume != b[i].volume)
            return false;
    }
    return true;
}

bool one_price(const Bar& b, double price) {
    return b.open == price && b.high == price && b.low == price && b.close == price
        && b.volume == 0.0;
}

void print(const char* label, const std::vector<Bar>& path, std::int64_t origin) {
    std::printf("%s:", label);
    for (const Bar& b : path) {
        std::printf(" [%+lldms %.2f %.2f %.2f %.2f]",
                    static_cast<long long>(b.timestamp - origin), b.open, b.high, b.low, b.close);
    }
    std::printf("\n");
}

// 1. The table.
void table() {
    const char* rows[][2] = {
        {"1S", "1S"}, {"15S", "1S"}, {"30S", "5S"}, {"45S", "5S"},
        {"1", "10S"}, {"3", "10S"}, {"5", "30S"}, {"7", "30S"},
        {"10", "1"}, {"12", "1"}, {"15", "2"}, {"20", "2"}, {"25", "2"},
        {"30", "5"}, {"45", "5"}, {"60", "10"}, {"120", "10"}, {"180", "10"},
        {"240", "30"}, {"720", "30"}, {"1440", "60"},
        {"D", "60"}, {"1D", "60"}, {"2D", "60"}, {"3D", "240"}, {"5D", "240"},
        {"W", "1D"}, {"1W", "1D"}, {"1M", "1D"}, {"3M", "1D"},
    };
    for (const auto& row : rows) {
        const std::string got = tradingview_intrabar_timeframe(row[0]);
        if (got != row[1]) std::printf("  table %s -> %s, want %s\n", row[0], got.c_str(), row[1]);
        CHECK(got == row[1]);
    }
    CHECK(tradingview_intrabar_timeframe("1H").empty());
    CHECK(tradingview_intrabar_timeframe("").empty());
}

// 2. 24x7, a 15-minute chart: 00:00-00:29 UTC of 1 Oct 2025.
void fifteen_minute_straddle() {
    const std::int64_t t0 = 1759276800000LL;   // 2025-10-01 00:00 UTC
    const auto feed = minutes(t0, 30);
    const auto path = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                                 "1", "15", "24x7", "UTC");
    print("15m", path, t0);
    // :00 bar: [0,2) .. [12,14) and its own close (minute :14's close).
    // :15 bar: its own open (minute :15's), the [14,16) straddler, [16,18) ..
    // [28,30).
    CHECK(path.size() == 7 + 1 + 1 + 8);
    if (path.size() != 17) return;
    for (int k = 0; k < 7; ++k) {
        const Bar& b = path[static_cast<std::size_t>(k)];
        CHECK(b.timestamp == t0 + 2 * k * kMin);
        CHECK(b.open == feed[static_cast<std::size_t>(2 * k)].open);
        CHECK(b.close == feed[static_cast<std::size_t>(2 * k + 1)].close);
        CHECK(b.high == feed[static_cast<std::size_t>(2 * k + 1)].high);   // rising feed
        CHECK(b.low == feed[static_cast<std::size_t>(2 * k)].low);
        CHECK(b.volume == 2.0);
    }
    CHECK(one_price(path[7], feed[14].close));
    CHECK(path[7].timestamp == t0 + 14 * kMin);
    CHECK(one_price(path[8], feed[15].open));
    CHECK(path[8].timestamp == t0 + 15 * kMin);
    const Bar& straddler = path[9];
    CHECK(straddler.timestamp == t0 + 15 * kMin + 1);
    CHECK(straddler.open == feed[14].open);
    CHECK(straddler.low == feed[14].low);
    CHECK(straddler.high == feed[15].high);
    CHECK(straddler.close == feed[15].close);
    for (int k = 0; k < 7; ++k) {
        const Bar& b = path[static_cast<std::size_t>(10 + k)];
        CHECK(b.timestamp == t0 + (16 + 2 * k) * kMin);
        CHECK(b.open == feed[static_cast<std::size_t>(16 + 2 * k)].open);
        CHECK(b.close == feed[static_cast<std::size_t>(17 + 2 * k)].close);
    }
}

// 3. NSE: 09:15-15:30 Asia/Kolkata, 15-minute chart. 8 Oct 2025 09:15 IST is
// 03:45 UTC -- an odd minute, so epoch 2-minute bars would straddle the
// session's open; TradingView's start at it.
void session_anchor_and_close() {
    const std::int64_t open = 1759895100000LL;   // 2025-10-08 03:45 UTC = 09:15 IST
    const auto feed = minutes(open, 375);         // the whole session, 09:15-15:30
    const auto path = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                                 "1", "15", "0915-1530", "Asia/Kolkata");
    CHECK(!path.empty());
    if (path.empty()) return;
    // The first chart bar opens the session: [09:15, 09:17) first, no open bar.
    CHECK(path[0].timestamp == open);
    CHECK(path[0].open == feed[0].open && path[0].close == feed[1].close);
    // The 09:30 bar: its own open, then the [09:29, 09:31) straddler.
    int at_930 = -1;
    for (std::size_t i = 0; i < path.size(); ++i)
        if (path[i].timestamp == open + 15 * kMin) { at_930 = static_cast<int>(i); break; }
    CHECK(at_930 > 0);
    if (at_930 > 0) {
        const auto i = static_cast<std::size_t>(at_930);
        CHECK(one_price(path[i], feed[15].open));
        CHECK(one_price(path[i - 1], feed[14].close));   // the 09:15 bar's own close
        CHECK(path[i + 1].timestamp == open + 15 * kMin + 1);
        CHECK(path[i + 1].open == feed[14].open && path[i + 1].close == feed[15].close);
    }
    // The session's last chart bar, 15:15: its last intrabar is [15:29,
    // 15:30), cut at the close -- one minute, owned by that bar, no close bar.
    const Bar& last = path.back();
    CHECK(last.timestamp == open + 374 * kMin);
    CHECK(last.open == feed[374].open && last.close == feed[374].close);
    CHECK(last.volume == 1.0);
}

// 4. A 1D chart of a 09:30-16:00 New York session (EDT): seven hourly
// intrabars from the session's open, the last one 15:30-16:00.
void daily_session_hours() {
    const std::int64_t open = 1759843800000LL;   // 2025-10-07 13:30 UTC = 09:30 EDT
    std::vector<Bar> feed = minutes(open, 390);
    const auto day2 = minutes(open + 86400000LL, 390);
    feed.insert(feed.end(), day2.begin(), day2.end());
    const auto path = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                                 "1", "1D", "0930-1600", "America/New_York");
    CHECK(path.size() == 14);
    if (path.size() != 14) return;
    for (int day = 0; day < 2; ++day) {
        for (int h = 0; h < 7; ++h) {
            const Bar& b = path[static_cast<std::size_t>(7 * day + h)];
            const std::size_t first = static_cast<std::size_t>(390 * day + 60 * h);
            const std::size_t last = std::min(first + 59, static_cast<std::size_t>(390 * day + 389));
            CHECK(b.timestamp == feed[first].timestamp);
            CHECK(b.open == feed[first].open && b.close == feed[last].close);
            CHECK(b.volume == static_cast<double>(last - first + 1));
        }
    }
}

// 5. Feeds that are left as they are.
void unchanged_feeds() {
    const std::int64_t t0 = 1759276800000LL;
    const auto feed = minutes(t0, 60);
    CHECK(same(tradingview_magnifier_bars(feed.data(), 60, "1", "10", "24x7", "UTC"), feed));
    CHECK(same(tradingview_magnifier_bars(feed.data(), 60, "1", "5", "24x7", "UTC"), feed));
    CHECK(same(tradingview_magnifier_bars(feed.data(), 60, "1", "1", "24x7", "UTC"), feed));
    CHECK(same(tradingview_magnifier_bars(feed.data(), 60, "1H", "15", "24x7", "UTC"), feed));
    std::vector<Bar> three;
    for (int i = 0; i < 20; ++i) three.push_back(feed[static_cast<std::size_t>(3 * i)]);
    for (auto& b : three) b.timestamp = t0 + (b.timestamp - t0);
    CHECK(same(tradingview_magnifier_bars(three.data(), 20, "3", "15", "24x7", "UTC"), three));
    CHECK(tradingview_magnifier_bars(nullptr, 0, "1", "15", "24x7", "UTC").empty());
}

// 6. Stamps: strictly increasing, each inside its owner, over a sparse feed.
void stamps_inside_owners() {
    const std::int64_t t0 = 1759276800000LL;
    const auto feed = minutes(t0, 24 * 60, 7);   // every 7th minute missing
    const auto path = tradingview_magnifier_bars(feed.data(), static_cast<int>(feed.size()),
                                                 "1", "15", "24x7", "UTC");
    CHECK(!path.empty());
    bool increasing = true;
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i].timestamp <= path[i - 1].timestamp) increasing = false;
    CHECK(increasing);
    // Every path bar's chart bar holds at least one feed minute at or before
    // its stamp, and none of its stamps reach the next chart bar.
    bool inside = true;
    for (const Bar& b : path) {
        const std::int64_t chart = b.timestamp - (b.timestamp - t0) % (15 * kMin);
        bool has = false;
        for (const Bar& m : feed)
            if (m.timestamp >= chart && m.timestamp <= b.timestamp) { has = true; break; }
        if (!has) inside = false;
    }
    CHECK(inside);
}

}  // namespace

int main() {
    table();
    fifteen_minute_straddle();
    session_anchor_and_close();
    daily_session_hours();
    unchanged_feeds();
    stamps_inside_owners();
    std::printf("test_magnifier_tradingview_intrabars: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
