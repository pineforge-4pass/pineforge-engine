/*
 * TradingView requested-bar grid tapes for the next release.
 *
 * All paths below are relative to fixtures/requested_bar_grid/btc60/.
 * BINANCE:BTCUSDT, chart 60, 2024-01-01 .. 2024-03-01, ws-report-v1.
 * feed/bars.csv: 1,441 bars, sha256
 * ec711e3df42852c33d5a12be56258add6a1851767cdc028bfb9e6c4f338b9c82.
 *
 * Row | Tape slug | tv_trades.csv sha256 prefix | Expected at each entry
 * R1  | bi-300    | f236de64                    | Size (qty), exit time
 * R1  | bi-420    | 03cf2251                    | Size (qty), exit time
 * R1  | bi-600    | 209d637a                    | Size (qty), exit time
 * R2  | open-300  | 4ed2c9f8                    | Size (qty), exit time
 * R2  | open-420  | 187d0a4a                    | Size (qty), exit time
 * R3  | trace-300 | 5ec58e18                    | Size, exit time, raw ro/ri/ti
 * R4  | bi-120    | d7498a16                    | Size (qty), exit time
 * R4  | bi-180    | e6b47ce4                    | Size (qty), exit time
 * R4  | bi-240    | a8fc3012                    | Size (qty), exit time
 * R4  | bi-360    | b0ed83fd                    | Size (qty), exit time
 * R4  | bi-480    | 9aaf10b8                    | Size (qty), exit time
 * R4  | open-120  | edcc837a                    | Size (qty), exit time
 *
 * Every expected quantity is the ENTRY's literal Size (qty) decimal.
 * Every expected exit is its paired EXIT's Date and time. Labels are UTC+8:
 * UTC = label minus eight hours, as in test_aggregated_entry_bar_index_tape.
 * Join by entry fill time in UTC + Signal before the first '|', never by row
 * position. Include the report's terminal range-end row (blank exit Signal).
 * Each row must join all 205 entries, with no extras or differences.
 *
 * The bi-* Pine sources make ONE request.security(tickerid, tf, bar_index);
 * qty = 1 + requested_bar_index % 5. The open-* sources make ONE request for
 * open; qty = 1 + int(requested_open) % 5. Both enter when chart bar_index > 6
 * and bar_index % 7 == 3, and close when > 6 and % 7 == 5, at next-bar open.
 * R4 widths divide 1440 minutes and must pass on the base engine; R1/R2 do
 * not, and must fail until the requested-bar grid is corrected.
 *
 * trace-300 keeps the scalar request and adds a tuple request for
 * [time, bar_index, ta.valuewhen(barstate.isfirst, time, 0)]. ENTRY Signal:
 * orbit|c=<chart open ms>|ci=<chart index>|ro=<requested open ms>
 *      |ri=<scalar requested index>|ti=<tuple requested index>
 *      |cf=<chart first open ms>|rf=<requested first open ms>.
 * These are TradingView's raw observations, NOT inferred from quantities.
 * R3 compares all seven fields, including ro and ri at the signal bar c,
 * from the same evaluate_security publication that the strategy reads.
 *
 * The host follows generated code: register_security_eval(id, tf, input_tf_,
 * false, false), increment a counter initially -1 exactly when
 * security_series_slot_is_new(id), publish that counter (or bar.open) in
 * evaluate_security, then read the published payload in on_source_bar.
 * No expected value comes from an engine aggregation or a synthetic grid.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

#ifndef PINEFORGE_REQUESTED_BAR_GRID_FIXTURE_DIR
#error "PINEFORGE_REQUESTED_BAR_GRID_FIXTURE_DIR must name tests/fixtures/requested_bar_grid"
#endif

namespace {

int tests_passed = 0;
int tests_failed = 0;
const char* scenario = "feed";

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL [%s] %s:%d  %s\n", scenario, __FILE__,         \
                        __LINE__, #expr);                                      \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (false)

constexpr std::int64_t kHour = 3'600'000;
constexpr std::size_t kEntries = 205;
using Key = std::pair<std::int64_t, std::string>;
using Trace = std::array<std::int64_t, 7>;
constexpr const char* kTraceFields[] = {"c", "ci", "ro", "ri", "ti", "cf", "rf"};

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::int64_t integer(const std::string& text) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "invalid integer: " + text);
    return result;
}

double number(const std::string& text) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double result = 0;
    in >> std::noskipws >> result;
    require(!in.fail() && in.peek() == std::char_traits<char>::eof()
                && std::isfinite(result), "invalid number: " + text);
    return result;
}

// Preserve the decimal's coefficient and scale, with no epsilon or rounding.
// The carrier quantities in these tapes are integral and exactly representable.
struct Decimal {
    std::int64_t coefficient = 0, scale = 1;
    std::string text;
    double value() const {
        return static_cast<double>(coefficient) / static_cast<double>(scale);
    }
};

Decimal decimal(const std::string& text) {
    Decimal result;
    result.text = text;
    bool fraction = false, digit = false;
    for (char c : text) {
        if (c == '.' && !fraction && digit) {
            fraction = true;
            digit = false;
            continue;
        }
        require(c >= '0' && c <= '9', "invalid quantity decimal: " + text);
        require(result.coefficient <= (std::numeric_limits<std::int64_t>::max()
                    - (c - '0')) / 10, "quantity overflow: " + text);
        result.coefficient = result.coefficient * 10 + (c - '0');
        if (fraction) {
            require(result.scale <= std::numeric_limits<std::int64_t>::max() / 10,
                    "quantity scale overflow: " + text);
            result.scale *= 10;
        }
        digit = true;
    }
    require(digit && result.coefficient > 0, "empty or nonpositive quantity: " + text);
    return result;
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

Csv read_csv(const std::string& path) {
    std::ifstream in(path);
    require(in.is_open(), "cannot open " + path);
    Csv rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (rows.empty() && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        try {
            rows.push_back(csv_fields(line));
        } catch (const std::exception& error) {
            throw std::runtime_error(path + ":" + std::to_string(rows.size() + 1)
                                     + ": " + error.what());
        }
    }
    require(in.eof() && !in.bad() && !rows.empty(), "unreadable or empty CSV: " + path);
    return rows;
}

std::string fixture(const std::string& suffix) {
    return std::string(PINEFORGE_REQUESTED_BAR_GRID_FIXTURE_DIR) + "/btc60/" + suffix;
}

std::vector<Bar> read_feed() {
    const Csv rows = read_csv(fixture("feed/bars.csv"));
    require(rows.front() == std::vector<std::string>(
                {"timestamp", "open", "high", "low", "close", "volume"}),
            "unexpected feed header");
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
            require(bar.timestamp > 0 && (bars.empty()
                        || bar.timestamp == bars.back().timestamp + kHour),
                    "feed timestamps must advance by one hour");
            require(bar.low <= bar.open && bar.open <= bar.high
                        && bar.low <= bar.close && bar.close <= bar.high
                        && bar.volume >= 0, "invalid feed OHLCV");
            bars.push_back(bar);
        } catch (const std::exception& error) {
            throw std::runtime_error("feed/bars.csv:" + std::to_string(i + 1)
                                     + ": " + error.what());
        }
    }
    require(bars.size() == 1441, "expected 1,441 chart bars");
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
                && text[10] == ' ' && text[13] == ':', "invalid tape date: " + text);
    const int y = static_cast<int>(integer(text.substr(0, 4)));
    const int mo = static_cast<int>(integer(text.substr(5, 2)));
    const int d = static_cast<int>(integer(text.substr(8, 2)));
    const int h = static_cast<int>(integer(text.substr(11, 2)));
    const int mi = static_cast<int>(integer(text.substr(14, 2)));
    const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    require(y >= 1970 && mo >= 1 && mo <= 12 && h >= 0 && h < 24
                && mi >= 0 && mi < 60, "out-of-range tape date: " + text);
    const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    require(d >= 1 && d <= month_days[mo - 1] + (mo == 2 && leap),
            "out-of-range tape day: " + text);
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))
                * 24 + h) * 60 + mi) * 60'000 - 8 * kHour;
}

Trace read_trace(const std::string& signal) {
    require(signal.substr(0, signal.find('|')) == "orbit", "unexpected trace id");
    Trace values{};
    std::size_t pos = signal.find('|');
    for (std::size_t i = 0; i < values.size(); ++i) {
        require(pos != std::string::npos, "missing trace field");
        const std::size_t end = signal.find('|', pos + 1);
        const std::string field = signal.substr(pos + 1,
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
    std::string id, signal;
    Decimal qty;
    bool entry_seen = false, exit_seen = false;
};

std::map<Key, TapeTrade> read_tape(const char* slug) {
    const Csv rows = read_csv(fixture(std::string(slug) + "/tv_trades.csv"));
    const auto& header = rows.front();
    require(header.size() == 17 && header[0] == "Trade number" && header[1] == "Type"
                && header[2] == "Date and time" && header[3] == "Signal"
                && header[4] == "Price USDT" && header[5] == "Size (qty)",
            "unexpected tape header");
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
            const Decimal qty = decimal(f[5]);
            if (entry) {
                require(qty.value() == t.qty.value(), "entry/exit quantity differs");
                t.entry_seen = true;
                t.entry_ms = ms;
                t.id = f[3].substr(0, f[3].find('|'));
                t.signal = f[3];
                t.qty = qty;
                require(!t.id.empty() && t.entry_ms < t.exit_ms, "invalid entry");
            } else {
                t.exit_seen = true;
                t.exit_ms = ms;
                t.qty = qty;
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(slug) + "/tv_trades.csv:"
                                     + std::to_string(i + 1) + ": " + error.what());
        }
    }
    std::map<Key, TapeTrade> trades;
    std::int64_t next = 1;
    for (const auto& pair : pairs) {
        const auto& t = pair.second;
        require(pair.first == next++ && t.entry_seen && t.exit_seen, "unpaired tape trade");
        require(trades.emplace(Key{t.entry_ms, t.id}, t).second, "duplicate tape entry key");
    }
    return trades;
}

enum class Payload { Index, Open, TraceIndex };

class RequestedGridHost final : public source::PineStrategyHost {
public:
    RequestedGridHost(const char* tf, Payload payload) : tf_(tf), payload_(payload) {
        attach_pine_execution_adapter();
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config{};
        config.initial_capital = 1000000;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
    }

    void prepare_script_run(const Bar*, int, bool) override {
        indices_ = {{-1, -1}};
        requested_ = std::numeric_limits<double>::quiet_NaN();
        tuple_index_ = -1;
        requested_open_ms_ = first_requested_ms_ = first_chart_ms_ = -1;
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, tf_, input_tf_, false, false);
        if (payload_ == Payload::TraceIndex)
            register_security_eval(1, tf_, input_tf_, false, false);
    }

    void evaluate_security(int id, const Bar& bar, bool) override {
        require(id == 0 || (id == 1 && payload_ == Payload::TraceIndex),
                "unexpected security evaluator id");
        if (security_series_slot_is_new(id)) ++indices_[static_cast<std::size_t>(id)];
        if (id == 0) {
            requested_ = payload_ == Payload::Open ? bar.open : indices_[0];
        } else {
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
        const double value = payload_ == Payload::Open ? std::trunc(requested_) : requested_;
        const double qty = 1 + std::fmod(value, 5.0);
        if (index > 6 && index % 7 == 3) {
            std::string comment;
            if (payload_ == Payload::TraceIndex) {
                require(std::isfinite(requested_) && requested_ >= 0
                            && requested_ <= std::numeric_limits<int>::max()
                            && requested_ == std::trunc(requested_),
                        "trace scalar index is unavailable or nonintegral");
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
    std::string tf_;
    Payload payload_;
    std::array<int, 2> indices_{{-1, -1}};
    int tuple_index_ = -1;
    double requested_ = std::numeric_limits<double>::quiet_NaN();
    std::int64_t requested_open_ms_ = -1, first_requested_ms_ = -1, first_chart_ms_ = -1;
};

struct Row {
    const char* group;
    const char* slug;
    const char* tf;
    Payload payload;
};

const Row kRows[] = {
    {"R1", "bi-300", "300", Payload::Index},
    {"R1", "bi-420", "420", Payload::Index},
    {"R1", "bi-600", "600", Payload::Index},
    {"R2", "open-300", "300", Payload::Open},
    {"R2", "open-420", "420", Payload::Open},
    {"R3", "trace-300", "300", Payload::TraceIndex},
    {"R4", "bi-120", "120", Payload::Index},
    {"R4", "bi-180", "180", Payload::Index},
    {"R4", "bi-240", "240", Payload::Index},
    {"R4", "bi-360", "360", Payload::Index},
    {"R4", "bi-480", "480", Payload::Index},
    {"R4", "open-120", "120", Payload::Open},
};

std::string expected_text(const TapeTrade& t, bool trace) {
    return "qty=" + t.qty.text + " exit_ms=" + std::to_string(t.exit_ms)
        + (trace ? " signal=" + t.signal : "");
}

std::string actual_text(const Trade& t, bool trace) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10)
         << "qty=" << t.qty << " exit_ms=" << t.exit_time;
    if (trace) text << " signal=" << t.entry_comment;
    return text.str();
}

void run_row(const Row& row, const std::vector<Bar>& bars) {
    scenario = row.slug;
    const int failures_before = tests_failed;
    const auto tape = read_tape(row.slug);
    const bool trace = row.payload == Payload::TraceIndex;
    RequestedGridHost host(row.tf, row.payload);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  engine error: %s\n", host.last_error().c_str());
    std::map<Key, Trade> engine;
    for (int i = 0; i < host.report_trade_count(); ++i) {
        const Trade trade = host.get_report_trade(i);
        CHECK(engine.emplace(Key{trade.entry_time, trade.entry_id}, trade).second);
    }

    std::size_t joined = 0, equal = 0, different = 0, engine_only = 0, tape_only = 0;
    std::string first;
    for (const auto& pair : tape) {
        const auto& key = pair.first;
        const auto& tv = pair.second;
        const auto found = engine.find(key);
        if (found == engine.end()) {
            ++tape_only;
            if (first.empty()) first = "entry_ms=" + std::to_string(key.first)
                + " id=" + key.second + " expected " + expected_text(tv, trace)
                + " got <missing>";
            continue;
        }
        ++joined;
        const Trade& got = found->second;
        bool same = got.is_long && got.qty == tv.qty.value() && got.exit_time == tv.exit_ms;
        if (trace) {
            const Trace expected = read_trace(tv.signal);
            const Trace actual = read_trace(got.entry_comment);
            require(expected[3] == expected[4], "tape scalar/tuple index differs");
            // c/ci identify the chart signal; ro/ri/ti are the raw requested
            // values. cf/rf also pin the two contexts' initial observations.
            const bool raw_same = actual == expected;
            same = raw_same && same;
        }
        if (same) ++equal;
        else {
            ++different;
            if (first.empty()) first = "entry_ms=" + std::to_string(key.first)
                + " id=" + key.second + " expected " + expected_text(tv, trace)
                + " got " + actual_text(got, trace);
        }
    }
    for (const auto& pair : engine) {
        if (tape.count(pair.first) != 0) continue;
        ++engine_only;
        if (first.empty()) first = "entry_ms=" + std::to_string(pair.first.first)
            + " id=" + pair.first.second + " expected <missing> got "
            + actual_text(pair.second, trace);
    }
    CHECK(tape.size() == kEntries);
    CHECK(joined == kEntries);
    CHECK(equal == kEntries);
    CHECK(different == 0);
    CHECK(engine_only == 0);
    CHECK(tape_only == 0);
    std::printf("%s [%s %s] joined=%zu equal=%zu different=%zu engine-only=%zu tape-only=%zu\n",
                tests_failed == failures_before ? "PASS" : "FAIL", row.group, row.slug,
                joined, equal, different, engine_only, tape_only);
    std::printf("  first differing entry: %s\n", first.empty() ? "none" : first.c_str());
}

}  // namespace

int main() {
    try {
        const auto bars = read_feed();
        for (const Row& row : kRows) {
            try {
                run_row(row, bars);
            } catch (const std::exception& error) {
                ++tests_failed;
                std::printf("FAIL [%s %s] joined=unavailable equal=unavailable "
                            "different=unavailable engine-only=unavailable tape-only=unavailable\n"
                            "  first differing entry: unavailable; %s\n",
                            row.group, row.slug, error.what());
            }
        }
    } catch (const std::exception& error) {
        ++tests_failed;
        std::printf("FAIL feed: %s\n", error.what());
    }
    std::printf("\n%s requested-bar grid tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
