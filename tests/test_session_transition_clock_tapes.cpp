// TradingView's session clock across a daylight-saving switch, replayed row
// for row against TradingView's own tapes.
//
// Every fixture under tests/fixtures/session_transition_clock is a
// self-written BINANCE:ETHUSDT.P 15 script (syminfo.timezone Etc/UTC) that
// reads time(tf, "<session>", "<zone>") for a few sessions around the 2025
// autumn and 2026 spring switches of New York or London and prints, on every
// bar, whether each one is na (KEY=0) or not (KEY=1) -- or, for
// ny-fallback-1800-0200-stamp, the value itself (SESSION=<ms> / NaN) -- into
// the order comment that TradingView's export carries as the Entry row's
// Signal. This test calls exactly what codegen emits for those calls,
// pine_time(bar, tf, session, tz, script_tf_, syminfo_.timezone,
// syminfo_.session) with script_tf_ = "15", for every Entry row and every
// session the row prints, and requires TradingView's answer on each: the
// na-ness, and the stamp where the tape prints one. The script's sessions
// and the comment's keys are read from the fixture's strategy.pine itself.
//
// Fail-before: with detail::session_clock_switches().transition_wall_clock
// off (the plain wall-clock windows the engine read before the rule), the
// same rows must NOT all match, and two known deviating bars must read the
// old answers -- New York's 1800-0200 opening at 18:00 EDT (22:00 UTC) on
// 2025-11-01, where TradingView opens it at 18:00 EST (23:00 UTC), and its
// 06:15 UTC bar that morning, na before the rule and in the session on
// TradingView's tape.
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PINEFORGE_SESSION_TRANSITION_CLOCK_FIXTURE_DIR
#error "PINEFORGE_SESSION_TRANSITION_CLOCK_FIXTURE_DIR must name tests/fixtures/session_transition_clock"
#endif

namespace {

const char* const kFixtures[] = {
    "ny-fallback-round1",  "ny-spring-round1", "ny-fallback-round2", "ny-spring-round2",
    "london-autumn-round2", "london-spring-round2", "ny-round3",       "london-round3",
    "ny-round4",           "london-round4",    "ny-fallback-1800-0200-stamp",
};

// The chart and the symbol every fixture was exported on.
const std::string kChartTf = "15";
const std::string kSymTz = "Etc/UTC";
const std::string kSymSession = "24x7";

struct Site {
    std::string tf;  // "" for timeframe.period
    std::string session;
    std::string tz;
};

struct Script {
    std::map<std::string, Site> sites;        // variable -> its time() call
    std::map<std::string, std::string> keys;  // comment key -> variable
};

struct Row {
    std::string label;  // the tape's own time, UTC+8
    int64_t bar_ms = 0;
    std::vector<std::pair<std::string, std::string>> fields;
};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return std::string();
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Script read_script(const std::string& path) {
    Script script;
    const std::string text = read_file(path);
    const std::regex site(
        R"re((?:^|\n)[ \t]*(?:int[ \t]+)?(\w+)[ \t]*=[ \t]*time\([ \t]*(timeframe\.period|"[^"]*")[ \t]*,[ \t]*"([^"]+)"[ \t]*,[ \t]*"([^"]+)"[ \t]*\))re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), site);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        Site s;
        const std::string tf = m[2].str();
        s.tf = tf == "timeframe.period" ? std::string() : tf.substr(1, tf.size() - 2);
        s.session = m[3].str();
        s.tz = m[4].str();
        script.sites[m[1].str()] = s;
    }
    const std::regex key(R"re("[|;]?([A-Z][A-Z0-9]*)=" \+ (?:\(na\((\w+)\)|str\.tostring\((\w+)\)))re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), key);
         it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        script.keys[m[1].str()] = m[2].matched ? m[2].str() : m[3].str();
    }
    return script;
}

int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// "YYYY-MM-DD HH:MM" on the export's UTC+8 clock -> epoch ms.
bool tape_time_ms(const std::string& s, int64_t& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5)
        return false;
    out = (days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 - 8 * 3600) * 1000;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<Row> read_entry_rows(const std::string& path) {
    std::vector<Row> rows;
    std::istringstream in(read_file(path));
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        const std::vector<std::string> cols = split(line, ',');
        if (cols.size() < 4 || cols[1].rfind("Entry", 0) != 0)
            continue;
        Row row;
        row.label = cols[2];
        if (!tape_time_ms(cols[2], row.bar_ms)) {
            std::fprintf(stderr, "bad tape time %s in %s\n", cols[2].c_str(), path.c_str());
            continue;
        }
        std::string signal = cols[3];
        for (char& c : signal) {
            if (c == ';')
                c = '|';
        }
        for (const std::string& field : split(signal, '|')) {
            const std::size_t eq = field.find('=');
            if (eq != std::string::npos)
                row.fields.emplace_back(field.substr(0, eq), field.substr(eq + 1));
        }
        rows.push_back(row);
    }
    return rows;
}

int64_t engine_time(int64_t bar_ms, const Site& site) {
    // codegen: pine_time(current_bar_.timestamp, <tf>, <session>, <tz>, script_tf_
    //          PF_PINE_TIME_SESSION_DAY_ARGS(syminfo_.timezone, syminfo_.session))
    const std::string tf = site.tf.empty() ? kChartTf : site.tf;
    return pineforge::pine_time(bar_ms, tf, site.session, site.tz, kChartTf, kSymTz, kSymSession);
}

struct Tally {
    int rows = 0;
    int fields = 0;
    int mismatches = 0;
};

// Every Entry row of one fixture against the engine; prints at most
// `print` mismatching rows.
Tally replay(const std::string& name, int print, bool& broken) {
    const std::string dir = std::string(PINEFORGE_SESSION_TRANSITION_CLOCK_FIXTURE_DIR) + "/" + name;
    const Script script = read_script(dir + "/strategy.pine");
    const std::vector<Row> rows = read_entry_rows(dir + "/tv_trades.csv");
    Tally tally;
    if (script.sites.empty() || script.keys.empty() || rows.empty()) {
        std::fprintf(stderr, "%s: no sessions, keys or rows read\n", name.c_str());
        broken = true;
        return tally;
    }
    for (const Row& row : rows) {
        bool row_ok = true;
        bool any = false;
        std::string detail;
        for (const auto& field : row.fields) {
            const auto key = script.keys.find(field.first);
            if (key == script.keys.end())
                continue;
            const auto site = script.sites.find(key->second);
            if (site == script.sites.end()) {
                std::fprintf(stderr, "%s: key %s names no time() call\n", name.c_str(),
                             field.first.c_str());
                broken = true;
                continue;
            }
            any = true;
            ++tally.fields;
            const int64_t got = engine_time(row.bar_ms, site->second);
            const bool got_na = pineforge::is_na(got);
            bool ok = false;
            if (field.second == "0" || field.second == "1") {
                ok = got_na == (field.second == "0");
            } else if (field.second == "NaN") {
                ok = got_na;
            } else {
                ok = !got_na && std::to_string(got) == field.second;
            }
            if (!ok) {
                row_ok = false;
                detail += " " + field.first + "(" + site->second.session + ") tv=" + field.second
                    + " engine=" + (got_na ? std::string("na") : std::to_string(got));
            }
        }
        if (!any) {
            std::fprintf(stderr, "%s: row %s prints no session\n", name.c_str(), row.label.c_str());
            broken = true;
            continue;
        }
        ++tally.rows;
        if (!row_ok) {
            if (tally.mismatches < print)
                std::printf("  MISMATCH %s %s (UTC+8):%s\n", name.c_str(), row.label.c_str(),
                            detail.c_str());
            ++tally.mismatches;
        }
    }
    return tally;
}

// The chart's own time_close. The Pine host reads its bar close through the
// SYMBOL's session (chart_bar_close_ms, and pine_time_offset's close of a chart
// bar) on the plain wall-clock windows: the session clock is pinned on script
// arguments only. 0200-2200 Europe/Berlin starts at the 2026-03-29 switch's
// pre-transition wall time, so the two readings differ that morning.
class ChartCloseHost final : public pineforge::source::PineStrategyHost {
public:
    ChartCloseHost() {
        attach_pine_execution_adapter();
        configure_pine_strategy(pineforge::source::PineStrategyConfig{});
        set_syminfo_timezone("Europe/Berlin");
        set_syminfo_session("0200-2200");
    }
    void on_source_bar(const pineforge::Bar&) override {}
    int64_t chart_close(int64_t stamp) const { return chart_bar_close_ms(stamp); }
    int64_t offset_close(int64_t stamp) const {
        return pine_time_offset(stamp, 0, kChartTf, "", "", 0, true);
    }
};

// Returns the number of bars whose plain and clock readings differ (must be
// > 0 for the check to bite); `ok` turns false on any host read that is not
// the plain one.
int chart_close_stays_plain(bool& ok) {
    const int64_t from = 1774699200000;  // 2026-03-28 12:00 UTC
    const int64_t to = 1774872000000;    // 2026-03-30 12:00 UTC
    std::vector<pineforge::Bar> bars;
    for (int64_t t = from; t < to; t += 15 * 60000) {
        pineforge::Bar bar{};
        bar.timestamp = t;
        bar.open = bar.high = bar.low = bar.close = 100.0;
        bar.volume = 1.0;
        bars.push_back(bar);
    }
    ChartCloseHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), kChartTf, kChartTf, false);
    if (!host.last_error().empty()) {
        std::printf("chart close host: run error %s\n", host.last_error().c_str());
        ok = false;
        return 0;
    }
    int differing = 0;
    for (const pineforge::Bar& bar : bars) {
        const int64_t plain = pineforge::detail::symbol_session_time_close(
            bar.timestamp, kChartTf, "0200-2200", "Europe/Berlin", kChartTf);
        const int64_t clock = pineforge::pine_time_close(bar.timestamp, kChartTf, "0200-2200",
                                                         "Europe/Berlin", kChartTf);
        const auto same = [](int64_t a, int64_t b) {
            return (pineforge::is_na(a) && pineforge::is_na(b)) || a == b;
        };
        if (!same(plain, clock)) ++differing;
        if (!same(host.chart_close(bar.timestamp), plain)
            || !same(host.offset_close(bar.timestamp), plain)) {
            std::printf("chart close at %lld: host %lld / offset %lld, plain %lld\n",
                        static_cast<long long>(bar.timestamp),
                        static_cast<long long>(host.chart_close(bar.timestamp)),
                        static_cast<long long>(host.offset_close(bar.timestamp)),
                        static_cast<long long>(plain));
            ok = false;
        }
    }
    return differing;
}

}  // namespace

int main() {
    bool broken = false;
    int failures = 0;
    pineforge::detail::session_clock_switches().transition_wall_clock = true;
    int total_rows = 0;
    for (const char* name : kFixtures) {
        const Tally t = replay(name, 8, broken);
        std::printf("fixture %-28s rows checked %4d / mismatches %d (session reads %d)\n", name,
                    t.rows, t.mismatches, t.fields);
        failures += t.mismatches;
        total_rows += t.rows;
    }
    std::printf("all fixtures: rows checked %d / mismatches %d\n", total_rows, failures);

    // Fail-before: the plain wall-clock windows the engine read before the session clock.
    const Site ny{std::string(), "1800-0200", "America/New_York"};
    const int64_t open_edt = 1762034400000;  // 2025-11-01 22:00 UTC, 18:00 EDT
    const int64_t open_est = 1762038000000;  // 2025-11-01 23:00 UTC, 18:00 EST
    const int64_t phantom = 1761977700000;   // 2025-11-01 06:15 UTC, 01:15 EST
    const int64_t on_edt = engine_time(open_edt, ny);
    const int64_t on_est = engine_time(open_est, ny);
    const int64_t on_phantom = engine_time(phantom, ny);
    pineforge::detail::session_clock_switches().transition_wall_clock = false;
    const int64_t off_edt = engine_time(open_edt, ny);
    const int64_t off_est = engine_time(open_est, ny);
    const int64_t off_phantom = engine_time(phantom, ny);
    int off_failures = 0;
    for (const char* name : kFixtures) {
        const Tally t = replay(name, 0, broken);
        std::printf("switch off: fixture %-28s rows checked %4d / mismatches %d\n", name, t.rows,
                    t.mismatches);
        off_failures += t.mismatches;
    }
    pineforge::detail::session_clock_switches().transition_wall_clock = true;
    // The switch flips back cleanly: the day caches never mix the readings.
    const int64_t again_edt = engine_time(open_edt, ny);
    const int64_t again_phantom = engine_time(phantom, ny);
    std::printf("switch off: all fixtures mismatches %d (must be > 0)\n", off_failures);
    std::printf("NY 1800-0200 2025-11-01 22:00Z: on=%s off=%s; 23:00Z: on=%lld off=%lld; "
                "06:15Z: on=%s off=%s\n",
                pineforge::is_na(on_edt) ? "na" : std::to_string(on_edt).c_str(),
                pineforge::is_na(off_edt) ? "na" : std::to_string(off_edt).c_str(),
                static_cast<long long>(on_est), static_cast<long long>(off_est),
                pineforge::is_na(on_phantom) ? "na" : std::to_string(on_phantom).c_str(),
                pineforge::is_na(off_phantom) ? "na" : std::to_string(off_phantom).c_str());
    bool chart_ok = true;
    const int chart_differing = chart_close_stays_plain(chart_ok);
    std::printf("chart time_close on the plain windows: %s (%d bars where the session clock "
                "would differ, must be > 0)\n", chart_ok ? "yes" : "NO", chart_differing);
    bool ok = !broken && failures == 0 && off_failures > 0 && chart_ok && chart_differing > 0;
    ok = ok && pineforge::is_na(on_edt) && off_edt == open_edt;  // TV: na until 18:00 EST
    ok = ok && on_est == open_est && off_est == open_est;
    ok = ok && on_phantom == phantom && pineforge::is_na(off_phantom);
    ok = ok && pineforge::is_na(again_edt) && again_phantom == phantom;
    if (!ok) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
