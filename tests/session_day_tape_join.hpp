#pragma once

#include "session_flag_tape_fixture.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace session_tape {

// Both sizes are counts of the recorded bars, independent of the engine.
struct ChartCase {
    const char* chart;
    const char* feed;
    const char* session;
    const char* zone;
    const char* input_tf;
    const char* chart_tf;
    std::size_t chart_size;
    std::size_t feed_size;
    int at_open_misses;
};

inline constexpr ChartCase split_charts[] = {
    {"ksw-flags-tse7203-60", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15", "60", 70, 230, 10},
    {"ksw-flags-tse7203-240", "cgs2-flags-tse7203-15", "0900-1130,1230-1545", "Asia/Tokyo", "15", "240", 20, 230, 0},
    {"ksw-flags-zc1-60", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15", "60", 190, 710, 10},
    {"ksw-flags-zc1-240", "cgs2-flags-zc1-15", "1900-0745,0830-1320", "America/Chicago", "15", "240", 50, 710, 0},
    {"cgs2-flags-hkex700-15", "cgs2-flags-hkex700-15", "0930-1200,1300-1600", "Asia/Hong_Kong", "15", "15", 220, 220, 0},
    {"ksw-flags-hkex700-60", "cgs2-flags-hkex700-15", "0930-1200,1300-1600", "Asia/Hong_Kong", "15", "60", 60, 220, 0},
    {"ksw-flags-hkex700-240", "ksw-flags-hkex700-240", "0930-1200,1300-1600", "Asia/Hong_Kong", "240", "240", 20, 20, 0},
    {"ksw-flags-hkex700-240", "cgs2-flags-hkex700-15", "0930-1200,1300-1600", "Asia/Hong_Kong", "15", "240", 20, 220, 0},
};

inline std::string arm(const ChartCase& c) {
    return std::string(c.chart) + " " + c.input_tf + "->" + c.chart_tf;
}

template<typename Rows>
bool ordered(const Rows& rows) {
    return std::adjacent_find(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.ts >= b.ts;
    }) == rows.end();
}

inline bool read_tape(const ChartCase& c, const char* dir, const char* slug, const char* role,
                      std::size_t required, std::vector<TapeBar>& rows) {
    const std::string blob = std::string("tests/fixtures/session_windows/") + slug + "/tv_trades.csv";
    const std::string path = std::string(dir) + "/" + slug + "/tv_trades.csv";
    std::ifstream in(path, std::ios::binary);
    std::ostringstream raw;
    raw << in.rdbuf();
    if (!in.is_open() || in.bad() || !raw) {
        std::printf("  NOT COMPARED %s %s: cannot read %s\n", arm(c).c_str(), role, blob.c_str());
        return false;
    }
    const std::string bytes = raw.str();
    std::printf("  TAPE %s %s %s bytes=%zu\n", arm(c).c_str(), role, blob.c_str(), bytes.size());
    std::istringstream entries(bytes);
    bool ok = true;
    rows = read_entries(entries, ok);
    if (!ok || rows.size() != required || !ordered(rows)) {
        std::printf("  NOT COMPARED %s %s: parsed=%d rows=%zu required=%zu ordered_unique=%d\n",
                    arm(c).c_str(), role, ok, rows.size(), required, ordered(rows));
        return false;
    }
    return true;
}

inline bool inputs(const ChartCase& c, const char* dir, std::vector<TapeBar>& tape,
                   std::vector<pineforge::Bar>& bars) {
    std::vector<TapeBar> feed;
    const bool chart_ok = read_tape(c, dir, c.chart, "chart", c.chart_size, tape);
    const bool feed_ok = read_tape(c, dir, c.feed, "feed", c.feed_size, feed);
    if (!chart_ok || !feed_ok) return false;
    for (const auto& row : feed) bars.push_back(flat_bar(row.ts));
    return true;
}

// Compare values only at common opens. A partial join can locate a matched
// wrong bit but can never establish either verdict for the whole chart.
template<typename Rows, typename Compare>
bool join(const ChartCase& c, const Rows& engine, const std::vector<TapeBar>& tape, Compare compare) {
    if (engine.empty() || tape.empty() || !ordered(engine) || !ordered(tape)) {
        std::printf("  NOT COMPARED %s: need nonempty distinct ordered opens, engine=%zu tape=%zu\n",
                    arm(c).c_str(), engine.size(), tape.size());
        return false;
    }
    std::size_t e = 0, t = 0, matched = 0;
    while (e < engine.size() || t < tape.size()) {
        if (t == tape.size() || (e < engine.size() && engine[e].ts < tape[t].ts)) {
            std::printf("  NOT COMPARED %s engine_open_ms=%lld tape_open_ms=<absent>\n",
                        arm(c).c_str(), static_cast<long long>(engine[e++].ts));
        } else if (e == engine.size() || tape[t].ts < engine[e].ts) {
            std::printf("  NOT COMPARED %s engine_open_ms=<absent> tape_open_ms=%lld\n",
                        arm(c).c_str(), static_cast<long long>(tape[t++].ts));
        } else {
            compare(engine[e], tape[t], e + 1 == engine.size() || t + 1 == tape.size());
            ++matched;
            ++e;
            ++t;
        }
    }
    std::printf("  %s engine=%zu tape=%zu matched=%zu engine_only=%zu tape_only=%zu\n",
                arm(c).c_str(), engine.size(), tape.size(), matched,
                engine.size() - matched, tape.size() - matched);
    return matched == engine.size() && matched == tape.size();
}

inline int miss(const ChartCase& c, std::int64_t open, const char* column, bool engine, bool tv) {
    if (engine == tv) return 0;
    std::printf("  FAIL %s matched open_ms=%lld %s engine=%d tv=%d\n",
                arm(c).c_str(), static_cast<long long>(open), column, engine, tv);
    return 1;
}

}  // namespace session_tape
