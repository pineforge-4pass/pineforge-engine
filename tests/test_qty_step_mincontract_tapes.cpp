/*
 * Quantity-step metadata tapes for the next release (CR-726).
 * Tape sha256 prefix: e0085eab.
 *
 * The caller declares the quantity step through set_syminfo_metadata.
 * Q1 declares mincontract alone; Q2 declares qty_step alone.
 * Q3/Q4 declare conflicting values in opposite orders: qty_step wins.
 * Expected quantities are the tape's literal ENTRY Size (qty) decimals;
 * expected exits are the paired EXIT times. Join on entry fill UTC + id.
 * Tape labels are UTC+8, so UTC = label minus eight hours.
 *
 * The Pine control uses only limit entries:
 *   strategy('repro', process_orders_on_close = true)
 *   if bar_index % 50 == 0 and strategy.position_size == 0
 *       strategy.entry('Long', strategy.long, limit = close)
 *   if strategy.position_size > 0 and bar_index % 50 == 10
 *       strategy.close('Long')
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

#ifndef PINEFORGE_QTY_STEP_FIXTURE_DIR
#error "PINEFORGE_QTY_STEP_FIXTURE_DIR must name tests/fixtures/qty_step_mincontract"
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
constexpr std::size_t kEntries = 29;
using Key = std::pair<std::int64_t, std::string>;

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

// Tape quantities stay decimal integers throughout parsing and comparison.
// Normalize trailing zeros only; never round either side to the declared step.
struct Decimal {
    std::int64_t coefficient = 0, scale = 1;
    std::string text;
    bool operator==(const Decimal& other) const {
        return coefficient == other.coefficient && scale == other.scale;
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
    while (result.scale > 1 && result.coefficient % 10 == 0) {
        result.coefficient /= 10;
        result.scale /= 10;
    }
    return result;
}

Decimal filled_decimal(double qty) {
    require(std::isfinite(qty) && qty > 0.0, "invalid engine filled quantity");
    // No precision argument: to_chars emits the shortest decimal that
    // round-trips to this exact filled double. An adjacent double differs.
    char text[1024];
    const auto written = std::to_chars(text, text + sizeof text, qty,
                                       std::chars_format::fixed);
    require(written.ec == std::errc{}, "cannot serialize engine filled quantity");
    return decimal(std::string(text, written.ptr));
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
    return std::string(PINEFORGE_QTY_STEP_FIXTURE_DIR) + "/btc60/" + suffix;
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
            require(bar.low > 0 && bar.low <= bar.open && bar.open <= bar.high
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

struct TapeTrade {
    std::int64_t entry_ms = 0, exit_ms = 0;
    std::string id;
    Decimal qty;
    bool entry_seen = false, exit_seen = false;
};

std::map<Key, TapeTrade> read_tape() {
    const Csv rows = read_csv(fixture("reporter/tv_trades.csv"));
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
                require(qty == t.qty, "entry/exit quantity differs");
                t.entry_seen = true;
                t.entry_ms = ms;
                t.id = f[3];
                t.qty = qty;
                require(t.id == "Long" && t.entry_ms < t.exit_ms, "invalid entry");
            } else {
                t.exit_seen = true;
                t.exit_ms = ms;
                t.qty = qty;
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("reporter/tv_trades.csv:" + std::to_string(i + 1)
                                     + ": " + error.what());
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

struct Row {
    const char* name;
    std::vector<std::pair<std::string, double>> metadata;
};

const Row kRows[] = {
    {"Q1 mincontract only", {{"mincontract", 0.00001}}},
    {"Q2 qty_step only", {{"qty_step", 0.00001}}},
    {"Q3 qty_step last", {{"mincontract", 0.0001}, {"qty_step", 0.00001}}},
    {"Q4 mincontract last", {{"qty_step", 0.00001}, {"mincontract", 0.0001}}},
};

class QuantityStepHost final : public source::PineStrategyHost {
public:
    explicit QuantityStepHost(const Row& row) {
        attach_pine_execution_adapter();
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config{};
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.pyramiding = 1;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        for (const auto& item : row.metadata)
            set_syminfo_metadata(item.first, item.second);
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        if (index % 50 == 0 && signed_position_size() == 0.0)
            strategy_entry("Long", true, bar.close);
        if (signed_position_size() > 0.0 && index % 50 == 10)
            strategy_close("Long");
    }
};

std::string expected_text(const TapeTrade& t) {
    return "qty=" + t.qty.text + " exit_ms=" + std::to_string(t.exit_ms);
}

std::string actual_text(const Trade& t) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10)
         << "qty=" << t.qty << " exit_ms=" << t.exit_time;
    return text.str();
}

void run_row(const Row& row, const std::vector<Bar>& bars) {
    scenario = row.name;
    const int failures_before = tests_failed;
    const auto tape = read_tape();
    QuantityStepHost host(row);
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
                + " id=" + key.second + " expected " + expected_text(tv) + " got <missing>";
            continue;
        }
        ++joined;
        const Trade& got = found->second;
        const bool same = got.is_long && filled_decimal(got.qty) == tv.qty
            && got.exit_time == tv.exit_ms;
        if (same) ++equal;
        else {
            ++different;
            if (first.empty()) first = "entry_ms=" + std::to_string(key.first)
                + " id=" + key.second + " expected " + expected_text(tv)
                + " got " + actual_text(got);
        }
    }
    for (const auto& pair : engine) {
        if (tape.count(pair.first) != 0) continue;
        ++engine_only;
        if (first.empty()) first = "entry_ms=" + std::to_string(pair.first.first)
            + " id=" + pair.first.second + " expected <missing> got " + actual_text(pair.second);
    }
    CHECK(tape.size() == kEntries);
    CHECK(joined == kEntries);
    CHECK(equal == kEntries);
    CHECK(different == 0);
    CHECK(engine_only == 0);
    CHECK(tape_only == 0);
    std::printf("%s [%s] joined=%zu equal=%zu different=%zu engine-only=%zu tape-only=%zu\n",
                tests_failed == failures_before ? "PASS" : "FAIL", row.name,
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
                std::printf("FAIL [%s] joined=unavailable equal=unavailable "
                            "different=unavailable engine-only=unavailable tape-only=unavailable\n"
                            "  first differing entry: unavailable; %s\n",
                            row.name, error.what());
            }
        }
    } catch (const std::exception& error) {
        ++tests_failed;
        std::printf("FAIL feed: %s\n", error.what());
    }
    std::printf("\n%s quantity-step tapes: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
