#pragma once
// A `lab tv --no-note` tape whose synthetic strategy spells what it read on
// each chart bar in the comment of an exit (lane W11-ENG-TIME-COLOR's probes,
// tests/fixtures/{session_clock,color_tv,supertrend_warmup,daily_break_close}).
// An order placed on a chart bar fills at the next bar's open, so an exit
// row's Signal is what the script read on the chart bar before the one the
// exit fills on. Every probe fills an order on every chart bar from the
// second on, so the fill times are the chart bars' opens, gaps and weekends
// included: a reading belongs to the latest fill time before its exit's.
// tv_trades.csv renders times at UTC+8 and quotes a Signal holding a comma.
// No engine header: the tape alone.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace exit_comment_tape {

constexpr std::int64_t kMinute = 60'000;

inline std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" at UTC+8 -> UTC milliseconds; -1 when it does not parse.
inline std::int64_t tape_time_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

// One CSV record: fields split at commas outside double quotes, a doubled
// quote inside quotes read as one.
inline std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { out.back() += '"'; ++i; }
            else if (c == '"') quoted = false;
            else out.back() += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.emplace_back();
        } else if (c != '\r') {
            out.back() += c;
        }
    }
    return out;
}

inline std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> out(1);
    for (const char c : text) {
        if (c == separator) out.emplace_back();
        else out.back() += c;
    }
    return out;
}

struct Reading {
    std::int64_t bar_ms = 0;  // the chart bar the script read it on, its open
    std::string signal;       // the exit comment
};

// The readings of `<dir>/<slug>/tv_trades.csv`, in bar order (a bar whose
// script closed several positions has one reading per exit). `ok` turns false
// when the file is missing, a time does not parse, or a reading has no earlier
// fill to belong to, so a test reads a broken fixture as a failure, never as
// a pass. TradingView's range-end close of a still-open position carries no
// comment and is no reading.
inline std::vector<Reading> read(const std::string& dir, const std::string& slug, bool& ok) {
    std::ifstream in(dir + "/" + slug + "/tv_trades.csv");
    std::vector<Reading> readings;
    if (!in) {
        std::printf("  missing tape %s/%s\n", dir.c_str(), slug.c_str());
        ok = false;
        return readings;
    }
    std::set<std::int64_t> fills;
    std::vector<std::pair<std::int64_t, std::string>> exits;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        const auto cell = csv_fields(line);
        if (cell.size() < 4) continue;
        const std::int64_t t = tape_time_ms(cell[2]);
        if (t < 0) { ok = false; continue; }
        fills.insert(t);
        if (cell[1].rfind("Exit", 0) == 0 && !cell[3].empty()) exits.emplace_back(t, cell[3]);
    }
    std::sort(exits.begin(), exits.end());
    for (const auto& exit : exits) {
        const auto after = fills.lower_bound(exit.first);
        if (after == fills.begin()) { ok = false; continue; }
        readings.push_back({*std::prev(after), exit.second});
    }
    if (readings.empty()) ok = false;
    return readings;
}

}  // namespace exit_comment_tape
