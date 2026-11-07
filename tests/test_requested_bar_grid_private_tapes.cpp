/*
 * Lab-only requested-bar grid measurements for the next release.
 *
 * Inputs are supplied only by environment variables; no private file is
 * embedded, copied, generated, or written by this executable.
 * PINEFORGE_REQBAR_NYSE_FEED: chart 60 CSV, 287 bars, first timestamp
 * 2024-01-02 14:30 UTC. Reference sha256 prefix d90236d4.
 *
 * Row         | Class    | Tape environment variable          | Expected
 * E-ORIG-300  | EXPLICIT | PINEFORGE_REQBAR_NYSE_TAPE_ORIG     | qty, exit time
 * E-TRACE-300 | EXPLICIT | PINEFORGE_REQBAR_NYSE_TAPE_TRACE    | raw ro, ri
 *
 * Tape reference sha256 prefixes: orig aff81207ebf08f09, trace f78a73f0fdad92e7.
 * These references are documentation only, never assertions or row selectors.
 * Each tape has 40 entries. Every expected quantity is its ENTRY's literal
 * Size (qty) decimal; every expected exit is the paired EXIT's Date and time.
 * Tape labels have the measured +8 hour offset: UTC = label minus 8 hours.
 * Join by entry fill time UTC and Signal before the first '|', never position.
 * Include terminal range-end trades even when the exit Signal is empty.
 *
 * The original source makes one request.security(tickerid, "300", bar_index).
 * The trace source retains it and adds a tuple request for
 * [time, bar_index, ta.valuewhen(barstate.isfirst, time, 0)]. ENTRY Signal:
 * orbit|c=..|ci=..|ro=..|ri=..|ti=..|cf=..|rf=...
 * Trace expectations are ONLY its raw requested open timestamp ro and scalar
 * requested index ri, observed at chart signal time c. Other telemetry fields
 * are parsed, not compared as payload expectations. No expectation is inferred
 * from a quantity, from the engine, or from a locally reconstructed grid.
 *
 * Like test_requested_bar_grid_tapes, register_security_eval uses input_tf_,
 * gaps=false, lookahead=false. Each counter starts at -1 and increments only
 * on security_series_slot_is_new. Quantity is 1 + requested_index % 5;
 * enter when chart index > 6 and % 7 == 3, close when > 6 and % 7 == 5.
 * EXPLICIT sets only timezone, session and type; DEFAULT sets no symbol facts.
 * The two EXPLICIT rows are the fix criterion. DEFAULT rows remain owed under
 * the caller-metadata contract (CR-738), retained separately as a reverse patch.
 *
 * Diagnostics never echo paths, CSV cells, prices, OHLCV, full trade objects,
 * or engine exception messages. Only quantities, counts, indices, timestamps,
 * fixed identifiers, environment names and controlled error messages appear.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

constexpr const char* kFeed = "PINEFORGE_REQBAR_NYSE_FEED";
constexpr const char* kOrig = "PINEFORGE_REQBAR_NYSE_TAPE_ORIG";
constexpr const char* kTrace = "PINEFORGE_REQBAR_NYSE_TAPE_TRACE";
constexpr const char* kDefaultLabel =
    "DEFAULT: waits on the caller-metadata contract (CR-738)";
constexpr std::int64_t kHour = 3'600'000;
constexpr std::size_t kEntries = 40;
using Key = std::pair<std::int64_t, std::string>;
using Trace = std::array<std::int64_t, 7>;
constexpr const char* kTraceFields[] = {"c", "ci", "ro", "ri", "ti", "cf", "rf"};

// Only this exception's controlled text is safe to print.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& reason) {
    if (!condition) throw Failure(reason);
}

std::int64_t integer(const std::string& text) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "invalid integer");
    return result;
}

double number(const std::string& text) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double result = 0;
    in >> std::noskipws >> result;
    require(!in.fail() && in.peek() == std::char_traits<char>::eof()
                && std::isfinite(result), "invalid finite number");
    return result;
}

// Normalize decimal spelling, without converting the expected quantity to
// floating point. The strategy's modulo carrier is exactly integral.
std::string decimal(const std::string& text) {
    bool point = false, digit = false;
    for (char c : text) {
        if (c == '.' && !point && digit) {
            point = true;
            digit = false;
        } else {
            require(c >= '0' && c <= '9', "invalid quantity decimal");
            digit = true;
        }
    }
    require(digit, "empty or incomplete quantity decimal");
    const auto dot = text.find('.');
    std::string whole = text.substr(0, dot);
    const auto nonzero = whole.find_first_not_of('0');
    whole = nonzero == std::string::npos ? "0" : whole.substr(nonzero);
    std::string fraction = dot == std::string::npos ? "" : text.substr(dot + 1);
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    require(whole != "0" || !fraction.empty(), "nonpositive quantity");
    return whole + (fraction.empty() ? "" : "." + fraction);
}

std::string quantity_text(double qty) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << qty;
    return out.str();
}

bool equal_quantity(double got, const std::string& expected) {
    return std::isfinite(got) && got > 0 && got == std::trunc(got)
        && quantity_text(got) == expected;
}

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> cells;
    std::size_t pos = 0;
    for (;;) {
        std::string cell;
        if (pos < line.size() && line[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < line.size()) {
                const char c = line[pos++];
                if (c != '"') cell += c;
                else if (pos < line.size() && line[pos] == '"') {
                    cell += '"';
                    ++pos;
                } else {
                    closed = true;
                    break;
                }
            }
            require(closed, "unterminated CSV quote");
        } else {
            while (pos < line.size() && line[pos] != ',') {
                require(line[pos] != '"', "quote inside unquoted CSV cell");
                cell += line[pos++];
            }
        }
        cells.push_back(cell);
        if (pos == line.size()) break;
        require(line[pos++] == ',', "unexpected text after CSV quote");
    }
    return cells;
}

using Csv = std::vector<std::vector<std::string>>;

Csv read_csv(const char* variable) {
    const char* path = std::getenv(variable);
    require(path != nullptr && *path != '\0', std::string(variable) + ": unset or empty");
    std::ifstream in(path);
    require(in.is_open(), std::string(variable) + ": missing or unreadable file");
    Csv rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (rows.empty() && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        try {
            rows.push_back(csv_fields(line));
        } catch (const Failure& error) {
            throw Failure(std::string(variable) + ": line "
                          + std::to_string(rows.size() + 1) + ": " + error.what());
        }
    }
    require(in.eof() && !in.bad() && !rows.empty(),
            std::string(variable) + ": unreadable or empty CSV");
    return rows;
}

std::vector<Bar> read_feed() {
    const Csv rows = read_csv(kFeed);
    require(rows.front() == std::vector<std::string>(
                {"timestamp", "open", "high", "low", "close", "volume"}),
            std::string(kFeed) + ": unexpected feed header");
    std::vector<Bar> bars;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        try {
            const auto& f = rows[i];
            require(f.size() == 6, "expected six feed columns");
            Bar bar{};
            bar.timestamp = integer(f[0]);
            bar.open = number(f[1]); bar.high = number(f[2]);
            bar.low = number(f[3]); bar.close = number(f[4]);
            bar.volume = number(f[5]);
            // Session gaps and the final short bar are legitimate.
            require(bar.timestamp > 0 && (bars.empty()
                        || bar.timestamp > bars.back().timestamp),
                    "feed timestamps must strictly increase");
            require(bar.low <= bar.open && bar.open <= bar.high
                        && bar.low <= bar.close && bar.close <= bar.high
                        && bar.volume >= 0, "invalid feed OHLCV");
            bars.push_back(bar);
        } catch (const Failure& error) {
            throw Failure(std::string(kFeed) + ": line " + std::to_string(i + 1)
                          + ": " + error.what());
        }
    }
    require(bars.size() == 287, std::string(kFeed) + ": expected 287 chart bars");
    return bars;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

std::int64_t tape_ms(const std::string& text) {
    require(text.size() == 16 && text[4] == '-' && text[7] == '-'
                && text[10] == ' ' && text[13] == ':', "invalid tape date");
    const int y = static_cast<int>(integer(text.substr(0, 4)));
    const int mo = static_cast<int>(integer(text.substr(5, 2)));
    const int d = static_cast<int>(integer(text.substr(8, 2)));
    const int h = static_cast<int>(integer(text.substr(11, 2)));
    const int mi = static_cast<int>(integer(text.substr(14, 2)));
    const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    require(y >= 1970 && mo >= 1 && mo <= 12 && h >= 0 && h < 24
                && mi >= 0 && mi < 60, "out-of-range tape date");
    const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    require(d >= 1 && d <= month_days[mo - 1] + (mo == 2 && leap),
            "out-of-range tape day");
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))
                * 24 + h) * 60 + mi) * 60'000 - 8 * kHour;
}

Trace read_trace(const std::string& signal) {
    require(signal.substr(0, signal.find('|')) == "orbit", "unexpected trace id");
    Trace values{};
    std::size_t pos = signal.find('|');
    for (std::size_t i = 0; i < values.size(); ++i) {
        require(pos != std::string::npos, "missing trace field");
        const auto end = signal.find('|', pos + 1);
        const auto field = signal.substr(pos + 1,
            end == std::string::npos ? end : end - pos - 1);
        const std::string prefix = std::string(kTraceFields[i]) + "=";
        require(field.compare(0, prefix.size(), prefix) == 0, "unexpected trace field");
        values[i] = integer(field.substr(prefix.size()));
        pos = end;
    }
    require(pos == std::string::npos, "extra trace fields");
    return values;
}

struct TapeTrade {
    std::int64_t entry_ms = 0, exit_ms = 0;
    std::string qty;
    Trace trace{};
    bool entry_seen = false, exit_seen = false;
};
using Tape = std::map<Key, TapeTrade>;

Tape read_tape(const char* variable, bool trace) {
    const Csv rows = read_csv(variable);
    const auto& header = rows.front();
    // The currency suffix is feed-specific; no price column is read.
    require(header.size() == 17 && header[0] == "Trade number" && header[1] == "Type"
                && header[2] == "Date and time" && header[3] == "Signal"
                && header[4].compare(0, 6, "Price ") == 0 && header[5] == "Size (qty)",
            std::string(variable) + ": unexpected tape header");
    std::map<std::int64_t, TapeTrade> pairs;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        try {
            const auto& f = rows[i];
            require(f.size() == header.size(), "wrong tape column count");
            const auto n = integer(f[0]);
            require(n > 0, "nonpositive trade number");
            const bool entry = f[1] == "Entry long";
            require(entry || f[1] == "Exit long", "unexpected trade type");
            auto& t = pairs[n];
            require(entry ? (!t.entry_seen && t.exit_seen)
                          : (!t.exit_seen && !t.entry_seen), "expected Exit then Entry");
            const auto ms = tape_ms(f[2]);
            const auto qty = decimal(f[5]);
            if (entry) {
                require(qty == t.qty, "entry/exit quantity differs");
                require(f[3].substr(0, f[3].find('|')) == "orbit", "unexpected entry id");
                t.entry_seen = true;
                t.entry_ms = ms;
                t.qty = qty;
                if (trace) t.trace = read_trace(f[3]);
                require(t.entry_ms < t.exit_ms, "invalid entry/exit order");
            } else {
                t.exit_seen = true;
                t.exit_ms = ms;
                t.qty = qty;
            }
        } catch (const Failure& error) {
            throw Failure(std::string(variable) + ": line " + std::to_string(i + 1)
                          + ": " + error.what());
        }
    }
    Tape trades;
    std::int64_t next = 1;
    for (const auto& pair : pairs) {
        const auto& t = pair.second;
        require(pair.first == next++ && t.entry_seen && t.exit_seen,
                std::string(variable) + ": unpaired tape trade");
        require(trades.emplace(Key{t.entry_ms, "orbit"}, t).second,
                std::string(variable) + ": duplicate entry key");
    }
    return trades;
}

class RequestedGridHost final : public source::PineStrategyHost {
public:
    RequestedGridHost(bool trace, bool explicit_arm) : trace_(trace) {
        attach_pine_execution_adapter();
        if (explicit_arm) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
            set_syminfo_type("stock");
        }
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100;
        configure_pine_strategy(config);
    }

    void prepare_script_run(const Bar*, int, bool) override {
        indices_ = {{-1, -1}};
        requested_ = std::numeric_limits<double>::quiet_NaN();
        tuple_index_ = -1;
        requested_open_ms_ = first_requested_ms_ = first_chart_ms_ = -1;
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "300", input_tf_, false, false);
        if (trace_) register_security_eval(1, "300", input_tf_, false, false);
    }

    void evaluate_security(int id, const Bar& bar, bool) override {
        require(id == 0 || (id == 1 && trace_), "unexpected security evaluator id");
        if (security_series_slot_is_new(id)) ++indices_[static_cast<std::size_t>(id)];
        if (id == 0) requested_ = indices_[0];
        else {
            requested_open_ms_ = bar.timestamp;
            tuple_index_ = indices_[1];
            if (first_requested_ms_ < 0) first_requested_ms_ = bar.timestamp;
        }
    }

    void clear_security(int id) override {
        if (id == 0) requested_ = std::numeric_limits<double>::quiet_NaN();
        else {
            requested_open_ms_ = -1;
            tuple_index_ = -1;
        }
    }

    void on_source_bar(const Bar& bar) override {
        if (first_chart_ms_ < 0) first_chart_ms_ = bar.timestamp;
        const int index = pine_bar_index();
        const double qty = 1 + std::fmod(requested_, 5.0);
        if (index > 6 && index % 7 == 3) {
            std::string comment;
            if (trace_) {
                require(std::isfinite(requested_) && requested_ >= 0
                            && requested_ <= std::numeric_limits<int>::max()
                            && requested_ == std::trunc(requested_),
                        "trace scalar index unavailable or nonintegral");
                const Trace values{{bar.timestamp, index, requested_open_ms_,
                                    static_cast<std::int64_t>(requested_), tuple_index_,
                                    first_chart_ms_, first_requested_ms_}};
                comment = "orbit";
                for (std::size_t i = 0; i < values.size(); ++i)
                    comment += "|" + std::string(kTraceFields[i]) + "="
                        + std::to_string(values[i]);
            }
            strategy_entry("orbit", true, na<double>(), na<double>(), qty, comment);
        }
        if (index > 6 && index % 7 == 5) strategy_close("orbit");
    }

private:
    bool trace_;
    std::array<int, 2> indices_{{-1, -1}};
    int tuple_index_ = -1;
    double requested_ = std::numeric_limits<double>::quiet_NaN();
    std::int64_t requested_open_ms_ = -1, first_requested_ms_ = -1, first_chart_ms_ = -1;
};

struct Row {
    const char* name;
    bool explicit_arm, trace;
};
const Row kRows[] = {
    {"E-ORIG-300", true, false}, {"E-TRACE-300", true, true},
};

const char* row_class(const Row& row) {
    return row.explicit_arm ? "EXPLICIT" : "DEFAULT";
}

void print_default_label(const Row& row) {
    if (!row.explicit_arm) std::printf("  %s\n", kDefaultLabel);
}

std::string expected_text(const TapeTrade& t, bool trace) {
    if (trace) return "ro=" + std::to_string(t.trace[2])
        + " ri=" + std::to_string(t.trace[3]);
    return "qty=" + t.qty + " exit_ms=" + std::to_string(t.exit_ms);
}

std::string actual_text(const Trade& t, bool trace) {
    if (trace) {
        const Trace raw = read_trace(t.entry_comment);
        return "ro=" + std::to_string(raw[2]) + " ri=" + std::to_string(raw[3]);
    }
    return "qty=" + quantity_text(t.qty) + " exit_ms=" + std::to_string(t.exit_time);
}

bool run_row(const Row& row, const std::vector<Bar>& bars) {
    const auto tape = read_tape(row.trace ? kTrace : kOrig, row.trace);
    RequestedGridHost host(row.trace, row.explicit_arm);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
    require(host.last_error().empty(), "engine reported an error (details withheld)");
    std::map<Key, Trade> engine;
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const Trade t = host.get_report_trade(i);
        require(engine.emplace(Key{t.entry_time, t.entry_id}, t).second,
                "duplicate engine entry key");
    }
    std::size_t joined = 0, equal = 0, different = 0, engine_only = 0, tape_only = 0;
    std::string first;
    for (const auto& pair : tape) {
        const auto& key = pair.first;
        const auto& tv = pair.second;
        const auto found = engine.find(key);
        const std::string entry = "entry_ms=" + std::to_string(key.first);
        if (found == engine.end()) {
            ++tape_only;
            if (first.empty()) first = entry + " expected " + expected_text(tv, row.trace)
                + " got <missing>";
            continue;
        }
        ++joined;
        const Trade& got = found->second;
        bool same;
        if (row.trace) {
            const Trace actual = read_trace(got.entry_comment);
            // c is the signal observation key; only ro and ri are payloads.
            same = actual[0] == tv.trace[0] && actual[2] == tv.trace[2]
                && actual[3] == tv.trace[3];
            if (actual[0] != tv.trace[0] && first.empty())
                first = entry + " expected " + expected_text(tv, true)
                    + " got " + actual_text(got, true) + " (signal observation key differs)";
        } else {
            same = got.is_long && equal_quantity(got.qty, tv.qty)
                && got.exit_time == tv.exit_ms;
        }
        if (same) ++equal;
        else {
            ++different;
            if (first.empty()) first = entry + " expected " + expected_text(tv, row.trace)
                + " got " + actual_text(got, row.trace);
        }
    }
    for (const auto& pair : engine) {
        if (tape.count(pair.first) != 0) continue;
        ++engine_only;
        if (first.empty()) first = "entry_ms=" + std::to_string(pair.first.first)
            + " expected <missing> got " + actual_text(pair.second, row.trace);
    }
    const bool pass = tape.size() == kEntries && joined == kEntries && equal == kEntries
        && different == 0 && engine_only == 0 && tape_only == 0;
    std::printf("%s [%s %s] joined=%zu equal=%zu different=%zu engine-only=%zu tape-only=%zu\n",
                pass ? "PASS" : "FAIL", row_class(row), row.name,
                joined, equal, different, engine_only, tape_only);
    std::printf("  first differing entry: %s\n", first.empty() ? "none" : first.c_str());
    if (tape.size() != kEntries)
        std::printf("  tape entry count: expected=%zu got=%zu\n", kEntries, tape.size());
    print_default_label(row);
    return pass;
}

void failed_row(const Row& row, const std::string& reason) {
    std::printf("FAIL [%s %s] joined=unavailable equal=unavailable different=unavailable "
                "engine-only=unavailable tape-only=unavailable\n"
                "  first differing entry: unavailable; %s\n",
                row_class(row), row.name, reason.c_str());
    print_default_label(row);
}

}  // namespace

int main() {
    for (const char* variable : {kFeed, kOrig, kTrace}) {
        const char* value = std::getenv(variable);
        std::printf("INPUT %s: %s\n", variable,
                    value != nullptr && *value != '\0' ? "provided" : "unset or empty");
    }
    std::vector<Bar> bars;
    std::string feed_error;
    try {
        bars = read_feed();
    } catch (const Failure& error) {
        feed_error = error.what();
    } catch (const std::exception&) {
        feed_error = std::string(kFeed) + ": input exception (details withheld)";
    }
    int rows = 0, explicit_rows = 0, explicit_failed = 0, failures = 0;
    for (const Row& row : kRows) {
        ++rows;
        if (row.explicit_arm) ++explicit_rows;
        bool pass = false;
        try {
            if (!feed_error.empty()) failed_row(row, feed_error);
            else pass = run_row(row, bars);
        } catch (const Failure& error) {
            failed_row(row, error.what());
        } catch (const std::exception&) {
            failed_row(row, "execution or input exception (details withheld)");
        }
        if (!pass) {
            ++failures;
            if (row.explicit_arm) ++explicit_failed;
        }
    }
    std::printf("EXPLICIT fix criterion: rows=%d failed=%d\n", explicit_rows, explicit_failed);
    std::printf("%s lab-only requested-bar grid tapes: rows=%d failed=%d\n",
                failures == 0 ? "PASS" : "FAIL", rows, failures);
    return failures == 0 ? 0 : 1;
}
