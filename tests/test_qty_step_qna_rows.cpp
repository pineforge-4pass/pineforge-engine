// CR-726: qty=na percent sizing against private TradingView signal rows
// for the next release. Fixture sha256 prefix: af2f37103f3b.
// Receipt values are read at run time; no tape values are embedded here.

#include <array>
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#ifndef PINEFORGE_QTY_STEP_FIXTURE_DIR
#error "PINEFORGE_QTY_STEP_FIXTURE_DIR must name tests/fixtures/qty_step_mincontract"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kRequest = 1.234567;
constexpr double kPriceTick = 0.01;

struct Group {
    const char* name;
    int matched = 0;
    int total = 0;

    void check(bool match, const char* row) {
        ++total;
        if (match) {
            ++matched;
        } else {
            std::printf("  FAIL %s: %s\n", name, row);
        }
    }

    int finish() const {
        std::printf("%s: %d / %d matched\n", name, matched, total);
        return total - matched;
    }
};

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

pineforge::Bar flat_bar(int index, double price) {
    return {price, price, price, price, 1.0,
            1704067200000LL + 3600000LL * index};
}

class EntryProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit EntryProbe(double request = kRequest, double capital = 100.0,
                        bool percent = false)
        : request_(request) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(
            percent ? pineforge::QtyType::PERCENT_OF_EQUITY : pineforge::QtyType::FIXED);
        config.default_qty_value = percent ? 100.0 : 1.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_margin_call_enabled(false);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() == 0) {
            signal_equity = live_current_equity();
            signal_position = live_position_size();
            signal_pending = pending_order_count();
            strategy_entry("L", true, kNaN, kNaN, request_);
        } else if (pine_bar_index() == 2) {
            strategy_close_all();
        }
    }

    // The same read-only getter generated syminfo.mincontract expressions use.
    double metadata(const std::string& key) const {
        return get_syminfo_metadata(key);
    }

    void run_entry(double signal_close = 1.0, double fill_open = 1.0) {
        const std::array<pineforge::Bar, 4> bars{{
            flat_bar(0, signal_close), flat_bar(1, fill_open),
            flat_bar(2, fill_open), flat_bar(3, fill_open),
        }};
        run(bars.data(), static_cast<int>(bars.size()));
    }

    double signal_equity = kNaN;
    double signal_position = kNaN;
    int signal_pending = -1;

private:
    double request_;
};

struct SignalRow {
    int signal_bar;
    double equity;
    double signal_close;
    double fill_open;
    bool tv_placed;
    double tv_qty;
    std::int64_t fill_open_cents;
};

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            result.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    require(result.size() == 10, "expected exactly ten TSV columns");
    for (const auto& field : result) {
        require(!field.empty(), "empty TSV column");
    }
    return result;
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

int integer(const std::string& text) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                && value >= 0, "invalid nonnegative integer");
    return value;
}

// Accept finite, unsigned decimal text only, with no whitespace or suffix.
// strtod gives the exact double represented by the receipt's tv_qty text.
double number(const std::string& text) {
    require(!text.empty(), "empty decimal");
    std::size_t i = 0;
    while (i < text.size() && digit(text[i])) ++i;
    require(i > 0, "decimal needs an integer part");
    if (i < text.size() && text[i] == '.') {
        const std::size_t fraction = ++i;
        while (i < text.size() && digit(text[i])) ++i;
        require(i > fraction, "decimal needs a fractional part");
    }
    require(i == text.size(), "invalid decimal syntax");
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    require(end == text.c_str() + text.size() && errno != ERANGE
                && std::isfinite(value), "invalid finite decimal");
    return value;
}

bool flag(const std::string& text) {
    require(text == "0" || text == "1", "expected a 0/1 flag");
    return text == "1";
}

void timestamp(const std::string& text) {
    require(text.size() == 16, "invalid UTC timestamp length");
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool valid = i == 4 || i == 7 ? text[i] == '-'
            : i == 10 ? text[i] == ' '
            : i == 13 ? text[i] == ':' : digit(text[i]);
        require(valid, "invalid UTC timestamp syntax");
    }
    const int year = integer(text.substr(0, 4));
    const int month = integer(text.substr(5, 2));
    const int day = integer(text.substr(8, 2));
    const int hour = integer(text.substr(11, 2));
    const int minute = integer(text.substr(14, 2));
    require(year > 0 && month >= 1 && month <= 12 && hour < 24 && minute < 60,
            "invalid UTC timestamp range");
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int last_day = days[month - 1] + (month == 2 && leap ? 1 : 0);
    require(day >= 1 && day <= last_day, "invalid UTC calendar day");
}

// Convert the fill_open TEXT directly to cents, never via a floating value.
std::int64_t cents(const std::string& text) {
    std::int64_t value = 0;
    bool fraction = false;
    int fractional_digits = 0;
    int whole_digits = 0;
    for (char c : text) {
        if (c == '.') {
            require(!fraction && whole_digits > 0, "invalid cents decimal point");
            fraction = true;
        } else {
            require(digit(c), "invalid cents digit");
            if (fraction) {
                ++fractional_digits;
                require(fractional_digits <= 2, "fill price is not integer cents");
            } else {
                ++whole_digits;
            }
            const int next = c - '0';
            require(value <= (std::numeric_limits<std::int64_t>::max() - next) / 10,
                    "cents overflow");
            value = value * 10 + next;
        }
    }
    require(whole_digits > 0 && (!fraction || fractional_digits > 0),
            "incomplete cents decimal");
    while (fractional_digits < 2) {
        require(value <= std::numeric_limits<std::int64_t>::max() / 10,
                "cents overflow");
        value *= 10;
        ++fractional_digits;
    }
    return value;
}

std::vector<SignalRow> read_rows() {
    const std::string path = std::string(PINEFORGE_QTY_STEP_FIXTURE_DIR)
        + "/qna/signal_rows.tsv";
    std::ifstream in(path);
    require(in.is_open(), "cannot open " + path);
    std::string line;
    require(static_cast<bool>(std::getline(in, line)), "missing TSV header");
    const std::string header =
        "signal_bar_index\tsignal_time_utc\tfill_time_utc\tequity_before_signal\t"
        "signal_close\tfill_open\ttv_placed\ttv_qty\trule_qty\trule_placed";
    require(line == header, "unexpected TSV header");
    std::vector<SignalRow> rows;
    std::size_t line_number = 1;
    while (std::getline(in, line)) {
        ++line_number;
        try {
            const auto columns = fields(line);
            SignalRow row{};
            row.signal_bar = integer(columns[0]);
            // Unused columns are validated for syntax only.
            timestamp(columns[1]);
            timestamp(columns[2]);
            row.equity = number(columns[3]);
            row.signal_close = number(columns[4]);
            row.fill_open = number(columns[5]);
            row.tv_placed = flag(columns[6]);
            row.tv_qty = number(columns[7]);
            number(columns[8]);
            flag(columns[9]);
            row.fill_open_cents = cents(columns[5]);
            require(row.equity > 0.0 && row.signal_close > 0.0 && row.fill_open > 0.0,
                    "capital and prices must be positive");
            require(row.tv_placed ? row.tv_qty > 0.0 : row.tv_qty == 0.0,
                    "TV quantity does not match TV placement flag");
            if (row.fill_open > row.signal_close) rows.push_back(row);
        } catch (const std::exception& error) {
            throw std::runtime_error(path + ":" + std::to_string(line_number)
                                     + ": " + error.what());
        }
    }
    require(in.eof() && !in.bad(), "error reading " + path);
    require(line_number - 1 == 205, "expected all 205 signal rows");
    return rows;
}

int default_sizing(const std::vector<SignalRow>& rows) {
    Group group{"(ii) mincontract-only qty=na percent sizing"};
    Group placed{"(ii) TV placed rows"};
    Group declined{"(ii) TV declined rows"};
    for (const auto& row : rows) {
        // Fresh handle, capital E, no trades, fees, FX or open position:
        // signal equity = E + realized P&L(0) + open P&L(0) = E.
        // The callback submits literal na with a 100%-of-equity declaration;
        // the next bar opens at the receipt's fill_open. Closing later at
        // that same price exposes the admitted quantity in a public trade.
        // A market fill books integer ticks * mintick, not necessarily the
        // raw decimal literal's bits (engine.hpp: round_to_mintick).
        const double expected_entry_price = static_cast<double>(row.fill_open_cents) * kPriceTick;
        EntryProbe probe(kNaN, row.equity, true);
        probe.set_syminfo_mintick(kPriceTick);
        probe.set_syminfo_metadata("mincontract", 0.00001);
        probe.run_entry(row.signal_close, row.fill_open);
        const std::string error = probe.last_error();
        const int trade_count = probe.trade_count();
        const double position = probe.live_position_size();
        const int pending_count = probe.pending_order_count();
        const double filled_qty = trade_count > 0 ? probe.get_trade(0).qty : kNaN;
        const double entry_price = trade_count > 0 ? probe.get_trade(0).entry_price : kNaN;
        const bool fill_checked = row.tv_placed && trade_count == 1;
        bool match = row.fill_open > row.signal_close
            && error.empty()
            && bits(probe.signal_equity) == bits(row.equity)
            && probe.signal_position == 0.0
            && probe.signal_pending == 0
            && trade_count == (row.tv_placed ? 1 : 0)
            && position == 0.0
            && pending_count == 0;
        if (fill_checked) {
            match = match
                && bits(filled_qty) == bits(row.tv_qty)
                && bits(entry_price) == bits(expected_entry_price);
        }
        char label[2048];
        std::snprintf(label, sizeof label,
            "signal_bar=%d tv_placed=%d\n"
            "    gap_up=%d: fill_open=%.17g > signal_close=%.17g\n"
            "    signal_equity_match=%d: actual=%.17g [0x%016" PRIx64
            "] expected=%.17g [0x%016" PRIx64 "]\n"
            "    signal_position_match=%d: actual=%.17g expected=0\n"
            "    signal_pending_match=%d: actual=%d expected=0\n"
            "    trade_count_match=%d: actual=%d expected=%d\n"
            "    position_match=%d: actual=%.17g expected=0\n"
            "    pending_count_match=%d: actual=%d expected=0\n"
            "    fill_checked=%d qty_bits_match=%d: actual=%.17g [0x%016" PRIx64
            "] expected=%.17g [0x%016" PRIx64 "]\n"
            "    fill_checked=%d entry_price_bits_match=%d: actual=%.17g [0x%016" PRIx64
            "] expected=%.17g [0x%016" PRIx64 "]\n"
            "    raw_fill_price_bits_match=%d: raw=%.17g [0x%016" PRIx64
            "] fill_open_cents=%" PRId64 " mintick=%.17g\n"
            "    last_error_empty=%d: error='%s'",
            row.signal_bar, static_cast<int>(row.tv_placed),
            static_cast<int>(row.fill_open > row.signal_close), row.fill_open, row.signal_close,
            static_cast<int>(bits(probe.signal_equity) == bits(row.equity)),
            probe.signal_equity, bits(probe.signal_equity), row.equity, bits(row.equity),
            static_cast<int>(probe.signal_position == 0.0), probe.signal_position,
            static_cast<int>(probe.signal_pending == 0), probe.signal_pending,
            static_cast<int>(trade_count == (row.tv_placed ? 1 : 0)),
            trade_count, row.tv_placed ? 1 : 0,
            static_cast<int>(position == 0.0), position,
            static_cast<int>(pending_count == 0), pending_count,
            static_cast<int>(fill_checked), static_cast<int>(bits(filled_qty) == bits(row.tv_qty)),
            filled_qty, bits(filled_qty), row.tv_qty, bits(row.tv_qty),
            static_cast<int>(fill_checked),
            static_cast<int>(bits(entry_price) == bits(expected_entry_price)),
            entry_price, bits(entry_price), expected_entry_price, bits(expected_entry_price),
            static_cast<int>(bits(entry_price) == bits(row.fill_open)),
            row.fill_open, bits(row.fill_open), row.fill_open_cents, kPriceTick,
            static_cast<int>(error.empty()), error.c_str());
        group.check(match, label);
        (row.tv_placed ? placed : declined).check(match, label);
        // Preserve evidence of the old price-oracle mismatch even when the
        // corrected exact-price assertion succeeds.
        if (match && row.tv_placed && bits(expected_entry_price) != bits(row.fill_open)) {
            std::printf("  PRICE REPRESENTATION %s\n", label);
        }
    }
    char population[128];
    std::snprintf(population, sizeof population,
                  "receipt population: rows=%d expected=59 placed=%d expected=32 declined=%d expected=27",
                  placed.total + declined.total, placed.total, declined.total);
    group.check(rows.size() == 59 && placed.total == 32 && declined.total == 27, population);
    placed.finish();
    declined.finish();
    return group.finish();
}

}  // namespace

int main() {
    int failures = 0;
    try {
        const auto rows = read_rows();
        failures += default_sizing(rows);
    } catch (const std::exception& error) {
        std::printf("  FAIL private signal rows: %s\n", error.what());
        ++failures;
    }
    std::printf("qty=na quantity-step regression: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
