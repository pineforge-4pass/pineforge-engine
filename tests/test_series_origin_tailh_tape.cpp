// S1: declare only series_origin_ms; compare both EMA comment fields to the
// private tape. Expected texts are read at runtime, never copied into source.
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/str_utils.hpp>
#include <pineforge/ta.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_SERIES_ORIGIN_FIXTURE_DIR
#error "PINEFORGE_SERIES_ORIGIN_FIXTURE_DIR must name tests/fixtures/series_origin"
#endif

using namespace pineforge;
namespace {
int tests_passed = 0, tests_failed = 0;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        std::printf("FAIL [S1] %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
        ++tests_failed;                                                        \
    } else {                                                                   \
        ++tests_passed;                                                        \
    }                                                                          \
} while (false)

constexpr std::int64_t kMinute = 60'000, kStep = 15 * kMinute;
constexpr std::size_t kExits = 336;
using Texts = std::pair<std::string, std::string>;
using Csv = std::vector<std::vector<std::string>>;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
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
    double value = 0;
    in >> std::noskipws >> value;
    require(!in.fail() && in.peek() == std::char_traits<char>::eof()
                && std::isfinite(value), "invalid finite number");
    return value;
}

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t pos = 0;
    for (;;) {
        std::string field;
        if (pos < line.size() && line[pos] == '"') {
            ++pos;
            bool closed = false;
            while (pos < line.size()) {
                const char c = line[pos++];
                if (c != '"') field += c;
                else if (pos < line.size() && line[pos] == '"') {
                    field += '"';
                    ++pos;
                } else {
                    closed = true;
                    break;
                }
            }
            require(closed, "unterminated CSV quote");
        } else {
            while (pos < line.size() && line[pos] != ',') {
                require(line[pos] != '"', "quote inside unquoted CSV field");
                field += line[pos++];
            }
        }
        fields.push_back(field);
        if (pos == line.size()) break;
        require(line[pos++] == ',', "text after quoted CSV field");
    }
    return fields;
}

Csv read_csv(const std::string& relative) {
    std::ifstream in(std::string(PINEFORGE_SERIES_ORIGIN_FIXTURE_DIR) + "/" + relative);
    require(in.is_open(), "cannot open " + relative);
    Csv rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (rows.empty() && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        try {
            rows.push_back(csv_fields(line));
        } catch (const std::exception& error) {
            throw std::runtime_error(relative + ":" + std::to_string(rows.size() + 1)
                                     + ": " + error.what());
        }
    }
    require(in.eof() && !in.bad() && !rows.empty(), "empty/unreadable " + relative);
    return rows;
}

std::vector<Bar> read_feed() {
    const auto rows = read_csv("feed/bars.csv");
    require(rows.front() == std::vector<std::string>(
                {"timestamp", "open", "high", "low", "close", "volume"}),
            "unexpected feed header");
    std::vector<Bar> bars;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& f = rows[i];
        require(f.size() == 6, "wrong feed column count");
        Bar b{};
        b.timestamp = integer(f[0]);
        b.open = number(f[1]); b.high = number(f[2]); b.low = number(f[3]);
        b.close = number(f[4]); b.volume = number(f[5]);
        require(b.timestamp > 0 && b.timestamp % kStep == 0
                    && (bars.empty() || b.timestamp == bars.back().timestamp + kStep),
                "feed must advance on the 15m grid");
        require(b.low > 0 && b.low <= b.open && b.open <= b.high
                    && b.low <= b.close && b.close <= b.high && b.volume >= 0,
                "invalid feed OHLCV");
        bars.push_back(b);
    }
    require(bars.size() == 2 * kExits, "expected 672 feed bars");
    return bars;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

std::int64_t tape_ms(const std::string& text) {
    require(text.size() == 16 && text[4] == '-' && text[7] == '-'
                && text[10] == ' ' && text[13] == ':', "invalid tape date shape");
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
    // Exported chart labels use Asia/Taipei (UTC+8).
    return ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))
             * 24 + h) * 60 + mi - 8 * 60) * kMinute;
}

std::vector<std::string> signal_fields(const std::string& text) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find('|', start);
        fields.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    require(fields.size() == 7, "expected seven exit Signal fields");
    for (std::size_t i = 0; i < 2; ++i) {
        if (fields[i] == "na") continue;
        require(number(fields[i]) > 0, "invalid EMA Signal field");
        const auto dot = fields[i].find('.');
        require(dot == std::string::npos || fields[i].size() - dot - 1 <= 4,
                "EMA Signal exceeds four decimal places");
    }
    return fields;
}

struct TapeExit {
    std::int64_t index = 0, entry_ms = 0, exit_ms = 0;
    Texts text;
    bool entry_seen = false, exit_seen = false;
};

std::map<std::int64_t, TapeExit> read_tape() {
    const auto rows = read_csv("tailh/tv_trades.csv");
    const auto& header = rows.front();
    require(header.size() == 17 && header[0] == "Trade number" && header[1] == "Type"
                && header[2] == "Date and time" && header[3] == "Signal"
                && header[4] == "Price USDT" && header[5] == "Size (qty)",
            "unexpected tape header");
    std::map<std::int64_t, TapeExit> pairs;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& f = rows[i];
        require(f.size() == header.size(), "wrong tape column count");
        const auto index = integer(f[0]);
        require(index > 0, "nonpositive trade number");
        const bool entry = f[1] == "Entry long";
        require(entry || f[1] == "Exit long", "unexpected tape type");
        require(number(f[4]) > 0 && number(f[5]) == 1.0, "invalid tape fill/quantity");
        auto& t = pairs[index];
        require(entry ? (t.exit_seen && !t.entry_seen)
                      : (!t.exit_seen && !t.entry_seen), "expected Exit then Entry");
        const auto ms = tape_ms(f[2]);
        require(ms % kStep == 0, "tape fill off 15m grid");
        t.index = index;
        if (entry) {
            require(f[3] == "L", "unexpected entry id");
            t.entry_seen = true;
            t.entry_ms = ms;
        } else {
            const auto fields = signal_fields(f[3]);
            t.exit_seen = true;
            t.exit_ms = ms;
            t.text = {fields[0], fields[1]};
        }
    }
    std::map<std::int64_t, TapeExit> exits;
    std::int64_t next = 1;
    for (const auto& pair : pairs) {
        const auto& t = pair.second;
        require(pair.first == next++ && t.entry_seen && t.exit_seen
                    && t.exit_ms == t.entry_ms + kStep, "unpaired tape trade");
        require(exits.emplace(t.exit_ms, t).second, "duplicate tape exit time");
    }
    require(exits.size() == kExits, "expected 336 tape exits");
    return exits;
}

// Use Pine's decimal half-up formatter for actual values only. Expected
// values remain the tape's literal texts, with no numeric tolerance.
std::string spelled(double value) {
    if (std::isnan(value)) return "na";
    require(std::isfinite(value), "nonfinite engine EMA");
    return str_tostring(value, "#.####");
}

class TailHost final : public source::PineStrategyHost {
public:
    ta::EMA ema{12};
    double previous = na<double>();
    std::map<std::int64_t, Texts> exits;
    std::size_t entries = 0;
    explicit TailHost(std::int64_t origin) {
        attach_pine_execution_adapter();
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config{};
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        // S1 sets neither of the raw warmup keys.
        set_syminfo_metadata("series_origin_ms", static_cast<double>(origin));
    }
    void on_source_bar(const Bar& bar) override {
        const double current = ema.compute(bar.close);
        const auto minute = bar.timestamp / kMinute % 60;
        if (minute == 0 || minute == 30) {
            strategy_entry("L", true, na<double>(), na<double>(), 1.0);
            ++entries;
        }
        if (minute == 15 || minute == 45) {
            CHECK(signed_position_size() == 1.0);
            const Texts text{spelled(current), spelled(previous)};
            // Market orders fill on the next open. Join the close evaluation
            // to that tape fill time, including the terminal close evaluation.
            CHECK(exits.emplace(bar.timestamp + kStep, text).second);
            strategy_close("L", text.first + "|" + text.second);
        }
        previous = current;
    }
};

void run_s1() {
    const auto bars = read_feed();
    const auto tape = read_tape();
    TailHost host(bars.front().timestamp);
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    CHECK(host.last_error().empty());
    CHECK(host.entries == kExits);
    CHECK(host.exits.size() == kExits);
    std::size_t joined = 0, equal = 0, different = 0, finite_na = 0;
    std::size_t tape_only = 0, engine_only = 0;
    std::string first;
    for (const auto& pair : tape) {
        const auto& t = pair.second;
        const auto got = host.exits.find(pair.first);
        if (got == host.exits.end()) {
            ++tape_only;
            if (first.empty()) first = "index=" + std::to_string(t.index)
                + " expected " + t.text.first + "|" + t.text.second + " got <missing>";
            continue;
        }
        ++joined;
        finite_na += (t.text.first == "na") != (got->second.first == "na");
        finite_na += (t.text.second == "na") != (got->second.second == "na");
        if (t.text == got->second) ++equal;
        else {
            ++different;
            if (first.empty()) first = "index=" + std::to_string(t.index)
                + " expected " + t.text.first + "|" + t.text.second
                + " got " + got->second.first + "|" + got->second.second;
        }
    }
    for (const auto& pair : host.exits)
        if (tape.count(pair.first) == 0) ++engine_only;
    CHECK(joined == kExits);
    CHECK(equal == kExits);
    CHECK(different == 0);
    CHECK(finite_na == 0);
    CHECK(tape_only == 0);
    CHECK(engine_only == 0);
    std::printf("%s [S1] joined=%zu equal=%zu different=%zu finite-vs-na=%zu "
                "tape-only=%zu engine-only=%zu\n",
                tests_failed == 0 ? "PASS" : "FAIL", joined, equal, different,
                finite_na, tape_only, engine_only);
    // These texts belong only in the private test log.
    std::printf("  first differing exit: %s\n", first.empty() ? "none" : first.c_str());
}
}  // namespace

int main() {
    try {
        run_s1();
    } catch (const std::exception& error) {
        ++tests_failed;
        std::printf("FAIL [S1] joined=unavailable equal=unavailable different=unavailable "
                    "finite-vs-na=unavailable tape-only=unavailable engine-only=unavailable\n"
                    "  first differing exit: unavailable; %s\n", error.what());
    }
    std::printf("series-origin tape: %d checks, %d failures\n",
                tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
