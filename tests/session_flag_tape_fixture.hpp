#pragma once
// TradingView's session flags, one chart bar per tape entry
// (tests/fixtures/session_windows, README.md there). Each probe reverses its
// position at the close of every chart bar (process_orders_on_close), so
// every Entry row is one chart bar, dated at its open (tv_trades.csv renders
// times at UTC+8), and its Signal carries the flags TradingView evaluated on
// it: "M1P0Q0F1L0f1l0" (M session.ismarket, P session.ispremarket, Q
// session.ispostmarket, F session.isfirstbar, L session.islastbar, f
// session.isfirstbar_regular, l session.islastbar_regular), or the history
// probe's "C" and seven bits in that order. No engine header: the tape and
// its flat bars only.
#include <pineforge/bar.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace session_tape {

constexpr std::int64_t kMinute = 60'000;

inline std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" at a fixed UTC offset in hours -> UTC milliseconds.
inline std::int64_t utc_ms(int y, int mo, int d, int h, int mi, int offset_hours) {
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 24 + h - offset_hours) * 60 + mi) * kMinute;
}

struct Flags {
    bool market = false, premarket = false, postmarket = false;
    bool first = false, last = false, first_regular = false, last_regular = false;
};

struct TapeBar {
    std::int64_t ts = 0;  // the chart bar's open, UTC ms
    Flags tv;             // TradingView's flags on it
};

// The Entry rows of `<dir>/<slug>/tv_trades.csv`, in time order. `ok` turns
// false when the file is missing, a row does not parse, or a Signal is not a
// flag word, so a test reads a broken fixture as a failure, never as a pass.
inline std::vector<TapeBar> read_tape(const std::string& dir, const std::string& slug, bool& ok) {
    std::ifstream in(dir + "/" + slug + "/tv_trades.csv");
    std::vector<TapeBar> bars;
    if (!in) { ok = false; return bars; }
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
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) {
            ok = false;
            continue;
        }
        std::string word = cell[3];
        if (word.size() >= 8 && word[0] == 'C') {
            std::string spelled;
            const char* letters = "MPQFLfl";
            for (int k = 0; k < 7; ++k) { spelled += letters[k]; spelled += word[1 + k]; }
            word = spelled;
        }
        if (word.size() != 14) { ok = false; continue; }
        TapeBar bar;
        bar.ts = utc_ms(y, mo, d, h, mi, 8);
        bool* slot[7] = {&bar.tv.market, &bar.tv.premarket, &bar.tv.postmarket, &bar.tv.first,
                         &bar.tv.last, &bar.tv.first_regular, &bar.tv.last_regular};
        const char* letters = "MPQFLfl";
        for (int k = 0; k < 7; ++k) {
            if (word[2 * k] != letters[k] || (word[2 * k + 1] != '0' && word[2 * k + 1] != '1')) {
                ok = false;
                break;
            }
            *slot[k] = word[2 * k + 1] == '1';
        }
        bars.push_back(bar);
    }
    for (std::size_t i = 1; i < bars.size(); ++i)
        for (std::size_t j = i; j > 0 && bars[j - 1].ts > bars[j].ts; --j)
            std::swap(bars[j - 1], bars[j]);
    if (bars.empty()) ok = false;
    return bars;
}

inline pineforge::Bar flat_bar(std::int64_t ts) {
    pineforge::Bar b{};
    b.timestamp = ts;
    b.open = 100.0; b.high = 101.0; b.low = 99.0; b.close = 100.5;
    b.volume = 1.0;
    return b;
}

}  // namespace session_tape
